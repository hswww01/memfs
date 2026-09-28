#include <Windows.h>
#include <sddl.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "memfs_core.h"

static int g_checks;
static int g_failures;

#define CHECK(expr)                                                                                                    \
	do {                                                                                                               \
		g_checks++;                                                                                                    \
		if (!(expr)) {                                                                                                 \
			g_failures++;                                                                                              \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                                     \
		}                                                                                                              \
	} while (0)

static void test_tree_and_lookup(void) {
	Memfs* fs = NULL;
	MemfsNode* root;
	MemfsNode* dir;
	MemfsNode* file;
	MemfsNode* found;
	wchar_t name[MEMFS_MAX_NAME + 1];
	MemfsNode* parent;

	printf("== tree / lookup ==\n");

	CHECK(memfs_create(16 * 1024 * 1024ULL, L"TEST", &fs) == MEMFS_OK);
	CHECK(fs != NULL);

	root = fs->root;
	CHECK(root != NULL);
	CHECK(memfs_node_is_directory(root));

	CHECK(memfs_node_create(fs, root, L"Dir", true, FILE_ATTRIBUTE_DIRECTORY, NULL, 0, &dir) == MEMFS_OK);

	CHECK(memfs_node_create(fs, dir, L"file.txt", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);

	CHECK(memfs_lookup_path(fs, L"\\Dir\\file.txt", &found) == MEMFS_OK);
	CHECK(found == file);

	CHECK(memfs_lookup_path(fs, L"\\dir\\FILE.TXT", &found) == MEMFS_OK);
	CHECK(found == file);

	CHECK(memfs_lookup_parent(fs, L"\\Dir\\next.bin", &parent, name) == MEMFS_OK);
	CHECK(parent == dir);
	CHECK(wcscmp(name, L"next.bin") == 0);

	memfs_node_close(file);
	memfs_node_close(dir);
	memfs_destroy(fs);
}

static void test_io_and_resize(void) {
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t input[1024];
	uint8_t output[2048];
	uint32_t transferred;
	uint32_t i;

	printf("== io / resize ==\n");

	CHECK(memfs_create(8 * 1024 * 1024ULL, L"TEST", &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"data.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);

	for (i = 0; i < sizeof(input); i++)
		input[i] = (uint8_t)i;

	CHECK(memfs_node_write(file, input, 0, sizeof(input), false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(input));
	CHECK(file->file_size == sizeof(input));
	CHECK(file->allocation_size == 1024);

	memset(output, 0, sizeof(output));
	CHECK(memfs_node_read(file, output, 0, sizeof(input), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(input));
	CHECK(memcmp(input, output, sizeof(input)) == 0);

	CHECK(memfs_node_set_file_size(file, 4096) == MEMFS_OK);
	CHECK(file->file_size == 4096);
	CHECK(file->allocation_size == 4096);

	memset(output, 0xff, sizeof(output));
	CHECK(memfs_node_read(file, output, 1024, sizeof(output), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(output));
	for (i = 0; i < sizeof(output); i++)
		CHECK(output[i] == 0);

	CHECK(memfs_node_set_allocation_size(file, 512) == MEMFS_OK);
	CHECK(file->allocation_size == 512);
	CHECK(file->file_size == 512);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	CHECK(fs->orphan_head == file);
	memfs_node_close(file);
	CHECK(fs->orphan_head == NULL);
	CHECK((uint64_t)fs->used_bytes == 0);

	memfs_destroy(fs);
}

static void test_rename_and_delete(void) {
	Memfs* fs = NULL;
	MemfsNode* a;
	MemfsNode* b;
	MemfsNode* file;
	MemfsNode* found;

	printf("== rename / delete ==\n");

	CHECK(memfs_create(8 * 1024 * 1024ULL, L"TEST", &fs) == MEMFS_OK);

	CHECK(memfs_node_create(fs, fs->root, L"A", true, FILE_ATTRIBUTE_DIRECTORY, NULL, 0, &a) == MEMFS_OK);

	CHECK(memfs_node_create(fs, fs->root, L"B", true, FILE_ATTRIBUTE_DIRECTORY, NULL, 0, &b) == MEMFS_OK);

	CHECK(memfs_node_create(fs, a, L"x.txt", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);

	CHECK(memfs_node_rename(file, b, L"y.txt", false) == MEMFS_OK);
	CHECK(memfs_lookup_path(fs, L"\\A\\x.txt", &found) == MEMFS_ERR_NOT_FOUND);
	CHECK(memfs_lookup_path(fs, L"\\B\\y.txt", &found) == MEMFS_OK);
	CHECK(found == file);
	CHECK(wcscmp(file->name, L"y.txt") == 0);

	CHECK(memfs_node_unlink(b) == MEMFS_ERR_NOT_EMPTY);
	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);

	CHECK(memfs_node_unlink(b) == MEMFS_OK);
	memfs_node_close(b);

	CHECK(memfs_node_unlink(a) == MEMFS_OK);
	memfs_node_close(a);

	memfs_destroy(fs);
}

static void test_capacity(void) {
	Memfs* fs = NULL;
	MemfsNode* a;
	MemfsNode* b;
	uint32_t written;
	uint8_t buffer[1024] = {0};

	printf("== capacity ==\n");

	CHECK(memfs_create(1536, L"TINY", &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"a", false, FILE_ATTRIBUTE_NORMAL, NULL, 1024, &a) == MEMFS_OK);
	CHECK(memfs_node_write(a, buffer, 0, 1024, false, false, &written) == MEMFS_OK);
	CHECK(written == 1024);
	CHECK(memfs_free_bytes(fs) == 512);

	// AllocationSize preallocation does not consume byte-granular logical quota.
	CHECK(memfs_node_create(fs, fs->root, L"b", false, FILE_ATTRIBUTE_NORMAL, NULL, 1024, &b) == MEMFS_OK);
	CHECK(memfs_node_write(b, buffer, 0, 1024, false, false, &written) == MEMFS_ERR_NO_SPACE);

	CHECK(memfs_node_unlink(a) == MEMFS_OK);
	memfs_node_close(a);

	CHECK(memfs_node_write(b, buffer, 0, 1024, false, false, &written) == MEMFS_OK);
	CHECK(written == 1024);

	CHECK(memfs_node_unlink(b) == MEMFS_OK);
	memfs_node_close(b);
	memfs_destroy(fs);
}

static void test_directory_order(void) {
	Memfs* fs = NULL;
	MemfsNode* nodes[5];
	const wchar_t* names[] = {L"zeta", L"Alpha", L"gamma", L"beta", L"Delta"};
	const wchar_t* expected[] = {L"Alpha", L"beta", L"Delta", L"gamma", L"zeta"};
	MemfsNode* p;
	uint32_t i;

	printf("== directory order ==\n");

	CHECK(memfs_create(8 * 1024 * 1024ULL, L"TEST", &fs) == MEMFS_OK);

	for (i = 0; i < 5; i++) {
		CHECK(memfs_node_create(fs, fs->root, names[i], false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &nodes[i]) == MEMFS_OK);
	}

	p = memfs_dir_first(fs->root);
	for (i = 0; i < 5; i++) {
		CHECK(p != NULL);
		if (p) {
			CHECK(_wcsicmp(p->name, expected[i]) == 0);
			p = memfs_dir_next(p);
		}
	}
	CHECK(p == NULL);
	CHECK(memfs_dir_upper_bound(fs->root, L"") == nodes[1]);
	CHECK(memfs_dir_upper_bound(fs->root, L"Alpha") == nodes[3]);
	CHECK(memfs_dir_upper_bound(fs->root, L"Delta") == nodes[2]);
	CHECK(memfs_dir_upper_bound(fs->root, L"zeta") == NULL);

	for (i = 0; i < 5; i++) {
		CHECK(memfs_node_unlink(nodes[i]) == MEMFS_OK);
		memfs_node_close(nodes[i]);
	}

	memfs_destroy(fs);
}

static void test_small_storage(void) {
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t value = 0x5a;
	uint8_t out = 0;
	uint32_t transferred;

	printf("== small storage ==\n");

	CHECK(memfs_create(8 * 1024 * 1024ULL, L"TEST", &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"small.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);

	CHECK(memfs_node_write(file, &value, 0, 1, false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == 1);
	CHECK(file->small_inline);
	CHECK(memfs_node_page_group_count(file) == 0);
	CHECK(file->small_capacity == 1);
	CHECK(memfs_node_resident_bytes(file) == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);
	CHECK(file->allocation_size == 1);
	CHECK((uint64_t)fs->used_bytes == 1);

	CHECK(memfs_node_read(file, &out, 0, 1, &transferred) == MEMFS_OK);
	CHECK(out == value);

	CHECK(memfs_node_write(file, &value, 1, 1, false, false, &transferred) == MEMFS_OK);
	CHECK(file->small_capacity == 2);
	CHECK(memfs_node_resident_bytes(file) == 0);
	CHECK(file->allocation_size == 2);
	CHECK((uint64_t)fs->used_bytes == 2);

	CHECK(memfs_node_write(file, &value, 2, 1, false, false, &transferred) == MEMFS_OK);
	CHECK(file->small_capacity == 4);
	CHECK(memfs_node_resident_bytes(file) == 0);
	CHECK(file->allocation_size == 3);
	CHECK((uint64_t)fs->used_bytes == 3);

	CHECK(memfs_node_write(file, &value, 4, 1, false, false, &transferred) == MEMFS_OK);
	CHECK(file->small_capacity == 8);
	CHECK(memfs_node_resident_bytes(file) == 0);
	CHECK(file->allocation_size == 5);
	CHECK((uint64_t)fs->used_bytes == 5);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	CHECK((uint64_t)fs->resident_bytes == 0);
	CHECK((uint64_t)fs->used_bytes == 0);
	memfs_destroy(fs);
}

static void test_sparse_pages(void) {
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t value = 0xa7;
	uint8_t buffer[32];
	uint32_t transferred;
	uint64_t sparse_size = 64ULL * 1024ULL * 1024ULL;
	uint64_t write_offset = 32ULL * 1024ULL * 1024ULL + 123;
	uint32_t i;

	printf("== sparse pages ==\n");

	CHECK(memfs_create(128ULL * 1024ULL * 1024ULL, L"TEST", &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"sparse.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);

	CHECK(memfs_node_set_file_size(file, sparse_size) == MEMFS_OK);
	CHECK(file->file_size == sparse_size);
	CHECK(file->allocation_size == sparse_size);
	CHECK((uint64_t)fs->used_bytes == sparse_size);
	CHECK(memfs_node_resident_bytes(file) == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);

	memset(buffer, 0xcc, sizeof(buffer));
	CHECK(memfs_node_read(file, buffer, 16ULL * 1024ULL * 1024ULL, sizeof(buffer), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(buffer));
	for (i = 0; i < sizeof(buffer); i++)
		CHECK(buffer[i] == 0);

	CHECK(memfs_node_write(file, &value, write_offset, 1, false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == 1);
	CHECK(memfs_node_resident_bytes(file) >= MEMFS_PAGE_SIZE);
	CHECK(memfs_node_resident_bytes(file) < MEMFS_PAGE_SIZE + 64U);
	CHECK((uint64_t)fs->resident_bytes >= MEMFS_PAGE_SIZE);
	CHECK((uint64_t)fs->resident_bytes < MEMFS_PAGE_SIZE + 64U);
	CHECK(memfs_node_page_group_count(file) != 0);
	CHECK(sizeof(MemfsPageGroup) <= 64U);
	if (memfs_node_page_group_count(file) != 0) {
		uint64_t group_index = (write_offset >> MEMFS_PAGE_SHIFT) >> MEMFS_PAGE_GROUP_SHIFT;
		MemfsPageGroup* group = NULL;

		CHECK(memfs_node_page_group_count(file) == 1);
		CHECK(memfs_node_page_group_capacity(file) == 1);
		CHECK(memfs_node_page_group_index(file, 0) == group_index);

		if (memfs_node_page_group_count(file) == 1)
			group = memfs_node_page_group(file, 0);

		CHECK(group != NULL);
		if (group) {
			CHECK(group->page_count == 1);
			CHECK(group->page_capacity == 1);
			CHECK(group->pages != NULL);
			if (group->pages)
				CHECK(group->pages[0] != NULL);
		}
	}

	memset(buffer, 0xcc, sizeof(buffer));
	CHECK(memfs_node_read(file, buffer, write_offset - 8, sizeof(buffer), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(buffer));
	CHECK(buffer[8] == value);
	for (i = 0; i < sizeof(buffer); i++) {
		if (i != 8)
			CHECK(buffer[i] == 0);
	}

	CHECK(memfs_node_set_file_size(file, 2048) == MEMFS_OK);
	CHECK(file->file_size == 2048);
	CHECK(file->allocation_size == sparse_size);
	CHECK(memfs_node_resident_bytes(file) == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);

	CHECK(memfs_node_set_allocation_size(file, 2048) == MEMFS_OK);
	CHECK(file->allocation_size == 2048);
	CHECK((uint64_t)fs->used_bytes == 2048);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	CHECK((uint64_t)fs->used_bytes == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);
	memfs_destroy(fs);
}

static void test_very_high_sparse_offset(void) {
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t value = 0x6b;
	uint8_t out = 0;
	uint32_t transferred;
	uint64_t offset = 1ULL << 40;
	uint64_t expected_group = (offset >> MEMFS_PAGE_SHIFT) >> MEMFS_PAGE_GROUP_SHIFT;

	printf("== very high sparse offset ==\n");

	CHECK(memfs_create(2ULL << 40, L"HUGE", &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"huge-sparse.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) ==
		  MEMFS_OK);

	CHECK(memfs_node_write(file, &value, offset, 1, false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == 1);

	CHECK(memfs_node_page_group_count(file) == 1);
	CHECK(memfs_node_page_group_capacity(file) == 1);
	CHECK(memfs_node_page_group_count(file) != 0);

	if (memfs_node_page_group_count(file) != 0) {
		CHECK(memfs_node_page_group_index(file, 0) == expected_group);
		CHECK(memfs_node_page_group(file, 0) != NULL);
		if (memfs_node_page_group(file, 0)) {
			CHECK(memfs_node_page_group(file, 0)->page_count == 1);
			CHECK(memfs_node_page_group(file, 0)->page_capacity == 1);
		}
	}

	CHECK(memfs_node_read(file, &out, offset, 1, &transferred) == MEMFS_OK);
	CHECK(out == value);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	CHECK(memfs_resident_bytes(fs) == 0);
	memfs_destroy(fs);
}

static void test_small_to_paged_promotion(void) {
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t prefix[1024];
	uint8_t value = 0x7d;
	uint8_t verify[1024];
	uint32_t transferred;
	uint32_t i;

	printf("== small -> paged promotion ==\n");

	for (i = 0; i < sizeof(prefix); i++)
		prefix[i] = (uint8_t)i;

	CHECK(memfs_create(16ULL * 1024ULL * 1024ULL, L"TEST", &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"promote.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);

	CHECK(memfs_node_write(file, prefix, 0, sizeof(prefix), false, false, &transferred) == MEMFS_OK);
	CHECK(!file->small_inline);
	CHECK(memfs_node_resident_bytes(file) >= 1024);
	CHECK(memfs_node_resident_bytes(file) < 1088);

	CHECK(memfs_node_write(file, &value, 2ULL * MEMFS_PAGE_SIZE + 17, 1, false, false, &transferred) == MEMFS_OK);

	CHECK(file->small_capacity == 0);
	CHECK(memfs_node_page_group_count(file) != 0);
	CHECK(memfs_node_resident_bytes(file) >= 2ULL * MEMFS_PAGE_SIZE);
	CHECK(memfs_node_resident_bytes(file) < 2ULL * MEMFS_PAGE_SIZE + 128U);

	memset(verify, 0, sizeof(verify));
	CHECK(memfs_node_read(file, verify, 0, sizeof(verify), &transferred) == MEMFS_OK);
	CHECK(memcmp(prefix, verify, sizeof(prefix)) == 0);

	CHECK(memfs_node_set_file_size(file, 1024) == MEMFS_OK);
	CHECK(!file->small_inline);
	CHECK(memfs_node_page_group_count(file) == 0);
	CHECK(memfs_node_resident_bytes(file) >= 1024);
	CHECK(memfs_node_resident_bytes(file) < 1088);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	CHECK((uint64_t)fs->resident_bytes == 0);
	memfs_destroy(fs);
}
static void test_large_directory(void) {
	enum { COUNT = 5000 };
	Memfs* fs = NULL;
	MemfsNode** nodes;
	MemfsNode* node;
	MemfsNode* prev = NULL;
	wchar_t name[64];
	uint32_t i;
	uint32_t count = 0;

	printf("== large directory treap ==\n");

	nodes = calloc(COUNT, sizeof(*nodes));
	CHECK(nodes != NULL);
	CHECK(memfs_create(64ULL * 1024ULL * 1024ULL, L"TEST", &fs) == MEMFS_OK);

	if (nodes == NULL || fs == NULL)
		goto exit;

	for (i = 0; i < COUNT; i++) {
		uint32_t value = COUNT - 1U - i;

		swprintf_s(name, _countof(name), L"item-%05u", value);
		CHECK(memfs_node_create(fs, fs->root, name, false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &nodes[value]) == MEMFS_OK);
	}

	CHECK(fs->root->dir->child_count == COUNT);
	CHECK(fs->root->dir->hash != NULL);
	CHECK(sizeof(MemfsDir) <= 24);
	CHECK(sizeof(MemfsNode) <= 136);
	CHECK(fs->root->dir->hash->count == COUNT);

	for (i = 0; i < COUNT; i += 97U) {
		swprintf_s(name, _countof(name), L"ITEM-%05u", i);
		CHECK(memfs_dir_lookup(fs->root, name) == nodes[i]);
	}

	for (node = memfs_dir_first(fs->root); node; node = memfs_dir_next(node)) {
		if (prev)
			CHECK(_wcsicmp(prev->name, node->name) < 0);
		prev = node;
		count++;
	}
	CHECK(count == COUNT);

	// Robin Hood backward-shift deletion: remove half the table, verify both
	// negative and positive lookups, then fill it again to exercise probe chains.
	for (i = 1; i < COUNT; i += 2U) {
		CHECK(memfs_node_unlink(nodes[i]) == MEMFS_OK);
		memfs_node_close(nodes[i]);
		nodes[i] = NULL;
	}
	CHECK(fs->root->dir->child_count == COUNT / 2U);
	CHECK(fs->root->dir->hash != NULL);
	CHECK(fs->root->dir->hash->count == COUNT / 2U);

	for (i = 0; i < COUNT; i++) {
		swprintf_s(name, _countof(name), L"item-%05u", i);
		if (i & 1U)
			CHECK(memfs_dir_lookup(fs->root, name) == NULL);
		else
			CHECK(memfs_dir_lookup(fs->root, name) == nodes[i]);
	}

	for (i = 1; i < COUNT; i += 2U) {
		swprintf_s(name, _countof(name), L"new-%05u", i);
		CHECK(memfs_node_create(fs, fs->root, name, false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &nodes[i]) == MEMFS_OK);
	}
	CHECK(fs->root->dir->child_count == COUNT);
	CHECK(fs->root->dir->hash != NULL);
	CHECK(fs->root->dir->hash->count == COUNT);

	prev = NULL;
	count = 0;
	for (node = memfs_dir_first(fs->root); node; node = memfs_dir_next(node)) {
		if (prev)
			CHECK(_wcsicmp(prev->name, node->name) < 0);
		prev = node;
		count++;
	}
	CHECK(count == COUNT);

	for (i = 0; i < COUNT; i++) {
		CHECK(memfs_node_unlink(nodes[i]) == MEMFS_OK);
		memfs_node_close(nodes[i]);
	}

exit:
	free(nodes);
	memfs_destroy(fs);
}

static void test_compression(void) {
	MemfsOptions options = {0};
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t input[8192];
	uint8_t output[8192];
	uint32_t transferred;
	MemfsPageGroup* group;

	printf("== zstd compression ==\n");

	memset(input, 'A', sizeof(input));
	options.capacity = 16ULL * 1024ULL * 1024ULL;
	options.volume_label = L"COMP";
	options.compression_enabled = true;
	options.compression_level = 1;

	CHECK(memfs_create_ex(&options, &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"compressed.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);

	CHECK(memfs_node_write(file, input, 0, sizeof(input), false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(input));
	CHECK(memfs_node_page_group_count(file) != 0);

	group = memfs_node_page_group(file, 0);
	CHECK(group != NULL);
	if (group) {
		CHECK(group->pages[0] != NULL);
		CHECK(group->pages[1] != NULL);
		if (group->pages[0])
			CHECK(0 != (group->pages[0]->flags & MEMFS_PAGE_COMPRESSED));
		if (group->pages[1])
			CHECK(0 != (group->pages[1]->flags & MEMFS_PAGE_COMPRESSED));
	}

	CHECK(memfs_node_resident_bytes(file) < 1024);

	memset(output, 0, sizeof(output));
	CHECK(memfs_node_read(file, output, 0, sizeof(output), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(output));
	CHECK(memcmp(input, output, sizeof(input)) == 0);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	memfs_destroy(fs);
}

static void test_adaptive_compression(void) {
	MemfsOptions options = {0};
	Memfs* fs = NULL;
	MemfsNode* file;
	MemfsPageGroup* group;
	uint8_t page[MEMFS_PAGE_SIZE];
	uint8_t verify[MEMFS_PAGE_SIZE];
	uint32_t transferred;
	uint32_t state = 0x12345678U;
	uint32_t page_index;
	uint32_t i;

	printf("== adaptive compression ==\n");

	options.capacity = 4ULL * 1024ULL * 1024ULL;
	options.volume_label = L"ADAPT";
	options.compression_enabled = true;
	options.compression_level = 1;

	CHECK(memfs_create_ex(&options, &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"adaptive.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);

	// 64 pages of pseudo-random input: after a few failed probes the
	// compressor should enter skip/probe mode.
	for (page_index = 0; page_index < 64; page_index++) {
		for (i = 0; i < sizeof(page); i += sizeof(state)) {
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			memcpy(page + i, &state, sizeof(state));
		}

		CHECK(memfs_node_write(file, page, (uint64_t)page_index * MEMFS_PAGE_SIZE, sizeof(page), false, false,
							   &transferred) == MEMFS_OK);
	}

	CHECK(memfs_node_compression_score(file) >= MEMFS_COMPRESSION_SKIP_SCORE);

	// Data becomes compressible. Periodic probes must discover this and
	// re-enable normal compression attempts.
	memset(page, 'A', sizeof(page));
	for (; page_index < 128; page_index++) {
		CHECK(memfs_node_write(file, page, (uint64_t)page_index * MEMFS_PAGE_SIZE, sizeof(page), false, false,
							   &transferred) == MEMFS_OK);
	}

	CHECK(memfs_node_compression_score(file) < MEMFS_COMPRESSION_SKIP_SCORE);

	group = memfs_node_page_group(file, 0);
	CHECK(group != NULL);
	if (group && group->page_count == 128) {
		CHECK(0 != (group->pages[127]->flags & MEMFS_PAGE_COMPRESSED));
	}

	memset(verify, 0, sizeof(verify));
	CHECK(memfs_node_read(file, verify, 127ULL * MEMFS_PAGE_SIZE, sizeof(verify), &transferred) == MEMFS_OK);
	CHECK(memcmp(page, verify, sizeof(page)) == 0);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	memfs_destroy(fs);
}

static void test_encryption(void) {
	MemfsOptions options = {0};
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t key[MEMFS_ENCRYPTION_KEY_SIZE];
	uint8_t value = 0x5a;
	uint8_t output = 0;
	uint8_t saved;
	uint32_t transferred;
	uint32_t i;

	printf("== XChaCha20-Poly1305 encryption ==\n");

	for (i = 0; i < sizeof(key); i++)
		key[i] = (uint8_t)(i * 7U + 3U);

	options.capacity = 8ULL * 1024ULL * 1024ULL;
	options.volume_label = L"CRYPT";
	options.encryption_enabled = true;
	options.encryption_key = key;
	options.encryption_key_size = sizeof(key);

	CHECK(memfs_create_ex(&options, &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"secret.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);

	CHECK(memfs_node_write(file, &value, 0, 1, false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == 1);
	CHECK(file->small_capacity == 1);
	CHECK(!file->small_inline);
	CHECK(memfs_node_resident_bytes(file) == sizeof(MemfsPage) + sizeof(uint64_t) + 1U + 16U);

	if (file->small_page) {
		CHECK(0 != (file->small_page->flags & MEMFS_PAGE_ENCRYPTED));
		CHECK(file->small_page->stored_size > file->small_capacity);
	}

	CHECK(memfs_node_read(file, &output, 0, 1, &transferred) == MEMFS_OK);
	CHECK(output == value);

	if (file->small_page && file->small_page->stored_size) {
		saved = file->small_page->data[file->small_page->stored_size - 1U];
		file->small_page->data[file->small_page->stored_size - 1U] ^= 0x80;
		CHECK(memfs_node_read(file, &output, 0, 1, &transferred) == MEMFS_ERR_DATA);
		file->small_page->data[file->small_page->stored_size - 1U] = saved;
	}

	CHECK(memfs_node_read(file, &output, 0, 1, &transferred) == MEMFS_OK);
	CHECK(output == value);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	memfs_destroy(fs);
}

static void test_compression_encryption(void) {
	MemfsOptions options = {0};
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t key[MEMFS_ENCRYPTION_KEY_SIZE];
	uint8_t input[8192];
	uint8_t output[8192];
	uint32_t transferred;
	uint32_t i;
	MemfsPageGroup* group;

	printf("== compression + encryption ==\n");

	for (i = 0; i < sizeof(key); i++)
		key[i] = (uint8_t)(0xa5U ^ i);
	memset(input, 0x11, sizeof(input));

	options.capacity = 16ULL * 1024ULL * 1024ULL;
	options.volume_label = L"BOTH";
	options.compression_enabled = true;
	options.compression_level = 3;
	options.encryption_enabled = true;
	options.encryption_key = key;
	options.encryption_key_size = sizeof(key);

	CHECK(memfs_create_ex(&options, &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"both.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);

	CHECK(memfs_node_write(file, input, 0, sizeof(input), false, false, &transferred) == MEMFS_OK);

	group = memfs_node_page_group_count(file) ? memfs_node_page_group(file, 0) : NULL;
	CHECK(group != NULL);
	if (group && group->pages[0]) {
		CHECK(0 != (group->pages[0]->flags & MEMFS_PAGE_COMPRESSED));
		CHECK(0 != (group->pages[0]->flags & MEMFS_PAGE_ENCRYPTED));
	}
	CHECK(memfs_node_resident_bytes(file) < 2048);

	memset(output, 0, sizeof(output));
	CHECK(memfs_node_read(file, output, 0, sizeof(output), &transferred) == MEMFS_OK);
	CHECK(memcmp(input, output, sizeof(input)) == 0);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	memfs_destroy(fs);
}

static void test_shared_security(void) {
	enum { FILES = 256 };
	Memfs* fs = NULL;
	MemfsNode* dir;
	MemfsNode* files[FILES] = {0};
	PSECURITY_DESCRIPTOR alternate = NULL;
	ULONG alternate_size = 0;
	MemfsSecurity* shared;
	LONG before;
	uint32_t i;

	printf("== shared ACL metadata ==\n");

	CHECK(memfs_create(32ULL * 1024ULL * 1024ULL, L"ACL", &fs) == MEMFS_OK);
	if (fs == NULL)
		return;

	CHECK(memfs_node_create(fs, fs->root, L"dir", true, FILE_ATTRIBUTE_DIRECTORY, NULL, 0, &dir) == MEMFS_OK);
	if (dir == NULL) {
		memfs_destroy(fs);
		return;
	}

	shared = memfs_node_get_security(dir);
	CHECK(shared == memfs_node_get_security(fs->root));
	before = shared->ref_count;

	for (i = 0; i < FILES; i++) {
		wchar_t name[32];

		swprintf_s(name, _countof(name), L"f-%03u", i);

		CHECK(memfs_node_create(fs, dir, name, false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &files[i]) == MEMFS_OK);

		if (files[i])
			CHECK(memfs_node_get_security(files[i]) == shared);
	}

	CHECK(shared->ref_count == before + FILES);
	CHECK(shared->size > 0);

	CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;GRGW;;;WD)", SDDL_REVISION_1,
															   &alternate, &alternate_size));

	if (alternate && files[0]) {
		CHECK(memfs_node_replace_security(files[0], alternate, alternate_size) == MEMFS_OK);
		CHECK(memfs_node_get_security(files[0]) != shared);
		CHECK(shared->ref_count == before + FILES - 1);
	}

	if (alternate)
		LocalFree(alternate);

	for (i = 0; i < FILES; i++) {
		if (files[i]) {
			CHECK(memfs_node_unlink(files[i]) == MEMFS_OK);
			memfs_node_close(files[i]);
		}
	}

	CHECK(memfs_node_unlink(dir) == MEMFS_OK);
	memfs_node_close(dir);
	memfs_destroy(fs);
}

typedef struct ConcurrentIoArg {
	MemfsNode* file;
	uint8_t value;
	uint32_t blocks;
	uint32_t failures;
} ConcurrentIoArg;

static DWORD WINAPI concurrent_io_thread(void* parameter) {
	ConcurrentIoArg* arg = parameter;
	uint8_t block[4096];
	uint8_t verify[4096];
	uint32_t transferred;
	uint32_t i;

	memset(block, arg->value, sizeof(block));

	for (i = 0; i < arg->blocks; i++) {
		if (memfs_node_write(arg->file, block, 0, sizeof(block), true, false, &transferred) != MEMFS_OK ||
			transferred != sizeof(block)) {
			arg->failures++;
			return 0;
		}
	}

	memset(verify, 0, sizeof(verify));
	if (memfs_node_read(arg->file, verify, (uint64_t)(arg->blocks - 1U) * sizeof(block), sizeof(verify),
						&transferred) != MEMFS_OK ||
		transferred != sizeof(verify)) {
		arg->failures++;
		return 0;
	}

	for (i = 0; i < sizeof(verify); i++) {
		if (verify[i] != arg->value) {
			arg->failures++;
			break;
		}
	}

	return 0;
}

static void test_concurrent_files(void) {
	enum { THREADS = 8, BLOCKS = 128 };
	MemfsOptions options = {0};
	Memfs* fs = NULL;
	MemfsNode* files[THREADS] = {0};
	ConcurrentIoArg args[THREADS] = {0};
	HANDLE threads[THREADS] = {0};
	uint8_t key[MEMFS_ENCRYPTION_KEY_SIZE];
	uint32_t i;
	uint64_t expected_size = (uint64_t)BLOCKS * MEMFS_PAGE_SIZE;

	printf("== concurrent files / accounting ==\n");

	for (i = 0; i < sizeof(key); i++)
		key[i] = (uint8_t)(0x31U + i * 3U);

	options.capacity = 64ULL * 1024ULL * 1024ULL;
	options.volume_label = L"PAR";
	options.compression_enabled = true;
	options.compression_level = 1;
	options.encryption_enabled = true;
	options.encryption_key = key;
	options.encryption_key_size = sizeof(key);

	CHECK(memfs_create_ex(&options, &fs) == MEMFS_OK);
	if (fs == NULL)
		return;

	for (i = 0; i < THREADS; i++) {
		wchar_t name[32];

		swprintf_s(name, _countof(name), L"thread-%u.bin", i);
		CHECK(memfs_node_create(fs, fs->root, name, false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &files[i]) == MEMFS_OK);

		args[i].file = files[i];
		args[i].value = (uint8_t)(i + 1U);
		args[i].blocks = BLOCKS;

		threads[i] = CreateThread(NULL, 0, concurrent_io_thread, &args[i], 0, NULL);
		CHECK(threads[i] != NULL);
	}

	WaitForMultipleObjects(THREADS, threads, TRUE, INFINITE);

	for (i = 0; i < THREADS; i++) {
		if (threads[i])
			CloseHandle(threads[i]);

		CHECK(args[i].failures == 0);
		CHECK(files[i]->file_size == expected_size);
		CHECK(files[i]->allocation_size == expected_size);
	}

	CHECK(memfs_free_bytes(fs) == options.capacity - (uint64_t)THREADS * expected_size);
	CHECK(memfs_resident_bytes(fs) < (uint64_t)THREADS * expected_size / 4U);

	for (i = 0; i < THREADS; i++) {
		CHECK(memfs_node_unlink(files[i]) == MEMFS_OK);
		memfs_node_close(files[i]);
	}

	CHECK(memfs_free_bytes(fs) == options.capacity);
	CHECK(memfs_resident_bytes(fs) == 0);
	memfs_destroy(fs);
}

static void test_allocator_reclaim(void) {
	enum { FILES = 20000 };
	Memfs* fs = NULL;
	MemfsNode* node;
	MemfsAllocatorStats before_delete;
	MemfsAllocatorStats after_delete;
	uint32_t i;
	wchar_t name[32];

	printf("== allocator reclaim ==\n");

	CHECK(memfs_create(8 * 1024 * 1024ULL, L"ALLOC", &fs) == MEMFS_OK);
	if (fs == NULL)
		return;

	for (i = 0; i < FILES; i++) {
		swprintf_s(name, _countof(name), L"f%05u", i);
		CHECK(memfs_node_create(fs, fs->root, name, false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &node) == MEMFS_OK);
		memfs_node_close(node);
	}

	memfs_allocator_get_stats(&fs->allocator, &before_delete);
	CHECK(before_delete.live_objects > FILES);
	CHECK(before_delete.reserved_bytes > 1024 * 1024ULL);

	for (i = 0; i < FILES; i++) {
		swprintf_s(name, _countof(name), L"f%05u", i);
		node = memfs_dir_lookup(fs->root, name);
		CHECK(node != NULL);
		if (node)
			CHECK(memfs_node_unlink(node) == MEMFS_OK);
	}

	memfs_allocator_get_stats(&fs->allocator, &after_delete);
	CHECK(after_delete.live_objects == 3);
	CHECK(after_delete.reserved_bytes < 1024 * 1024ULL);

	memfs_destroy(fs);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);

	test_tree_and_lookup();
	test_io_and_resize();
	test_rename_and_delete();
	test_capacity();
	test_directory_order();
	test_small_storage();
	test_sparse_pages();
	test_very_high_sparse_offset();
	test_small_to_paged_promotion();
	test_large_directory();
	test_compression();
	test_adaptive_compression();
	test_encryption();
	test_compression_encryption();
	test_shared_security();
	test_concurrent_files();
	test_allocator_reclaim();

	printf("\nchecks=%d failures=%d => %s\n", g_checks, g_failures, g_failures ? "FAIL" : "PASS");

	return g_failures ? 1 : 0;
}
