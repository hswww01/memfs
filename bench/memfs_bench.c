#include "memfs_core.h"

#include <Windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_SMALL_FILE_COUNT 100000U
#define BENCH_4KB_FILE_COUNT 10000U
#define BENCH_1MB_FILE_COUNT 100U
#define BENCH_RANDOM_REWRITE_COUNT 1000U
#define BENCH_1MB_SIZE (1ULL * 1024ULL * 1024ULL)
#define BENCH_4KB_SIZE 4096U
#define BENCH_1B_SIZE 1U

typedef enum BenchMode {
	BENCH_MODE_COMPARE = 0,
	BENCH_MODE_PLAIN,
	BENCH_MODE_COMPRESSION,
	BENCH_MODE_ENCRYPTION,
	BENCH_MODE_COMBINED
} BenchMode;

typedef struct BenchConfig {
	bool compression_enabled;
	bool encryption_enabled;
	int compression_level;
	uint8_t encryption_key[MEMFS_ENCRYPTION_KEY_SIZE];
	bool has_encryption_key;
} BenchConfig;

typedef struct BenchResult {
	const char* label;
	double create_seconds;
	double lookup_seconds;
	double delete_seconds;
	uint64_t used_after_create;
	uint64_t resident_after_create;
	uint64_t used_after_delete;
	uint64_t resident_after_delete;
} BenchResult;

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

static void print_config(const BenchConfig* config) {
	printf("compression:      %s level=%d\n", config->compression_enabled ? "enabled" : "disabled",
		   config->compression_level);
	printf("encryption:       %s\n", config->encryption_enabled ? "enabled" : "disabled");
}

