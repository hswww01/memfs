#pragma once

#include <Windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include "memfs_alloc.h"

#define MEMFS_ALLOCATION_UNIT 512U
#define MEMFS_MAX_NAME 255U

// 小目录只用 treap；达到该规模后再启用 hash 加速精确查找。
#define MEMFS_DIR_HASH_THRESHOLD 256U
#define MEMFS_DIR_HASH_LOAD_PERCENT 85U

// 极小文件 resident storage：1/2/4/8B class，8B 以上再按 8B 对齐。
// Windows AllocationSize 仍按 512B 对外报告。
#define MEMFS_SMALL_GRANULE 8U
#define MEMFS_SMALL_LIMIT 4096U
#define MEMFS_COMPRESSION_SKIP_SCORE 4

// 大文件按 4KB logical page 管理，一个二级 group 覆盖 1MB 文件范围。
#define MEMFS_PAGE_SHIFT 12U
#define MEMFS_PAGE_SIZE (1U << MEMFS_PAGE_SHIFT)
#define MEMFS_PAGE_MASK (MEMFS_PAGE_SIZE - 1U)
#define MEMFS_PAGES_PER_GROUP 256U
#define MEMFS_PAGE_GROUP_SHIFT 8U
#define MEMFS_PAGE_GROUP_MASK (MEMFS_PAGES_PER_GROUP - 1U)
#define MEMFS_PAGE_GROUP_WORDS (MEMFS_PAGES_PER_GROUP / 64U)
#define MEMFS_PAGE_GROUP_BYTES ((uint64_t)MEMFS_PAGE_SIZE * MEMFS_PAGES_PER_GROUP)

#define MEMFS_ENCRYPTION_KEY_SIZE 32U
#define MEMFS_ENCRYPTION_NONCE_PREFIX_SIZE 16U

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
typedef struct MemfsStorageMeta MemfsStorageMeta;
typedef struct MemfsDir MemfsDir;
typedef struct MemfsDirHash MemfsDirHash;
typedef struct MemfsSecurity MemfsSecurity;
typedef struct MemfsPage MemfsPage;
typedef struct MemfsPageGroup MemfsPageGroup;
typedef struct MemfsPageGroupEntry MemfsPageGroupEntry;

typedef struct MemfsOptions {
	uint64_t capacity;
	const wchar_t* volume_label;
	bool compression_enabled;
	int compression_level;
	bool encryption_enabled;
	const uint8_t* encryption_key;
	size_t encryption_key_size;
} MemfsOptions;

struct MemfsDirHash {
	MemfsNode** slots;
	uint32_t capacity;
	uint32_t count;
	MemfsAllocator* owner;
};

struct MemfsDir {
	MemfsNode* root;
	MemfsDirHash* hash;
	uint32_t child_count;
};

// 大多数节点继承完全相同的 ACL，使用引用计数共享，避免每节点复制安全描述符。
struct MemfsSecurity {
	volatile LONG ref_count;
	uint32_t size;
	MemfsAllocator* owner;
	uint8_t data[];
};

// encoded blob header。raw/compressed payload 存在 data 中。
// 加密 payload 前 8B 保存 nonce sequence，XChaCha nonce 的前 16B 来自 fs 随机 prefix。
struct MemfsPage {
	uint16_t stored_size;
	uint16_t plain_size;
	uint8_t flags;
	uint8_t reserved[3];
	uint8_t data[];
};

// 自适应 compact group：
// bitmap 表示 256 个 logical page 是否存在；pages[] 只保存实际存在 page 的紧凑指针。
// sparse group 不再固定付 2KB pointer table。
struct MemfsPageGroup {
	uint64_t bitmap[MEMFS_PAGE_GROUP_WORDS];
	MemfsPage** pages;
	uint16_t page_count;
	uint16_t page_capacity;
};

struct MemfsPageGroupEntry {
	uint64_t index;
	MemfsPageGroup* group;
};

