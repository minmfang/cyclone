// Asynchronous fault tolerant pmem log replication with cyclone
#include "cyclone.hpp"
#include "cyclone_context.hpp"
#include "timeouts.hpp"
#include "checkpoint.hpp"

#ifdef TRACING
extern void trace_send_cmd(void *data, const int size);
extern void trace_pre_append(void *data, const int size);
extern void trace_post_append(void *data, const int size);
extern void trace_send_entry(void *data, const int size);
#endif


void *cyclone_control_socket_out(void *cyclone_handle, 
				 int replica)
{
  return ((cyclone_t *)cyclone_handle)->router->control_output_socket(replica);
}
void *cyclone_control_socket_in(void *cyclone_handle)
{
  return ((cyclone_t *)cyclone_handle)->router->control_input_socket();
}

/** Raft callback for sending request vote message */
static int __send_requestvote(raft_server_t* raft,
			      void *user_data,
			      raft_node_t *node,
			      msg_requestvote_t* m)
{
  cyclone_t* cyclone_handle = (cyclone_t *)user_data;
  void *socket      = raft_node_get_udata(node);
  msg_t msg;
  msg.source      = cyclone_handle->me;
  msg.msg_type    = MSG_REQUESTVOTE;
  msg.rv          = *m;
  cyclone_tx(socket, 
	      (unsigned char *)&msg, 
	      sizeof(msg_t), 
	      "__send_requestvote");
  return 0;
}

int cyclone_serialize_last_applied(void *cyclone_handle, void *buf)
{
  int size = 0;
  raft_entry_t *ety = raft_last_applied_ety(cyclone_handle->raft_handle);
  if(ety != NULL) {
    memcpy(buf, ety, sizeof(raft_entry_t));
    size += sizeof(raft_entry_t);
    buf  += sizeof(raft_entry_t);
    (void)cyclone_handle->read_from_log(buf, ety->data.buf);
    buf  += ety->data.len;
    size += ety->data.len;
  }
  return size;
}

void cyclone_deserialize_last_applied(void *cyclone_handle, raft_entry_t *ety)
{
  TX_BEGIN(cyclone_handle->pop_raft_state) {
    if(cyclone_handle->append_to_raft_log((unsigned char *)ety,
					  sizeof(raft_entry_t)) != 0) {
      pmemobj_tx_abort(-1);
    }
    void * saved_ptr = (void *)cyclone_handle->get_log_offset();
    if(cyclone_handle->append_to_raft_log((unsigned char *)ety->data.buf,
					  ety->data.len) != 0) {
      pmemobj_tx_abort(-1);
    }
    ety->data.buf = saved_ptr;
  } TX_ONABORT {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to write init log entry for img build";
    exit(-1);
  } TX_END
}

