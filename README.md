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

- Windows 10/11 x64, or Windows 11 ARM64 running the x64 user-mode executable under Windows x64 emulation. 32-bit Windows and Windows 10 ARM64 are not supported.
- WinFsp Git source tree at `D:\\src\\winfsp`; `scripts\\prepare-winfsp-static.ps1` derives the exact source commit from the installed signed driver and prepares `D:\\src\\winfsp-memfs-static`.
- Visual Studio C/C++ build tools.
- LLVM/clang-cl.
- CMake + Ninja.
- vcpkg at `D:\vcpkg` for the supplied presets.

The vcpkg manifest installs `zstd` and `libsodium`. Before the first build, run `scripts\\prepare-winfsp-static.ps1`. It reads the installed signed WinFsp driver's FileVersion, resolves the matching Git commit, creates `D:\\src\\winfsp-memfs-static`, applies the minimal static-user-mode support, and builds Debug/Release static libraries from that exact source version. The supplied presets then link that matching static runtime, so `memfs.exe` does not require `winfsp-x64.dll` at runtime.

Both signed WinFsp kernel drivers (`winfsp-x64.sys` and `winfsp-a64.sys`) are embedded into the single x64 `memfs.exe`. CMake requires the two SYS files to come from the same signed WinFsp release and to match the statically linked x64 user-mode runtime. At runtime memfs uses `IsWow64Process2` to determine the native Windows machine architecture: AMD64 hosts use the x64 SYS, while Windows 11 ARM64 hosts running the x64 EXE under emulation use the ARM64 SYS. Kernel drivers are never emulated. CTest also calls `FspVersion()` from the linked static library, checks both embedded SYS resources, and parses the built PE import/delay-import tables, so a stale cached static library or reintroduced `winfsp*.dll` dependency fails verification. If no compatible official WinFsp driver is available, an elevated memfs process installs/starts the matching embedded signed fallback driver and retries `FspFileSystemCreate`.

## Build

```powershell
cd D:\work\memfs

.\scripts\prepare-winfsp-static.ps1

cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug

cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset x64-release
```

## Release package and third-party notices

Create the current x64 release package with:

```powershell
.\scripts\package-release.ps1
```

The package contains `memfs.exe`, `THIRD_PARTY_NOTICES.md`, SHA-256 sums,
and the authoritative license texts for WinFsp, libsodium and zstd.

This same x64 package is used on Windows 11 ARM64. Windows runs `memfs.exe` under x64 emulation, while memfs selects the embedded native ARM64 WinFsp kernel driver when a fallback driver is required.

The current build statically links WinFsp user-mode code. WinFsp's local
`License.txt` states GPLv3 and its FLOSS special exception explicitly covers
linking with the platform WinFsp DLLs, not a general static-linking exception.
Before proprietary distribution, obtain appropriate WinFsp commercial
permission or otherwise ensure the chosen distribution terms satisfy the
applicable WinFsp/GPLv3 obligations. See `THIRD_PARTY_NOTICES.md`.

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
--service               Run under the Windows Service Control Manager
--uninstall-private-driver  Remove only MemfsC-owned private WinFsp fallback service/files
--stats                 Print a human-readable runtime snapshot at mount and stop
--stats-json            Print stable one-line JSON runtime snapshots at mount and stop
--help
```

Stop with Ctrl+C.

## Windows Service

The production path is a normal Windows `SERVICE_WIN32_OWN_PROCESS` service named `MemfsC`. The executable uses the Service Control Manager directly (`StartServiceCtrlDispatcherW`, `RegisterServiceCtrlHandlerExW`) and handles both STOP and SHUTDOWN controls. Service stop first stops the WinFsp dispatcher and then tears down the filesystem/allocator before reporting `SERVICE_STOPPED`.

Run the following from an elevated shell. Keep the service name `MemfsC`, because that is the service name registered by the executable:

```powershell
$exe = (Resolve-Path .\build\x64-release\memfs.exe).Path
$bin = '"' + $exe + '" --service --mount R: --size 512M --label MEMFS'
sc.exe create MemfsC "binPath=" $bin "start=" auto
sc.exe description MemfsC "MemfsC volatile memory filesystem"

sc.exe start MemfsC
sc.exe queryex MemfsC

