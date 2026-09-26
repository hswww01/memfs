#include "memfs_core.h"

#include <sddl.h>
#include <wctype.h>

#define MEMFS_DEFAULT_SDDL L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;WD)"

static wchar_t* memfs_wcsdup(const wchar_t* s) {
	size_t chars;
	wchar_t* copy;

	if (s == NULL)
		return NULL;

	chars = wcslen(s) + 1;
	if (chars > SIZE_MAX / sizeof(wchar_t))
		return NULL;

	copy = malloc(chars * sizeof(wchar_t));
	if (copy)
		memcpy(copy, s, chars * sizeof(wchar_t));
	return copy;
}

static uint32_t memfs_name_hash(const wchar_t* name) {
	uint32_t hash = 2166136261u;

	while (*name) {
		uint32_t c = (uint32_t)towlower(*name++);
		hash ^= c;
		hash *= 16777619u;
	}
	return hash;
}

static MemfsDir* memfs_dir_create(void) {
	MemfsDir* dir = calloc(1, sizeof(*dir));

	if (dir == NULL)
		return NULL;

	dir->bucket_count = MEMFS_DIR_BUCKETS_INITIAL;
	dir->buckets = calloc(dir->bucket_count, sizeof(*dir->buckets));
	if (dir->buckets == NULL) {
		free(dir);
		return NULL;
	}
	return dir;
}

static void memfs_dir_destroy(MemfsDir* dir) {
	if (dir == NULL)
		return;

	free(dir->buckets);
	free(dir);
}

static void memfs_all_insert(Memfs* fs, MemfsNode* node) {
	node->all_next = fs->all_head;
	node->all_prev = NULL;

	if (fs->all_head)
		fs->all_head->all_prev = node;

	fs->all_head = node;
}

static void memfs_all_remove(Memfs* fs, MemfsNode* node) {
	if (node->all_prev)
		node->all_prev->all_next = node->all_next;
	else
		fs->all_head = node->all_next;

	if (node->all_next)
		node->all_next->all_prev = node->all_prev;

	node->all_prev = NULL;
	node->all_next = NULL;
}

static void memfs_resident_add(MemfsNode* node, uint64_t bytes) {
	node->resident_bytes += bytes;
	node->fs->resident_bytes += bytes;
}

static void memfs_resident_sub(MemfsNode* node, uint64_t bytes) {
	if (bytes > node->resident_bytes)
		bytes = node->resident_bytes;
	if (bytes > node->fs->resident_bytes)
		node->fs->resident_bytes = 0;
	else
		node->fs->resident_bytes -= bytes;
	node->resident_bytes -= bytes;
}

static MemfsPageGroup* memfs_storage_group(MemfsNode* node, uint64_t group_index) {
	if (group_index >= node->page_group_capacity)
		return NULL;
	return node->page_groups[group_index];
}

static uint8_t* memfs_storage_page(MemfsNode* node, uint64_t page_index) {
	uint64_t group_index = page_index >> MEMFS_PAGE_GROUP_SHIFT;
	MemfsPageGroup* group = memfs_storage_group(node, group_index);

	if (group == NULL)
		return NULL;
	return group->pages[page_index & MEMFS_PAGE_GROUP_MASK];
}

static MemfsResult memfs_storage_ensure_group(MemfsNode* node, uint64_t group_index, MemfsPageGroup** out_group) {
	MemfsPageGroup** groups;
	MemfsPageGroup* group;
	uint32_t old_capacity;
	uint32_t new_capacity;

	if (group_index >= UINT32_MAX)
		return MEMFS_ERR_NO_SPACE;

	if (group_index >= node->page_group_capacity) {
		old_capacity = node->page_group_capacity;
		new_capacity = old_capacity ? old_capacity : 4U;

		while (group_index >= new_capacity) {
			if (new_capacity > UINT32_MAX / 2U) {
				new_capacity = (uint32_t)group_index + 1U;
				break;
			}
			new_capacity <<= 1;
		}

		if ((size_t)new_capacity > SIZE_MAX / sizeof(*groups))
			return MEMFS_ERR_NO_MEMORY;

		groups = realloc(node->page_groups, (size_t)new_capacity * sizeof(*groups));
		if (groups == NULL)
			return MEMFS_ERR_NO_MEMORY;

		memset(groups + old_capacity, 0, (size_t)(new_capacity - old_capacity) * sizeof(*groups));

		node->page_groups = groups;
		node->page_group_capacity = new_capacity;
	}

	group = node->page_groups[group_index];
	if (group == NULL) {
		group = calloc(1, sizeof(*group));
		if (group == NULL)
			return MEMFS_ERR_NO_MEMORY;

		node->page_groups[group_index] = group;
	}

	*out_group = group;
	return MEMFS_OK;
}

