# memfs

A high-performance volatile Windows memory file system written in C17 on top of the native WinFsp API.

## Features

- Pure C17, CMake, Ninja, clang-cl and vcpkg.
- Native WinFsp API; no FUSE layer.
- Case-insensitive lookup with case-preserved names.
- Adaptive directory index: intrusive treap plus a lazy open-addressing hash for large directories.
- Create/open/read/write/truncate/rename/move/delete and directory enumeration.
- Sparse file storage with 4 KiB logical pages.
- Tiny-file storage with 8-byte plaintext granularity.
- Optional per-page Zstd compression.
- Optional per-page XChaCha20-Poly1305 authenticated encryption.
- Windows file attributes, timestamps and ACL/security descriptors.
- WinFsp FINE operation guard so different files can perform I/O concurrently.
- Thread-safe global capacity and resident-memory accounting.

All file-system contents are volatile. Unmounting or terminating the process loses all files.

## Prerequisites

- Windows 10/11 x64.
- WinFsp installed under `C:\Program Files (x86)\WinFsp` or `C:\Program Files\WinFsp`.
- Visual Studio C/C++ build tools.
- LLVM/clang-cl.
- CMake + Ninja.
- vcpkg at `D:\vcpkg` for the supplied presets.

The vcpkg manifest installs `zstd` and `libsodium`. WinFsp itself is consumed from its installed SDK.

## Build

```powershell
cd D:\work\memfs

cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug

cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset x64-release
```

## Run

Basic 512 MiB RAM disk:

```powershell
.\build\x64-release\memfs.exe --mount R:
```

Custom capacity and label:

```powershell
.\build\x64-release\memfs.exe --mount R: --size 1G --label FASTRAM
```

Compression:

```powershell
.\build\x64-release\memfs.exe --mount R: --compress
.\build\x64-release\memfs.exe --mount R: --compression-level 3
```

Encryption with a random process-lifetime key:

```powershell
.\build\x64-release\memfs.exe --mount R: --encrypt
```

For a caller-supplied 256-bit key, prefer an environment variable so the key does not appear in the command line:

```powershell
$env:MEMFS_KEY = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
.\build\x64-release\memfs.exe --mount R: --compress --key-env MEMFS_KEY
```

`--key-hex <64hex>` is also supported for testing, but command-line arguments may be visible to other processes or diagnostic tools.

Options:

```text
--mount <path>          Drive letter or directory mount point
--size <bytes>          Capacity; K/M/G suffix supported
--label <name>          Volume label
--threads <n>           WinFsp dispatcher threads; 0 = automatic
--compress              Enable Zstd compression at level 1
--compression-level <n> Enable Zstd level 1..22
--encrypt               Use a random XChaCha20-Poly1305 key
--key-hex <64hex>       Use a fixed 256-bit key
--key-env <name>        Read a 64-hex key from an environment variable
--debug                 Enable WinFsp debug logging
--help
```

Stop with Ctrl+C.

## Directory design

MemfsDir allocates no hash table for small directories. Every child is stored in an intrusive treap, which is the authoritative ordered index and provides expected O(log n) insertion/deletion/lookup plus naturally sorted enumeration.

At 256 children memfs lazily adds a Robin Hood open-addressing hash accelerator. The hash targets an 85% maximum load, uses backward-shift deletion instead of tombstones, and is discarded again when the directory falls below half the activation threshold. The treap remains authoritative, so a hash allocation failure never corrupts the namespace.

Directory/file metadata is packed aggressively:

- MemfsNode is 144 bytes on x64.
- directory state, tiny-file storage and paged-file storage are mutually exclusive and share one pointer-width union;
- a newly created node, its optional MemfsDir, and its initial name are one allocation;
- inherited ACLs are reference-counted and shared;
- nodes removed from the namespace reuse treap links for the orphan/open-handle list.

Local A/B benchmark with 50,000 files in one directory:

| metric | 26638fb baseline | current |
|---|---:|---:|
| create 50k | ~32.9-33.8 ms | ~21.9-23.7 ms |
| lookup 50k | ~11.6-13.7 ms | ~6.9-9.8 ms |
| enumerate 50k | ~1.35-1.91 ms | ~0.6-1.6 ms |
| process private-memory delta / entry | ~457-458 B | ~220 B |
| hash slots at 50k | 131,072 | 65,536 |

The current test suite also removes half of a large Robin Hood table, validates positive/negative lookups, reinserts new names and re-enumerates the ordered treap to exercise backward-shift deletion.

## File storage

Windows-visible AllocationSize, internal quota accounting and actual RAM residency are intentionally separate.

### Tiny files

Files up to 4 KiB use an encoded small representation. Plaintext capacity uses exact 1/2/4/8-byte tiny classes and then 8-byte granularity. Uncompressed/unencrypted data up to 8 bytes is stored directly inside the inode union, so it performs no additional heap allocation at all.

The WinFsp volume still uses 512-byte sectors, therefore the AllocationSize reported to Windows is rounded to 512 bytes. Internally, quota accounting is byte-granular: a 1-byte file consumes 1 byte of configured memfs capacity, not 512 bytes.

Uncompressed/unencrypted local measurements:

| file size | Windows AllocationSize | internal quota | tracked data resident |
|---:|---:|---:|---:|
| 1 B | 512 B | 1 B | 0 B |
| 7 B | 512 B | 7 B | 0 B |
| 8 B | 512 B | 8 B | 0 B |
| 9 B | 512 B | 9 B | 24 B |
| 100 B | 512 B | 100 B | 112 B |
| 500 B | 512 B | 500 B | 512 B |
| 1024 B | 1024 B | 1024 B | 1032 B |