sc.exe stop MemfsC
sc.exe delete MemfsC
```

A helper that still uses `sc.exe` for all service control is included:

```powershell
.\scripts\memfs-service.ps1 install -Exe .\build\x64-release\memfs.exe -Mount R: -Size 512M
.\scripts\memfs-service.ps1 start
.\scripts\memfs-service.ps1 query
.\scripts\memfs-service.ps1 stop
.\scripts\memfs-service.ps1 delete
.\scripts\memfs-service.ps1 purge-driver -Exe .\build\x64-release\memfs.exe
```

The service runs as LocalSystem by default. The first driver installation requires administrator rights. If a compatible official WinFsp SxS driver is already installed, memfs reuses it and does not create a parallel driver service. If the driver is missing or unloadable, memfs extracts the embedded signed WinFsp SYS resource, registers it through SCM, starts it, and retries `FspFileSystemCreate`.

Abnormal service recovery is covered by a destructive-but-self-cleaning integration test:

```powershell
.\tests\service-recovery.ps1 -Exe .\build\x64-release\memfs.exe -Drive R: -Size 64M
```

The test installs `MemfsC` with the configured SCM restart policy, starts it, writes a sentinel file, forcibly terminates the service process, waits for SCM to restart it with a new PID and remount a fresh volatile filesystem, verifies post-restart I/O, and finally stops/deletes the service. On a machine with official WinFsp installed it also asserts that no private `WinFsp+MemfsC` fallback driver appears.

If Service initialization fails, `sc.exe query MemfsC` preserves the underlying WinFsp/NTSTATUS value in `SERVICE_EXIT_CODE` instead of exposing only the generic Windows service error 1066.

### Deployment verification harness

`scripts\verify-deployment.ps1` provides repeatable deployment checks without silently modifying an official WinFsp installation.

On a development machine with official WinFsp already installed:

```powershell
.\scripts\verify-deployment.ps1 -Scenario All -Report .agent\deployment.json
```

This runs the release deployment gate tests, verifies the no-`MemfsC` service state, mounts a real filesystem through the installed official WinFsp driver, exercises normal file I/O, and confirms that no private `WinFsp+MemfsC` fallback service or `memfs-winfsp-*.sys` file was created.

The embedded private-driver path requires a genuinely clean VM with no official WinFsp SxS driver installed. It is intentionally opt-in:

```powershell
.\scripts\verify-deployment.ps1 -Scenario PrivateFallback -ExercisePrivateFallback -Report C:\temp\memfs-private-fallback.json
```

That scenario requires administrator rights, refuses to run if an official WinFsp driver is detected, mounts memfs to force the embedded fallback path, validates that the private driver points only to MemfsC-owned filenames, and removes the private service/files afterward unless `-KeepPrivateDriver` is specified.

## Directory design

MemfsDir allocates no hash table for small directories. Every child is stored in an intrusive treap, which is the authoritative ordered index and provides expected O(log n) insertion/deletion/lookup plus naturally sorted enumeration.

At 256 children memfs lazily adds a Robin Hood open-addressing hash accelerator. The hash targets an 85% maximum load, uses backward-shift deletion instead of tombstones, and is discarded again when the directory falls below half the activation threshold. The treap remains authoritative, so a hash allocation failure never corrupts the namespace.

Directory/file metadata is packed aggressively:

- MemfsNode is 128 bytes on x64 and is compile-time pinned to the allocator's 128-byte class; the previous 136-byte layout rounded up to a 192-byte slab slot, so every live file/directory node now consumes 64 fewer slab bytes (33.3% less node-slot memory).
- directory state, tiny-file storage and paged-file storage are mutually exclusive and share one pointer-width union;
- a newly created node, its optional MemfsDir, and its initial name are one allocation;
- inherited ACLs are reference-counted and shared;
- nodes removed from the namespace reuse treap links for the orphan/open-handle list.
- the WinFsp `IndexNumber` is derived from the stable node address through a per-filesystem seeded 64-bit permutation instead of storing another 8-byte field; it remains stable across rename/unlink while the node is alive.

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
### Allocator v2 and auto capacity

The allocator v2 backend is size-class based rather than object-type based. Node, directory, page-group, name and generic allocation APIs all converge on the same rounded-size class, so there is one **logical size-class pool** rather than separate node/name/generic pools. Internally, each class uses 16 concurrency lanes to avoid a single hot SRW lock; these lanes partition synchronization/slab ownership only and are not business-type pools or permanent per-thread arenas. The local lane is always the fast path. If it has no free object, an atomic available-lane mask lets the allocator reuse partial free capacity from another lane of the same size class before committing a new slab; when no lane has capacity, the scan is skipped entirely. Empty slabs still return immediately to the VM backend. This keeps the memory semantics of one shared size-class pool without putting every allocation behind one global lock. A three-run A/B benchmark against the former 8-lane layout showed the largest gains at high concurrency: at 16 threads, 64-byte alloc/free throughput improved by about 114% and 4 KiB alloc/free throughput by about 209%, while single-thread throughput stayed effectively flat. The extra allocator-state metadata is about 12 KiB per filesystem.

The process-wide control allocation used for `Memfs` itself goes through the same size-class/slab/area machinery; only allocator-state bootstrap reaches the VM backend directly. Small allocations use adaptive slab backing (4 KiB / 8 KiB / 16 KiB / 32 KiB / 64 KiB according to object size); allocations above the small-object threshold use area allocations. `reserved_bytes` is address-space reservation only; `committed_bytes` is the real OS-backed committed memory, and `physical_bytes` is an alias for `committed_bytes`, not a separate source. `live_bytes` is the caller-visible payload/object bytes currently allocated.

Zero-initialized typed allocations (`node`, `dir`, `page_group`) keep their existing safety contract. A separate `memfs_allocator_alloc_uninit()` fast path is used only where the caller immediately overwrites the complete requested payload (encoded page blobs, copied security descriptors, temporary path buffers and copied names). This avoids redundant memset traffic without exposing stale bytes to code that expects zero-filled objects. The allocator contention benchmark reports both `zero` and `uninit` modes.

`capacity_auto=true` makes memfs derive its writable allowance from current system memory and allocator backing instead of using a fixed user capacity. `memfs_auto_allowance_bytes()` reports the current allowance, while `used_bytes`, `resident_bytes`, `committed_bytes` and `physical_bytes` remain separate measurements: logical quota, resident payload, and allocator physical backing. The benchmark reports these separately so high-water checks do not confuse allocator backing with logical file usage.

Runtime diagnostics are opt-in and do not start a sampling thread. `memfs_get_runtime_stats()` returns a point-in-time `MemfsRuntimeStats` snapshot containing logical used/free bytes, resident bytes, allocator live/reserved/committed/physical bytes, slab/area/cache counts, scavenger totals, and the current auto-capacity allowance plus its 256 MiB hard and 512 MiB soft pressure margins.

For interactive diagnostics:

```powershell
.\build\x64-release\memfs.exe --mount R: --size auto --stats
.\build\x64-release\memfs.exe --mount R: --size auto --stats-json
```

`--stats` prints a stable `key=value` snapshot after mount and immediately before shutdown. `--stats-json` suppresses the normal mount banner and emits one JSON object per snapshot with `type=memfs_stats` and `phase=mounted|stopping`, making redirected stdout suitable for machine parsing. The API is also available to service/control-plane code without enabling any background sampler.

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

## Long-run memory drift soak

`memfs_soak_bench` runs a mixed core workload with compression and encryption enabled. Each cycle covers tiny-file create/read/rename/unlink, a 256 KiB large-file write with truncate/regrow and zero-fill verification, and a 64 MiB high-offset sparse write/read/truncate/regrow. Every sample reports logical/resident bytes, allocator live/reserved/committed/physical bytes, process private bytes, slab/area/cache counts, and the final summary reports linear private/committed drift slopes.

The default mode is intentionally short for CI:

```powershell
.\build\x64-release\memfs_soak_bench.exe
.\build\x64-release\memfs_soak_bench.exe --seconds 10 --sample-ms 1000
```

Long soak mode is explicit and accepts 10 through 60 minutes:

```powershell
.\build\x64-release\memfs_soak_bench.exe --soak 10
.\build\x64-release\memfs_soak_bench.exe --soak 60 --sample-ms 5000
```

Before measurement the benchmark warms all workload shapes and codec/crypto paths. At exit it scavenges allocator caches and requires logical used, resident, allocator live objects/bytes, reserved, committed, physical, slab and area state to return to the post-warmup baseline. The private-byte slope is reported separately because Windows/runtime libraries may retain process-private bookkeeping that is not memfs allocator backing.

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
  memfs_core_test.c      unit/stress/concurrency/codec tests
  integration.ps1       real mounted-drive integration test
  service-recovery.ps1  SCM abnormal-termination restart/remount integration test
scripts/
  prepare-winfsp-static.ps1 derive/build a static WinFsp runtime matching the signed driver
  memfs-service.ps1         install/start/query/stop/delete wrapper around sc.exe
  verify-deployment.ps1     repeatable official/no-service/private-fallback deployment harness
```

## Planned next steps

- Global security-descriptor interning beyond the current parent/inherited ACL sharing.
- Named streams, reparse points and extended attributes.
- Persistence/snapshot support.
- Password-based key derivation if persistent encrypted images are introduced.
- More rename/delete/open race stress tests.
- Continue profiling unified size-class shard counts, slab sizing and the slab/area threshold under real workloads; do not introduce object-type-specific physical pools.