/** Raft callback for sending appendentries message */
static int __send_appendentries(raft_server_t* raft,
				void *udata,
				raft_node_t *node,
				msg_appendentries_t* m)
{
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  void *socket      = raft_node_get_udata(node);
  msg_t *msg = (msg_t *)cyclone_handle->cyclone_buffer_out;
  unsigned char *ptr = (unsigned char *)(msg + 1);
  rtc_clock clock;
  int nodeidx = raft_node_get_id(node);
  bool same_log_point  =
    (m->prev_log_term == cyclone_handle->throttles[nodeidx].prev_log_term &&
     m->prev_log_idx  == cyclone_handle->throttles[nodeidx].prev_log_idx);
  bool prev_was_heartbeat = (cyclone_handle->throttles[nodeidx].prev_entries == 0);
  bool current_is_heartbeat = (m->n_entries == 0);
  if(same_log_point && (prev_was_heartbeat == current_is_heartbeat)) {
    if((clock.current_time() - cyclone_handle->throttles[nodeidx].last_tx_time) <=
       cyclone_handle->throttles[nodeidx].timeout) {
      return 0; // throttle retx
    }
    else {
      cyclone_handle->throttles[nodeidx].last_tx_time = clock.current_time();
      cyclone_handle->throttles[nodeidx].timeout = 2*cyclone_handle->throttles[nodeidx].timeout;
    }
  }
  else {
    cyclone_handle->throttles[nodeidx].prev_log_term = m->prev_log_term;
    cyclone_handle->throttles[nodeidx].prev_log_idx = m->prev_log_idx;
    cyclone_handle->throttles[nodeidx].prev_entries = m->n_entries;
    cyclone_handle->throttles[nodeidx].last_tx_time = clock.current_time();
    cyclone_handle->throttles[nodeidx].timeout = RAFT_REQUEST_TIMEOUT/2;
  }
  msg->msg_type         = MSG_APPENDENTRIES;
  msg->source           = cyclone_handle->me;
  msg->ae.term          = m->term;
  msg->ae.prev_log_idx  = m->prev_log_idx;
  msg->ae.prev_log_term = m->prev_log_term;
  msg->ae.leader_commit = m->leader_commit;
  unsigned long spc_remains = MSG_MAXSIZE - sizeof(msg_t);
  int i;
  for(i=0;i<m->n_entries;i++) {
    unsigned long spc_needed = sizeof(msg_entry_t) + m->entries[i].data.len;
    if(spc_needed > spc_remains) {
      break;
    }
    spc_remains -= spc_needed;
    memcpy(ptr, &m->entries[i], sizeof(msg_entry_t));
    ptr += sizeof(msg_entry_t);
  }
  // Adjust number of entries
  m->n_entries          = i;
  msg->ae.n_entries     = m->n_entries;
  for(i=0;i<m->n_entries;i++) {
    (void)cyclone_handle->read_from_log(ptr,
					(unsigned long)m->entries[i].data.buf);
#ifdef TRACING
    BOOST_LOG_TRIVIAL(info) << "SENDING CMD TO " << nodeidx;
    trace_send_entry(ptr, m->entries[i].data.len);
#endif
    ptr += m->entries[i].data.len;
  }
  cyclone_tx(socket, 
	      cyclone_handle->cyclone_buffer_out, 
	      ptr - cyclone_handle->cyclone_buffer_out, 
	      "__send_requestvote");
  return 0;
}



/** Raft callback for saving voted_for field to disk.
 * This only returns when change has been made to disk. */
static int __persist_vote(raft_server_t* raft,
			  void *udata,
			  const int voted_for)
{
  int status = 0;
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  TOID(raft_pstate_t) root = POBJ_ROOT(cyclone_handle->pop_raft_state,
				       raft_pstate_t);
#ifdef TRACING
  rtc_clock timer;
  timer.start();
#endif  
  TX_BEGIN(cyclone_handle->pop_raft_state) {
    TX_ADD(root);
    D_RW(root)->voted_for = voted_for;
  }TX_ONABORT {
    status = -1;
  } TX_END
#ifdef TRACING
  timer.stop();
  //BOOST_LOG_TRIVIAL(info) << "VOTE_PERSIST_DELTA ms:" << timer.elapsed_time()/1000;
#endif
  return status;
}


/** Raft callback for saving term field to disk.
 * This only returns when change has been made to disk. */
static int __persist_term(raft_server_t* raft,
			  void *udata,
			  const int current_term)
{
  int status = 0;
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  TOID(raft_pstate_t) root = POBJ_ROOT(cyclone_handle->pop_raft_state,
				       raft_pstate_t);
#ifdef TRACING
  rtc_clock timer;
  timer.start();
#endif
  TX_BEGIN(cyclone_handle->pop_raft_state) {
    TX_ADD(root);
    D_RW(root)->term = current_term;
  } TX_ONABORT {
    status = -1;
  } TX_END
#ifdef TRACING
  timer.stop();    
  //BOOST_LOG_TRIVIAL(info) << "TERM_PERSIST_DELTA ms: " << timer.elapsed_time()/1000;
#endif
  return status;
}

