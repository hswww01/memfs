param(
    [Parameter(Mandatory = $true)]
    [string]$Exe,

    [string]$Drive = "R:",

    [string]$Size = "64M"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Exe)) {
    throw "memfs executable not found: $Exe"
}

if (Test-Path "$Drive\") {
    throw "drive is already in use: $Drive"
}

$exePath = (Resolve-Path $Exe).Path
$logDir = Split-Path $exePath
$stdout = Join-Path $logDir "integration.stdout.log"
$stderr = Join-Path $logDir "integration.stderr.log"

Remove-Item $stdout, $stderr -Force -ErrorAction SilentlyContinue

$process = Start-Process `
    -FilePath $exePath `
    -ArgumentList @("--mount", $Drive, "--size", $Size, "--label", "MEMTEST") `
    -RedirectStandardOutput $stdout `
    -RedirectStandardError $stderr `
    -PassThru

try {
    $mounted = $false
    for ($i = 0; $i -lt 50; $i++) {
        Start-Sleep -Milliseconds 100
        if (Test-Path "$Drive\") {
            $mounted = $true
            break
        }
        if ($process.HasExited) { break }
    }

    if (-not $mounted) {
        if (Test-Path $stderr) { Get-Content $stderr | Write-Host }
        throw "memfs did not mount $Drive"
    }

    New-Item -ItemType Directory -Path "$Drive\dir" | Out-Null
    [IO.File]::WriteAllText("$Drive\hello.txt", "hello memfs")
    [IO.File]::WriteAllBytes("$Drive\dir\data.bin", [byte[]](0..255))

    if ([IO.File]::ReadAllText("$Drive\hello.txt") -ne "hello memfs") {
        throw "text readback mismatch"
    }

    $bytes = [IO.File]::ReadAllBytes("$Drive\dir\data.bin")
    if ($bytes.Length -ne 256 -or $bytes[0] -ne 0 -or $bytes[255] -ne 255) {
        throw "binary readback mismatch"
    }

    Rename-Item "$Drive\hello.txt" "renamed.txt"
    Move-Item "$Drive\renamed.txt" "$Drive\dir\moved.txt"

    $names = @(Get-ChildItem "$Drive\dir" | Sort-Object Name | Select-Object -ExpandProperty Name)
    if (($names -join ",") -ne "data.bin,moved.txt") {
        throw "directory enumeration mismatch: $($names -join ',')"
    }

    Remove-Item "$Drive\dir\moved.txt"
    Remove-Item "$Drive\dir\data.bin"
    Remove-Item "$Drive\dir"

    if ((Get-ChildItem "$Drive\" | Measure-Object).Count -ne 0) {
        throw "root directory is not empty after cleanup"
    }

    Write-Host "integration PASS"
}
finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    Wait-Process -Id $process.Id -ErrorAction SilentlyContinue
    for ($i = 0; $i -lt 30; $i++) {
        if (-not (Test-Path "$Drive\")) { break }
        Start-Sleep -Milliseconds 100
    }
    if (Test-Path "$Drive\") {
        Write-Warning "mount point still exists after process exit: $Drive"
    }
}
