#pragma once

#include <winfsp/winfsp.h>

#include "memfs_core.h"

typedef struct MemfsWinFsp {
	FSP_FILE_SYSTEM* file_system;
	Memfs* store;
	HANDLE dispatcher_stopped_event;
	volatile LONG dispatcher_stop_reason;
} MemfsWinFsp;

enum {
	MEMFS_DISPATCHER_ACTIVE = 0,
	MEMFS_DISPATCHER_STOPPED_NORMALLY = 1,
	MEMFS_DISPATCHER_STOPPED_ABNORMALLY = 2
};

NTSTATUS memfs_winfsp_create(const MemfsOptions* options, MemfsWinFsp** out_instance);
NTSTATUS memfs_winfsp_create_ex(
    const MemfsOptions* options,
    MemfsWinFsp** out_instance,
    DWORD* detail_error);

NTSTATUS memfs_winfsp_mount(MemfsWinFsp* instance, const wchar_t* mount_point);
NTSTATUS memfs_winfsp_start(MemfsWinFsp* instance, uint32_t thread_count);
HANDLE memfs_winfsp_dispatcher_stopped_event(MemfsWinFsp* instance);
bool memfs_winfsp_dispatcher_stopped_normally(const MemfsWinFsp* instance);
NTSTATUS memfs_winfsp_dispatcher_result(const MemfsWinFsp* instance);
#if defined(MEMFS_WINFSP_TESTING)
void memfs_winfsp_test_dispatcher_stopped(
    MemfsWinFsp* instance,
    bool normally);
#endif

void memfs_winfsp_stop(MemfsWinFsp* instance);
void memfs_winfsp_destroy(MemfsWinFsp* instance);
