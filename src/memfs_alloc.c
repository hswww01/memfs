#include "memfs_alloc.h"

#include <Windows.h>
#include <stdint.h>
#include <string.h>

#define MEMFS_SLAB_MAGIC 0x4D46534CU
#define MEMFS_ALLOC_STATE_MAGIC 0x4D464153U
#define MEMFS_AREA_MAGIC 0x4D464152U
#define MEMFS_MIN_SLAB_BYTES (4U * 1024U)
#define MEMFS_MAX_SLAB_BYTES (64U * 1024U)
#define MEMFS_ALLOC_ALIGNMENT 16U

typedef struct MemfsFreeObject {
    struct MemfsFreeObject* next;
} MemfsFreeObject;

typedef struct MemfsPool MemfsPool;
typedef struct MemfsSlab MemfsSlab;
typedef struct MemfsAreaBlock MemfsAreaBlock;

struct MemfsPool {
    MemfsAllocatorState* state;
    size_t object_size;
    size_t slab_bytes;
    MemfsSlab* slabs;
    MemfsSlab* available;
    SRWLOCK lock;
    uint64_t live_objects;
    uint64_t live_bytes;
    uint64_t reserved_bytes;
    uint64_t committed_bytes;
    uint32_t slab_count;
};

struct MemfsSlab {
    MemfsSlab* next;
    MemfsSlab* prev;
    MemfsSlab* available_next;
    MemfsSlab* available_prev;
    MemfsPool* owner;
    MemfsFreeObject* free_list;
    size_t region_bytes;
    uint32_t capacity;
    uint32_t free_count;
    uint32_t magic;
    uint32_t reserved;
};

struct MemfsAreaBlock {
    MemfsAreaBlock* next;
    MemfsAreaBlock* prev;
    MemfsAllocatorState* owner;
    uint64_t requested_bytes;
    uint64_t region_bytes;
    uint32_t magic;
    uint32_t reserved;
};

struct MemfsAllocatorState {
    uint32_t magic;
    uint32_t class_count;
    size_t allocation_granularity;
    uint64_t region_bytes;
    MemfsPool pools[MEMFS_ALLOC_CLASS_COUNT];

    SRWLOCK area_lock;
    MemfsAreaBlock* areas;
    uint64_t area_count;
    uint64_t area_live_bytes;
    uint64_t area_reserved_bytes;
    uint64_t area_committed_bytes;
};

static const size_t g_class_sizes[MEMFS_ALLOC_CLASS_COUNT] = {
    8U, 16U, 24U, 32U, 48U, 64U, 96U, 128U, 192U, 256U,
    384U, 512U, 768U, 1024U, 1536U, 2048U, 3072U, 4096U, 8192U
};

#if !defined(NDEBUG)
static volatile LONG g_memfs_alloc_fail_point = MEMFS_ALLOC_FAIL_NONE;
static volatile LONG g_memfs_alloc_fail_skip;
static volatile LONG g_memfs_alloc_fail_remaining;

void memfs_allocator_test_fail_after(MemfsAllocFailPoint point,
                                     uint32_t successful_calls_before_failure,
                                     uint32_t failure_count) {
    InterlockedExchange(&g_memfs_alloc_fail_point, MEMFS_ALLOC_FAIL_NONE);
    InterlockedExchange(&g_memfs_alloc_fail_skip, (LONG)successful_calls_before_failure);
    InterlockedExchange(&g_memfs_alloc_fail_remaining, (LONG)failure_count);
    InterlockedExchange(&g_memfs_alloc_fail_point, (LONG)point);
}

void memfs_allocator_test_clear_failures(void) {
    InterlockedExchange(&g_memfs_alloc_fail_point, MEMFS_ALLOC_FAIL_NONE);
    InterlockedExchange(&g_memfs_alloc_fail_skip, 0);
    InterlockedExchange(&g_memfs_alloc_fail_remaining, 0);
}

