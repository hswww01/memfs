#pragma once

#include <winfsp/winfsp.h>

#include "memfs_core.h"

typedef struct MemfsWinFsp {
	FSP_FILE_SYSTEM* file_system;
	Memfs* store;
} MemfsWinFsp;

NTSTATUS memfs_winfsp_create(const MemfsOptions* options, MemfsWinFsp** out_instance);
NTSTATUS memfs_winfsp_create_ex(
    const MemfsOptions* options,
    MemfsWinFsp** out_instance,
    DWORD* detail_error);

NTSTATUS memfs_winfsp_mount(MemfsWinFsp* instance, const wchar_t* mount_point);
NTSTATUS memfs_winfsp_start(MemfsWinFsp* instance, uint32_t thread_count);
void memfs_winfsp_stop(MemfsWinFsp* instance);
void memfs_winfsp_destroy(MemfsWinFsp* instance);
