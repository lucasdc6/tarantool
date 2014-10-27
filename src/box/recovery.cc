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
#include "recovery.h"

#include "scoped_guard.h"
#include "fiber.h"
#include "bootstrap.h"
#include "xlog.h"
#include "xrow.h"
#include "box.h"
#include "bsync.h"
#include "replica.h"
#include "cluster.h"
#include "session.h"
#include "iproto_constants.h"

/*
 * Recovery subsystem
 * ------------------
 *
 * A facade of the recovery subsystem is struct recovery_state.
 *
 * Depending on the configuration, start-up parameters, the
 * actual task being performed, the recovery can be
 * in a different state.
 *
 * The main factors influencing recovery state are:
 * - temporal: whether or not the instance is just booting
 *   from a snapshot, is in 'local hot standby mode', or
 *   is already accepting requests
 * - task based: whether it's a master process,
 *   snapshot saving process or a replication relay.
 *
 * Depending on the above factors, recovery can be in two main
 * operation modes: "read mode", recovering in-memory state
 * from existing data, and "write mode", i.e. recording on
 * disk changes of the in-memory state.
 *
 * Let's enumerate all possible distinct states of recovery:
 *
 * Read mode
 * ---------
 * IR - initial recovery, initiated right after server start:
 * reading data from the snapshot and existing WALs
 * and restoring the in-memory state
 * IRR - initial replication relay mode, reading data from
 * existing WALs (xlogs) and sending it to the client.
 *
 * HS - standby mode, entered once all existing WALs are read:
 * following the WAL directory for all changes done by the master
 * and updating the in-memory state
 * RR - replication relay, following the WAL directory for all
 * changes done by the master and sending them to the
 * replica
 *
 * Write mode
 * ----------
 * M - master mode, recording in-memory state changes in the WAL
 * R - replica mode, receiving changes from the master and
 * recording them in the WAL
 *
 * The following state transitions are possible/supported:
 *
 * recovery_init() -> IR | IRR # recover()
 * IR -> HS         # recovery_follow_local()
 * IRR -> RR        # recovery_follow_local()
 * HS -> M          # recovery_finalize()
 * M -> R           # recovery_follow_remote()
 * R -> M           # recovery_stop_remote()
 */

/* {{{ LSN API */

void
recovery_fill_lsn(struct recovery_state *r, struct xrow_header *row)
{
	if (row->server_id == 0) {
		/* Local request. */
		row->server_id = r->server_id;
		row->lsn = vclock_inc(&r->vclock, r->server_id);
	} else {
		/* Replication request. */
		if (!vclock_has(&r->vclock, row->server_id)) {
			/*
			 * A safety net, this can only occur
			 * if we're fed a strangely broken xlog.
			 */
			tnt_raise(ClientError, ER_UNKNOWN_SERVER,
				  int2str(row->server_id));
		}
		vclock_follow(&r->vclock,  row->server_id, row->lsn);
	}
}

int64_t
recovery_last_checkpoint(struct recovery_state *r)
{
	/* recover last snapshot lsn */
	struct vclock *vclock = vclockset_last(&r->snap_dir.index);
	return vclock ? vclock_sum(vclock) : -1;
}

/* }}} */

/* {{{ Initial recovery */

/**
 * Throws an exception in  case of error.
 */
struct recovery_state *
recovery_new(const char *snap_dirname, const char *wal_dirname,
	     apply_row_f apply_row, void *apply_row_param)
{
	struct recovery_state *r = (struct recovery_state *)
			calloc(1, sizeof(*r));

	if (r == NULL) {
		tnt_raise(OutOfMemory, sizeof(*r), "malloc",
			  "struct recovery");
	}

	auto guard = make_scoped_guard([=]{
		free(r);
	});
	r->commit.begin = r->commit.end = 0;
	memset(r->commit.queue_gc_init, 0, sizeof(r->commit.queue_gc_init));
	recovery_update_mode(r, WAL_NONE);

	r->apply_row = apply_row;
	r->apply_row_param = apply_row_param;
	r->snap_io_rate_limit = UINT64_MAX;

	xdir_create(&r->snap_dir, snap_dirname, SNAP, &r->server_uuid);

	xdir_create(&r->wal_dir, wal_dirname, XLOG, &r->server_uuid);

	vclock_create(&r->vclock);

	xdir_scan(&r->snap_dir);
	/**
	 * Avoid scanning WAL dir before we recovered
	 * the snapshot and know server UUID - this will
	 * make sure the scan skips files with wrong
	 * UUID, see replication/cluster.test for
	 * details.
	 */
	xdir_check(&r->wal_dir);

	r->watcher = NULL;
	recovery_init_remote(r);

	guard.is_active = false;
	return r;
}

