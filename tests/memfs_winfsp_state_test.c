#include <stdio.h>
#include <string.h>

#include "memfs_winfsp.h"
#include <sddl.h>

static int g_failures;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "CHECK failed: %s:%d: %s\n",                     \
                    __FILE__, __LINE__, #expr);                                \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

typedef struct SecurityWorker {
    FSP_FILE_SYSTEM* fs;
    MemfsNode* node;
    HANDLE start;
    HANDLE ready;
    HANDLE done;
    PSECURITY_DESCRIPTOR descriptor[2];
    volatile LONG errors;
    unsigned iterations;
    bool writer;
} SecurityWorker;

static DWORD WINAPI security_worker(void* opaque) {
    SecurityWorker* w = opaque;
    if (WaitForSingleObject(w->start, 10000) != WAIT_OBJECT_0)
        return 1;
    if (w->ready) SetEvent(w->ready);
    for (unsigned i = 0; i < w->iterations; ++i) {
        NTSTATUS status;
        if (w->writer) {
            status = memfs_winfsp_test_set_security(w->fs, w->node,
                DACL_SECURITY_INFORMATION, w->descriptor[i & 1]);
        } else {
            union { UINT64 alignment; BYTE data[4096]; } buffer;
            SIZE_T bytes = sizeof(buffer.data);
            if (i & 1)
                status = memfs_winfsp_test_get_security(w->fs, w->node, buffer.data, &bytes);
            else
                status = memfs_winfsp_test_get_security_by_name(w->fs, L"\\acl", buffer.data, &bytes);
            if (NT_SUCCESS(status) &&
                (bytes > sizeof(buffer.data) || !IsValidSecurityDescriptor(buffer.data)))
                InterlockedIncrement(&w->errors);
        }
        if (!NT_SUCCESS(status)) InterlockedIncrement(&w->errors);
    }
    if (w->done) SetEvent(w->done);
    return 0;
}

static void test_security_snapshot_guards(void) {
    Memfs* store = NULL;
    MemfsNode* node = NULL;
    FSP_FILE_SYSTEM fs;
    MemfsWinFsp instance;
    SecurityWorker workers[3];
    HANDLE handles[3] = {0};
    HANDLE start = NULL, ready = NULL, done = NULL;
    PSECURITY_DESCRIPTOR descriptors[2] = {0};
    DWORD count = 0;
    memset(&fs, 0, sizeof(fs));
    memset(&instance, 0, sizeof(instance));
    memset(workers, 0, sizeof(workers));
    CHECK(memfs_create(64ULL << 20, L"SD_GUARDS", &store) == MEMFS_OK);
    if (!store) return;
    CHECK(memfs_node_create(store, store->root, L"acl", false, FILE_ATTRIBUTE_NORMAL,
                           NULL, 0, &node) == MEMFS_OK);
    if (!node) goto cleanup;
    instance.store = store;
    instance.file_system = &fs;
    fs.UserContext = &instance;
    InitializeSRWLock(&fs.OpGuardLock);
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:SYG:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;WD)",
        SDDL_REVISION_1, &descriptors[0], NULL));
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:SYG:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)",
        SDDL_REVISION_1, &descriptors[1], NULL));
    if (!descriptors[0] || !descriptors[1]) goto cleanup;
    start = CreateEventW(NULL, TRUE, TRUE, NULL);
    ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    done = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(start && ready && done);
    if (!start || !ready || !done) goto cleanup;
    workers[0] = (SecurityWorker){&fs, node, start, ready, done,
        {descriptors[0], descriptors[1]}, 0, 1, true};
    /* Mirror a FILE_OPEN borrow of the descriptor under FINE's shared guard.
     * SetSecurity must not free that snapshot until the borrow ends. */
    AcquireSRWLockShared(&fs.OpGuardLock);
    handles[0] = CreateThread(NULL, 0, security_worker, &workers[0], 0, NULL);
    CHECK(handles[0] != NULL);
    if (handles[0]) {
        CHECK(WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0);
        CHECK(WaitForSingleObject(done, 50) == WAIT_TIMEOUT);
    }
    ReleaseSRWLockShared(&fs.OpGuardLock);
    if (!handles[0]) goto cleanup;
    if (WaitForSingleObject(handles[0], 10000) != WAIT_OBJECT_0) ExitProcess(2);
    CHECK(workers[0].errors == 0);
    CloseHandle(handles[0]); handles[0] = NULL;
    ResetEvent(start);
    for (unsigned i = 0; i < 3; ++i) {
        workers[i] = (SecurityWorker){&fs, node, start, NULL, NULL,
            {descriptors[0], descriptors[1]}, 0, i == 0 ? 2000U : 10000U, i == 0};
        handles[i] = CreateThread(NULL, 0, security_worker, &workers[i], 0, NULL);
        CHECK(handles[i] != NULL);
        if (!handles[i]) break;
        ++count;
    }
    SetEvent(start);
    if (count && WaitForMultipleObjects(count, handles, TRUE, 30000) != WAIT_OBJECT_0)
        ExitProcess(2);
    for (unsigned i = 0; i < count; ++i) {
        DWORD code = 1;
        CHECK(GetExitCodeThread(handles[i], &code) && code == 0);
        CHECK(workers[i].errors == 0);
        CloseHandle(handles[i]); handles[i] = NULL;
    }
    CHECK(count == 3);
    CHECK(IsValidSecurityDescriptor(memfs_node_get_security(node)->data));
    puts("security snapshot: 2000 replacements / 20000 callback reads PASS");
