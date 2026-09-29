#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "memfs_alloc.h"

#define ALLOC_BENCH_MAX_THREADS 16
#define ALLOC_BENCH_SLOTS 64

typedef struct AllocBenchThread {
    MemfsAllocator* allocator;
    HANDLE start_event;
    size_t size;
    uint32_t iterations;
    uint64_t operations;
    uint64_t errors;
    void* slots[ALLOC_BENCH_SLOTS];
} AllocBenchThread;

static DWORD WINAPI alloc_bench_worker(LPVOID parameter) {
    AllocBenchThread* thread = (AllocBenchThread*)parameter;
    uint32_t i;

    WaitForSingleObject(thread->start_event, INFINITE);

    for (i = 0; i < thread->iterations; ++i) {
        uint32_t slot = i % ALLOC_BENCH_SLOTS;
        void* next = memfs_allocator_alloc(thread->allocator, thread->size);

        if (next == NULL) {
            thread->errors++;
            continue;
        }

        ((volatile uint8_t*)next)[0] = (uint8_t)(i + 1U);
        thread->operations++;

        if (thread->slots[slot] != NULL) {
            memfs_allocator_free(
                thread->allocator, thread->slots[slot], thread->size);
            thread->operations++;
        }
        thread->slots[slot] = next;
    }

    for (i = 0; i < ALLOC_BENCH_SLOTS; ++i) {
        if (thread->slots[i] != NULL) {
            memfs_allocator_free(
                thread->allocator, thread->slots[i], thread->size);
            thread->slots[i] = NULL;
            thread->operations++;
        }
    }

    return 0;
}

static int run_case(size_t size, int thread_count, uint32_t iterations) {
    MemfsAllocator allocator;
    MemfsAllocatorStats stats;
    AllocBenchThread contexts[ALLOC_BENCH_MAX_THREADS];
    HANDLE threads[ALLOC_BENCH_MAX_THREADS];
    HANDLE start_event = NULL;
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER end;
    uint64_t operations = 0;
    uint64_t errors = 0;
    double seconds;
    double ops_per_second;
    int started = 0;
    int result = 1;
    int i;

    memset(&allocator, 0, sizeof(allocator));
    memset(&stats, 0, sizeof(stats));
    memset(contexts, 0, sizeof(contexts));
    memset(threads, 0, sizeof(threads));

    if (!memfs_allocator_init(&allocator, 64U, 64U, 64U)) {
        fprintf(stderr, "allocator init failed\n");
        return 1;
    }

    start_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (start_event == NULL)
        goto cleanup;

    for (i = 0; i < thread_count; ++i) {
        contexts[i].allocator = &allocator;
        contexts[i].start_event = start_event;
        contexts[i].size = size;
        contexts[i].iterations = iterations;
        threads[i] = CreateThread(
            NULL, 0, alloc_bench_worker, &contexts[i], 0, NULL);
        if (threads[i] == NULL)
            goto cleanup;
        started++;
    }

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    SetEvent(start_event);

    if (WaitForMultipleObjects(
            (DWORD)started, threads, TRUE, 120000) != WAIT_OBJECT_0) {
        fprintf(stderr, "allocator benchmark timeout size=%llu threads=%d\n",
                (unsigned long long)size, thread_count);
        goto cleanup;
    }

    QueryPerformanceCounter(&end);

    for (i = 0; i < started; ++i) {
        operations += contexts[i].operations;
        errors += contexts[i].errors;
    }

    seconds = (double)(end.QuadPart - start.QuadPart)
        / (double)frequency.QuadPart;
    ops_per_second = seconds > 0.0 ? (double)operations / seconds : 0.0;

    memfs_allocator_get_stats(&allocator, &stats);

    printf(
        "size=%-6llu threads=%-2d iterations/thread=%-7u "
        "ops=%-10llu seconds=%8.4f ops/s=%12.0f "
        "errors=%llu post_slabs=%u post_areas=%u post_reserved=%llu\n",
        (unsigned long long)size,
        thread_count,
        iterations,
        (unsigned long long)operations,
        seconds,
        ops_per_second,
        (unsigned long long)errors,
        stats.slab_count,
        stats.dedicated_count,
        (unsigned long long)stats.reserved_bytes);

    if (errors != 0 || stats.live_objects != 0 ||
        stats.slab_count != 0 || stats.dedicated_count != 0) {
        fprintf(stderr,
                "allocator benchmark invariant failed size=%llu threads=%d\n",
                (unsigned long long)size, thread_count);
        goto cleanup;
    }

    result = 0;

cleanup:
    if (start_event != NULL)
        SetEvent(start_event);

    for (i = 0; i < started; ++i) {
        if (threads[i] != NULL) {
            WaitForSingleObject(threads[i], 5000);
            CloseHandle(threads[i]);
        }
    }

    if (start_event != NULL)
        CloseHandle(start_event);

    memfs_allocator_destroy(&allocator);
    return result;
}

int main(void) {
    static const int thread_counts[] = {1, 2, 4, 8, 16};
    static const struct {
        size_t size;
        uint32_t iterations;
    } workloads[] = {
        {64U, 500000U},
        {512U, 400000U},
        {4096U, 200000U},
        {16384U, 20000U},
    };
    size_t w;
    size_t t;

    printf("=== memfs allocator shared-pool contention benchmark ===\n");
    printf("one logical size-class pool is shared by all worker threads\n");

    for (w = 0; w < sizeof(workloads) / sizeof(workloads[0]); ++w) {
        printf("\n[allocation size %llu]\n",
               (unsigned long long)workloads[w].size);
        for (t = 0; t < sizeof(thread_counts) / sizeof(thread_counts[0]); ++t) {
            if (run_case(
                    workloads[w].size,
                    thread_counts[t],
                    workloads[w].iterations) != 0) {
                return 1;
            }
        }
    }

    return 0;
}
