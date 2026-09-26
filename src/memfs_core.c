#include "memfs_core.h"

#include <sddl.h>
#include <sodium.h>
#include <zstd.h>
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

static MemfsDir* memfs_dir_create(void) {
	return calloc(1, sizeof(MemfsDir));
}

static void memfs_dir_destroy(MemfsDir* dir) {
	if (dir == NULL)
		return;

	if (dir->hash) {
		free(dir->hash->slots);
		free(dir->hash);
	}

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

typedef struct MemfsPageAad {
	uint64_t node_index;
	uint64_t storage_index;
	uint16_t plain_size;
	uint8_t flags;
	uint8_t reserved[5];
} MemfsPageAad;

static size_t memfs_page_heap_size(const MemfsPage* page) {
	if (page == NULL)
		return 0;
	return sizeof(*page) + page->stored_size;
}

static void memfs_resident_add(MemfsNode* node, uint64_t bytes) {
	if (bytes == 0)
		return;

	node->resident_bytes += bytes;

	AcquireSRWLockExclusive(&node->fs->accounting_lock);
	node->fs->resident_bytes += bytes;
	ReleaseSRWLockExclusive(&node->fs->accounting_lock);
}

static void memfs_resident_sub(MemfsNode* node, uint64_t bytes) {
	if (bytes == 0)
		return;

	if (bytes > node->resident_bytes)
		bytes = node->resident_bytes;

	node->resident_bytes -= bytes;

	AcquireSRWLockExclusive(&node->fs->accounting_lock);
	if (bytes > node->fs->resident_bytes)
		node->fs->resident_bytes = 0;
	else
		node->fs->resident_bytes -= bytes;
	ReleaseSRWLockExclusive(&node->fs->accounting_lock);
}

static bool memfs_buffer_is_zero(const uint8_t* data, uint32_t size) {
	uint32_t i = 0;

	while (i + sizeof(uint64_t) <= size) {
		uint64_t value;

		memcpy(&value, data + i, sizeof(value));
		if (value)
			return false;
		i += sizeof(uint64_t);
	}

	while (i < size) {
		if (data[i])
			return false;
		i++;
	}

	return true;
}

static MemfsResult memfs_page_encode(MemfsNode* node, uint64_t storage_index, const uint8_t* plain, uint16_t plain_size,
									 MemfsPage** out_page) {
	uint8_t compressed[ZSTD_COMPRESSBOUND(MEMFS_PAGE_SIZE)];
	const uint8_t* payload = plain;
	size_t payload_size = plain_size;
	uint8_t flags = 0;
	size_t stored_size;
	MemfsPage* page;

	*out_page = NULL;

	if (plain_size == 0 || memfs_buffer_is_zero(plain, plain_size))
		return MEMFS_OK;

	if (node->fs->compression_enabled && plain_size >= 128U) {
		size_t compressed_size =
			ZSTD_compress(compressed, sizeof(compressed), plain, plain_size, node->fs->compression_level);

		if (!ZSTD_isError(compressed_size) && compressed_size + 32U < plain_size) {
			payload = compressed;
			payload_size = compressed_size;
			flags |= MEMFS_PAGE_COMPRESSED;
		}
	}

	if (node->fs->encryption_enabled) {
		stored_size =
			crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + payload_size + crypto_aead_xchacha20poly1305_ietf_ABYTES;
	} else {
		stored_size = payload_size;
	}

	if (stored_size > UINT16_MAX)
		return MEMFS_ERR_NO_MEMORY;

	page = malloc(sizeof(*page) + stored_size);
	if (page == NULL)
		return MEMFS_ERR_NO_MEMORY;

	page->stored_size = (uint16_t)stored_size;
	page->plain_size = plain_size;
	page->flags = flags;
	memset(page->reserved, 0, sizeof(page->reserved));

	if (node->fs->encryption_enabled) {
		MemfsPageAad aad;
		uint8_t* nonce = page->data;
		uint8_t* cipher = page->data + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
		unsigned long long cipher_size = 0;

		page->flags |= MEMFS_PAGE_ENCRYPTED;

		memset(&aad, 0, sizeof(aad));
		aad.node_index = node->index_number;
		aad.storage_index = storage_index;
		aad.plain_size = plain_size;
		aad.flags = page->flags;

		randombytes_buf(nonce, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);

		if (0 != crypto_aead_xchacha20poly1305_ietf_encrypt(
					 cipher, &cipher_size, payload, (unsigned long long)payload_size, (const unsigned char*)&aad,
					 sizeof(aad), NULL, nonce, node->fs->encryption_key)) {
			free(page);
			return MEMFS_ERR_DATA;
		}

		if (cipher_size != payload_size + crypto_aead_xchacha20poly1305_ietf_ABYTES) {
			free(page);
			return MEMFS_ERR_DATA;
		}
	} else {
		memcpy(page->data, payload, payload_size);
	}

	*out_page = page;
	return MEMFS_OK;
}

static MemfsResult memfs_page_decode(MemfsNode* node, uint64_t storage_index, const MemfsPage* page, uint8_t* plain,
									 uint16_t expected_plain_size) {
	uint8_t stage[MEMFS_PAGE_SIZE];
	const uint8_t* payload;
	size_t payload_size;
	uint16_t plain_size;

	if (expected_plain_size > MEMFS_PAGE_SIZE)
		return MEMFS_ERR_INVALID;

	memset(plain, 0, expected_plain_size);

	if (page == NULL)
		return MEMFS_OK;

	plain_size = page->plain_size;
	if (plain_size != expected_plain_size)
		return MEMFS_ERR_DATA;

	payload = page->data;
	payload_size = page->stored_size;

	if (page->flags & MEMFS_PAGE_ENCRYPTED) {
		MemfsPageAad aad;
		unsigned long long decoded_size = 0;
		const uint8_t* nonce;
		const uint8_t* cipher;
		size_t cipher_size;

		if (!node->fs->encryption_enabled)
			return MEMFS_ERR_DATA;
		if (payload_size < crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + crypto_aead_xchacha20poly1305_ietf_ABYTES) {
			return MEMFS_ERR_DATA;
		}

		nonce = payload;
		cipher = payload + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
		cipher_size = payload_size - crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;

		memset(&aad, 0, sizeof(aad));
		aad.node_index = node->index_number;
		aad.storage_index = storage_index;
		aad.plain_size = page->plain_size;
		aad.flags = page->flags;

		if (0 != crypto_aead_xchacha20poly1305_ietf_decrypt(stage, &decoded_size, NULL, cipher,
															(unsigned long long)cipher_size, (const unsigned char*)&aad,
															sizeof(aad), nonce, node->fs->encryption_key)) {
			return MEMFS_ERR_DATA;
		}

		if (decoded_size > sizeof(stage))
			return MEMFS_ERR_DATA;

		payload = stage;
		payload_size = (size_t)decoded_size;
	} else if (node->fs->encryption_enabled) {
		return MEMFS_ERR_DATA;
	}

	if (page->flags & MEMFS_PAGE_COMPRESSED) {
		size_t decoded = ZSTD_decompress(plain, expected_plain_size, payload, payload_size);

		if (ZSTD_isError(decoded) || decoded != expected_plain_size) {
			return MEMFS_ERR_DATA;
		}
		return MEMFS_OK;
	}

	if (payload_size != expected_plain_size)
		return MEMFS_ERR_DATA;

	memcpy(plain, payload, expected_plain_size);
	return MEMFS_OK;
}

static MemfsPageGroup* memfs_storage_group(MemfsNode* node, uint64_t group_index) {
	if (group_index >= node->page_group_capacity)
		return NULL;
	return node->page_groups[group_index];
}

static MemfsPage* memfs_storage_page(MemfsNode* node, uint64_t page_index) {
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

		if ((size_t)new_capacity > SIZE_MAX / sizeof(*groups)) {
			return MEMFS_ERR_NO_MEMORY;
		}

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

static void memfs_storage_replace_page(MemfsNode* node, MemfsPageGroup* group, uint32_t slot, MemfsPage* new_page) {
	MemfsPage* old_page = group->pages[slot];
	size_t old_size = memfs_page_heap_size(old_page);
	size_t new_size = memfs_page_heap_size(new_page);

	if (old_page == NULL && new_page)
		group->present_pages++;
	else if (old_page && new_page == NULL)
		group->present_pages--;

	group->pages[slot] = new_page;

	if (old_size)
		memfs_resident_sub(node, old_size);
	if (new_size)
		memfs_resident_add(node, new_size);

	free(old_page);
}

static void memfs_storage_replace_small(MemfsNode* node, MemfsPage* new_page, uint32_t new_capacity) {
	MemfsPage* old_page = node->small_page;
	size_t old_size = memfs_page_heap_size(old_page);
	size_t new_size = memfs_page_heap_size(new_page);

	node->small_page = new_page;
	node->small_capacity = new_capacity;

	if (old_size)
		memfs_resident_sub(node, old_size);
	if (new_size)
		memfs_resident_add(node, new_size);

	free(old_page);
}

static void memfs_storage_compact_groups(MemfsNode* node) {
	uint32_t used = node->page_group_capacity;
	uint32_t target = 0;
	MemfsPageGroup** groups;

	while (used && node->page_groups[used - 1U] == NULL) {
		used--;
	}

	if (used == 0) {
		free(node->page_groups);
		node->page_groups = NULL;
		node->page_group_capacity = 0;
		return;
	}

	target = 4U;
	while (target < used && target <= UINT32_MAX / 2U) {
		target <<= 1;
	}

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
			MemfsPage* page = group->pages[slot];

			if (page) {
				memfs_resident_sub(node, memfs_page_heap_size(page));
				free(page);
				group->pages[slot] = NULL;
			}
		}

		free(group);
	}

	free(node->page_groups);
	node->page_groups = NULL;
	node->page_group_capacity = 0;
}

