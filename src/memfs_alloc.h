#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct MemfsObjectPool MemfsObjectPool;

enum { MEMFS_ALLOC_NAME_POOL_COUNT = 12 };

typedef struct MemfsAllocator {
	MemfsObjectPool* node_pool;
	MemfsObjectPool* dir_pool;
	MemfsObjectPool* page_group_pool;
	MemfsObjectPool* name_pools[MEMFS_ALLOC_NAME_POOL_COUNT];
} MemfsAllocator;

typedef struct MemfsAllocatorStats {
	uint64_t reserved_bytes;
	uint64_t live_bytes;
	uint64_t live_objects;
	uint32_t slab_count;
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

void memfs_allocator_get_stats(MemfsAllocator* allocator, MemfsAllocatorStats* stats);
