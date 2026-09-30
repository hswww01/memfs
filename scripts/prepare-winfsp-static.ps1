param(
    [string]$WinFspRepo = "D:\src\winfsp",
    [string]$Worktree = "D:\src\winfsp-memfs-static",
    [ValidateSet("x64", "ARM64")]
    [string]$Platform = "x64",
    [string]$DriverPath = "",
    [string[]]$Configurations = @("Debug", "Release")
)

$ErrorActionPreference = "Stop"

function Find-WinFspDriver {
    param([string]$Arch)

    $name = if ($Arch -eq "ARM64") { "winfsp-a64.sys" } else { "winfsp-x64.sys" }
    $candidates = @()

    foreach ($base in @(
        "${env:ProgramFiles(x86)}\WinFsp\SxS",
        "${env:ProgramFiles}\WinFsp\SxS"
    )) {
        if (-not $base -or -not (Test-Path $base)) {
            continue
        }

        $candidates += Get-ChildItem -Path $base -Directory -Filter "sxs.*" -ErrorAction SilentlyContinue |
            ForEach-Object {
                $path = Join-Path $_.FullName "bin\$name"
                if (Test-Path $path) {
                    Get-Item $path
                }
            }
    }

    $selected = $candidates |
        Sort-Object -Property @{Expression = { $_.Directory.Parent.Name }; Descending = $true} |
        Select-Object -First 1

    if (-not $selected) {
        throw "Cannot find an installed signed WinFsp $name under Program Files."
    }

    return $selected.FullName
}

if (-not (Test-Path (Join-Path $WinFspRepo ".git"))) {
    throw "WinFspRepo is not a Git working tree: $WinFspRepo"
}

if (-not $DriverPath) {
    $DriverPath = Find-WinFspDriver -Arch $Platform
}
$DriverPath = (Resolve-Path $DriverPath).Path

$version = (Get-Item $DriverPath).VersionInfo.FileVersion
if ($version -notmatch '^([0-9]+)\.([0-9]+)\..*\.([0-9a-fA-F]{7,40})$') {
    throw "Cannot derive WinFsp source commit from driver FileVersion '$version'."
}

$major = [int]$Matches[1]
$minor = [int]$Matches[2]
$ref = $Matches[3]
$canonical = "$major.$minor"
$encoded = (($major -shl 16) -bor $minor)
$staticVersion = "0x{0:X8}U" -f $encoded

$objectType = & git -C $WinFspRepo cat-file -t $ref 2>$null
if ($LASTEXITCODE -ne 0 -or $objectType.Trim() -ne "commit") {
    throw "Driver commit $ref is not present in $WinFspRepo."
}

if (Test-Path $Worktree) {
    & git -C $WinFspRepo worktree remove --force $Worktree 2>$null
    if (Test-Path $Worktree) {
        Remove-Item -Recurse -Force $Worktree
    }
}
& git -C $WinFspRepo worktree prune
& git -C $WinFspRepo worktree add --detach $Worktree $ref
if ($LASTEXITCODE -ne 0) {
    throw "git worktree add failed."
}

$headerPath = Join-Path $Worktree "inc\winfsp\winfsp.h"
$header = [IO.File]::ReadAllText($headerPath)
if ($header -notmatch 'defined\(WINFSP_STATIC\)') {
    $old = "#if defined(WINFSP_DLL_INTERNAL)"
    $new = "#if defined(WINFSP_STATIC)$([Environment]::NewLine)#define FSP_API$([Environment]::NewLine)#elif defined(WINFSP_DLL_INTERNAL)"
    if (-not $header.Contains($old)) {
        throw "Cannot locate FSP_API definition in $headerPath."
    }
    $header = $header.Replace($old, $new)
    [IO.File]::WriteAllText($headerPath, $header, [Text.UTF8Encoding]::new($false))
}