static MemfsResult memfs_storage_ensure_page(MemfsNode* node, uint64_t page_index, uint8_t** out_page) {
	uint64_t group_index = page_index >> MEMFS_PAGE_GROUP_SHIFT;
	uint32_t slot = (uint32_t)(page_index & MEMFS_PAGE_GROUP_MASK);
	MemfsPageGroup* group;
	MemfsResult result;
	uint8_t* page;

	result = memfs_storage_ensure_group(node, group_index, &group);
	if (result != MEMFS_OK)
		return result;

	page = group->pages[slot];
	if (page == NULL) {
		page = calloc(1, MEMFS_PAGE_SIZE);
		if (page == NULL) {
			if (group->present_pages == 0) {
				free(group);
				node->page_groups[group_index] = NULL;
			}
			return MEMFS_ERR_NO_MEMORY;
		}

		group->pages[slot] = page;
		group->present_pages++;
		memfs_resident_add(node, MEMFS_PAGE_SIZE);
	}

	*out_page = page;
	return MEMFS_OK;
}

static void memfs_storage_compact_groups(MemfsNode* node) {
	uint32_t used = node->page_group_capacity;
	uint32_t target = 0;
	MemfsPageGroup** groups;

	while (used && node->page_groups[used - 1U] == NULL)
		used--;

	if (used == 0) {
		free(node->page_groups);
		node->page_groups = NULL;
		node->page_group_capacity = 0;
		return;
	}

	target = 4U;
	while (target < used && target <= UINT32_MAX / 2U)
		target <<= 1;
	if (target < used)
		target = used;

	if (target >= node->page_group_capacity)
		return;

	groups = realloc(node->page_groups, (size_t)target * sizeof(*groups));
	if (groups) {
		node->page_groups = groups;
		node->page_group_capacity = target;
	}
}

static void memfs_storage_destroy_pages(MemfsNode* node) {
	uint32_t group_index;

	for (group_index = 0; group_index < node->page_group_capacity; group_index++) {
		MemfsPageGroup* group = node->page_groups[group_index];
		uint32_t slot;

		if (group == NULL)
			continue;

		for (slot = 0; slot < MEMFS_PAGES_PER_GROUP; slot++) {
			if (group->pages[slot]) {
				free(group->pages[slot]);
				group->pages[slot] = NULL;
				memfs_resident_sub(node, MEMFS_PAGE_SIZE);
			}
		}

		free(group);
	}

	free(node->page_groups);
	node->page_groups = NULL;
	node->page_group_capacity = 0;
}

static void memfs_storage_destroy(MemfsNode* node) {
	if (node->small_data) {
		free(node->small_data);
		memfs_resident_sub(node, node->small_capacity);
		node->small_data = NULL;
		node->small_capacity = 0;
	}

	memfs_storage_destroy_pages(node);
}

static MemfsResult memfs_storage_ensure_small(MemfsNode* node, uint64_t required_end) {
	uint64_t aligned;
	uint8_t* data;

	if (required_end > MEMFS_PAGE_SIZE)
		return MEMFS_ERR_INVALID;

	aligned = memfs_align_allocation(required_end);
	if (aligned == UINT64_MAX || aligned > MEMFS_PAGE_SIZE)
		return MEMFS_ERR_NO_SPACE;

	if (aligned <= node->small_capacity)
		return MEMFS_OK;

	data = realloc(node->small_data, (size_t)aligned);
	if (data == NULL)
		return MEMFS_ERR_NO_MEMORY;

	memset(data + node->small_capacity, 0, (size_t)(aligned - node->small_capacity));

	memfs_resident_add(node, aligned - node->small_capacity);
	node->small_data = data;
	node->small_capacity = (uint32_t)aligned;
	return MEMFS_OK;
}

static MemfsResult memfs_storage_promote(MemfsNode* node) {
	uint8_t* page;
	MemfsResult result;

	if (node->small_data == NULL)
		return MEMFS_OK;

	result = memfs_storage_ensure_page(node, 0, &page);
	if (result != MEMFS_OK)
		return result;

	memcpy(page, node->small_data, node->small_capacity);
	free(node->small_data);
	memfs_resident_sub(node, node->small_capacity);
	node->small_data = NULL;
	node->small_capacity = 0;
	return MEMFS_OK;
}

static void memfs_storage_trim_small(MemfsNode* node, uint64_t new_size) {
	uint64_t target;
	uint8_t* data;

	if (node->small_data == NULL)
		return;

	if (new_size == 0) {
		free(node->small_data);
		memfs_resident_sub(node, node->small_capacity);
		node->small_data = NULL;
		node->small_capacity = 0;
		return;
	}

	if (new_size < node->small_capacity) {
		memset(node->small_data + new_size, 0, (size_t)(node->small_capacity - new_size));
	}

	target = memfs_align_allocation(new_size);
	if (target == UINT64_MAX || target >= node->small_capacity)
		return;

	data = realloc(node->small_data, (size_t)target);
	if (data == NULL)
		return;

	memfs_resident_sub(node, node->small_capacity - target);
	node->small_data = data;
	node->small_capacity = (uint32_t)target;
}

