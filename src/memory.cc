/*
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
#include "memory.h"

struct slab_arena runtime;
struct quota memory_quota;

static const size_t RUNTIME_SLAB_SIZE = 4 * 1024 * 1024;
static const size_t DEFAULT_MEM_QUOTA_SIZE = QUOTA_MAX_ALLOC;

void
memory_init()
{
	/* default quota initialization */
	quota_init(&memory_quota, DEFAULT_MEM_QUOTA_SIZE);

	/* No limit on the runtime memory. */
	slab_arena_create(&runtime, &memory_quota, 0,
		RUNTIME_SLAB_SIZE, MAP_PRIVATE);
}

void
memory_free()
{
	/*
	 * If this is called from a fiber != sched, then
	 * %rsp is pointing at the memory that we
	 * would be trying to unmap. Don't.
	 */
#if 0
	slab_arena_destroy(&runtime);
#endif
}
