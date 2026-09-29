#pragma once

#include <Windows.h>

DWORD memfs_winfsp_prepare_runtime(void);
DWORD memfs_winfsp_install_embedded_driver(void);
const wchar_t* memfs_winfsp_runtime_directory(void);
