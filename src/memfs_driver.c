#include "memfs_driver.h"
#include "memfs_resource.h"

#include <Windows.h>
#include <stdint.h>
#include <wchar.h>

#define MEMFS_WINFSP_DRIVER_SERVICE L"WinFsp+MemfsC"
#define MEMFS_RUNTIME_DIR_NAME L"MemfsC"

#if defined(_M_ARM64)
#define MEMFS_WINFSP_DLL_FILE L"winfsp-a64.dll"
#define MEMFS_WINFSP_SYS_FILE L"memfs-winfsp-a64.sys"
#else
#define MEMFS_WINFSP_DLL_FILE L"winfsp-x64.dll"
#define MEMFS_WINFSP_SYS_FILE L"memfs-winfsp-x64.sys"
#endif

static INIT_ONCE g_runtime_once = INIT_ONCE_STATIC_INIT;
static DWORD g_runtime_error = ERROR_SUCCESS;
static wchar_t g_runtime_dir[MAX_PATH];

static DWORD resource_view(WORD resource_id, const void** data, DWORD* size) {
    HMODULE module = GetModuleHandleW(NULL);
    HRSRC resource;
    HGLOBAL loaded;

    if (data == NULL || size == NULL)
        return ERROR_INVALID_PARAMETER;

    *data = NULL;
    *size = 0;

    resource = FindResourceW(module, MAKEINTRESOURCEW(resource_id), MAKEINTRESOURCEW(10));
    if (resource == NULL)
        return GetLastError();

    *size = SizeofResource(module, resource);
    if (*size == 0)
        return ERROR_RESOURCE_DATA_NOT_FOUND;

    loaded = LoadResource(module, resource);
    if (loaded == NULL)
        return GetLastError();

    *data = LockResource(loaded);
    if (*data == NULL)
        return ERROR_RESOURCE_DATA_NOT_FOUND;

    return ERROR_SUCCESS;
}

static DWORD file_matches_buffer(const wchar_t* path,
                                 const void* expected,
                                 DWORD expected_size,
                                 BOOL* matches) {
    HANDLE file;
    LARGE_INTEGER size;
    const uint8_t* expected_bytes = expected;
    uint8_t buffer[4096];
    DWORD offset = 0;

    if (expected == NULL || matches == NULL)
        return ERROR_INVALID_PARAMETER;
    *matches = FALSE;

    file = CreateFileW(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                   ? ERROR_SUCCESS
                   : error;
    }

    if (!GetFileSizeEx(file, &size)) {
        DWORD error = GetLastError();
        CloseHandle(file);
        return error;
    }
    if (size.QuadPart != (LONGLONG)expected_size) {
        CloseHandle(file);
        return ERROR_SUCCESS;
    }

    while (offset < expected_size) {
        DWORD chunk = expected_size - offset;
        DWORD read = 0;
        if (chunk > sizeof(buffer))
            chunk = sizeof(buffer);
        if (!ReadFile(file, buffer, chunk, &read, NULL) || read != chunk) {
            DWORD error = GetLastError();
            CloseHandle(file);
            return error != ERROR_SUCCESS ? error : ERROR_READ_FAULT;
        }
        if (memcmp(buffer, expected_bytes + offset, chunk) != 0) {
            CloseHandle(file);
            return ERROR_SUCCESS;
        }
        offset += chunk;
    }

    CloseHandle(file);
    *matches = TRUE;
    return ERROR_SUCCESS;
}

#if defined(MEMFS_DRIVER_TESTING)
DWORD memfs_driver_test_file_matches_buffer(const wchar_t* path,
                                            const void* expected,
                                            DWORD expected_size,
                                            BOOL* matches) {
    return file_matches_buffer(path, expected, expected_size, matches);
}
#endif

