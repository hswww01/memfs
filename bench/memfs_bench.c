#include "memfs_core.h"

#include <Windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>

#define BENCH_SMALL_FILE_COUNT 100000U
#define BENCH_4KB_FILE_COUNT 10000U
#define BENCH_1MB_FILE_COUNT 100U
#define BENCH_RANDOM_REWRITE_COUNT 1000U
#define BENCH_1MB_SIZE (1ULL * 1024ULL * 1024ULL)
#define BENCH_4KB_SIZE 4096U
#define BENCH_1B_SIZE 1U

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

static void print_fs_state(Memfs* fs, const char* label) {
	printf("%-12s used_bytes=%llu resident_bytes=%llu\n", label, (unsigned long long)fs->used_bytes,
		   (unsigned long long)fs->resident_bytes);
}

static int fail(Memfs* fs, const char* message, uint32_t index) {
	fprintf(stderr, "%s at %u\n", message, index);
	memfs_destroy(fs);
	return 1;
}
static int bench_small_files(Memfs* fs, uint32_t count, LARGE_INTEGER frequency) {
	uint8_t one_byte = 0x5a;
	LARGE_INTEGER start;
	LARGE_INTEGER end;
	uint32_t i;
	wchar_t name[32];

	printf("\n[small 1B create/delete]\n");
	printf("files:            %u\n", count);

	QueryPerformanceCounter(&start);
	for (i = 0; i < count; i++) {
		MemfsNode* node = NULL;
		swprintf_s(name, _countof(name), L"small%07u", i);
		if (memfs_node_create(fs, fs->root, name, false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &node) != MEMFS_OK) {
			return fail(fs, "small create failed", i);
		}
		if (memfs_node_write(node, &one_byte, 0, BENCH_1B_SIZE, false, false, &(uint32_t){0}) != MEMFS_OK) {
			memfs_node_close(node);
			return fail(fs, "small write failed", i);
		}
		memfs_node_close(node);
	}
	QueryPerformanceCounter(&end);
	print_rate("create+1B", count, seconds_between(start, end, frequency));
	print_fs_state(fs, "after-create");

	QueryPerformanceCounter(&start);
	for (i = 0; i < count; i++) {
		MemfsNode* node;
		swprintf_s(name, _countof(name), L"small%07u", i);
		node = memfs_dir_lookup(fs->root, name);
		if (node == NULL || memfs_node_unlink(node) != MEMFS_OK) {
			return fail(fs, "small delete failed", i);
		}
	}
	QueryPerformanceCounter(&end);
	print_rate("delete", count, seconds_between(start, end, frequency));
	print_fs_state(fs, "after-delete");

	return 0;
}