void
recovery_update_mode(struct recovery_state *r, enum wal_mode mode)
{
	assert(mode < WAL_MODE_MAX);
	r->wal_mode = mode;
}

void
recovery_update_io_rate_limit(struct recovery_state *r, double new_limit)
{
	r->snap_io_rate_limit = new_limit * 1024 * 1024;
	if (r->snap_io_rate_limit == 0)
		r->snap_io_rate_limit = UINT64_MAX;
}

void
recovery_setup_panic(struct recovery_state *r, bool on_snap_error,
		     bool on_wal_error)
{
	r->wal_dir.panic_if_error = on_wal_error;
	r->snap_dir.panic_if_error = on_snap_error;
}

static inline void
recovery_close_log(struct recovery_state *r)
{
	if (r->current_wal == NULL)
		return;
	if (r->current_wal->eof_read) {
		say_info("done `%s'", r->current_wal->filename);
	} else {
		say_warn("file `%s` wasn't correctly closed",
			 r->current_wal->filename);
	}
	xlog_close(r->current_wal);
	r->current_wal = NULL;
}

void
recovery_delete(struct recovery_state *r)
{
	recovery_stop_local(r);

	if (r->writer)
		wal_writer_stop(r);

	xdir_destroy(&r->snap_dir);
	xdir_destroy(&r->wal_dir);
	if (r->current_wal) {
		/*
		 * Possible if shutting down a replication
		 * relay or if error during startup.
		 */
		xlog_close(r->current_wal);
	}
	free(r);
}

void
recovery_exit(struct recovery_state *r)
{
	/* Avoid fibers, there is no event loop */
	r->watcher = NULL;
	recovery_delete(r);
}

void
recovery_atfork(struct recovery_state *r)
{
       xlog_atfork(&r->current_wal);
       /*
        * Make sure that atexit() handlers in the child do
        * not try to stop the non-existent thread.
        * The writer is not used in the child.
        */
       r->writer = NULL;
}

void
recovery_apply_row(struct recovery_state *r, struct xrow_header *row)
{
	/* Check lsn */
	if (row->bodycnt > 0) {
		int64_t current_lsn = vclock_get(&r->vclock, row->server_id);
		if (current_lsn < 0 && r != recovery)
			current_lsn = 0;
		assert(current_lsn >= 0);
		if (row->lsn > current_lsn)
			r->apply_row(r, r->apply_row_param, row);
	} else if (row->server_id > 0) {
		assert(row->commit_sn > 0 || row->rollback_sn > 0);
		r->apply_row(r, r->apply_row_param, row);
	}
}

static void
recovery_commit_rows(struct recovery_state *r, uint64_t sign, bool panic)
{
	while (vclock_sum(&r->vclock) < sign) {
		struct xrow_header *row = &r->commit.queue[r->commit.begin];
		try {
			recovery_apply_row(r, row);
			region_free(&r->commit.queue_gc[r->commit.begin]);
		} catch (ClientError *e) {
			if (panic)
				throw;
			say_error("can't apply row: ");
			e->log();
			if (row->commit_sn) {
				vclock_follow(&r->vclock, row->server_id, row->lsn);
			}
			region_free(&r->commit.queue_gc[r->commit.begin]);
		}
		if (row->server_id && vclock_get(&r->vclock, row->server_id) < row->lsn)
			vclock_follow(&r->vclock, row->server_id, row->lsn);
		assert(r->commit.end != r->commit.begin);
		if (++r->commit.begin == MAX_UNCOMMITED_REQ)
			r->commit.begin = 0;
	}
}

static void
recovery_rollback_row(struct recovery_state *r)
{
	region_free(&r->commit.queue_gc[r->commit.begin]);
	if (++r->commit.begin == MAX_UNCOMMITED_REQ)
		r->commit.begin = 0;
}

static void
recovery_rollback_end(struct recovery_state *r)
{
	while (r->commit.begin != r->commit.end)
		recovery_rollback_row(r);
}

static void
recovery_rollback_rows(struct recovery_state *r, uint64_t sign)
{
	struct vclock vclock_local;
	vclock_copy(&vclock_local, &r->vclock);
	while (vclock_sum(&vclock_local) < sign) {
		struct xrow_header *row = &r->commit.queue[r->commit.begin];
		if (row->type != IPROTO_WAL_FLAG)
			vclock_follow(&vclock_local, row->server_id, row->lsn);
		recovery_rollback_row(r);
		assert(r->commit.end != r->commit.begin);
	}
}

/**
 * Read all rows in a file starting from the last position.
 * Advance the position. If end of file is reached,
 * set l.eof_read.
 */
