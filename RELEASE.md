# Release verification

Verified on 2026-09-30 on Windows x64.

## Build inputs

- memfs repository: `D:\work\memfs`
- WinFsp Git repository: `D:\src\winfsp`
- signed embedded drivers from the same WinFsp SxS release:
  - x64: `C:\Program Files (x86)\WinFsp\SxS\sxs.20251221T124141Z\bin\winfsp-x64.sys`
  - ARM64: `C:\Program Files (x86)\WinFsp\SxS\sxs.20251221T124141Z\bin\winfsp-a64.sys`
- both driver FileVersions: `2.1.25156.ddca7bd`
- matching WinFsp source commit: `ddca7bd` (tag `v2.1`)
- embedded driver SHA-256:
  - x64: `03553FFFACD362F4A9A08C00B4F236A82354A183BC6028494FB32055386E13C9`
  - ARM64: `BF3B1BF90A7C070D456FC43C8552CCF8BE85CDDDEF4F3B19091B4A64AE4E686C`
- generated matching static worktree: `D:\src\winfsp-memfs-static`

Run this before a clean build:

```powershell
.\scripts\prepare-winfsp-static.ps1
```

The script reads the installed signed driver's FileVersion, extracts its Git revision,
creates a detached worktree at that exact commit, applies the minimal static-user-mode
support, and builds both Debug and Release `winfsp-static-x64.lib` archives.

This avoids mixing a newer user-mode runtime with an older signed kernel driver.

## Static WinFsp version contract

The build now enforces the WinFsp version in three places:

1. CMake reads `MyCanonicalVersion` from the selected WinFsp source tree.
2. CMake reads the embedded SYS FileVersion and rejects a different major/minor.
3. `memfs_winfsp_version_test` calls `FspVersion()` from the linked static archive and
   verifies the encoded version at runtime.

A deliberate negative configure test using WinFsp v2.2 static source with the v2.1
signed driver fails with:

```text
WinFsp version mismatch: static runtime expects 2.2,
but embedded driver ... is 2.1.25156.ddca7bd
```

The supplied presets use:

- `VCPKG_TARGET_TRIPLET=x64-windows-static`
- `MEMFS_STATIC_WINFSP=ON`
- `WINFSP_SOURCE_ROOT=D:/src/winfsp-memfs-static`

When a source root is supplied, CMake forcibly selects that tree's
configuration-matched static archive. This prevents an old CMake cache from silently
linking a different WinFsp static library.

## Build and test matrix

```powershell
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug --output-on-failure

cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset x64-release --output-on-failure
```

Both Debug and Release pass 11/11 tests:

1. `memfs_mt_stress_test`
2. `memfs_core_test`
3. `memfs_soak_smoke`
4. `memfs_no_crt_heap_guard`
5. `memfs_no_crt_heap_guard_good_fixture`
6. `memfs_no_crt_heap_guard_bad_fixture`
7. `memfs_driver_test`
8. `memfs_static_winfsp_no_dll_import`
9. `memfs_static_winfsp_version`
10. `memfs_cli_help`
11. `memfs_embedded_winfsp_resources`


## Real mounted-drive integration

The Release executable passed the real WinFsp integration test in all four modes:

- plain: PASS
- `--compress`: PASS
- `--encrypt`: PASS
- `--compress --encrypt`: PASS

Each run mounted R:, exercised normal Windows file APIs, binary I/O, sparse-file
zero-fill, rename/move/enumeration/delete, and unmounted cleanly.

## Static WinFsp verification

Matching v2.1 static libraries:

- Debug:
  `D:\src\winfsp-memfs-static\build\VStudio\build\Debug\winfsp-static-x64.lib`
  SHA-256:
  `4F7B338947B7D64A38F7C6D7905C445100F004430FAD38F9D63C949010A898A7`
- Release:
  `D:\src\winfsp-memfs-static\build\VStudio\build\Release\winfsp-static-x64.lib`
  SHA-256:
  `7184AC5FACCDD24F266A980C0037AB261D05DC3DAD466741E029805742C8032F`


`memfs_static_winfsp_no_dll_import` verifies that the final EXE has no WinFsp DLL
import or delay-import dependency.

The Release EXE therefore does not require `winfsp-x64.dll` at runtime.

## Embedded driver behavior

The x64 user-mode executable embeds both signed kernel drivers from the matching WinFsp SxS release:

```text
x64:   winfsp-x64.sys  FileVersion 2.1.25156.ddca7bd
ARM64: winfsp-a64.sys  FileVersion 2.1.25156.ddca7bd
```

