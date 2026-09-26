# memfs

A small in-memory Windows file system written in C on top of WinFsp.

## Current features

- Pure C17 implementation.
- WinFsp native file-system API, not FUSE.
- Case-insensitive lookup with case-preserved names.
- Directories with hash lookup and deterministic sorted enumeration.
- File create/open/read/write/truncate/allocation-size handling.
- Rename and cross-directory move.
- Delete-on-cleanup semantics with open-node lifetime tracking.
- Windows file attributes and timestamps.
- Persistent ACL support in memory through WinFsp security-descriptor helpers.
- Configurable memory-disk capacity and volume label.
- 512-byte allocation unit to reduce waste for small files.
- Coarse WinFsp operation guard for a simple, correct first concurrency model.

All file-system data is volatile. Unmounting or terminating the process loses all files.

## Prerequisites

- Windows 10/11 x64.
- WinFsp installed. The project searches:
  - `C:\Program Files (x86)\WinFsp`
  - `C:\Program Files\WinFsp`
- Visual Studio C/C++ build tools.
- LLVM/clang-cl.
- CMake + Ninja.
- vcpkg at `D:\vcpkg` for the supplied presets.

WinFsp itself is consumed from its installed SDK rather than vcpkg, because the local vcpkg registry does not provide a WinFsp port. The CMake presets still use the vcpkg toolchain for project dependencies.

## Build

Debug:

```powershell
cd D:\work\memfs
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug
```

Release:

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset x64-release
```

The WinFsp runtime DLL is copied next to `memfs.exe` automatically.

## Run

Mount a 512 MiB memory disk at `R:`:

```powershell
.\build\x64-release\memfs.exe --mount R:
```

Custom capacity and label:

```powershell
.\build\x64-release\memfs.exe --mount R: --size 1G --label FASTRAM
```

Options:

```text
--mount <path>     Drive letter or directory mount point
--size <bytes>     Capacity; K/M/G suffix supported
--label <name>     Volume label
--threads <n>      WinFsp dispatcher threads; 0 = automatic
--debug            Enable WinFsp debug logging
--help
```

Stop with Ctrl+C.

## Integration test

The integration script starts the file system on an unused drive letter, exercises Windows file APIs, then terminates the process and checks that the mount disappears:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\integration.ps1 -Exe .\build\x64-debug\memfs.exe -Drive R:
```

## Internal design

### Node tree

Each file or directory is a `MemfsNode`. Directories maintain:

- a hash table for fast case-insensitive child lookup;
- a sorted sibling list for stable directory enumeration.

Nodes store only their component name and parent pointer. Full paths are resolved by walking components, so renaming a directory does not require rewriting every descendant path.

### File data

File contents live in one dynamically sized memory buffer per file. Allocation size is rounded to 512 bytes and counted against the configured volume capacity. Extending a file zero-fills the new range.

### Handle lifetime and deletion

WinFsp uses the node pointer as `FileContext`. Open/create increments the node's open count. Delete-on-cleanup unlinks the node from the directory tree, but the node remains alive until the final close.

### Concurrency

The first version uses WinFsp's coarse operation guard, which serializes file-system callbacks. This keeps rename/delete/open lifetime semantics straightforward. The internal data structures can later move to fine-grained directory/node locking without changing the public file-system model.

## Source layout

```text
src/
  main.c            command-line mount process
  memfs_core.h      in-memory node/tree API
  memfs_core.c      tree, file data, capacity, rename/delete logic
  memfs_winfsp.h    WinFsp adapter API
  memfs_winfsp.c    WinFsp callbacks

tests/
  memfs_core_test.c core unit tests
  integration.ps1   real mounted-drive integration test
```

## Planned next steps

- Fine-grained locking for parallel I/O.
- Optional chunked file storage to avoid realloc/copy for large files.
- Named streams, reparse points and extended attributes.
- Persistence/snapshot support.
- Richer stress tests for rename/delete/open races.
