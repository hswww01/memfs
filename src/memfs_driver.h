#pragma once

#include <Windows.h>

DWORD memfs_winfsp_prepare_runtime(void);
DWORD memfs_winfsp_install_embedded_driver(void);
const wchar_t* memfs_winfsp_runtime_directory(void);

#if defined(MEMFS_DRIVER_TESTING)
DWORD memfs_driver_test_file_matches_buffer(const wchar_t* path,
                                            const void* expected,
                                            DWORD expected_size,
                                            BOOL* matches);
#endif
