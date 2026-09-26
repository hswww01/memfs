#pragma once

#include <Windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define MEMFS_ALLOCATION_UNIT 512U
#define MEMFS_MAX_NAME 255U
#define MEMFS_DIR_HASH_THRESHOLD 64U

// 真实 RAM 驻留对极小文件使用更细的粒度；Windows AllocationSize 仍按 512B 报告。
#define MEMFS_SMALL_GRANULE 8U
#define MEMFS_SMALL_LIMIT 4096U

// 大文件按 4KB logical page 管理。一个二级表覆盖 1MB 文件范围。
#define MEMFS_PAGE_SHIFT 12U
#define MEMFS_PAGE_SIZE (1U << MEMFS_PAGE_SHIFT)
#define MEMFS_PAGE_MASK (MEMFS_PAGE_SIZE - 1U)
#define MEMFS_PAGES_PER_GROUP 256U
#define MEMFS_PAGE_GROUP_SHIFT 8U
#define MEMFS_PAGE_GROUP_MASK (MEMFS_PAGES_PER_GROUP - 1U)
#define MEMFS_PAGE_GROUP_BYTES ((uint64_t)MEMFS_PAGE_SIZE * MEMFS_PAGES_PER_GROUP)

#define MEMFS_ENCRYPTION_KEY_SIZE 32U

enum { MEMFS_PAGE_COMPRESSED = 0x01, MEMFS_PAGE_ENCRYPTED = 0x02 };

typedef enum MemfsResult {
	MEMFS_OK = 0,
	MEMFS_ERR_NOT_FOUND,
	MEMFS_ERR_PATH_NOT_FOUND,
	MEMFS_ERR_EXISTS,
	MEMFS_ERR_NOT_DIRECTORY,
	MEMFS_ERR_IS_DIRECTORY,
	MEMFS_ERR_NOT_EMPTY,
	MEMFS_ERR_NO_SPACE,
	MEMFS_ERR_NO_MEMORY,
	MEMFS_ERR_INVALID,
	MEMFS_ERR_ACCESS,
	MEMFS_ERR_DATA
} MemfsResult;

typedef struct Memfs Memfs;
typedef struct MemfsNode MemfsNode;
typedef struct MemfsDir MemfsDir;
typedef struct MemfsDirHash MemfsDirHash;
typedef struct MemfsPage MemfsPage;
typedef struct MemfsPageGroup MemfsPageGroup;

typedef struct MemfsOptions {
	uint64_t capacity;
	const wchar_t* volume_label;
	bool compression_enabled;
	int compression_level;
	bool encryption_enabled;
	const uint8_t* encryption_key;
	size_t encryption_key_size;
} MemfsOptions;

// intrusive treap root；不再为每个目录单独分配 hash bucket 数组。
// 节点中的 tree_* 指针同时完成查找、插入、删除和有序枚举。
struct MemfsDirHash {
	MemfsNode** slots;
	uint32_t capacity;
	uint32_t count;
	uint32_t tombstones;
};

struct MemfsDir {
	MemfsNode* root;
	MemfsDirHash* hash;
	uint32_t child_count;
};

// encoded page header。data 中存 raw/compressed payload；加密时为 nonce + ciphertext。
struct MemfsPage {
	uint16_t stored_size;
	uint16_t plain_size;
	uint8_t flags;
	uint8_t reserved[3];
	uint8_t data[];
};

// 256 个 page pointer，约 2KB metadata，覆盖 1MB 文件范围。
// group 和具体 page 都是按需创建，sparse hole 不占 data page。
struct MemfsPageGroup {
	MemfsPage* pages[MEMFS_PAGES_PER_GROUP];
	uint16_t present_pages;
};

struct MemfsNode {
	Memfs* fs;
	MemfsNode* parent;

	MemfsNode* tree_left;
	MemfsNode* tree_right;
	MemfsNode* tree_parent;
	uint32_t tree_priority;

	MemfsNode* all_prev;
	MemfsNode* all_next;

	wchar_t* name;
	MemfsDir* dir;

	MemfsPage* small_page;
	uint32_t small_capacity;

	MemfsPageGroup** page_groups;
	uint32_t page_group_capacity;
	uint64_t resident_bytes;

	uint64_t file_size;
	uint64_t allocation_size;

	PSECURITY_DESCRIPTOR security;
	uint32_t security_size;

	uint64_t index_number;
	uint64_t creation_time;
	uint64_t last_access_time;
	uint64_t last_write_time;
	uint64_t change_time;

	uint32_t attributes;
	uint32_t open_count;
	bool deleted;
};

struct Memfs {
	MemfsNode* root;
	MemfsNode* all_head;

	// WinFsp FINE guard 负责 namespace + per-file I/O 并发。
	// accounting_lock 只保护跨文件共享的容量/驻留统计。
	SRWLOCK accounting_lock;

	uint64_t capacity;
	uint64_t used_bytes;
	uint64_t resident_bytes;
	uint64_t next_index;

	bool compression_enabled;
	bool encryption_enabled;
	int compression_level;
	uint8_t encryption_key[MEMFS_ENCRYPTION_KEY_SIZE];

	wchar_t volume_label[32];
	uint16_t volume_label_bytes;
};

uint64_t memfs_now(void);
uint64_t memfs_align_allocation(uint64_t size);
uint64_t memfs_free_bytes(Memfs* fs);
uint64_t memfs_resident_bytes(Memfs* fs);

MemfsResult memfs_create(uint64_t capacity, const wchar_t* volume_label, Memfs** out_fs);
MemfsResult memfs_create_ex(const MemfsOptions* options, Memfs** out_fs);
void memfs_destroy(Memfs* fs);

MemfsResult memfs_lookup_path(Memfs* fs, const wchar_t* path, MemfsNode** out_node);
MemfsResult memfs_lookup_parent(Memfs* fs, const wchar_t* path, MemfsNode** out_parent,
								wchar_t name[MEMFS_MAX_NAME + 1]);

MemfsNode* memfs_dir_lookup(MemfsNode* dir, const wchar_t* name);
MemfsNode* memfs_dir_first(MemfsNode* dir);
MemfsNode* memfs_dir_next(MemfsNode* node);

MemfsResult memfs_node_create(Memfs* fs, MemfsNode* parent, const wchar_t* name, bool directory, uint32_t attributes,
							  PSECURITY_DESCRIPTOR security, uint64_t allocation_size, MemfsNode** out_node);

void memfs_node_open(MemfsNode* node);
void memfs_node_close(MemfsNode* node);
MemfsResult memfs_node_unlink(MemfsNode* node);
MemfsResult memfs_node_rename(MemfsNode* node, MemfsNode* new_parent, const wchar_t* new_name, bool replace_if_exists);

MemfsResult memfs_node_set_file_size(MemfsNode* node, uint64_t new_size);
MemfsResult memfs_node_set_allocation_size(MemfsNode* node, uint64_t new_size);
MemfsResult memfs_node_read(MemfsNode* node, void* buffer, uint64_t offset, uint32_t length, uint32_t* bytes_read);
MemfsResult memfs_node_write(MemfsNode* node, const void* buffer, uint64_t offset, uint32_t length, bool write_to_end,
							 bool constrained_io, uint32_t* bytes_written);

MemfsResult memfs_node_replace_security(MemfsNode* node, PSECURITY_DESCRIPTOR security, uint32_t security_size);

bool memfs_node_is_directory(const MemfsNode* node);
bool memfs_node_is_ancestor(const MemfsNode* ancestor, const MemfsNode* node);