static int __applylog(raft_server_t* raft,
		      void *udata,
		      raft_entry_t *ety)
{
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  unsigned char *chunk = (unsigned char *)malloc(ety->data.len);
  int delta_node_id;
  TX_BEGIN(cyclone_handle->pop_raft_state) {
    (void)cyclone_handle->read_from_log(chunk, (unsigned long)ety->data.buf);
  } TX_END
  if(ety->type != RAFT_LOGTYPE_ADD_NODE &&
     cyclone_handle->cyclone_commit_cb != NULL) {    
    cyclone_handle->cyclone_commit_cb(cyclone_handle->user_arg, chunk, ety->data.len);
  }
  if(ety->type == RAFT_LOGTYPE_REMOVE_NODE) {
    cfg_change_t *cfg = (cfg_change_t *)(chunk + sizeof(rpc_t));
    delta_node_id = cfg->node;
    BOOST_LOG_TRIVIAL(info) << "SHUTDOWN node " << delta_node_id;
    if(delta_node_id == cyclone_handle->me) {
      exit(-1);
    }
  }
  else if(ety->type == RAFT_LOGTYPE_ADD_NONVOTING_NODE) {
    cfg_change_t *cfg = (cfg_change_t *)(chunk + sizeof(rpc_t));
    delta_node_id = cfg->node;
    BOOST_LOG_TRIVIAL(info) << "INIT nonvoting node " << delta_node_id;
  }
  else if(ety->type == RAFT_LOGTYPE_ADD_NODE) {
    TX_BEGIN(cyclone_handle->pop_raft_state) {
      (void)cyclone_handle->read_from_log(chunk, (unsigned long)ety->data.buf);
    } TX_END
    int *delta_node_idp = (int *)chunk;
    BOOST_LOG_TRIVIAL(info) << "STARTUP node " << *delta_node_idp;
  }
  free(chunk);
  return 0;
}


/** Raft callback for appending an item to the log */
static int __raft_logentry_offer(raft_server_t* raft,
				 void *udata,
				 raft_entry_t *ety,
				 int ety_idx)
{
  int result = 0;
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  void *chunk = ety->data.buf;
#ifdef TRACING
    trace_pre_append((unsigned char *)chunk, ety->data.len);
#endif
  TX_BEGIN(cyclone_handle->pop_raft_state) {
#ifdef TRACING
    trace_pre_append((unsigned char *)chunk, ety->data.len);
#endif

    if(cyclone_handle->append_to_raft_log((unsigned char *)ety,
					  sizeof(raft_entry_t)) != 0) {
      pmemobj_tx_abort(-1);
    }
    void * saved_ptr = (void *)cyclone_handle->get_log_offset();
    if(cyclone_handle->append_to_raft_log((unsigned char *)ety->data.buf,
					  ety->data.len) != 0) {
      pmemobj_tx_abort(-1);
    }
    ety->data.buf = saved_ptr;
#ifdef TRACING
    trace_post_append(chunk, ety->data.len);
#endif

  } TX_ONABORT {
    result = -1;
  } TX_END
#ifdef TRACING
    trace_post_append(chunk, ety->data.len);
#endif
  if(ety->type == RAFT_LOGTYPE_ADD_NONVOTING_NODE) {
    cfg_change_t *cfg = (cfg_change_t *)((char *)chunk + sizeof(rpc_t));
    int delta_node_id = cfg->node;
    // call raft add non-voting node
    raft_add_non_voting_node(cyclone_handle->raft_handle,
			     cfg->last_included_idx,
			     cyclone_handle->router->output_socket(delta_node_id),
			     delta_node_id,
			     delta_node_id == cyclone_handle->me ? 1:0);
    
  }
  else if(ety->type == RAFT_LOGTYPE_ADD_NODE) {
    int delta_node_id = *(int *)chunk;
    // call raft add node
    raft_add_node(cyclone_handle->raft_handle,
		  cyclone_handle->router->output_socket(delta_node_id),
		  delta_node_id,
		  delta_node_id == cyclone_handle->me ? 1:0);
  }
  else if(ety->type == RAFT_LOGTYPE_REMOVE_NODE) {
    cfg_change_t *cfg = (cfg_change_t *)((char *)chunk + sizeof(rpc_t));
    int delta_node_id = cfg->node;
    // call raft remove node
    raft_remove_node(cyclone_handle->raft_handle,
		     raft_get_node(cyclone_handle->raft_handle,
				   delta_node_id));
  }
  if(cyclone_handle->cyclone_rep_cb != NULL && ety->type != RAFT_LOGTYPE_ADD_NODE) {    
    cyclone_handle->cyclone_rep_cb(cyclone_handle->user_arg,
				   (const unsigned char *)chunk,
				   ety->data.len,
				   ety_idx,
				   ety->term);
    
  }
  free(chunk); // release temporary memory
  return result;
}


/** Raft callback for removing the first entry from the log
 * @note this is provided to support log compaction in the future */