static void memfs_storage_trim_pages(MemfsNode* node, uint64_t new_size) {
	uint64_t first_free_page = (new_size + MEMFS_PAGE_MASK) >> MEMFS_PAGE_SHIFT;
	uint64_t tail_page = new_size >> MEMFS_PAGE_SHIFT;
	uint32_t group_index;

	if ((new_size & MEMFS_PAGE_MASK) != 0) {
		uint8_t* page = memfs_storage_page(node, tail_page);

		if (page) {
			uint32_t in_page = (uint32_t)(new_size & MEMFS_PAGE_MASK);

			memset(page + in_page, 0, MEMFS_PAGE_SIZE - in_page);
		}
	}

	for (group_index = 0; group_index < node->page_group_capacity; group_index++) {
		MemfsPageGroup* group = node->page_groups[group_index];
		uint32_t slot;

		if (group == NULL)
			continue;

		for (slot = 0; slot < MEMFS_PAGES_PER_GROUP; slot++) {
			uint64_t page_index = ((uint64_t)group_index << MEMFS_PAGE_GROUP_SHIFT) | slot;

			if (page_index < first_free_page)
				continue;
			if (group->pages[slot] == NULL)
				continue;

			free(group->pages[slot]);
			group->pages[slot] = NULL;
			group->present_pages--;
			memfs_resident_sub(node, MEMFS_PAGE_SIZE);
		}

		if (group->present_pages == 0) {
			free(group);
			node->page_groups[group_index] = NULL;
		}
	}

	memfs_storage_compact_groups(node);
}

static void memfs_storage_try_demote(MemfsNode* node, uint64_t new_size) {
	uint64_t target;
	uint8_t* data = NULL;
	uint8_t* page;

	if (node->page_groups == NULL || new_size > MEMFS_PAGE_SIZE)
		return;

	target = memfs_align_allocation(new_size);
	if (target == UINT64_MAX || target > MEMFS_PAGE_SIZE)
		return;

	if (target) {
		data = calloc(1, (size_t)target);
		if (data == NULL)
			return;

		page = memfs_storage_page(node, 0);
		if (page)
			memcpy(data, page, (size_t)new_size);
	}

	memfs_storage_destroy_pages(node);
	node->small_data = data;
	node->small_capacity = (uint32_t)target;
	if (target)
		memfs_resident_add(node, target);
}

static void memfs_storage_trim_data(MemfsNode* node, uint64_t new_size) {
	if (node->page_groups) {
		memfs_storage_trim_pages(node, new_size);
		memfs_storage_try_demote(node, new_size);
	} else {
		memfs_storage_trim_small(node, new_size);
	}
}

static void memfs_storage_read_range(MemfsNode* node, uint8_t* buffer, uint64_t offset, uint64_t length) {
	uint64_t done = 0;

	if (length == 0)
		return;

	if (node->page_groups == NULL) {
		uint64_t available = 0;

		memset(buffer, 0, (size_t)length);
		if (node->small_data == NULL || offset >= node->small_capacity)
			return;

		available = node->small_capacity - offset;
		if (available > length)
			available = length;

		memcpy(buffer, node->small_data + offset, (size_t)available);
		return;
	}

	while (done < length) {
		uint64_t pos = offset + done;
		uint64_t page_index = pos >> MEMFS_PAGE_SHIFT;
		uint32_t in_page = (uint32_t)(pos & MEMFS_PAGE_MASK);
		uint32_t span = MEMFS_PAGE_SIZE - in_page;
		uint8_t* page = memfs_storage_page(node, page_index);

		if (span > length - done)
			span = (uint32_t)(length - done);

		if (page)
			memcpy(buffer + done, page + in_page, span);
		else
			memset(buffer + done, 0, span);

		done += span;
	}
}