static void memfs_storage_destroy(MemfsNode* node) {
	if (node->small_page) {
		memfs_resident_sub(node, memfs_page_heap_size(node->small_page));
		free(node->small_page);
		node->small_page = NULL;
		node->small_capacity = 0;
	}

	memfs_storage_destroy_pages(node);
}

static uint32_t memfs_small_capacity(uint64_t required_end) {
	uint64_t aligned;

	if (required_end == 0)
		return 0;
	if (required_end > MEMFS_SMALL_LIMIT)
		return 0;

	aligned = (required_end + MEMFS_SMALL_GRANULE - 1U) & ~((uint64_t)MEMFS_SMALL_GRANULE - 1U);

	if (aligned > MEMFS_SMALL_LIMIT)
		return 0;

	return (uint32_t)aligned;
}

static MemfsResult memfs_storage_promote(MemfsNode* node) {
	uint8_t plain[MEMFS_PAGE_SIZE];
	MemfsPage* new_page = NULL;
	MemfsPageGroup* group;
	MemfsResult result;

	if (node->small_capacity == 0)
		return MEMFS_OK;

	memset(plain, 0, sizeof(plain));

	result = memfs_page_decode(node, UINT64_MAX, node->small_page, plain, (uint16_t)node->small_capacity);
	if (result != MEMFS_OK)
		return result;

	result = memfs_page_encode(node, 0, plain, MEMFS_PAGE_SIZE, &new_page);
	if (result != MEMFS_OK)
		return result;

	if (new_page) {
		result = memfs_storage_ensure_group(node, 0, &group);
		if (result != MEMFS_OK) {
			free(new_page);
			return result;
		}

		memfs_storage_replace_page(node, group, 0, new_page);
	}

	memfs_storage_replace_small(node, NULL, 0);
	return MEMFS_OK;
}