static int __raft_logentry_poll(raft_server_t* raft,
				void *udata,
				raft_entry_t *entry,
				int ety_idx)
{
  int result = 0;
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  TX_BEGIN(cyclone_handle->pop_raft_state) {
    if(cyclone_handle->remove_head_raft_log() != 0) {
      pmemobj_tx_abort(-1);
    }
    if(cyclone_handle->remove_head_raft_log() != 0) {
      pmemobj_tx_abort(-1);
    }
  } TX_ONABORT {
    result = -1;
  } TX_END
  return result;
}

/** Raft callback for deleting the most recent entry from the log.
 * This happens when an invalid leader finds a valid leader and has to delete
 * superseded log entries. */
static int __raft_logentry_pop(raft_server_t* raft,
			       void *udata,
			       raft_entry_t *entry,
			       int ety_idx)
{
  int result = 0;
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  if(raft_entry_is_cfg_change(entry)) {
    // Reverse configuration change -- TBD
  }
  if(cyclone_handle->cyclone_pop_cb != NULL && entry->type != RAFT_LOGTYPE_ADD_NODE) {
    unsigned char *chunk = (unsigned char *)malloc(entry->data.len);
    TX_BEGIN(cyclone_handle->pop_raft_state) {
      (void)cyclone_handle->read_from_log(chunk, 
					  (unsigned long)entry->data.buf);
    } TX_END
    cyclone_handle->cyclone_pop_cb(cyclone_handle->user_arg,
				   chunk,
				   entry->data.len,
				   ety_idx,
				   entry->term);
    free(chunk);
  }
  TX_BEGIN(cyclone_handle->pop_raft_state) {
    if(cyclone_handle->remove_tail_raft_log() != 0) {
      pmemobj_tx_abort(-1);
    }
    if(cyclone_handle->remove_tail_raft_log() != 0) {
      pmemobj_tx_abort(-1);
    }
  } TX_ONABORT {
    result = -1;
  } TX_END
  return result;
}

/** Raft callback for detecting when a node has sufficient logs */
void __raft_has_sufficient_logs(raft_server_t *raft,
				void *user_data,
				raft_node_t *node)
{
  msg_entry_t client_req;
  msg_entry_response_t *client_rep;
  cyclone_t* cyclone_handle = (cyclone_t *)user_data;
  client_req.id = rand();
  if(client_req.id == 0) {
    client_req.id = 1;
  }
  client_req.data.buf = malloc(sizeof(int));
  *(int *)client_req.data.buf = raft_node_get_id(node);
  BOOST_LOG_TRIVIAL(info) << "NODE HAS SUFFICIENT LOGS " << *(int *)client_req.data.buf;
  client_req.data.len = sizeof(int);
  client_req.type = RAFT_LOGTYPE_ADD_NODE;
  // TBD: Handle error
  client_rep = (msg_entry_response_t *)malloc(sizeof(msg_entry_response_t));
  (void)raft_recv_entry(cyclone_handle->raft_handle, 
			&client_req, 
			client_rep);
  free(client_rep);
}

/** Raft callback for displaying debugging information */
void __raft_log(raft_server_t* raft, 
		raft_node_t *node,
		void *udata, 
		const char *buf)
{
  //BOOST_LOG_TRIVIAL(debug) << "CYCLONE::RAFT " << buf;
}



raft_cbs_t raft_funcs = {
  __send_requestvote,
  __send_appendentries,
  __applylog,
  __persist_vote,
  __persist_term,
  __raft_logentry_offer,
  __raft_logentry_poll,
  __raft_logentry_pop,
  __raft_has_sufficient_logs,
  __raft_log,
};

int cyclone_is_leader(void *cyclone_handle)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  if(handle->replicas == 1)
    return 1;
  int leader = raft_get_current_leader(handle->raft_handle);
  return (leader == handle->me) ? 1:0;
}

int cyclone_get_leader(void *cyclone_handle)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  return raft_get_current_leader(handle->raft_handle);
}

int cyclone_get_term(void *cyclone_handle)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  return raft_get_current_term(handle->raft_handle);
}

void* cyclone_add_entry(void *cyclone_handle, void *data, int size)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  msg_t msg;
  void *cookie = NULL;
  msg.source      = handle->me;
  msg.msg_type    = MSG_CLIENT_REQ;
  msg.client.ptr  = data;
  msg.client.size = size;
#ifdef TRACING
  trace_send_cmd(data, size);