bool memfs_allocator_test_should_fail(MemfsAllocFailPoint point) {
    LONG current;
    LONG value;

    if (point == MEMFS_ALLOC_FAIL_NONE)
        return false;

    current = InterlockedCompareExchange(&g_memfs_alloc_fail_point, 0, 0);
    if (current != (LONG)point)
        return false;

    for (;;) {
        value = InterlockedCompareExchange(&g_memfs_alloc_fail_skip, 0, 0);
        if (value <= 0)
            break;
        if (InterlockedCompareExchange(&g_memfs_alloc_fail_skip, value - 1, value) == value)
            return false;
    }

    for (;;) {
        value = InterlockedCompareExchange(&g_memfs_alloc_fail_remaining, 0, 0);
        if (value <= 0)
            return false;
        if (InterlockedCompareExchange(&g_memfs_alloc_fail_remaining, value - 1, value) == value)
            return true;
    }
}
#endif

static size_t align_up(size_t value, size_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1U)) != 0)
        return 0;
    if (value > SIZE_MAX - (alignment - 1U))
        return 0;
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static uint64_t virtual_region_bytes(const void* ptr) {
    MEMORY_BASIC_INFORMATION info;

    if (ptr == NULL || VirtualQuery(ptr, &info, sizeof(info)) != sizeof(info))
        return 0;
    return (uint64_t)info.RegionSize;
}

static int class_index(size_t bytes) {
    int i;

    if (bytes == 0 || bytes > MEMFS_ALLOC_AREA_THRESHOLD)
        return -1;

    for (i = 0; i < MEMFS_ALLOC_CLASS_COUNT; i++) {
        if (bytes <= g_class_sizes[i])
            return i;
    }
    return -1;
}

static size_t slab_bytes_for_object(size_t object_size) {
    if (object_size <= 128U)
        return 4U * 1024U;
    if (object_size <= 256U)
        return 8U * 1024U;
    if (object_size <= 512U)
        return 16U * 1024U;
    if (object_size <= 1024U)
        return 32U * 1024U;
    return 64U * 1024U;
}

#if !defined(NDEBUG)
bool memfs_allocator_test_class_layout(size_t bytes,
                                       size_t* class_bytes,
                                       size_t* slab_bytes) {
    int index = class_index(bytes);

    if (class_bytes)
        *class_bytes = 0;
    if (slab_bytes)
        *slab_bytes = 0;
    if (index < 0)
        return false;

    if (class_bytes)
        *class_bytes = g_class_sizes[index];
    if (slab_bytes)
        *slab_bytes = slab_bytes_for_object(g_class_sizes[index]);
    return true;
}
#endif

static void available_insert(MemfsPool* pool, MemfsSlab* slab) {
    slab->available_prev = NULL;
    slab->available_next = pool->available;
    if (pool->available)
        pool->available->available_prev = slab;
    pool->available = slab;
}

static void available_remove(MemfsPool* pool, MemfsSlab* slab) {
    if (slab->available_prev)
        slab->available_prev->available_next = slab->available_next;
    else if (pool->available == slab)
        pool->available = slab->available_next;

    if (slab->available_next)
        slab->available_next->available_prev = slab->available_prev;

    slab->available_prev = NULL;
    slab->available_next = NULL;
}

static void all_insert(MemfsPool* pool, MemfsSlab* slab) {
    slab->prev = NULL;
    slab->next = pool->slabs;
    if (pool->slabs)
        pool->slabs->prev = slab;
    pool->slabs = slab;
}

static void all_remove(MemfsPool* pool, MemfsSlab* slab) {
    if (slab->prev)
        slab->prev->next = slab->next;
    else
        pool->slabs = slab->next;

    if (slab->next)
        slab->next->prev = slab->prev;

    slab->prev = NULL;
    slab->next = NULL;
}

