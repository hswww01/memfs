#include <Windows.h>

#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#include "memfs_driver.h"

static int g_failures;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fwprintf(stderr, L"CHECK failed: %S:%d: %S\n",                   \
                     __FILE__, __LINE__, #expr);                               \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static DWORD write_bytes(const wchar_t* path, const void* data, DWORD size) {
    HANDLE file;
    DWORD written = 0;

    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return GetLastError();

    if (!WriteFile(file, data, size, &written, NULL) || written != size) {
        DWORD error = GetLastError();
        CloseHandle(file);
        return error != ERROR_SUCCESS ? error : ERROR_WRITE_FAULT;
    }

    CloseHandle(file);
    return ERROR_SUCCESS;
}

int wmain(void) {
    wchar_t temp_dir[MAX_PATH];
    wchar_t path[MAX_PATH];
    uint8_t expected[8193];
    BOOL matches = TRUE;
    DWORD error;
    DWORD i;

    DWORD chars = GetTempPathW(_countof(temp_dir), temp_dir);
    CHECK(chars > 0 && chars < _countof(temp_dir));
    if (chars == 0 || chars >= _countof(temp_dir))
        return 1;

    CHECK(_snwprintf_s(path, _countof(path), _TRUNCATE,
                       L"%smemfs-driver-test-%lu.bin",
                       temp_dir, GetCurrentProcessId()) >= 0);
    {
        const wchar_t* primary = L"C:\\Windows\\System32\\drivers\\memfs-winfsp-x64.sys";
        const wchar_t* alternate = L"C:\\Windows\\System32\\drivers\\memfs-winfsp-x64.alt.sys";

        CHECK(memfs_driver_test_select_update_path(primary, primary, alternate) == alternate);
        CHECK(memfs_driver_test_select_update_path(alternate, primary, alternate) == primary);
        CHECK(memfs_driver_test_select_update_path(L"C:\\legacy\\driver.sys", primary, alternate) == primary);
        CHECK(memfs_driver_test_select_update_path(
                  L"C:\\Program Files (x86)\\WinFsp\\SxS\\sxs.test\\bin\\winfsp-x64.sys",
                  primary, alternate) == primary);
    }

    DeleteFileW(path);

    for (i = 0; i < (DWORD)sizeof(expected); i++)
        expected[i] = (uint8_t)((i * 37U + 11U) & 0xffU);

    matches = TRUE;
    error = memfs_driver_test_file_matches_buffer(
        path, expected, (DWORD)sizeof(expected), &matches);
    CHECK(error == ERROR_SUCCESS);
    CHECK(matches == FALSE);

    CHECK(write_bytes(path, expected, (DWORD)sizeof(expected)) == ERROR_SUCCESS);

    matches = FALSE;
    error = memfs_driver_test_file_matches_buffer(
        path, expected, (DWORD)sizeof(expected), &matches);
    CHECK(error == ERROR_SUCCESS);
    CHECK(matches == TRUE);

    expected[4097] ^= 0x5aU;
    matches = TRUE;
    error = memfs_driver_test_file_matches_buffer(
        path, expected, (DWORD)sizeof(expected), &matches);
    CHECK(error == ERROR_SUCCESS);
    CHECK(matches == FALSE);
    expected[4097] ^= 0x5aU;

    matches = TRUE;
    error = memfs_driver_test_file_matches_buffer(
        path, expected, (DWORD)sizeof(expected) - 1U, &matches);
    CHECK(error == ERROR_SUCCESS);
    CHECK(matches == FALSE);

    DeleteFileW(path);

    if (g_failures != 0) {
        fprintf(stderr, "memfs_driver_test: %d failure(s)\n", g_failures);
        return 1;
    }

    printf("memfs_driver_test: OK\n");
    return 0;
}
