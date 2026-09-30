# Release verification

Verified on 2026-09-30 on Windows x64.

## Build inputs

- memfs repository: `D:\work\memfs`
- WinFsp Git repository: `D:\src\winfsp`
- signed embedded driver:
  `C:\Program Files (x86)\WinFsp\SxS\sxs.20251221T124141Z\bin\winfsp-x64.sys`
- driver FileVersion: `2.1.25156.ddca7bd`
- matching WinFsp source commit: `ddca7bd` (tag `v2.1`)
- embedded driver SHA-256:
  `03553FFFACD362F4A9A08C00B4F236A82354A183BC6028494FB32055386E13C9`
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
  `3DEF208459510144CCD7CD09A420F62C9E451A8485148803DB61D61AAF680714`
- Release:
  `D:\src\winfsp-memfs-static\build\VStudio\build\Release\winfsp-static-x64.lib`
  SHA-256:
  `F1CDC481AF01FE2087158A94B9050C39D19253A6CB0D78535866F5FDE54D4130`

`memfs_static_winfsp_no_dll_import` verifies that the final EXE has no WinFsp DLL
import or delay-import dependency.

The Release EXE therefore does not require `winfsp-x64.dll` at runtime.

## Embedded driver behavior

The installed compatible official SxS driver is:

```text
SERVICE_NAME: WinFsp+20251221T124141Z
TYPE: FILE_SYSTEM_DRIVER
STATE: RUNNING
FileVersion: 2.1.25156.ddca7bd
Path: C:\Program Files (x86)\WinFsp\SxS\sxs.20251221T124141Z\bin\winfsp-x64.sys
```

memfs first attempts normal `FspFileSystemCreate`. If a compatible driver is already
available, it reuses it and does not install a parallel fallback service.

Only when the driver is missing/unloadable does memfs extract the embedded signed SYS,
register/start its private fallback filesystem-driver service, and retry
`FspFileSystemCreate`.

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

The current size-class pool contains internal concurrency lanes. These are not separate
node/name/generic pools; they partition slab ownership to avoid a single hot lock.

A/B measurements on this machine showed why the lanes remain for now. For 64-byte
alloc/free at 16 threads:

- current internal lanes: about 18.8M ops/s
- literal one-lock physical pool: about 3.1M ops/s
- one-pool global SList experiment: about 4.9M ops/s

The single-pool experiments were reverted rather than accepting a 4-7x concurrency
regression. Future allocator work should target magazines/remote-free or a comparable
scheme that preserves the one-size-class abstraction without paying that lock cost.

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

- size: 3,139,584 bytes
- SHA-256: `06B1C533285448329678DAC9C0BC21CA8DA1D13485B8B41E6BB8BF45C22F71E9`

Release:

- size: 1,148,416 bytes
- SHA-256: `D86B3B36081710E58DB71BC10A36D85CECE99AD38573D4E3D2A9FA11FCCF1C0F`

These hashes are verification artifacts for this local build, not permanent release
identifiers.

## Release packaging

`scripts\package-release.ps1` creates the current x64 release directory with:

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
- full memfs ARM64 configure/build/package after the Visual Studio ARM64 C/C++ toolchain is installed (the driver-matched ARM64 WinFsp static runtime already builds);
- coexistence testing against materially older/newer official WinFsp installations, while preserving the private-service isolation rules already implemented;
- service recovery across a real OS reboot (abnormal process termination is now covered automatically);
- clean-machine embedded-driver fallback with no WinFsp installation present, using the now-checked-in `verify-deployment.ps1 -Scenario PrivateFallback -ExercisePrivateFallback` harness.