CMake requires both SYS files to be the exact same signed WinFsp release as the statically linked x64 user-mode runtime. The shipping user-mode binary is always x64. On normal AMD64 Windows it selects the x64 SYS; on Windows 11 ARM64 running the x64 EXE under emulation, `IsWow64Process2` reports the native ARM64 machine and memfs selects the ARM64 SYS. No 32-bit user-mode or driver path is supported.

memfs first attempts normal `FspFileSystemCreate`. If a compatible official driver is already available, it reuses it and does not install a parallel fallback service. Only when the driver is missing/unloadable does memfs extract the architecture-matched embedded signed SYS, register/start its private fallback filesystem-driver service, and retry `FspFileSystemCreate`.

The extraction path compares an existing file byte-for-byte before replacing it.

## Windows Service verification

The final Release `memfs.exe` was verified through the real Service Control Manager as
LocalSystem.

Successful lifecycle:

```text
sc create
sc start
STATE: RUNNING
mount R:
create/write/read/delete
sc stop
STATE: STOPPED
sc delete
```

A real file round-trip returned:

```text
READBACK=service-release-ok
```

The helper is also verified end-to-end:

```powershell
.\scripts\memfs-service.ps1 install -Exe .\build\x64-release\memfs.exe -Mount R: -Size 64M -StartType demand
.\scripts\memfs-service.ps1 start
.\scripts\memfs-service.ps1 stop
.\scripts\memfs-service.ps1 delete
```

`stop` and `delete` now handle an already-absent service cleanly.

Abnormal-termination recovery is also verified with `tests\service-recovery.ps1`. The service was started as LocalSystem on R:, its process was forcibly terminated, and SCM restarted it with a new PID:

```text
old_pid=13196
new_pid=46288
readback=service-recovery-ok
```

The remounted filesystem was correctly empty after the crash (volatile data did not survive process death), new I/O succeeded, no private WinFsp fallback was created while the official SxS driver was available, and the test cleaned up `MemfsC` afterward. Reboot recovery remains a separate clean-machine/manual validation because this test intentionally does not reboot the development machine.

Service startup failures preserve the underlying WinFsp NTSTATUS in
`SERVICE_EXIT_CODE`. For example, deliberately trying to mount an already-existing
normal directory produced:

```text
WIN32_EXIT_CODE    : 1066  (0x42a)
SERVICE_EXIT_CODE  : 3221225525  (0xc0000035)
```

rather than losing the root cause behind a generic 1066.

## Allocator design verification

Allocator policy is:

- business APIs converge by allocation size, not object type;
- small allocations use adaptive slab backing: 4K / 8K / 16K / 32K / 64K;
- allocations above the small-object threshold use area allocations;
- core/storage code does not call VirtualAlloc directly;
- VM primitives are isolated behind `memfs_vm`.

The current size-class pool contains 16 internal concurrency lanes. These are not
separate node/name/generic pools; they partition slab ownership to avoid a single hot
lock, and empty slabs are still returned immediately to the VM backend.

A three-run A/B benchmark against the previous 8-lane layout showed that at 16 threads
64-byte alloc/free throughput improved by about 114% and 4 KiB throughput by about
209%, with effectively flat single-thread performance. The cost is approximately
12 KiB of additional allocator-state metadata per filesystem.

The allocator also has an explicit uninitialized fast path for buffers that are
completely overwritten before first read. Typed node/dir/page-group allocations and
the normal zeroed APIs retain their zero-initialization contract. Reused slab and area
blocks are covered by tests to ensure zeroed callers never observe stale data.

Per-node metadata was subsequently reduced from 136 to 128 bytes without packing or removing the cached directory-name hash. This moves every `MemfsNode` from the allocator's 192-byte class into the 128-byte class, saving 64 slab bytes per live file/directory node. The former stored 64-bit `index_number` was replaced by a per-filesystem-seeded stable opaque ID derived from the node address; file IDs stay stable for the node lifetime and encryption AAD uses the same derived identity. Debug/Release tests, a 10-second soak, and real mounted plain/compression/encryption/combined integration all pass.

Plain uncompressed full pages were also redesigned so a logical 4 KiB page consumes the allocator's 4096-byte class directly instead of storing an 8-byte `MemfsPage` header beside 4096 bytes of data and rounding the 4104-byte request into the 8192-byte class. Raw pages use a tagged aligned pointer representation; compressed/encrypted pages retain the structured `MemfsPage` object. Existing raw pages overwrite in place, full-zero writes remain sparse, and new-page writes use two-phase encode/commit for rollback safety. The measured resident backing for one plain full page is now 4096 bytes rather than 8192 bytes.

