#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "memfs_core.h"

#define STRESS_MAX_THREADS 16
#define STRESS_OPS_PER_THREAD 128
#define STRESS_IO_BYTES (MEMFS_PAGE_SIZE * 2U)
#define STRESS_DEADLOCK_TIMEOUT_MS 120000
#define STRESS_GRACE_TIMEOUT_MS 10000

/*
 * memfs_core deliberately relies on the WinFsp FINE operation guard for
 * namespace and same-file I/O synchronization. This test mirrors that
 * contract: namespace mutations are serialized by g_namespace_lock, while
 * different private files perform storage/accounting work concurrently.
 *
 * The core does not expose lock-wait/CAS telemetry, so this test reports only
 * externally observable throughput and correctness counters.
 */
static SRWLOCK g_namespace_lock = SRWLOCK_INIT;
static volatile LONG g_stop = 0;

typedef struct StressThread {
    Memfs* fs;
    MemfsNode* dir;
    int thread_id;
    LONG64 cycles;
    LONG64 create_ops;
    LONG64 write_ops;
    LONG64 read_ops;
    LONG64 truncate_ops;
    LONG64 rename_ops;
    LONG64 unlink_ops;
    LONG64 errors;
} StressThread;

static LONG64 qpc_ticks(void) {
    LARGE_INTEGER counter;

    if (!QueryPerformanceCounter(&counter))
        return 0;
    return counter.QuadPart;
}

static bool stop_requested(void) {
    return InterlockedCompareExchange(&g_stop, 0, 0) != 0;
}

static MemfsResult guarded_create(
    Memfs* fs,
    MemfsNode* parent,
    const wchar_t* name,
    bool directory,
    MemfsNode** out_node) {
    MemfsResult result;

    AcquireSRWLockExclusive(&g_namespace_lock);
    result = memfs_node_create(
        fs,
        parent,
        name,
        directory,
        directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
        NULL,
        0,
        out_node);
    ReleaseSRWLockExclusive(&g_namespace_lock);
    return result;
}

static MemfsResult guarded_rename(
    MemfsNode* node,
    MemfsNode* parent,
    const wchar_t* name) {
    MemfsResult result;

    AcquireSRWLockExclusive(&g_namespace_lock);
    result = memfs_node_rename(node, parent, name, false);
    ReleaseSRWLockExclusive(&g_namespace_lock);
    return result;
}

static MemfsResult guarded_unlink(MemfsNode* node) {
    MemfsResult result;

    AcquireSRWLockExclusive(&g_namespace_lock);
    result = memfs_node_unlink(node);
    ReleaseSRWLockExclusive(&g_namespace_lock);
    return result;
}

static void cleanup_file(StressThread* thread, MemfsNode* node) {
    if (node == NULL)
        return;

    /* Close is asynchronous under WinFsp FINE. Exercise the actual orphan
     * path instead of avoiding the shared list by closing before unlink. */
    if (guarded_unlink(node) != MEMFS_OK)
        thread->errors++;
    memfs_node_close(node);
}

