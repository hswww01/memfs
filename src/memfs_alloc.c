#include "memfs_alloc.h"

#include <Windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MEMFS_ALLOC_SLAB_SIZE (64U * 1024U)
#define MEMFS_SLAB_MAGIC 0x4D46534CU

_Static_assert((MEMFS_ALLOC_SLAB_SIZE & (MEMFS_ALLOC_SLAB_SIZE - 1U)) == 0, "slab size must be a power of two");

typedef struct MemfsFreeObject {
	struct MemfsFreeObject* next;
} MemfsFreeObject;

typedef struct MemfsSlab MemfsSlab;

struct MemfsObjectPool {
	size_t object_size;
	MemfsSlab* slabs;
	MemfsSlab* available;
	SRWLOCK lock;
	uint64_t live_objects;
	uint64_t reserved_bytes;
	uint32_t slab_count;
};

struct MemfsSlab {
	MemfsSlab* next;
	MemfsSlab* prev;
	MemfsSlab* available_next;
	MemfsSlab* available_prev;
	MemfsObjectPool* owner;
	MemfsFreeObject* free_list;
	uint32_t capacity;
	uint32_t free_count;
	uint32_t magic;
	uint32_t reserved;
	uint8_t data[];
};

static const size_t g_name_pool_sizes[MEMFS_ALLOC_NAME_POOL_COUNT] = {8U,  16U,	 24U,  32U,	 48U,  64U,
																	  96U, 128U, 192U, 256U, 384U, 512U};

static size_t align_up(size_t value, size_t alignment) {
	if (value > SIZE_MAX - (alignment - 1U))
		return 0;

	return (value + alignment - 1U) & ~(alignment - 1U);
}

static size_t pool_object_size(size_t size) {
	const size_t alignment = sizeof(void*);
	size_t object_size = size < sizeof(MemfsFreeObject) ? sizeof(MemfsFreeObject) : size;

	return align_up(object_size, alignment);
}

static MemfsObjectPool* pool_create(size_t size) {
	MemfsObjectPool* pool = calloc(1, sizeof(*pool));

	if (pool == NULL)
		return NULL;

	pool->object_size = pool_object_size(size);
	if (pool->object_size == 0 ||
		pool->object_size > MEMFS_ALLOC_SLAB_SIZE - align_up(sizeof(MemfsSlab), sizeof(void*))) {
		free(pool);
		return NULL;
	}

	InitializeSRWLock(&pool->lock);
	return pool;
}

static void available_insert(MemfsObjectPool* pool, MemfsSlab* slab) {
	slab->available_prev = NULL;
	slab->available_next = pool->available;

	if (pool->available != NULL)
		pool->available->available_prev = slab;

	pool->available = slab;
}

static void available_remove(MemfsObjectPool* pool, MemfsSlab* slab) {
	if (slab->available_prev != NULL)
		slab->available_prev->available_next = slab->available_next;
	else if (pool->available == slab)
		pool->available = slab->available_next;

	if (slab->available_next != NULL)
		slab->available_next->available_prev = slab->available_prev;

	slab->available_prev = NULL;
	slab->available_next = NULL;
}

static void all_insert(MemfsObjectPool* pool, MemfsSlab* slab) {
	slab->prev = NULL;
	slab->next = pool->slabs;

	if (pool->slabs != NULL)
		pool->slabs->prev = slab;

	pool->slabs = slab;
}

static void all_remove(MemfsObjectPool* pool, MemfsSlab* slab) {
	if (slab->prev != NULL)
		slab->prev->next = slab->next;
	else
		pool->slabs = slab->next;

	if (slab->next != NULL)
		slab->next->prev = slab->prev;

	slab->prev = NULL;
	slab->next = NULL;
}

