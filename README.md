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

`MemfsDir` does not allocate a hash table when the directory is created. Every child is always stored in an intrusive treap. The treap is the authoritative ordered index and provides expected O(log n) insertion, deletion and lookup together with naturally sorted enumeration.

When a directory reaches 64 children, memfs lazily creates an open-addressing hash index. The hash is only an acceleration index; the treap remains authoritative. If the directory becomes small again, the hash is discarded.

This avoids the old design's fixed bucket allocation for every directory while keeping large-directory lookup fast.

Local A/B benchmark, 15,000 pseudo-randomly inserted files:

| operation | old hash + sorted list | treap + lazy hash |
|---|---:|---:|
| create 15,000 entries | ~320-335 ms | ~11.5-11.9 ms |
| lookup 15,000 entries | ~1.64-1.70 ms | ~1.35-1.48 ms |

For 10,000 empty directories, measured process private-memory growth fell from about 5.97 MiB to 4.50 MiB.

## File storage

Windows `AllocationSize` and actual RAM residency are intentionally separate.

### Tiny files

Files up to 4 KiB use an encoded small blob whose plaintext capacity grows in 8-byte units. Windows still reports a minimum 512-byte allocation unit because the WinFsp volume uses 512-byte sectors, but the actual data blob no longer starts at 512 bytes.

Uncompressed/unencrypted local measurements:

| file size | Windows AllocationSize | tracked resident blob |
|---:|---:|---:|
| 1 B | 512 B | 16 B |
| 7 B | 512 B | 16 B |
| 8 B | 512 B | 16 B |
| 9 B | 512 B | 24 B |
| 100 B | 512 B | 112 B |
| 500 B | 512 B | 512 B |
| 1024 B | 1024 B | 1032 B |

`resident_bytes` includes the memfs encoded-blob header but not the host heap allocator's private bookkeeping.

### Sparse paged files

Files that grow past 4 KiB, or receive a high-offset write, use a two-level sparse page table:

- logical page size: 4 KiB;
- 256 page pointers per second-level group;
- one group covers 1 MiB of file address space;
- page groups and data pages are allocated only on demand;
- absent pages read as zero.

`AllocationSize` is reserved against configured volume capacity in 512-byte units, preserving Windows `FileSize <= AllocationSize` semantics. Extending an empty 64 MiB file therefore reserves 64 MiB of volume capacity while allocating no data pages.

Truncation releases pages. A file that shrinks below 4 KiB can demote back to the compact tiny representation.

The original whole-file `realloc` implementation took about 1.4 seconds to append 32 MiB in 64 KiB writes on the current machine. Sparse paged storage reduced the same benchmark to roughly 9 ms.

## Compression

Compression is optional and local to each tiny blob or 4 KiB page. memfs tries Zstd only when enabled and keeps the compressed representation only when it saves enough bytes to justify the metadata/cost. Incompressible pages stay raw.

For 16 MiB of highly compressible repeated data in the local benchmark:

| mode | tracked resident | resident/logical |
|---|---:|---:|
| raw | ~16.03 MiB | ~100.2% |
| Zstd | ~108 KiB | ~0.66% |
| encryption only | ~16.19 MiB | ~101.2% |
| Zstd + encryption | ~268 KiB | ~1.64% |

Compression is not enabled by default because incompressible workloads would spend CPU without reducing memory.

## Encryption

Encryption is also local to each non-zero blob/page. The encoding order is:

1. optional Zstd compression;
2. XChaCha20-Poly1305 encryption;
3. a fresh random 192-bit nonce for every rewrite.

AEAD additional authenticated data binds ciphertext to the node identity, page index (or tiny-blob sentinel), plaintext size and codec flags. Normal reads therefore detect ciphertext or authenticated-metadata tampering.

The in-memory 256-bit key is kept in a dedicated field, locked with `sodium_mlock` on a best-effort basis, and explicitly zeroed during teardown.

Encryption protects the stored page representation in the memfs heap. It is not process-memory isolation: plaintext necessarily exists transiently in application I/O buffers and small stack work buffers during reads, writes, compression and decompression.

All-zero pages remain implicit sparse holes and allocate no ciphertext.

## Concurrency

memfs uses WinFsp's FINE operation guard. WinFsp protects namespace operations with its shared/exclusive namespace lock, while I/O to different files can execute concurrently. The FSD continues to synchronize operations on each individual file.

A narrow `SRWLOCK accounting_lock` protects only cross-file shared accounting (`used_bytes` and global `resident_bytes`). The file data path does not take a global filesystem lock.

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

- Intern/share identical security descriptors to reduce per-node metadata further.
- Named streams, reparse points and extended attributes.
- Persistence/snapshot support.
- Password-based key derivation if persistent encrypted images are introduced.
- More rename/delete/open race stress tests.
- Optional adaptive codec policy based on observed per-file compressibility.
