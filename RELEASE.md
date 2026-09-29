# Release verification

Verified on 2026-09-29 on Windows x64.

## Build inputs

- memfs repository: `D:\work\memfs`
- WinFsp source: `D:\src\winfsp`
- WinFsp static-library commit: `620d042e build: add static WinFsp user-mode library`
- embedded driver source:
  `C:\Program Files (x86)\WinFsp\SxS\sxs.20251221T124141Z\bin\winfsp-x64.sys`
- embedded driver SHA-256:
  `03553FFFACD362F4A9A08C00B4F236A82354A183BC6028494FB32055386E13C9`

The supplied CMake presets use the static CRT triplet and static WinFsp user-mode library:

- `VCPKG_TARGET_TRIPLET=x64-windows-static`
- `MEMFS_STATIC_WINFSP=ON`
- `WINFSP_SOURCE_ROOT=D:/src/winfsp`

## Build and test matrix

```powershell
cmake --build --preset x64-debug
ctest --preset x64-debug --output-on-failure

cmake --build --preset x64-release
ctest --preset x64-release --output-on-failure
```

Both Debug and Release pass 8/8 tests:

1. `memfs_mt_stress_test`
2. `memfs_core_test`
3. `memfs_no_crt_heap_guard`
4. `memfs_no_crt_heap_guard_good_fixture`
5. `memfs_no_crt_heap_guard_bad_fixture`
6. `memfs_driver_test`
7. `memfs_cli_help`
8. `memfs_embedded_winfsp_resources`

## Real mounted-drive integration

The Release executable passed the real WinFsp mount integration test in all four runtime modes:

- plain: PASS
- `--compress`: PASS
- `--encrypt`: PASS
- `--compress --encrypt`: PASS

Each run mounted R:, exercised normal Windows file APIs (including namespace and sparse-file behavior through the integration script), and unmounted cleanly.

## Static WinFsp verification

Static libraries:

- `D:\src\winfsp\build\VStudio\build\Debug\winfsp-static-x64.lib`
- `D:\src\winfsp\build\VStudio\build\Release\winfsp-static-x64.lib`

`dumpbin /dependents` on both Debug and Release `memfs.exe` shows no dependency on `winfsp-x64.dll`.

Release dependencies are Windows system DLLs only:

- ADVAPI32.dll
- KERNEL32.dll
- NETAPI32.dll
- USER32.dll
- VERSION.dll
- ole32.dll
- SHELL32.dll
- WLDAP32.dll
- RPCRT4.dll

The Release EXE therefore does not require an external WinFsp user-mode DLL.

## Embedded driver behavior

The configured official SxS driver is currently registered as:

```text
SERVICE_NAME: WinFsp+20251221T124141Z
TYPE: FILE_SYSTEM_DRIVER
STATE: RUNNING
Path: C:\Program Files (x86)\WinFsp\SxS\sxs.20251221T124141Z\bin\winfsp-x64.sys
```

memfs first attempts normal `FspFileSystemCreate`. When a compatible official SxS driver is available, it is reused.

Only when the driver is missing or unloadable does memfs extract the embedded signed SYS resource, register/start the fallback filesystem-driver service, and retry the WinFsp create path.

The driver fallback code compares an existing extracted driver byte-for-byte before replacing it.

## Windows Service verification

Release `memfs.exe` was verified through the real Service Control Manager using LocalSystem:

```text
sc create
sc start
sc query/queryex
filesystem mount at R:
file create/write/read/delete
sc stop
mount disappears
sc delete
```

Observed running state:

```text
TYPE: WIN32_OWN_PROCESS
STATE: RUNNING
SERVICE_START_NAME: LocalSystem
STOPPABLE
ACCEPTS_SHUTDOWN
```

On `sc stop`, the service reached `STOP_PENDING`, then `STOPPED`, and the R: mount disappeared.

Use `scripts\memfs-service.ps1` or the direct `sc.exe` commands documented in README.md.

## Current binaries

Debug:

- size: 3,130,880 bytes
- SHA-256: `A43A5E05A0ADC340FDC1B2780A1000AE0E19B82642544D511F98653867DADABA`

Release:

- size: 1,129,984 bytes
- SHA-256: `737D12FEB416D9DFDBA525DAE9FC3C24E15489EF89870DD101B62A533C002E21`

These hashes are verification artifacts for the local build above, not long-term release identifiers.

## Remaining productization items

Before broad external distribution, separately validate:

- code-signing policy for the final EXE;
- WinFsp redistribution/license notices;
- Windows 10 and Windows 11 clean-machine installation;
- x64 and ARM64 packages independently;
- upgrade/uninstall behavior when an older official WinFsp installation exists;
- service recovery after reboot and abnormal termination;
- clean-machine driver fallback with no WinFsp installation present.