static MemfsResult memfs_storage_write_range(MemfsNode* node, const uint8_t* buffer, uint64_t offset, uint64_t length) {
	uint64_t end = offset + length;
	uint64_t done;
	MemfsResult result;

	if (length == 0)
		return MEMFS_OK;

	if (end <= MEMFS_PAGE_SIZE && node->page_groups == NULL) {
		result = memfs_storage_ensure_small(node, end);
		if (result != MEMFS_OK)
			return result;

		memcpy(node->small_data + offset, buffer, (size_t)length);
		return MEMFS_OK;
	}

	result = memfs_storage_promote(node);
	if (result != MEMFS_OK)
		return result;

	// 第一遍只保证所有目标页都存在。这样一旦 malloc 失败，
	// 不会出现部分用户数据已经写入、但整个 Write 返回失败的状态。
	done = 0;
	while (done < length) {
		uint64_t pos = offset + done;
		uint64_t page_index = pos >> MEMFS_PAGE_SHIFT;
		uint32_t in_page = (uint32_t)(pos & MEMFS_PAGE_MASK);
		uint32_t span = MEMFS_PAGE_SIZE - in_page;
		uint8_t* page;

		if (span > length - done)
			span = (uint32_t)(length - done);

		result = memfs_storage_ensure_page(node, page_index, &page);
		if (result != MEMFS_OK)
			return result;

		done += span;
	}

	// 第二遍才真正写数据。
	done = 0;
	while (done < length) {
		uint64_t pos = offset + done;
		uint64_t page_index = pos >> MEMFS_PAGE_SHIFT;
		uint32_t in_page = (uint32_t)(pos & MEMFS_PAGE_MASK);
		uint32_t span = MEMFS_PAGE_SIZE - in_page;
		uint8_t* page = memfs_storage_page(node, page_index);

		if (span > length - done)
			span = (uint32_t)(length - done);

		memcpy(page + in_page, buffer + done, span);
		done += span;
	}

	return MEMFS_OK;
}
static void memfs_node_free(MemfsNode* node) {
	Memfs* fs;

	if (node == NULL)
		return;

	fs = node->fs;
	if (node->allocation_size <= fs->used_bytes)
		fs->used_bytes -= node->allocation_size;

	memfs_storage_destroy(node);
	memfs_all_remove(fs, node);
	memfs_dir_destroy(node->dir);
	free(node->security);
	free(node->name);
	free(node);
}
static MemfsResult memfs_copy_security(PSECURITY_DESCRIPTOR security, PSECURITY_DESCRIPTOR* out_security,
									   uint32_t* out_size) {
	uint32_t size;
	PSECURITY_DESCRIPTOR copy;

	if (security == NULL)
		return MEMFS_ERR_INVALID;

	size = GetSecurityDescriptorLength(security);
	if (size == 0)
		return MEMFS_ERR_INVALID;

	copy = malloc(size);
	if (copy == NULL)
		return MEMFS_ERR_NO_MEMORY;

	memcpy(copy, security, size);
	*out_security = copy;
	*out_size = size;
	return MEMFS_OK;
}

static MemfsResult memfs_default_security(PSECURITY_DESCRIPTOR* out_security, uint32_t* out_size) {
	PSECURITY_DESCRIPTOR descriptor = NULL;
	ULONG size = 0;
	MemfsResult result;

	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(MEMFS_DEFAULT_SDDL, SDDL_REVISION_1, &descriptor,
															  &size)) {
		return MEMFS_ERR_ACCESS;
	}

	result = memfs_copy_security(descriptor, out_security, out_size);
	LocalFree(descriptor);
	return result;
}

static void memfs_dir_rehash(MemfsNode* parent, uint32_t new_bucket_count) {
	MemfsDir* dir = parent->dir;
	MemfsNode** buckets;
	MemfsNode* node;

	if (new_bucket_count <= dir->bucket_count)
		return;

	buckets = calloc(new_bucket_count, sizeof(*buckets));
	if (buckets == NULL)
		return;

	for (node = dir->child_head; node; node = node->sibling_next) {
		uint32_t bucket = memfs_name_hash(node->name) & (new_bucket_count - 1U);

		node->hash_next = buckets[bucket];
		buckets[bucket] = node;
	}

	free(dir->buckets);
	dir->buckets = buckets;
	dir->bucket_count = new_bucket_count;
}

static void memfs_dir_insert(MemfsNode* parent, MemfsNode* node) {
	MemfsDir* dir = parent->dir;
	uint32_t bucket;
	MemfsNode* pos;
	MemfsNode* prev = NULL;

	if (dir->child_count >= dir->bucket_count * 2U && dir->bucket_count < 4096U)
		memfs_dir_rehash(parent, dir->bucket_count << 1);

	bucket = memfs_name_hash(node->name) & (dir->bucket_count - 1U);
	node->hash_next = dir->buckets[bucket];
	dir->buckets[bucket] = node;

	for (pos = dir->child_head; pos; pos = pos->sibling_next) {
		if (_wcsicmp(node->name, pos->name) < 0)
			break;
		prev = pos;
	}

	node->sibling_prev = prev;
	node->sibling_next = pos;

	if (prev)
		prev->sibling_next = node;
	else
		dir->child_head = node;

	if (pos)
		pos->sibling_prev = node;

	dir->child_count++;
	node->parent = parent;
}