static DWORD WINAPI stress_worker(LPVOID argument) {
    StressThread* thread = (StressThread*)argument;
    unsigned char write_buffer[STRESS_IO_BYTES];
    unsigned char read_buffer[STRESS_IO_BYTES];

    for (int i = 0; i < STRESS_OPS_PER_THREAD && !stop_requested(); i++) {
        wchar_t original_name[64];
        wchar_t renamed_name[64];
        MemfsNode* node = NULL;
        uint32_t written = 0;
        uint32_t read = 0;
        MemfsResult result;

        swprintf(original_name, 64, L"f%04d.bin", i);
        swprintf(renamed_name, 64, L"r%04d.bin", i);

        result = guarded_create(
            thread->fs, thread->dir, original_name, false, &node);
        if (result != MEMFS_OK || node == NULL) {
            thread->errors++;
            thread->cycles++;
            continue;
        }
        thread->create_ops++;

        memset(
            write_buffer,
            (unsigned char)((thread->thread_id * 31 + i) & 0xff),
            sizeof(write_buffer));
        memset(read_buffer, 0, sizeof(read_buffer));

        result = memfs_node_write(
            node,
            write_buffer,
            0,
            (uint32_t)sizeof(write_buffer),
            false,
            false,
            &written);
        if (result != MEMFS_OK || written != sizeof(write_buffer)) {
            thread->errors++;
            cleanup_file(thread, node);
            thread->cycles++;
            continue;
        }
        thread->write_ops++;

        result = memfs_node_read(
            node,
            read_buffer,
            0,
            (uint32_t)sizeof(read_buffer),
            &read);
        if (result != MEMFS_OK
            || read != sizeof(read_buffer)
            || memcmp(write_buffer, read_buffer, sizeof(write_buffer)) != 0) {
            thread->errors++;
            cleanup_file(thread, node);
            thread->cycles++;
            continue;
        }
        thread->read_ops++;

        result = memfs_node_set_file_size(node, MEMFS_PAGE_SIZE);
        if (result != MEMFS_OK) {
            thread->errors++;
            cleanup_file(thread, node);
            thread->cycles++;
            continue;
        }
        thread->truncate_ops++;

        result = guarded_rename(node, thread->dir, renamed_name);
        if (result != MEMFS_OK) {
            thread->errors++;
            cleanup_file(thread, node);
            thread->cycles++;
            continue;
        }
        thread->rename_ops++;

        /* Namespace mutation remains externally serialized, but independent
         * final closes must be safe without the namespace guard. */
        result = guarded_unlink(node);
        memfs_node_close(node);
        if (result != MEMFS_OK) {
            thread->errors++;
            thread->cycles++;
            continue;
        }
        thread->unlink_ops++;
        thread->cycles++;
    }

    return 0;
}

static int cleanup_directories(StressThread* threads, int count) {
    int errors = 0;

    for (int i = 0; i < count; i++) {
        MemfsNode* dir = threads[i].dir;

        if (dir == NULL)
            continue;

        memfs_node_close(dir);
        if (guarded_unlink(dir) != MEMFS_OK)
            errors++;
        threads[i].dir = NULL;
    }

    return errors;
}

