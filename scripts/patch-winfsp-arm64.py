#!/usr/bin/env python3
"""Patch a driver-matched WinFsp worktree for the minimal ARM64 static runtime.

WinFsp v2.1 predates strict ClangCL ARM64 diagnostics. memfs only uses the
native WinFsp API, so legacy FUSE/Network Provider front-ends are excluded.
The source edits below are type-correct Win32 API boundary fixes and do not
change WinFsp behavior.
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path


def replace_required(path: Path, old: str, new: str, *, count: int = -1) -> None:
    text = path.read_text(encoding="utf-8-sig")
    found = text.count(old)
    if found == 0:
        raise RuntimeError(f"patch anchor not found in {path}: {old!r}")
    if count > 0 and found < count:
        raise RuntimeError(
            f"patch anchor count mismatch in {path}: expected >= {count}, got {found}"
        )
    text = text.replace(old, new, count if count > 0 else -1)
    path.write_text(text, encoding="utf-8")


def patch_sources(root: Path) -> None:
    security = root / "src" / "dll" / "security.c"
    replace_required(
        security,
        "    UINT32 TraverseAccess, ParentAccess, DesiredAccess2;",
        "    DWORD TraverseAccess;\n    UINT32 ParentAccess, DesiredAccess2;",
        count=1,
    )
    replace_required(
        security,
        "            PGrantedAccess, &AccessStatus))",
        "            (LPDWORD)PGrantedAccess, &AccessStatus))",
    )

    fsctl = root / "src" / "dll" / "fsctl.c"
    replace_required(
        fsctl,
        "static ULONG FspFsctlServiceVersionValue;",
        "static UINT32 FspFsctlServiceVersionValue;",
        count=1,
    )
    replace_required(
        fsctl,
        "    DWORD Size;\n"
        "    SC_HANDLE ScmHandle = 0;\n"
        "    SC_HANDLE SvcHandle = 0;\n"
        "    PVOID VersionInfo = 0;\n"
        "    SERVICE_DESCRIPTION ServiceDescription;",
        "    DWORD Size;\n"
        "    UINT ValueSize;\n"
        "    SC_HANDLE ScmHandle = 0;\n"
        "    SC_HANDLE SvcHandle = 0;\n"
        "    PVOID VersionInfo = 0;\n"
        "    SERVICE_DESCRIPTION ServiceDescription;",
        count=1,
    )
    replace_required(
        fsctl,
        '            VerQueryValueW(VersionInfo, L"\\\\StringFileInfo\\\\040904b0\\\\FileDescription",\n'
        "                &ServiceDescription.lpDescription, &Size))",
        '            VerQueryValueW(VersionInfo, L"\\\\StringFileInfo\\\\040904b0\\\\FileDescription",\n'
        "                (LPVOID *)&ServiceDescription.lpDescription, &ValueSize))",
        count=1,
    )

    service = root / "src" / "dll" / "service.c"
    replace_required(
        service,
        "        PWSTR *Argv;\n        DWORD Argc;\n        DWORD WaitResult;",
        "        PWSTR *Argv;\n        int Argc;\n        DWORD WaitResult;",
        count=1,
    )

    util = root / "src" / "dll" / "util.c"
    replace_required(
        util,
        "        DWORD Size;\n        VS_FIXEDFILEINFO *FixedFileInfo = 0;",
        "        DWORD Size;\n        UINT ValueSize;\n"
        "        VS_FIXEDFILEINFO *FixedFileInfo = 0;",
        count=1,
    )
    replace_required(
        util,
        "    DWORD Size;\n    VS_FIXEDFILEINFO *FixedFileInfo = 0;",
        "    DWORD Size;\n    UINT ValueSize;\n"
        "    VS_FIXEDFILEINFO *FixedFileInfo = 0;",
        count=1,
    )
    replace_required(
        util,
        'VerQueryValueW(VersionInfo, L"\\\\", &FixedFileInfo, &Size)',
        'VerQueryValueW(VersionInfo, L"\\\\", '
        "(LPVOID *)&FixedFileInfo, &ValueSize)",
    )

    posix = root / "src" / "shared" / "ku" / "posix.c"
    replace_required(
        posix,
        "    Result = LsaQueryInformationPolicy(PolicyHandle, "
        "PolicyAccountDomainInformation,\n        &AccountDomainInfo);",
        "    Result = LsaQueryInformationPolicy(PolicyHandle, "
        "PolicyAccountDomainInformation,\n        (PVOID *)&AccountDomainInfo);",
        count=1,
    )
    replace_required(
        posix,
        "    Result = LsaQueryInformationPolicy(PolicyHandle, "
        "PolicyDnsDomainInformation,\n        &PrimaryDomainInfo);",
        "    Result = LsaQueryInformationPolicy(PolicyHandle, "
        "PolicyDnsDomainInformation,\n        (PVOID *)&PrimaryDomainInfo);",
        count=1,
    )
    replace_required(
        posix,
        "            if (!GetAce(Acl, Index, &Ace))",
        "            if (!GetAce(Acl, Index, (LPVOID *)&Ace))",
        count=1,
    )


def patch_project(project: Path) -> None:
    text = project.read_text(encoding="utf-8-sig")
    original = text
    text, fuse_count = re.subn(
        r'(?m)^\s*<ClCompile Include="\.\.\\\.\.\\src\\dll\\fuse[^"]*" />\r?\n',
        "",
        text,
    )
    text, np_count = re.subn(
        r'(?m)^\s*<ClCompile Include="\.\.\\\.\.\\src\\dll\\np\.c" />\r?\n',
        "",
        text,
    )
    if fuse_count == 0 or np_count != 1:
        raise RuntimeError(
            f"unexpected project source layout: fuse={fuse_count}, np={np_count}"
        )
    if text == original:
        raise RuntimeError("project patch made no changes")
    project.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("worktree")
    parser.add_argument("project")
    args = parser.parse_args()

    root = Path(args.worktree).resolve()
    project = Path(args.project).resolve()
    patch_sources(root)
    patch_project(project)
    print(f"Patched ARM64 WinFsp static runtime: {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
