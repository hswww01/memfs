param(
    [string]$BuildDir = "D:\work\memfs\build\x64-release",
    [string]$OutputDir = "D:\work\memfs\dist\memfs-x64",
    [string]$WinFspSourceRoot = "D:\src\winfsp"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $BuildDir "memfs.exe"
$notice = Join-Path $repoRoot "THIRD_PARTY_NOTICES.md"
$winfspLicense = Join-Path $WinFspSourceRoot "License.txt"
$sodiumLicense = Join-Path $BuildDir "vcpkg_installed\x64-windows-static\share\libsodium\copyright"
$zstdLicense = Join-Path $BuildDir "vcpkg_installed\x64-windows-static\share\zstd\copyright"

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

Write-Output "Packaged release: $OutputDir"
Write-Output ""
Write-Output "IMPORTANT: this build statically links WinFsp user-mode code."
Write-Output "Review THIRD_PARTY_NOTICES.md and the WinFsp license before distribution."
Get-Content $manifestPath
