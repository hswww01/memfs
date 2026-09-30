param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [switch]$RefreshWinFsp,
    [switch]$SkipPackage
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ($env:ProgramFiles + " (x86)") "Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "vswhere.exe not found: $vswhere"
}

$vsInstall = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath | Select-Object -First 1)
if (-not $vsInstall) {
    throw "Visual Studio ARM64 C/C++ tools are not installed. Add Microsoft.VisualStudio.Component.VC.Tools.ARM64 first."
}

$vcvars = Join-Path $vsInstall "VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf)) {
    throw "vcvarsall.bat not found: $vcvars"
}

function Import-VcVarsArm64 {
    param([string]$VcVarsPath)
    $command = '"' + $VcVarsPath + '" x64_arm64 >nul && set'
    $lines = & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "vcvarsall x64_arm64 failed with exit code $LASTEXITCODE"
    }

    foreach ($line in $lines) {
        $index = $line.IndexOf("=")
        if ($index -le 0) { continue }
        $name = $line.Substring(0, $index)
        $value = $line.Substring($index + 1)
        [Environment]::SetEnvironmentVariable($name, $value, "Process")
    }
}

$configLower = $Configuration.ToLowerInvariant()
$preset = "arm64-$configLower"
$winfspBuildRoot = "D:\src\winfsp-memfs-static\build\VStudio\build"
$winfspDebugLib = Join-Path $winfspBuildRoot "Debug\winfsp-static-a64.lib"
$winfspReleaseLib = Join-Path $winfspBuildRoot "Release\winfsp-static-a64.lib"

if ($RefreshWinFsp -or
    -not (Test-Path -LiteralPath $winfspDebugLib -PathType Leaf) -or
    -not (Test-Path -LiteralPath $winfspReleaseLib -PathType Leaf)) {
    # prepare-winfsp-static recreates its detached worktree; always build both
    # configurations together so switching Debug/Release never invalidates the
    # other preset's static library.
    & (Join-Path $PSScriptRoot "prepare-winfsp-static.ps1") -Platform ARM64 -Configurations @("Debug", "Release")
    if ($LASTEXITCODE -ne 0) {
        throw "ARM64 WinFsp static preparation failed."
    }
}

Import-VcVarsArm64 -VcVarsPath $vcvars

Push-Location $repoRoot
try {
    & cmake --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $preset" }

    & cmake --build --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed for $preset" }

    $buildDir = Join-Path $repoRoot ("build\arm64-" + $configLower)
    $exe = Join-Path $buildDir "memfs.exe"
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        throw "ARM64 memfs.exe not found after build: $exe"
    }

    $llvmReadObj = Get-Command llvm-readobj.exe -ErrorAction SilentlyContinue
    if (-not $llvmReadObj) {
        $candidate = Join-Path $vsInstall "VC\Tools\Llvm\x64\bin\llvm-readobj.exe"
        if (Test-Path -LiteralPath $candidate) {
            $llvmReadObj = Get-Item $candidate
        }
    }
    if (-not $llvmReadObj) {
        throw "llvm-readobj.exe is required for ARM64 cross-build verification."
    }

    $headers = & $llvmReadObj.Source --file-headers $exe | Out-String
    if ($headers -notmatch "IMAGE_FILE_MACHINE_ARM64") {
        throw "memfs.exe is not an ARM64 PE image."
    }

    $imports = & $llvmReadObj.Source --coff-imports $exe | Out-String
    if ($imports -match "(?i)winfsp[^\r\n]*\.dll") {
        throw "ARM64 memfs.exe unexpectedly imports a WinFsp DLL."
    }

    $testExeNames = @(
        "memfs_core_test.exe",
        "memfs_mt_stress_test.exe",
        "memfs_soak_bench.exe",
        "memfs_driver_test.exe",
        "memfs_winfsp_version_test.exe",
        "memfs_resource_test.exe",
        "memfs_static_import_test.exe"
    )
    foreach ($name in $testExeNames) {
        $path = Join-Path $buildDir $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Expected ARM64 test/helper binary missing: $path"
        }
        $testHeaders = & $llvmReadObj.Source --file-headers $path | Out-String
        if ($testHeaders -notmatch "IMAGE_FILE_MACHINE_ARM64") {
            throw "Expected ARM64 binary is not ARM64: $path"
        }
    }

    Write-Output "ARM64 cross-build verification PASS: $exe"
    Write-Output "NOTE: ARM64 executables cannot run on this x64 host; runtime CTest must run on Windows ARM64 hardware/VM."

    if ($Configuration -eq "Release" -and -not $SkipPackage) {
        & (Join-Path $PSScriptRoot "package-release.ps1") -Architecture arm64
        if ($LASTEXITCODE -ne 0) {
            throw "ARM64 release packaging failed."
        }
    }
}
finally {
    Pop-Location
}