static MemfsResult memfs_storage_trim_small(MemfsNode* node, uint64_t new_size) {
	uint8_t plain[MEMFS_SMALL_LIMIT];
	uint32_t target = memfs_small_capacity(new_size);
	MemfsPage* new_page = NULL;
	MemfsResult result;

	if (node->small_capacity == 0) {
		node->small_capacity = target;
		return MEMFS_OK;
	}

	if (target == 0) {
		memfs_storage_replace_small(node, NULL, 0);
		return MEMFS_OK;
	}

	memset(plain, 0, sizeof(plain));

	result = memfs_page_decode(node, UINT64_MAX, node->small_page, plain, (uint16_t)node->small_capacity);
	if (result != MEMFS_OK)
		return result;

	if (new_size < target) {
		memset(plain + new_size, 0, (size_t)(target - new_size));
	}

	result = memfs_page_encode(node, UINT64_MAX, plain, (uint16_t)target, &new_page);
	if (result != MEMFS_OK)
		return result;

	memfs_storage_replace_small(node, new_page, target);
	return MEMFS_OK;
}

static MemfsResult memfs_storage_trim_pages(MemfsNode* node, uint64_t new_size) {
	uint64_t first_free_page = (new_size + MEMFS_PAGE_MASK) >> MEMFS_PAGE_SHIFT;
	uint64_t tail_page = new_size >> MEMFS_PAGE_SHIFT;
	uint32_t group_index;

	if ((new_size & MEMFS_PAGE_MASK) != 0) {
		uint64_t group_index64 = tail_page >> MEMFS_PAGE_GROUP_SHIFT;
		MemfsPageGroup* group = memfs_storage_group(node, group_index64);

		if (group) {
			uint32_t slot = (uint32_t)(tail_page & MEMFS_PAGE_GROUP_MASK);
			MemfsPage* old_page = group->pages[slot];

			if (old_page) {
				uint8_t plain[MEMFS_PAGE_SIZE];
				MemfsPage* new_page = NULL;
				MemfsResult result;
				uint32_t in_page = (uint32_t)(new_size & MEMFS_PAGE_MASK);

				result = memfs_page_decode(node, tail_page, old_page, plain, MEMFS_PAGE_SIZE);
				if (result != MEMFS_OK)
					return result;

				memset(plain + in_page, 0, MEMFS_PAGE_SIZE - in_page);

				result = memfs_page_encode(node, tail_page, plain, MEMFS_PAGE_SIZE, &new_page);
				if (result != MEMFS_OK)
					return result;

				memfs_storage_replace_page(node, group, slot, new_page);
			}
		}
	}

	for (group_index = 0; group_index < node->page_group_capacity; group_index++) {
		MemfsPageGroup* group = node->page_groups[group_index];
		uint32_t slot;

		if (group == NULL)
			continue;

		for (slot = 0; slot < MEMFS_PAGES_PER_GROUP; slot++) {
			uint64_t page_index = ((uint64_t)group_index << MEMFS_PAGE_GROUP_SHIFT) | slot;
			MemfsPage* page;

			if (page_index < first_free_page)
				continue;

			page = group->pages[slot];
			if (page == NULL)
				continue;

			memfs_storage_replace_page(node, group, slot, NULL);
		}

		if (group->present_pages == 0) {
			free(group);
			node->page_groups[group_index] = NULL;
		}
	}

	memfs_storage_compact_groups(node);
	return MEMFS_OK;
}

static void memfs_storage_try_demote(MemfsNode* node, uint64_t new_size) {
	uint32_t target;
	uint8_t plain[MEMFS_SMALL_LIMIT];
	MemfsPage* new_page = NULL;
	MemfsPage* first_page;
	MemfsResult result;

	if (node->page_groups == NULL || new_size > MEMFS_SMALL_LIMIT) {
		return;
	}

	target = memfs_small_capacity(new_size);
	if (target == 0 && new_size != 0)
		return;

	memset(plain, 0, sizeof(plain));

	first_page = memfs_storage_page(node, 0);
	if (first_page) {
		result = memfs_page_decode(node, 0, first_page, plain, MEMFS_PAGE_SIZE);
		if (result != MEMFS_OK)
			return;
	}

	if (target) {
		result = memfs_page_encode(node, UINT64_MAX, plain, (uint16_t)target, &new_page);
		if (result != MEMFS_OK)
			return;
	}

	memfs_storage_destroy_pages(node);
	memfs_storage_replace_small(node, new_page, target);
}

