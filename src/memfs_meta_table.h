#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct MemfsMetaTable MemfsMetaTable;
typedef struct MemfsMetaEntry MemfsMetaEntry;

MemfsMetaTable* memfs_meta_table_create(uint32_t bucket_count);
void memfs_meta_table_destroy(MemfsMetaTable* table, void (*free_value)(void*));

bool memfs_meta_table_insert(MemfsMetaTable* table, uint64_t key, void* value);
void* memfs_meta_table_lookup(MemfsMetaTable* table, uint64_t key);
void* memfs_meta_table_remove(MemfsMetaTable* table, uint64_t key);

uint32_t memfs_meta_table_count(const MemfsMetaTable* table);
