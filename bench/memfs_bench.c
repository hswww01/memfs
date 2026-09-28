#include "memfs_core.h"

#include <Windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>

static double seconds_between(LARGE_INTEGER start, LARGE_INTEGER end, LARGE_INTEGER frequency) {
	return (double)(end.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
}

static uint64_t private_bytes(void) {
	PROCESS_MEMORY_COUNTERS_EX counters;

	ZeroMemory(&counters, sizeof(counters));
	if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&counters, sizeof(counters))) {
		return 0;
	}

	return (uint64_t)counters.PrivateUsage;
}

static void print_rate(const char* label, uint32_t count, double seconds) {
	double rate = seconds > 0.0 ? (double)count / seconds : 0.0;
	printf("%-12s %10.3f s  %12.0f ops/s\n", label, seconds, rate);
}

int main(int argc, char** argv) {
	uint32_t count = 1000000U;
	uint32_t payload_size = 0;
	uint8_t payload[MEMFS_SMALL_LIMIT];
	Memfs* fs = NULL;
	MemfsNode* node = NULL;
	MemfsAllocatorStats stats;
	LARGE_INTEGER frequency;
	LARGE_INTEGER start;
	LARGE_INTEGER end;
	uint64_t private_before;
	uint64_t private_after_create;
	uint64_t private_after_delete;
	uint32_t i;
	wchar_t name[32];

	if (argc > 1) {
		unsigned long requested = strtoul(argv[1], NULL, 10);
		if (requested == 0 || requested > UINT32_MAX) {
			fprintf(stderr, "invalid file count: %s\n", argv[1]);
			return 2;
		}
		count = (uint32_t)requested;
	}

	if (argc > 2) {
		unsigned long requested = strtoul(argv[2], NULL, 10);
		if (requested > MEMFS_SMALL_LIMIT) {
			fprintf(stderr, "invalid payload size: %s\n", argv[2]);
			return 2;
		}
		payload_size = (uint32_t)requested;
	}
	memset(payload, 0x5a, sizeof(payload));

	if (!QueryPerformanceFrequency(&frequency)) {
		fprintf(stderr, "QueryPerformanceFrequency failed\n");
		return 2;
	}

	if (memfs_create(4ULL * 1024ULL * 1024ULL * 1024ULL, L"BENCH", &fs) != MEMFS_OK) {
		fprintf(stderr, "memfs_create failed\n");
		return 1;
	}

	printf("MemfsNode:          %zu B\n", sizeof(MemfsNode));
	printf("payload/file:       %u B\n", payload_size);

	private_before = private_bytes();

	QueryPerformanceCounter(&start);
	for (i = 0; i < count; i++) {
		swprintf_s(name, _countof(name), L"f%07u", i);

		if (memfs_node_create(fs, fs->root, name, false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &node) != MEMFS_OK) {
			fprintf(stderr, "create failed at %u\n", i);
			memfs_destroy(fs);
			return 1;
		}

		if (payload_size != 0) {
			uint32_t written = 0;
			if (memfs_node_write(node, payload, 0, payload_size, false, false, &written) != MEMFS_OK ||
				written != payload_size) {
				fprintf(stderr, "write failed at %u\n", i);
				memfs_destroy(fs);
				return 1;
			}
		}

		memfs_node_close(node);
	}
	QueryPerformanceCounter(&end);
	print_rate("create", count, seconds_between(start, end, frequency));

	private_after_create = private_bytes();
	memfs_allocator_get_stats(&fs->allocator, &stats);

	printf("private delta:      %.2f MiB\n", (double)(private_after_create - private_before) / (1024.0 * 1024.0));
	printf("private/file:       %.2f B\n", count ? (double)(private_after_create - private_before) / count : 0.0);
	printf("allocator reserved: %.2f MiB\n", (double)stats.reserved_bytes / (1024.0 * 1024.0));
	printf("allocator/file:     %.2f B\n", count ? (double)stats.reserved_bytes / count : 0.0);
	printf("allocator live:     %llu objects / %.2f MiB\n", (unsigned long long)stats.live_objects,
		   (double)stats.live_bytes / (1024.0 * 1024.0));
	printf("allocator slabs:    %u\n", stats.slab_count);

	QueryPerformanceCounter(&start);
	for (i = 0; i < count; i++) {
		swprintf_s(name, _countof(name), L"f%07u", i);
		node = memfs_dir_lookup(fs->root, name);
		if (node == NULL) {
			fprintf(stderr, "lookup failed at %u\n", i);
			memfs_destroy(fs);
			return 1;
		}
	}
	QueryPerformanceCounter(&end);
	print_rate("lookup", count, seconds_between(start, end, frequency));

	QueryPerformanceCounter(&start);
	for (i = 0; i < count; i++) {
		swprintf_s(name, _countof(name), L"f%07u", i);
		node = memfs_dir_lookup(fs->root, name);
		if (node == NULL || memfs_node_unlink(node) != MEMFS_OK) {
			fprintf(stderr, "delete failed at %u\n", i);
			memfs_destroy(fs);
			return 1;
		}
	}
	QueryPerformanceCounter(&end);
	print_rate("delete", count, seconds_between(start, end, frequency));

	private_after_delete = private_bytes();
	memfs_allocator_get_stats(&fs->allocator, &stats);

	printf("post-delete private: %.2f MiB\n", (double)private_after_delete / (1024.0 * 1024.0));
	printf("post-delete live:    %llu objects / %.2f MiB\n", (unsigned long long)stats.live_objects,
		   (double)stats.live_bytes / (1024.0 * 1024.0));
	printf("reserved for reuse:  %.2f MiB\n", (double)stats.reserved_bytes / (1024.0 * 1024.0));

	memfs_destroy(fs);
	return 0;
}