static MemfsResult memfs_storage_trim_data(MemfsNode* node, uint64_t new_size) {
	MemfsResult result;

	if (node->page_groups) {
		result = memfs_storage_trim_pages(node, new_size);
		if (result != MEMFS_OK)
			return result;

		memfs_storage_try_demote(node, new_size);
		return MEMFS_OK;
	}

	return memfs_storage_trim_small(node, new_size);
}

static MemfsResult memfs_storage_read_range(MemfsNode* node, uint8_t* buffer, uint64_t offset, uint64_t length) {
	uint64_t done = 0;

	if (length == 0)
		return MEMFS_OK;

	if (node->page_groups == NULL) {
		uint64_t available = 0;
		uint8_t plain[MEMFS_SMALL_LIMIT];
		MemfsResult result;

		memset(buffer, 0, (size_t)length);

		if (node->small_capacity == 0 || offset >= node->small_capacity) {
			return MEMFS_OK;
		}

		memset(plain, 0, sizeof(plain));
		result = memfs_page_decode(node, UINT64_MAX, node->small_page, plain, (uint16_t)node->small_capacity);
		if (result != MEMFS_OK)
			return result;

		available = node->small_capacity - offset;
		if (available > length)
			available = length;

		memcpy(buffer, plain + offset, (size_t)available);
		return MEMFS_OK;
	}

	while (done < length) {
		uint64_t pos = offset + done;
		uint64_t page_index = pos >> MEMFS_PAGE_SHIFT;
		uint32_t in_page = (uint32_t)(pos & MEMFS_PAGE_MASK);
		uint32_t span = MEMFS_PAGE_SIZE - in_page;
		MemfsPage* page = memfs_storage_page(node, page_index);

		if (span > length - done)
			span = (uint32_t)(length - done);

		if (page == NULL) {
			memset(buffer + done, 0, span);
		} else if (page->flags == 0 && page->plain_size == MEMFS_PAGE_SIZE && page->stored_size == MEMFS_PAGE_SIZE) {
			memcpy(buffer + done, page->data + in_page, span);
		} else {
			uint8_t plain[MEMFS_PAGE_SIZE];
			MemfsResult result = memfs_page_decode(node, page_index, page, plain, MEMFS_PAGE_SIZE);

			if (result != MEMFS_OK)
				return result;

			memcpy(buffer + done, plain + in_page, span);
		}

		done += span;
	}

	return MEMFS_OK;
}

static MemfsResult memfs_storage_write_small(MemfsNode* node, const uint8_t* buffer, uint64_t offset, uint64_t length) {
	uint64_t end = offset + length;
	uint32_t target = memfs_small_capacity(end);
	uint8_t plain[MEMFS_SMALL_LIMIT];
	MemfsPage* new_page = NULL;
	MemfsResult result;

	if (target == 0)
		return MEMFS_ERR_INVALID;

	memset(plain, 0, sizeof(plain));

	if (node->small_capacity) {
		result = memfs_page_decode(node, UINT64_MAX, node->small_page, plain, (uint16_t)node->small_capacity);
		if (result != MEMFS_OK)
			return result;
	}

	memcpy(plain + offset, buffer, (size_t)length);

	result = memfs_page_encode(node, UINT64_MAX, plain, (uint16_t)target, &new_page);
	if (result != MEMFS_OK)
		return result;

	memfs_storage_replace_small(node, new_page, target);
	return MEMFS_OK;
}

static MemfsResult memfs_storage_write_raw_pages(MemfsNode* node, const uint8_t* buffer, uint64_t offset,
												 uint64_t length) {
	uint64_t end = offset + length;
	uint64_t first_page = offset >> MEMFS_PAGE_SHIFT;
	uint64_t last_page = (end - 1U) >> MEMFS_PAGE_SHIFT;
	uint64_t page_index;
	uint64_t done = 0;
	MemfsResult result;

	for (page_index = first_page; page_index <= last_page; page_index++) {
		uint64_t group_index = page_index >> MEMFS_PAGE_GROUP_SHIFT;
		uint32_t slot = (uint32_t)(page_index & MEMFS_PAGE_GROUP_MASK);
		MemfsPageGroup* group;
		MemfsPage* page = memfs_storage_page(node, page_index);

		if (page == NULL) {
			page = calloc(1, sizeof(*page) + MEMFS_PAGE_SIZE);
			if (page == NULL)
				return MEMFS_ERR_NO_MEMORY;

			page->stored_size = MEMFS_PAGE_SIZE;
			page->plain_size = MEMFS_PAGE_SIZE;

			result = memfs_storage_ensure_group(node, group_index, &group);
			if (result != MEMFS_OK) {
				free(page);
				return result;
			}

			memfs_storage_replace_page(node, group, slot, page);
		} else if (page->flags != 0 || page->plain_size != MEMFS_PAGE_SIZE || page->stored_size != MEMFS_PAGE_SIZE) {
			return MEMFS_ERR_DATA;
		}
	}

	while (done < length) {
		uint64_t pos = offset + done;
		page_index = pos >> MEMFS_PAGE_SHIFT;
		uint32_t in_page = (uint32_t)(pos & MEMFS_PAGE_MASK);
		uint32_t span = MEMFS_PAGE_SIZE - in_page;
		MemfsPage* page = memfs_storage_page(node, page_index);

		if (span > length - done)
			span = (uint32_t)(length - done);

		memcpy(page->data + in_page, buffer + done, span);
		done += span;
	}

	return MEMFS_OK;
}

