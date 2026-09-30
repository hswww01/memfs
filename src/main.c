#include <Windows.h>
#include <winfsp/winfsp.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "memfs_winfsp.h"
#include "memfs_driver.h"

#define MEMFS_SERVICE_NAME L"MemfsC"

typedef struct MemfsRunConfig {
    const wchar_t* mount_point;
    const wchar_t* volume_label;
    const wchar_t* stop_event_name;
    uint64_t capacity;
    bool capacity_auto;
    uint32_t thread_count;
    uint32_t compression_level;
    bool compression_enabled;
    bool encryption_enabled;
    bool have_fixed_key;
    bool debug;
    bool service_mode;
    bool uninstall_private_driver;
    bool stats_enabled;
    bool stats_json;
    uint8_t encryption_key[MEMFS_ENCRYPTION_KEY_SIZE];
} MemfsRunConfig;

static HANDLE g_stop_event;
static SERVICE_STATUS_HANDLE g_service_status_handle;
static SERVICE_STATUS g_service_status;
static MemfsRunConfig g_config;
static volatile LONG g_service_stop_requested;

static void service_report_ex(DWORD state,
                              DWORD win32_exit,
                              DWORD service_specific_exit,
                              DWORD wait_hint) {
    static DWORD checkpoint = 1;

    if (g_service_status_handle == NULL)
        return;

    memset(&g_service_status, 0, sizeof(g_service_status));
    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = state;
    g_service_status.dwWin32ExitCode = win32_exit;
    g_service_status.dwServiceSpecificExitCode =
        win32_exit == ERROR_SERVICE_SPECIFIC_ERROR ? service_specific_exit : 0;
    g_service_status.dwWaitHint = wait_hint;

    if (state == SERVICE_START_PENDING)
        g_service_status.dwControlsAccepted = 0;
    else if (state == SERVICE_RUNNING)
        g_service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    else if (state == SERVICE_STOP_PENDING)
        g_service_status.dwControlsAccepted = 0;

    if (state == SERVICE_RUNNING || state == SERVICE_STOPPED)
        g_service_status.dwCheckPoint = 0;
    else
        g_service_status.dwCheckPoint = checkpoint++;

    SetServiceStatus(g_service_status_handle, &g_service_status);
}

static void service_report(DWORD state, DWORD win32_exit, DWORD wait_hint) {
    service_report_ex(state, win32_exit, 0, wait_hint);
}

static DWORD WINAPI service_handler(DWORD control,
                                    DWORD event_type,
                                    void* event_data,
                                    void* context) {
    (void)event_type;
    (void)event_data;
    (void)context;

    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        service_report(SERVICE_STOP_PENDING, NO_ERROR, 15000);
        InterlockedExchange(&g_service_stop_requested, 1);
        if (g_stop_event)
            SetEvent(g_stop_event);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static BOOL WINAPI console_handler(DWORD control_type) {
    switch (control_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_stop_event)
            SetEvent(g_stop_event);
        return TRUE;
    default:
        return FALSE;
    }
}

static void print_usage(const wchar_t* exe) {
    fwprintf(stderr,
             L"Usage: %s --mount M: [options]\n"
             L"       %s --service --mount M: [options]\n"
             L"\n"
             L"Options:\n"
             L"  --mount <path>          Drive letter or directory mount point.\n"
             L"  --size <bytes|auto>     Capacity; supports K/M/G suffix. Default: auto.\n"
             L"  --label <name>          Volume label. Default: MEMFS.\n"
             L"  --threads <n>           WinFsp dispatcher threads. 0 = automatic.\n"
             L"  --stop-event <name>     Optional named event for graceful console shutdown/automation.\n"
             L"  --compress              Enable per-page Zstd compression (level 1).\n"
             L"  --compression-level <n> Enable compression with level 1..22.\n"
             L"  --encrypt               Enable XChaCha20-Poly1305 with random session key.\n"
             L"  --key-hex <64hex>       Enable encryption with a fixed 256-bit key.\n"
             L"  --key-env <name>        Read the 64-hex encryption key from an env variable.\n"
             L"  --debug                 Enable all WinFsp debug logging.\n"
             L"  --service               Run under the Windows Service Control Manager.\n"             L"  --uninstall-private-driver  Remove only MemfsC private WinFsp driver files/service.\n"
             L"  --stats                 Print human-readable runtime stats at mount/stop.\n"
             L"  --stats-json            Print machine-readable JSON stats at mount/stop.\n"
             L"  --help                  Show this help.\n"
             L"\n"
             L"Service control example (run from an elevated shell):\n"
             L"  sc create MemfsC binPath= \"\\\"C:\\path\\memfs.exe\\\" --service --mount M: --size auto\" start= auto\n"
             L"  sc start MemfsC\n"
             L"  sc stop MemfsC\n"
             L"  sc delete MemfsC\n",
             exe, exe);
}