static int bench_4kb_files(Memfs* fs, uint32_t count, LARGE_INTEGER frequency) {
	uint8_t payload[BENCH_4KB_SIZE];
	LARGE_INTEGER start;
	LARGE_INTEGER end;
	uint32_t i;
	wchar_t name[32];

	memset(payload, 0x5a, sizeof(payload));

	printf("\n[4KB write]\n");
	printf("files:            %u\n", count);
	printf("bytes/file:       %u\n", BENCH_4KB_SIZE);

	QueryPerformanceCounter(&start);
	for (i = 0; i < count; i++) {
		MemfsNode* node = NULL;
		uint32_t written = 0;
		swprintf_s(name, _countof(name), L"kb4%07u", i);
		if (memfs_node_create(fs, fs->root, name, false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &node) != MEMFS_OK) {
			return fail(fs, "4KB create failed", i);
		}
		if (memfs_node_write(node, payload, 0, BENCH_4KB_SIZE, false, false, &written) != MEMFS_OK ||
			written != BENCH_4KB_SIZE) {
			memfs_node_close(node);
			return fail(fs, "4KB write failed", i);
		}
		memfs_node_close(node);
	}
	QueryPerformanceCounter(&end);
	print_rate("create+4KB", count, seconds_between(start, end, frequency));
	print_fs_state(fs, "after-create");

	QueryPerformanceCounter(&start);
	for (i = 0; i < count; i++) {
		MemfsNode* node;
		swprintf_s(name, _countof(name), L"kb4%07u", i);
		node = memfs_dir_lookup(fs->root, name);
		if (node == NULL || memfs_node_unlink(node) != MEMFS_OK) {
			return fail(fs, "4KB delete failed", i);
		}
	}
	QueryPerformanceCounter(&end);
	print_rate("delete", count, seconds_between(start, end, frequency));
	print_fs_state(fs, "after-delete");

	return 0;
}
static int bench_1mb_sparse_write(Memfs* fs, uint32_t count, LARGE_INTEGER frequency) {
	uint8_t page[BENCH_4KB_SIZE];
	LARGE_INTEGER start;
	LARGE_INTEGER end;
	uint32_t i;
	wchar_t name[32];

	memset(page, 0x5a, sizeof(page));

	printf("\n[1MB sparse write]\n");
	printf("files:            %u\n", count);
	printf("bytes/file:       %llu\n", (unsigned long long)BENCH_1MB_SIZE);
	printf("write pattern:    4KB at offset 0 and 1MB-4KB\n");

	QueryPerformanceCounter(&start);
	for (i = 0; i < count; i++) {
		MemfsNode* node = NULL;
		uint32_t written = 0;
		swprintf_s(name, _countof(name), L"mb1%07u", i);
		if (memfs_node_create(fs, fs->root, name, false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &node) != MEMFS_OK) {
			return fail(fs, "1MB create failed", i);
		}
		if (memfs_node_write(node, page, 0, BENCH_4KB_SIZE, false, false, &written) != MEMFS_OK ||
			written != BENCH_4KB_SIZE) {
			memfs_node_close(node);
			return fail(fs, "1MB first write failed", i);
		}
		if (memfs_node_write(node, page, BENCH_1MB_SIZE - BENCH_4KB_SIZE, BENCH_4KB_SIZE, true, false, &written) !=
			MEMFS_OK ||
			written != BENCH_4KB_SIZE) {
			memfs_node_close(node);
			return fail(fs, "1MB tail write failed", i);
		}
		memfs_node_close(node);
	}
	QueryPerformanceCounter(&end);
	print_rate("create+sparse", count, seconds_between(start, end, frequency));
	print_fs_state(fs, "after-create");

	QueryPerformanceCounter(&start);
	for (i = 0; i < count; i++) {
		MemfsNode* node;
		swprintf_s(name, _countof(name), L"mb1%07u", i);
		node = memfs_dir_lookup(fs->root, name);
		if (node == NULL || memfs_node_unlink(node) != MEMFS_OK) {
			return fail(fs, "1MB delete failed", i);
		}
	}
	QueryPerformanceCounter(&end);
	print_rate("delete", count, seconds_between(start, end, frequency));
	print_fs_state(fs, "after-delete");

	return 0;
}
static int bench_random_rewrite(Memfs* fs, uint32_t count, LARGE_INTEGER frequency) {
	uint8_t page[BENCH_4KB_SIZE];
	LARGE_INTEGER start;
	LARGE_INTEGER end;
	uint32_t i;
	wchar_t name[32];

	memset(page, 0x5a, sizeof(page));

	printf("\n[random rewrite]\n");
	printf("files:            %u\n", count);
	printf("rewrite size:     %u B\n", BENCH_4KB_SIZE);
	printf("file size:        %llu\n", (unsigned long long)BENCH_1MB_SIZE);

	QueryPerformanceCounter(&start);
	for (i = 0; i < count; i++) {
		MemfsNode* node = NULL;
		uint32_t written = 0;
		uint64_t offset;
		swprintf_s(name, _countof(name), L"rw%07u", i);
		if (memfs_node_create(fs, fs->root, name, false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &node) != MEMFS_OK) {
			return fail(fs, "rewrite create failed", i);
		}
		if (memfs_node_write(node, page, 0, BENCH_4KB_SIZE, false, false, &written) != MEMFS_OK ||
			written != BENCH_4KB_SIZE) {
			memfs_node_close(node);
			return fail(fs, "rewrite seed write failed", i);
		}
		if (memfs_node_set_file_size(node, BENCH_1MB_SIZE) != MEMFS_OK) {
			memfs_node_close(node);
			return fail(fs, "rewrite resize failed", i);
		}
		offset = ((uint64_t)i * 2654435761ULL) % (BENCH_1MB_SIZE / BENCH_4KB_SIZE) * BENCH_4KB_SIZE;
		if (memfs_node_write(node, page, offset, BENCH_4KB_SIZE, false, false, &written) != MEMFS_OK ||
			written != BENCH_4KB_SIZE) {
			memfs_node_close(node);
			return fail(fs, "rewrite random write failed", i);
		}
		memfs_node_close(node);
	}
	QueryPerformanceCounter(&end);
	print_rate("create+rewrite", count, seconds_between(start, end, frequency));
	print_fs_state(fs, "after-create");

	QueryPerformanceCounter(&start);
	for (i = 0; i < count; i++) {
		MemfsNode* node;
		swprintf_s(name, _countof(name), L"rw%07u", i);
		node = memfs_dir_lookup(fs->root, name);
		if (node == NULL || memfs_node_unlink(node) != MEMFS_OK) {
			return fail(fs, "rewrite delete failed", i);
		}
	}
	QueryPerformanceCounter(&end);
	print_rate("delete", count, seconds_between(start, end, frequency));
	print_fs_state(fs, "after-delete");

	return 0;
}
int main(int argc, char** argv) {
	Memfs* fs = NULL;
	LARGE_INTEGER frequency;
	uint64_t private_before;
	uint64_t private_after;
	MemfsAllocatorStats stats;
	int rc;

	if (!QueryPerformanceFrequency(&frequency)) {
		fprintf(stderr, "QueryPerformanceFrequency failed\n");
		return 2;
	}

	if (memfs_create(4ULL * 1024ULL * 1024ULL * 1024ULL, L"BENCH", &fs) != MEMFS_OK) {
		fprintf(stderr, "memfs_create failed\n");
		return 1;
	}

	printf("MemfsNode:          %zu B\n", sizeof(MemfsNode));
	printf("capacity:           %llu B\n", (unsigned long long)fs->capacity);
	print_fs_state(fs, "initial");

	private_before = private_bytes();

	rc = bench_small_files(fs, BENCH_SMALL_FILE_COUNT, frequency);
	if (rc != 0) {
		return rc;
	}

	rc = bench_4kb_files(fs, BENCH_4KB_FILE_COUNT, frequency);
	if (rc != 0) {
		return rc;
	}

	rc = bench_1mb_sparse_write(fs, BENCH_1MB_FILE_COUNT, frequency);
	if (rc != 0) {
		return rc;
	}

	rc = bench_random_rewrite(fs, BENCH_RANDOM_REWRITE_COUNT, frequency);
	if (rc != 0) {
		return rc;
	}

	private_after = private_bytes();
	memfs_allocator_get_stats(&fs->allocator, &stats);

	printf("\n[summary]\n");
	printf("private delta:      %.2f MiB\n", (double)(private_after - private_before) / (1024.0 * 1024.0));
	printf("allocator reserved: %.2f MiB\n", (double)stats.reserved_bytes / (1024.0 * 1024.0));
	printf("allocator live:     %llu objects / %.2f MiB\n", (unsigned long long)stats.live_objects,
		   (double)stats.live_bytes / (1024.0 * 1024.0));
	printf("allocator slabs:    %u\n", stats.slab_count);
	print_fs_state(fs, "final");

	memfs_destroy(fs);
	return 0;
}
