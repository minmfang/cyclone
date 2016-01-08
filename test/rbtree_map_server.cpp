/*
 * Copyright (c) 2015, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include<assert.h>
#include<errno.h>
#include<libcyclone.hpp>
#include<string.h>
#include<stdlib.h>
#include "../core/logging.hpp"
#include "../core/clock.hpp"
#include<stdio.h>
extern "C" {
#include "../nvml.git/src/examples/libpmemobj/tree_map/rbtree_map.h"
}
#include "tree_map.hpp"

TOID_DECLARE(uint64_t, TOID_NUM_BASE);

static unsigned long server_id;
static TOID(struct rbtree_map) the_tree;
static PMEMobjpool *pop;
static TOID(uint64_t) version_table;

typedef struct heap_root_st {
  TOID(uint64_t) version_table;
  TOID(struct rbtree_map) the_tree;
}heap_root_t;

const unsigned long version_table_size = (1UL  << 20);
const unsigned long version_table_mask = (version_table_size - 1);						  
unsigned long key_to_vtable_index(uint64_t key)
{
  return (key & (version_table_size - 1));
}

TOID(uint64_t) new_store_item(uint64_t val)
{
  TOID(uint64_t) item = TX_ALLOC(uint64_t, sizeof(uint64_t));
  *D_RW(item) = val;
  return item;
}

int callback(const unsigned char *data,
	     const int len,
	     void **return_value)
{
  struct proposal *req = (struct proposal *)data;
  int code = req->fn;
  if(code == FN_LOCK) {
    uint64_t *vptr = D_RW(version_table) +
      key_to_vtable_index(req->kv_data.key);
    *return_value  = malloc(sizeof(struct proposal));
    struct proposal *rep  = (struct proposal *)*return_value;
    uint64_t version = *vptr;
    rep->code = version;
    if(version == req->kv_data.value) { // UNLOCKED ?
      pmemobj_tx_add_range_direct(vptr, sizeof(uint64_t));
      *vptr = *vptr + 1; // LOCK
    }
    return sizeof(struct proposal);
  }
  else if(code == FN_GET_VERSION) {
    uint64_t *vptr = D_RW(version_table) +
      key_to_vtable_index(req->k_data.key);
    *return_value  = malloc(sizeof(struct proposal));
    struct proposal *rep  = (struct proposal *)*return_value;
    uint64_t version = *vptr;
    rep->code = version;
    return sizeof(struct proposal);
  }
  else if(code == FN_UNLOCK) {
    uint64_t *vptr = D_RW(version_table) +
      key_to_vtable_index(req->kv_data.key);
    *return_value  = malloc(sizeof(struct proposal));
    struct proposal *rep  = (struct proposal *)*return_value;
    uint64_t version = *vptr;
    if(version == req->kv_data.value) { // LOCKED ?
      pmemobj_tx_add_range_direct(vptr, sizeof(uint64_t));
      *vptr = *vptr + 1; // UNLOCK
    }
    return sizeof(struct proposal);
  }
  if(code == FN_INSERT) {
    rbtree_map_insert(pop, 
		    the_tree, 
		    req->kv_data.key,
		    new_store_item(req->kv_data.value).oid);
    return 0;
  }
  else if(code == FN_DELETE) {
    PMEMoid item = rbtree_map_remove(pop,
				   the_tree, 
				   req->k_data.key);
    if(OID_IS_NULL(item)) {
      return 0;
    }
    else {
      *return_value  = malloc(sizeof(struct proposal));
      struct proposal *rep  = (struct proposal *)*return_value;
      rep->code = CODE_OK;
      rep->kv_data.key   = req->k_data.key;
      rep->kv_data.value = *(uint64_t *)pmemobj_direct(item);
      pmemobj_tx_free(item);
      return sizeof(struct proposal);
    }
  }
  else if(code == FN_LOOKUP) {
    *return_value  = malloc(sizeof(struct proposal));
    struct proposal *rep  = (struct proposal *)*return_value;
    PMEMoid item = rbtree_map_get(pop, the_tree, req->k_data.key);
    if(OID_IS_NULL(item)) {
      rep->code = CODE_NOK;
    }
    else {
      rep->code = CODE_OK;
      rep->kv_data.key     = req->k_data.key;
      rep->kv_data.value = *(uint64_t *)pmemobj_direct(item);
    }
    return sizeof(struct proposal);
  }
  else if(code == FN_BUMP) {
    *return_value  = malloc(sizeof(struct proposal));
    struct proposal *rep  = (struct proposal *)*return_value;
    PMEMoid item = rbtree_map_get(pop, the_tree, req->k_data.key);
    if(OID_IS_NULL(item)) {
      rep->code = CODE_NOK;
    }
    else {
      rep->code = CODE_OK;
      rep->kv_data.key     = req->k_data.key;
      pmemobj_tx_add_range(item, 0, sizeof(uint64_t));
      uint64_t *ptr = (uint64_t *)pmemobj_direct(item);
      (*ptr)++;
      rep->kv_data.value = *ptr;
    }
    return sizeof(struct proposal);
  }
  else {
    BOOST_LOG_TRIVIAL(fatal) << "Tree: unknown fn !";
    exit(-1);
  }
}

int callback_leader(const unsigned char *data,
		    const int len,
		    unsigned char **follower_data,
		    int * follower_data_size, 
		    void **return_value)
{
  int sz = callback(data, len, return_value);
  *follower_data = NULL;
  *follower_data_size = 0;
  return sz;
}

int callback_follower(const unsigned char *data,
		      const int len,
		      unsigned char *follower_data,
		      int follower_data_size, 
		      void **return_value)
{
  int sz = callback(data, len, return_value);
  return sz;
}


TOID(char) nvheap_setup(TOID(char) recovered,
			PMEMobjpool *state)
{
  
  TOID(char) store;
  heap_root_t heap_root;
  pop = state;
  if(TOID_IS_NULL(recovered)) {
    rbtree_map_new(state, &the_tree, NULL);
    version_table = TX_ZALLOC(uint64_t, version_table_size);
    store = TX_ALLOC(char, sizeof(heap_root_t));
    heap_root.version_table = version_table;
    heap_root.the_tree = the_tree;
    TX_MEMCPY(D_RW(store), &heap_root, sizeof(heap_root_t));
    return store;
  }
  else {
    memcpy(&heap_root, D_RO(recovered), sizeof(heap_root_t));
    the_tree = heap_root.the_tree;
    version_table = heap_root.version_table;
    return recovered;
  }
}

void gc(void *data)
{
  free(data);
}



int main(int argc, char *argv[])
{
  if(argc != 4) {
    printf("Usage: %s server_id replicas clients\n", argv[0]);
    exit(-1);
  }
  server_id = atoi(argv[1]);
  int replicas = atoi(argv[2]);
  int clients  = atoi(argv[3]);
  dispatcher_start("cyclone_test.ini", callback, 
		   callback_leader, callback_follower, 
		   gc, nvheap_setup, server_id, replicas, clients);
}