#endif
  cyclone_tx(handle->router->output_socket(handle->me), 
	     (const unsigned char *)&msg, 
	     sizeof(msg_t), 
	     "client req");
  cyclone_rx(handle->router->output_socket(handle->me),
	     (unsigned char *)&cookie,
	     sizeof(void *),
	     "CLIENT REQ recv");
  return cookie;
}

void* cyclone_add_entry_cfg(void *cyclone_handle, int type, void *data, int size)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  msg_t msg;
  void *cookie = NULL;
  msg.source      = handle->me;
  msg.msg_type    = MSG_CLIENT_REQ_CFG;
  msg.client.ptr  = data;
  msg.client.size = size;
  msg.client.type = type;
#ifdef TRACING
  trace_send_cmd(data, size);
#endif
  cyclone_tx(handle->router->output_socket(handle->me), 
	     (const unsigned char *)&msg, 
	     sizeof(msg_t), 
	     "client req");
  cyclone_rx(handle->router->output_socket(handle->me),
	     (unsigned char *)&cookie,
	     sizeof(void *),
	     "CLIENT REQ recv");
  return cookie;
}

void* cyclone_add_entry_term(void *cyclone_handle, 
			     void *data, 
			     int size,
			     int term)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  msg_t msg;
  void *cookie = NULL;
  msg.source      = handle->me;
  msg.msg_type    = MSG_CLIENT_REQ_TERM;
  msg.client.ptr  = data;
  msg.client.size = size;
  msg.client.term = term;
#ifdef TRACING
  trace_send_cmd(data, size);
#endif
  cyclone_tx(handle->router->output_socket(handle->me), 
	     (const unsigned char *)&msg, 
	     sizeof(msg_t), 
	     "client req");
  cyclone_rx(handle->router->output_socket(handle->me),
	     (unsigned char *)&cookie,
	     sizeof(void *),
	     "CLIENT REQ recv");
  return cookie;
}

void* cyclone_set_img_build(void *cyclone_handle)
			    
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  msg_t msg;
  void *cookie = NULL;
  msg.source      = handle->me;
  msg.msg_type    = MSG_CLIENT_REQ_SET_IMGBUILD;
  cyclone_tx(handle->router->output_socket(handle->me), 
	     (const unsigned char *)&msg, 
	     sizeof(msg_t), 
	     "client req");
  cyclone_rx(handle->router->output_socket(handle->me),
	     (unsigned char *)&cookie,
	     sizeof(void *),
	     "CLIENT REQ recv");
  return cookie;
}

void* cyclone_unset_img_build(void *cyclone_handle)
			    
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  msg_t msg;
  void *cookie = NULL;
  msg.source      = handle->me;
  msg.msg_type    = MSG_CLIENT_REQ_UNSET_IMGBUILD;
  cyclone_tx(handle->router->output_socket(handle->me), 
	     (const unsigned char *)&msg, 
	     sizeof(msg_t), 
	     "client req");
  cyclone_rx(handle->router->output_socket(handle->me),
	     (unsigned char *)&cookie,
	     sizeof(void *),
	     "CLIENT REQ recv");
  return cookie;
}

int cyclone_check_status(void *cyclone_handle, void *cookie)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  msg_t msg;
  msg.source      = handle->me;
  msg.msg_type    = MSG_CLIENT_STATUS;
  msg.client.ptr  = cookie;
  int result;
  cyclone_tx(handle->router->output_socket(handle->me), 
	      (unsigned char *)&msg, 
	      sizeof(msg_t), 
	      "client status");
  cyclone_rx(handle->router->output_socket(handle->me),
	      (unsigned char *)&result,
	      sizeof(int),
	      "CLIENT STATUS RECV");
  return result;
}

static void init_log(PMEMobjpool *pop, void *ptr, void *arg)
{
  struct circular_log *log = (struct circular_log *)ptr;
  log->log_head  = 0;
  log->log_tail  = 0;
}


static struct cyclone_img_load_st {
  cyclone_t * cyclone_handle;
  cyclone_build_image_t cyclone_build_image_callback;
  void operator ()()
  {
    cyclone_build_image_callback(cyclone_handle->router->control_input_socket());
    raft_unset_img_build(cyclone_handle->raft_handle);
  }
  
} cyclone_image_loader;

