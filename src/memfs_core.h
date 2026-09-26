#pragma once

#include <Windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

#define MEMFS_ALLOCATION_UNIT 512U
#define MEMFS_MAX_NAME 255U
#define MEMFS_DIR_BUCKETS_INITIAL 16U

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
	MEMFS_ERR_ACCESS
} MemfsResult;

typedef struct Memfs Memfs;
typedef struct MemfsNode MemfsNode;
typedef struct MemfsDir MemfsDir;

struct MemfsDir {
	MemfsNode** buckets;
	uint32_t bucket_count;
	uint32_t child_count;
	MemfsNode* child_head;
};

struct MemfsNode {
	Memfs* fs;
	MemfsNode* parent;

	MemfsNode* sibling_prev;
	MemfsNode* sibling_next;
	MemfsNode* hash_next;

	MemfsNode* all_prev;
	MemfsNode* all_next;

	wchar_t* name;
	MemfsDir* dir;

	uint8_t* data;
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

	uint64_t capacity;
	uint64_t used_bytes;
	uint64_t next_index;

	wchar_t volume_label[32];
	uint16_t volume_label_bytes;
};

uint64_t memfs_now(void);
uint64_t memfs_align_allocation(uint64_t size);

MemfsResult memfs_create(uint64_t capacity, const wchar_t* volume_label, Memfs** out_fs);
void memfs_destroy(Memfs* fs);

MemfsResult memfs_lookup_path(Memfs* fs, const wchar_t* path, MemfsNode** out_node);
MemfsResult memfs_lookup_parent(Memfs* fs, const wchar_t* path, MemfsNode** out_parent,
								wchar_t name[MEMFS_MAX_NAME + 1]);

MemfsNode* memfs_dir_lookup(MemfsNode* dir, const wchar_t* name);
MemfsNode* memfs_dir_first(MemfsNode* dir);

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
