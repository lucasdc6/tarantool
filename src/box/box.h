#ifndef INCLUDES_TARANTOOL_BOX_H
#define INCLUDES_TARANTOOL_BOX_H
/*
 * Copyright 2010-2015, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "trivia/util.h"
#include "tt_uuid.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/*
 * Box - data storage (spaces, indexes) and query
 * processor (INSERT, UPDATE, DELETE, SELECT, Lua)
 * subsystem of Tarantool.
 */

struct port;

/** To be called at program start. */
void box_load_cfg();
/** To be called at program end. */
void box_free(void);

/** A pthread_atfork() callback for box */
void
box_atfork();

/**
 * The main entry point to the
 * Box: callbacks into the request processor.
 * These are function pointers since they can
 * change when entering/leaving read-only mode
 * (master->slave propagation).
 */
typedef void (*box_process_func)(struct request *request, struct port *port);
/** For read-write operations. */
extern box_process_func box_process;

void
box_set_ro(bool ro);

bool
box_is_ro(void);

/** True if snapshot is in progress. */
extern bool snapshot_in_progress;
/** Incremented with each next snapshot. */
extern uint32_t snapshot_version;

/**
 * Iterate over all spaces and save them to the
 * snapshot file.
 */
int box_snapshot(void);

/**
 * Spit out some basic module status (master/slave, etc.
 */
const char *box_status(void);

void
box_authenticate(const char *user_name, uint32_t len,
		 const char *tuple, const char *tuple_end);

void
box_on_cluster_join(const tt_uuid *server_uuid);

bool
box_process_join(int fd, struct xrow_header *header);

void
box_process_subscribe(int fd, struct xrow_header *header);

void
box_generate_initial_snapshot(void *data __attribute__((unused)));

/**
 * Check Lua configuration before initialization or
 * in case of a configuration change.
 */
void
box_check_config();

void box_set_listen(void);
void box_set_replication_source(void);
void box_set_wal_mode(void);
void box_set_log_level(void);
void box_set_io_collect_interval(void);
void box_set_snap_io_rate_limit(void);
void box_set_too_long_threshold(void);
void box_set_readahead(void);

extern struct recovery_state *recovery;

#if defined(__cplusplus)
}
#endif /* defined(__cplusplus) */

struct request;
struct port;
struct box_function_ctx {
	struct request *request;
	struct port *port;
};

typedef struct tuple box_tuple_t;

/* box_select is private and used only by FFI */
API_EXPORT int
box_select(struct port *port, uint32_t space_id, uint32_t index_id,
	   int iterator, uint32_t offset, uint32_t limit,
	   const char *key, const char *key_end);

/** \cond public */
typedef struct box_function_ctx box_function_ctx_t;
API_EXPORT int
box_return_tuple(box_function_ctx_t *ctx, box_tuple_t *tuple);

API_EXPORT uint32_t
box_space_id_by_name(const char *name, uint32_t len);

API_EXPORT uint32_t
box_index_id_by_name(uint32_t space_id, const char *name, uint32_t len);

API_EXPORT int
box_insert(uint32_t space_id, const char *tuple, const char *tuple_end,
	   box_tuple_t **result);

API_EXPORT int
box_replace(uint32_t space_id, const char *tuple, const char *tuple_end,
	    box_tuple_t **result);

API_EXPORT int
box_delete(uint32_t space_id, uint32_t index_id, const char *key,
	   const char *key_end, box_tuple_t **result);

API_EXPORT int
box_update(uint32_t space_id, uint32_t index_id, const char *key,
	   const char *key_end, const char *ops, const char *ops_end,
	   int index_base, box_tuple_t **result);

API_EXPORT int
box_upsert(uint32_t space_id, uint32_t index_id, const char *key,
	   const char *key_end, const char *ops, const char *ops_end,
	   const char *tuple, const char *tuple_end, int index_base,
	   box_tuple_t **result);

/** \endcond public */

#endif /* INCLUDES_TARANTOOL_BOX_H */