#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <Windows.h>
#else
typedef void SRWLOCK;
#endif

typedef struct MemfsObjectPool MemfsObjectPool;

enum { MEMFS_ALLOC_NAME_POOL_COUNT = 12 };

enum { MEMFS_ALLOC_GENERIC_POOL_COUNT = 20 };

typedef struct MemfsDedicatedBlock MemfsDedicatedBlock;

typedef enum MemfsAllocFailPoint {
	MEMFS_ALLOC_FAIL_NONE = 0,
	MEMFS_ALLOC_FAIL_BOOTSTRAP,
	MEMFS_ALLOC_FAIL_SLAB,
	MEMFS_ALLOC_FAIL_DEDICATED,
	MEMFS_ALLOC_FAIL_NODE,
	MEMFS_ALLOC_FAIL_DIR,
	MEMFS_ALLOC_FAIL_GROUP,
	MEMFS_ALLOC_FAIL_NAME,
	MEMFS_ALLOC_FAIL_GENERIC,
	MEMFS_ALLOC_FAIL_METADATA,
	MEMFS_ALLOC_FAIL_PAGE,
	MEMFS_ALLOC_FAIL_SECURITY
} MemfsAllocFailPoint;

/*
 * Deterministic allocation-failure seam for Debug/test builds.
 * Release builds compile these hooks to constant no-ops, so MemfsAllocator ABI
 * and production hot paths remain unchanged.
 */
#if !defined(NDEBUG)
void memfs_allocator_test_fail_after(MemfsAllocFailPoint point, uint32_t successful_calls_before_failure,
									 uint32_t failure_count);
void memfs_allocator_test_clear_failures(void);
bool memfs_allocator_test_should_fail(MemfsAllocFailPoint point);
#else
static inline void memfs_allocator_test_fail_after(MemfsAllocFailPoint point,
											 uint32_t successful_calls_before_failure,
											 uint32_t failure_count) {
	(void)point;
	(void)successful_calls_before_failure;
	(void)failure_count;
}
static inline void memfs_allocator_test_clear_failures(void) {
}
static inline bool memfs_allocator_test_should_fail(MemfsAllocFailPoint point) {
	(void)point;
	return false;
}
#endif


typedef struct MemfsAllocator {
	MemfsObjectPool* node_pool;
	MemfsObjectPool* dir_pool;
	MemfsObjectPool* page_group_pool;
	MemfsObjectPool* name_pools[MEMFS_ALLOC_NAME_POOL_COUNT];
	MemfsObjectPool* generic_pools[MEMFS_ALLOC_GENERIC_POOL_COUNT];
	MemfsDedicatedBlock* dedicated;
	SRWLOCK dedicated_lock;
} MemfsAllocator;

typedef struct MemfsAllocatorStats {
	// reserved_bytes: VirtualAlloc MEM_RESERVE address-space backing.
	// committed_bytes: VirtualAlloc MEM_COMMIT physical backing actually backed by the OS.
	// physical_bytes: alias for committed_bytes, exposed for capacity policy.
	// live_bytes: usable payload/object bytes currently allocated to callers.
	uint64_t reserved_bytes;
	uint64_t committed_bytes;
	uint64_t physical_bytes;
	uint64_t live_bytes;
	uint64_t live_objects;
	uint32_t slab_count;
	uint32_t dedicated_count;
	uint64_t dedicated_reserved_bytes;
	uint64_t dedicated_committed_bytes;
	uint64_t dedicated_live_bytes;
} MemfsAllocatorStats;

bool memfs_allocator_init(MemfsAllocator* allocator, size_t node_size, size_t dir_size, size_t page_group_size);
void memfs_allocator_destroy(MemfsAllocator* allocator);

void* memfs_allocator_alloc_node(MemfsAllocator* allocator);
void memfs_allocator_free_node(MemfsAllocator* allocator, void* ptr);

void* memfs_allocator_alloc_dir(MemfsAllocator* allocator);
void memfs_allocator_free_dir(MemfsAllocator* allocator, void* ptr);

void* memfs_allocator_alloc_page_group(MemfsAllocator* allocator);
void memfs_allocator_free_page_group(MemfsAllocator* allocator, void* ptr);

void* memfs_allocator_alloc_name(MemfsAllocator* allocator, size_t bytes);
void memfs_allocator_free_name(MemfsAllocator* allocator, void* ptr, size_t bytes);
void* memfs_allocator_alloc(MemfsAllocator* allocator, size_t bytes);
void* memfs_allocator_alloc_zero(MemfsAllocator* allocator, size_t bytes);
void* memfs_allocator_realloc(MemfsAllocator* allocator, void* ptr, size_t old_bytes, size_t new_bytes);
void memfs_allocator_free(MemfsAllocator* allocator, void* ptr, size_t bytes);

void memfs_allocator_get_stats(MemfsAllocator* allocator, MemfsAllocatorStats* stats);