With encryption enabled a 1-byte file uses 33 bytes of tracked encoded storage: 8-byte blob header + 8-byte nonce sequence + 1 byte payload + 16-byte Poly1305 tag.

resident_bytes tracks encoded data blobs/pages and intentionally excludes host allocator bookkeeping and namespace metadata.

### Sparse paged files

Files that grow past 4 KiB, or receive a high-offset write, use sparse 4 KiB logical pages:

- one logical group covers 1 MiB (256 pages);
- a group has a 256-bit presence bitmap and a compact pointer vector containing only pages that actually exist;
- top-level group entries are a compact sorted array of {group_index, group*} and are binary-searched;
- very high sparse offsets therefore do not allocate a giant intermediate pointer array;
- absent pages read as zero.

Extending an empty file reserves byte-granular memfs quota but allocates no data pages. The aligned 512-byte AllocationSize exists only at the WinFsp reporting boundary.

Truncation releases encoded pages and empty groups. A file that shrinks below 4 KiB can demote back to the compact tiny representation.

The original whole-file realloc implementation took about 1.4 seconds to append 32 MiB in 64 KiB writes on the current machine. Sparse paged storage reduced the same benchmark to roughly 9 ms.

## Compression

Compression is optional and local to each tiny blob or 4 KiB page. memfs uses Zstd and keeps the compressed representation only when it saves enough bytes to justify the metadata and CPU cost.

Compression is adaptive per file. Repeatedly incompressible pages raise a small score; after the score crosses the skip threshold, paged files only probe one page out of 16 for renewed compressibility. A successful probe lowers the score again. Tiny blobs remain cheap enough to probe on every rewrite.

For 16 MiB of highly compressible repeated data in the current build:

| mode | tracked resident | resident/logical | write time (local run) |
|---|---:|---:|---:|
| raw | 16.03 MiB | 100.195% | ~4.4 ms |
| Zstd | 108 KiB | 0.659% | ~19-31 ms |
| encryption only | 16.13 MiB | 100.781% | ~15.6-16.3 ms |
| Zstd + encryption | 204 KiB | 1.245% | ~11.5-11.7 ms |

Compression is not enabled by default because truly incompressible workloads would otherwise spend CPU without reducing memory.

## Encryption

Encryption is optional and local to each non-zero blob/page. The encoding order is:

1. optional Zstd compression;
2. XChaCha20-Poly1305 authenticated encryption.

Each memfs instance creates a random 128-bit nonce prefix. Every encoded rewrite obtains an atomic monotonically increasing 64-bit sequence; together they form the 192-bit XChaCha nonce. Only the 8-byte sequence is stored with each blob/page, reducing per-page nonce overhead while retaining a fresh nonce space for every mount even when a fixed key is reused.

AEAD additional authenticated data binds ciphertext to the node identity, storage/page index (or tiny-blob sentinel), nonce sequence, plaintext size and codec flags. Normal reads therefore detect ciphertext or authenticated-metadata tampering.

The in-memory 256-bit key is kept in a dedicated field, locked with sodium_mlock on a best-effort basis, and explicitly zeroed during teardown. The nonce prefix is also zeroed during teardown.

Encryption protects the stored page representation in the memfs heap. It is not process-memory isolation: plaintext necessarily exists transiently in application I/O buffers and small stack work buffers during reads, writes, compression and decompression.

All-zero pages remain implicit sparse holes and allocate no ciphertext.

## Concurrency

memfs uses WinFsp's FINE operation guard. WinFsp protects namespace operations with its shared/exclusive namespace lock, while I/O to different files can execute concurrently. The FSD continues to synchronize operations on each individual file.

Cross-file capacity and resident accounting use Interlocked atomics rather than a global filesystem lock. The normal file-data path therefore has no memfs-wide mutex.

The core test suite includes concurrent writes to multiple files with compression and encryption enabled.

## Handle lifetime and deletion

WinFsp uses the node pointer as `FileContext`. Open/create increments the node's open count. Delete-on-cleanup removes the node from the namespace, but the node remains alive until the final close.

## Integration test

The integration test mounts a real WinFsp drive and exercises normal Windows file APIs, sparse writes, rename/move/enumeration/delete:

```powershell
& .\tests\integration.ps1 -Exe .\build\x64-release\memfs.exe -Drive R:
& .\tests\integration.ps1 -Exe .\build\x64-release\memfs.exe -Drive R: -ExtraArgs @("--compress")
```

The current test suite has also been run successfully in plain, compression-only, encryption-only and compression+encryption mount modes.

## Source layout

```text
src/
  main.c            command-line mount process
  memfs_core.h      tree/storage/codec API and structures
  memfs_core.c      directory index, sparse data, compression, encryption
  memfs_winfsp.h    WinFsp adapter API
  memfs_winfsp.c    WinFsp callbacks

tests/
  memfs_core_test.c unit/stress/concurrency/codec tests
  integration.ps1   real mounted-drive integration test
```

## Planned next steps

- Global security-descriptor interning beyond the current parent/inherited ACL sharing.
- Named streams, reparse points and extended attributes.
- Persistence/snapshot support.
- Password-based key derivation if persistent encrypted images are introduced.
- More rename/delete/open race stress tests.
- Optional dedicated metadata slab allocator if profiling shows CRT heap bookkeeping is still material.