static bool parse_size(const wchar_t* text, uint64_t* out, bool* auto_size) {
    wchar_t* end;
    unsigned long long value;
    uint64_t multiplier = 1;

    if (text == NULL || *text == L'\0' || out == NULL)
        return false;

    if (auto_size)
        *auto_size = false;

    if (_wcsicmp(text, L"auto") == 0) {
        if (auto_size)
            *auto_size = true;
        *out = 0;
        return true;
    }

    errno = 0;
    value = wcstoull(text, &end, 0);
    if (errno || end == text)
        return false;

    if (*end) {
        if (end[1] != L'\0')
            return false;

        switch (towupper(*end)) {
        case L'K':
            multiplier = 1024ULL;
            break;
        case L'M':
            multiplier = 1024ULL * 1024ULL;
            break;
        case L'G':
            multiplier = 1024ULL * 1024ULL * 1024ULL;
            break;
        default:
            return false;
        }
    }

    if (value > UINT64_MAX / multiplier)
        return false;

    *out = (uint64_t)value * multiplier;
    return *out != 0;
}

static bool parse_u32(const wchar_t* text, uint32_t* out) {
    wchar_t* end;
    unsigned long value;

    if (text == NULL || *text == L'\0' || out == NULL)
        return false;

    errno = 0;
    value = wcstoul(text, &end, 0);
    if (errno || end == text || *end || value > UINT32_MAX)
        return false;

    *out = (uint32_t)value;
    return true;
}

static int hex_value(wchar_t c) {
    if (c >= L'0' && c <= L'9')
        return (int)(c - L'0');
    if (c >= L'a' && c <= L'f')
        return (int)(c - L'a') + 10;
    if (c >= L'A' && c <= L'F')
        return (int)(c - L'A') + 10;
    return -1;
}

static bool parse_key_hex(const wchar_t* text, uint8_t key[MEMFS_ENCRYPTION_KEY_SIZE]) {
    uint32_t i;

    if (text == NULL || wcslen(text) != MEMFS_ENCRYPTION_KEY_SIZE * 2U)
        return false;

    for (i = 0; i < MEMFS_ENCRYPTION_KEY_SIZE; i++) {
        int hi = hex_value(text[i * 2U]);
        int lo = hex_value(text[i * 2U + 1U]);

        if (hi < 0 || lo < 0)
            return false;

        key[i] = (uint8_t)((hi << 4) | lo);
    }

    return true;
}

