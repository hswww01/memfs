#include "memfs_core.h"
#include "memfs_object.h"
#include "memfs_meta_table.h"

#include <intrin.h>
#include <sddl.h>
#include <sodium.h>
#include <zstd.h>
#include <wctype.h>

#define MEMFS_DEFAULT_SDDL L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;WD)"

typedef struct MemfsNodeMetaValue {
	MemfsSecurity* security;
	uint64_t creation_time;
	uint64_t last_access_time;
	uint64_t last_write_time;
	uint64_t change_time;
} MemfsNodeMetaValue;

static MemfsNodeMetaValue* memfs_meta_lookup(MemfsNode* node) {
	return node && node->fs ? (MemfsNodeMetaValue*)memfs_meta_table_lookup(node->fs->meta_table, node->index_number)
							: NULL;
}
MemfsSecurity* memfs_node_get_security(MemfsNode* node) {
	MemfsNodeMetaValue* meta = memfs_meta_lookup(node);
	return meta ? meta->security : NULL;
}

uint64_t memfs_node_get_creation_time(const MemfsNode* node) {
	MemfsNodeMetaValue* meta = memfs_meta_lookup((MemfsNode*)node);
	return meta ? meta->creation_time : 0;
}

uint64_t memfs_node_get_last_access_time(const MemfsNode* node) {
	MemfsNodeMetaValue* meta = memfs_meta_lookup((MemfsNode*)node);
	return meta ? meta->last_access_time : 0;
}

uint64_t memfs_node_get_last_write_time(const MemfsNode* node) {
	MemfsNodeMetaValue* meta = memfs_meta_lookup((MemfsNode*)node);
	return meta ? meta->last_write_time : 0;
}

uint64_t memfs_node_get_change_time(const MemfsNode* node) {
	MemfsNodeMetaValue* meta = memfs_meta_lookup((MemfsNode*)node);
	return meta ? meta->change_time : 0;
}
static MemfsNodeMetaValue* memfs_meta_get_or_create(MemfsNode* node) {
	MemfsNodeMetaValue* meta;

	if (node == NULL || node->fs == NULL)
		return NULL;

	meta = memfs_meta_lookup(node);
	if (meta != NULL)
		return meta;

	meta = calloc(1, sizeof(*meta));
	if (meta == NULL)
		return NULL;


	if (!memfs_meta_table_insert(node->fs->meta_table, node->index_number, meta)) {
		free(meta);
		return (MemfsNodeMetaValue*)memfs_meta_lookup(node);
	}

	return meta;
}

static bool memfs_node_set_security_meta(MemfsNode* node, MemfsSecurity* security) {
	MemfsNodeMetaValue* meta = memfs_meta_get_or_create(node);
	if (meta == NULL)
		return false;
	meta->security = security;
	return true;
}

void memfs_node_set_creation_time(MemfsNode* node, uint64_t value) {
	MemfsNodeMetaValue* meta = memfs_meta_get_or_create(node);
	if (meta)
		meta->creation_time = value;
}
void memfs_node_set_last_access_time(MemfsNode* node, uint64_t value) {
	MemfsNodeMetaValue* meta = memfs_meta_get_or_create(node);
	if (meta)
		meta->last_access_time = value;
}
void memfs_node_set_last_write_time(MemfsNode* node, uint64_t value) {
	MemfsNodeMetaValue* meta = memfs_meta_get_or_create(node);
	if (meta)
		meta->last_write_time = value;
}
void memfs_node_set_change_time(MemfsNode* node, uint64_t value) {
	MemfsNodeMetaValue* meta = memfs_meta_get_or_create(node);
	if (meta)
		meta->change_time = value;
}

static void memfs_dir_destroy(MemfsDir* dir) {
	if (dir == NULL)
		return;

	if (dir->hash) {
		free(dir->hash->slots);
		free(dir->hash);
		dir->hash = NULL;
	}
}
static void memfs_orphan_insert(Memfs* fs, MemfsNode* node) {
	// orphan 已经不在 namespace treap 中，复用 tree_left/tree_right
	// 作为 prev/next，避免给每个普通节点额外保存一套全局链指针。
	node->tree_left = NULL;
	node->tree_right = fs->orphan_head;
	node->tree_parent = NULL;

	if (fs->orphan_head)
		fs->orphan_head->tree_left = node;

	fs->orphan_head = node;
}

static void memfs_orphan_remove(Memfs* fs, MemfsNode* node) {
	MemfsNode* prev = node->tree_left;
	MemfsNode* next = node->tree_right;

	if (prev)
		prev->tree_right = next;
	else if (fs->orphan_head == node)
		fs->orphan_head = next;

	if (next)
		next->tree_left = prev;

	node->tree_left = NULL;
	node->tree_right = NULL;
	node->tree_parent = NULL;
}
typedef struct MemfsPageAad {
	uint64_t node_index;
	uint64_t storage_index;
	uint64_t nonce_sequence;
	uint16_t plain_size;
	uint8_t flags;
	uint8_t reserved[5];
} MemfsPageAad;

static void memfs_build_nonce(Memfs* fs, uint64_t sequence,
							  uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES]) {
	memcpy(nonce, fs->encryption_nonce_prefix, MEMFS_ENCRYPTION_NONCE_PREFIX_SIZE);
	memcpy(nonce + MEMFS_ENCRYPTION_NONCE_PREFIX_SIZE, &sequence, sizeof(sequence));
}
static void memfs_security_release(MemfsSecurity* security);

static size_t memfs_page_heap_size(const MemfsPage* page) {
	if (page == NULL)
		return 0;
	return sizeof(*page) + page->stored_size;
}

static uint64_t memfs_atomic_load_u64(volatile LONG64* value) {
	return (uint64_t)InterlockedCompareExchange64(value, 0, 0);
}

static void memfs_atomic_sub_clamped(volatile LONG64* value, uint64_t delta) {
	for (;;) {
		uint64_t current = memfs_atomic_load_u64(value);
		uint64_t next = delta > current ? 0 : current - delta;

		if ((uint64_t)InterlockedCompareExchange64(value, (LONG64)next, (LONG64)current) == current) {
			return;
		}
	}
}

static bool memfs_atomic_reserve(volatile LONG64* value, uint64_t capacity, uint64_t delta) {
	for (;;) {
		uint64_t current = memfs_atomic_load_u64(value);
		uint64_t next;

		if (current > capacity || delta > capacity - current)
			return false;

		next = current + delta;
		if ((uint64_t)InterlockedCompareExchange64(value, (LONG64)next, (LONG64)current) == current) {
			return true;
		}
	}
}

static void memfs_resident_add(MemfsNode* node, uint64_t bytes) {
	if (bytes == 0)
		return;

	(void)InterlockedAdd64(&node->fs->resident_bytes, (LONG64)bytes);
}