void* cyclone_boot(const char *config_path,
		   cyclone_callback_t cyclone_rep_callback,
		   cyclone_callback_t cyclone_pop_callback,
		   cyclone_commit_t cyclone_commit_callback,
		   cyclone_build_image_t cyclone_build_image_callback,
		   int me,
		   int replicas,
		   void *user_arg)
{
  cyclone_t *cyclone_handle;
  std::stringstream key;
  std::stringstream addr;

  cyclone_handle = new cyclone_t();
  cyclone_handle->cyclone_rep_cb = cyclone_rep_callback;
  cyclone_handle->cyclone_pop_cb = cyclone_pop_callback;
  cyclone_handle->cyclone_commit_cb = cyclone_commit_callback;
  cyclone_handle->user_arg   = user_arg;
  
  boost::property_tree::read_ini(config_path, cyclone_handle->pt);
  std::string path_raft           = cyclone_handle->pt.get<std::string>("storage.raftpath");
  char me_str[100];
  sprintf(me_str, "%d", me);
  path_raft.append(me_str);
  cyclone_handle->RAFT_LOGSIZE    = cyclone_handle->pt.get<unsigned long>("storage.logsize");
  cyclone_handle->replicas        = replicas;
  cyclone_handle->me              = me;
  int baseport  = cyclone_handle->pt.get<int>("quorum.baseport"); 
  cyclone_handle->raft_handle = raft_new();
  
  /* Setup raft state */
  if(access(path_raft.c_str(), F_OK)) {
    // TBD: figure out how to make this atomic
    cyclone_handle->pop_raft_state = pmemobj_create(path_raft.c_str(),
						    POBJ_LAYOUT_NAME(raft_persistent_state),
						    cyclone_handle->RAFT_LOGSIZE + PMEMOBJ_MIN_POOL,
						    0666);
    if(cyclone_handle->pop_raft_state == NULL) {
      BOOST_LOG_TRIVIAL(fatal)
	<< "Unable to creat pmemobj pool:"
	<< strerror(errno);
      exit(-1);
    }
  
    TOID(raft_pstate_t) root = POBJ_ROOT(cyclone_handle->pop_raft_state, raft_pstate_t);
    TX_BEGIN(cyclone_handle->pop_raft_state) {
      TX_ADD(root);
      D_RW(root)->term      = 0;
      D_RW(root)->voted_for = -1;
      D_RW(root)->log = 
	TX_ALLOC(struct circular_log, 
		 (sizeof(struct circular_log) + cyclone_handle->RAFT_LOGSIZE));
      log_t log = D_RO(root)->log;
      TX_ADD(log);
      D_RW(log)->log_head = 0;
      D_RW(log)->log_tail = 0;
    } TX_ONABORT {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "Unable to allocate log:"
	<< strerror(errno);
      exit(-1);
    } TX_END
  }
  else {
    cyclone_handle->pop_raft_state = pmemobj_open(path_raft.c_str(),
						  "raft_persistent_state");
    if(cyclone_handle->pop_raft_state == NULL) {
      BOOST_LOG_TRIVIAL(fatal)
	<< "Unable to open pmemobj pool:"
	<< strerror(errno);
      exit(-1);
    }
    BOOST_LOG_TRIVIAL(info) << "CYCLONE: Recovering state";
    TOID(raft_pstate_t) root = POBJ_ROOT(cyclone_handle->pop_raft_state, raft_pstate_t);
    log_t log = D_RO(root)->log;
    raft_vote(cyclone_handle->raft_handle, 
	      raft_get_node(cyclone_handle->raft_handle,
			    D_RO(root)->voted_for));
    raft_set_current_term(cyclone_handle->raft_handle, 
			  D_RO(root)->term);
    unsigned long ptr = D_RO(log)->log_head;
    raft_entry_t ety;
    int raft_idx = 0; // TBD fix this on log compaction
    while(ptr != D_RO(log)->log_tail) {
      // Optimize later by removing transaction
      TX_BEGIN(cyclone_handle->pop_raft_state) {
	ptr = cyclone_handle->read_from_log((unsigned char *)&ety, ptr);
      } TX_END
      ety.data.buf = (void *)ptr;
      TX_BEGIN(cyclone_handle->pop_raft_state) {
	ptr = cyclone_handle->skip_log_entry(ptr);
      } TX_END
      raft_append_entry(cyclone_handle->raft_handle, &ety);
      if(cyclone_rep_callback != NULL) {
	unsigned char *chunk = (unsigned char *)malloc(ety.data.len);
	TX_BEGIN(cyclone_handle->pop_raft_state) {
	  (void)cyclone_handle->read_from_log(chunk, 
					      (unsigned long)ety.data.buf);
	} TX_END
	cyclone_rep_callback(user_arg,
			     (const unsigned char *)chunk,
			     ety.data.len,
			     raft_idx++,
			     ety.term);
	free(chunk);
      }
    }
    BOOST_LOG_TRIVIAL(info) << "CYCLONE: Recovery complete";
  }

  // Note: set raft callbacks AFTER recovery
  raft_set_callbacks(cyclone_handle->raft_handle, &raft_funcs, cyclone_handle);
  raft_set_election_timeout(cyclone_handle->raft_handle, RAFT_ELECTION_TIMEOUT);
  raft_set_request_timeout(cyclone_handle->raft_handle, RAFT_REQUEST_TIMEOUT);

  cyclone_handle->throttles   = new throttle_st[cyclone_handle->replicas];

  for(int i=0;i<cyclone_handle->replicas;i++) {
    cyclone_handle->throttles[i].prev_log_term  = -1;
    cyclone_handle->throttles[i].prev_log_idx   = -1;
    cyclone_handle->throttles[i].prev_entries   = 0;
    
    cyclone_handle->throttles[i].timeout = RAFT_REQUEST_TIMEOUT/2;
  }

  /* setup connections */
  cyclone_handle->zmq_context  = zmq_init(1); // One thread should be enough ?
  cyclone_handle->router = new raft_switch(cyclone_handle->zmq_context,
					   &cyclone_handle->pt,
					   cyclone_handle->me,
					   cyclone_handle->replicas,
					   false);
  bool i_am_active = false;
  for(int i=0;i<cyclone_handle->pt.get<int>("active.replicas");i++) {
    char nodeidxkey[100];
    sprintf(nodeidxkey, "active.entry%d",i);
    int nodeidx = cyclone_handle->pt.get<int>(nodeidxkey);
    if(nodeidx == cyclone_handle->me) {
      i_am_active = true;
    }
    raft_add_peer(cyclone_handle->raft_handle,
		  cyclone_handle->router->output_socket(nodeidx),
		  nodeidx,
		  nodeidx == cyclone_handle->me ? 1:0);
  }

  cyclone_handle->cyclone_buffer_in  = new unsigned char[MSG_MAXSIZE];
  cyclone_handle->cyclone_buffer_out = new unsigned char[MSG_MAXSIZE];
  cyclone_handle->monitor_obj    = new cyclone_monitor();
  cyclone_handle->monitor_obj->cyclone_handle    = cyclone_handle;

  // Must activate myself
  if(!i_am_active) {
    raft_set_img_build(cyclone_handle->raft_handle);
    raft_add_peer(cyclone_handle->raft_handle,
		  cyclone_handle->router->output_socket(cyclone_handle->me),
		  cyclone_handle->me,
		  1);

    // Obtain and load checkpoint
    int loaded_term, loaded_idx, master;
    raft_entry_t *init_ety;
    init_build_image(cyclone_handle->router->control_input_socket(),
		     &loaded_term,
		     &loaded_idx,
		     &master,
		     (void **)&init_ety);
    if(init_ety != NULL) {
      cyclone_deserialize_last_applied(cyclone_handle, init_ety);
    }
    raft_loaded_checkpoint(cyclone_handle->raft_handle,
			   loaded_term, 
			   loaded_idx,
			   init_ety,
			   master);
    cyclone_image_loader.cyclone_handle = cyclone_handle;
    cyclone_image_loader.cyclone_build_image_callback = cyclone_build_image_callback;
    cyclone_handle->checkpoint_thread = new boost::thread(boost::ref(cyclone_image_loader));
  }
  /* Launch cyclone service */
  cyclone_handle->monitor_thread = new boost::thread(boost::ref(*cyclone_handle->monitor_obj));
  return cyclone_handle;
}

void cyclone_shutdown(void *cyclone_handle)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  handle->monitor_obj->terminate = true;
  handle->monitor_thread->join();
  handle->checkpoint_thread->join();
  delete handle->monitor_obj;
  delete handle->router;
  zmq_ctx_destroy(handle->zmq_context);
  pmemobj_close(handle->pop_raft_state);
  delete[] handle->cyclone_buffer_in;
  delete[] handle->cyclone_buffer_out;
  delete handle;
}