static MemfsResult memfs_storage_write_encoded_pages(MemfsNode* node, const uint8_t* buffer, uint64_t offset,
													 uint64_t length) {
	uint64_t end = offset + length;
	uint64_t first_page = offset >> MEMFS_PAGE_SHIFT;
	uint64_t last_page = (end - 1U) >> MEMFS_PAGE_SHIFT;
	uint64_t page_count = last_page - first_page + 1U;
	MemfsPage** replacements;
	uint64_t i;
	MemfsResult result = MEMFS_OK;

	if (page_count > SIZE_MAX / sizeof(*replacements))
		return MEMFS_ERR_NO_MEMORY;

	replacements = calloc((size_t)page_count, sizeof(*replacements));
	if (replacements == NULL)
		return MEMFS_ERR_NO_MEMORY;

	for (i = 0; i < page_count; i++) {
		uint64_t page_index = first_page + i;
		uint64_t page_start = page_index << MEMFS_PAGE_SHIFT;
		uint64_t write_start = offset > page_start ? offset : page_start;
		uint64_t page_end = page_start + MEMFS_PAGE_SIZE;
		uint64_t write_end = end < page_end ? end : page_end;
		uint32_t in_page = (uint32_t)(write_start - page_start);
		uint32_t span = (uint32_t)(write_end - write_start);
		uint8_t plain[MEMFS_PAGE_SIZE];
		MemfsPage* old_page = memfs_storage_page(node, page_index);

		result = memfs_page_decode(node, page_index, old_page, plain, MEMFS_PAGE_SIZE);
		if (result != MEMFS_OK)
			goto exit;

		memcpy(plain + in_page, buffer + (write_start - offset), span);

		result = memfs_page_encode(node, page_index, plain, MEMFS_PAGE_SIZE, &replacements[i]);
		if (result != MEMFS_OK)
			goto exit;
	}

	// 先把所有可能需要的新 group 都准备好；此阶段不会替换旧数据。
	for (i = 0; i < page_count; i++) {
		uint64_t page_index = first_page + i;
		uint64_t group_index = page_index >> MEMFS_PAGE_GROUP_SHIFT;
		MemfsPageGroup* group = memfs_storage_group(node, group_index);

		if (replacements[i] && group == NULL) {
			result = memfs_storage_ensure_group(node, group_index, &group);
			if (result != MEMFS_OK)
				goto exit;
		}
	}

	// 所有编码和 metadata 分配都成功以后才提交替换。
	for (i = 0; i < page_count; i++) {
		uint64_t page_index = first_page + i;
		uint64_t group_index = page_index >> MEMFS_PAGE_GROUP_SHIFT;
		uint32_t slot = (uint32_t)(page_index & MEMFS_PAGE_GROUP_MASK);
		MemfsPageGroup* group = memfs_storage_group(node, group_index);

		if (group) {
			memfs_storage_replace_page(node, group, slot, replacements[i]);
			replacements[i] = NULL;

			if (group->present_pages == 0) {
				free(group);
				node->page_groups[group_index] = NULL;
			}
		}
	}

	memfs_storage_compact_groups(node);

exit:
	for (i = 0; i < page_count; i++)
		free(replacements[i]);
	free(replacements);
	return result;
}