static int run_stress(int thread_count, const char* label) {
    Memfs* fs = NULL;
    StressThread threads[STRESS_MAX_THREADS];
    HANDLE handles[STRESS_MAX_THREADS];
    int started = 0;
    bool fatal_timeout = false;
    LONG64 wall_start;
    LONG64 wall_end;
    LARGE_INTEGER frequency;
    LONG64 cycles = 0;
    LONG64 creates = 0;
    LONG64 writes = 0;
    LONG64 reads = 0;
    LONG64 truncates = 0;
    LONG64 renames = 0;
    LONG64 unlinks = 0;
    LONG64 errors = 0;
    LONG64 total_ops;
    LONG64 expected_cycles;
    double seconds;
    double total_ops_per_second;
    double per_thread_ops_per_second;

    if (memfs_create(1024ULL * 1024ULL * 1024ULL, L"stress", &fs) != MEMFS_OK) {
        printf("[%s] create fs failed\n", label);
        return 1;
    }

    memset(threads, 0, sizeof(threads));
    memset(handles, 0, sizeof(handles));
    InterlockedExchange(&g_stop, 0);

    /*
     * Root namespace setup is intentionally single-threaded. Each worker then
     * owns exactly one directory, matching the external WinFsp guard contract.
     */
    for (int i = 0; i < thread_count; i++) {
        wchar_t dir_name[32];

        threads[i].fs = fs;
        threads[i].thread_id = i;
        swprintf(dir_name, 32, L"t%02d", i);
        if (guarded_create(fs, fs->root, dir_name, true, &threads[i].dir)
            != MEMFS_OK
            || threads[i].dir == NULL) {
            printf("[%s] failed to create directory for thread %d\n", label, i);
            errors++;
            errors += cleanup_directories(threads, i);
            memfs_destroy(fs);
            return 1;
        }
    }

    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        printf("[%s] QueryPerformanceFrequency failed\n", label);
        errors += cleanup_directories(threads, thread_count);
        memfs_destroy(fs);
        return 1;
    }

    wall_start = qpc_ticks();
    for (int i = 0; i < thread_count; i++) {
        handles[i] = CreateThread(
            NULL, 0, stress_worker, &threads[i], 0, NULL);
        if (handles[i] == NULL) {
            printf("[%s] CreateThread failed at thread %d\n", label, i);
            InterlockedExchange(&g_stop, 1);

            break;
        }
        started++;
    }

    if (started > 0) {
        DWORD wait = WaitForMultipleObjects(
            (DWORD)started,
            handles,
            TRUE,
            STRESS_DEADLOCK_TIMEOUT_MS);

        if (wait == WAIT_TIMEOUT || wait == WAIT_FAILED) {
            DWORD grace;

            InterlockedExchange(&g_stop, 1);
            printf(
                "[%s] worker wait %s; requesting cooperative stop\n",
                label,
                wait == WAIT_TIMEOUT ? "timed out" : "failed");
            grace = WaitForMultipleObjects(
                (DWORD)started,
                handles,
                TRUE,
                STRESS_GRACE_TIMEOUT_MS);
            if (grace == WAIT_TIMEOUT || grace == WAIT_FAILED) {
                fatal_timeout = true;
                printf(
                    "[%s] workers did not stop within grace period; "
                    "leaving fs alive until process exit\n",
                    label);
            }
        }
    }

    wall_end = qpc_ticks();

    for (int i = 0; i < started; i++) {
        if (handles[i] != NULL)
            CloseHandle(handles[i]);
    }

    if (fatal_timeout) {
        /*
         * Threads may still dereference fs. Do not free directories or fs.
         * main() treats return code 2 as fatal and exits the process.
         */
        return 2;
    }

    if (started != thread_count) {
        errors++;
    }

    for (int i = 0; i < thread_count; i++) {
        cycles += threads[i].cycles;
        creates += threads[i].create_ops;
        writes += threads[i].write_ops;
        reads += threads[i].read_ops;
        truncates += threads[i].truncate_ops;
        renames += threads[i].rename_ops;
        unlinks += threads[i].unlink_ops;
        errors += threads[i].errors;
    }

    expected_cycles = (LONG64)thread_count * STRESS_OPS_PER_THREAD;
    if (errors == 0
        && (cycles != expected_cycles
            || creates != expected_cycles
            || writes != expected_cycles
            || reads != expected_cycles
            || truncates != expected_cycles
            || renames != expected_cycles
            || unlinks != expected_cycles)) {
        printf(
            "[%s] operation-count mismatch: cycles=%lld create=%lld write=%lld "
            "read=%lld truncate=%lld rename=%lld unlink=%lld expected=%lld\n",
            label,
            (long long)cycles,
            (long long)creates,
            (long long)writes,
            (long long)reads,
            (long long)truncates,
            (long long)renames,
            (long long)unlinks,
            (long long)expected_cycles);
        errors++;
    }

    errors += cleanup_directories(threads, thread_count);

    total_ops = creates + writes + reads + truncates + renames + unlinks;
    seconds = (double)(wall_end - wall_start) / (double)frequency.QuadPart;
    total_ops_per_second = seconds > 0.0 ? (double)total_ops / seconds : 0.0;
    per_thread_ops_per_second =
        thread_count > 0 ? total_ops_per_second / thread_count : 0.0;

    printf(
        "[%s] threads=%d cycles=%lld total_ops=%lld errors=%lld "
        "total_ops/s=%.0f per_thread_ops/s=%.0f\n",
        label,
        thread_count,
        (long long)cycles,
        (long long)total_ops,
        (long long)errors,
        total_ops_per_second,
        per_thread_ops_per_second);

    memfs_destroy(fs);
    return errors == 0 ? 0 : 1;
}

int main(void) {
    static const int thread_counts[] = {1, 2, 4, 8, 16};
    static const char* labels[] = {"baseline", "t2", "t4", "t8", "t16"};
    int failed = 0;

    printf("=== memfs multithread stress test ===\n");

    for (size_t i = 0; i < sizeof(thread_counts) / sizeof(thread_counts[0]); i++) {
        int result = run_stress(thread_counts[i], labels[i]);

        if (result == 2) {
            printf("FATAL TIMEOUT DETECTED\n");
            return 2;
        }
        if (result != 0)
            failed = 1;
    }

    if (failed) {
        printf("ERRORS DETECTED\n");
        return 1;
    }

    printf("ALL STRESS TESTS PASSED\n");
    return 0;
}
