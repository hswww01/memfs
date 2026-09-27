#include "memfs_meta_table.h"

#include <Windows.h>
#include <stdlib.h>

struct MemfsMetaEntry {
	uint64_t key;
	void* value;
	MemfsMetaEntry* next;
};

struct MemfsMetaTable {
	SRWLOCK lock;
	uint32_t bucket_count;
	uint32_t count;
	MemfsMetaEntry** buckets;
};

static uint32_t meta_hash(uint64_t key, uint32_t count) {
	key ^= key >> 33;
	key *= 0xff51afd7ed558ccdULL;
	key ^= key >> 33;
	return (uint32_t)key % count;
}

MemfsMetaTable* memfs_meta_table_create(uint32_t bucket_count) {
	MemfsMetaTable* table;

	if (bucket_count == 0)
		bucket_count = 1024;

	table = calloc(1, sizeof(*table));
	if (table == NULL)
		return NULL;

	table->buckets = calloc(bucket_count, sizeof(*table->buckets));
	if (table->buckets == NULL) {
		free(table);
		return NULL;
	}

	InitializeSRWLock(&table->lock);
	table->bucket_count = bucket_count;
	return table;
}

void memfs_meta_table_destroy(MemfsMetaTable* table, void (*free_value)(void*)) {
	uint32_t i;

	if (table == NULL)
		return;

	for (i = 0; i < table->bucket_count; i++) {
		MemfsMetaEntry* entry = table->buckets[i];
		while (entry != NULL) {
			MemfsMetaEntry* next = entry->next;
			if (free_value)
				free_value(entry->value);
			free(entry);
			entry = next;
		}
	}

	free(table->buckets);
	free(table);
}

bool memfs_meta_table_insert(MemfsMetaTable* table, uint64_t key, void* value) {
	uint32_t bucket;
	MemfsMetaEntry* entry;

	if (table == NULL)
		return false;

	bucket = meta_hash(key, table->bucket_count);
	AcquireSRWLockExclusive(&table->lock);

	for (entry = table->buckets[bucket]; entry; entry = entry->next) {
		if (entry->key == key) {
			ReleaseSRWLockExclusive(&table->lock);
			return false;
		}
	}

	entry = calloc(1, sizeof(*entry));
	if (entry) {
		entry->key = key;
		entry->value = value;
		entry->next = table->buckets[bucket];
		table->buckets[bucket] = entry;
		table->count++;
	}

	ReleaseSRWLockExclusive(&table->lock);
	return entry != NULL;
}

void* memfs_meta_table_lookup(MemfsMetaTable* table, uint64_t key) {
	MemfsMetaEntry* entry;
	void* value = NULL;

	if (table == NULL)
		return NULL;

	AcquireSRWLockShared(&table->lock);
	for (entry = table->buckets[meta_hash(key, table->bucket_count)]; entry; entry = entry->next) {
		if (entry->key == key) {
			value = entry->value;
			break;
		}
	}
	ReleaseSRWLockShared(&table->lock);
	return value;
}

void* memfs_meta_table_remove(MemfsMetaTable* table, uint64_t key) {
	MemfsMetaEntry** link;
	void* value = NULL;

	if (table == NULL)
		return NULL;

	AcquireSRWLockExclusive(&table->lock);
	link = &table->buckets[meta_hash(key, table->bucket_count)];
	while (*link) {
		MemfsMetaEntry* entry = *link;
		if (entry->key == key) {
			*link = entry->next;
			value = entry->value;
			free(entry);
			table->count--;
			break;
		}
		link = &entry->next;
	}
	ReleaseSRWLockExclusive(&table->lock);
	return value;
}

uint32_t memfs_meta_table_count(const MemfsMetaTable* table) {
	return table ? table->count : 0;
}