To keep the 16 slab lanes from increasing allocator high-water across repeated identical workloads, a size-class pool now exposes an atomic available-shard bitmap. Allocation stays on the thread's home shard while it has capacity; before growing a new slab it probes only lanes advertised as having free objects and reuses that capacity across shards. At most one shard lock is held at a time. An 8-round fragmentation/reuse benchmark now keeps slab backing stable, final scavenging returns to baseline, allocator MT benchmarks report zero errors, and a 10-second soak reports zero committed/private drift.

## Repeatable deployment verification

`scripts\verify-deployment.ps1` separates safe development-machine checks from the destructive clean-machine fallback path.

Current x64 development-machine verification:

```powershell
.\scripts\verify-deployment.ps1 -Scenario All -Report .agent\deployment-current.json
```

Result on 2026-09-30:

- release executable/CLI gate: PASS;
- driver/static-version/no-WinFsp-DLL-import/resource CTest gate: PASS;
- no `MemfsC` service: PASS;
- no private `WinFsp+MemfsC` fallback service/file before mount: PASS;
- real mounted-drive smoke using official `WinFsp+20251221T124141Z`: PASS;
- no private fallback service/file after official-driver mount: PASS;
- private-fallback execution: SKIP by default.

The guard was also tested explicitly:

```powershell
.\scripts\verify-deployment.ps1 -Scenario PrivateFallback -ExercisePrivateFallback
```

On this machine it fails closed because an official WinFsp SxS driver is present. The script will not remove, stop, overwrite, or repoint an official WinFsp installation merely to exercise the fallback.

The remaining clean-VM validation is therefore explicit and reproducible rather than simulated. On a disposable elevated Windows VM with no WinFsp installation:

```powershell
.\scripts\verify-deployment.ps1 -Scenario PrivateFallback -ExercisePrivateFallback -Report C:\temp\memfs-private-fallback.json
```

The harness verifies private service creation, MemfsC-owned driver filename/path, a real mounted-drive I/O smoke test, and uninstall cleanup. It refuses to run this scenario if an official WinFsp driver is detected.

## Current binaries

Debug:

- size: 3,315,200 bytes
- SHA-256: `9AD497AD07FD44859D641643405477EB68306D5FC9ACE670C31DF8A6853FFD9D`

Release:

- size: 1,319,424 bytes
- SHA-256: `D9CFF0432EE5F5CB7BDC9B30F600B1D59B677E513A30C995D55731ECDE959B85`

These hashes are verification artifacts for this local build, not permanent release
identifiers.

## Release packaging

`scripts\package-release.ps1` creates the single x64 user-mode release directory `dist\memfs-x64` with:

That same package is the Windows 11 ARM64 package; there is no separate ARM64 user-mode build.

- `memfs.exe`
- `THIRD_PARTY_NOTICES.md`
- `licenses/WinFsp-License.txt`
- `licenses/libsodium.txt`
- `licenses/zstd.txt`
- `SHA256SUMS.txt`

The package intentionally includes the original third-party license texts rather
than paraphrasing them as the authoritative legal terms. The static WinFsp build
also has an explicit release gate in `THIRD_PARTY_NOTICES.md`: the WinFsp
FLOSS exception in the local source tree names platform DLL linking, while this
build statically links user-mode WinFsp code. Proprietary distribution therefore
requires an appropriate licensing decision before release.

## Remaining productization items

Before broad external distribution, separately validate:

- code-signing policy for the final EXE;
- final WinFsp distribution-license decision for static linking (for proprietary distribution, obtain appropriate commercial permission or use distribution terms compatible with the applicable WinFsp/GPLv3 obligations);
- Windows 10 and Windows 11 clean-machine installation;
- Windows 11 ARM64 validation of the single x64 `memfs.exe` under x64 emulation, including selection/install of the embedded ARM64 kernel driver;
- coexistence testing against materially older/newer official WinFsp installations, while preserving the private-service isolation rules already implemented;
- service recovery across a real OS reboot (abnormal process termination is now covered automatically);
- clean-machine embedded-driver fallback with no WinFsp installation present, using the now-checked-in `verify-deployment.ps1 -Scenario PrivateFallback -ExercisePrivateFallback` harness.
