// Dispatcher for cyclone
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<errno.h>
#include<unistd.h>
#include "libcyclone.hpp"
#include "../core/clock.hpp"
#include "cyclone_comm.hpp"
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include<boost/log/trivial.hpp>
#include <boost/thread.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/bind.hpp>
#include<libpmemobj.h>
#include "dispatcher_layout.hpp"


static void *cyclone_handle;
static boost::property_tree::ptree pt;


static PMEMobjpool *state;
static unsigned long seen_client_txid[MAX_CLIENTS];
static unsigned long executed_client_txid[MAX_CLIENTS];
static int me;
static unsigned long last_global_txid;

int dispatcher_me()
{
  return me;
}


void event_seen(const rpc_t *rpc)
{
  if(rpc->client_txid > seen_client_txid[rpc->client_id]) {
    seen_client_txid[rpc->client_id] = rpc->client_txid;    
  }
  if(rpc->global_txid > last_global_txid) {
    last_global_txid = rpc->global_txid;
  }
}

void event_remove(const rpc_t *rpc)
{
  if(rpc->client_txid <= seen_client_txid[rpc->client_id]) {
    seen_client_txid[rpc->client_id] = rpc->client_txid - 1;    
  }
  if(rpc->global_txid <= last_global_txid) {
    last_global_txid = rpc->global_txid - 1;
  }
}

// This function must be executed in the context of a tx
void event_executed(const rpc_t *rpc, const void* ret_value, const int ret_size)
{
  int client_id = rpc->client_id;
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  void *old = (void *)&D_RW(root)->client_state[client_id].last_return_value;
  pmemobj_tx_add_range_direct(old, sizeof(TOID(char)));
  if(!TOID_IS_NULL(D_RW(root)->client_state[client_id].last_return_value))
  {
    TX_FREE(D_RW(root)->client_state[client_id].last_return_value);
  }
  if(ret_size > 0) {
    D_RW(root)->client_state[client_id].last_return_value =
      TX_ALLOC(char, ret_size);
    TX_MEMCPY(D_RW(D_RW(root)->client_state[client_id].last_return_value),
	      ret_value,
	      ret_size);
  }
  else {
    TOID_ASSIGN(D_RW(root)->client_state[rpc->client_id].last_return_value, OID_NULL);
  }
}

void event_committed(const rpc_t *rpc)
{
  int client_id = rpc->client_id;
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  if(rpc->client_txid > D_RO(root)->client_state[client_id].committed_txid) {
    D_RW(root)->client_state[client_id].committed_txid = rpc->client_txid;
    unsigned long *ptr =
      (unsigned long  *)&D_RW(root)->client_state[client_id].committed_txid;
    pmemobj_tx_add_range_direct(ptr, sizeof(unsigned long));
    *ptr = rpc->client_txid;
  }
}

void cyclone_commit_cb(void *user_arg, const unsigned char *data, const int len)
{
  const rpc_t *rpc = (const rpc_t *)data;
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  // Execute callback as a transaction
  TX_BEGIN(state) {
    // Call up to app. -- note that the call executes in an NVML transaction
    void *ret_value;
    int sz = execute_rpc((const unsigned char *)(rpc + 1),
			 len - sizeof(rpc_t),
			 &ret_value);
    //BOOST_LOG_TRIVIAL(info) << "Execute RPC: " << rpc->global_txid;
    event_executed(rpc, data, len);
    event_committed(rpc);
  } TX_END
}

void cyclone_rep_cb(void *user_arg, const unsigned char *data, const int len)
{
  const rpc_t *rpc = (const rpc_t *)data;
  event_seen(rpc);
}

void cyclone_pop_cb(void *user_arg, const unsigned char *data, const int len)
{
  const rpc_t *rpc = (const rpc_t *)data;
  event_remove(rpc);
}


static unsigned char tx_buffer[DISP_MAX_MSGSIZE];
static unsigned char rx_buffer[DISP_MAX_MSGSIZE];

struct dispatcher_loop {
  void *zmq_context;
  cyclone_switch *router;
  int clients;
  
