#include <Windows.h>

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
			p = memfs_dir_next(p);
		}
	}
	CHECK(p == NULL);

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
	CHECK(file->small_page != NULL);
	CHECK(file->page_groups == NULL);
	CHECK(file->small_capacity == MEMFS_SMALL_GRANULE);
	CHECK(file->resident_bytes < MEMFS_ALLOCATION_UNIT);
	CHECK(fs->resident_bytes < MEMFS_ALLOCATION_UNIT);
	CHECK(file->allocation_size == MEMFS_ALLOCATION_UNIT);

	CHECK(memfs_node_read(file, &out, 0, 1, &transferred) == MEMFS_OK);
	CHECK(out == value);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	CHECK(fs->resident_bytes == 0);
	CHECK(fs->used_bytes == 0);
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
	CHECK(fs->used_bytes == sparse_size);
	CHECK(file->resident_bytes == 0);
	CHECK(fs->resident_bytes == 0);

	memset(buffer, 0xcc, sizeof(buffer));
	CHECK(memfs_node_read(file, buffer, 16ULL * 1024ULL * 1024ULL, sizeof(buffer), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(buffer));
	for (i = 0; i < sizeof(buffer); i++)
		CHECK(buffer[i] == 0);

	CHECK(memfs_node_write(file, &value, write_offset, 1, false, false, &transferred) == MEMFS_OK);
	CHECK(transferred == 1);
	CHECK(file->resident_bytes >= MEMFS_PAGE_SIZE);
	CHECK(file->resident_bytes < MEMFS_PAGE_SIZE + 64U);
	CHECK(fs->resident_bytes >= MEMFS_PAGE_SIZE);
	CHECK(fs->resident_bytes < MEMFS_PAGE_SIZE + 64U);
	CHECK(file->page_groups != NULL);

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
	CHECK(file->resident_bytes == 0);
	CHECK(fs->resident_bytes == 0);

	CHECK(memfs_node_set_allocation_size(file, 2048) == MEMFS_OK);
	CHECK(file->allocation_size == 2048);
	CHECK(fs->used_bytes == 2048);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	CHECK(fs->used_bytes == 0);
	CHECK(fs->resident_bytes == 0);
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
	CHECK(file->small_page != NULL);
	CHECK(file->resident_bytes >= 1024);
	CHECK(file->resident_bytes < 1088);

	CHECK(memfs_node_write(file, &value, 2ULL * MEMFS_PAGE_SIZE + 17, 1, false, false, &transferred) == MEMFS_OK);

	CHECK(file->small_page == NULL);
	CHECK(file->page_groups != NULL);
	CHECK(file->resident_bytes >= 2ULL * MEMFS_PAGE_SIZE);
	CHECK(file->resident_bytes < 2ULL * MEMFS_PAGE_SIZE + 128U);

	memset(verify, 0, sizeof(verify));
	CHECK(memfs_node_read(file, verify, 0, sizeof(verify), &transferred) == MEMFS_OK);
	CHECK(memcmp(prefix, verify, sizeof(prefix)) == 0);

	CHECK(memfs_node_set_file_size(file, 1024) == MEMFS_OK);
	CHECK(file->small_page != NULL);
	CHECK(file->page_groups == NULL);
	CHECK(file->resident_bytes >= 1024);
	CHECK(file->resident_bytes < 1088);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
	CHECK(fs->resident_bytes == 0);
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
	CHECK(file->page_groups != NULL);

	group = file->page_groups[0];
	CHECK(group != NULL);
	if (group) {
		CHECK(group->pages[0] != NULL);
		CHECK(group->pages[1] != NULL);
		if (group->pages[0])
			CHECK(0 != (group->pages[0]->flags & MEMFS_PAGE_COMPRESSED));
		if (group->pages[1])
			CHECK(0 != (group->pages[1]->flags & MEMFS_PAGE_COMPRESSED));
	}

	CHECK(file->resident_bytes < 1024);

	memset(output, 0, sizeof(output));
	CHECK(memfs_node_read(file, output, 0, sizeof(output), &transferred) == MEMFS_OK);
	CHECK(transferred == sizeof(output));
	CHECK(memcmp(input, output, sizeof(input)) == 0);

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
	CHECK(file->small_capacity == MEMFS_SMALL_GRANULE);
	CHECK(file->small_page != NULL);
	CHECK(file->resident_bytes < MEMFS_ALLOCATION_UNIT);

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

	group = file->page_groups ? file->page_groups[0] : NULL;
	CHECK(group != NULL);
	if (group && group->pages[0]) {
		CHECK(0 != (group->pages[0]->flags & MEMFS_PAGE_COMPRESSED));
		CHECK(0 != (group->pages[0]->flags & MEMFS_PAGE_ENCRYPTED));
	}
	CHECK(file->resident_bytes < 2048);

	memset(output, 0, sizeof(output));
	CHECK(memfs_node_read(file, output, 0, sizeof(output), &transferred) == MEMFS_OK);
	CHECK(memcmp(input, output, sizeof(input)) == 0);

	CHECK(memfs_node_unlink(file) == MEMFS_OK);
	memfs_node_close(file);
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

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);

	test_tree_and_lookup();
	test_io_and_resize();
	test_rename_and_delete();
	test_capacity();
	test_directory_order();
	test_small_storage();
	test_sparse_pages();
	test_small_to_paged_promotion();
	test_large_directory();
	test_compression();
	test_encryption();
	test_compression_encryption();
	test_concurrent_files();

	printf("\nchecks=%d failures=%d => %s\n", g_checks, g_failures, g_failures ? "FAIL" : "PASS");

	return g_failures ? 1 : 0;
}