static int parse_config(int argc, wchar_t** argv, MemfsRunConfig* config) {
    int i;

    memset(config, 0, sizeof(*config));
    config->volume_label = L"MEMFS";
    config->capacity_auto = true;
    config->compression_level = 1;

    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--mount") == 0 && i + 1 < argc) {
            config->mount_point = argv[++i];
        } else if (_wcsicmp(argv[i], L"--size") == 0 && i + 1 < argc) {
            if (!parse_size(argv[++i], &config->capacity, &config->capacity_auto)) {
                fwprintf(stderr, L"Invalid --size value.\n");
                return 2;
            }
        } else if (_wcsicmp(argv[i], L"--auto-size") == 0) {
            config->capacity = 0;
            config->capacity_auto = true;
        } else if (_wcsicmp(argv[i], L"--label") == 0 && i + 1 < argc) {
            config->volume_label = argv[++i];
        } else if (_wcsicmp(argv[i], L"--threads") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &config->thread_count)) {
                fwprintf(stderr, L"Invalid --threads value.\n");
                return 2;
            }
        } else if (_wcsicmp(argv[i], L"--stop-event") == 0 && i + 1 < argc) {
            config->stop_event_name = argv[++i];
        } else if (_wcsicmp(argv[i], L"--compress") == 0) {
            config->compression_enabled = true;
        } else if (_wcsicmp(argv[i], L"--compression-level") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &config->compression_level) ||
                config->compression_level < 1 || config->compression_level > 22) {
                fwprintf(stderr, L"Invalid compression level; use 1..22.\n");
                return 2;
            }
            config->compression_enabled = true;
        } else if (_wcsicmp(argv[i], L"--encrypt") == 0) {
            config->encryption_enabled = true;
        } else if (_wcsicmp(argv[i], L"--key-hex") == 0 && i + 1 < argc) {
            if (!parse_key_hex(argv[++i], config->encryption_key)) {
                fwprintf(stderr, L"Invalid --key-hex; expected exactly 64 hex characters.\n");
                return 2;
            }
            config->encryption_enabled = true;
            config->have_fixed_key = true;
        } else if (_wcsicmp(argv[i], L"--key-env") == 0 && i + 1 < argc) {
            const wchar_t* value = _wgetenv(argv[++i]);

            if (!parse_key_hex(value, config->encryption_key)) {
                fwprintf(stderr, L"Invalid or missing encryption-key environment variable.\n");
                return 2;
            }
            config->encryption_enabled = true;
            config->have_fixed_key = true;
        } else if (_wcsicmp(argv[i], L"--debug") == 0) {
            config->debug = true;
        } else if (_wcsicmp(argv[i], L"--service") == 0) {
            config->service_mode = true;
        } else if (_wcsicmp(argv[i], L"--uninstall-private-driver") == 0) {
            config->uninstall_private_driver = true;
        } else if (_wcsicmp(argv[i], L"--stats") == 0) {
            config->stats_enabled = true;
        } else if (_wcsicmp(argv[i], L"--stats-json") == 0) {
            config->stats_enabled = true;
            config->stats_json = true;
        } else if (_wcsicmp(argv[i], L"--help") == 0 ||
                   _wcsicmp(argv[i], L"-h") == 0 ||
                   _wcsicmp(argv[i], L"/?") == 0) {
            return 1;
        } else {
            fwprintf(stderr, L"Unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (config->service_mode && config->stop_event_name != NULL) {
        fwprintf(stderr, L"--stop-event is only valid in console mode.\n");
        return 2;
    }

    if (!config->uninstall_private_driver && config->mount_point == NULL) {
        fwprintf(stderr, L"--mount is required.\n");
        return 2;
    }

    return 0;
}


