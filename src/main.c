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
			 L"  --mount <path>          Drive letter or directory mount point.\n"
			 L"  --size <bytes>          Capacity; supports K/M/G suffix. Default: 512M.\n"
			 L"  --label <name>          Volume label. Default: MEMFS.\n"
			 L"  --threads <n>           WinFsp dispatcher threads. 0 = automatic.\n"
			 L"  --compress              Enable per-page Zstd compression (level 1).\n"
			 L"  --compression-level <n> Enable compression with level 1..22.\n"
			 L"  --encrypt               Enable XChaCha20-Poly1305 with random session key.\n"
			 L"  --key-hex <64hex>       Enable encryption with a fixed 256-bit key.\n"
			 L"  --key-env <name>        Read the 64-hex encryption key from an env variable.\n"
			 L"  --debug                 Enable all WinFsp debug logging.\n"
			 L"  --help                  Show this help.\n",
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

	if (text == NULL || wcslen(text) != MEMFS_ENCRYPTION_KEY_SIZE * 2U) {
		return false;
	}

	for (i = 0; i < MEMFS_ENCRYPTION_KEY_SIZE; i++) {
		int hi = hex_value(text[i * 2U]);
		int lo = hex_value(text[i * 2U + 1U]);

		if (hi < 0 || lo < 0)
			return false;

		key[i] = (uint8_t)((hi << 4) | lo);
	}

	return true;
}

int wmain(int argc, wchar_t** argv) {
	const wchar_t* mount_point = NULL;
	const wchar_t* volume_label = L"MEMFS";
	uint64_t capacity = 512ULL * 1024ULL * 1024ULL;
	uint32_t thread_count = 0;
	uint32_t compression_level = 1;
	bool compression_enabled = false;
	bool encryption_enabled = false;
	bool have_fixed_key = false;
	bool debug = false;
	uint8_t encryption_key[MEMFS_ENCRYPTION_KEY_SIZE] = {0};
	MemfsOptions options;
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
		} else if (_wcsicmp(argv[i], L"--compress") == 0) {
			compression_enabled = true;
		} else if (_wcsicmp(argv[i], L"--compression-level") == 0 && i + 1 < argc) {
			if (!parse_u32(argv[++i], &compression_level) || compression_level < 1 || compression_level > 22) {
				fwprintf(stderr, L"Invalid compression level; use 1..22.\n");
				return 2;
			}
			compression_enabled = true;
		} else if (_wcsicmp(argv[i], L"--encrypt") == 0) {
			encryption_enabled = true;
		} else if (_wcsicmp(argv[i], L"--key-hex") == 0 && i + 1 < argc) {
			if (!parse_key_hex(argv[++i], encryption_key)) {
				fwprintf(stderr, L"Invalid --key-hex; expected exactly 64 hex characters.\n");
				return 2;
			}
			encryption_enabled = true;
			have_fixed_key = true;
		} else if (_wcsicmp(argv[i], L"--key-env") == 0 && i + 1 < argc) {
			const wchar_t* value = _wgetenv(argv[++i]);

			if (!parse_key_hex(value, encryption_key)) {
				fwprintf(stderr, L"Invalid or missing encryption-key environment variable.\n");
				return 2;
			}
			encryption_enabled = true;
			have_fixed_key = true;
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

	memset(&options, 0, sizeof(options));
	options.capacity = capacity;
	options.volume_label = volume_label;
	options.compression_enabled = compression_enabled;
	options.compression_level = (int)compression_level;
	options.encryption_enabled = encryption_enabled;

	if (have_fixed_key) {
		options.encryption_key = encryption_key;
		options.encryption_key_size = sizeof(encryption_key);
	}

	status = memfs_winfsp_create(&options, &instance);
	SecureZeroMemory(encryption_key, sizeof(encryption_key));

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

	wprintf(L"MEMFS mounted at %s (%llu MiB)%s%s. Press Ctrl+C to stop.\n", mount_point,
			(unsigned long long)(capacity / (1024ULL * 1024ULL)), compression_enabled ? L", compressed" : L"",
			encryption_enabled ? L", encrypted" : L"");

	WaitForSingleObject(g_stop_event, INFINITE);
	exit_code = 0;

	SetConsoleCtrlHandler(console_handler, FALSE);
	CloseHandle(g_stop_event);
	g_stop_event = NULL;

stop:
	memfs_winfsp_stop(instance);

exit:
	memfs_winfsp_destroy(instance);
	SecureZeroMemory(encryption_key, sizeof(encryption_key));
	return exit_code;
}