static MemfsSlab* slab_create(MemfsPool* pool) {
    const size_t data_offset = align_up(sizeof(MemfsSlab), MEMFS_ALLOC_ALIGNMENT);
    MemfsSlab* slab;
    uint8_t* data;
    uint32_t capacity;
    uint32_t i;

    if (pool == NULL || data_offset == 0 || pool->slab_bytes <= data_offset)
        return NULL;

    capacity = (uint32_t)((pool->slab_bytes - data_offset) / pool->object_size);
    if (capacity == 0)
        return NULL;

    if (memfs_allocator_test_should_fail(MEMFS_ALLOC_FAIL_SLAB))
        return NULL;

    slab = VirtualAlloc(NULL, pool->slab_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (slab == NULL)
        return NULL;

    memset(slab, 0, data_offset);
    slab->owner = pool;
    slab->capacity = capacity;
    slab->free_count = capacity;
    slab->region_bytes = virtual_region_bytes(slab);
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
    pool->reserved_bytes += slab->region_bytes;
    pool->committed_bytes += slab->region_bytes;
    return slab;
}

static MemfsSlab* slab_from_object(MemfsAllocatorState* state, void* ptr) {
    uintptr_t address;
    uintptr_t granularity;

    if (state == NULL || ptr == NULL)
        return NULL;

    granularity = (uintptr_t)state->allocation_granularity;
    if (granularity == 0 || (granularity & (granularity - 1U)) != 0)
        return NULL;

    address = (uintptr_t)ptr;
    return (MemfsSlab*)(address & ~(granularity - 1U));
}

static void* pool_alloc(MemfsPool* pool, size_t requested_bytes) {
    MemfsSlab* slab;
    MemfsFreeObject* object;

    if (pool == NULL || requested_bytes == 0 || requested_bytes > pool->object_size)
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
    pool->live_bytes += requested_bytes;

    if (slab->free_count == 0)
        available_remove(pool, slab);

    ReleaseSRWLockExclusive(&pool->lock);

    memset(object, 0, pool->object_size);
    return object;
}

static void pool_adjust_live_bytes(MemfsPool* pool, size_t old_bytes, size_t new_bytes) {
    if (pool == NULL || old_bytes == new_bytes)
        return;

    AcquireSRWLockExclusive(&pool->lock);
    if (new_bytes >= old_bytes)
        pool->live_bytes += new_bytes - old_bytes;
    else if (pool->live_bytes >= old_bytes - new_bytes)
        pool->live_bytes -= old_bytes - new_bytes;
    else
        pool->live_bytes = 0;
    ReleaseSRWLockExclusive(&pool->lock);
}

static void pool_free(MemfsPool* pool, void* ptr, size_t requested_bytes) {
    MemfsFreeObject* object = (MemfsFreeObject*)ptr;
    MemfsSlab* slab;
    bool release_slab = false;

    if (pool == NULL || ptr == NULL)
        return;

    slab = slab_from_object(pool->state, ptr);
    if (slab == NULL)
        return;

    AcquireSRWLockExclusive(&pool->lock);

    if (slab->magic != MEMFS_SLAB_MAGIC || slab->owner != pool) {
        ReleaseSRWLockExclusive(&pool->lock);
        return;
    }

    if (slab->free_count == 0)
        available_insert(pool, slab);

    object->next = slab->free_list;
    slab->free_list = object;
    slab->free_count++;

    if (pool->live_objects)
        pool->live_objects--;
    if (pool->live_bytes >= requested_bytes)
        pool->live_bytes -= requested_bytes;
    else
        pool->live_bytes = 0;

    /*
     * Keep at most one empty slab while the class still has live objects.
     * Once a class becomes completely idle, return its final slab too.
     */
    if (slab->free_count == slab->capacity &&
        (pool->slab_count > 1U || pool->live_objects == 0)) {
        available_remove(pool, slab);
        all_remove(pool, slab);
        pool->slab_count--;
        pool->reserved_bytes -= slab->region_bytes;
        pool->committed_bytes -= slab->region_bytes;
        slab->magic = 0;
        release_slab = true;
    }

    ReleaseSRWLockExclusive(&pool->lock);

    if (release_slab)
        VirtualFree(slab, 0, MEM_RELEASE);
}

static uint64_t pool_scavenge(MemfsPool* pool) {
    MemfsSlab* slab;
    uint64_t released = 0;

    if (pool == NULL)
        return 0;

    AcquireSRWLockExclusive(&pool->lock);

    slab = pool->slabs;
    while (slab) {
        MemfsSlab* next = slab->next;

        if (slab->free_count == slab->capacity) {
            uint64_t bytes = slab->region_bytes;
            available_remove(pool, slab);
            all_remove(pool, slab);
            pool->slab_count--;
            pool->reserved_bytes -= bytes;
            pool->committed_bytes -= bytes;
            slab->magic = 0;
            VirtualFree(slab, 0, MEM_RELEASE);
            released += bytes;
        }

        slab = next;
    }

    ReleaseSRWLockExclusive(&pool->lock);
    return released;
}

static void pool_destroy(MemfsPool* pool) {
    MemfsSlab* slab;

    if (pool == NULL)
        return;

    slab = pool->slabs;
    while (slab) {
        MemfsSlab* next = slab->next;
        slab->magic = 0;
        VirtualFree(slab, 0, MEM_RELEASE);
        slab = next;
    }

    pool->slabs = NULL;
    pool->available = NULL;
    pool->live_objects = 0;
    pool->live_bytes = 0;
    pool->reserved_bytes = 0;
    pool->committed_bytes = 0;
    pool->slab_count = 0;
}

static void pool_add_stats(MemfsPool* pool, MemfsAllocatorStats* stats) {
    if (pool == NULL || stats == NULL)
        return;

    AcquireSRWLockShared(&pool->lock);
    stats->reserved_bytes += pool->reserved_bytes;
    stats->committed_bytes += pool->committed_bytes;
    stats->live_bytes += pool->live_bytes;
    stats->live_objects += pool->live_objects;
    stats->slab_count += pool->slab_count;
    ReleaseSRWLockShared(&pool->lock);
}

static size_t area_header_size(void) {
    return align_up(sizeof(MemfsAreaBlock), MEMFS_ALLOC_ALIGNMENT);
}

static void* area_alloc(MemfsAllocatorState* state, size_t bytes) {
    MemfsAreaBlock* block;
    size_t header_size;
    size_t total_size;

    if (state == NULL || bytes == 0)
        return NULL;

    header_size = area_header_size();
    if (header_size == 0 || bytes > SIZE_MAX - header_size)
        return NULL;
    total_size = header_size + bytes;

    if (memfs_allocator_test_should_fail(MEMFS_ALLOC_FAIL_DEDICATED))
        return NULL;

    block = VirtualAlloc(NULL, total_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (block == NULL)
        return NULL;

    memset(block, 0, header_size);
    block->owner = state;
    block->requested_bytes = bytes;
    block->region_bytes = virtual_region_bytes(block);
    block->magic = MEMFS_AREA_MAGIC;

    AcquireSRWLockExclusive(&state->area_lock);
    block->prev = NULL;
    block->next = state->areas;
    if (state->areas)
        state->areas->prev = block;
    state->areas = block;
    state->area_count++;
    state->area_live_bytes += bytes;
    state->area_reserved_bytes += block->region_bytes;
    state->area_committed_bytes += block->region_bytes;
    ReleaseSRWLockExclusive(&state->area_lock);

    return (uint8_t*)block + header_size;
}

static void area_free(MemfsAllocatorState* state, void* ptr) {
    MemfsAreaBlock* block;
    size_t header_size;

    if (state == NULL || ptr == NULL)
        return;

    header_size = area_header_size();
    block = (MemfsAreaBlock*)((uint8_t*)ptr - header_size);

    if (block->magic != MEMFS_AREA_MAGIC || block->owner != state)
        return;

    AcquireSRWLockExclusive(&state->area_lock);

    if (block->prev)
        block->prev->next = block->next;
    else
        state->areas = block->next;
    if (block->next)
        block->next->prev = block->prev;

    if (state->area_count)
        state->area_count--;
    if (state->area_live_bytes >= block->requested_bytes)
        state->area_live_bytes -= block->requested_bytes;
    else
        state->area_live_bytes = 0;
    if (state->area_reserved_bytes >= block->region_bytes)
        state->area_reserved_bytes -= block->region_bytes;
    else
        state->area_reserved_bytes = 0;
    if (state->area_committed_bytes >= block->region_bytes)
        state->area_committed_bytes -= block->region_bytes;
    else
        state->area_committed_bytes = 0;

    block->magic = 0;
    ReleaseSRWLockExclusive(&state->area_lock);

    VirtualFree(block, 0, MEM_RELEASE);
}

static void area_destroy_all(MemfsAllocatorState* state) {
    MemfsAreaBlock* block;

    if (state == NULL)
        return;

    AcquireSRWLockExclusive(&state->area_lock);
    block = state->areas;
    state->areas = NULL;
    state->area_count = 0;
    state->area_live_bytes = 0;
    state->area_reserved_bytes = 0;
    state->area_committed_bytes = 0;
    ReleaseSRWLockExclusive(&state->area_lock);

    while (block) {
        MemfsAreaBlock* next = block->next;
        block->magic = 0;
        VirtualFree(block, 0, MEM_RELEASE);
        block = next;
    }
}

static void area_add_stats(MemfsAllocatorState* state, MemfsAllocatorStats* stats) {
    if (state == NULL || stats == NULL)
        return;

    AcquireSRWLockShared(&state->area_lock);
    stats->dedicated_count = (uint32_t)state->area_count;
    stats->dedicated_live_bytes = state->area_live_bytes;
    stats->dedicated_reserved_bytes = state->area_reserved_bytes;
    stats->dedicated_committed_bytes = state->area_committed_bytes;

    stats->live_objects += state->area_count;
    stats->live_bytes += state->area_live_bytes;
    stats->reserved_bytes += state->area_reserved_bytes;
    stats->committed_bytes += state->area_committed_bytes;
    ReleaseSRWLockShared(&state->area_lock);
}

static MemfsAllocatorState* state_create(void) {
    MemfsAllocatorState* state;
    SYSTEM_INFO system_info;
    int i;

    if (memfs_allocator_test_should_fail(MEMFS_ALLOC_FAIL_BOOTSTRAP))
        return NULL;

    state = VirtualAlloc(NULL, sizeof(*state), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (state == NULL)
        return NULL;

    memset(state, 0, sizeof(*state));
    GetSystemInfo(&system_info);

    state->magic = MEMFS_ALLOC_STATE_MAGIC;
    state->class_count = MEMFS_ALLOC_CLASS_COUNT;
    state->allocation_granularity = system_info.dwAllocationGranularity;
    state->region_bytes = virtual_region_bytes(state);
    InitializeSRWLock(&state->area_lock);

    if (state->allocation_granularity < MEMFS_MAX_SLAB_BYTES ||
        (state->allocation_granularity & (state->allocation_granularity - 1U)) != 0) {
        VirtualFree(state, 0, MEM_RELEASE);
        return NULL;
    }

    for (i = 0; i < MEMFS_ALLOC_CLASS_COUNT; i++) {
        MemfsPool* pool = &state->pools[i];
        pool->state = state;
        pool->object_size = g_class_sizes[i];
        pool->slab_bytes = slab_bytes_for_object(pool->object_size);
        InitializeSRWLock(&pool->lock);
    }

    return state;
}

static void state_destroy(MemfsAllocatorState* state) {
    int i;

    if (state == NULL || state->magic != MEMFS_ALLOC_STATE_MAGIC)
        return;

    for (i = 0; i < MEMFS_ALLOC_CLASS_COUNT; i++)
        pool_destroy(&state->pools[i]);

    area_destroy_all(state);
    state->magic = 0;
    VirtualFree(state, 0, MEM_RELEASE);
}

static void* allocator_alloc_internal(MemfsAllocator* allocator, size_t bytes) {
    int index;

    if (allocator == NULL || allocator->state == NULL || bytes == 0)
        return NULL;

    index = class_index(bytes);
    if (index >= 0)
        return pool_alloc(&allocator->state->pools[index], bytes);

    return area_alloc(allocator->state, bytes);
}

static void allocator_free_internal(MemfsAllocator* allocator, void* ptr, size_t bytes) {
    int index;

    if (allocator == NULL || allocator->state == NULL || ptr == NULL)
        return;

    index = class_index(bytes);
    if (index >= 0)
        pool_free(&allocator->state->pools[index], ptr, bytes);
    else
        area_free(allocator->state, ptr);
}

bool memfs_allocator_init(MemfsAllocator* allocator,
                          size_t node_size,
                          size_t dir_size,
                          size_t page_group_size) {
    MemfsAllocatorState* state;

    if (allocator == NULL || node_size == 0 || dir_size == 0 || page_group_size == 0)
        return false;

    memset(allocator, 0, sizeof(*allocator));
    state = state_create();
    if (state == NULL)
        return false;

    allocator->state = state;
    allocator->node_size = node_size;
    allocator->dir_size = dir_size;
    allocator->page_group_size = page_group_size;
    return true;
}

void memfs_allocator_destroy(MemfsAllocator* allocator) {
    if (allocator == NULL)
        return;

    state_destroy(allocator->state);
    memset(allocator, 0, sizeof(*allocator));
}

void* memfs_allocator_alloc_node(MemfsAllocator* allocator) {
    if (allocator == NULL || memfs_allocator_test_should_fail(MEMFS_ALLOC_FAIL_NODE))
        return NULL;
    return allocator_alloc_internal(allocator, allocator->node_size);
}

void memfs_allocator_free_node(MemfsAllocator* allocator, void* ptr) {
    if (allocator)
        allocator_free_internal(allocator, ptr, allocator->node_size);
}

void* memfs_allocator_alloc_dir(MemfsAllocator* allocator) {
    if (allocator == NULL || memfs_allocator_test_should_fail(MEMFS_ALLOC_FAIL_DIR))
        return NULL;
    return allocator_alloc_internal(allocator, allocator->dir_size);
}

void memfs_allocator_free_dir(MemfsAllocator* allocator, void* ptr) {
    if (allocator)
        allocator_free_internal(allocator, ptr, allocator->dir_size);
}

void* memfs_allocator_alloc_page_group(MemfsAllocator* allocator) {
    if (allocator == NULL || memfs_allocator_test_should_fail(MEMFS_ALLOC_FAIL_GROUP))
        return NULL;
    return allocator_alloc_internal(allocator, allocator->page_group_size);
}

void memfs_allocator_free_page_group(MemfsAllocator* allocator, void* ptr) {
    if (allocator)
        allocator_free_internal(allocator, ptr, allocator->page_group_size);
}

void* memfs_allocator_alloc_name(MemfsAllocator* allocator, size_t bytes) {
    if (allocator == NULL || bytes == 0 ||
        memfs_allocator_test_should_fail(MEMFS_ALLOC_FAIL_NAME))
        return NULL;
    return allocator_alloc_internal(allocator, bytes);
}

void memfs_allocator_free_name(MemfsAllocator* allocator, void* ptr, size_t bytes) {
    allocator_free_internal(allocator, ptr, bytes);
}

void* memfs_allocator_alloc(MemfsAllocator* allocator, size_t bytes) {
    if (allocator == NULL || bytes == 0 ||
        memfs_allocator_test_should_fail(MEMFS_ALLOC_FAIL_GENERIC))
        return NULL;
    return allocator_alloc_internal(allocator, bytes);
}

void* memfs_allocator_alloc_zero(MemfsAllocator* allocator, size_t bytes) {
    return memfs_allocator_alloc(allocator, bytes);
}

void memfs_allocator_free(MemfsAllocator* allocator, void* ptr, size_t bytes) {
    allocator_free_internal(allocator, ptr, bytes);
}

void* memfs_allocator_realloc(MemfsAllocator* allocator,
                              void* ptr,
                              size_t old_bytes,
                              size_t new_bytes) {
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

    old_index = class_index(old_bytes);
    new_index = class_index(new_bytes);

    if (old_index >= 0 && old_index == new_index) {
        pool_adjust_live_bytes(&allocator->state->pools[old_index], old_bytes, new_bytes);
        return ptr;
    }

    next = memfs_allocator_alloc(allocator, new_bytes);
    if (next == NULL)
        return NULL;

    copy_bytes = old_bytes < new_bytes ? old_bytes : new_bytes;
    memcpy(next, ptr, copy_bytes);
    memfs_allocator_free(allocator, ptr, old_bytes);
    return next;
}

void memfs_allocator_get_stats(MemfsAllocator* allocator, MemfsAllocatorStats* stats) {
    MemfsAllocatorState* state;
    int i;

    if (stats == NULL)
        return;

    memset(stats, 0, sizeof(*stats));
    if (allocator == NULL || allocator->state == NULL)
        return;

    state = allocator->state;
    stats->reserved_bytes += state->region_bytes;
    stats->committed_bytes += state->region_bytes;

    for (i = 0; i < MEMFS_ALLOC_CLASS_COUNT; i++)
        pool_add_stats(&state->pools[i], stats);

    area_add_stats(state, stats);
    stats->physical_bytes = stats->committed_bytes;
    stats->scavenged_bytes = (uint64_t)InterlockedCompareExchange64(&allocator->scavenged_bytes, 0, 0);
    stats->scavenge_count = (uint64_t)InterlockedCompareExchange64(&allocator->scavenge_count, 0, 0);
}

uint64_t memfs_allocator_scavenge(MemfsAllocator* allocator) {
    uint64_t released = 0;
    int i;

    if (allocator == NULL || allocator->state == NULL)
        return 0;

    for (i = 0; i < MEMFS_ALLOC_CLASS_COUNT; i++)
        released += pool_scavenge(&allocator->state->pools[i]);

    if (released)
        InterlockedAdd64(&allocator->scavenged_bytes, (LONG64)released);
    InterlockedIncrement64(&allocator->scavenge_count);
    return released;
}

static INIT_ONCE g_control_once = INIT_ONCE_STATIC_INIT;
static MemfsAllocator g_control_allocator;

static BOOL CALLBACK control_allocator_init_once(PINIT_ONCE once, PVOID parameter, PVOID* context) {
    (void)once;
    (void)parameter;
    (void)context;
    return memfs_allocator_init(&g_control_allocator, sizeof(void*), sizeof(void*), sizeof(void*)) ? TRUE : FALSE;
}

static bool control_allocator_ready(void) {
    return !!InitOnceExecuteOnce(&g_control_once, control_allocator_init_once, NULL, NULL);
}

void* memfs_allocator_alloc_control(size_t bytes) {
    if (bytes == 0 || !control_allocator_ready())
        return NULL;
    return allocator_alloc_internal(&g_control_allocator, bytes);
}

void memfs_allocator_free_control(void* ptr, size_t bytes) {
    if (ptr == NULL)
        return;
    if (!control_allocator_ready())
        return;
    allocator_free_internal(&g_control_allocator, ptr, bytes);
}