static void memfs_resident_sub(MemfsNode* node, uint64_t bytes) {
	if (bytes == 0)
		return;

	memfs_atomic_sub_clamped(&node->fs->resident_bytes, bytes);
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

#define MEMFS_COMPRESSION_SKIP_SCORE 4
#define MEMFS_COMPRESSION_PROBE_MASK 15ULL

static int8_t memfs_storage_get_compression_score(MemfsNode* node) {
	return node ? node->compression_score : 0;
}

static void memfs_storage_adjust_compression_score(MemfsNode* node, bool useful) {
	if (node == NULL)
		return;
	if (useful) {
		if (node->compression_score > -8)
			node->compression_score--;
	} else if (node->compression_score < 8) {
		node->compression_score++;
	}
}

static bool memfs_compression_should_try(MemfsNode* node, uint64_t storage_index, uint16_t plain_size) {
	if (!node->fs->compression_enabled || plain_size < 128U)
		return false;

	if (memfs_storage_get_compression_score(node) < MEMFS_COMPRESSION_SKIP_SCORE)
		return true;

	// Tiny blobs are cheap and infrequently rewritten, so always sample them.
	// For paged files with poor compression history, probe one page out of 16.
	if (storage_index == UINT64_MAX)
		return true;

	return 0 == (storage_index & MEMFS_COMPRESSION_PROBE_MASK);
}

static void memfs_compression_feedback(MemfsNode* node, bool useful) {
	memfs_storage_adjust_compression_score(node, useful);
}

static MemfsResult memfs_page_encode(MemfsNode* node, uint64_t storage_index, const uint8_t* plain, uint16_t plain_size,
									 MemfsPage** out_page) {
	uint8_t compressed[ZSTD_COMPRESSBOUND(MEMFS_PAGE_SIZE)];
	const uint8_t* payload = plain;
	size_t payload_size = plain_size;
	uint8_t flags = 0;
	size_t stored_size;
	bool compression_useful = false;
	MemfsPage* page;

	*out_page = NULL;

	if (plain_size == 0 || memfs_buffer_is_zero(plain, plain_size))
		return MEMFS_OK;

	if (memfs_compression_should_try(node, storage_index, plain_size)) {
		size_t compressed_size;

		compressed_size = ZSTD_compress(compressed, sizeof(compressed), plain, plain_size, node->fs->compression_level);

		if (!ZSTD_isError(compressed_size) && compressed_size + 32U < plain_size) {
			payload = compressed;
			payload_size = compressed_size;
			flags |= MEMFS_PAGE_COMPRESSED;
			compression_useful = true;
		}

		memfs_compression_feedback(node, compression_useful);
	}

	if (node->fs->encryption_enabled) {
		stored_size = sizeof(uint64_t) + payload_size + crypto_aead_xchacha20poly1305_ietf_ABYTES;
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
		uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES];
		uint8_t* sequence_data = page->data;
		uint8_t* cipher = page->data + sizeof(uint64_t);
		unsigned long long cipher_size = 0;
		LONG64 sequence_signed = InterlockedIncrement64(&node->fs->encryption_nonce_counter);
		uint64_t sequence;

		if (sequence_signed <= 0) {
			free(page);
			return MEMFS_ERR_DATA;
		}

		sequence = (uint64_t)sequence_signed;
		page->flags |= MEMFS_PAGE_ENCRYPTED;

		memcpy(sequence_data, &sequence, sizeof(sequence));
		memfs_build_nonce(node->fs, sequence, nonce);

		memset(&aad, 0, sizeof(aad));
		aad.node_index = node->index_number;
		aad.storage_index = storage_index;
		aad.nonce_sequence = sequence;
		aad.plain_size = plain_size;
		aad.flags = page->flags;

		if (0 != crypto_aead_xchacha20poly1305_ietf_encrypt(
					 cipher, &cipher_size, payload, (unsigned long long)payload_size, (const unsigned char*)&aad,
					 sizeof(aad), NULL, nonce, node->fs->encryption_key)) {
			sodium_memzero(nonce, sizeof(nonce));
			free(page);
			return MEMFS_ERR_DATA;
		}

		sodium_memzero(nonce, sizeof(nonce));

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
		uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES];
		unsigned long long decoded_size = 0;
		uint64_t sequence;
		const uint8_t* cipher;
		size_t cipher_size;

		if (!node->fs->encryption_enabled)
			return MEMFS_ERR_DATA;

		if (payload_size < sizeof(uint64_t) + crypto_aead_xchacha20poly1305_ietf_ABYTES) {
			return MEMFS_ERR_DATA;
		}

		memcpy(&sequence, payload, sizeof(sequence));
		cipher = payload + sizeof(sequence);
		cipher_size = payload_size - sizeof(sequence);
		memfs_build_nonce(node->fs, sequence, nonce);

		memset(&aad, 0, sizeof(aad));
		aad.node_index = node->index_number;
		aad.storage_index = storage_index;
		aad.nonce_sequence = sequence;
		aad.plain_size = page->plain_size;
		aad.flags = page->flags;

		if (0 != crypto_aead_xchacha20poly1305_ietf_decrypt(stage, &decoded_size, NULL, cipher,
															(unsigned long long)cipher_size, (const unsigned char*)&aad,
															sizeof(aad), nonce, node->fs->encryption_key)) {
			sodium_memzero(nonce, sizeof(nonce));
			return MEMFS_ERR_DATA;
		}

		sodium_memzero(nonce, sizeof(nonce));

		if (decoded_size > sizeof(stage))
			return MEMFS_ERR_DATA;

		payload = stage;
		payload_size = (size_t)decoded_size;
	} else if (node->fs->encryption_enabled) {
		return MEMFS_ERR_DATA;
	}

	if (page->flags & MEMFS_PAGE_COMPRESSED) {
		size_t decoded = ZSTD_decompress(plain, expected_plain_size, payload, payload_size);

		if (ZSTD_isError(decoded) || decoded != expected_plain_size)
			return MEMFS_ERR_DATA;

		return MEMFS_OK;
	}

	if (payload_size != expected_plain_size)
		return MEMFS_ERR_DATA;

	memcpy(plain, payload, expected_plain_size);
	return MEMFS_OK;
}
static uint32_t memfs_popcount64(uint64_t value) {
#if defined(_MSC_VER)
	return (uint32_t)__popcnt64(value);
#else
	return (uint32_t)__builtin_popcountll(value);
#endif
}

static bool memfs_group_has(const MemfsPageGroup* group, uint32_t slot) {
	uint32_t word = slot >> 6;
	uint32_t bit = slot & 63U;

	return 0 != (group->bitmap[word] & (1ULL << bit));
}

static uint32_t memfs_group_rank(const MemfsPageGroup* group, uint32_t slot) {
	uint32_t word = slot >> 6;
	uint32_t bit = slot & 63U;
	uint32_t rank = 0;
	uint32_t i;

	for (i = 0; i < word; i++)
		rank += memfs_popcount64(group->bitmap[i]);

	if (bit) {
		uint64_t mask = (1ULL << bit) - 1ULL;
		rank += memfs_popcount64(group->bitmap[word] & mask);
	}

	return rank;
}

static MemfsPage* memfs_group_get(const MemfsPageGroup* group, uint32_t slot) {
	if (group == NULL || !memfs_group_has(group, slot))
		return NULL;

	return group->pages[memfs_group_rank(group, slot)];
}

static MemfsResult memfs_group_reserve(MemfsPageGroup* group, uint32_t required) {
	MemfsPage** pages;
	uint32_t capacity;

	if (required <= group->page_capacity)
		return MEMFS_OK;
	if (required > MEMFS_PAGES_PER_GROUP)
		return MEMFS_ERR_INVALID;

	capacity = group->page_capacity ? group->page_capacity : 1U;
	while (capacity < required) {
		if (capacity >= MEMFS_PAGES_PER_GROUP / 2U) {
			capacity = MEMFS_PAGES_PER_GROUP;
			break;
		}
		capacity <<= 1;
	}

	pages = realloc(group->pages, (size_t)capacity * sizeof(*pages));
	if (pages == NULL)
		return MEMFS_ERR_NO_MEMORY;

	group->pages = pages;
	group->page_capacity = (uint16_t)capacity;
	return MEMFS_OK;
}

static void memfs_group_try_shrink(MemfsPageGroup* group) {
	MemfsPage** pages;
	uint32_t target;

	if (group->page_count == 0) {
		free(group->pages);
		group->pages = NULL;
		group->page_capacity = 0;
		return;
	}

	if (group->page_capacity <= 1U || group->page_count * 2U > group->page_capacity) {
		return;
	}

	target = 1U;
	while (target < group->page_count)
		target <<= 1;

	pages = realloc(group->pages, (size_t)target * sizeof(*pages));
	if (pages) {
		group->pages = pages;
		group->page_capacity = (uint16_t)target;
	}
}