static void memfs_dir_remove(MemfsNode* node) {
	MemfsNode* parent = node->parent;
	MemfsDir* dir;
	uint32_t bucket;
	MemfsNode** p;

	if (parent == NULL || parent->dir == NULL)
		return;

	dir = parent->dir;
	bucket = memfs_name_hash(node->name) & (dir->bucket_count - 1U);
	p = &dir->buckets[bucket];

	while (*p) {
		if (*p == node) {
			*p = node->hash_next;
			break;
		}
		p = &(*p)->hash_next;
	}

	if (node->sibling_prev)
		node->sibling_prev->sibling_next = node->sibling_next;
	else if (dir->child_head == node)
		dir->child_head = node->sibling_next;

	if (node->sibling_next)
		node->sibling_next->sibling_prev = node->sibling_prev;

	if (dir->child_count)
		dir->child_count--;

	node->hash_next = NULL;
	node->sibling_prev = NULL;
	node->sibling_next = NULL;
	node->parent = NULL;
}

static void memfs_touch_directory(MemfsNode* dir) {
	uint64_t now;

	if (dir == NULL)
		return;

	now = memfs_now();
	dir->last_write_time = now;
	dir->change_time = now;
}

static MemfsResult memfs_node_alloc(Memfs* fs, MemfsNode* parent, const wchar_t* name, bool directory,
									uint32_t attributes, PSECURITY_DESCRIPTOR security, MemfsNode** out_node) {
	MemfsNode* node;
	MemfsResult result;

	node = calloc(1, sizeof(*node));
	if (node == NULL)
		return MEMFS_ERR_NO_MEMORY;

	node->fs = fs;
	node->name = memfs_wcsdup(name ? name : L"");
	if (node->name == NULL) {
		free(node);
		return MEMFS_ERR_NO_MEMORY;
	}

	if (directory) {
		node->dir = memfs_dir_create();
		if (node->dir == NULL) {
			free(node->name);
			free(node);
			return MEMFS_ERR_NO_MEMORY;
		}
		attributes |= FILE_ATTRIBUTE_DIRECTORY;
	} else {
		attributes &= ~FILE_ATTRIBUTE_DIRECTORY;
		if (attributes == 0)
			attributes = FILE_ATTRIBUTE_NORMAL;
	}

	if (security) {
		result = memfs_copy_security(security, &node->security, &node->security_size);
	} else if (parent && parent->security) {
		result = memfs_copy_security(parent->security, &node->security, &node->security_size);
	} else {
		result = memfs_default_security(&node->security, &node->security_size);
	}

	if (result != MEMFS_OK) {
		memfs_dir_destroy(node->dir);
		free(node->name);
		free(node);
		return result;
	}

	node->attributes = attributes;
	node->index_number = fs->next_index++;
	node->creation_time = memfs_now();
	node->last_access_time = node->creation_time;
	node->last_write_time = node->creation_time;
	node->change_time = node->creation_time;

	memfs_all_insert(fs, node);
	*out_node = node;
	return MEMFS_OK;
}

static MemfsResult memfs_resize_allocation(MemfsNode* node, uint64_t allocation_size) {
	Memfs* fs = node->fs;
	uint64_t old_size = node->allocation_size;

	if (allocation_size == old_size)
		return MEMFS_OK;

	if (allocation_size > old_size) {
		uint64_t delta = allocation_size - old_size;

		if (fs->used_bytes > fs->capacity || delta > fs->capacity - fs->used_bytes) {
			return MEMFS_ERR_NO_SPACE;
		}

		fs->used_bytes += delta;
	} else {
		uint64_t delta = old_size - allocation_size;

		// 缩小 AllocationSize 时，超出新边界的 resident page
		// 可以立即释放；页尾也会清零，避免以后扩容看到旧数据。
		memfs_storage_trim_data(node, allocation_size);

		if (delta > fs->used_bytes)
			fs->used_bytes = 0;
		else
			fs->used_bytes -= delta;
	}

	node->allocation_size = allocation_size;
	if (node->file_size > allocation_size)
		node->file_size = allocation_size;

	return MEMFS_OK;
}
uint64_t memfs_now(void) {
	FILETIME ft;
	ULARGE_INTEGER value;

	GetSystemTimeAsFileTime(&ft);
	value.LowPart = ft.dwLowDateTime;
	value.HighPart = ft.dwHighDateTime;
	return value.QuadPart;
}

uint64_t memfs_align_allocation(uint64_t size) {
	uint64_t mask = MEMFS_ALLOCATION_UNIT - 1U;

	if (size > UINT64_MAX - mask)
		return UINT64_MAX;
	return (size + mask) & ~mask;
}