void
recover_xlog(struct recovery_state *r, struct xlog *l)
{
	struct xlog_cursor i;

	xlog_cursor_open(&i, l);

	auto guard = make_scoped_guard([&]{
		xlog_cursor_close(&i);
	});
	bool panic = l->dir->panic_if_error;
	struct region *reg = NULL;
	if (l->snap || r != recovery) {
		reg = &fiber()->gc;
	} else {
		reg = &r->commit.queue_gc[r->commit.end];
		if (!r->commit.queue_gc_init[r->commit.end])
			region_create(reg, &cord()->slabc);
	}
	/*
	 * xlog_cursor_next() returns 1 when
	 * it can not read more rows. This doesn't mean
	 * the file is fully read: it's fully read only
	 * when EOF marker has been read, see i.eof_read
	 */
	for (struct xrow_header *row = &r->commit.queue[r->commit.end];
		xlog_cursor_next(&i, row, reg) == 0;
		row = &r->commit.queue[r->commit.end])
	{
		if (l->snap || r != recovery) {
			recovery_apply_row(r, row);
			continue;
		}
		if (++r->commit.end == MAX_UNCOMMITED_REQ)
			r->commit.end = 0;
		assert(r->commit.end != r->commit.begin);

		reg = &r->commit.queue_gc[r->commit.end];
		if (!r->commit.queue_gc_init[r->commit.end])
			region_create(reg, &cord()->slabc);

		if (row->commit_sn == 0 && row->rollback_sn == 0)
			continue;
		if (row->commit_sn && row->rollback_sn) {
			if (row->commit_sn > row->rollback_sn) {
				recovery_rollback_rows(r, row->rollback_sn);
				recovery_commit_rows(r, row->commit_sn, panic);
			} else {
				recovery_commit_rows(r, row->commit_sn, panic);
				recovery_rollback_rows(r, row->rollback_sn);
			}
		} else {
			if (row->commit_sn)
				recovery_commit_rows(r, row->commit_sn, panic);
			else
				recovery_rollback_rows(r, row->rollback_sn);
		}
	}
	/**
	 * We should never try to read snapshots with no EOF
	 * marker - such snapshots are very likely unfinished
	 * or corrupted, and should not be trusted.
	 */
	if (l->dir->type == SNAP && l->is_inprogress == false &&
	    i.eof_read == false) {
		panic("snapshot `%s' has no EOF marker", l->filename);
	}

}

void
recovery_bootstrap(struct recovery_state *r)
{
	/* Add a surrogate server id for snapshot rows */
	vclock_add_server(&r->vclock, 0);

	/* Recover from bootstrap.snap */
	say_info("initializing an empty data directory");
	const char *filename = "bootstrap.snap";
	FILE *f = fmemopen((void *) &bootstrap_bin,
			   sizeof(bootstrap_bin), "r");
	struct xlog *snap = xlog_open_stream(&r->snap_dir, 0, f, filename, true);
	auto guard = make_scoped_guard([=]{
		xlog_close(snap);
	});
	/** The snapshot must have a EOF marker. */
	recover_xlog(r, snap);
	assert(r->commit.begin == r->commit.end);
}

/**
 * Find out if there are new .xlog files since the current
 * LSN, and read them all up.
 *
 * This function will not close r->current_wal if
 * recovery was successful.
 */
static void
recover_remaining_wals(struct recovery_state *r)
{
	xdir_scan(&r->wal_dir);

	struct vclock *last = vclockset_last(&r->wal_dir.index);
	if (last == NULL) {
		assert(r->current_wal == NULL);
		/** Nothing to do. */
		return;
	}
	assert(vclockset_next(&r->wal_dir.index, last) == NULL);

	/* If the caller already opened WAL for us, recover from it first */
	struct vclock *clock;
	if (r->current_wal != NULL) {
		clock = vclockset_match(&r->wal_dir.index,
					&r->current_wal->vclock);
		if (clock != NULL &&
		    vclock_compare(clock, &r->current_wal->vclock) == 0)
			goto recover_current_wal;
		/*
		 * The current WAL has disappeared under our feet -
		 * assume anything can happen in production and go
		 * on.
		 */
		assert(false);
	}

	for (clock = vclockset_match(&r->wal_dir.index, &r->vclock);
	     clock != NULL;
	     clock = vclockset_next(&r->wal_dir.index, clock)) {

		if (vclock_compare(clock, &r->vclock) > 0) {
			/**
			 * The best clock we could find is
			 * greater or is incomparable with the
			 * current state of recovery.
			 */
			XlogGapError *e =
				tnt_error(XlogGapError, &r->vclock, clock);

			if (r->wal_dir.panic_if_error)
				throw e;

			e->log();
			/* Ignore missing WALs */
			say_warn("ignoring a gap in LSN");
		}
		recovery_close_log(r);

		r->current_wal = xlog_open(&r->wal_dir, vclock_sum(clock), false);

		say_info("recover from `%s'", r->current_wal->filename);

recover_current_wal:
		if (r->current_wal->eof_read == false)
			recover_xlog(r, r->current_wal);
		/**
		 * Keep the last log open to remember recovery
		 * position. This speeds up recovery in local hot
		 * standby mode, since we don't have to re-open
		 * and re-scan the last log in recovery_finalize().
		 */
	}
	recovery_rollback_end(r);
	region_free(&fiber()->gc);
}