static uint32_t memfs_storage_group_count(MemfsNode* node) {
	return node ? node->page_group_count : 0;
}

static uint32_t memfs_storage_group_capacity(MemfsNode* node) {
	return node ? node->page_group_capacity : 0;
}

static MemfsPageGroupEntry* memfs_storage_groups(MemfsNode* node) {
	return node ? node->page_groups : NULL;
}static uint32_t memfs_storage_group_position(MemfsNode* node, uint64_t group_index, bool* found) {
	uint32_t lo = 0;
	uint32_t hi = memfs_storage_group_count(node);

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2U;
		uint64_t current = node->page_groups[mid].index;

		if (current < group_index)
			lo = mid + 1U;
		else
			hi = mid;
	}

	if (found) {
		*found = lo < memfs_storage_group_count(node) && memfs_storage_groups(node)[lo].index == group_index;
	}

	return lo;
}

static MemfsPageGroup* memfs_storage_group(MemfsNode* node, uint64_t group_index) {
	bool found;
	uint32_t pos = memfs_storage_group_position(node, group_index, &found);

	if (!found)
		return NULL;

	return node->page_groups[pos].group;
}

static MemfsResult memfs_storage_group_reserve(MemfsNode* node, uint32_t required) {
	MemfsPageGroupEntry* entries;
	uint32_t capacity;

	if (required <= node->page_group_capacity)
		return MEMFS_OK;

	capacity = node->page_group_capacity ? node->page_group_capacity : 1U;

	while (capacity < required) {
		if (capacity > UINT32_MAX / 2U) {
			capacity = required;
			break;
		}
		capacity <<= 1;
	}

	if ((size_t)capacity > SIZE_MAX / sizeof(*entries)) {
		return MEMFS_ERR_NO_MEMORY;
	}

	entries = realloc(node->page_groups, (size_t)capacity * sizeof(*entries));
	if (entries == NULL)
		return MEMFS_ERR_NO_MEMORY;

	node->page_groups = entries;
	node->page_group_capacity = capacity;
	return MEMFS_OK;
}

static void memfs_storage_group_try_shrink(MemfsNode* node) {
	MemfsPageGroupEntry* entries;
	uint32_t target;

	if (node->page_group_count == 0) {
		free(node->page_groups);
		node->page_groups = NULL;
		node->page_group_capacity = 0;
		return;
	}

	if (node->page_group_capacity <= 1U || node->page_group_count * 2U > node->page_group_capacity) {
		return;
	}

	target = 1U;
	while (target < node->page_group_count)
		target <<= 1;

	entries = realloc(node->page_groups, (size_t)target * sizeof(*entries));
	if (entries) {
		node->page_groups = entries;
		node->page_group_capacity = target;
	}
}

static MemfsPage* memfs_storage_page(MemfsNode* node, uint64_t page_index) {
	uint64_t group_index = page_index >> MEMFS_PAGE_GROUP_SHIFT;
	MemfsPageGroup* group = memfs_storage_group(node, group_index);

	if (group == NULL)
		return NULL;

	return memfs_group_get(group, (uint32_t)(page_index & MEMFS_PAGE_GROUP_MASK));
}

static MemfsResult memfs_storage_ensure_group(MemfsNode* node, uint64_t group_index, MemfsPageGroup** out_group) {
	bool found;
	uint32_t pos = memfs_storage_group_position(node, group_index, &found);
	MemfsPageGroup* group;
	MemfsResult result;

	if (found) {
		*out_group = node->page_groups[pos].group;
		return MEMFS_OK;
	}

	group = memfs_allocator_alloc_page_group(&node->fs->allocator);
	if (group == NULL)
		return MEMFS_ERR_NO_MEMORY;

	result = memfs_storage_group_reserve(node, node->page_group_count + 1U);
	if (result != MEMFS_OK) {
		memfs_allocator_free_page_group(&node->fs->allocator, group);
		return result;
	}

	memmove(node->page_groups + pos + 1U, node->page_groups + pos,
			(size_t)(node->page_group_count - pos) * sizeof(*node->page_groups));

	node->page_groups[pos].index = group_index;
	node->page_groups[pos].group = group;
	node->page_group_count++;

	*out_group = group;
	return MEMFS_OK;
}

static void memfs_storage_remove_group(MemfsNode* node, uint64_t group_index) {
	bool found;
	uint32_t pos = memfs_storage_group_position(node, group_index, &found);
	MemfsPageGroup* group;

	if (!found)
		return;

	group = node->page_groups[pos].group;
	free(group->pages);
	memfs_allocator_free_page_group(&node->fs->allocator, group);

	memmove(node->page_groups + pos, node->page_groups + pos + 1U,
			(size_t)(node->page_group_count - pos - 1U) * sizeof(*node->page_groups));

	node->page_group_count--;
	memfs_storage_group_try_shrink(node);
}

static MemfsResult memfs_storage_replace_page(MemfsNode* node, MemfsPageGroup* group, uint32_t slot,
											  MemfsPage* new_page) {
	bool had_old = memfs_group_has(group, slot);
	uint32_t rank = memfs_group_rank(group, slot);
	MemfsPage* old_page = had_old ? group->pages[rank] : NULL;
	size_t old_size = memfs_page_heap_size(old_page);
	size_t new_size = memfs_page_heap_size(new_page);
	uint32_t word = slot >> 6;
	uint32_t bit = slot & 63U;
	MemfsResult result;

	if (!had_old && new_page == NULL)
		return MEMFS_OK;

	if (!had_old && new_page) {
		result = memfs_group_reserve(group, (uint32_t)group->page_count + 1U);
		if (result != MEMFS_OK)
			return result;

		memmove(group->pages + rank + 1U, group->pages + rank,
				(size_t)(group->page_count - rank) * sizeof(*group->pages));

		group->pages[rank] = new_page;
		group->bitmap[word] |= 1ULL << bit;
		group->page_count++;
	} else if (had_old && new_page) {
		group->pages[rank] = new_page;
	} else {
		memmove(group->pages + rank, group->pages + rank + 1U,
				(size_t)(group->page_count - rank - 1U) * sizeof(*group->pages));

		group->bitmap[word] &= ~(1ULL << bit);
		group->page_count--;
		memfs_group_try_shrink(group);
	}

	if (old_size)
		memfs_resident_sub(node, old_size);
	if (new_size)
		memfs_resident_add(node, new_size);

	free(old_page);
	return MEMFS_OK;
}
static bool memfs_small_inline_eligible(MemfsNode* node, uint32_t capacity) {
	return capacity != 0 && capacity <= sizeof(node->small_inline_data) && !node->fs->encryption_enabled;
}

static MemfsResult memfs_storage_decode_small(MemfsNode* node, uint8_t* plain, uint32_t plain_capacity) {
	if (plain_capacity < node->small_capacity)
		return MEMFS_ERR_INVALID;

	if (node->small_capacity == 0)
		return MEMFS_OK;

	if (node->small_inline) {
		memcpy(plain, node->small_inline_data, node->small_capacity);
		return MEMFS_OK;
	}

	return memfs_page_decode(node, UINT64_MAX, node->small_page, plain, (uint16_t)node->small_capacity);
}

static void memfs_storage_replace_small(MemfsNode* node, MemfsPage* new_page, uint32_t new_capacity) {
	MemfsPage* old_page = node->small_inline ? NULL : node->small_page;
	size_t old_size = memfs_page_heap_size(old_page);
	size_t new_size = memfs_page_heap_size(new_page);

	node->small_page = new_page;
	node->small_capacity = new_capacity;
	node->small_inline = false;

	if (old_size)
		memfs_resident_sub(node, old_size);
	if (new_size)
		memfs_resident_add(node, new_size);

	free(old_page);
}