static MemfsSlab* slab_create(MemfsObjectPool* pool) {
	const size_t data_offset = align_up(sizeof(MemfsSlab), sizeof(void*));
	uint8_t* data;
	MemfsSlab* slab;
	uint32_t capacity;
	uint32_t i;

	capacity = (uint32_t)((MEMFS_ALLOC_SLAB_SIZE - data_offset) / pool->object_size);
	if (capacity == 0)
		return NULL;

	slab = VirtualAlloc(NULL, MEMFS_ALLOC_SLAB_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (slab == NULL)
		return NULL;

	memset(slab, 0, data_offset);
	slab->owner = pool;
	slab->capacity = capacity;
	slab->free_count = capacity;
	slab->magic = MEMFS_SLAB_MAGIC;

	data = (uint8_t*)slab + data_offset;
	for (i = 0; i < capacity; i++) {
		MemfsFreeObject* object = (MemfsFreeObject*)(data + (size_t)i * pool->object_size);

		object->next = slab->free_list;
		slab->free_list = object;
	}

	all_insert(pool, slab);
	available_insert(pool, slab);
	pool->slab_count++;
	pool->reserved_bytes += MEMFS_ALLOC_SLAB_SIZE;
	return slab;
}

static MemfsSlab* slab_from_object(void* ptr) {
	uintptr_t address = (uintptr_t)ptr;
	uintptr_t base = address & ~((uintptr_t)MEMFS_ALLOC_SLAB_SIZE - 1U);

	return (MemfsSlab*)base;
}

static void* pool_alloc(MemfsObjectPool* pool) {
	MemfsFreeObject* object;
	MemfsSlab* slab;

	if (pool == NULL)
		return NULL;

	AcquireSRWLockExclusive(&pool->lock);

	slab = pool->available;
	if (slab == NULL) {
		slab = slab_create(pool);
		if (slab == NULL) {
			ReleaseSRWLockExclusive(&pool->lock);
			return NULL;
		}
	}

	object = slab->free_list;
	slab->free_list = object->next;
	slab->free_count--;
	pool->live_objects++;

	if (slab->free_count == 0)
		available_remove(pool, slab);

	ReleaseSRWLockExclusive(&pool->lock);

	memset(object, 0, pool->object_size);
	return object;
}

static void pool_free(MemfsObjectPool* pool, void* ptr) {
	MemfsFreeObject* object = ptr;
	MemfsSlab* slab;
	bool release_slab = false;

	if (pool == NULL || object == NULL)
		return;

	slab = slab_from_object(ptr);
	if (slab->magic != MEMFS_SLAB_MAGIC || slab->owner != pool)
		return;

	AcquireSRWLockExclusive(&pool->lock);

	if (slab->free_count == 0)
		available_insert(pool, slab);

	object->next = slab->free_list;
	slab->free_list = object;
	slab->free_count++;

	if (pool->live_objects != 0)
		pool->live_objects--;

	/*
	 * Keep one completely empty slab per size class as a hot cache.  Any
	 * additional empty slab is returned to Windows immediately, which keeps
	 * long-lived create/delete workloads from pinning their high-water RSS.
	 */
	if (slab->free_count == slab->capacity && (pool->slab_count > 1U || pool->live_objects == 0)) {
		available_remove(pool, slab);
		all_remove(pool, slab);
		pool->slab_count--;
		pool->reserved_bytes -= MEMFS_ALLOC_SLAB_SIZE;
		slab->magic = 0;
		release_slab = true;
	}

	ReleaseSRWLockExclusive(&pool->lock);

	if (release_slab)
		VirtualFree(slab, 0, MEM_RELEASE);
}

static void pool_destroy(MemfsObjectPool* pool) {
	MemfsSlab* slab;

	if (pool == NULL)
		return;

	slab = pool->slabs;
	while (slab != NULL) {
		MemfsSlab* next = slab->next;
		slab->magic = 0;
		VirtualFree(slab, 0, MEM_RELEASE);
		slab = next;
	}

	free(pool);
}

static void pool_add_stats(MemfsObjectPool* pool, MemfsAllocatorStats* stats) {
	if (pool == NULL)
		return;

	AcquireSRWLockShared(&pool->lock);
	stats->reserved_bytes += pool->reserved_bytes;
	stats->live_bytes += pool->live_objects * pool->object_size;
	stats->live_objects += pool->live_objects;
	stats->slab_count += pool->slab_count;
	ReleaseSRWLockShared(&pool->lock);
}

static int name_pool_index(size_t bytes) {
	int i;

	for (i = 0; i < MEMFS_ALLOC_NAME_POOL_COUNT; i++) {
		if (bytes <= g_name_pool_sizes[i])
			return i;
	}

	return -1;
}

bool memfs_allocator_init(MemfsAllocator* allocator, size_t node_size, size_t dir_size, size_t page_group_size) {
	int i;

	if (allocator == NULL || node_size == 0 || dir_size == 0 || page_group_size == 0)
		return false;

	memset(allocator, 0, sizeof(*allocator));

	allocator->node_pool = pool_create(node_size);
	allocator->dir_pool = pool_create(dir_size);
	allocator->page_group_pool = pool_create(page_group_size);

	if (allocator->node_pool == NULL || allocator->dir_pool == NULL || allocator->page_group_pool == NULL) {
		memfs_allocator_destroy(allocator);
		return false;
	}

	for (i = 0; i < MEMFS_ALLOC_NAME_POOL_COUNT; i++) {
		allocator->name_pools[i] = pool_create(g_name_pool_sizes[i]);
		if (allocator->name_pools[i] == NULL) {
			memfs_allocator_destroy(allocator);
			return false;
		}
	}

	return true;
}

void memfs_allocator_destroy(MemfsAllocator* allocator) {
	int i;

	if (allocator == NULL)
		return;

	pool_destroy(allocator->node_pool);
	pool_destroy(allocator->dir_pool);
	pool_destroy(allocator->page_group_pool);

	for (i = 0; i < MEMFS_ALLOC_NAME_POOL_COUNT; i++)
		pool_destroy(allocator->name_pools[i]);

	memset(allocator, 0, sizeof(*allocator));
}

void* memfs_allocator_alloc_node(MemfsAllocator* allocator) {
	return allocator ? pool_alloc(allocator->node_pool) : NULL;
}

void memfs_allocator_free_node(MemfsAllocator* allocator, void* ptr) {
	if (allocator != NULL)
		pool_free(allocator->node_pool, ptr);
}

void* memfs_allocator_alloc_dir(MemfsAllocator* allocator) {
	return allocator ? pool_alloc(allocator->dir_pool) : NULL;
}

void memfs_allocator_free_dir(MemfsAllocator* allocator, void* ptr) {
	if (allocator != NULL)
		pool_free(allocator->dir_pool, ptr);
}

void* memfs_allocator_alloc_page_group(MemfsAllocator* allocator) {
	return allocator ? pool_alloc(allocator->page_group_pool) : NULL;
}

void memfs_allocator_free_page_group(MemfsAllocator* allocator, void* ptr) {
	if (allocator != NULL)
		pool_free(allocator->page_group_pool, ptr);
}

void* memfs_allocator_alloc_name(MemfsAllocator* allocator, size_t bytes) {
	int index;

	if (allocator == NULL || bytes == 0)
		return NULL;

	index = name_pool_index(bytes);
	if (index < 0)
		return calloc(1, bytes);

	return pool_alloc(allocator->name_pools[index]);
}

void memfs_allocator_free_name(MemfsAllocator* allocator, void* ptr, size_t bytes) {
	int index;

	if (allocator == NULL || ptr == NULL)
		return;

	index = name_pool_index(bytes);
	if (index < 0) {
		free(ptr);
		return;
	}

	pool_free(allocator->name_pools[index], ptr);
}

void memfs_allocator_get_stats(MemfsAllocator* allocator, MemfsAllocatorStats* stats) {
	int i;

	if (stats == NULL)
		return;

	memset(stats, 0, sizeof(*stats));
	if (allocator == NULL)
		return;

	pool_add_stats(allocator->node_pool, stats);
	pool_add_stats(allocator->dir_pool, stats);
	pool_add_stats(allocator->page_group_pool, stats);

	for (i = 0; i < MEMFS_ALLOC_NAME_POOL_COUNT; i++)
		pool_add_stats(allocator->name_pools[i], stats);
}