static int create_fs(const BenchConfig* config, Memfs** out_fs) {
	MemfsOptions options;

	memset(&options, 0, sizeof(options));
	options.capacity = 4ULL * 1024ULL * 1024ULL * 1024ULL;
	options.volume_label = L"BENCH";
	options.compression_enabled = config->compression_enabled;
	options.compression_level = config->compression_level;
	options.encryption_enabled = config->encryption_enabled;
	if (config->has_encryption_key) {
		options.encryption_key = config->encryption_key;
		options.encryption_key_size = sizeof(config->encryption_key);
	}

	return memfs_create_ex(&options, out_fs);
}
static int bench_small_files(Memfs* fs, uint32_t count, LARGE_INTEGER frequency, BenchResult* result) {
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
	result->create_seconds = seconds_between(start, end, frequency);
	print_rate("create+1B", count, result->create_seconds);
	result->used_after_create = (uint64_t)fs->used_bytes;
	result->resident_after_create = (uint64_t)fs->resident_bytes;
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
	result->delete_seconds = seconds_between(start, end, frequency);
	print_rate("delete", count, result->delete_seconds);
	result->used_after_delete = (uint64_t)fs->used_bytes;
	result->resident_after_delete = (uint64_t)fs->resident_bytes;
	print_fs_state(fs, "after-delete");

	return 0;
}
static int bench_4kb_files(Memfs* fs, uint32_t count, LARGE_INTEGER frequency, BenchResult* result) {
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
	result->create_seconds = seconds_between(start, end, frequency);
	print_rate("create+4KB", count, result->create_seconds);
	result->used_after_create = (uint64_t)fs->used_bytes;
	result->resident_after_create = (uint64_t)fs->resident_bytes;
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
	result->delete_seconds = seconds_between(start, end, frequency);
	print_rate("delete", count, result->delete_seconds);
	result->used_after_delete = (uint64_t)fs->used_bytes;
	result->resident_after_delete = (uint64_t)fs->resident_bytes;
	print_fs_state(fs, "after-delete");

	return 0;
}
static int bench_1mb_sparse_write(Memfs* fs, uint32_t count, LARGE_INTEGER frequency, BenchResult* result) {
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
	result->create_seconds = seconds_between(start, end, frequency);
	print_rate("create+sparse", count, result->create_seconds);
	result->used_after_create = (uint64_t)fs->used_bytes;
	result->resident_after_create = (uint64_t)fs->resident_bytes;
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
	result->delete_seconds = seconds_between(start, end, frequency);
	print_rate("delete", count, result->delete_seconds);
	result->used_after_delete = (uint64_t)fs->used_bytes;
	result->resident_after_delete = (uint64_t)fs->resident_bytes;
	print_fs_state(fs, "after-delete");

	return 0;
}
static int bench_random_rewrite(Memfs* fs, uint32_t count, LARGE_INTEGER frequency, BenchResult* result) {
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
	result->create_seconds = seconds_between(start, end, frequency);
	print_rate("create+rewrite", count, result->create_seconds);
	result->used_after_create = (uint64_t)fs->used_bytes;
	result->resident_after_create = (uint64_t)fs->resident_bytes;
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
	result->delete_seconds = seconds_between(start, end, frequency);
	print_rate("delete", count, result->delete_seconds);
	result->used_after_delete = (uint64_t)fs->used_bytes;
	result->resident_after_delete = (uint64_t)fs->resident_bytes;
	print_fs_state(fs, "after-delete");

	return 0;
}
static int bench_large_dir(Memfs* fs, uint32_t count, LARGE_INTEGER frequency, BenchResult* result) {
	uint8_t one_byte = 0x5a;
	LARGE_INTEGER start;
	LARGE_INTEGER end;
	uint32_t i;
	wchar_t name[32];

	printf("\n[large dir create/lookup/delete]\n");
	printf("files:            %u\n", count);

	QueryPerformanceCounter(&start);
	for (i = 0; i < count; i++) {
		MemfsNode* node = NULL;
		swprintf_s(name, _countof(name), L"ld%07u", i);
		if (memfs_node_create(fs, fs->root, name, false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &node) != MEMFS_OK) {
			return fail(fs, "large dir create failed", i);
		}
		if (memfs_node_write(node, &one_byte, 0, BENCH_1B_SIZE, false, false, &(uint32_t){0}) != MEMFS_OK) {
			memfs_node_close(node);
			return fail(fs, "large dir write failed", i);
		}
		memfs_node_close(node);
	}
	QueryPerformanceCounter(&end);
	result->create_seconds = seconds_between(start, end, frequency);
	print_rate("create+1B", count, result->create_seconds);
	result->used_after_create = (uint64_t)fs->used_bytes;
	result->resident_after_create = (uint64_t)fs->resident_bytes;
	print_fs_state(fs, "after-create");


	QueryPerformanceCounter(&start);
	for (i = 0; i < count; i++) {
		MemfsNode* node;
		swprintf_s(name, _countof(name), L"ld%07u", i);
		node = memfs_dir_lookup(fs->root, name);
		if (node == NULL) {
			return fail(fs, "large dir lookup failed", i);
		}
	}
	QueryPerformanceCounter(&end);
	result->lookup_seconds = seconds_between(start, end, frequency);
	print_rate("lookup", count, result->lookup_seconds);

	QueryPerformanceCounter(&start);
	for (i = 0; i < count; i++) {
		MemfsNode* node;
		swprintf_s(name, _countof(name), L"ld%07u", i);
		node = memfs_dir_lookup(fs->root, name);
		if (node == NULL || memfs_node_unlink(node) != MEMFS_OK) {
			return fail(fs, "large dir delete failed", i);
		}
	}
	QueryPerformanceCounter(&end);
	result->delete_seconds = seconds_between(start, end, frequency);
	print_rate("delete", count, result->delete_seconds);
	result->used_after_delete = (uint64_t)fs->used_bytes;
	result->resident_after_delete = (uint64_t)fs->resident_bytes;
	print_fs_state(fs, "after-delete");

	return 0;
}
#define BENCH_SWEEP_FILE_COUNT 10000U
#define BENCH_HIGH_OFFSET_1GB (1ULL << 30)
#define BENCH_HIGH_OFFSET_64GB (64ULL << 30)
#define BENCH_HIGH_OFFSET_1GB_COUNT 1000U
#define BENCH_HIGH_OFFSET_64GB_COUNT 100U