static DWORD extract_resource(WORD resource_id, const wchar_t* target_path) {
    const void* data;
    DWORD size;
    DWORD error;
    BOOL matches;
    HANDLE file;
    wchar_t temp_path[MAX_PATH];
    DWORD written = 0;

    error = resource_view(resource_id, &data, &size);
    if (error != ERROR_SUCCESS)
        return error;

    error = file_matches_buffer(target_path, data, size, &matches);
    if (error == ERROR_SUCCESS && matches)
        return ERROR_SUCCESS;

    if (_snwprintf_s(temp_path, _countof(temp_path), _TRUNCATE, L"%s.tmp.%lu",
                     target_path, GetCurrentProcessId()) < 0)
        return ERROR_BUFFER_OVERFLOW;

    file = CreateFileW(temp_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return GetLastError();

    if (!WriteFile(file, data, size, &written, NULL) || written != size) {
        error = GetLastError();
        CloseHandle(file);
        DeleteFileW(temp_path);
        return error != ERROR_SUCCESS ? error : ERROR_WRITE_FAULT;
    }

    if (!FlushFileBuffers(file)) {
        error = GetLastError();
        CloseHandle(file);
        DeleteFileW(temp_path);
        return error;
    }

    CloseHandle(file);

    if (!MoveFileExW(temp_path, target_path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = GetLastError();
        DeleteFileW(temp_path);

        /*
         * A loaded DLL/driver can block replacement. If the installed copy
         * already matches the embedded payload byte-for-byte, it is acceptable.
         */
        if (file_matches_buffer(target_path, data, size, &matches) == ERROR_SUCCESS && matches)
            return ERROR_SUCCESS;
        return error;
    }

    return ERROR_SUCCESS;
}

static BOOL CALLBACK prepare_runtime_once(PINIT_ONCE once, PVOID parameter, PVOID* context) {
    (void)once;
    (void)parameter;
    (void)context;

#if defined(MEMFS_WINFSP_STATIC)
    g_runtime_dir[0] = L'\0';
    g_runtime_error = ERROR_SUCCESS;
    return TRUE;
#else
    wchar_t program_data[MAX_PATH];
    wchar_t dll_path[MAX_PATH];
    DWORD chars;

    chars = GetEnvironmentVariableW(L"ProgramData", program_data, _countof(program_data));
    if (chars == 0 || chars >= _countof(program_data)) {
        g_runtime_error = chars == 0 ? GetLastError() : ERROR_BUFFER_OVERFLOW;
        return TRUE;
    }

    if (_snwprintf_s(g_runtime_dir, _countof(g_runtime_dir), _TRUNCATE,
                     L"%s\\%s", program_data, MEMFS_RUNTIME_DIR_NAME) < 0) {
        g_runtime_error = ERROR_BUFFER_OVERFLOW;
        return TRUE;
    }

    if (!CreateDirectoryW(g_runtime_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        g_runtime_error = GetLastError();
        return TRUE;
    }

    if (_snwprintf_s(dll_path, _countof(dll_path), _TRUNCATE,
                     L"%s\\%s", g_runtime_dir, MEMFS_WINFSP_DLL_FILE) < 0) {
        g_runtime_error = ERROR_BUFFER_OVERFLOW;
        return TRUE;
    }

    g_runtime_error = extract_resource(IDR_MEMFS_WINFSP_DLL, dll_path);
    if (g_runtime_error != ERROR_SUCCESS)
        return TRUE;

    if (!SetDllDirectoryW(g_runtime_dir)) {
        g_runtime_error = GetLastError();
        return TRUE;
    }

    g_runtime_error = ERROR_SUCCESS;
    return TRUE;
#endif
}

DWORD memfs_winfsp_prepare_runtime(void) {
    if (!InitOnceExecuteOnce(&g_runtime_once, prepare_runtime_once, NULL, NULL))
        return GetLastError();
    return g_runtime_error;
}

const wchar_t* memfs_winfsp_runtime_directory(void) {
    if (memfs_winfsp_prepare_runtime() != ERROR_SUCCESS)
        return NULL;
    return g_runtime_dir;
}

static DWORD embedded_driver_path(wchar_t path[MAX_PATH]) {
    wchar_t system_dir[MAX_PATH];
    UINT chars = GetSystemDirectoryW(system_dir, _countof(system_dir));

    if (chars == 0)
        return GetLastError();
    if (chars >= _countof(system_dir))
        return ERROR_BUFFER_OVERFLOW;

    if (_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\drivers\\%s",
                     system_dir, MEMFS_WINFSP_SYS_FILE) < 0)
        return ERROR_BUFFER_OVERFLOW;

    return ERROR_SUCCESS;
}

DWORD memfs_winfsp_install_embedded_driver(void) {
    SC_HANDLE manager = NULL;
    SC_HANDLE service = NULL;
    wchar_t driver_path[MAX_PATH];
    DWORD error;
    DWORD start_error;

    error = embedded_driver_path(driver_path);
    if (error != ERROR_SUCCESS)
        return error;

    error = extract_resource(IDR_MEMFS_WINFSP_SYS, driver_path);
    if (error != ERROR_SUCCESS)
        return error;

    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (manager == NULL)
        return GetLastError();

    service = OpenServiceW(manager, MEMFS_WINFSP_DRIVER_SERVICE,
                           SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
    if (service == NULL && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
        service = CreateServiceW(manager,
                                 MEMFS_WINFSP_DRIVER_SERVICE,
                                 MEMFS_WINFSP_DRIVER_SERVICE,
                                 SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG,
                                 SERVICE_FILE_SYSTEM_DRIVER,
                                 SERVICE_DEMAND_START,
                                 SERVICE_ERROR_NORMAL,
                                 driver_path,
                                 NULL, NULL, NULL, NULL, NULL);
    }

    if (service == NULL) {
        error = GetLastError();
        CloseServiceHandle(manager);
        return error;
    }

    /*
     * A previous install may have been disabled. Restore demand-start rather
     * than requiring the user to repair it manually.
     */
    if (!ChangeServiceConfigW(service,
                              SERVICE_FILE_SYSTEM_DRIVER,
                              SERVICE_DEMAND_START,
                              SERVICE_ERROR_NORMAL,
                              driver_path,
                              NULL, NULL, NULL, NULL, NULL, NULL)) {
        error = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return error;
    }

    if (!StartServiceW(service, 0, NULL)) {
        start_error = GetLastError();
        if (start_error != ERROR_SERVICE_ALREADY_RUNNING)
            error = start_error;
        else
            error = ERROR_SUCCESS;
    } else {
        error = ERROR_SUCCESS;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return error;
}