static void memfs_storage_replace_small_inline(MemfsNode* node, const uint8_t* plain, uint32_t capacity) {
	MemfsPage* old_page = node->small_inline ? NULL : node->small_page;
	size_t old_size = memfs_page_heap_size(old_page);

	if (old_size)
		memfs_resident_sub(node, old_size);
	free(old_page);

	memset(node->small_inline_data, 0, sizeof(node->small_inline_data));

	if (plain && capacity) {
		memcpy(node->small_inline_data, plain, capacity);
	}

	node->small_capacity = capacity;
	node->small_inline = capacity != 0;
}

static void memfs_storage_destroy_pages(MemfsNode* node) {
	uint32_t entry_index;

	for (entry_index = 0; entry_index < node->page_group_count; entry_index++) {
		MemfsPageGroup* group = node->page_groups[entry_index].group;
		uint32_t i;

		for (i = 0; i < group->page_count; i++) {
			MemfsPage* page = group->pages[i];

			memfs_resident_sub(node, memfs_page_heap_size(page));
			free(page);
		}

		free(group->pages);
		memfs_allocator_free_page_group(&node->fs->allocator, group);
	}

	free(node->page_groups);
	node->page_groups = NULL;
	node->page_group_count = 0;
	node->page_group_capacity = 0;
}

static void memfs_storage_destroy(MemfsNode* node) {
	if (node->page_group_count != 0) {
		memfs_storage_destroy_pages(node);
		return;
	}

	if (node->small_capacity) {
		if (!node->small_inline && node->small_page) {
			memfs_resident_sub(node, memfs_page_heap_size(node->small_page));
			free(node->small_page);
		}

		memset(node->small_inline_data, 0, sizeof(node->small_inline_data));
		node->small_capacity = 0;
		node->small_inline = false;
	}
}

static uint32_t memfs_small_capacity(uint64_t required_end) {
	uint64_t aligned;

	if (required_end == 0)
		return 0;
	if (required_end > MEMFS_SMALL_LIMIT)
		return 0;

	// 1B 文件不再为了增长策略强制占 8B payload。
	// 1/2/4/8 采用 tiny class；之后保持原有 8B granule，
	// 避免大一点的小文件每增长 1B 都 realloc。
	if (required_end <= 1U)
		return 1U;
	if (required_end <= 2U)
		return 2U;
	if (required_end <= 4U)
		return 4U;
	if (required_end <= 8U)
		return 8U;

	aligned = (required_end + MEMFS_SMALL_GRANULE - 1U) & ~((uint64_t)MEMFS_SMALL_GRANULE - 1U);

	if (aligned > MEMFS_SMALL_LIMIT)
		return 0;

	return (uint32_t)aligned;
}

static MemfsResult memfs_storage_promote(MemfsNode* node) {
	uint8_t plain[MEMFS_PAGE_SIZE];
	uint8_t old_inline_data[sizeof(node->small_inline_data)];
	MemfsPage* new_page = NULL;
	MemfsPage* old_page;
	MemfsPageGroup* group;
	MemfsResult result;
	uint32_t old_capacity;
	bool old_inline;
	size_t old_page_size;

	if (node->small_capacity == 0)
		return MEMFS_OK;

	memset(plain, 0, sizeof(plain));

	result = memfs_storage_decode_small(node, plain, sizeof(plain));
	if (result != MEMFS_OK)
		return result;

	result = memfs_page_encode(node, 0, plain, MEMFS_PAGE_SIZE, &new_page);
	if (result != MEMFS_OK)
		return result;

	old_capacity = node->small_capacity;
	old_inline = node->small_inline;
	old_page = old_inline ? NULL : node->small_page;
	old_page_size = memfs_page_heap_size(old_page);

	if (old_inline)
		memcpy(old_inline_data, node->small_inline_data, sizeof(old_inline_data));

	// small storage 与 page_groups 共用 union。切换到 paged mode 前先把
	// 旧状态保存在局部变量中；如果 metadata 分配失败可以完整恢复。
	memset(node->small_inline_data, 0, sizeof(node->small_inline_data));
	node->small_capacity = 0;
	node->small_inline = false;
	node->page_groups = NULL;
	node->page_group_count = 0;
	node->page_group_capacity = 0;

	if (new_page) {
		result = memfs_storage_ensure_group(node, 0, &group);
		if (result != MEMFS_OK)
			goto rollback;

		result = memfs_storage_replace_page(node, group, 0, new_page);
		if (result != MEMFS_OK) {
			memfs_storage_remove_group(node, 0);
			goto rollback;
		}

		new_page = NULL; // ownership moved into group
	}

	if (old_page_size)
		memfs_resident_sub(node, old_page_size);
	free(old_page);
	return MEMFS_OK;

rollback:
	if (node->page_group_count)
		memfs_storage_destroy_pages(node);
	else {
		free(node->page_groups);
		node->page_groups = NULL;
		node->page_group_capacity = 0;
	}

	if (old_inline)
		memcpy(node->small_inline_data, old_inline_data, sizeof(old_inline_data));
	else
		node->small_page = old_page;

	node->small_capacity = (uint16_t)old_capacity;
	node->small_inline = old_inline;
	free(new_page);
	return result;
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

	result = memfs_storage_decode_small(node, plain, sizeof(plain));
	if (result != MEMFS_OK)
		return result;

	if (new_size < target) {
		memset(plain + new_size, 0, (size_t)(target - new_size));
	}

	if (memfs_small_inline_eligible(node, target)) {
		memfs_storage_replace_small_inline(node, plain, target);
		return MEMFS_OK;
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
	uint32_t entry_index = 0;

	if ((new_size & MEMFS_PAGE_MASK) != 0) {
		uint64_t group_index = tail_page >> MEMFS_PAGE_GROUP_SHIFT;
		MemfsPageGroup* group = memfs_storage_group(node, group_index);

		if (group) {
			uint32_t slot = (uint32_t)(tail_page & MEMFS_PAGE_GROUP_MASK);
			MemfsPage* old_page = memfs_group_get(group, slot);

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

				result = memfs_storage_replace_page(node, group, slot, new_page);
				if (result != MEMFS_OK) {
					free(new_page);
					return result;
				}
			}
		}
	}

	while (entry_index < node->page_group_count) {
		uint64_t group_index = node->page_groups[entry_index].index;
		MemfsPageGroup* group = node->page_groups[entry_index].group;
		uint32_t slot;

		for (slot = 0; slot < MEMFS_PAGES_PER_GROUP; slot++) {
			uint64_t page_index = (group_index << MEMFS_PAGE_GROUP_SHIFT) | slot;
			MemfsPage* page;
			MemfsResult result;

			if (page_index < first_free_page)
				continue;

			page = memfs_group_get(group, slot);
			if (page == NULL)
				continue;

			result = memfs_storage_replace_page(node, group, slot, NULL);
			if (result != MEMFS_OK)
				return result;
		}

		if (group->page_count == 0) {
			memfs_storage_remove_group(node, group_index);
			continue;
		}

		entry_index++;
	}

	return MEMFS_OK;
}