$utilPath = Join-Path $Worktree "src\dll\util.c"
$util = [IO.File]::ReadAllText($utilPath)
if ($util -notmatch 'WINFSP_STATIC_VERSION') {
    $newline = if ($util.Contains([Environment]::NewLine)) { [Environment]::NewLine } else { "`n" }
    $pattern = "FSP_API NTSTATUS FspVersion(PUINT32 PVersion)$newline{"
    if (-not $util.Contains($pattern)) {
        throw "Cannot locate FspVersion in $utilPath."
    }
    $block = "$pattern$newline#if defined(WINFSP_STATIC) && defined(WINFSP_STATIC_VERSION)$newline    *PVersion = (UINT32)(WINFSP_STATIC_VERSION);$newline    return STATUS_SUCCESS;$newline#endif$newline"
    $util = $util.Replace($pattern, $block)
    [IO.File]::WriteAllText($utilPath, $util, [Text.UTF8Encoding]::new($false))
}

$vsDir = Join-Path $Worktree "build\VStudio"
$srcProject = Join-Path $vsDir "winfsp_dll.vcxproj"
$dstProject = Join-Path $vsDir "winfsp_static.vcxproj"
$project = [IO.File]::ReadAllText($srcProject)

$project = $project.Replace(
    "{4A7C0B21-9E10-4C81-92DE-1493EFCF24EB}",
    "{7D6D78C2-B808-4E31-AFE4-D2A83F6D8375}")
$project = $project.Replace(
    "<ProjectName>winfsp.dll</ProjectName>",
    "<ProjectName>winfsp.static</ProjectName>")
$project = $project.Replace(
    '<WindowsTargetPlatformVersion>$(MyTargetPlatformVersion)</WindowsTargetPlatformVersion>',
    '<WindowsTargetPlatformVersion>$(LatestTargetPlatformVersion)</WindowsTargetPlatformVersion>')
$project = $project.Replace(
    "<ConfigurationType>DynamicLibrary</ConfigurationType>",
    "<ConfigurationType>StaticLibrary</ConfigurationType>")
$project = $project.Replace(
    "<WholeProgramOptimization>true</WholeProgramOptimization>",
    "<WholeProgramOptimization>false</WholeProgramOptimization>")
$project = $project.Replace(
    '<TargetName>$(MyProductFileName)-$(MyProductFileArch)</TargetName>',
    '<TargetName>winfsp-static-$(MyProductFileArch)</TargetName>')
$project = $project.Replace(
    "<PreprocessorDefinitions>",
    "<PreprocessorDefinitions>WINFSP_STATIC;WINFSP_STATIC_VERSION=$staticVersion;")

[IO.File]::WriteAllText(
    $dstProject,
    $project,
    [Text.UTF8Encoding]::new($false))

$msbuild = (Get-Command msbuild.exe -ErrorAction SilentlyContinue).Source
if (-not $msbuild) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installation = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
        if ($installation) {
            $msbuild = Join-Path $installation "MSBuild\Current\Bin\MSBuild.exe"
        }
    }
}
if (-not $msbuild -or -not (Test-Path $msbuild)) {
    throw "MSBuild not found."
}

foreach ($configuration in $Configurations) {
    & $msbuild $dstProject /m /p:Configuration=$configuration /p:Platform=$Platform /v:minimal
    if ($LASTEXITCODE -ne 0) {
        throw "WinFsp static build failed: $configuration|$Platform"
    }
}

$arch = if ($Platform -eq "ARM64") { "a64" } else { "x64" }

Write-Host ""
Write-Host "Prepared matching WinFsp static runtime:"
Write-Host "  driver:   $DriverPath"
Write-Host "  version:  $version"
Write-Host "  commit:   $ref"
Write-Host "  source:   $Worktree"
foreach ($configuration in $Configurations) {
    $lib = Join-Path $Worktree "build\VStudio\build\$configuration\winfsp-static-$arch.lib"
    if (-not (Test-Path $lib)) {
        throw "Expected static library not found: $lib"
    }
    Write-Host "  $configuration lib: $lib"
}
Write-Host ""
Write-Host "Use with memfs:"
Write-Host "  WINFSP_SOURCE_ROOT=$Worktree"
Write-Host "  WINFSP_DRIVER_SOURCE=$DriverPath"
Write-Host "  WINFSP_EXPECTED_VERSION=$canonical"