MemfsResult memfs_create(uint64_t capacity, const wchar_t* volume_label, Memfs** out_fs) {
	Memfs* fs;
	MemfsNode* root;
	MemfsResult result;
	size_t label_chars;

	if (out_fs == NULL || capacity == 0)
		return MEMFS_ERR_INVALID;

	fs = calloc(1, sizeof(*fs));
	if (fs == NULL)
		return MEMFS_ERR_NO_MEMORY;

	fs->capacity = capacity;
	fs->next_index = 1;

	if (volume_label == NULL || *volume_label == L'\0')
		volume_label = L"MEMFS";

	label_chars = wcslen(volume_label);
	if (label_chars > 31)
		label_chars = 31;

	memcpy(fs->volume_label, volume_label, label_chars * sizeof(wchar_t));
	fs->volume_label[label_chars] = L'\0';
	fs->volume_label_bytes = (uint16_t)(label_chars * sizeof(wchar_t));

	result = memfs_node_alloc(fs, NULL, L"", true, FILE_ATTRIBUTE_DIRECTORY, NULL, &root);
	if (result != MEMFS_OK) {
		free(fs);
		return result;
	}

	fs->root = root;
	*out_fs = fs;
	return MEMFS_OK;
}

void memfs_destroy(Memfs* fs) {
	MemfsNode* node;

	if (fs == NULL)
		return;

	while ((node = fs->all_head) != NULL)
		memfs_node_free(node);

	free(fs);
}

MemfsNode* memfs_dir_lookup(MemfsNode* dir_node, const wchar_t* name) {
	MemfsDir* dir;
	uint32_t bucket;
	MemfsNode* node;

	if (dir_node == NULL || dir_node->dir == NULL || name == NULL)
		return NULL;

	dir = dir_node->dir;
	bucket = memfs_name_hash(name) & (dir->bucket_count - 1U);

	for (node = dir->buckets[bucket]; node; node = node->hash_next) {
		if (_wcsicmp(node->name, name) == 0 && !node->deleted)
			return node;
	}
	return NULL;
}

MemfsNode* memfs_dir_first(MemfsNode* dir) {
	return dir && dir->dir ? dir->dir->child_head : NULL;
}

MemfsResult memfs_lookup_path(Memfs* fs, const wchar_t* path, MemfsNode** out_node) {
	const wchar_t* p;
	MemfsNode* node;
	wchar_t component[MEMFS_MAX_NAME + 1];

	if (fs == NULL || path == NULL || out_node == NULL || path[0] != L'\\')
		return MEMFS_ERR_INVALID;

	node = fs->root;
	p = path;

	while (*p) {
		size_t len = 0;

		while (*p == L'\\')
			p++;
		if (*p == L'\0')
			break;

		while (p[len] && p[len] != L'\\') {
			if (len >= MEMFS_MAX_NAME)
				return MEMFS_ERR_INVALID;
			component[len] = p[len];
			len++;
		}
		component[len] = L'\0';

		if (wcscmp(component, L".") == 0) {
			p += len;
			continue;
		}
		if (wcscmp(component, L"..") == 0) {
			if (node->parent)
				node = node->parent;
			p += len;
			continue;
		}

		if (node->dir == NULL)
			return MEMFS_ERR_NOT_DIRECTORY;

		node = memfs_dir_lookup(node, component);
		if (node == NULL)
			return MEMFS_ERR_NOT_FOUND;

		p += len;
	}

	*out_node = node;
	return MEMFS_OK;
}

MemfsResult memfs_lookup_parent(Memfs* fs, const wchar_t* path, MemfsNode** out_parent,
								wchar_t name[MEMFS_MAX_NAME + 1]) {
	const wchar_t* end;
	const wchar_t* sep;
	size_t name_len;
	size_t parent_len;
	wchar_t* parent_path = NULL;
	MemfsNode* parent;
	MemfsResult result;

	if (fs == NULL || path == NULL || out_parent == NULL || name == NULL || path[0] != L'\\')
		return MEMFS_ERR_INVALID;

	end = path + wcslen(path);
	while (end > path + 1 && end[-1] == L'\\')
		end--;

	sep = end;
	while (sep > path && sep[-1] != L'\\')
		sep--;

	if (sep == end)
		return MEMFS_ERR_INVALID;

	name_len = (size_t)(end - sep);
	if (name_len == 0 || name_len > MEMFS_MAX_NAME)
		return MEMFS_ERR_INVALID;

	memcpy(name, sep, name_len * sizeof(wchar_t));
	name[name_len] = L'\0';

	if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0)
		return MEMFS_ERR_INVALID;

	if (sep == path + 1) {
		parent = fs->root;
	} else {
		parent_len = (size_t)(sep - path);
		parent_path = malloc((parent_len + 1) * sizeof(wchar_t));
		if (parent_path == NULL)
			return MEMFS_ERR_NO_MEMORY;

		memcpy(parent_path, path, parent_len * sizeof(wchar_t));
		parent_path[parent_len] = L'\0';

		result = memfs_lookup_path(fs, parent_path, &parent);
		free(parent_path);
		if (result != MEMFS_OK)
			return MEMFS_ERR_PATH_NOT_FOUND;
	}

	if (parent->dir == NULL)
		return MEMFS_ERR_NOT_DIRECTORY;

	*out_parent = parent;
	return MEMFS_OK;
}

