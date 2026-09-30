param(
    [string]$BuildDir = "",
    [string]$OutputDir = "",
    [string]$WinFspSourceRoot = "D:\src\winfsp",
    [ValidateSet("x64", "arm64")]
    [string]$Architecture = "x64"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path $PSScriptRoot -Parent
$arch = $Architecture.ToLowerInvariant()
$triplet = if ($arch -eq "arm64") { "arm64-windows-static" } else { "x64-windows-static" }

if (-not $BuildDir) {
    $BuildDir = Join-Path $repoRoot ("build\" + $arch + "-release")
}
if (-not $OutputDir) {
    $OutputDir = Join-Path $repoRoot ("dist\memfs-" + $arch)
}

$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$OutputDir = [IO.Path]::GetFullPath($OutputDir)

$exe = Join-Path $BuildDir "memfs.exe"
$notice = Join-Path $repoRoot "THIRD_PARTY_NOTICES.md"
$winfspLicense = Join-Path $WinFspSourceRoot "License.txt"
$sodiumLicense = Join-Path $BuildDir ("vcpkg_installed\" + $triplet + "\share\libsodium\copyright")
$zstdLicense = Join-Path $BuildDir ("vcpkg_installed\" + $triplet + "\share\zstd\copyright")

foreach ($required in @($exe, $notice, $winfspLicense, $sodiumLicense, $zstdLicense)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required release input not found: $required"
    }
}

if (Test-Path -LiteralPath $OutputDir) {
    Remove-Item -LiteralPath $OutputDir -Recurse -Force
}
$licenses = Join-Path $OutputDir "licenses"
New-Item -ItemType Directory -Path $licenses -Force | Out-Null

Copy-Item -LiteralPath $exe -Destination (Join-Path $OutputDir "memfs.exe")
Copy-Item -LiteralPath $notice -Destination (Join-Path $OutputDir "THIRD_PARTY_NOTICES.md")
Copy-Item -LiteralPath $winfspLicense -Destination (Join-Path $licenses "WinFsp-License.txt")
Copy-Item -LiteralPath $sodiumLicense -Destination (Join-Path $licenses "libsodium.txt")
Copy-Item -LiteralPath $zstdLicense -Destination (Join-Path $licenses "zstd.txt")

$manifestPath = Join-Path $OutputDir "SHA256SUMS.txt"
$files = Get-ChildItem -LiteralPath $OutputDir -File -Recurse |
    Where-Object { $_.FullName -ne $manifestPath } |
    Sort-Object FullName

$lines = foreach ($file in $files) {
    $relative = $file.FullName.Substring($OutputDir.Length).TrimStart('\').Replace('\','/')
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $relative"
}
[IO.File]::WriteAllLines($manifestPath, $lines, [Text.UTF8Encoding]::new($false))

Write-Output "Packaged $Architecture release: $OutputDir"
Write-Output ""
Write-Output "IMPORTANT: this build statically links WinFsp user-mode code."
Write-Output "Review THIRD_PARTY_NOTICES.md and the WinFsp license before distribution."
Get-Content $manifestPath