static void memfs_storage_try_demote(MemfsNode* node, uint64_t new_size) {
	uint32_t target;
	uint8_t plain[MEMFS_SMALL_LIMIT];
	MemfsPage* new_page = NULL;
	MemfsPage* first_page;
	MemfsResult result;

	if (node->page_group_count == 0 || new_size > MEMFS_SMALL_LIMIT) {
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

	if (target && !memfs_small_inline_eligible(node, target)) {
		result = memfs_page_encode(node, UINT64_MAX, plain, (uint16_t)target, &new_page);
		if (result != MEMFS_OK)
			return;
	}

	memfs_storage_destroy_pages(node);

	if (memfs_small_inline_eligible(node, target)) {
		memfs_storage_replace_small_inline(node, plain, target);
	} else {
		memfs_storage_replace_small(node, new_page, target);
	}
}

static MemfsResult memfs_storage_trim_data(MemfsNode* node, uint64_t new_size) {
	MemfsResult result;

	if (node->page_group_count != 0) {
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

	if (node->page_group_count == 0) {
		uint64_t available = 0;
		uint8_t plain[MEMFS_SMALL_LIMIT];
		MemfsResult result;

		memset(buffer, 0, (size_t)length);

		if (node->small_capacity == 0 || offset >= node->small_capacity) {
			return MEMFS_OK;
		}

		memset(plain, 0, sizeof(plain));
		result = memfs_storage_decode_small(node, plain, sizeof(plain));
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
		result = memfs_storage_decode_small(node, plain, sizeof(plain));
		if (result != MEMFS_OK)
			return result;
	}

	memcpy(plain + offset, buffer, (size_t)length);

	if (memfs_small_inline_eligible(node, target)) {
		memfs_storage_replace_small_inline(node, plain, target);
		return MEMFS_OK;
	}

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

	// 第一遍只准备缺失页，不写用户数据。
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

			result = memfs_storage_replace_page(node, group, slot, page);
			if (result != MEMFS_OK) {
				free(page);
				return result;
			}
		} else if (page->flags != 0 || page->plain_size != MEMFS_PAGE_SIZE || page->stored_size != MEMFS_PAGE_SIZE) {
			return MEMFS_ERR_DATA;
		}
	}

	// 所有目标页存在后再写用户数据。
	while (done < length) {
		uint64_t pos = offset + done;
		uint32_t in_page = (uint32_t)(pos & MEMFS_PAGE_MASK);
		uint32_t span = MEMFS_PAGE_SIZE - in_page;
		MemfsPage* page;

		page_index = pos >> MEMFS_PAGE_SHIFT;
		page = memfs_storage_page(node, page_index);

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

	if (page_count > SIZE_MAX / sizeof(*replacements)) {
		return MEMFS_ERR_NO_MEMORY;
	}

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
		const uint8_t* source = buffer + (write_start - offset);

		// 整页覆盖不需要先 decrypt/decompress 旧页。
		if (in_page == 0 && span == MEMFS_PAGE_SIZE) {
			result = memfs_page_encode(node, page_index, source, MEMFS_PAGE_SIZE, &replacements[i]);
		} else {
			uint8_t plain[MEMFS_PAGE_SIZE];
			MemfsPage* old_page = memfs_storage_page(node, page_index);

			result = memfs_page_decode(node, page_index, old_page, plain, MEMFS_PAGE_SIZE);
			if (result == MEMFS_OK) {
				memcpy(plain + in_page, source, span);

				result = memfs_page_encode(node, page_index, plain, MEMFS_PAGE_SIZE, &replacements[i]);
			}
		}

		if (result != MEMFS_OK)
			goto exit;
	}

	// 预创建 group 并为本次新增 page pointer 一次性 reserve，
	// commit 阶段因此不会再因为 pointer-vector realloc 失败而半提交。
	{
		uint64_t first_group = first_page >> MEMFS_PAGE_GROUP_SHIFT;
		uint64_t last_group = last_page >> MEMFS_PAGE_GROUP_SHIFT;
		uint64_t group_index;

		for (group_index = first_group; group_index <= last_group; group_index++) {
			uint64_t group_first = group_index << MEMFS_PAGE_GROUP_SHIFT;
			uint64_t group_last = group_first + MEMFS_PAGES_PER_GROUP - 1U;
			uint64_t begin = first_page > group_first ? first_page : group_first;
			uint64_t finish = last_page < group_last ? last_page : group_last;
			MemfsPageGroup* group = memfs_storage_group(node, group_index);
			uint32_t additions = 0;
			uint64_t page_index;

			for (page_index = begin; page_index <= finish; page_index++) {
				uint64_t replacement_index = page_index - first_page;
				uint32_t slot = (uint32_t)(page_index & MEMFS_PAGE_GROUP_MASK);

				if (replacements[replacement_index] && (group == NULL || !memfs_group_has(group, slot))) {
					additions++;
				}
			}

			if (additions == 0)
				continue;

			if (group == NULL) {
				result = memfs_storage_ensure_group(node, group_index, &group);
				if (result != MEMFS_OK)
					goto exit;
			}

			result = memfs_group_reserve(group, (uint32_t)group->page_count + additions);
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
			result = memfs_storage_replace_page(node, group, slot, replacements[i]);
			if (result != MEMFS_OK)
				goto exit;

			replacements[i] = NULL;

			if (group->page_count == 0)
				memfs_storage_remove_group(node, group_index);
		}
	}

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

	if (end <= MEMFS_SMALL_LIMIT && node->page_group_count == 0) {
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

	if (MEMFS_NODE_IS_DIRECTORY(node)) {
		memfs_dir_destroy(node->dir);
		memfs_object_free_dir(fs, node->dir);
	} else {
		memfs_storage_destroy(node);
	}

	memfs_atomic_sub_clamped(&fs->used_bytes, node->file_size);
	{
		MemfsNodeMetaValue* meta = memfs_meta_table_remove(fs->meta_table, node->index_number);
		if (meta) {
			memfs_security_release(meta->security);
			free(meta);
		}
	}

	memfs_object_free_name(fs, node->name);
	memfs_object_free_node(fs, node);
}
static void memfs_destroy_namespace_node(MemfsNode* node);

static void memfs_destroy_child_tree(MemfsNode* node) {
	MemfsNode* left;
	MemfsNode* right;

	if (node == NULL)
		return;

	left = node->tree_left;
	right = node->tree_right;

	memfs_destroy_child_tree(left);
	memfs_destroy_child_tree(right);

	node->tree_left = NULL;
	node->tree_right = NULL;
	node->tree_parent = NULL;
	node->parent = NULL;

	memfs_destroy_namespace_node(node);
}

static void memfs_destroy_namespace_node(MemfsNode* node) {
	if (node == NULL)
		return;

	if (MEMFS_NODE_IS_DIRECTORY(node)) {
		memfs_destroy_child_tree(node->dir->root);
		node->dir->root = NULL;
		node->dir->child_count = 0;
	}

	memfs_node_free(node);
}

static MemfsResult memfs_security_create(PSECURITY_DESCRIPTOR security, MemfsSecurity** out_security) {
	uint32_t size;
	MemfsSecurity* shared;

	if (security == NULL || out_security == NULL)
		return MEMFS_ERR_INVALID;

	size = GetSecurityDescriptorLength(security);
	if (size == 0)
		return MEMFS_ERR_INVALID;

	shared = malloc(sizeof(*shared) + size);
	if (shared == NULL)
		return MEMFS_ERR_NO_MEMORY;

	shared->ref_count = 1;
	shared->size = size;
	memcpy(shared->data, security, size);

	*out_security = shared;
	return MEMFS_OK;
}

static void memfs_security_retain(MemfsSecurity* security) {
	if (security)
		InterlockedIncrement(&security->ref_count);
}

static void memfs_security_release(MemfsSecurity* security) {
	if (security == NULL)
		return;

	if (InterlockedDecrement(&security->ref_count) == 0) {
		SecureZeroMemory(security->data, security->size);
		free(security);
	}
}

static bool memfs_security_equal(const MemfsSecurity* shared, PSECURITY_DESCRIPTOR security) {
	uint32_t size;

	if (shared == NULL || security == NULL)
		return false;

	size = GetSecurityDescriptorLength(security);
	return size == shared->size && 0 == memcmp(shared->data, security, size);
}

static MemfsResult memfs_default_security(MemfsSecurity** out_security) {
	PSECURITY_DESCRIPTOR descriptor = NULL;
	ULONG size = 0;
	MemfsResult result;

	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(MEMFS_DEFAULT_SDDL, SDDL_REVISION_1, &descriptor,
															  &size)) {
		return MEMFS_ERR_ACCESS;
	}

	result = memfs_security_create(descriptor, out_security);
	LocalFree(descriptor);
	return result;
}