MemfsResult memfs_node_create(Memfs* fs, MemfsNode* parent, const wchar_t* name, bool directory, uint32_t attributes,
							  PSECURITY_DESCRIPTOR security, uint64_t allocation_size, MemfsNode** out_node) {
	MemfsNode* node;
	MemfsResult result;

	if (fs == NULL || parent == NULL || parent->dir == NULL || name == NULL || *name == L'\0' || out_node == NULL) {
		return MEMFS_ERR_INVALID;
	}

	if (memfs_dir_lookup(parent, name))
		return MEMFS_ERR_EXISTS;

	result = memfs_node_alloc(fs, parent, name, directory, attributes, security, &node);
	if (result != MEMFS_OK)
		return result;

	if (!directory && allocation_size) {
		allocation_size = memfs_align_allocation(allocation_size);
		if (allocation_size == UINT64_MAX) {
			memfs_node_free(node);
			return MEMFS_ERR_NO_SPACE;
		}

		result = memfs_resize_allocation(node, allocation_size);
		if (result != MEMFS_OK) {
			memfs_node_free(node);
			return result;
		}
	}

	node->open_count = 1;
	memfs_dir_insert(parent, node);
	memfs_touch_directory(parent);

	*out_node = node;
	return MEMFS_OK;
}

void memfs_node_open(MemfsNode* node) {
	if (node)
		node->open_count++;
}

void memfs_node_close(MemfsNode* node) {
	if (node == NULL)
		return;

	if (node->open_count)
		node->open_count--;

	if (node->deleted && node->open_count == 0)
		memfs_node_free(node);
}

bool memfs_node_is_directory(const MemfsNode* node) {
	return node && node->dir != NULL;
}

bool memfs_node_is_ancestor(const MemfsNode* ancestor, const MemfsNode* node) {
	for (; node; node = node->parent) {
		if (node == ancestor)
			return true;
	}
	return false;
}

MemfsResult memfs_node_unlink(MemfsNode* node) {
	MemfsNode* parent;

	if (node == NULL || node->fs == NULL || node == node->fs->root)
		return MEMFS_ERR_ACCESS;
	if (node->deleted)
		return MEMFS_OK;
	if (node->dir && node->dir->child_count)
		return MEMFS_ERR_NOT_EMPTY;

	parent = node->parent;
	memfs_dir_remove(node);
	node->deleted = true;
	node->change_time = memfs_now();
	memfs_touch_directory(parent);

	if (node->open_count == 0)
		memfs_node_free(node);

	return MEMFS_OK;
}

MemfsResult memfs_node_rename(MemfsNode* node, MemfsNode* new_parent, const wchar_t* new_name, bool replace_if_exists) {
	MemfsNode* existing;
	MemfsNode* old_parent;
	wchar_t* new_name_copy;

	if (node == NULL || new_parent == NULL || new_parent->dir == NULL || new_name == NULL || *new_name == L'\0') {
		return MEMFS_ERR_INVALID;
	}
	if (node == node->fs->root)
		return MEMFS_ERR_ACCESS;
	if (memfs_node_is_ancestor(node, new_parent))
		return MEMFS_ERR_ACCESS;

	existing = memfs_dir_lookup(new_parent, new_name);
	if (existing == node) {
		if (wcscmp(node->name, new_name) == 0)
			return MEMFS_OK;
	} else if (existing) {
		if (!replace_if_exists)
			return MEMFS_ERR_EXISTS;
		if (memfs_node_is_directory(existing) != memfs_node_is_directory(node))
			return MEMFS_ERR_ACCESS;
		if (existing->dir && existing->dir->child_count)
			return MEMFS_ERR_NOT_EMPTY;
	}

	new_name_copy = memfs_wcsdup(new_name);
	if (new_name_copy == NULL)
		return MEMFS_ERR_NO_MEMORY;

	if (existing && existing != node) {
		MemfsNode* existing_parent = existing->parent;

		memfs_dir_remove(existing);
		existing->deleted = true;
		memfs_touch_directory(existing_parent);

		if (existing->open_count == 0)
			memfs_node_free(existing);
	}

	old_parent = node->parent;
	memfs_dir_remove(node);
	free(node->name);
	node->name = new_name_copy;
	memfs_dir_insert(new_parent, node);

	node->change_time = memfs_now();
	memfs_touch_directory(old_parent);
	if (new_parent != old_parent)
		memfs_touch_directory(new_parent);

	return MEMFS_OK;
}