void
recovery_finalize(struct recovery_state *r, enum wal_mode wal_mode,
		  int rows_per_wal)
{
	recovery_stop_local(r);

	r->finalize = true;

	recover_remaining_wals(r);

	recovery_close_log(r);

	if (vclockset_last(&r->wal_dir.index) != NULL &&
	    vclock_sum(&r->vclock) ==
	    vclock_sum(vclockset_last(&r->wal_dir.index))) {
		/**
		 * The last log file had zero rows -> bump
		 * LSN so that we don't stumble over this
		 * file when trying to open a new xlog
		 * for writing.
		 */
		vclock_inc(&r->vclock, r->server_id);
	}
	r->wal_mode = wal_mode;
	if (r->wal_mode == WAL_FSYNC)
		(void) strcat(r->wal_dir.open_wflags, "s");

	bsync_start(r, rows_per_wal);
}


/* }}} */

/* {{{ Local recovery: support of hot standby and replication relay */

static void
recovery_stat_cb(ev_loop *loop, ev_stat *stat, int revents);

class EvStat {
public:
	struct ev_stat stat;
	struct fiber *f;
	bool *signaled;
	char path[PATH_MAX + 1];

	inline void start(const char *path_arg)
	{
		f = fiber();
		snprintf(path, sizeof(path), "%s", path_arg);
		ev_stat_init(&stat, recovery_stat_cb, path, 0.0);
		ev_stat_start(loop(), &stat);
	}
	inline void stop()
	{
		ev_stat_stop(loop(), &stat);
	}

	EvStat(bool *signaled_arg)
		:signaled(signaled_arg)
	{
		path[0] = '\0';
		ev_stat_init(&stat, recovery_stat_cb, path, 0.0);
		stat.data = this;
	}
	~EvStat()
	{
		stop();
	};
};


static void
recovery_stat_cb(ev_loop * /* loop */, ev_stat *stat, int /* revents */)
{
	EvStat *data = (EvStat *) stat->data;
	*data->signaled = true;
	if (data->f->flags & FIBER_IS_CANCELLABLE)
		fiber_wakeup(data->f);
}

static void
recovery_follow_f(va_list ap)
try {
	struct recovery_state *r = va_arg(ap, struct recovery_state *);
	ev_tstamp wal_dir_rescan_delay = va_arg(ap, ev_tstamp);
	fiber_set_user(fiber(), &admin_credentials);

	bool signaled = false;
	EvStat stat_dir(&signaled);
	EvStat stat_file(&signaled);

	stat_dir.start(r->wal_dir.dirname);

	while (! fiber_is_cancelled()) {
		recover_remaining_wals(r);

		/**
		 * Try to switch to in-memory replication
		 */
		if (bsync_follow(r)) {
			if (!cord_is_main()) {
				say_info("async replication stopped, start sync");
			}
			break;
		}

		if (r->current_wal == NULL ||
		    strcmp(r->current_wal->filename, stat_file.path) != 0) {

			stat_file.stop();
		}

		if (r->current_wal != NULL && !ev_is_active(&stat_file.stat))
			stat_file.start(r->current_wal->filename);

		if (signaled == false) {
			/**
			 * Allow an immediate wakeup/break loop
			 * from recovery_stop_local().
			 */
			fiber_set_cancellable(true);
			fiber_yield_timeout(wal_dir_rescan_delay);
			fiber_set_cancellable(false);
		}
		signaled = false;
	}
} catch(FiberCancelException *e) {
	say_error("replication cancelled");
	throw;
} catch(Exception* e) {
	say_error("error found: %s", e->errmsg());
	throw;
}

void
recovery_follow_local(struct recovery_state *r, const char *name,
		      ev_tstamp wal_dir_rescan_delay)
{
	assert(r->writer == NULL);
	assert(r->watcher == NULL);
	r->watcher = fiber_new(name, recovery_follow_f);
	fiber_set_joinable(r->watcher, true);
	fiber_start(r->watcher, r, wal_dir_rescan_delay);
}

void
recovery_stop_local(struct recovery_state *r)
{
	if (r->watcher) {
		struct fiber *f = r->watcher;
		r->watcher = NULL;
		fiber_cancel(f);
		fiber_join(f);
	}
}

/* }}} */