  void handle_rpc(unsigned long sz)
  {
    TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
    bool is_correct_txid, last_tx_committed;

    rpc_t *rpc_req = (rpc_t *)rx_buffer;
    rpc_t *rpc_rep = (rpc_t *)tx_buffer;
    unsigned long rep_sz = 0;
    switch(rpc_req->code) {
    case RPC_REQ_FN:
      rpc_rep->client_id   = rpc_req->client_id;
      rpc_rep->client_txid = rpc_req->client_txid;
      is_correct_txid =
	((seen_client_txid[rpc_req->client_id] + 1) == rpc_req->client_txid);
      last_tx_committed =
	(D_RO(root)->client_state[rpc_req->client_id].committed_txid ==
	 seen_client_txid[rpc_req->client_id]);
      if(is_correct_txid && last_tx_committed) {
	// Initiate replication
	rpc_req->global_txid = (++last_global_txid);
	void *cookie = cyclone_add_entry(cyclone_handle, rpc_req, sz);
	if(cookie != NULL) {
	  event_seen(rpc_req);
	  rep_sz = sizeof(rpc_t);
	  rpc_rep->code = RPC_REP_PENDING;
	}
	else {
	  rep_sz = sizeof(rpc_t);
	  rpc_rep->code = RPC_REP_INVSRV;
	  rpc_rep->master = cyclone_get_leader(cyclone_handle);
	}
      }
      else {
	rep_sz = sizeof(rpc_t);
	rpc_rep->code = RPC_REP_INVTXID;
	rpc_rep->client_txid = seen_client_txid[rpc_req->client_id];
      }
      break;
    case RPC_REQ_STATUS:
      rpc_rep->client_id   = rpc_req->client_id;
      rpc_rep->client_txid = rpc_req->client_txid;
      rep_sz = sizeof(rpc_t);
      if(seen_client_txid[rpc_req->client_id] != rpc_req->client_txid) {
	rpc_rep->code = RPC_REP_INVTXID;
	rpc_rep->client_txid = seen_client_txid[rpc_req->client_id];
      }
      else if(D_RO(root)->client_state[rpc_req->client_id].committed_txid
	      == rpc_req->client_txid) {
	const struct client_state_st * s =
	  &D_RO(root)->client_state[rpc_req->client_txid];
	rpc_rep->code = RPC_REP_COMPLETE;
	if(s->last_return_size > 0) {
	  memcpy(&rpc_rep->payload,
		 (void *)D_RO(s->last_return_value),
		 s->last_return_size);
	  rep_sz += s->last_return_size;
	}
      }
      else {
	rpc_rep->code = RPC_REP_PENDING;
      }
      break;
    default:
      BOOST_LOG_TRIVIAL(fatal) << "DISPATCH: unknown code";
      exit(-1);
    }
    cyclone_tx(router->output_socket(rpc_req->client_id), 
	       tx_buffer, 
	       rep_sz, 
	       "Dispatch reply");
  }

  void operator ()()
  {
    void *poll_items = setup_cyclone_inpoll(router->input_socket_array(), 
					    clients);
    while(true) {
      int e;
      do {
	e = cyclone_poll(poll_items, clients, -1);
      } while(e <= 0); 
      for(int i=0;i<clients;i++) {
	if(cyclone_socket_has_data(poll_items, i)) {
	  unsigned long sz = cyclone_rx(router->input_socket(i),
					rx_buffer,
					DISP_MAX_MSGSIZE,
					"DISP RCV");
	  handle_rpc(sz);
	}
      }
    }
  }
};

static dispatcher_loop * dispatcher_loop_obj;

void dispatcher_start(const char* config_path, rpc_callback_t rpc_callback)
{
  boost::property_tree::read_ini(config_path, pt);
  // Load/Setup state
  std::string file_path = pt.get<std::string>("dispatch.filepath");
  if(access(file_path.c_str(), F_OK)) {
    state = pmemobj_create(file_path.c_str(),
			   POBJ_LAYOUT_NAME(disp_state),
			   sizeof(disp_state_t) + PMEMOBJ_MIN_POOL,
			   0666);
    if(state == NULL) {
      BOOST_LOG_TRIVIAL(fatal)
	<< "Unable to creat pmemobj pool for dispatcher:"
	<< strerror(errno);
      exit(-1);
    }
  
    TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
    TX_BEGIN(state) {
      TX_ADD(root); // Add everything
      for(int i = 0;i < MAX_CLIENTS;i++) {
	D_RW(root)->client_state[i].committed_txid    = 0UL;
	D_RW(root)->client_state[i].last_return_size  = 0;
	TOID_ASSIGN(D_RW(root)->client_state[i].last_return_value, OID_NULL);
      }
    } TX_ONABORT {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "Unable to setup dispatcher state:"
	<< strerror(errno);
      exit(-1);
    } TX_END
  }
  else {
    state = pmemobj_open(file_path.c_str(),
			 "disp_state");
    if(state == NULL) {
      BOOST_LOG_TRIVIAL(fatal)
	<< "Unable to open pmemobj pool for dispatcher state:"
	<< strerror(errno);
      exit(-1);
    }
    BOOST_LOG_TRIVIAL(info) << "DISPATCHER: Recovered state";
  }
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  for(int i=0;i<MAX_CLIENTS;i++) {
   seen_client_txid[i] = D_RO(root)->client_state[i].committed_txid;
  }
  execute_rpc = rpc_callback;
  last_global_txid = 0; // Count up from zero, always
  // Boot cyclone -- this can lead to rep cbs on recovery
  cyclone_handle = cyclone_boot(config_path,
				&cyclone_rep_cb,
				&cyclone_pop_cb,
				&cyclone_commit_cb,
				NULL);
  // Listen on port
  void *zmq_context = zmq_init(1);
  me = pt.get<int>("network.me");
  dispatcher_loop_obj    = new dispatcher_loop();
  dispatcher_loop_obj->zmq_context = zmq_context;
  dispatcher_loop_obj->clients = pt.get<int>("dispatch.clients");
  int dispatch_server_baseport = pt.get<int>("dispatch.server_baseport");
  int dispatch_client_baseport = pt.get<int>("dispatch.client_baseport");
  dispatcher_loop_obj->router = new cyclone_switch(zmq_context,
						   &pt,
						   me,
						   dispatcher_loop_obj->clients,
						   dispatch_server_baseport,
						   dispatch_client_baseport,
						   false);
  threadpool.create_thread(boost::bind(&boost::asio::io_service::run,
				       &ioService));
  (*dispatcher_loop_obj)();
}