static void print_runtime_stats(Memfs* fs, const char* phase, bool json) {
    MemfsRuntimeStats stats;
    HANDLE output;
    DWORD written = 0;
    char line[4096];
    int length;

    if (fs == NULL || phase == NULL)
        return;

    memfs_get_runtime_stats(fs, &stats);

    if (json) {
        length = _snprintf_s(
            line, sizeof(line), _TRUNCATE,
            "{\"type\":\"memfs_stats\",\"phase\":\"%s\","
            "\"capacity_auto\":%s,\"capacity_bytes\":%llu,"
            "\"logical_used_bytes\":%llu,\"free_bytes\":%llu,"
            "\"resident_bytes\":%llu,\"auto_allowance_bytes\":%llu,"
            "\"auto_hard_margin_bytes\":%llu,\"auto_soft_margin_bytes\":%llu,"
            "\"allocator_live_bytes\":%llu,\"allocator_live_objects\":%llu,"
            "\"allocator_reserved_bytes\":%llu,\"allocator_committed_bytes\":%llu,"
            "\"allocator_physical_bytes\":%llu,\"slab_count\":%u,"
            "\"area_count\":%u,\"area_cached_count\":%u,"
            "\"area_cached_bytes\":%llu,\"allocator_scavenge_count\":%llu,"
            "\"allocator_scavenged_bytes\":%llu}\r\n",
            phase,
            stats.capacity_auto ? "true" : "false",
            (unsigned long long)stats.capacity_bytes,
            (unsigned long long)stats.logical_used_bytes,
            (unsigned long long)stats.free_bytes,
            (unsigned long long)stats.resident_bytes,
            (unsigned long long)stats.auto_allowance_bytes,
            (unsigned long long)stats.auto_hard_margin_bytes,
            (unsigned long long)stats.auto_soft_margin_bytes,
            (unsigned long long)stats.allocator_live_bytes,
            (unsigned long long)stats.allocator_live_objects,
            (unsigned long long)stats.allocator_reserved_bytes,
            (unsigned long long)stats.allocator_committed_bytes,
            (unsigned long long)stats.allocator_physical_bytes,
            stats.slab_count,
            stats.area_count,
            stats.area_cached_count,
            (unsigned long long)stats.area_cached_bytes,
            (unsigned long long)stats.allocator_scavenge_count,
            (unsigned long long)stats.allocator_scavenged_bytes);
    } else {
        length = _snprintf_s(
            line, sizeof(line), _TRUNCATE,
            "stats phase=%s capacity_auto=%u capacity_bytes=%llu "
            "logical_used_bytes=%llu free_bytes=%llu resident_bytes=%llu "
            "auto_allowance_bytes=%llu auto_hard_margin_bytes=%llu "
            "auto_soft_margin_bytes=%llu allocator_live_bytes=%llu "
            "allocator_live_objects=%llu allocator_reserved_bytes=%llu "
            "allocator_committed_bytes=%llu allocator_physical_bytes=%llu "
            "slab_count=%u area_count=%u area_cached_count=%u "
            "area_cached_bytes=%llu allocator_scavenge_count=%llu "
            "allocator_scavenged_bytes=%llu\r\n",
            phase,
            stats.capacity_auto ? 1U : 0U,
            (unsigned long long)stats.capacity_bytes,
            (unsigned long long)stats.logical_used_bytes,
            (unsigned long long)stats.free_bytes,
            (unsigned long long)stats.resident_bytes,
            (unsigned long long)stats.auto_allowance_bytes,
            (unsigned long long)stats.auto_hard_margin_bytes,
            (unsigned long long)stats.auto_soft_margin_bytes,
            (unsigned long long)stats.allocator_live_bytes,
            (unsigned long long)stats.allocator_live_objects,
            (unsigned long long)stats.allocator_reserved_bytes,
            (unsigned long long)stats.allocator_committed_bytes,
            (unsigned long long)stats.allocator_physical_bytes,
            stats.slab_count,
            stats.area_count,
            stats.area_cached_count,
            (unsigned long long)stats.area_cached_bytes,
            (unsigned long long)stats.allocator_scavenge_count,
            (unsigned long long)stats.allocator_scavenged_bytes);
    }

    if (length <= 0)
        return;

    output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == NULL || output == INVALID_HANDLE_VALUE)
        return;

    (void)WriteFile(output, line, (DWORD)length, &written, NULL);
}

