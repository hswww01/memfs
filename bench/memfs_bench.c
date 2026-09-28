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
static int parse_args(int argc, char** argv, BenchConfig* plain, BenchConfig* compression, BenchConfig* encryption,
					  BenchConfig* combined, BenchMode* mode) {
	int i;

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
	int rc;

	if (!QueryPerformanceFrequency(&frequency)) {
		fprintf(stderr, "QueryPerformanceFrequency failed\n");
		return 2;
	}

	rc = parse_args(argc, argv, &plain, &compression, &encryption, &combined, &mode);
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