MemfsResult memfs_node_set_allocation_size(MemfsNode* node, uint64_t new_size) {
	uint64_t aligned;
	MemfsResult result;

	if (node == NULL)
		return MEMFS_ERR_INVALID;
	if (node->dir)
		return MEMFS_ERR_IS_DIRECTORY;

	aligned = memfs_align_allocation(new_size);
	if (aligned == UINT64_MAX)
		return MEMFS_ERR_NO_SPACE;

	result = memfs_resize_allocation(node, aligned);
	if (result == MEMFS_OK)
		node->change_time = memfs_now();
	return result;
}

MemfsResult memfs_node_set_file_size(MemfsNode* node, uint64_t new_size) {
	uint64_t new_allocation;
	uint64_t old_file_size;
	MemfsResult result;

	if (node == NULL)
		return MEMFS_ERR_INVALID;
	if (node->dir)
		return MEMFS_ERR_IS_DIRECTORY;

	old_file_size = node->file_size;

	if (new_size > node->allocation_size) {
		new_allocation = memfs_align_allocation(new_size);
		if (new_allocation == UINT64_MAX)
			return MEMFS_ERR_NO_SPACE;

		result = memfs_resize_allocation(node, new_allocation);
		if (result != MEMFS_OK)
			return result;
	}

	if (new_size < old_file_size)
		memfs_storage_trim_data(node, new_size);

	node->file_size = new_size;
	node->change_time = memfs_now();
	return MEMFS_OK;
}

MemfsResult memfs_node_read(MemfsNode* node, void* buffer, uint64_t offset, uint32_t length, uint32_t* bytes_read) {
	uint64_t end;

	if (node == NULL || buffer == NULL || bytes_read == NULL)
		return MEMFS_ERR_INVALID;
	if (node->dir)
		return MEMFS_ERR_IS_DIRECTORY;

	*bytes_read = 0;
	if (length == 0)
		return MEMFS_OK;
	if (offset >= node->file_size)
		return MEMFS_ERR_NOT_FOUND;

	end = offset + length;
	if (end < offset)
		return MEMFS_ERR_INVALID;
	if (end > node->file_size)
		end = node->file_size;

	memfs_storage_read_range(node, buffer, offset, end - offset);

	*bytes_read = (uint32_t)(end - offset);
	node->last_access_time = memfs_now();
	return MEMFS_OK;
}

MemfsResult memfs_node_write(MemfsNode* node, const void* buffer, uint64_t offset, uint32_t length, bool write_to_end,
							 bool constrained_io, uint32_t* bytes_written) {
	uint64_t end;
	uint64_t write_length;
	uint64_t old_allocation;
	bool allocation_grew = false;
	MemfsResult result;

	if (node == NULL || buffer == NULL || bytes_written == NULL)
		return MEMFS_ERR_INVALID;
	if (node->dir)
		return MEMFS_ERR_IS_DIRECTORY;

	*bytes_written = 0;
	old_allocation = node->allocation_size;

	if (length == 0)
		return MEMFS_OK;

	if (write_to_end)
		offset = node->file_size;

	end = offset + length;
	if (end < offset)
		return MEMFS_ERR_INVALID;

	if (constrained_io) {
		if (offset >= node->file_size)
			return MEMFS_OK;

		if (end > node->file_size)
			end = node->file_size;
	} else if (end > node->allocation_size) {
		uint64_t new_allocation = memfs_align_allocation(end);

		if (new_allocation == UINT64_MAX)
			return MEMFS_ERR_NO_SPACE;

		result = memfs_resize_allocation(node, new_allocation);
		if (result != MEMFS_OK)
			return result;

		allocation_grew = true;
	}

	write_length = end - offset;
	result = memfs_storage_write_range(node, (const uint8_t*)buffer, offset, write_length);
	if (result != MEMFS_OK) {
		if (allocation_grew)
			memfs_resize_allocation(node, old_allocation);
		return result;
	}

	if (!constrained_io && end > node->file_size)
		node->file_size = end;

	*bytes_written = (uint32_t)write_length;
	node->last_write_time = memfs_now();
	node->change_time = node->last_write_time;
	node->attributes |= FILE_ATTRIBUTE_ARCHIVE;
	return MEMFS_OK;
}
MemfsResult memfs_node_replace_security(MemfsNode* node, PSECURITY_DESCRIPTOR security, uint32_t security_size) {
	PSECURITY_DESCRIPTOR copy;

	if (node == NULL || security == NULL || security_size == 0)
		return MEMFS_ERR_INVALID;

	copy = malloc(security_size);
	if (copy == NULL)
		return MEMFS_ERR_NO_MEMORY;

	memcpy(copy, security, security_size);
	free(node->security);
	node->security = copy;
	node->security_size = security_size;
	node->change_time = memfs_now();
	return MEMFS_OK;
}