static int run_filesystem(const MemfsRunConfig* config,
                          HANDLE stop_event,
                          bool console_mode,
                          DWORD* detail_error) {
    MemfsOptions options;
    MemfsWinFsp* instance = NULL;
    NTSTATUS status;
    DWORD create_detail = ERROR_SUCCESS;
    int exit_code = 1;

    if (detail_error)
        *detail_error = ERROR_SUCCESS;

    memset(&options, 0, sizeof(options));
    options.capacity = config->capacity;
    options.capacity_auto = config->capacity_auto;
    options.volume_label = config->volume_label;
    options.compression_enabled = config->compression_enabled;
    options.compression_level = (int)config->compression_level;
    options.encryption_enabled = config->encryption_enabled;

    if (config->have_fixed_key) {
        options.encryption_key = config->encryption_key;
        options.encryption_key_size = sizeof(config->encryption_key);
    }

    status = memfs_winfsp_create_ex(&options, &instance, &create_detail);
    if (!NT_SUCCESS(status)) {
        if (detail_error)
            *detail_error = create_detail != ERROR_SUCCESS
                                ? create_detail
                                : (DWORD)status;
        if (console_mode) {
            if (create_detail == ERROR_SUCCESS_REBOOT_REQUIRED) {
                fwprintf(stderr,
                         L"WinFsp private driver upgrade is staged but the "
                         L"currently loaded driver cannot be replaced safely; "
                         L"reboot is required.\n");
            } else if (create_detail != ERROR_SUCCESS) {
                fwprintf(stderr,
                         L"memfs_winfsp_create failed: 0x%08X "
                         L"(driver/runtime Win32 error=%lu)\n",
                         (unsigned)status, create_detail);
            } else {
                fwprintf(stderr,
                         L"memfs_winfsp_create failed: 0x%08X\n",
                         (unsigned)status);
            }
        }
        return 3;
    }

    if (config->debug)
        FspFileSystemSetDebugLog(instance->file_system, (ULONG)-1);

    status = memfs_winfsp_mount(instance, config->mount_point);
    if (!NT_SUCCESS(status)) {
        if (detail_error)
            *detail_error = (DWORD)status;
        if (console_mode)
            fwprintf(stderr, L"Cannot mount %s: 0x%08X\n",
                     config->mount_point, (unsigned)status);
        exit_code = 4;
        goto exit;
    }

    status = memfs_winfsp_start(instance, config->thread_count);
    if (!NT_SUCCESS(status)) {
        if (detail_error)
            *detail_error = (DWORD)status;
        if (console_mode)
            fwprintf(stderr, L"Cannot start WinFsp dispatcher: 0x%08X\n", (unsigned)status);
        exit_code = 5;
        goto exit;
    }

    if (!console_mode)
        service_report(SERVICE_RUNNING, NO_ERROR, 0);

    if (console_mode) {
        if (!config->stats_json) {
            if (config->capacity_auto)
                wprintf(L"MEMFS mounted at %s (auto capacity)%s%s. Press Ctrl+C to stop.\n",
                        config->mount_point,
                        config->compression_enabled ? L", compressed" : L"",
                        config->encryption_enabled ? L", encrypted" : L"");
            else
                wprintf(L"MEMFS mounted at %s (%llu MiB)%s%s. Press Ctrl+C to stop.\n",
                        config->mount_point,
                        (unsigned long long)(config->capacity / (1024ULL * 1024ULL)),
                        config->compression_enabled ? L", compressed" : L"",
                        config->encryption_enabled ? L", encrypted" : L"");
        }
        if (config->stats_enabled)
            print_runtime_stats(instance->store, "mounted", config->stats_json);
    }

    {
        HANDLE wait_handles[2];
        DWORD wait_result;

        wait_handles[0] = stop_event;
        wait_handles[1] = memfs_winfsp_dispatcher_stopped_event(instance);
        wait_result = WaitForMultipleObjects(
            (DWORD)_countof(wait_handles), wait_handles, FALSE, INFINITE);

        if (wait_result == WAIT_OBJECT_0) {
            if (console_mode && config->stats_enabled)
                print_runtime_stats(instance->store, "stopping", config->stats_json);
            exit_code = 0;
            memfs_winfsp_stop(instance);
        } else if (wait_result == WAIT_OBJECT_0 + 1U) {
            NTSTATUS dispatcher_status =
                memfs_winfsp_dispatcher_result(instance);
            bool normal =
                memfs_winfsp_dispatcher_stopped_normally(instance);

            /*
             * A normal callback should only follow our own StopDispatcher,
             * which is issued after the stop event wins the wait above. If it
             * arrives here without a stop request, treat it as an unexpected
             * lifetime break rather than silently leaving a "running" service.
             */
            if (normal &&
                WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
                exit_code = 0;
            } else {
                if (NT_SUCCESS(dispatcher_status))
                    dispatcher_status = STATUS_DEVICE_NOT_CONNECTED;
                if (detail_error)
                    *detail_error = (DWORD)dispatcher_status;
                if (console_mode) {
                    fwprintf(stderr,
                             L"WinFsp dispatcher stopped unexpectedly: "
                             L"0x%08X\n",
                             (unsigned)dispatcher_status);
                }
                exit_code = 7;
            }
        } else {
            DWORD wait_error =
                wait_result == WAIT_FAILED ? GetLastError()
                                           : ERROR_GEN_FAILURE;
            if (detail_error)
                *detail_error = wait_error;
            if (console_mode)
                fwprintf(stderr,
                         L"Dispatcher/stop wait failed: %lu\n",
                         wait_error);
            exit_code = 8;
        }
    }

exit:
    memfs_winfsp_destroy(instance);
    return exit_code;
}

