#pragma once

#include <Windows.h>

DWORD memfs_winfsp_prepare_runtime(void);
DWORD memfs_winfsp_install_embedded_driver(void);
DWORD memfs_winfsp_uninstall_embedded_driver(void);
const wchar_t* memfs_winfsp_runtime_directory(void);

#if defined(MEMFS_DRIVER_TESTING)
DWORD memfs_driver_test_file_matches_buffer(const wchar_t* path,
                                            const void* expected,
                                            DWORD expected_size,
                                            BOOL* matches);
const wchar_t* memfs_driver_test_select_update_path(
    const wchar_t* running_path,
    const wchar_t* primary_path,
    const wchar_t* alternate_path);
DWORD memfs_driver_test_spec_for_machine(
    USHORT native_machine,
    WORD* resource_id,
    const wchar_t** primary_file,
    const wchar_t** alternate_file);
DWORD memfs_driver_test_native_machine(USHORT* native_machine);
#endif