static uint32_t memfs_name_hash(const wchar_t* name) {
	uint32_t hash = 2166136261U;

	while (*name) {
		uint32_t c = (uint32_t)towlower(*name++);

		hash ^= c;
		hash *= 16777619U;
	}

	return hash;
}

static uint32_t memfs_dir_hash_probe_distance(const MemfsDirHash* hash, const MemfsNode* node, uint32_t slot) {
	uint32_t mask = hash->capacity - 1U;
	uint32_t ideal = node->name_hash & mask;

	return (slot - ideal) & mask;
}

static MemfsNode* memfs_dir_hash_lookup(MemfsDirHash* hash, const wchar_t* name) {
	uint32_t name_hash = memfs_name_hash(name);
	uint32_t mask = hash->capacity - 1U;
	uint32_t slot = name_hash & mask;
	uint32_t distance = 0;

	for (;;) {
		MemfsNode* node = hash->slots[slot];
		uint32_t node_distance;

		if (node == NULL)
			return NULL;

		node_distance = memfs_dir_hash_probe_distance(hash, node, slot);
		if (node_distance < distance)
			return NULL;

		if (node->name_hash == name_hash && _wcsicmp(node->name, name) == 0) {
			return node;
		}

		slot = (slot + 1U) & mask;
		distance++;

		if (distance >= hash->capacity)
			return NULL;
	}
}

static bool memfs_dir_hash_insert_node(MemfsDirHash* hash, MemfsNode* node) {
	uint32_t mask = hash->capacity - 1U;
	uint32_t slot = node->name_hash & mask;
	uint32_t distance = 0;
	MemfsNode* candidate = node;

	for (;;) {
		MemfsNode* current = hash->slots[slot];

		if (current == NULL) {
			hash->slots[slot] = candidate;
			hash->count++;
			return true;
		}

		if (current == candidate)
			return true;

		{
			uint32_t current_distance = memfs_dir_hash_probe_distance(hash, current, slot);

			if (current_distance < distance) {
				hash->slots[slot] = candidate;
				candidate = current;
				distance = current_distance;
			}
		}

		slot = (slot + 1U) & mask;
		distance++;

		if (distance >= hash->capacity)
			return false;
	}
}