static VOID WINAPI service_main(DWORD argc, LPWSTR* argv) {
    int result;
    DWORD detail_error = ERROR_SUCCESS;

    (void)argc;
    (void)argv;

    g_service_status_handle =
        RegisterServiceCtrlHandlerExW(MEMFS_SERVICE_NAME, service_handler, NULL);
    if (g_service_status_handle == NULL)
        return;

    service_report(SERVICE_START_PENDING, NO_ERROR, 15000);

    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_stop_event == NULL) {
        service_report(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }
    /*
     * The SCM may deliver STOP immediately after handler registration and
     * before this event exists. Persist that request in the handler and replay
     * it here so startup cannot lose a stop/shutdown control.
     */
    if (InterlockedCompareExchange(&g_service_stop_requested, 0, 0) != 0)
        SetEvent(g_stop_event);

    result = run_filesystem(&g_config, g_stop_event, false, &detail_error);

    CloseHandle(g_stop_event);
    g_stop_event = NULL;

    if (result == 0) {
        service_report(SERVICE_STOPPED, NO_ERROR, 0);
    } else {
        service_report_ex(
            SERVICE_STOPPED,
            ERROR_SERVICE_SPECIFIC_ERROR,
            detail_error != ERROR_SUCCESS ? detail_error : (DWORD)result,
            0);
    }
}

int wmain(int argc, wchar_t** argv) {
    int parse_result;

    parse_result = parse_config(argc, argv, &g_config);
    if (parse_result == 1) {
        print_usage(argv[0]);
        return 0;
    }
    if (parse_result != 0) {
        print_usage(argv[0]);
        SecureZeroMemory(g_config.encryption_key, sizeof(g_config.encryption_key));
        return parse_result;
    }

    if (g_config.uninstall_private_driver) {
        DWORD error = memfs_winfsp_uninstall_embedded_driver();

        if (error == ERROR_SUCCESS) {
            wprintf(L"MemfsC private WinFsp driver is removed.\n");
            return 0;
        }
        if (error == ERROR_SUCCESS_REBOOT_REQUIRED) {
            wprintf(L"MemfsC private WinFsp driver removal is scheduled; reboot required.\n");
            return 0;
        }

        fwprintf(stderr, L"Cannot remove MemfsC private WinFsp driver: %lu\n", error);
        return 6;
    }

    if (g_config.service_mode) {
        SERVICE_TABLE_ENTRYW service_table[] = {
            {(LPWSTR)MEMFS_SERVICE_NAME, service_main},
            {NULL, NULL},
        };

        if (!StartServiceCtrlDispatcherW(service_table)) {
            DWORD error = GetLastError();
            if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
                fwprintf(stderr,
                         L"--service must be launched by SCM. Use 'sc create/start MemfsC'.\n");
            } else {
                fwprintf(stderr, L"StartServiceCtrlDispatcher failed: %lu\n", error);
            }
            SecureZeroMemory(g_config.encryption_key, sizeof(g_config.encryption_key));
            return 4;
        }

        SecureZeroMemory(g_config.encryption_key, sizeof(g_config.encryption_key));
        return 0;
    }

    g_stop_event = CreateEventW(NULL, TRUE, FALSE, g_config.stop_event_name);
    if (g_stop_event == NULL) {
        fwprintf(stderr, L"CreateEvent failed: %lu\n", GetLastError());
        SecureZeroMemory(g_config.encryption_key, sizeof(g_config.encryption_key));
        return 1;
    }

    SetConsoleCtrlHandler(console_handler, TRUE);
    parse_result = run_filesystem(&g_config, g_stop_event, true, NULL);
    SetConsoleCtrlHandler(console_handler, FALSE);

    CloseHandle(g_stop_event);
    g_stop_event = NULL;
    SecureZeroMemory(g_config.encryption_key, sizeof(g_config.encryption_key));
    return parse_result;
}
