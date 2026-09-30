#include <stdio.h>
#include <string.h>

#include "memfs_winfsp.h"

static int g_failures;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "CHECK failed: %s:%d: %s\n",                     \
                    __FILE__, __LINE__, #expr);                                \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

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

    if (g_failures != 0) {
        fprintf(stderr, "memfs_winfsp_state_test: %d failure(s)\n", g_failures);
        return 1;
    }

    printf("memfs_winfsp_state_test: OK\n");
    return 0;
}