static bool memfs_dir_hash_remove_node(MemfsDirHash* hash, MemfsNode* node) {
	uint32_t mask = hash->capacity - 1U;
	uint32_t slot = node->name_hash & mask;
	uint32_t distance = 0;

	for (;;) {
		MemfsNode* current = hash->slots[slot];

		if (current == NULL)
			return false;

		if (current == node)
			break;

		if (memfs_dir_hash_probe_distance(hash, current, slot) < distance)
			return false;

		slot = (slot + 1U) & mask;
		distance++;
		if (distance >= hash->capacity)
			return false;
	}

	// Backward-shift deletion keeps the Robin Hood invariant and eliminates
	// tombstones, which is important for long-lived create/delete workloads.
	for (;;) {
		uint32_t next = (slot + 1U) & mask;
		MemfsNode* current = hash->slots[next];

		if (current == NULL || memfs_dir_hash_probe_distance(hash, current, next) == 0) {
			hash->slots[slot] = NULL;
			break;
		}

		hash->slots[slot] = current;
		slot = next;
	}

	hash->count--;
	return true;
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

static uint32_t memfs_dir_hash_capacity_for_count(uint32_t count) {
	uint32_t capacity = 128U;

	while ((uint64_t)count * 100ULL > (uint64_t)capacity * MEMFS_DIR_HASH_LOAD_PERCENT) {
		if (capacity > UINT32_MAX / 2U)
			return capacity;
		capacity <<= 1;
	}

	return capacity;
}

static void memfs_dir_hash_after_insert(MemfsDir* dir, MemfsNode* node) {
	MemfsDirHash* hash = dir->hash;

	if (hash == NULL) {
		if (dir->child_count >= MEMFS_DIR_HASH_THRESHOLD) {
			uint32_t capacity = memfs_dir_hash_capacity_for_count(dir->child_count);

			(void)memfs_dir_hash_rebuild(dir, capacity);
		}
		return;
	}

	if ((uint64_t)(hash->count + 1U) * 100ULL > (uint64_t)hash->capacity * MEMFS_DIR_HASH_LOAD_PERCENT) {
		if (hash->capacity <= UINT32_MAX / 2U && memfs_dir_hash_rebuild(dir, hash->capacity << 1)) {
			return;
		}
	}

	hash = dir->hash;
	if (!memfs_dir_hash_insert_node(hash, node))
		memfs_dir_hash_disable(dir);
}

static void memfs_dir_hash_before_remove(MemfsDir* dir, MemfsNode* node) {
	if (dir->hash)
		(void)memfs_dir_hash_remove_node(dir->hash, node);
}

static void memfs_dir_hash_after_remove(MemfsDir* dir) {
	MemfsDirHash* hash = dir->hash;

	if (hash == NULL)
		return;

	if (dir->child_count < MEMFS_DIR_HASH_THRESHOLD / 2U) {
		memfs_dir_hash_disable(dir);
		return;
	}

	{
		uint32_t target = memfs_dir_hash_capacity_for_count(dir->child_count);

		if (target < hash->capacity)
			(void)memfs_dir_hash_rebuild(dir, target);
	}
}

static uint32_t memfs_treap_priority(const MemfsNode* node) {
	uint64_t x = node->index_number ^ node->fs->treap_seed;

	x += 0x9e3779b97f4a7c15ULL;
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

	while (node->tree_parent && memfs_treap_priority(node->tree_parent) > memfs_treap_priority(node)) {
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

	if (!MEMFS_NODE_IS_DIRECTORY(parent))
		return;

	dir = parent->dir;
	memfs_dir_hash_before_remove(dir, node);

	while (node->tree_left || node->tree_right) {
		if (node->tree_left == NULL) {
			memfs_tree_rotate_left(dir, node);
		} else if (node->tree_right == NULL) {
			memfs_tree_rotate_right(dir, node);
		} else if (memfs_treap_priority(node->tree_left) < memfs_treap_priority(node->tree_right)) {
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

	if (dir == NULL)
		return;

	memfs_node_set_change_time(dir, memfs_now());
}

static MemfsResult memfs_node_alloc(Memfs* fs, MemfsNode* parent, const wchar_t* name, bool directory,
									uint32_t attributes, PSECURITY_DESCRIPTOR security, MemfsNode** out_node) {
	const wchar_t* source_name = name ? name : L"";
	MemfsNode* node;
	MemfsResult result;

	if (fs == NULL || out_node == NULL)
		return MEMFS_ERR_INVALID;

	*out_node = NULL;

	node = memfs_object_alloc_node(fs);
	if (node == NULL)
		return MEMFS_ERR_NO_MEMORY;

	node->fs = fs;

	if (directory) {
		node->dir = memfs_object_alloc_dir(fs);
		if (node->dir == NULL) {
			memfs_object_free_node(fs, node);
			return MEMFS_ERR_NO_MEMORY;
		}
		attributes |= FILE_ATTRIBUTE_DIRECTORY;
	} else {
		attributes &= ~FILE_ATTRIBUTE_DIRECTORY;
		if (attributes == 0)
			attributes = FILE_ATTRIBUTE_NORMAL;
	}

	node->name = memfs_object_dup_name(fs, source_name);
	if (node->name == NULL) {
		if (directory)
			memfs_object_free_dir(fs, node->dir);
		memfs_object_free_node(fs, node);
		return MEMFS_ERR_NO_MEMORY;
	}

	node->name_hash = memfs_name_hash(node->name);
	node->index_number = fs->next_index++;

	if (security) {
		if (parent && memfs_security_equal(memfs_node_get_security(parent), security)) {
			memfs_security_retain(memfs_node_get_security(parent));
			memfs_node_set_security_meta(node, memfs_node_get_security(parent));
			result = MEMFS_OK;
		} else {
			{
			MemfsSecurity* created = NULL;
			result = memfs_security_create(security, &created);
			if (result == MEMFS_OK) {
				memfs_node_set_security_meta(node, created);
				}
		}
		}
	} else if (parent && memfs_node_get_security(parent)) {
		memfs_security_retain(memfs_node_get_security(parent));
		memfs_node_set_security_meta(node, memfs_node_get_security(parent));
		result = MEMFS_OK;
	} else {
		{
			MemfsSecurity* created = NULL;
			result = memfs_default_security(&created);
			if (result == MEMFS_OK) {
				memfs_node_set_security_meta(node, created);
				}
		}
	}

	if (result != MEMFS_OK) {
		memfs_object_free_name(fs, node->name);
		if (directory)
			memfs_object_free_dir(fs, node->dir);
		memfs_object_free_node(fs, node);
		return result;
	}

	node->attributes = attributes;
memfs_node_set_creation_time(node, memfs_now());
	memfs_node_set_last_access_time(node, memfs_node_get_creation_time(node));
	memfs_node_set_last_write_time(node, memfs_node_get_creation_time(node));
	memfs_node_set_change_time(node, memfs_node_get_creation_time(node));

	*out_node = node;
	return MEMFS_OK;
}

static MemfsResult memfs_resize_allocation(MemfsNode* node, uint64_t allocation_size) {
	uint64_t old_size = node->allocation_size;
	MemfsResult result;

	if (allocation_size == old_size)
		return MEMFS_OK;

	if (allocation_size < old_size) {
		result = memfs_storage_trim_data(node, allocation_size);
		if (result != MEMFS_OK)
			return result;
	}

	node->allocation_size = allocation_size;
	return MEMFS_OK;
}

static MemfsResult memfs_account_file_size(MemfsNode* node, uint64_t old_size, uint64_t new_size) {
	Memfs* fs = node->fs;

	if (new_size == old_size)
		return MEMFS_OK;

	if (new_size > old_size) {
		uint64_t delta = new_size - old_size;

		if (!memfs_atomic_reserve(&fs->used_bytes, fs->capacity, delta)) {
			return MEMFS_ERR_NO_SPACE;
		}
	} else {
		memfs_atomic_sub_clamped(&fs->used_bytes, old_size - new_size);
	}

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
	uint64_t used;

	if (fs == NULL)
		return 0;

	used = memfs_atomic_load_u64(&fs->used_bytes);
	return used <= fs->capacity ? fs->capacity - used : 0;
}

uint64_t memfs_resident_bytes(Memfs* fs) {
	if (fs == NULL)
		return 0;

	return memfs_atomic_load_u64(&fs->resident_bytes);
}

uint64_t memfs_node_resident_bytes(const MemfsNode* node) {
	uint64_t resident = 0;
	uint32_t entry_index;

	if (node == NULL)
		return 0;

	if (node->small_capacity && !node->small_inline)
		resident += memfs_page_heap_size(node->small_page);

	for (entry_index = 0; entry_index < node->page_group_count; entry_index++) {
		const MemfsPageGroup* group = node->page_groups[entry_index].group;
		uint32_t i;

		if (group == NULL)
			continue;

		for (i = 0; i < group->page_count; i++)
			resident += memfs_page_heap_size(group->pages[i]);
	}

	return resident;
}

MemfsResult memfs_create_ex(const MemfsOptions* options, Memfs** out_fs) {
	Memfs* fs;
	MemfsNode* root;
	MemfsResult result;
	const wchar_t* volume_label;
	size_t label_chars;

	if (options == NULL || out_fs == NULL || options->capacity == 0 || options->capacity > INT64_MAX) {
		return MEMFS_ERR_INVALID;
	}

	if (options->encryption_key && options->encryption_key_size != MEMFS_ENCRYPTION_KEY_SIZE) {
		return MEMFS_ERR_INVALID;
	}

	if (sodium_init() < 0)
		return MEMFS_ERR_ACCESS;

	fs = calloc(1, sizeof(*fs));
	if (fs == NULL)
		return MEMFS_ERR_NO_MEMORY;

	if (!memfs_allocator_init(&fs->allocator, sizeof(MemfsNode), sizeof(MemfsDir), sizeof(MemfsPageGroup))) {
		free(fs);
		return MEMFS_ERR_NO_MEMORY;
	}

	fs->meta_table = memfs_meta_table_create(1024);
	if (fs->meta_table == NULL) {
		memfs_allocator_destroy(&fs->allocator);
		free(fs);
		return MEMFS_ERR_NO_MEMORY;
	}

	fs->capacity = options->capacity;
	fs->next_index = 1;
	randombytes_buf(&fs->treap_seed, sizeof(fs->treap_seed));
	if (fs->treap_seed == 0)
		fs->treap_seed = 0x9e3779b97f4a7c15ULL;
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

		// XChaCha nonce = random 128-bit process prefix + monotonic 64-bit sequence.
		// Prefix makes nonce space fresh across mounts even when a fixed key is reused.
		randombytes_buf(fs->encryption_nonce_prefix, MEMFS_ENCRYPTION_NONCE_PREFIX_SIZE);
		fs->encryption_nonce_counter = 0;

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
		memfs_allocator_destroy(&fs->allocator);
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

	node = fs->root;
	fs->root = NULL;
	memfs_destroy_namespace_node(node);

	while ((node = fs->orphan_head) != NULL) {
		memfs_orphan_remove(fs, node);
		memfs_node_free(node);
	}

	if (fs->encryption_enabled) {
		sodium_memzero(fs->encryption_key, MEMFS_ENCRYPTION_KEY_SIZE);
		sodium_memzero(fs->encryption_nonce_prefix, MEMFS_ENCRYPTION_NONCE_PREFIX_SIZE);
		(void)sodium_munlock(fs->encryption_key, MEMFS_ENCRYPTION_KEY_SIZE);
	}

	if (fs->meta_table != NULL)
		memfs_meta_table_destroy(fs->meta_table, NULL);
	memfs_allocator_destroy(&fs->allocator);
	free(fs);
}

MemfsNode* memfs_dir_lookup(MemfsNode* dir_node, const wchar_t* name) {
	MemfsNode* node;

	if (!MEMFS_NODE_IS_DIRECTORY(dir_node) || name == NULL)
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

	if (!MEMFS_NODE_IS_DIRECTORY(dir_node))
		return NULL;

	node = dir_node->dir->root;
	if (node == NULL)
		return NULL;

	while (node->tree_left)
		node = node->tree_left;

	return node;
}

MemfsNode* memfs_dir_upper_bound(MemfsNode* dir_node, const wchar_t* marker) {
	MemfsNode* node;
	MemfsNode* candidate = NULL;

	if (!MEMFS_NODE_IS_DIRECTORY(dir_node) || marker == NULL) {
		return NULL;
	}

	node = dir_node->dir->root;
	while (node) {
		int cmp = _wcsicmp(node->name, marker);

		if (cmp > 0) {
			candidate = node;
			node = node->tree_left;
		} else {
			node = node->tree_right;
		}
	}

	return candidate;
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

		if (!MEMFS_NODE_IS_DIRECTORY(node))
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

	if (!MEMFS_NODE_IS_DIRECTORY(parent))
		return MEMFS_ERR_NOT_DIRECTORY;

	*out_parent = parent;
	return MEMFS_OK;
}

MemfsResult memfs_node_create(Memfs* fs, MemfsNode* parent, const wchar_t* name, bool directory, uint32_t attributes,
							  PSECURITY_DESCRIPTOR security, uint64_t allocation_size, MemfsNode** out_node) {
	MemfsNode* node;
	MemfsResult result;

	if (fs == NULL || !MEMFS_NODE_IS_DIRECTORY(parent) || name == NULL || *name == L'\0' || out_node == NULL) {
		return MEMFS_ERR_INVALID;
	}

	if (memfs_dir_lookup(parent, name))
		return MEMFS_ERR_EXISTS;

	result = memfs_node_alloc(fs, parent, name, directory, attributes, security, &node);
	if (result != MEMFS_OK)
		return result;

	if (!directory && allocation_size) {
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

	if (node->deleted && node->open_count == 0) {
		memfs_orphan_remove(node->fs, node);
		memfs_node_free(node);
	}
}

bool memfs_node_is_directory(const MemfsNode* node) {
	return MEMFS_NODE_IS_DIRECTORY(node);
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
	if (MEMFS_NODE_IS_DIRECTORY(node) && node->dir->child_count)
		return MEMFS_ERR_NOT_EMPTY;

	parent = node->parent;
	memfs_dir_remove(node);
	node->deleted = true;
	memfs_node_set_change_time(node, memfs_now());
	memfs_touch_directory(parent);

	if (node->open_count == 0)
		memfs_node_free(node);
	else
		memfs_orphan_insert(node->fs, node);

	return MEMFS_OK;
}

MemfsResult memfs_node_rename(MemfsNode* node, MemfsNode* new_parent, const wchar_t* new_name, bool replace_if_exists) {
	MemfsNode* existing;
	MemfsNode* old_parent;
	wchar_t* new_name_copy;

	if (node == NULL || !MEMFS_NODE_IS_DIRECTORY(new_parent) || new_name == NULL || *new_name == L'\0') {
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
		if (MEMFS_NODE_IS_DIRECTORY(existing) && existing->dir->child_count)
			return MEMFS_ERR_NOT_EMPTY;
	}

	new_name_copy = memfs_object_dup_name(node->fs, new_name);
	if (new_name_copy == NULL)
		return MEMFS_ERR_NO_MEMORY;

	if (existing && existing != node) {
		MemfsNode* existing_parent = existing->parent;

		memfs_dir_remove(existing);
		existing->deleted = true;
		memfs_touch_directory(existing_parent);

		if (existing->open_count == 0)
			memfs_node_free(existing);
		else
			memfs_orphan_insert(existing->fs, existing);
	}

	old_parent = node->parent;
	memfs_dir_remove(node);

	memfs_object_free_name(node->fs, node->name);

	node->name = new_name_copy;
	node->name_hash = memfs_name_hash(node->name);
	memfs_dir_insert(new_parent, node);

	memfs_node_set_change_time(node, memfs_now());
	memfs_touch_directory(old_parent);
	if (new_parent != old_parent)
		memfs_touch_directory(new_parent);

	return MEMFS_OK;
}

MemfsResult memfs_node_set_allocation_size(MemfsNode* node, uint64_t new_size) {
	MemfsResult result;

	if (node == NULL)
		return MEMFS_ERR_INVALID;
	if (MEMFS_NODE_IS_DIRECTORY(node))
		return MEMFS_ERR_IS_DIRECTORY;

	// Windows requires FileSize <= AllocationSize, so truncate EOF first.
	// Internal allocation_size stays byte-granular; WinFsp reports it aligned to 512B.
	if (new_size < node->file_size) {
		result = memfs_node_set_file_size(node, new_size);
		if (result != MEMFS_OK)
			return result;
	}

	result = memfs_resize_allocation(node, new_size);
	if (result == MEMFS_OK)
		memfs_node_set_change_time(node, memfs_now());

	return result;
}

MemfsResult memfs_node_set_file_size(MemfsNode* node, uint64_t new_size) {
	uint64_t old_file_size;
	MemfsResult result;

	if (node == NULL)
		return MEMFS_ERR_INVALID;
	if (MEMFS_NODE_IS_DIRECTORY(node))
		return MEMFS_ERR_IS_DIRECTORY;

	old_file_size = node->file_size;
	if (new_size == old_file_size)
		return MEMFS_OK;

	if (new_size < old_file_size) {
		result = memfs_storage_trim_data(node, new_size);
		if (result != MEMFS_OK)
			return result;
	}

	result = memfs_account_file_size(node, old_file_size, new_size);
	if (result != MEMFS_OK)
		return result;

	if (new_size > node->allocation_size)
		node->allocation_size = new_size;

	node->file_size = new_size;
	memfs_node_set_change_time(node, memfs_now());
	return MEMFS_OK;
}

MemfsResult memfs_node_read(MemfsNode* node, void* buffer, uint64_t offset, uint32_t length, uint32_t* bytes_read) {
	uint64_t end;

	if (node == NULL || buffer == NULL || bytes_read == NULL)
		return MEMFS_ERR_INVALID;
	if (MEMFS_NODE_IS_DIRECTORY(node))
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
	memfs_node_set_last_access_time(node, memfs_now());
	return MEMFS_OK;
}

MemfsResult memfs_node_write(MemfsNode* node, const void* buffer, uint64_t offset, uint32_t length, bool write_to_end,
							 bool constrained_io, uint32_t* bytes_written) {
	uint64_t end;
	uint64_t write_length;
	uint64_t old_allocation;
	uint64_t old_file_size;
	bool allocation_grew = false;
	bool file_size_reserved = false;
	MemfsResult result;

	if (node == NULL || buffer == NULL || bytes_written == NULL)
		return MEMFS_ERR_INVALID;
	if (MEMFS_NODE_IS_DIRECTORY(node))
		return MEMFS_ERR_IS_DIRECTORY;

	*bytes_written = 0;
	old_allocation = node->allocation_size;
	old_file_size = node->file_size;

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
	} else {
		if (end > old_file_size) {
			result = memfs_account_file_size(node, old_file_size, end);
			if (result != MEMFS_OK)
				return result;

			file_size_reserved = true;
		}

		if (end > node->allocation_size) {
			result = memfs_resize_allocation(node, end);
			if (result != MEMFS_OK) {
				if (file_size_reserved) {
					(void)memfs_account_file_size(node, end, old_file_size);
				}
				return result;
			}

			allocation_grew = true;
		}
	}

	write_length = end - offset;
	result = memfs_storage_write_range(node, (const uint8_t*)buffer, offset, write_length);
	if (result != MEMFS_OK) {
		if (allocation_grew)
			(void)memfs_resize_allocation(node, old_allocation);

		if (file_size_reserved) {
			(void)memfs_account_file_size(node, end, old_file_size);
		}
		return result;
	}

	if (!constrained_io && end > old_file_size)
		node->file_size = end;

	*bytes_written = (uint32_t)write_length;
	memfs_node_set_last_write_time(node, memfs_now());
	memfs_node_set_change_time(node, memfs_node_get_last_write_time(node));
	node->attributes |= FILE_ATTRIBUTE_ARCHIVE;
	return MEMFS_OK;
}

MemfsResult memfs_node_replace_security(MemfsNode* node, PSECURITY_DESCRIPTOR security, uint32_t security_size) {
	MemfsSecurity* replacement;
	MemfsSecurity* old;
	MemfsResult result;

	if (node == NULL || security == NULL || security_size == 0) {
		return MEMFS_ERR_INVALID;
	}

	if (GetSecurityDescriptorLength(security) != security_size)
		return MEMFS_ERR_INVALID;

	if (memfs_security_equal(memfs_node_get_security(node), security))
		return MEMFS_OK;

	result = memfs_security_create(security, &replacement);
	if (result != MEMFS_OK)
		return result;

	old = memfs_node_get_security(node);
	if (!memfs_node_set_security_meta(node, replacement)) {
		memfs_security_release(replacement);
		return MEMFS_ERR_NO_MEMORY;
	}
	memfs_security_release(old);

	memfs_node_set_change_time(node, memfs_now());
	return MEMFS_OK;
}
