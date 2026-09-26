#include <Windows.h>

#include <stdint.h>
#include <stdio.h>
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
	memfs_node_close(file);
	CHECK(fs->used_bytes == 0);

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

	CHECK(memfs_node_create(fs, fs->root, L"b", false, FILE_ATTRIBUTE_NORMAL, NULL, 1024, &b) == MEMFS_ERR_NO_SPACE);

	CHECK(memfs_node_create(fs, fs->root, L"b", false, FILE_ATTRIBUTE_NORMAL, NULL, 0, &b) == MEMFS_OK);

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
			p = p->sibling_next;
		}
	}
	CHECK(p == NULL);

	for (i = 0; i < 5; i++) {
		CHECK(memfs_node_unlink(nodes[i]) == MEMFS_OK);
		memfs_node_close(nodes[i]);
	}

	memfs_destroy(fs);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);

	test_tree_and_lookup();
	test_io_and_resize();
	test_rename_and_delete();
	test_capacity();
	test_directory_order();

	printf("\nchecks=%d failures=%d => %s\n", g_checks, g_failures, g_failures ? "FAIL" : "PASS");

	return g_failures ? 1 : 0;
}