static int bench_high_offset_sparse(Memfs* fs, LARGE_INTEGER frequency) {
	uint8_t page[BENCH_4KB_SIZE];
	uint64_t offsets[2] = {BENCH_HIGH_OFFSET_1GB, BENCH_HIGH_OFFSET_64GB};
	uint32_t counts[2] = {BENCH_HIGH_OFFSET_1GB_COUNT, BENCH_HIGH_OFFSET_64GB_COUNT};
	const char* labels[2] = {"1GiB", "64GiB"};
	uint32_t o;

	memset(page, 0x5a, sizeof(page));

	printf("\n[high offset sparse single-page write]\n");
	printf("pattern:          create file, write 4KB at logical offset, close, unlink\n");
	printf("expectation:      page_group_count=1 per file if high offset does not allocate dense intermediate tables\n");

	for (o = 0; o < 2; o++) {
		Memfs* target_fs = fs;
		Memfs* temp_fs = NULL;
		uint64_t offset = offsets[o];
		uint32_t count = counts[o];
		uint64_t private_before;
		uint64_t private_after;
		uint64_t used_before;
		uint64_t used_after;
		uint64_t resident_before;
		uint64_t resident_after;
		MemfsAllocatorStats alloc_before;
		MemfsAllocatorStats alloc_after;
		LARGE_INTEGER start;
		LARGE_INTEGER end;
		double create_seconds;
		uint32_t i;
		wchar_t name[32];
		uint32_t last_group_count = 0;
		uint32_t last_group_capacity = 0;
		uint64_t last_group_index = 0;
		uint64_t last_node_resident = 0;
		int rc = 0;

		if (o == 1) {
			MemfsOptions options;
			memset(&options, 0, sizeof(options));
			options.capacity = 128ULL * 1024ULL * 1024ULL * 1024ULL;
			options.volume_label = L"BENCH_HIGH_OFFSET";
			rc = memfs_create_ex(&options, &temp_fs);
			if (rc != MEMFS_OK) {
				fprintf(stderr, "high offset 64GiB temp fs create failed rc=%d\n", rc);
				return 1;
			}
			target_fs = temp_fs;
		}

		printf("\n[high offset %s]\n", labels[o]);
		printf("files:            %u\n", count);
		printf("logical offset:   %llu B\n", (unsigned long long)offset);
		printf("write size:       %u B\n", BENCH_4KB_SIZE);
		printf("capacity:         %llu B\n", (unsigned long long)target_fs->capacity);

		private_before = private_bytes();
		used_before = target_fs->used_bytes;
		resident_before = target_fs->resident_bytes;
		memfs_allocator_get_stats(&target_fs->allocator, &alloc_before);

		QueryPerformanceCounter(&start);
		for (i = 0; i < count; i++) {
			MemfsNode* node = NULL;
			uint32_t written = 0;
			swprintf_s(name, _countof(name), L"ho%07u", i);
			if (memfs_node_create(target_fs, target_fs->root, name, false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &node) !=
				MEMFS_OK) {
				rc = 1;
				break;
			}
			if (memfs_node_write(node, page, offset, BENCH_4KB_SIZE, false, false, &written) != MEMFS_OK ||
				written != BENCH_4KB_SIZE) {
				memfs_node_close(node);
				rc = 1;
				break;
			}
			last_group_count = memfs_node_page_group_count(node);
			last_group_capacity = memfs_node_page_group_capacity(node);
			last_group_index = last_group_count > 0 ? memfs_node_page_group_index(node, 0) : 0;
			last_node_resident = memfs_node_resident_bytes(node);
			memfs_node_close(node);
		}
		QueryPerformanceCounter(&end);
		create_seconds = seconds_between(start, end, frequency);

		private_after = private_bytes();
		used_after = target_fs->used_bytes;
		resident_after = target_fs->resident_bytes;
		memfs_allocator_get_stats(&target_fs->allocator, &alloc_after);

		printf("%-12s %10.3f s  %12.0f ops/s\n", "create+4KB", create_seconds,
			   create_seconds > 0.0 ? (double)count / create_seconds : 0.0);
		printf("last page_group_count=%u page_group_capacity=%u first_group_index=%llu node_resident=%llu B\n",
			   last_group_count, last_group_capacity, (unsigned long long)last_group_index,
			   (unsigned long long)last_node_resident);
		printf("after-create used_bytes=%llu resident_bytes=%llu\n", (unsigned long long)used_after,
			   (unsigned long long)resident_after);
		printf("used delta:         %llu B resident delta: %llu B\n",
			   (unsigned long long)(used_after - used_before),
			   (unsigned long long)(resident_after - resident_before));
		printf("private delta:      %llu B (%.2f MiB)\n", (unsigned long long)(private_after - private_before),
			   (double)(private_after - private_before) / (1024.0 * 1024.0));
		printf("alloc reserved delta=%llu B live delta=%llu B live_objects delta=%llu\n",
			   (unsigned long long)(alloc_after.reserved_bytes - alloc_before.reserved_bytes),
			   (unsigned long long)(alloc_after.live_bytes - alloc_before.live_bytes),
			   (unsigned long long)(alloc_after.live_objects - alloc_before.live_objects));

		if (rc == 0) {
			QueryPerformanceCounter(&start);
			for (i = 0; i < count; i++) {
				MemfsNode* node;
				swprintf_s(name, _countof(name), L"ho%07u", i);
				node = memfs_dir_lookup(target_fs->root, name);
				if (node == NULL || memfs_node_unlink(node) != MEMFS_OK) {
					rc = 1;
					break;
				}
			}
			QueryPerformanceCounter(&end);
			print_rate("delete", count, seconds_between(start, end, frequency));
		}

		private_after = private_bytes();
		used_after = target_fs->used_bytes;
		resident_after = target_fs->resident_bytes;
		memfs_allocator_get_stats(&target_fs->allocator, &alloc_after);
		printf("after-delete used_bytes=%llu resident_bytes=%llu\n", (unsigned long long)used_after,
			   (unsigned long long)resident_after);
		printf("used delta total:   %llu B resident delta total: %llu B\n",
			   (unsigned long long)(used_after - used_before),
			   (unsigned long long)(resident_after - resident_before));
		printf("private delta total=%llu B (%.2f MiB)\n", (unsigned long long)(private_after - private_before),
			   (double)(private_after - private_before) / (1024.0 * 1024.0));
		printf("alloc reserved delta=%llu B live delta=%llu B live_objects delta=%llu\n",
			   (unsigned long long)(alloc_after.reserved_bytes - alloc_before.reserved_bytes),
			   (unsigned long long)(alloc_after.live_bytes - alloc_before.live_bytes),
			   (unsigned long long)(alloc_after.live_objects - alloc_before.live_objects));

		if (temp_fs != NULL) {
			memfs_destroy(temp_fs);
		}
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}
static int bench_small_size_sweep(Memfs* fs, uint32_t count, LARGE_INTEGER frequency) {
	static const uint32_t sizes[6] = {0U, 1U, 8U, 9U, 64U, 4096U};
	uint8_t payload[BENCH_4KB_SIZE];
	uint32_t s;
	uint32_t i;
	wchar_t name[32];

	memset(payload, 0x5a, sizeof(payload));

	printf("\n[small size sweep]\n");
	printf("files:            %u\n", count);
	printf("%-8s %-8s %12s %18s %16s %24s %18s %20s %14s %16s\n", "size(B)", "files", "create_ops/s",
		   "private_delta(B)", "private/file(B)", "alloc_reserved_delta(B)", "alloc_live_delta(B)",
		   "alloc_live_obj_delta", "used/file(B)", "resident/file(B)");

	for (s = 0; s < 6; s++) {
		uint32_t size = sizes[s];
		uint64_t private_before;
		uint64_t private_after;
		uint64_t used_before;
		uint64_t used_after;
		uint64_t resident_before;
		uint64_t resident_after;
		MemfsAllocatorStats alloc_before;
		MemfsAllocatorStats alloc_after;
		LARGE_INTEGER start;
		LARGE_INTEGER end;
		double create_seconds;

		private_before = private_bytes();
		used_before = fs->used_bytes;
		resident_before = fs->resident_bytes;
		memfs_allocator_get_stats(&fs->allocator, &alloc_before);

		QueryPerformanceCounter(&start);
		for (i = 0; i < count; i++) {
			MemfsNode* node = NULL;
			swprintf_s(name, _countof(name), L"ss%07u", i);
			if (memfs_node_create(fs, fs->root, name, false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &node) != MEMFS_OK) {
				return fail(fs, "sweep create failed", i);
			}
			if (size > 0) {
				uint32_t written = 0;
				if (memfs_node_write(node, payload, 0, size, false, false, &written) != MEMFS_OK ||
					written != size) {
					memfs_node_close(node);
					return fail(fs, "sweep write failed", i);
				}
			}
			memfs_node_close(node);
		}
		QueryPerformanceCounter(&end);
		create_seconds = seconds_between(start, end, frequency);

		private_after = private_bytes();
		used_after = fs->used_bytes;
		resident_after = fs->resident_bytes;
		memfs_allocator_get_stats(&fs->allocator, &alloc_after);

		printf("%-8u %-8u %12.0f %18llu %16.2f %24llu %18llu %20llu %14.2f %16.2f\n", size, count,
			   create_seconds > 0.0 ? (double)count / create_seconds : 0.0,
			   (unsigned long long)(private_after - private_before),
			   (double)(private_after - private_before) / (double)count,
			   (unsigned long long)(alloc_after.reserved_bytes - alloc_before.reserved_bytes),
			   (unsigned long long)(alloc_after.live_bytes - alloc_before.live_bytes),
			   (unsigned long long)(alloc_after.live_objects - alloc_before.live_objects),
			   (double)(used_after - used_before) / (double)count,
			   (double)(resident_after - resident_before) / (double)count);

		for (i = 0; i < count; i++) {
			MemfsNode* node;
			swprintf_s(name, _countof(name), L"ss%07u", i);
			node = memfs_dir_lookup(fs->root, name);
			if (node == NULL || memfs_node_unlink(node) != MEMFS_OK) {
				return fail(fs, "sweep delete failed", i);
			}
		}
		printf("size=%u after-delete used_bytes=%llu resident_bytes=%llu\n", size,
			   (unsigned long long)fs->used_bytes, (unsigned long long)fs->resident_bytes);
	}

	return 0;
}
static int run_suite(const BenchConfig* config, const char* label, LARGE_INTEGER frequency, BenchResult* results) {
	Memfs* fs = NULL;
	uint64_t private_before;
	uint64_t private_after;
	MemfsAllocatorStats stats;
	int rc;

	printf("\n=== mode: %s ===\n", label);
	print_config(config);

	if (create_fs(config, &fs) != MEMFS_OK) {
		fprintf(stderr, "memfs_create_ex failed for %s\n", label);
		return 1;
	}

	printf("MemfsNode:          %zu B\n", sizeof(MemfsNode));
	printf("capacity:           %llu B\n", (unsigned long long)fs->capacity);
	print_fs_state(fs, "initial");

	private_before = private_bytes();

	rc = bench_small_size_sweep(fs, BENCH_SWEEP_FILE_COUNT, frequency);
	if (rc != 0) {
		return rc;
	}

	results[0].label = "small";
	rc = bench_small_files(fs, BENCH_SMALL_FILE_COUNT, frequency, &results[0]);
	if (rc != 0) {
		return rc;
	}

	results[1].label = "4kb";
	rc = bench_4kb_files(fs, BENCH_4KB_FILE_COUNT, frequency, &results[1]);
	if (rc != 0) {
		return rc;
	}

	results[2].label = "1mb";
	rc = bench_1mb_sparse_write(fs, BENCH_1MB_FILE_COUNT, frequency, &results[2]);
	if (rc != 0) {
		return rc;
	}

	results[3].label = "rewrite";
	rc = bench_random_rewrite(fs, BENCH_RANDOM_REWRITE_COUNT, frequency, &results[3]);
	if (rc != 0) {
		return rc;
	}

	{
		static const uint32_t large_dir_counts[4] = {128U, 256U, 512U, 4096U};
		static const char* large_dir_labels[4] = {"dir128", "dir256", "dir512", "dir4096"};
		uint32_t k;
		for (k = 0; k < 4; k++) {
			results[4 + k].label = large_dir_labels[k];
			rc = bench_large_dir(fs, large_dir_counts[k], frequency, &results[4 + k]);
			if (rc != 0) {
				return rc;
			}
		}
	}

	rc = bench_high_offset_sparse(fs, frequency);
	if (rc != 0) {
		return rc;
	}

	private_after = private_bytes();
	memfs_allocator_get_stats(&fs->allocator, &stats);

	printf("\n[summary %s]\n", label);
	printf("private delta:      %.2f MiB\n", (double)(private_after - private_before) / (1024.0 * 1024.0));
	printf("allocator reserved: %.2f MiB\n", (double)stats.reserved_bytes / (1024.0 * 1024.0));
	printf("allocator live:     %llu objects / %.2f MiB\n", (unsigned long long)stats.live_objects,
		   (double)stats.live_bytes / (1024.0 * 1024.0));
	printf("allocator slabs:    %u\n", stats.slab_count);
	{
		uint64_t waste_bytes = stats.reserved_bytes > stats.live_bytes ? stats.reserved_bytes - stats.live_bytes : 0;
		double waste_percent = stats.reserved_bytes > 0 ? (double)waste_bytes * 100.0 / (double)stats.reserved_bytes : 0.0;
		printf("allocator reserved_bytes: %llu B\n", (unsigned long long)stats.reserved_bytes);
		printf("allocator live_bytes:     %llu B\n", (unsigned long long)stats.live_bytes);
		printf("allocator waste_bytes:    %llu B\n", (unsigned long long)waste_bytes);
		printf("allocator waste_percent:  %.2f%%\n", waste_percent);
	}
	print_fs_state(fs, "final");

	memfs_destroy(fs);
	return 0;
}
static void print_comparison(const BenchResult* plain, const BenchResult* compression, const BenchResult* encryption,
								const BenchResult* combined) {
	const BenchResult* modes[4];
	const char* names[4];
	uint32_t i;

	modes[0] = plain;
	names[0] = "plain";
	modes[1] = compression;
	names[1] = "compression";
	modes[2] = encryption;
	names[2] = "encryption";
	modes[3] = combined;
	names[3] = "combined";

	printf("\n[comparison]\n");
	printf("%-12s %-10s %12s %12s %12s %12s\n", "mode", "suite", "create_ops/s", "delete_ops/s", "used_after", "resident_after");
	for (i = 0; i < 8; i++) {
		uint32_t counts[8] = {BENCH_SMALL_FILE_COUNT, BENCH_4KB_FILE_COUNT, BENCH_1MB_FILE_COUNT, BENCH_RANDOM_REWRITE_COUNT,
							  128U, 256U, 512U, 4096U};
		uint32_t j;

		for (j = 0; j < 4; j++) {
			double create_ops = modes[j][i].create_seconds > 0.0 ? (double)counts[i] / modes[j][i].create_seconds : 0.0;
			double delete_ops = modes[j][i].delete_seconds > 0.0 ? (double)counts[i] / modes[j][i].delete_seconds : 0.0;

			printf("%-12s %-10s %12.0f %12.0f %12llu %12llu\n", names[j], modes[j][i].label, create_ops, delete_ops,
				   (unsigned long long)modes[j][i].used_after_create,
				   (unsigned long long)modes[j][i].resident_after_create);
		}
	}
}
static void print_comparison_csv(const BenchResult* plain, const BenchResult* compression, const BenchResult* encryption,
								  const BenchResult* combined) {
	const BenchResult* modes[4];
	const char* names[4];
	uint32_t i;
	uint32_t counts[8] = {BENCH_SMALL_FILE_COUNT, BENCH_4KB_FILE_COUNT, BENCH_1MB_FILE_COUNT, BENCH_RANDOM_REWRITE_COUNT,
						  128U, 256U, 512U, 4096U};

	modes[0] = plain;
	names[0] = "plain";
	modes[1] = compression;
	names[1] = "compression";
	modes[2] = encryption;
	names[2] = "encryption";
	modes[3] = combined;
	names[3] = "combined";

	printf("\n[comparison-csv]\n");
	printf("mode,suite,create_ops_per_s,delete_ops_per_s,used_after_create,resident_after_create\n");
	for (i = 0; i < 8; i++) {
		uint32_t j;
		for (j = 0; j < 4; j++) {
			double create_ops = modes[j][i].create_seconds > 0.0 ? (double)counts[i] / modes[j][i].create_seconds : 0.0;
			double delete_ops = modes[j][i].delete_seconds > 0.0 ? (double)counts[i] / modes[j][i].delete_seconds : 0.0;
			printf("%s,%s,%.0f,%.0f,%llu,%llu\n", names[j], modes[j][i].label, create_ops, delete_ops,
				   (unsigned long long)modes[j][i].used_after_create,
				   (unsigned long long)modes[j][i].resident_after_create);
		}
	}
}
static int parse_args(int argc, char** argv, BenchConfig* plain, BenchConfig* compression, BenchConfig* encryption,
					  BenchConfig* combined, BenchMode* mode, bool* csv_enabled) {
	int i;

	*csv_enabled = false;
	memset(plain, 0, sizeof(*plain));
	memset(compression, 0, sizeof(*compression));
	memset(encryption, 0, sizeof(*encryption));
	memset(combined, 0, sizeof(*combined));

	plain->compression_level = 1;

	compression->compression_enabled = true;
	compression->compression_level = 1;

	encryption->encryption_enabled = true;
	encryption->compression_level = 1;

	combined->compression_enabled = true;
	combined->compression_level = 1;
	combined->encryption_enabled = true;

	*mode = BENCH_MODE_COMPARE;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--plain") == 0) {
			*mode = BENCH_MODE_PLAIN;
		} else if (strcmp(argv[i], "--compression") == 0) {
			*mode = BENCH_MODE_COMPRESSION;
		} else if (strcmp(argv[i], "--encryption") == 0) {
			*mode = BENCH_MODE_ENCRYPTION;
		} else if (strcmp(argv[i], "--combined") == 0) {
			*mode = BENCH_MODE_COMBINED;
		} else if (strcmp(argv[i], "--compare") == 0) {
			*mode = BENCH_MODE_COMPARE;
		} else if (strcmp(argv[i], "--csv") == 0) {
			*csv_enabled = true;
		} else if (strcmp(argv[i], "--compression-level") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "--compression-level requires a value\n");
				return 1;
			}
			compression->compression_level = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			printf("usage: memfs_bench [--compare | --plain | --compression | --encryption]\n");
			printf("  --compare            run plain, compression, encryption and print comparison (default)\n");
			printf("  --plain              run plain mode only\n");
			printf("  --compression        run compression mode only\n");
			printf("  --encryption         run encryption mode only\n");
			printf("  --combined           run compression+encryption combined mode only\n");
			printf("  --compression-level N set compression level for compression mode\n");
			printf("  --csv                append CSV comparison data after --compare table\n");
			return 2;
		} else {
			fprintf(stderr, "unknown argument: %s\n", argv[i]);
			return 1;
		}
	}

	return 0;
}
int main(int argc, char** argv) {
	LARGE_INTEGER frequency;
	BenchConfig plain;
	BenchConfig compression;
	BenchConfig encryption;
	BenchConfig combined;
	BenchResult plain_results[8];
	BenchResult compression_results[8];
	BenchResult encryption_results[8];
	BenchResult combined_results[8];
	BenchMode mode;
	bool csv_enabled = false;
	int rc;

	if (!QueryPerformanceFrequency(&frequency)) {
		fprintf(stderr, "QueryPerformanceFrequency failed\n");
		return 2;
	}

	rc = parse_args(argc, argv, &plain, &compression, &encryption, &combined, &mode, &csv_enabled);
	if (rc == 2)
		return 0;
	if (rc != 0)
		return rc;

	if (mode == BENCH_MODE_COMPARE) {
		rc = run_suite(&plain, "plain", frequency, plain_results);
		if (rc != 0) {
			return rc;
		}

		rc = run_suite(&compression, "compression", frequency, compression_results);
		if (rc != 0) {
			return rc;
		}

		rc = run_suite(&encryption, "encryption", frequency, encryption_results);
		if (rc != 0) {
			return rc;
		}

		rc = run_suite(&combined, "combined", frequency, combined_results);
		if (rc != 0) {
			return rc;
		}

		print_comparison(plain_results, compression_results, encryption_results, combined_results);
		if (csv_enabled) {
			print_comparison_csv(plain_results, compression_results, encryption_results, combined_results);
		}
	} else if (mode == BENCH_MODE_PLAIN) {
		rc = run_suite(&plain, "plain", frequency, plain_results);
	} else if (mode == BENCH_MODE_COMPRESSION) {
		rc = run_suite(&compression, "compression", frequency, compression_results);
	} else if (mode == BENCH_MODE_ENCRYPTION) {
		rc = run_suite(&encryption, "encryption", frequency, encryption_results);
	} else if (mode == BENCH_MODE_COMBINED) {
		rc = run_suite(&combined, "combined", frequency, combined_results);
	}

	return rc;
}