static MemfsResult memfs_storage_write_range(MemfsNode* node, const uint8_t* buffer, uint64_t offset, uint64_t length) {
	uint64_t end = offset + length;
	MemfsResult result;

	if (length == 0)
		return MEMFS_OK;

	if (end <= MEMFS_SMALL_LIMIT && node->page_groups == NULL) {
		return memfs_storage_write_small(node, buffer, offset, length);
	}

	result = memfs_storage_promote(node);
	if (result != MEMFS_OK)
		return result;

	if (!node->fs->compression_enabled && !node->fs->encryption_enabled) {
		return memfs_storage_write_raw_pages(node, buffer, offset, length);
	}

	return memfs_storage_write_encoded_pages(node, buffer, offset, length);
}
static void memfs_node_free(MemfsNode* node) {
	Memfs* fs;

	if (node == NULL)
		return;

	fs = node->fs;

	memfs_storage_destroy(node);

	AcquireSRWLockExclusive(&fs->accounting_lock);
	if (node->allocation_size <= fs->used_bytes)
		fs->used_bytes -= node->allocation_size;
	else
		fs->used_bytes = 0;
	ReleaseSRWLockExclusive(&fs->accounting_lock);

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

#define MEMFS_DIR_HASH_TOMBSTONE ((MemfsNode*)(uintptr_t)1)

static uint32_t memfs_name_hash(const wchar_t* name) {
	uint32_t hash = 2166136261U;

	while (*name) {
		uint32_t c = (uint32_t)towlower(*name++);

		hash ^= c;
		hash *= 16777619U;
	}

	return hash;
}

static MemfsNode* memfs_dir_hash_lookup(MemfsDirHash* hash, const wchar_t* name) {
	uint32_t mask = hash->capacity - 1U;
	uint32_t slot = memfs_name_hash(name) & mask;
	uint32_t i;

	for (i = 0; i < hash->capacity; i++) {
		MemfsNode* node = hash->slots[slot];

		if (node == NULL)
			return NULL;

		if (node != MEMFS_DIR_HASH_TOMBSTONE && _wcsicmp(node->name, name) == 0) {
			return node;
		}

		slot = (slot + 1U) & mask;
	}

	return NULL;
}

static bool memfs_dir_hash_insert_node(MemfsDirHash* hash, MemfsNode* node) {
	uint32_t mask = hash->capacity - 1U;
	uint32_t slot = memfs_name_hash(node->name) & mask;
	uint32_t tombstone = UINT32_MAX;
	uint32_t i;

	for (i = 0; i < hash->capacity; i++) {
		MemfsNode* current = hash->slots[slot];

		if (current == NULL) {
			if (tombstone != UINT32_MAX) {
				slot = tombstone;
				hash->tombstones--;
			}

			hash->slots[slot] = node;
			hash->count++;
			return true;
		}

		if (current == MEMFS_DIR_HASH_TOMBSTONE) {
			if (tombstone == UINT32_MAX)
				tombstone = slot;
		} else if (current == node) {
			return true;
		}

		slot = (slot + 1U) & mask;
	}

	if (tombstone != UINT32_MAX) {
		hash->slots[tombstone] = node;
		hash->tombstones--;
		hash->count++;
		return true;
	}

	return false;
}

static void memfs_dir_hash_fill_tree(MemfsDirHash* hash, MemfsNode* node, bool* ok) {
	if (node == NULL || !*ok)
		return;

	memfs_dir_hash_fill_tree(hash, node->tree_left, ok);

	if (*ok && !memfs_dir_hash_insert_node(hash, node))
		*ok = false;

	memfs_dir_hash_fill_tree(hash, node->tree_right, ok);
}

static bool memfs_dir_hash_rebuild(MemfsDir* dir, uint32_t capacity) {
	MemfsDirHash* hash;
	bool ok = true;

	if (capacity < 128U)
		capacity = 128U;

	if (capacity & (capacity - 1U))
		return false;

	hash = calloc(1, sizeof(*hash));
	if (hash == NULL)
		return false;

	hash->slots = calloc(capacity, sizeof(*hash->slots));
	if (hash->slots == NULL) {
		free(hash);
		return false;
	}

	hash->capacity = capacity;
	memfs_dir_hash_fill_tree(hash, dir->root, &ok);

	if (!ok || hash->count != dir->child_count) {
		free(hash->slots);
		free(hash);
		return false;
	}

	if (dir->hash) {
		free(dir->hash->slots);
		free(dir->hash);
	}

	dir->hash = hash;
	return true;
}

static void memfs_dir_hash_disable(MemfsDir* dir) {
	if (dir->hash == NULL)
		return;

	free(dir->hash->slots);
	free(dir->hash);
	dir->hash = NULL;
}

static void memfs_dir_hash_after_insert(MemfsDir* dir, MemfsNode* node) {
	MemfsDirHash* hash = dir->hash;

	if (hash == NULL) {
		if (dir->child_count >= MEMFS_DIR_HASH_THRESHOLD)
			(void)memfs_dir_hash_rebuild(dir, 128U);
		return;
	}

	if ((uint64_t)(hash->count + hash->tombstones + 1U) * 10ULL >= (uint64_t)hash->capacity * 7ULL) {
		if (hash->capacity <= UINT32_MAX / 2U && memfs_dir_hash_rebuild(dir, hash->capacity << 1)) {
			return;
		}
	}

	hash = dir->hash;
	if (!memfs_dir_hash_insert_node(hash, node))
		memfs_dir_hash_disable(dir);
}

static void memfs_dir_hash_before_remove(MemfsDir* dir, MemfsNode* node) {
	MemfsDirHash* hash = dir->hash;
	uint32_t mask;
	uint32_t slot;
	uint32_t i;

	if (hash == NULL)
		return;

	mask = hash->capacity - 1U;
	slot = memfs_name_hash(node->name) & mask;

	for (i = 0; i < hash->capacity; i++) {
		MemfsNode* current = hash->slots[slot];

		if (current == NULL)
			break;

		if (current == node) {
			hash->slots[slot] = MEMFS_DIR_HASH_TOMBSTONE;
			hash->count--;
			hash->tombstones++;
			break;
		}

		slot = (slot + 1U) & mask;
	}
}

static void memfs_dir_hash_after_remove(MemfsDir* dir) {
	MemfsDirHash* hash = dir->hash;

	if (hash == NULL)
		return;

	if (dir->child_count < MEMFS_DIR_HASH_THRESHOLD / 2U) {
		memfs_dir_hash_disable(dir);
		return;
	}

	if (hash->tombstones > hash->count / 2U)
		(void)memfs_dir_hash_rebuild(dir, hash->capacity);
}

static uint32_t memfs_treap_priority(uint64_t index_number) {
	uint64_t x = index_number + 0x9e3779b97f4a7c15ULL;

	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	x ^= x >> 31;
	return (uint32_t)(x ^ (x >> 32));
}

static void memfs_tree_replace_parent_child(MemfsDir* dir, MemfsNode* parent, MemfsNode* old_child,
											MemfsNode* new_child) {
	if (parent == NULL) {
		dir->root = new_child;
	} else if (parent->tree_left == old_child) {
		parent->tree_left = new_child;
	} else {
		parent->tree_right = new_child;
	}

	if (new_child)
		new_child->tree_parent = parent;
}

static void memfs_tree_rotate_left(MemfsDir* dir, MemfsNode* node) {
	MemfsNode* right = node->tree_right;
	MemfsNode* parent = node->tree_parent;

	node->tree_right = right->tree_left;
	if (right->tree_left)
		right->tree_left->tree_parent = node;

	right->tree_left = node;
	node->tree_parent = right;

	memfs_tree_replace_parent_child(dir, parent, node, right);
}

static void memfs_tree_rotate_right(MemfsDir* dir, MemfsNode* node) {
	MemfsNode* left = node->tree_left;
	MemfsNode* parent = node->tree_parent;

	node->tree_left = left->tree_right;
	if (left->tree_right)
		left->tree_right->tree_parent = node;

	left->tree_right = node;
	node->tree_parent = left;

	memfs_tree_replace_parent_child(dir, parent, node, left);
}

static void memfs_dir_insert(MemfsNode* parent, MemfsNode* node) {
	MemfsDir* dir = parent->dir;
	MemfsNode* current = dir->root;
	MemfsNode* tree_parent = NULL;
	int cmp = 0;

	node->tree_left = NULL;
	node->tree_right = NULL;
	node->tree_parent = NULL;

	while (current) {
		tree_parent = current;
		cmp = _wcsicmp(node->name, current->name);
		current = cmp < 0 ? current->tree_left : current->tree_right;
	}

	node->tree_parent = tree_parent;
	if (tree_parent == NULL) {
		dir->root = node;
	} else if (cmp < 0) {
		tree_parent->tree_left = node;
	} else {
		tree_parent->tree_right = node;
	}

	while (node->tree_parent && node->tree_parent->tree_priority > node->tree_priority) {
		if (node->tree_parent->tree_left == node)
			memfs_tree_rotate_right(dir, node->tree_parent);
		else
			memfs_tree_rotate_left(dir, node->tree_parent);
	}

	dir->child_count++;
	node->parent = parent;
	memfs_dir_hash_after_insert(dir, node);
}

static void memfs_dir_remove(MemfsNode* node) {
	MemfsNode* parent = node->parent;
	MemfsDir* dir;

	if (parent == NULL || parent->dir == NULL)
		return;

	dir = parent->dir;
	memfs_dir_hash_before_remove(dir, node);

	while (node->tree_left || node->tree_right) {
		if (node->tree_left == NULL) {
			memfs_tree_rotate_left(dir, node);
		} else if (node->tree_right == NULL) {
			memfs_tree_rotate_right(dir, node);
		} else if (node->tree_left->tree_priority < node->tree_right->tree_priority) {
			memfs_tree_rotate_right(dir, node);
		} else {
			memfs_tree_rotate_left(dir, node);
		}
	}

	memfs_tree_replace_parent_child(dir, node->tree_parent, node, NULL);

	if (dir->child_count)
		dir->child_count--;

	memfs_dir_hash_after_remove(dir);

	node->tree_left = NULL;
	node->tree_right = NULL;
	node->tree_parent = NULL;
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
	node->tree_priority = memfs_treap_priority(node->index_number);
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
	MemfsResult result;

	if (allocation_size == old_size)
		return MEMFS_OK;

	if (allocation_size > old_size) {
		uint64_t delta = allocation_size - old_size;

		AcquireSRWLockExclusive(&fs->accounting_lock);

		if (fs->used_bytes > fs->capacity || delta > fs->capacity - fs->used_bytes) {
			ReleaseSRWLockExclusive(&fs->accounting_lock);
			return MEMFS_ERR_NO_SPACE;
		}

		fs->used_bytes += delta;
		ReleaseSRWLockExclusive(&fs->accounting_lock);
	} else {
		uint64_t delta = old_size - allocation_size;

		result = memfs_storage_trim_data(node, allocation_size);
		if (result != MEMFS_OK)
			return result;

		AcquireSRWLockExclusive(&fs->accounting_lock);
		if (delta > fs->used_bytes)
			fs->used_bytes = 0;
		else
			fs->used_bytes -= delta;
		ReleaseSRWLockExclusive(&fs->accounting_lock);
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

uint64_t memfs_free_bytes(Memfs* fs) {
	uint64_t free_bytes;

	if (fs == NULL)
		return 0;

	AcquireSRWLockShared(&fs->accounting_lock);
	free_bytes = fs->used_bytes <= fs->capacity ? fs->capacity - fs->used_bytes : 0;
	ReleaseSRWLockShared(&fs->accounting_lock);
	return free_bytes;
}

uint64_t memfs_resident_bytes(Memfs* fs) {
	uint64_t resident;

	if (fs == NULL)
		return 0;

	AcquireSRWLockShared(&fs->accounting_lock);
	resident = fs->resident_bytes;
	ReleaseSRWLockShared(&fs->accounting_lock);
	return resident;
}

MemfsResult memfs_create_ex(const MemfsOptions* options, Memfs** out_fs) {
	Memfs* fs;
	MemfsNode* root;
	MemfsResult result;
	const wchar_t* volume_label;
	size_t label_chars;

	if (options == NULL || out_fs == NULL || options->capacity == 0) {
		return MEMFS_ERR_INVALID;
	}

	if (options->encryption_key && options->encryption_key_size != MEMFS_ENCRYPTION_KEY_SIZE) {
		return MEMFS_ERR_INVALID;
	}

	if ((options->encryption_enabled || options->encryption_key) && sodium_init() < 0) {
		return MEMFS_ERR_ACCESS;
	}

	fs = calloc(1, sizeof(*fs));
	if (fs == NULL)
		return MEMFS_ERR_NO_MEMORY;

	InitializeSRWLock(&fs->accounting_lock);

	fs->capacity = options->capacity;
	fs->next_index = 1;
	fs->compression_enabled = options->compression_enabled;
	fs->compression_level = options->compression_level ? options->compression_level : 1;

	if (fs->compression_enabled) {
		if (fs->compression_level < ZSTD_minCLevel())
			fs->compression_level = ZSTD_minCLevel();
		if (fs->compression_level > ZSTD_maxCLevel())
			fs->compression_level = ZSTD_maxCLevel();
	}

	fs->encryption_enabled = options->encryption_enabled || options->encryption_key != NULL;

	if (fs->encryption_enabled) {
		if (options->encryption_key) {
			memcpy(fs->encryption_key, options->encryption_key, MEMFS_ENCRYPTION_KEY_SIZE);
		} else {
			randombytes_buf(fs->encryption_key, MEMFS_ENCRYPTION_KEY_SIZE);
		}

		(void)sodium_mlock(fs->encryption_key, MEMFS_ENCRYPTION_KEY_SIZE);
	}

	volume_label = options->volume_label;
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
		if (fs->encryption_enabled) {
			sodium_memzero(fs->encryption_key, MEMFS_ENCRYPTION_KEY_SIZE);
			(void)sodium_munlock(fs->encryption_key, MEMFS_ENCRYPTION_KEY_SIZE);
		}
		free(fs);
		return result;
	}

	fs->root = root;
	*out_fs = fs;
	return MEMFS_OK;
}

MemfsResult memfs_create(uint64_t capacity, const wchar_t* volume_label, Memfs** out_fs) {
	MemfsOptions options;

	memset(&options, 0, sizeof(options));
	options.capacity = capacity;
	options.volume_label = volume_label;
	return memfs_create_ex(&options, out_fs);
}
void memfs_destroy(Memfs* fs) {
	MemfsNode* node;

	if (fs == NULL)
		return;

	while ((node = fs->all_head) != NULL)
		memfs_node_free(node);

	if (fs->encryption_enabled) {
		sodium_memzero(fs->encryption_key, MEMFS_ENCRYPTION_KEY_SIZE);
		(void)sodium_munlock(fs->encryption_key, MEMFS_ENCRYPTION_KEY_SIZE);
	}

	free(fs);
}

MemfsNode* memfs_dir_lookup(MemfsNode* dir_node, const wchar_t* name) {
	MemfsNode* node;

	if (dir_node == NULL || dir_node->dir == NULL || name == NULL)
		return NULL;

	if (dir_node->dir->hash) {
		node = memfs_dir_hash_lookup(dir_node->dir->hash, name);
		if (node)
			return node->deleted ? NULL : node;
	}

	node = dir_node->dir->root;
	while (node) {
		int cmp = _wcsicmp(name, node->name);

		if (cmp == 0)
			return node->deleted ? NULL : node;

		node = cmp < 0 ? node->tree_left : node->tree_right;
	}

	return NULL;
}

MemfsNode* memfs_dir_first(MemfsNode* dir_node) {
	MemfsNode* node;

	if (dir_node == NULL || dir_node->dir == NULL)
		return NULL;

	node = dir_node->dir->root;
	if (node == NULL)
		return NULL;

	while (node->tree_left)
		node = node->tree_left;

	return node;
}

MemfsNode* memfs_dir_next(MemfsNode* node) {
	MemfsNode* parent;

	if (node == NULL)
		return NULL;

	if (node->tree_right) {
		node = node->tree_right;
		while (node->tree_left)
			node = node->tree_left;
		return node;
	}

	parent = node->tree_parent;
	while (parent && parent->tree_right == node) {
		node = parent;
		parent = parent->tree_parent;
	}
	return parent;
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

	if (new_size < old_file_size) {
		result = memfs_storage_trim_data(node, new_size);
		if (result != MEMFS_OK)
			return result;
	}

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

	{
		MemfsResult result = memfs_storage_read_range(node, buffer, offset, end - offset);

		if (result != MEMFS_OK)
			return result;
	}

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
