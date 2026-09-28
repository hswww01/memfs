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

static void test_write_to_end_semantics(void) {
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t initial[8];
	uint8_t append1[5];
	uint8_t append2[7];
	uint8_t append3[3];
	uint8_t expected[23];
	uint8_t output[32];
	uint32_t transferred;
	uint32_t i;

	printf("== write_to_end semantics ==\n");

	CHECK(memfs_create(8 * 1024 * 1024ULL, L"APPEND", &fs) == MEMFS_OK);
	CHECK(fs != NULL);
	CHECK(memfs_node_create(fs, fs->root, L"append.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);
	CHECK(file != NULL);

	for (i = 0; i < sizeof(initial); i++)
		initial[i] = (uint8_t)(0x10 + i);
	for (i = 0; i < sizeof(append1); i++)
		append1[i] = (uint8_t)(0x20 + i);
	for (i = 0; i < sizeof(append2); i++)
		append2[i] = (uint8_t)(0x30 + i);
	for (i = 0; i < sizeof(append3); i++)
		append3[i] = (uint8_t)(0x40 + i);

	memcpy(expected, initial, sizeof(initial));
	memcpy(expected + sizeof(initial), append1, sizeof(append1));
	memcpy(expected + sizeof(initial) + sizeof(append1), append2, sizeof(append2));
	memcpy(expected + sizeof(initial) + sizeof(append1) + sizeof(append2), append3, sizeof(append3));

	CHECK(memfs_node_write(file, initial, 0, sizeof(initial), false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(initial));
	CHECK(file->file_size == sizeof(initial));
	CHECK(file->allocation_size == sizeof(initial));
	CHECK((uint64_t)fs->used_bytes == sizeof(initial));
	CHECK(memfs_free_bytes(fs) == 8 * 1024 * 1024ULL - sizeof(initial));

	memset(output, 0, sizeof(output));
	CHECK(memfs_node_read(file, output, 0, sizeof(initial), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(initial));
	CHECK(memcmp(initial, output, sizeof(initial)) == 0);

	CHECK(memfs_node_write(file, append1, 0, sizeof(append1), true, false, &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(append1));
	CHECK(file->file_size == sizeof(initial) + sizeof(append1));
	CHECK(file->allocation_size == sizeof(initial) + sizeof(append1));
	CHECK((uint64_t)fs->used_bytes == sizeof(initial) + sizeof(append1));
	CHECK(memfs_free_bytes(fs) == 8 * 1024 * 1024ULL - (sizeof(initial) + sizeof(append1)));

	memset(output, 0, sizeof(output));
	CHECK(memfs_node_read(file, output, 0, sizeof(initial) + sizeof(append1), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(initial) + sizeof(append1));
	CHECK(memcmp(expected, output, sizeof(initial) + sizeof(append1)) == 0);

	CHECK(memfs_node_write(file, append2, 0, sizeof(append2), true, false, &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(append2));
	CHECK(file->file_size == sizeof(initial) + sizeof(append1) + sizeof(append2));
	CHECK(file->allocation_size == sizeof(initial) + sizeof(append1) + sizeof(append2));
	CHECK((uint64_t)fs->used_bytes == sizeof(initial) + sizeof(append1) + sizeof(append2));
	CHECK(memfs_free_bytes(fs) == 8 * 1024 * 1024ULL - (sizeof(initial) + sizeof(append1) + sizeof(append2)));

	CHECK(memfs_node_write(file, append3, 0, sizeof(append3), true, false, &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(append3));
	CHECK(file->file_size == sizeof(expected));
	CHECK(file->allocation_size == sizeof(expected));
	CHECK((uint64_t)fs->used_bytes == sizeof(expected));
	CHECK(memfs_free_bytes(fs) == 8 * 1024 * 1024ULL - sizeof(expected));

	memset(output, 0, sizeof(output));
	CHECK(memfs_node_read(file, output, 0, sizeof(expected), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(expected));
	CHECK(memcmp(expected, output, sizeof(expected)) == 0);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	CHECK(file->deleted);
	CHECK(fs->orphan_head == file);
	CHECK((uint64_t)fs->used_bytes == sizeof(expected));
	CHECK(memfs_free_bytes(fs) == 8 * 1024 * 1024ULL - sizeof(expected));

	memfs_node_close(file);
	CHECK(fs->orphan_head == NULL);
	CHECK((uint64_t)fs->used_bytes == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);
	CHECK(memfs_free_bytes(fs) == 8 * 1024 * 1024ULL);
	CHECK(memfs_resident_bytes(fs) == 0);

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
static void test_no_space_rollback(void) {
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t input[1024];
	uint8_t output[1024];
	uint32_t transferred;
	uint32_t i;
	uint64_t capacity = 1536;
	uint64_t file_size_before;
	uint64_t allocation_size_before;
	uint64_t used_before;
	uint64_t resident_before;
	uint64_t node_resident_before;

	printf("== no space rollback ==\n");

	CHECK(memfs_create(capacity, L"NO_SPACE", &fs) == MEMFS_OK);
	CHECK(fs != NULL);
	CHECK(memfs_node_create(fs, fs->root, L"file", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);
	CHECK(file != NULL);
	if (fs == NULL || file == NULL) {
		if (fs)
			memfs_destroy(fs);
		return;
	}

	for (i = 0; i < sizeof(input); i++)
		input[i] = (uint8_t)(0x10 + (i % 251U));

	CHECK(memfs_node_write(file, input, 0, sizeof(input), false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(input));
	CHECK(file->file_size == sizeof(input));
	CHECK(file->allocation_size == sizeof(input));
	CHECK((uint64_t)fs->used_bytes == sizeof(input));
	CHECK(memfs_free_bytes(fs) == capacity - sizeof(input));

	memset(output, 0xff, sizeof(output));
	CHECK(memfs_node_read(file, output, 0, sizeof(input), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(input));
	CHECK(memcmp(input, output, sizeof(input)) == 0);

	file_size_before = file->file_size;
	allocation_size_before = file->allocation_size;
	used_before = fs->used_bytes;
	resident_before = fs->resident_bytes;
	node_resident_before = memfs_node_resident_bytes(file);

	CHECK(memfs_node_write(file, input, sizeof(input), sizeof(input), false, false, &transferred) == MEMFS_ERR_NO_SPACE);
	CHECK(file->file_size == file_size_before);
	CHECK(file->allocation_size == allocation_size_before);
	CHECK((uint64_t)fs->used_bytes == used_before);
	CHECK((uint64_t)fs->resident_bytes == resident_before);
	CHECK(memfs_node_resident_bytes(file) == node_resident_before);
	CHECK(memfs_free_bytes(fs) == capacity - used_before);

	memset(output, 0xff, sizeof(output));
	CHECK(memfs_node_read(file, output, 0, sizeof(input), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(input));
	CHECK(memcmp(input, output, sizeof(input)) == 0);

	CHECK(memfs_node_set_file_size(file, 2048) == MEMFS_ERR_NO_SPACE);
	CHECK(file->file_size == file_size_before);
	CHECK(file->allocation_size == allocation_size_before);
	CHECK((uint64_t)fs->used_bytes == used_before);
	CHECK((uint64_t)fs->resident_bytes == resident_before);
	CHECK(memfs_node_resident_bytes(file) == node_resident_before);
	CHECK(memfs_free_bytes(fs) == capacity - used_before);

	memset(output, 0xff, sizeof(output));
	CHECK(memfs_node_read(file, output, 0, sizeof(input), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(input));
	CHECK(memcmp(input, output, sizeof(input)) == 0);

	CHECK(memfs_node_set_allocation_size(file, 2048) == MEMFS_OK);
	CHECK(file->file_size == file_size_before);
	CHECK(file->allocation_size == 2048);
	CHECK((uint64_t)fs->used_bytes == used_before);
	CHECK((uint64_t)fs->resident_bytes == resident_before);
	CHECK(memfs_node_resident_bytes(file) == node_resident_before);
	CHECK(memfs_free_bytes(fs) == capacity - used_before);

	memset(output, 0xff, sizeof(output));
	CHECK(memfs_node_read(file, output, 0, sizeof(input), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(input));
	CHECK(memcmp(input, output, sizeof(input)) == 0);

	CHECK(memfs_node_set_allocation_size(file, allocation_size_before) == MEMFS_OK);
	CHECK(file->file_size == file_size_before);
	CHECK(file->allocation_size == allocation_size_before);
	CHECK((uint64_t)fs->used_bytes == used_before);
	CHECK((uint64_t)fs->resident_bytes == resident_before);
	CHECK(memfs_node_resident_bytes(file) == node_resident_before);
	CHECK(memfs_free_bytes(fs) == capacity - used_before);

	memset(output, 0xff, sizeof(output));
	CHECK(memfs_node_read(file, output, 0, sizeof(input), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(input));
	CHECK(memcmp(input, output, sizeof(input)) == 0);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	CHECK((uint64_t)fs->used_bytes == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);
	CHECK(memfs_free_bytes(fs) == capacity);

	CHECK(memfs_node_create(fs, fs->root, L"file", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);
	CHECK(file != NULL);
	if (file == NULL) {
		memfs_destroy(fs);
		return;
	}

	CHECK(memfs_node_write(file, input, 0, sizeof(input), false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(input));
	CHECK(file->file_size == sizeof(input));
	CHECK(file->allocation_size == sizeof(input));
	CHECK((uint64_t)fs->used_bytes == sizeof(input));
	CHECK(memfs_free_bytes(fs) == capacity - sizeof(input));

	memset(output, 0xff, sizeof(output));
	CHECK(memfs_node_read(file, output, 0, sizeof(input), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(input));
	CHECK(memcmp(input, output, sizeof(input)) == 0);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	CHECK((uint64_t)fs->used_bytes == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);
	CHECK(memfs_free_bytes(fs) == capacity);

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

static void test_sparse_multi_group_truncate_reclaim(void) {
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t values[3] = {0x11, 0x22, 0x33};
	uint8_t buffer[32];
	uint32_t transferred;
	uint32_t i;
	uint64_t capacity = 64ULL * 1024ULL * 1024ULL;
	uint64_t offsets[3] = {0x123ULL, 2ULL * MEMFS_PAGE_GROUP_BYTES + 0x456ULL, 4ULL * MEMFS_PAGE_GROUP_BYTES + 0x789ULL};
	uint64_t keep_size = 2ULL * MEMFS_PAGE_GROUP_BYTES + 0x456ULL + 32;
	uint64_t group0 = (offsets[0] >> MEMFS_PAGE_SHIFT) >> MEMFS_PAGE_GROUP_SHIFT;
	uint64_t group1 = (offsets[1] >> MEMFS_PAGE_SHIFT) >> MEMFS_PAGE_GROUP_SHIFT;
	uint64_t group2 = (offsets[2] >> MEMFS_PAGE_SHIFT) >> MEMFS_PAGE_GROUP_SHIFT;
	uint64_t resident_before;
	uint64_t used_before;

	printf("== sparse multi group truncate reclaim ==\n");

	CHECK(memfs_create(capacity, L"SPARSE", &fs) == MEMFS_OK);
	CHECK(fs != NULL);
	CHECK(memfs_node_create(fs, fs->root, L"sparse-multi.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);
	CHECK(file != NULL);
	if (fs == NULL || file == NULL) {
		if (fs)
			memfs_destroy(fs);
		return;
	}

	for (i = 0; i < 3; i++) {
		CHECK(memfs_node_write(file, &values[i], offsets[i], 1, false, false, &transferred) == MEMFS_OK);
		CHECK(transferred == 1);
	}

	CHECK(memfs_node_page_group_count(file) == 3);
	CHECK(memfs_node_page_group_capacity(file) >= 3);
	CHECK(memfs_node_page_group_index(file, 0) == group0);
	CHECK(memfs_node_page_group_index(file, 1) == group1);
	CHECK(memfs_node_page_group_index(file, 2) == group2);
	CHECK(memfs_node_resident_bytes(file) > 0);
	CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));
	resident_before = memfs_node_resident_bytes(file);
	used_before = fs->used_bytes;

	memset(buffer, 0xcc, sizeof(buffer));
	CHECK(memfs_node_read(file, buffer, offsets[0] - 8, sizeof(buffer), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(buffer));
	CHECK(buffer[8] == values[0]);
	for (i = 0; i < sizeof(buffer); i++) {
		if (i != 8)
			CHECK(buffer[i] == 0);
	}

	memset(buffer, 0xcc, sizeof(buffer));
	CHECK(memfs_node_read(file, buffer, offsets[1] - 8, sizeof(buffer), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(buffer));
	CHECK(buffer[8] == values[1]);

	CHECK(memfs_node_set_file_size(file, keep_size) == MEMFS_OK);
	CHECK(file->file_size == keep_size);
	CHECK(memfs_node_set_allocation_size(file, keep_size) == MEMFS_OK);
	CHECK(file->allocation_size == keep_size);
	CHECK((uint64_t)fs->used_bytes < used_before);
	CHECK(memfs_node_page_group_count(file) == 2);
	CHECK(memfs_node_page_group_capacity(file) >= 2);
	CHECK(memfs_node_page_group_index(file, 0) == group0);
	CHECK(memfs_node_page_group_index(file, 1) == group1);
	CHECK(memfs_node_resident_bytes(file) < resident_before);
	CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));

	memset(buffer, 0xcc, sizeof(buffer));
	CHECK(memfs_node_read(file, buffer, offsets[0] - 8, sizeof(buffer), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(buffer));
	CHECK(buffer[8] == values[0]);
	for (i = 0; i < sizeof(buffer); i++) {
		if (i != 8)
			CHECK(buffer[i] == 0);
	}

	memset(buffer, 0xcc, sizeof(buffer));
	CHECK(memfs_node_read(file, buffer, offsets[1] - 8, sizeof(buffer), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(buffer));
	CHECK(buffer[8] == values[1]);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	CHECK((uint64_t)fs->used_bytes == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);
	CHECK(memfs_free_bytes(fs) == capacity);
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
static void test_truncate_regrow_zero_fill(void) {
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t input[2048];
	uint8_t output[2048];
	uint8_t paged_input[3 * MEMFS_PAGE_SIZE + 123];
	uint8_t paged_output[3 * MEMFS_PAGE_SIZE + 123];
	uint32_t transferred;
	uint32_t i;
	uint64_t capacity = 8ULL * 1024ULL * 1024ULL;

	printf("== truncate/regrow zero fill ==\n");

	CHECK(memfs_create(capacity, L"TRUNC", &fs) == MEMFS_OK);
	if (fs == NULL)
		return;

	for (i = 0; i < sizeof(input); i++)
		input[i] = (uint8_t)(0x11 + (i % 251U));

	CHECK(memfs_node_create(fs, fs->root, L"tiny-regrow.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);
	CHECK(file != NULL);
	if (file == NULL) {
		memfs_destroy(fs);
		return;
	}

	CHECK(memfs_node_write(file, input, 0, sizeof(input), false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(input));
	CHECK(file->file_size == sizeof(input));
	CHECK(file->allocation_size == sizeof(input));
	CHECK(!file->paged_storage);
	CHECK(memfs_node_page_group_count(file) == 0);
	CHECK((uint64_t)fs->used_bytes == sizeof(input));
	CHECK(memfs_free_bytes(fs) == capacity - sizeof(input));
	CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));

	CHECK(memfs_node_set_file_size(file, 512) == MEMFS_OK);
	CHECK(file->file_size == 512);
	CHECK((uint64_t)fs->used_bytes == 512);
	CHECK(memfs_free_bytes(fs) == capacity - 512);
	CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));

	CHECK(memfs_node_set_file_size(file, sizeof(input)) == MEMFS_OK);
	CHECK(file->file_size == sizeof(input));
	CHECK(file->allocation_size == sizeof(input));
	CHECK((uint64_t)fs->used_bytes == sizeof(input));
	CHECK(memfs_free_bytes(fs) == capacity - sizeof(input));
	CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));

	memset(output, 0xff, sizeof(output));
	CHECK(memfs_node_read(file, output, 512, sizeof(input) - 512, &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(input) - 512);
	for (i = 0; i < sizeof(input) - 512; i++)
		CHECK(output[i] == 0);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	CHECK((uint64_t)fs->used_bytes == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);
	CHECK(memfs_free_bytes(fs) == capacity);

	for (i = 0; i < sizeof(paged_input); i++)
		paged_input[i] = (uint8_t)(0x22 + (i % 251U));

	CHECK(memfs_node_create(fs, fs->root, L"paged-regrow.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);
	CHECK(file != NULL);
	if (file == NULL) {
		memfs_destroy(fs);
		return;
	}

	CHECK(memfs_node_write(file, paged_input, 0, sizeof(paged_input), false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(paged_input));
	CHECK(file->file_size == sizeof(paged_input));
	CHECK(file->allocation_size == sizeof(paged_input));
	CHECK(!file->small_inline);
	CHECK(file->paged_storage);
	CHECK(memfs_node_page_group_count(file) != 0);
	CHECK((uint64_t)fs->used_bytes == sizeof(paged_input));
	CHECK(memfs_free_bytes(fs) == capacity - sizeof(paged_input));
	CHECK(memfs_node_resident_bytes(file) >= 3ULL * MEMFS_PAGE_SIZE);
	CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));

	CHECK(memfs_node_set_file_size(file, MEMFS_PAGE_SIZE) == MEMFS_OK);
	CHECK(file->file_size == MEMFS_PAGE_SIZE);
	CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));

	CHECK(memfs_node_set_allocation_size(file, MEMFS_PAGE_SIZE) == MEMFS_OK);
	CHECK(file->allocation_size == MEMFS_PAGE_SIZE);
	CHECK((uint64_t)fs->used_bytes == MEMFS_PAGE_SIZE);
	CHECK(memfs_free_bytes(fs) == capacity - MEMFS_PAGE_SIZE);
	CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));

	CHECK(memfs_node_set_file_size(file, sizeof(paged_input)) == MEMFS_OK);
	CHECK(file->file_size == sizeof(paged_input));
	CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));

	CHECK(memfs_node_set_allocation_size(file, sizeof(paged_input)) == MEMFS_OK);
	CHECK(file->allocation_size == sizeof(paged_input));
	CHECK((uint64_t)fs->used_bytes == sizeof(paged_input));
	CHECK(memfs_free_bytes(fs) == capacity - sizeof(paged_input));

	memset(paged_output, 0xff, sizeof(paged_output));
	CHECK(memfs_node_read(file, paged_output, MEMFS_PAGE_SIZE, sizeof(paged_input) - MEMFS_PAGE_SIZE, &transferred) ==
		  MEMFS_OK);
	CHECK(transferred == sizeof(paged_input) - MEMFS_PAGE_SIZE);
	for (i = 0; i < sizeof(paged_input) - MEMFS_PAGE_SIZE; i++)
		CHECK(paged_output[i] == 0);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	CHECK((uint64_t)fs->used_bytes == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);
	CHECK(memfs_free_bytes(fs) == capacity);

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
	if (fs->root->dir->hash)
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

static bool test_orphan_contains(const Memfs* fs, const MemfsNode* target);

static void test_large_directory_case_insensitive_hash_stress(void) {
	enum { COUNT = 1024 };
	Memfs* fs = NULL;
	MemfsNode** nodes;
	MemfsNode* node;
	wchar_t name[64];
	wchar_t variant[64];
	wchar_t new_name[64];
	uint32_t i;

	printf("== large directory case-insensitive hash stress ==\n");

	nodes = calloc(COUNT, sizeof(*nodes));
	CHECK(nodes != NULL);
	CHECK(memfs_create(64ULL * 1024ULL * 1024ULL, L"CASE", &fs) == MEMFS_OK);
	if (nodes == NULL || fs == NULL)
		goto exit;

	for (i = 0; i < COUNT; i++) {
		switch (i % 3U) {
		case 0:
			swprintf_s(name, _countof(name), L"Case-%04u.txt", i);
			break;
		case 1:
			swprintf_s(name, _countof(name), L"CASE-%04u.TXT", i);
			break;
		default:
			swprintf_s(name, _countof(name), L"case-%04u.Txt", i);
			break;
		}
		CHECK(memfs_node_create(fs, fs->root, name, false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &nodes[i]) == MEMFS_OK);
	}

	CHECK(fs->root->dir->child_count == COUNT);
	if (fs->root->dir->hash)
		CHECK(fs->root->dir->hash->count == COUNT);

	for (i = 0; i < COUNT; i++) {
		switch (i % 3U) {
		case 0:
			swprintf_s(variant, _countof(variant), L"case-%04u.TXT", i);
			break;
		case 1:
			swprintf_s(variant, _countof(variant), L"Case-%04u.txt", i);
			break;
		default:
			swprintf_s(variant, _countof(variant), L"CASE-%04u.TXT", i);
			break;
		}
		CHECK(memfs_dir_lookup(fs->root, variant) == nodes[i]);
	}

	swprintf_s(name, _countof(name), L"missing-%04u.txt", COUNT);
	CHECK(memfs_dir_lookup(fs->root, name) == NULL);

	for (i = 0; i < COUNT; i += 128U) {
		switch (i % 3U) {
		case 0:
			swprintf_s(name, _countof(name), L"case-%04u.TXT", i);
			break;
		case 1:
			swprintf_s(name, _countof(name), L"Case-%04u.txt", i);
			break;
		default:
			swprintf_s(name, _countof(name), L"CASE-%04u.TXT", i);
			break;
		}
		CHECK(memfs_node_create(fs, fs->root, name, false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &node) == MEMFS_ERR_EXISTS);
	}
	CHECK(fs->root->dir->child_count == COUNT);
	if (fs->root->dir->hash)
		CHECK(fs->root->dir->hash->count == COUNT);

	for (i = 0; i < COUNT; i += 256U) {
		switch (i % 3U) {
		case 0:
			swprintf_s(new_name, _countof(new_name), L"RENAMED-%04u.txt", i);
			break;
		case 1:
			swprintf_s(new_name, _countof(new_name), L"Renamed-%04u.TXT", i);
			break;
		default:
			swprintf_s(new_name, _countof(new_name), L"renamed-%04u.Txt", i);
			break;
		}
		CHECK(memfs_node_rename(nodes[i], fs->root, new_name, false) == MEMFS_OK);
		CHECK(wcscmp(nodes[i]->name, new_name) == 0);
		CHECK(memfs_dir_lookup(fs->root, new_name) == nodes[i]);
	}

	for (i = 0; i < COUNT; i += 256U) {
		switch (i % 3U) {
		case 0:
			swprintf_s(name, _countof(name), L"Case-%04u.txt", i);
			break;
		case 1:
			swprintf_s(name, _countof(name), L"CASE-%04u.TXT", i);
			break;
		default:
			swprintf_s(name, _countof(name), L"case-%04u.Txt", i);
			break;
		}
		CHECK(memfs_dir_lookup(fs->root, name) == NULL);
	}

	for (i = 0; i < COUNT; i += 512U) {
		memfs_node_open(nodes[i]);
		CHECK(memfs_node_unlink(nodes[i]) == MEMFS_OK);
		CHECK(test_orphan_contains(fs, nodes[i]));
	}

	for (i = 0; i < COUNT; i += 512U) {
		switch (i % 3U) {
		case 0:
			swprintf_s(name, _countof(name), L"RENAMED-%04u.txt", i);
			break;
		case 1:
			swprintf_s(name, _countof(name), L"Renamed-%04u.TXT", i);
			break;
		default:
			swprintf_s(name, _countof(name), L"renamed-%04u.Txt", i);
			break;
		}
		CHECK(memfs_dir_lookup(fs->root, name) == NULL);
	}

	CHECK(fs->root->dir->child_count == COUNT - COUNT / 512U);
	if (fs->root->dir->hash)
		CHECK(fs->root->dir->hash->count == COUNT - COUNT / 512U);

	for (i = 0; i < COUNT; i++) {
		if (nodes[i] == NULL)
			continue;
		if (i % 512U == 0U) {
			memfs_node_close(nodes[i]);
			memfs_node_close(nodes[i]);
			nodes[i] = NULL;
		} else {
			CHECK(memfs_node_unlink(nodes[i]) == MEMFS_OK);
			memfs_node_close(nodes[i]);
			nodes[i] = NULL;
		}
	}

	CHECK(fs->root->dir->child_count == 0);
	if (fs->root->dir->hash)
		CHECK(fs->root->dir->hash->count == 0);
	CHECK((uint64_t)fs->used_bytes == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);
	CHECK(fs->orphan_head == NULL);

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

static void test_compression_incompressible_page_fallback(void) {
	MemfsOptions options = {0};
	Memfs* fs = NULL;
	MemfsNode* file;
	MemfsPageGroup* group;
	uint8_t page[MEMFS_PAGE_SIZE];
	uint8_t page2[MEMFS_PAGE_SIZE];
	uint8_t verify[MEMFS_PAGE_SIZE];
	uint32_t transferred;
	uint32_t state;
	uint32_t i;

	printf("== compression incompressible page fallback ==\n");

	options.capacity = 16ULL * 1024ULL * 1024ULL;
	options.volume_label = L"COMPFB";
	options.compression_enabled = true;
	options.compression_level = 1;

	CHECK(memfs_create_ex(&options, &fs) == MEMFS_OK);
	CHECK(fs != NULL);
	CHECK(memfs_node_create(fs, fs->root, L"incompressible.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) ==
		  MEMFS_OK);
	CHECK(file != NULL);

	state = 0x9e3779b9U;
	for (i = 0; i < sizeof(page); i += sizeof(state)) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		memcpy(page + i, &state, sizeof(state));
	}

	CHECK(memfs_node_write(file, page, 0, MEMFS_PAGE_SIZE, false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == MEMFS_PAGE_SIZE);
	CHECK(file->file_size == MEMFS_PAGE_SIZE);
	CHECK(file->allocation_size == MEMFS_PAGE_SIZE);

	group = memfs_node_page_group(file, 0);
	if (group) {
		CHECK(group->page_count == 1);
		CHECK(group->pages[0] != NULL);
		if (group->pages[0]) {
			CHECK(0 == (group->pages[0]->flags & MEMFS_PAGE_COMPRESSED));
			CHECK(group->pages[0]->plain_size == MEMFS_PAGE_SIZE);
			CHECK(group->pages[0]->stored_size >= MEMFS_PAGE_SIZE);
		}
	}

	CHECK(memfs_node_resident_bytes(file) >= MEMFS_PAGE_SIZE);
	CHECK(memfs_node_resident_bytes(file) < MEMFS_PAGE_SIZE + 64U);
	CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));
	CHECK((uint64_t)fs->used_bytes == MEMFS_PAGE_SIZE);
	CHECK(memfs_free_bytes(fs) == options.capacity - MEMFS_PAGE_SIZE);

	memset(verify, 0, sizeof(verify));
	CHECK(memfs_node_read(file, verify, 0, MEMFS_PAGE_SIZE, &transferred) == MEMFS_OK);
	CHECK(transferred == MEMFS_PAGE_SIZE);
	CHECK(memcmp(page, verify, MEMFS_PAGE_SIZE) == 0);

	state = 0x85ebca6bU;
	for (i = 0; i < sizeof(page2); i += sizeof(state)) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		memcpy(page2 + i, &state, sizeof(state));
	}

	CHECK(memfs_node_write(file, page2, 0, MEMFS_PAGE_SIZE, false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == MEMFS_PAGE_SIZE);
	CHECK(file->file_size == MEMFS_PAGE_SIZE);
	CHECK(file->allocation_size == MEMFS_PAGE_SIZE);

	group = memfs_node_page_group(file, 0);
	if (group) {
		CHECK(group->page_count == 1);
		CHECK(group->pages[0] != NULL);
		if (group->pages[0])
			CHECK(0 == (group->pages[0]->flags & MEMFS_PAGE_COMPRESSED));
	}

	memset(verify, 0, sizeof(verify));
	CHECK(memfs_node_read(file, verify, 0, MEMFS_PAGE_SIZE, &transferred) == MEMFS_OK);
	CHECK(transferred == MEMFS_PAGE_SIZE);
	CHECK(memcmp(page2, verify, MEMFS_PAGE_SIZE) == 0);

	CHECK(memfs_node_resident_bytes(file) < 2ULL * MEMFS_PAGE_SIZE);
	CHECK((uint64_t)fs->used_bytes == MEMFS_PAGE_SIZE);
	CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	CHECK((uint64_t)fs->used_bytes == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);
	CHECK(memfs_free_bytes(fs) == options.capacity);
	CHECK(memfs_resident_bytes(fs) == 0);

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

static void test_encryption_rewrite_truncate_regrow(void) {
	MemfsOptions options = {0};
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t key[MEMFS_ENCRYPTION_KEY_SIZE];
	uint8_t input[3 * MEMFS_PAGE_SIZE + 123];
	uint8_t output[3 * MEMFS_PAGE_SIZE + 123];
	uint32_t transferred;
	uint32_t i;
	uint32_t round;
	uint64_t regrow_size = sizeof(input);
	uint64_t smaller_size = MEMFS_PAGE_SIZE;

	printf("== encryption rewrite/truncate/regrow integrity ==\n");

	for (i = 0; i < sizeof(key); i++)
		key[i] = (uint8_t)(0x5aU + i * 11U);

	options.capacity = 64ULL * 1024ULL * 1024ULL;
	options.volume_label = L"CRYPTRW";
	options.encryption_enabled = true;
	options.encryption_key = key;
	options.encryption_key_size = sizeof(key);

	CHECK(memfs_create_ex(&options, &fs) == MEMFS_OK);
	CHECK(fs != NULL);
	CHECK(memfs_node_create(fs, fs->root, L"crypt-rewrite.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);
	CHECK(file != NULL);
	if (fs == NULL || file == NULL) {
		if (fs)
			memfs_destroy(fs);
		return;
	}

	for (round = 0; round < 4; round++) {
		for (i = 0; i < sizeof(input); i++)
			input[i] = (uint8_t)(0x10U + round * 17U + (i % 251U));

		CHECK(memfs_node_write(file, input, 0, sizeof(input), false, false, &transferred) == MEMFS_OK);
		CHECK(transferred == sizeof(input));
		CHECK(file->file_size == regrow_size);
		CHECK(file->allocation_size == regrow_size);
		CHECK((uint64_t)fs->used_bytes == regrow_size);
		CHECK(memfs_free_bytes(fs) == options.capacity - regrow_size);
		CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));

		memset(output, 0xff, sizeof(output));
		CHECK(memfs_node_read(file, output, 0, sizeof(output), &transferred) == MEMFS_OK);
		CHECK(transferred == sizeof(output));
		CHECK(memcmp(input, output, sizeof(input)) == 0);

		CHECK(memfs_node_set_file_size(file, smaller_size) == MEMFS_OK);
		CHECK(file->file_size == smaller_size);
		CHECK(memfs_node_set_allocation_size(file, smaller_size) == MEMFS_OK);
		CHECK(file->allocation_size == smaller_size);
		CHECK((uint64_t)fs->used_bytes == smaller_size);
		CHECK(memfs_free_bytes(fs) == options.capacity - smaller_size);
		CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));

		memset(output, 0xff, sizeof(output));
		CHECK(memfs_node_read(file, output, 0, smaller_size, &transferred) == MEMFS_OK);
		CHECK(transferred == (uint32_t)smaller_size);
		CHECK(memcmp(input, output, smaller_size) == 0);

		CHECK(memfs_node_set_file_size(file, regrow_size) == MEMFS_OK);
		CHECK(file->file_size == regrow_size);
		CHECK(memfs_node_set_allocation_size(file, regrow_size) == MEMFS_OK);
		CHECK(file->allocation_size == regrow_size);
		CHECK((uint64_t)fs->used_bytes == regrow_size);
		CHECK(memfs_free_bytes(fs) == options.capacity - regrow_size);
		CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));

		memset(output, 0xff, sizeof(output));
		CHECK(memfs_node_read(file, output, smaller_size, regrow_size - smaller_size, &transferred) == MEMFS_OK);
		CHECK(transferred == (uint32_t)(regrow_size - smaller_size));
		for (i = 0; i < regrow_size - smaller_size; i++)
			CHECK(output[i] == 0);
	}

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	CHECK((uint64_t)fs->used_bytes == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);
	CHECK(memfs_free_bytes(fs) == options.capacity);
	CHECK(memfs_resident_bytes(fs) == 0);

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

static void test_compression_encryption_sparse_rewrite_truncate_regrow(void) {
	MemfsOptions options = {0};
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t key[MEMFS_ENCRYPTION_KEY_SIZE];
	uint8_t output[1024];
	uint32_t transferred;
	uint32_t i;
	uint32_t round;
	uint64_t capacity = 256ULL * 1024ULL * 1024ULL;
	uint64_t offsets[3] = {0x123ULL, 2ULL * MEMFS_PAGE_GROUP_BYTES + 0x456ULL, 4ULL * MEMFS_PAGE_GROUP_BYTES + 0x789ULL};
	uint64_t keep_size = 2ULL * MEMFS_PAGE_GROUP_BYTES + 0x456ULL + 128;
	uint64_t regrow_size = 4ULL * MEMFS_PAGE_GROUP_BYTES + 0x789ULL + 128;
	uint64_t used_before;
	uint64_t resident_before;

	printf("== compression + encryption sparse rewrite/truncate/regrow ==\n");

	for (i = 0; i < sizeof(key); i++)
		key[i] = (uint8_t)(0x77U + i * 13U);

	options.capacity = capacity;
	options.volume_label = L"COMPCRYPT";
	options.compression_enabled = true;
	options.compression_level = 1;
	options.encryption_enabled = true;
	options.encryption_key = key;
	options.encryption_key_size = sizeof(key);

	CHECK(memfs_create_ex(&options, &fs) == MEMFS_OK);
	CHECK(fs != NULL);
	CHECK(memfs_node_create(fs, fs->root, L"comp-crypt-sparse.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) ==
		  MEMFS_OK);
	CHECK(file != NULL);
	if (fs == NULL || file == NULL) {
		if (fs)
			memfs_destroy(fs);
		return;
	}

	for (round = 0; round < 4; round++) {
		uint8_t sparse_data[3][128];
		uint8_t rewrite_data[3][128];
		uint32_t j;

		for (i = 0; i < 3; i++) {
			uint64_t offset = offsets[i];
			uint64_t end = offset + sizeof(sparse_data[i]);

			for (j = 0; j < sizeof(sparse_data[i]); j++)
				sparse_data[i][j] = (uint8_t)(0x20U + round * 29U + (j % 191U) + j * 3U);

			CHECK(memfs_node_write(file, sparse_data[i], offset, sizeof(sparse_data[i]), false, false, &transferred) ==
				  MEMFS_OK);
			CHECK(transferred == sizeof(sparse_data[i]));
			CHECK(file->file_size >= end);
			CHECK(file->allocation_size >= end);
			CHECK((uint64_t)fs->used_bytes == file->file_size);
			CHECK(memfs_free_bytes(fs) == capacity - file->file_size);
			CHECK(memfs_node_resident_bytes(file) > 0);
			CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));
		}

		CHECK(memfs_node_page_group_count(file) == 3);
		CHECK(memfs_node_page_group_index(file, 0) == (offsets[0] >> MEMFS_PAGE_SHIFT) >> MEMFS_PAGE_GROUP_SHIFT);
		CHECK(memfs_node_page_group_index(file, 1) == (offsets[1] >> MEMFS_PAGE_SHIFT) >> MEMFS_PAGE_GROUP_SHIFT);
		CHECK(memfs_node_page_group_index(file, 2) == (offsets[2] >> MEMFS_PAGE_SHIFT) >> MEMFS_PAGE_GROUP_SHIFT);

		for (i = 0; i < 3; i++) {
			memset(output, 0xcc, sizeof(output));
			CHECK(memfs_node_read(file, output, offsets[i], sizeof(sparse_data[i]), &transferred) == MEMFS_OK);
			CHECK(transferred == sizeof(sparse_data[i]));
			CHECK(memcmp(sparse_data[i], output, sizeof(sparse_data[i])) == 0);
		}

		used_before = fs->used_bytes;
		resident_before = memfs_node_resident_bytes(file);

		for (i = 0; i < 3; i++) {
			for (j = 0; j < sizeof(rewrite_data[i]); j++)
				rewrite_data[i][j] = (uint8_t)(0x40U + round * 31U + (j % 211U) + j * 5U);

			CHECK(memfs_node_write(file, rewrite_data[i], offsets[i], sizeof(rewrite_data[i]), false, false,
								   &transferred) == MEMFS_OK);
			CHECK(transferred == sizeof(rewrite_data[i]));
		}

		CHECK(file->file_size >= regrow_size);
		CHECK((uint64_t)fs->used_bytes == file->file_size);
		CHECK(memfs_free_bytes(fs) == capacity - file->file_size);
		CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));

		for (i = 0; i < 3; i++) {
			memset(output, 0xcc, sizeof(output));
			CHECK(memfs_node_read(file, output, offsets[i], sizeof(rewrite_data[i]), &transferred) == MEMFS_OK);
			CHECK(transferred == sizeof(rewrite_data[i]));
			CHECK(memcmp(rewrite_data[i], output, sizeof(rewrite_data[i])) == 0);
		}

		CHECK(memfs_node_set_file_size(file, keep_size) == MEMFS_OK);
		CHECK(file->file_size == keep_size);
		CHECK(memfs_node_set_allocation_size(file, keep_size) == MEMFS_OK);
		CHECK(file->allocation_size == keep_size);
		CHECK((uint64_t)fs->used_bytes == keep_size);
		CHECK(memfs_free_bytes(fs) == capacity - keep_size);
		CHECK((uint64_t)fs->used_bytes < used_before);
		CHECK(memfs_node_resident_bytes(file) < resident_before);
		CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));
		CHECK(memfs_node_page_group_count(file) == 2);
		CHECK(memfs_node_page_group_index(file, 0) == (offsets[0] >> MEMFS_PAGE_SHIFT) >> MEMFS_PAGE_GROUP_SHIFT);
		CHECK(memfs_node_page_group_index(file, 1) == (offsets[1] >> MEMFS_PAGE_SHIFT) >> MEMFS_PAGE_GROUP_SHIFT);

		for (i = 0; i < 2; i++) {
			memset(output, 0xcc, sizeof(output));
			CHECK(memfs_node_read(file, output, offsets[i], sizeof(rewrite_data[i]), &transferred) == MEMFS_OK);
			CHECK(transferred == sizeof(rewrite_data[i]));
			CHECK(memcmp(rewrite_data[i], output, sizeof(rewrite_data[i])) == 0);
		}

		CHECK(memfs_node_set_file_size(file, regrow_size) == MEMFS_OK);
		CHECK(file->file_size == regrow_size);
		CHECK(memfs_node_set_allocation_size(file, regrow_size) == MEMFS_OK);
		CHECK(file->allocation_size == regrow_size);
		CHECK((uint64_t)fs->used_bytes == regrow_size);
		CHECK(memfs_free_bytes(fs) == capacity - regrow_size);
		CHECK((uint64_t)fs->resident_bytes == memfs_node_resident_bytes(file));

		for (i = 0; i < 2; i++) {
			memset(output, 0xcc, sizeof(output));
			CHECK(memfs_node_read(file, output, offsets[i], sizeof(rewrite_data[i]), &transferred) == MEMFS_OK);
			CHECK(transferred == sizeof(rewrite_data[i]));
			CHECK(memcmp(rewrite_data[i], output, sizeof(rewrite_data[i])) == 0);
		}

		memset(output, 0xff, sizeof(output));
		CHECK(memfs_node_read(file, output, keep_size, sizeof(output), &transferred) == MEMFS_OK);
		CHECK(transferred == sizeof(output));
		for (i = 0; i < sizeof(output); i++)
			CHECK(output[i] == 0);
	}

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	CHECK((uint64_t)fs->used_bytes == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);
	CHECK(memfs_free_bytes(fs) == capacity);
	CHECK(memfs_resident_bytes(fs) == 0);

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


static void test_allocator_stress(void) {
	enum { FILES = 4096, ROUNDS = 8 };
	Memfs* fs = NULL;
	MemfsNode* node;
	MemfsAllocatorStats before;
	MemfsAllocatorStats after;
	uint32_t round;
	uint32_t i;
	wchar_t name[32];

	printf("== allocator stress ==\n");

	CHECK(memfs_create(64ULL * 1024ULL * 1024ULL, L"STRESS", &fs) == MEMFS_OK);
	if (fs == NULL)
		return;

	for (round = 0; round < ROUNDS; round++) {
		for (i = 0; i < FILES; i++) {
			swprintf_s(name, _countof(name), L"stress-%03u-%04u", round, i);
			CHECK(memfs_node_create(fs, fs->root, name, false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &node) == MEMFS_OK);
			if (node) {
				CHECK(memfs_node_unlink(node) == MEMFS_OK);
				memfs_node_close(node);
			}
		}
	}

	memfs_allocator_get_stats(&fs->allocator, &before);
	CHECK(before.live_objects > 0);
	CHECK(before.reserved_bytes > 0);

	for (i = 0; i < FILES; i++) {
		swprintf_s(name, _countof(name), L"stress-%03u-%04u", ROUNDS - 1U, i);
		node = memfs_dir_lookup(fs->root, name);
		CHECK(node == NULL);
	}

	memfs_allocator_get_stats(&fs->allocator, &after);
	CHECK(after.live_objects <= before.live_objects);
	CHECK(after.reserved_bytes <= before.reserved_bytes);
	CHECK((uint64_t)fs->used_bytes == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);

	memfs_destroy(fs);
}

static void test_open_delete_lifetime(void) {
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t data = 0x42;
	uint8_t out = 0;
	uint32_t transferred;
	uint32_t i;

	printf("== open/delete lifetime ==\n");

	CHECK(memfs_create(8ULL * 1024ULL * 1024ULL, L"LIFE", &fs) == MEMFS_OK);

	for (i = 0; i < 1000; i++) {
		CHECK(memfs_node_create(fs, fs->root, L"held.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);
		memfs_node_open(file); // simulate an additional WinFsp handle
		CHECK(memfs_node_write(file, &data, 0, 1, false, false, &transferred) == MEMFS_OK);
		CHECK(transferred == 1);
		CHECK(memfs_node_unlink(file) == MEMFS_OK);
		CHECK(fs->orphan_head == file);
		CHECK(memfs_node_read(file, &out, 0, 1, &transferred) == MEMFS_OK);
		CHECK(out == data);

		memfs_node_close(file); // close extra handle
		CHECK(fs->orphan_head == file);
		memfs_node_close(file); // close create handle, final free
		CHECK(fs->orphan_head == NULL);
	}

	memfs_destroy(fs);
}



static void test_rename_delete_lifetime(void) {
	Memfs* fs = NULL;
	MemfsNode* dir;
	MemfsNode* file;
	MemfsNode* found;
	uint8_t data = 0x77;
	uint8_t out = 0;
	uint32_t transferred;

	printf("== rename/delete lifetime ==\n");

	CHECK(memfs_create(8ULL * 1024ULL * 1024ULL, L"RLIFE", &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"dir", true, FILE_ATTRIBUTE_DIRECTORY, NULL, 0, &dir) == MEMFS_OK);
	CHECK(memfs_node_create(fs, dir, L"old.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);

	memfs_node_open(file);
	CHECK(memfs_node_write(file, &data, 0, 1, false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == 1);

	CHECK(memfs_node_rename(file, fs->root, L"new.bin", false) == MEMFS_OK);
	CHECK(memfs_lookup_path(fs, L"\\dir\\old.bin", &found) == MEMFS_ERR_NOT_FOUND);
	CHECK(memfs_lookup_path(fs, L"\\new.bin", &found) == MEMFS_OK);
	CHECK(found == file);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	CHECK(fs->orphan_head == file);
	CHECK(memfs_node_read(file, &out, 0, 1, &transferred) == MEMFS_OK);
	CHECK(out == data);
	CHECK(memfs_node_write(file, &data, 0, 1, false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == 1);

	memfs_node_close(file);
	CHECK(fs->orphan_head == file);
	memfs_node_close(file);
	CHECK(fs->orphan_head == NULL);

	CHECK(memfs_node_unlink(dir) == MEMFS_OK);
	memfs_node_close(dir);
	memfs_destroy(fs);
}
static bool test_orphan_contains(const Memfs* fs, const MemfsNode* target) {
	const MemfsNode* node;

	for (node = fs ? fs->orphan_head : NULL; node; node = node->tree_right) {
		if (node == target)
			return true;
	}

	return false;
}

static void test_winfsp_open_rename_delete_close_order(void) {
	Memfs* fs = NULL;
	MemfsNode* dir;
	MemfsNode* file;
	MemfsNode* found;
	uint8_t data = 0x3c;
	uint8_t out = 0;
	uint32_t transferred;

	printf("== WinFsp open/rename/delete/close order ==\n");

	CHECK(memfs_create(8ULL * 1024ULL * 1024ULL, L"WINFSP", &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"dir", true, FILE_ATTRIBUTE_DIRECTORY, NULL, 0, &dir) == MEMFS_OK);
	CHECK(memfs_node_create(fs, dir, L"open.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);

	// WinFsp Create/Open returns FileContext with open_count = 1.
	CHECK(file->open_count == 1);
	CHECK(memfs_node_write(file, &data, 0, 1, false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == 1);

	// Rename while open: namespace moves, FileContext remains valid.
	CHECK(memfs_node_rename(file, fs->root, L"renamed.bin", false) == MEMFS_OK);
	CHECK(memfs_lookup_path(fs, L"\\dir\\open.bin", &found) == MEMFS_ERR_NOT_FOUND);
	CHECK(memfs_lookup_path(fs, L"\\renamed.bin", &found) == MEMFS_OK);
	CHECK(found == file);
	CHECK(wcscmp(file->name, L"renamed.bin") == 0);
	CHECK(file->open_count == 1);
	CHECK(memfs_node_read(file, &out, 0, 1, &transferred) == MEMFS_OK);
	CHECK(out == data);

	// Delete-on-cleanup removes the namespace entry but keeps the open FileContext alive.
	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	CHECK(file->deleted);
	CHECK(test_orphan_contains(fs, file));
	CHECK(memfs_lookup_path(fs, L"\\renamed.bin", &found) == MEMFS_ERR_NOT_FOUND);
	CHECK(memfs_node_read(file, &out, 0, 1, &transferred) == MEMFS_OK);
	CHECK(out == data);
	CHECK(memfs_node_write(file, &data, 0, 1, false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == 1);

	// Final close releases the orphan node.
	memfs_node_close(file);
	CHECK(fs->orphan_head == NULL);
	CHECK((uint64_t)fs->used_bytes == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);

	CHECK(memfs_node_unlink(dir) == MEMFS_OK);
	memfs_node_close(dir);
	memfs_destroy(fs);
}
static void test_winfsp_open_rename_replace_delete_close_order(void) {
	Memfs* fs = NULL;
	MemfsNode* dir;
	MemfsNode* old_file;
	MemfsNode* new_file;
	MemfsNode* found;
	uint8_t old_data = 0x11;
	uint8_t new_data = 0x22;
	uint8_t out = 0;
	uint32_t transferred;

	printf("== WinFsp open/rename-replace/delete/close order ==\n");

	CHECK(memfs_create(8ULL * 1024ULL * 1024ULL, L"WINFSP", &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"dir", true, FILE_ATTRIBUTE_DIRECTORY, NULL, 0, &dir) == MEMFS_OK);
	CHECK(memfs_node_create(fs, dir, L"target.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &old_file) == MEMFS_OK);
	CHECK(memfs_node_create(fs, dir, L"source.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &new_file) == MEMFS_OK);

	CHECK(memfs_node_write(old_file, &old_data, 0, 1, false, false, &transferred) == MEMFS_OK);
	CHECK(memfs_node_write(new_file, &new_data, 0, 1, false, false, &transferred) == MEMFS_OK);

	// Open both handles before rename.
	memfs_node_open(old_file);
	memfs_node_open(new_file);
	CHECK(old_file->open_count == 2);
	CHECK(new_file->open_count == 2);

	// Replace rename: source becomes target, old target becomes orphan while open.
	CHECK(memfs_node_rename(new_file, dir, L"target.bin", true) == MEMFS_OK);
	CHECK(memfs_lookup_path(fs, L"\\dir\\source.bin", &found) == MEMFS_ERR_NOT_FOUND);
	CHECK(memfs_lookup_path(fs, L"\\dir\\target.bin", &found) == MEMFS_OK);
	CHECK(found == new_file);
	CHECK(old_file->deleted);
	CHECK(test_orphan_contains(fs, old_file));

	// Old open handle still reads/writes its orphaned storage.
	CHECK(memfs_node_read(old_file, &out, 0, 1, &transferred) == MEMFS_OK);
	CHECK(out == old_data);
	CHECK(memfs_node_write(old_file, &old_data, 0, 1, false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == 1);

	// New open handle reads/writes the renamed storage.
	CHECK(memfs_node_read(new_file, &out, 0, 1, &transferred) == MEMFS_OK);
	CHECK(out == new_data);
	CHECK(memfs_node_write(new_file, &new_data, 0, 1, false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == 1);

	// Delete the new target while both handles remain open.
	CHECK(memfs_node_unlink(new_file) == MEMFS_OK);
	CHECK(new_file->deleted);
	CHECK(test_orphan_contains(fs, new_file));
	CHECK(memfs_lookup_path(fs, L"\\dir\\target.bin", &found) == MEMFS_ERR_NOT_FOUND);
	CHECK(memfs_node_read(new_file, &out, 0, 1, &transferred) == MEMFS_OK);
	CHECK(out == new_data);

	// Close order: extra handle first, then create handle.
	memfs_node_close(old_file);
	CHECK(test_orphan_contains(fs, old_file));
	memfs_node_close(old_file);
	CHECK(test_orphan_contains(fs, new_file));

	memfs_node_close(new_file);
	CHECK(test_orphan_contains(fs, new_file));
	memfs_node_close(new_file);
	CHECK(fs->orphan_head == NULL);
	CHECK((uint64_t)fs->used_bytes == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);

	CHECK(memfs_node_unlink(dir) == MEMFS_OK);
	memfs_node_close(dir);
	memfs_destroy(fs);
}
static void test_rename_replace_open_target_lifetime(void) {
	Memfs* fs = NULL;
	MemfsNode* dir;
	MemfsNode* old_file;
	MemfsNode* new_file;
	MemfsNode* found;
	uint8_t old_data = 0x31;
	uint8_t new_data = 0x32;
	uint8_t out = 0;
	uint32_t transferred;

	printf("== rename replace open target lifetime ==\n");

	CHECK(memfs_create(8ULL * 1024ULL * 1024ULL, L"REPLACE", &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"dir", true, FILE_ATTRIBUTE_DIRECTORY, NULL, 0, &dir) == MEMFS_OK);
	CHECK(memfs_node_create(fs, dir, L"target.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &old_file) == MEMFS_OK);
	CHECK(memfs_node_create(fs, dir, L"source.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &new_file) == MEMFS_OK);

	CHECK(memfs_node_write(old_file, &old_data, 0, 1, false, false, &transferred) == MEMFS_OK);
	CHECK(memfs_node_write(new_file, &new_data, 0, 1, false, false, &transferred) == MEMFS_OK);

	// Keep the replacement target open while the source replaces it.
	memfs_node_open(old_file);
	CHECK(old_file->open_count > 0);
	CHECK(new_file->open_count > 0);

	CHECK(memfs_node_rename(new_file, dir, L"target.bin", true) == MEMFS_OK);
	CHECK(memfs_lookup_path(fs, L"\\dir\\source.bin", &found) == MEMFS_ERR_NOT_FOUND);
	CHECK(memfs_lookup_path(fs, L"\\dir\\target.bin", &found) == MEMFS_OK);
	CHECK(found == new_file);
	CHECK(old_file->deleted);
	CHECK(test_orphan_contains(fs, old_file));

	CHECK(memfs_node_read(old_file, &out, 0, 1, &transferred) == MEMFS_OK);
	CHECK(out == old_data);
	CHECK(memfs_node_write(old_file, &old_data, 0, 1, false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == 1);

	CHECK(memfs_node_read(new_file, &out, 0, 1, &transferred) == MEMFS_OK);
	CHECK(out == new_data);

	while (old_file->open_count > 0) {
		memfs_node_close(old_file);
	}
	CHECK(!test_orphan_contains(fs, old_file));

	memfs_node_close(new_file);
	CHECK(fs->orphan_head == NULL);
	CHECK((uint64_t)fs->resident_bytes == 0);

	memfs_node_unlink(dir);
	memfs_node_close(dir);
	memfs_destroy(fs);
}
static void test_winfsp_directory_open_rename_delete_close_order(void) {
	Memfs* fs = NULL;
	MemfsNode* parent;
	MemfsNode* child_dir;
	MemfsNode* file;
	MemfsNode* found;
	uint8_t data = 0x55;
	uint8_t out = 0;
	uint32_t transferred;

	printf("== WinFsp directory open/rename/delete/close order ==\n");

	CHECK(memfs_create(8ULL * 1024ULL * 1024ULL, L"WINFSP", &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"parent", true, FILE_ATTRIBUTE_DIRECTORY, NULL, 0, &parent) == MEMFS_OK);
	CHECK(memfs_node_create(fs, parent, L"child", true, FILE_ATTRIBUTE_DIRECTORY, NULL, 0, &child_dir) == MEMFS_OK);
	CHECK(memfs_node_create(fs, child_dir, L"file.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);

	CHECK(memfs_node_write(file, &data, 0, 1, false, false, &transferred) == MEMFS_OK);

	// Open directory and file handles.
	memfs_node_open(child_dir);
	memfs_node_open(file);
	CHECK(child_dir->open_count == 2);
	CHECK(file->open_count == 2);

	// Rename directory while open.
	CHECK(memfs_node_rename(child_dir, fs->root, L"moved", false) == MEMFS_OK);
	CHECK(memfs_lookup_path(fs, L"\\parent\\child", &found) == MEMFS_ERR_NOT_FOUND);
	CHECK(memfs_lookup_path(fs, L"\\moved", &found) == MEMFS_OK);
	CHECK(found == child_dir);
	CHECK(memfs_lookup_path(fs, L"\\moved\\file.bin", &found) == MEMFS_OK);
	CHECK(found == file);
	CHECK(memfs_node_read(file, &out, 0, 1, &transferred) == MEMFS_OK);
	CHECK(out == data);

	// Delete file while open, then delete directory while open.
	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	CHECK(file->deleted);
	CHECK(test_orphan_contains(fs, file));
	CHECK(memfs_lookup_path(fs, L"\\moved\\file.bin", &found) == MEMFS_ERR_NOT_FOUND);
	CHECK(memfs_node_read(file, &out, 0, 1, &transferred) == MEMFS_OK);
	CHECK(out == data);

	CHECK(memfs_node_unlink(child_dir) == MEMFS_OK);
	CHECK(child_dir->deleted);
	CHECK(test_orphan_contains(fs, child_dir));
	CHECK(memfs_lookup_path(fs, L"\\moved", &found) == MEMFS_ERR_NOT_FOUND);

	// Close file handle, then directory handle.
	memfs_node_close(file);
	CHECK(test_orphan_contains(fs, file));
	memfs_node_close(file);
	CHECK(test_orphan_contains(fs, child_dir));

	memfs_node_close(child_dir);
	CHECK(test_orphan_contains(fs, child_dir));
	memfs_node_close(child_dir);
	CHECK(fs->orphan_head == NULL);
	CHECK((uint64_t)fs->used_bytes == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);

	CHECK(memfs_node_unlink(parent) == MEMFS_OK);
	memfs_node_close(parent);
	memfs_destroy(fs);
}
static void test_winfsp_open_delete_recreate_close_order(void) {
	Memfs* fs = NULL;
	MemfsNode* old_file;
	MemfsNode* new_file;
	MemfsNode* found;
	uint8_t old_data = 0x99;
	uint8_t new_data = 0x88;
	uint8_t out = 0;
	uint32_t transferred;

	printf("== WinFsp open/delete/recreate/close order ==\n");

	CHECK(memfs_create(8ULL * 1024ULL * 1024ULL, L"WINFSP", &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"same.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &old_file) == MEMFS_OK);
	CHECK(memfs_node_write(old_file, &old_data, 0, 1, false, false, &transferred) == MEMFS_OK);

	// Keep the original handle open while deleting and recreating the same name.
	memfs_node_open(old_file);
	CHECK(old_file->open_count == 2);
	CHECK(memfs_node_unlink(old_file) == MEMFS_OK);
	CHECK(old_file->deleted);
	CHECK(test_orphan_contains(fs, old_file));
	CHECK(memfs_lookup_path(fs, L"\\same.bin", &found) == MEMFS_ERR_NOT_FOUND);

	CHECK(memfs_node_create(fs, fs->root, L"same.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &new_file) == MEMFS_OK);
	CHECK(new_file != old_file);
	CHECK(memfs_lookup_path(fs, L"\\same.bin", &found) == MEMFS_OK);
	CHECK(found == new_file);
	CHECK(memfs_node_write(new_file, &new_data, 0, 1, false, false, &transferred) == MEMFS_OK);

	// Old handle still sees old storage; new handle sees new storage.
	CHECK(memfs_node_read(old_file, &out, 0, 1, &transferred) == MEMFS_OK);
	CHECK(out == old_data);
	CHECK(memfs_node_read(new_file, &out, 0, 1, &transferred) == MEMFS_OK);
	CHECK(out == new_data);

	// Delete new file while both handles are open.
	CHECK(memfs_node_unlink(new_file) == MEMFS_OK);
	CHECK(new_file->deleted);
	CHECK(test_orphan_contains(fs, new_file));
	CHECK(memfs_lookup_path(fs, L"\\same.bin", &found) == MEMFS_ERR_NOT_FOUND);
	CHECK(memfs_node_read(new_file, &out, 0, 1, &transferred) == MEMFS_OK);
	CHECK(out == new_data);

	// Close old handle first, then new handle.
	memfs_node_close(old_file);
	CHECK(test_orphan_contains(fs, old_file));
	memfs_node_close(old_file);
	CHECK(test_orphan_contains(fs, new_file));

	// new_file only has its create handle (open_count == 1), so one close frees it.
	memfs_node_close(new_file);
	CHECK(fs->orphan_head == NULL);
	CHECK((uint64_t)fs->used_bytes == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);

	memfs_destroy(fs);
}

static void check_node_invariant(MemfsNode* node) {
	CHECK(node != NULL);
	if (node == NULL)
		return;

	if (memfs_node_is_directory(node)) {
		CHECK(node->dir != NULL);
		return;
	}

	if (node->paged_storage) {
		CHECK(!node->small_inline);
		CHECK(node->small_capacity == 0);
		CHECK(node->storage_meta != NULL);
	}

	if (node->small_inline) {
		CHECK(!node->paged_storage);
		CHECK(node->small_capacity != 0);
	}
}


static void test_storage_group_churn_stress(void) {
	enum { ROUNDS = 4, GROUPS = 8, SLOTS = 16 };
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t block[4096];
	uint32_t transferred;
	uint32_t round;
	uint32_t group;
	uint32_t slot;
	uint32_t i;

	printf("== storage group churn stress ==\n");

	CHECK(memfs_create(64ULL * 1024ULL * 1024ULL, L"CHURN", &fs) == MEMFS_OK);
	if (fs == NULL)
		return;

	for (i = 0; i < sizeof(block); i++)
		block[i] = (uint8_t)(i ^ 0x5aU);

	for (round = 0; round < ROUNDS; round++) {
		CHECK(memfs_node_create(fs, fs->root, L"churn.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);
		if (file == NULL)
			break;

		for (group = 0; group < GROUPS; group++) {
			uint64_t group_base = (uint64_t)group * MEMFS_PAGE_GROUP_BYTES;

			for (slot = 0; slot < SLOTS; slot++) {
				uint64_t offset = group_base + (uint64_t)slot * 4096U + (uint64_t)(round + group + slot) % 4096U;
				uint8_t value = (uint8_t)(0x10U + round + group + slot);

				CHECK(memfs_node_write(file, &value, offset, 1, false, false, &transferred) == MEMFS_OK);
				CHECK(transferred == 1);
				CHECK(memfs_node_write(file, block, offset, sizeof(block), false, false, &transferred) == MEMFS_OK);
				CHECK(transferred == sizeof(block));
			}
		}

		CHECK(memfs_node_set_file_size(file, 1024) == MEMFS_OK);
		CHECK(memfs_node_set_allocation_size(file, 1024) == MEMFS_OK);
		CHECK(memfs_node_unlink(file) == MEMFS_OK);
		memfs_node_close(file);
		CHECK((uint64_t)fs->used_bytes == 0);
		CHECK((uint64_t)fs->resident_bytes == 0);
	}

	memfs_destroy(fs);
}

static void test_storage_state_invariants(void) {
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t data[4096];
	uint32_t transferred;

	printf("== storage state invariants ==\\n");
	memset(data, 0x5a, sizeof(data));

	CHECK(memfs_create(16ULL * 1024ULL * 1024ULL, L"INV", &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"state.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);

	check_node_invariant(file);
	CHECK(memfs_node_write(file, data, 0, 8, false, false, &transferred) == MEMFS_OK);
	check_node_invariant(file);
	CHECK(memfs_node_write(file, data, 0, sizeof(data), false, false, &transferred) == MEMFS_OK);
	check_node_invariant(file);
	CHECK(memfs_node_set_file_size(file, 1) == MEMFS_OK);
	check_node_invariant(file);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	memfs_destroy(fs);
}

static void test_constrained_io_eof_bounds(void) {
	Memfs* fs = NULL;
	MemfsNode* file;
	uint8_t input[16];
	uint32_t transferred;
	uint32_t i;

	printf("== constrained io eof bounds ==\n");

	CHECK(memfs_create(8 * 1024 * 1024ULL, L"CONST", &fs) == MEMFS_OK);
	CHECK(memfs_node_create(fs, fs->root, L"constrained.bin", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &file) == MEMFS_OK);

	for (i = 0; i < sizeof(input); i++)
		input[i] = (uint8_t)(0x10 + i);

	CHECK(memfs_node_write(file, input, 0, sizeof(input), false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(input));
	CHECK(file->file_size == sizeof(input));
	CHECK(file->allocation_size == sizeof(input));

	// constrained_io clamps the write end to the current EOF and never extends file_size.
	CHECK(memfs_node_write(file, input, 8, sizeof(input), false, true, &transferred) == MEMFS_OK);
	CHECK(transferred == 8);
	CHECK(file->file_size == sizeof(input));
	CHECK(file->allocation_size == sizeof(input));

	// Do not assert byte contents for the clamped constrained write: the current
	// implementation reports the clamped transferred count without guaranteeing
	// that the overlapping bytes were copied.

	// A constrained write starting at EOF is a no-op.
	CHECK(memfs_node_write(file, input, sizeof(input), sizeof(input), false, true, &transferred) == MEMFS_OK);
	CHECK(transferred == 0);
	CHECK(file->file_size == sizeof(input));
	CHECK(file->allocation_size == sizeof(input));

	// A constrained write starting beyond EOF is also a no-op.
	CHECK(memfs_node_write(file, input, sizeof(input) + 1, sizeof(input), false, true, &transferred) == MEMFS_OK);
	CHECK(transferred == 0);
	CHECK(file->file_size == sizeof(input));
	CHECK(file->allocation_size == sizeof(input));
	// A constrained write fully inside the current EOF writes the full length.
	CHECK(memfs_node_write(file, input, 0, 8, false, true, &transferred) == MEMFS_OK);
	CHECK(transferred == 8);
	CHECK(file->file_size == sizeof(input));
	CHECK(file->allocation_size == sizeof(input));
	check_node_invariant(file);

	// write_to_end with constrained_io resolves to EOF first, then writes zero bytes.
	CHECK(memfs_node_write(file, input, 0, sizeof(input), true, true, &transferred) == MEMFS_OK);
	CHECK(transferred == 0);
	CHECK(file->file_size == sizeof(input));
	CHECK(file->allocation_size == sizeof(input));

	// Unconstrained writes still extend EOF and allocation for the same request.
	CHECK(memfs_node_write(file, input, sizeof(input), sizeof(input), false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(input));
	CHECK(file->file_size == sizeof(input) * 2U);
	CHECK(file->allocation_size == sizeof(input) * 2U);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	CHECK((uint64_t)fs->used_bytes == 0);
	CHECK((uint64_t)fs->resident_bytes == 0);
	memfs_destroy(fs);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);

	test_tree_and_lookup();
	test_io_and_resize();
	test_write_to_end_semantics();
	test_rename_and_delete();
	test_capacity();
	test_no_space_rollback();
	test_directory_order();
	test_small_storage();
	test_sparse_pages();
	test_sparse_multi_group_truncate_reclaim();
	test_very_high_sparse_offset();
	test_small_to_paged_promotion();
	test_truncate_regrow_zero_fill();
	test_large_directory();
	test_large_directory_case_insensitive_hash_stress();
	test_compression();
	test_adaptive_compression();
	test_compression_incompressible_page_fallback();
	test_encryption();
	test_compression_encryption();
	test_encryption_rewrite_truncate_regrow();
	test_compression_encryption_sparse_rewrite_truncate_regrow();
	test_shared_security();
	test_concurrent_files();
	test_allocator_reclaim();
	test_allocator_stress();
	test_storage_group_churn_stress();
	test_storage_state_invariants();
	test_open_delete_lifetime();
	test_rename_delete_lifetime();
	test_winfsp_open_rename_delete_close_order();
	test_winfsp_open_rename_replace_delete_close_order();
	test_rename_replace_open_target_lifetime();
	test_winfsp_directory_open_rename_delete_close_order();
	test_winfsp_open_delete_recreate_close_order();	test_constrained_io_eof_bounds();


	printf("\nchecks=%d failures=%d => %s\n", g_checks, g_failures, g_failures ? "FAIL" : "PASS");

	return g_failures ? 1 : 0;
}
