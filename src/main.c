#include <Windows.h>
#include <winfsp/winfsp.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

#include "memfs_winfsp.h"

static HANDLE g_stop_event;

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
			 L"\n"
			 L"Options:\n"
			 L"  --mount <path>     Drive letter or directory mount point.\n"
			 L"  --size <bytes>     Capacity; supports K/M/G suffix. Default: 512M.\n"
			 L"  --label <name>     Volume label. Default: MEMFS.\n"
			 L"  --threads <n>      WinFsp dispatcher threads. 0 = automatic.\n"
			 L"  --debug            Enable all WinFsp debug logging.\n"
			 L"  --help             Show this help.\n",
			 exe);
}

static bool parse_size(const wchar_t* text, uint64_t* out) {
	wchar_t* end;
	unsigned long long value;
	uint64_t multiplier = 1;

	if (text == NULL || *text == L'\0' || out == NULL)
		return false;

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

int wmain(int argc, wchar_t** argv) {
	const wchar_t* mount_point = NULL;
	const wchar_t* volume_label = L"MEMFS";
	uint64_t capacity = 512ULL * 1024ULL * 1024ULL;
	uint32_t thread_count = 0;
	bool debug = false;
	MemfsWinFsp* instance = NULL;
	NTSTATUS status;
	int i;
	int exit_code = 1;

	for (i = 1; i < argc; i++) {
		if (_wcsicmp(argv[i], L"--mount") == 0 && i + 1 < argc) {
			mount_point = argv[++i];
		} else if (_wcsicmp(argv[i], L"--size") == 0 && i + 1 < argc) {
			if (!parse_size(argv[++i], &capacity)) {
				fwprintf(stderr, L"Invalid --size value.\n");
				return 2;
			}
		} else if (_wcsicmp(argv[i], L"--label") == 0 && i + 1 < argc) {
			volume_label = argv[++i];
		} else if (_wcsicmp(argv[i], L"--threads") == 0 && i + 1 < argc) {
			if (!parse_u32(argv[++i], &thread_count)) {
				fwprintf(stderr, L"Invalid --threads value.\n");
				return 2;
			}
		} else if (_wcsicmp(argv[i], L"--debug") == 0) {
			debug = true;
		} else if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0 ||
				   _wcsicmp(argv[i], L"/?") == 0) {
			print_usage(argv[0]);
			return 0;
		} else {
			fwprintf(stderr, L"Unknown argument: %s\n", argv[i]);
			print_usage(argv[0]);
			return 2;
		}
	}

	if (mount_point == NULL) {
		print_usage(argv[0]);
		return 2;
	}

	status = memfs_winfsp_create(capacity, volume_label, &instance);
	if (!NT_SUCCESS(status)) {
		fwprintf(stderr, L"memfs_winfsp_create failed: 0x%08X\n", (unsigned)status);
		goto exit;
	}

	if (debug)
		FspFileSystemSetDebugLog(instance->file_system, (ULONG)-1);

	status = memfs_winfsp_mount(instance, mount_point);
	if (!NT_SUCCESS(status)) {
		fwprintf(stderr, L"Cannot mount %s: 0x%08X\n", mount_point, (unsigned)status);
		goto exit;
	}

	status = memfs_winfsp_start(instance, thread_count);
	if (!NT_SUCCESS(status)) {
		fwprintf(stderr, L"Cannot start WinFsp dispatcher: 0x%08X\n", (unsigned)status);
		goto exit;
	}

	g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (g_stop_event == NULL) {
		fwprintf(stderr, L"CreateEvent failed: %lu\n", GetLastError());
		goto stop;
	}

	SetConsoleCtrlHandler(console_handler, TRUE);

	wprintf(L"MEMFS mounted at %s (%llu MiB). Press Ctrl+C to stop.\n", mount_point,
			(unsigned long long)(capacity / (1024ULL * 1024ULL)));

	WaitForSingleObject(g_stop_event, INFINITE);
	exit_code = 0;

	SetConsoleCtrlHandler(console_handler, FALSE);
	CloseHandle(g_stop_event);
	g_stop_event = NULL;

stop:
	memfs_winfsp_stop(instance);

exit:
	memfs_winfsp_destroy(instance);
	return exit_code;
}