cleanup:
    if (start) CloseHandle(start);
    if (ready) CloseHandle(ready);
    if (done) CloseHandle(done);
    if (descriptors[0]) LocalFree(descriptors[0]);
    if (descriptors[1]) LocalFree(descriptors[1]);
    if (node) { CHECK(memfs_node_unlink(node) == MEMFS_OK); memfs_node_close(node); }
    memfs_destroy(store);
}

int main(void) {
    MemfsWinFsp instance;
    FSP_FILE_SYSTEM file_system;
    HANDLE event;

    memset(&instance, 0, sizeof(instance));
    memset(&file_system, 0, sizeof(file_system));

    event = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(event != NULL);
    if (event == NULL)
        return 1;

    instance.file_system = &file_system;
    instance.dispatcher_stopped_event = event;
    instance.dispatcher_stop_reason = MEMFS_DISPATCHER_ACTIVE;
    file_system.UserContext = &instance;
    file_system.DispatcherResult = STATUS_DEVICE_NOT_CONNECTED;

    memfs_winfsp_test_dispatcher_stopped(&instance, false);
    CHECK(WaitForSingleObject(event, 0) == WAIT_OBJECT_0);
    CHECK(!memfs_winfsp_dispatcher_stopped_normally(&instance));
    CHECK(instance.dispatcher_stop_reason ==
          MEMFS_DISPATCHER_STOPPED_ABNORMALLY);
    CHECK(memfs_winfsp_dispatcher_result(&instance) ==
          STATUS_DEVICE_NOT_CONNECTED);

    ResetEvent(event);
    InterlockedExchange(
        &instance.dispatcher_stop_reason,
        MEMFS_DISPATCHER_ACTIVE);
    file_system.DispatcherResult = STATUS_SUCCESS;

    memfs_winfsp_test_dispatcher_stopped(&instance, true);
    CHECK(WaitForSingleObject(event, 0) == WAIT_OBJECT_0);
    CHECK(memfs_winfsp_dispatcher_stopped_normally(&instance));
    CHECK(instance.dispatcher_stop_reason ==
          MEMFS_DISPATCHER_STOPPED_NORMALLY);
    CHECK(memfs_winfsp_dispatcher_result(&instance) == STATUS_SUCCESS);

    CHECK(memfs_winfsp_test_name_matches_pattern(NULL, L"anything.bin"));
    CHECK(memfs_winfsp_test_name_matches_pattern(L"*", L"anything.bin"));
    CHECK(memfs_winfsp_test_name_matches_pattern(L"*.TXT", L"readme.txt"));
    CHECK(memfs_winfsp_test_name_matches_pattern(L"file?.dat", L"FILE1.DAT"));
    CHECK(memfs_winfsp_test_name_matches_pattern(L"foo*", L"Foobar"));
    CHECK(!memfs_winfsp_test_name_matches_pattern(L"*.txt", L"image.bin"));
    CHECK(!memfs_winfsp_test_name_matches_pattern(L"file?.dat", L"file12.dat"));

    CloseHandle(event);
    test_security_snapshot_guards();

    if (g_failures != 0) {
        fprintf(stderr, "memfs_winfsp_state_test: %d failure(s)\n", g_failures);
        return 1;
    }

    printf("memfs_winfsp_state_test: OK\n");
    return 0;
}