MemfsSecurity* memfs_node_get_security(MemfsNode* node);
uint64_t memfs_node_get_creation_time(const MemfsNode* node);
uint64_t memfs_node_get_last_access_time(const MemfsNode* node);
uint64_t memfs_node_get_last_write_time(const MemfsNode* node);
uint64_t memfs_node_get_change_time(const MemfsNode* node);
void memfs_node_set_creation_time(MemfsNode* node, uint64_t value);
void memfs_node_set_last_access_time(MemfsNode* node, uint64_t value);
void memfs_node_set_last_write_time(MemfsNode* node, uint64_t value);
void memfs_node_set_change_time(MemfsNode* node, uint64_t value);

uint32_t memfs_node_page_group_count(const MemfsNode* node);
uint32_t memfs_node_page_group_capacity(const MemfsNode* node);
uint64_t memfs_node_page_group_index(const MemfsNode* node, uint32_t position);
MemfsPageGroup* memfs_node_page_group(const MemfsNode* node, uint32_t position);
int8_t memfs_node_compression_score(const MemfsNode* node);

struct MemfsNode {
	Memfs* fs;
	MemfsNode* parent;

	// namespace 中是 treap links；节点 unlink 后这三个字段会复用为 orphan 链。
	MemfsNode* tree_left;
	MemfsNode* tree_right;
	MemfsNode* tree_parent;

	wchar_t* name;

	// 目录索引、tiny storage、paged storage 三种状态互斥，共用一个指针宽度。
	union {
		MemfsDir* dir;
		MemfsPage* small_page;
		uint8_t small_inline_data[sizeof(MemfsPage*)];
		MemfsStorageMeta* storage_meta;
	};

	uint64_t file_size;
	uint64_t allocation_size;

	MemfsSecurity* security;
	uint64_t index_number;
	uint64_t creation_time;
	uint64_t last_access_time;
	uint64_t last_write_time;
	uint64_t change_time;

	uint32_t attributes;
	uint32_t open_count;
	uint32_t name_hash;
	uint16_t small_capacity;
	uint8_t small_inline : 1;
	uint8_t deleted : 1;
	uint8_t paged_storage : 1;
	uint8_t reserved_flags : 5;
	int8_t compression_score;
};

#define MEMFS_NODE_IS_DIRECTORY(node) ((node) != NULL && 0 != ((node)->attributes & FILE_ATTRIBUTE_DIRECTORY))

struct Memfs {
	MemfsAllocator allocator;

	MemfsNode* root;

	// 仅保存已经从 namespace 移除但仍被 open handle 引用的节点。
	// orphan 节点复用 tree_left/tree_right 作为 prev/next。
	MemfsNode* orphan_head;

	// WinFsp FINE guard 负责 namespace + per-file I/O 并发。
	// accounting_lock 只保护跨文件共享容量/驻留统计。
	uint64_t capacity;
	volatile LONG64 used_bytes;
	volatile LONG64 resident_bytes;
	uint64_t next_index;
	uint64_t treap_seed;

	bool compression_enabled;
	bool encryption_enabled;
	int compression_level;

	uint8_t encryption_key[MEMFS_ENCRYPTION_KEY_SIZE];
	uint8_t encryption_nonce_prefix[MEMFS_ENCRYPTION_NONCE_PREFIX_SIZE];
	volatile LONG64 encryption_nonce_counter;

	wchar_t volume_label[32];
	uint16_t volume_label_bytes;
};

uint64_t memfs_now(void);
uint64_t memfs_align_allocation(uint64_t size);
uint64_t memfs_free_bytes(Memfs* fs);
uint64_t memfs_resident_bytes(Memfs* fs);
uint64_t memfs_node_resident_bytes(const MemfsNode* node);

MemfsResult memfs_create(uint64_t capacity, const wchar_t* volume_label, Memfs** out_fs);
MemfsResult memfs_create_ex(const MemfsOptions* options, Memfs** out_fs);
void memfs_destroy(Memfs* fs);

MemfsResult memfs_lookup_path(Memfs* fs, const wchar_t* path, MemfsNode** out_node);
MemfsResult memfs_lookup_parent(Memfs* fs, const wchar_t* path, MemfsNode** out_parent,
								wchar_t name[MEMFS_MAX_NAME + 1]);

MemfsNode* memfs_dir_lookup(MemfsNode* dir, const wchar_t* name);
MemfsNode* memfs_dir_first(MemfsNode* dir);
MemfsNode* memfs_dir_upper_bound(MemfsNode* dir, const wchar_t* marker);
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
