#include "memfs_alloc.h"

#include <Windows.h>
#include <stdint.h>
#include <string.h>

#define MEMFS_ALLOC_SLAB_SIZE (64U * 1024U)
#define MEMFS_SLAB_MAGIC 0x4D46534CU
#define MEMFS_ALLOC_BOOTSTRAP_MAGIC 0x4D464253U
#define MEMFS_ALLOC_POOL_COUNT (3U + MEMFS_ALLOC_NAME_POOL_COUNT + MEMFS_ALLOC_GENERIC_POOL_COUNT)
#define MEMFS_DEDICATED_MAGIC 0x4D46444BU

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
	uint64_t committed_bytes;
	uint32_t slab_count;
	uint32_t in_use;
	uint32_t slot_index;
	uint32_t magic;
};

typedef struct MemfsBootstrap {
	uint32_t magic;
	uint32_t size;
	uint32_t pool_count;
	uint32_t reserved;
	MemfsObjectPool pools[MEMFS_ALLOC_POOL_COUNT];
} MemfsBootstrap;
struct MemfsDedicatedBlock {
	MemfsDedicatedBlock* next;
	uint8_t* backing;
	uint64_t requested_bytes;
	uint64_t allocated_bytes;
	uint64_t committed_bytes;
	uint32_t magic;
	uint32_t reserved;
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
static const size_t g_generic_pool_sizes[MEMFS_ALLOC_GENERIC_POOL_COUNT] = {
	8U,		16U,	24U,	32U,	48U,	64U,	96U,	128U,	192U,	256U,	384U,
	512U,	768U,	1024U, 1536U, 2048U, 4096U, 8192U, 16384U, 32768U
};

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

static MemfsBootstrap* bootstrap_from_pool(const MemfsObjectPool* pool) {
	MemfsBootstrap* bootstrap;
	uintptr_t base;

	if (pool == NULL || pool->magic != MEMFS_SLAB_MAGIC || pool->slot_index >= MEMFS_ALLOC_POOL_COUNT)
		return NULL;

	base = (uintptr_t)pool - offsetof(MemfsBootstrap, pools) -
		   (size_t)pool->slot_index * sizeof(MemfsObjectPool);
	bootstrap = (MemfsBootstrap*)base;
	if (bootstrap->magic != MEMFS_ALLOC_BOOTSTRAP_MAGIC ||
		bootstrap->pool_count != MEMFS_ALLOC_POOL_COUNT ||
		&bootstrap->pools[pool->slot_index] != pool)
		return NULL;

	return bootstrap;
}

static MemfsObjectPool* pool_create(MemfsBootstrap* bootstrap, uint32_t slot_index, size_t size) {
	MemfsObjectPool* pool;

	if (bootstrap == NULL || slot_index >= bootstrap->pool_count || bootstrap->pools[slot_index].in_use)
		return NULL;

	pool = &bootstrap->pools[slot_index];
	memset(pool, 0, sizeof(*pool));
	pool->object_size = pool_object_size(size);
	if (pool->object_size == 0 ||
		pool->object_size > MEMFS_ALLOC_SLAB_SIZE - align_up(sizeof(MemfsSlab), sizeof(void*)))
		return NULL;

	pool->slot_index = slot_index;
	pool->in_use = 1;
	pool->magic = MEMFS_SLAB_MAGIC;
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
	pool->committed_bytes += MEMFS_ALLOC_SLAB_SIZE;
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
		pool->committed_bytes -= MEMFS_ALLOC_SLAB_SIZE;
		slab->magic = 0;
		release_slab = true;
	}

	ReleaseSRWLockExclusive(&pool->lock);

	if (release_slab)
		VirtualFree(slab, 0, MEM_RELEASE);
}

static void pool_destroy(MemfsObjectPool* pool) {
	MemfsSlab* slab;

	if (pool == NULL || pool->magic != MEMFS_SLAB_MAGIC)
		return;

	slab = pool->slabs;
	while (slab != NULL) {
		MemfsSlab* next = slab->next;
		slab->magic = 0;
		VirtualFree(slab, 0, MEM_RELEASE);
		slab = next;
	}

	memset(pool, 0, sizeof(*pool));
}


static void pool_add_stats(MemfsObjectPool* pool, MemfsAllocatorStats* stats) {
	if (pool == NULL)
		return;

	AcquireSRWLockShared(&pool->lock);
	stats->reserved_bytes += pool->reserved_bytes;
	stats->committed_bytes += pool->committed_bytes;
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
static int generic_pool_index(size_t bytes) {
	int i;

	for (i = 0; i < MEMFS_ALLOC_GENERIC_POOL_COUNT; i++) {
		if (bytes <= g_generic_pool_sizes[i])
			return i;
	}

	return -1;
}

static uint64_t virtual_region_bytes(const void* ptr) {
	MEMORY_BASIC_INFORMATION info;

	if (ptr == NULL || VirtualQuery(ptr, &info, sizeof(info)) != sizeof(info))
		return 0;

	return (uint64_t)info.RegionSize;
}

static void* dedicated_alloc(MemfsAllocator* allocator, size_t bytes) {
	MemfsDedicatedBlock* block;
	size_t header_size;
	size_t total_size;

	if (allocator == NULL || bytes == 0)
		return NULL;

	header_size = align_up(sizeof(*block), sizeof(void*));
	if (header_size == 0 || bytes > SIZE_MAX - header_size)
		return NULL;
	total_size = header_size + bytes;

	block = VirtualAlloc(NULL, total_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (block == NULL)
		return NULL;

	memset(block, 0, header_size);
	block->backing = (uint8_t*)block + header_size;
	block->requested_bytes = bytes;
	block->allocated_bytes = virtual_region_bytes(block);
	block->committed_bytes = block->allocated_bytes;
	block->magic = MEMFS_DEDICATED_MAGIC;

	AcquireSRWLockExclusive(&allocator->dedicated_lock);
	block->next = allocator->dedicated;
	allocator->dedicated = block;
	ReleaseSRWLockExclusive(&allocator->dedicated_lock);

	return block->backing;
}

static void dedicated_free(MemfsAllocator* allocator, void* ptr) {
	MemfsDedicatedBlock* block;
	MemfsDedicatedBlock* prev;
	bool found = false;

	if (allocator == NULL || ptr == NULL)
		return;

	AcquireSRWLockExclusive(&allocator->dedicated_lock);
	prev = NULL;
	for (block = allocator->dedicated; block != NULL; block = block->next) {
		if (block->magic == MEMFS_DEDICATED_MAGIC && block->backing == ptr) {
			found = true;
			if (prev == NULL)
				allocator->dedicated = block->next;
			else
				prev->next = block->next;
			break;
		}
		prev = block;
	}
	if (!found) {
		ReleaseSRWLockExclusive(&allocator->dedicated_lock);
		return;
	}

	block->magic = 0;
	ReleaseSRWLockExclusive(&allocator->dedicated_lock);

	VirtualFree(block, 0, MEM_RELEASE);
}

static void dedicated_add_stats(MemfsAllocator* allocator, MemfsAllocatorStats* stats) {
	MemfsDedicatedBlock* block;

	if (allocator == NULL || stats == NULL)
		return;

	AcquireSRWLockShared(&allocator->dedicated_lock);
	for (block = allocator->dedicated; block != NULL; block = block->next) {
		if (block->magic != MEMFS_DEDICATED_MAGIC)
			continue;
		stats->dedicated_count++;
		stats->dedicated_reserved_bytes += block->allocated_bytes;
		stats->dedicated_committed_bytes += block->committed_bytes;
		stats->dedicated_live_bytes += block->requested_bytes;
		stats->reserved_bytes += block->allocated_bytes;
		stats->committed_bytes += block->committed_bytes;
		stats->live_bytes += block->requested_bytes;
		stats->live_objects++;
	}
	ReleaseSRWLockShared(&allocator->dedicated_lock);
}

static bool bootstrap_init(MemfsBootstrap** out_bootstrap) {
	MemfsBootstrap* bootstrap;

	if (out_bootstrap == NULL)
		return false;

	*out_bootstrap = NULL;
	bootstrap = VirtualAlloc(NULL, sizeof(MemfsBootstrap), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (bootstrap == NULL)
		return false;

	memset(bootstrap, 0, sizeof(*bootstrap));
	bootstrap->magic = MEMFS_ALLOC_BOOTSTRAP_MAGIC;
	bootstrap->size = sizeof(MemfsBootstrap);
	bootstrap->pool_count = MEMFS_ALLOC_POOL_COUNT;
	*out_bootstrap = bootstrap;
	return true;
}

static void bootstrap_destroy(MemfsBootstrap* bootstrap) {
	if (bootstrap == NULL || bootstrap->magic != MEMFS_ALLOC_BOOTSTRAP_MAGIC)
		return;

	memset(bootstrap, 0, sizeof(*bootstrap));
	VirtualFree(bootstrap, 0, MEM_RELEASE);
}

static void allocator_fail_destroy(MemfsAllocator* allocator, MemfsBootstrap* bootstrap) {
	int i;

	if (allocator != NULL) {
		pool_destroy(allocator->node_pool);
		pool_destroy(allocator->dir_pool);
		pool_destroy(allocator->page_group_pool);
		for (i = 0; i < MEMFS_ALLOC_NAME_POOL_COUNT; i++)
			pool_destroy(allocator->name_pools[i]);
		for (i = 0; i < MEMFS_ALLOC_GENERIC_POOL_COUNT; i++)
			pool_destroy(allocator->generic_pools[i]);
		memset(allocator, 0, sizeof(*allocator));
	}
	bootstrap_destroy(bootstrap);
}

bool memfs_allocator_init(MemfsAllocator* allocator, size_t node_size, size_t dir_size, size_t page_group_size) {
	MemfsBootstrap* bootstrap;
	int i;

	if (allocator == NULL || node_size == 0 || dir_size == 0 || page_group_size == 0)
		return false;

	memset(allocator, 0, sizeof(*allocator));
	if (!bootstrap_init(&bootstrap))
		return false;

	allocator->node_pool = pool_create(bootstrap, 0U, node_size);
	allocator->dir_pool = pool_create(bootstrap, 1U, dir_size);
	allocator->page_group_pool = pool_create(bootstrap, 2U, page_group_size);

	if (allocator->node_pool == NULL || allocator->dir_pool == NULL || allocator->page_group_pool == NULL) {
		allocator_fail_destroy(allocator, bootstrap);
		return false;
	}

	for (i = 0; i < MEMFS_ALLOC_NAME_POOL_COUNT; i++) {
		allocator->name_pools[i] = pool_create(bootstrap, (uint32_t)(3 + i), g_name_pool_sizes[i]);
		if (allocator->name_pools[i] == NULL) {
			allocator_fail_destroy(allocator, bootstrap);
			return false;
		}
	}

	for (i = 0; i < MEMFS_ALLOC_GENERIC_POOL_COUNT; i++) {
		allocator->generic_pools[i] = pool_create(bootstrap, (uint32_t)(3 + MEMFS_ALLOC_NAME_POOL_COUNT + i),
												  g_generic_pool_sizes[i]);
		if (allocator->generic_pools[i] == NULL) {
			allocator_fail_destroy(allocator, bootstrap);
			return false;
		}
	}

	allocator->dedicated = NULL;
	InitializeSRWLock(&allocator->dedicated_lock);
	return true;
}


void memfs_allocator_destroy(MemfsAllocator* allocator) {
	MemfsBootstrap* bootstrap;
	int i;

	if (allocator == NULL)
		return;

	bootstrap = bootstrap_from_pool(allocator->node_pool);
	if (bootstrap == NULL)
		bootstrap = bootstrap_from_pool(allocator->dir_pool);
	if (bootstrap == NULL)
		bootstrap = bootstrap_from_pool(allocator->page_group_pool);
	for (i = 0; i < MEMFS_ALLOC_NAME_POOL_COUNT && bootstrap == NULL; i++)
		bootstrap = bootstrap_from_pool(allocator->name_pools[i]);

	pool_destroy(allocator->node_pool);
	pool_destroy(allocator->dir_pool);
	pool_destroy(allocator->page_group_pool);

	for (i = 0; i < MEMFS_ALLOC_NAME_POOL_COUNT; i++)
		pool_destroy(allocator->name_pools[i]);

	for (i = 0; i < MEMFS_ALLOC_GENERIC_POOL_COUNT; i++)
		pool_destroy(allocator->generic_pools[i]);

	while (allocator->dedicated != NULL) {
		MemfsDedicatedBlock* block = allocator->dedicated;
		allocator->dedicated = block->next;
		VirtualFree(block, 0, MEM_RELEASE);
	}

	memset(allocator, 0, sizeof(*allocator));
	bootstrap_destroy(bootstrap);
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
		return dedicated_alloc(allocator, bytes);

	return pool_alloc(allocator->name_pools[index]);
}

void memfs_allocator_free_name(MemfsAllocator* allocator, void* ptr, size_t bytes) {
	int index;

	if (allocator == NULL || ptr == NULL)
		return;

	index = name_pool_index(bytes);
	if (index < 0) {
		dedicated_free(allocator, ptr);
		return;
	}

	pool_free(allocator->name_pools[index], ptr);
}
void* memfs_allocator_alloc(MemfsAllocator* allocator, size_t bytes) {
	int index;

	if (allocator == NULL || bytes == 0)
		return NULL;

	index = generic_pool_index(bytes);
	if (index >= 0)
		return pool_alloc(allocator->generic_pools[index]);

	return dedicated_alloc(allocator, bytes);
}

void* memfs_allocator_alloc_zero(MemfsAllocator* allocator, size_t bytes) {
	return memfs_allocator_alloc(allocator, bytes);
}

void memfs_allocator_free(MemfsAllocator* allocator, void* ptr, size_t bytes) {
	int index;

	if (allocator == NULL || ptr == NULL)
		return;

	index = generic_pool_index(bytes);
	if (index >= 0) {
		pool_free(allocator->generic_pools[index], ptr);
		return;
	}

	dedicated_free(allocator, ptr);
}

void* memfs_allocator_realloc(MemfsAllocator* allocator, void* ptr, size_t old_bytes, size_t new_bytes) {
	void* next;
	size_t copy_bytes;
	int old_index;
	int new_index;

	if (allocator == NULL)
		return NULL;

	if (ptr == NULL)
		return memfs_allocator_alloc(allocator, new_bytes);

	if (new_bytes == 0) {
		memfs_allocator_free(allocator, ptr, old_bytes);
		return NULL;
	}

	old_index = generic_pool_index(old_bytes);
	new_index = generic_pool_index(new_bytes);

	if (old_index >= 0 && new_index >= 0 && old_index == new_index)
		return ptr;

	next = memfs_allocator_alloc(allocator, new_bytes);
	if (next == NULL)
		return NULL;

	copy_bytes = old_bytes < new_bytes ? old_bytes : new_bytes;
	memcpy(next, ptr, copy_bytes);
	memfs_allocator_free(allocator, ptr, old_bytes);
	return next;
}

void memfs_allocator_get_stats(MemfsAllocator* allocator, MemfsAllocatorStats* stats) {
	MemfsBootstrap* bootstrap;
	int i;

	if (stats == NULL)
		return;

	memset(stats, 0, sizeof(*stats));
	if (allocator == NULL)
		return;

	bootstrap = bootstrap_from_pool(allocator->node_pool);
	if (bootstrap == NULL)
		bootstrap = bootstrap_from_pool(allocator->dir_pool);
	if (bootstrap == NULL)
		bootstrap = bootstrap_from_pool(allocator->page_group_pool);
	if (bootstrap != NULL) {
		uint64_t bootstrap_bytes = virtual_region_bytes(bootstrap);

		stats->reserved_bytes += bootstrap_bytes;
		stats->committed_bytes += bootstrap_bytes;
	}

	pool_add_stats(allocator->node_pool, stats);
	pool_add_stats(allocator->dir_pool, stats);
	pool_add_stats(allocator->page_group_pool, stats);

	for (i = 0; i < MEMFS_ALLOC_NAME_POOL_COUNT; i++)
		pool_add_stats(allocator->name_pools[i], stats);

	for (i = 0; i < MEMFS_ALLOC_GENERIC_POOL_COUNT; i++)
		pool_add_stats(allocator->generic_pools[i], stats);

	dedicated_add_stats(allocator, stats);
	stats->physical_bytes = stats->committed_bytes;
}
