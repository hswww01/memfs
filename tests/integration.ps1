param(
    [Parameter(Mandatory = $true)]
    [string]$Exe,

    [string]$Drive = "R:",

    [string]$Size = "64M",

    [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = "Stop"
$stopEventName = "Local\MemfsIntegration-" + [Guid]::NewGuid().ToString("N")
$nativeType = [System.Management.Automation.PSTypeName]'MemfsTest.NativeMethods'
if ($null -eq $nativeType.Type) {
    Add-Type -TypeDefinition @"
using System.Text;
using System.Runtime.InteropServices;
namespace MemfsTest {
    public static class NativeMethods {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern uint QueryDosDevice(
            string lpDeviceName,
            StringBuilder lpTargetPath,
            int ucchMax);
    }
}
"@
}

function Test-DosDeviceExists {
    param([string]$Device)
    $name = $Device.TrimEnd('\')
    $buffer = [Text.StringBuilder]::new(4096)
    return [MemfsTest.NativeMethods]::QueryDosDevice(
        $name, $buffer, $buffer.Capacity) -ne 0
}

if (-not (Test-Path $Exe)) {
    throw "memfs executable not found: $Exe"
}

if (Test-DosDeviceExists $Drive) {
    throw "drive is already in use: $Drive"
}

$exePath = (Resolve-Path $Exe).Path
$logDir = Split-Path $exePath
$stdout = Join-Path $logDir "integration.stdout.log"
$stderr = Join-Path $logDir "integration.stderr.log"

Remove-Item $stdout, $stderr -Force -ErrorAction SilentlyContinue

$arguments = @(
    "--mount", $Drive,
    "--size", $Size,
    "--label", "MEMTEST",
    "--stop-event", $stopEventName
) + $ExtraArgs

$process = Start-Process -FilePath $exePath -ArgumentList $arguments -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru

try {
    $mounted = $false
    for ($i = 0; $i -lt 50; $i++) {
        Start-Sleep -Milliseconds 100
        if (Test-Path "$Drive\") { $mounted = $true; break }
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

    $stream = [IO.File]::Open("$Drive\dir\sparse.bin", [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite)
    try {
        $stream.SetLength(8MB)
        $stream.Position = 4MB + 17
        $stream.WriteByte(0x7d)
        $stream.Position = 4MB
        $check = New-Object byte[] 32
        [void]$stream.Read($check, 0, $check.Length)
        if ($check[17] -ne 0x7d) { throw "sparse write/read mismatch" }
        for ($i = 0; $i -lt $check.Length; $i++) {
            if ($i -ne 17 -and $check[$i] -ne 0) {
                throw "sparse hole was not zero-filled"
            }
        }
    }
    finally {
        $stream.Dispose()
    }

    Rename-Item "$Drive\hello.txt" "renamed.txt"
    Move-Item "$Drive\renamed.txt" "$Drive\dir\moved.txt"

    $names = @(Get-ChildItem "$Drive\dir" | Sort-Object Name | Select-Object -ExpandProperty Name)
    if (($names -join ",") -ne "data.bin,moved.txt,sparse.bin") {
        throw "directory enumeration mismatch: $($names -join ',')"
    }

    Remove-Item "$Drive\dir\moved.txt"
    Remove-Item "$Drive\dir\data.bin"
    Remove-Item "$Drive\dir\sparse.bin"
    Remove-Item "$Drive\dir"

    if ((Get-ChildItem "$Drive\" | Measure-Object).Count -ne 0) {
        throw "root directory is not empty after cleanup"
    }

    Write-Host "integration PASS"
}
finally {
    $forced = $false
    if (-not $process.HasExited) {
        try {
            $stopEvent = [System.Threading.EventWaitHandle]::OpenExisting($stopEventName)
            try {
                [void]$stopEvent.Set()
            }
            finally {
                $stopEvent.Dispose()
            }
        }
        catch {
            Write-Warning "cannot open graceful stop event; falling back to forced termination: $_"
            $forced = $true
        }

        if (-not $forced -and -not $process.WaitForExit(10000)) {
            Write-Warning "memfs did not exit within 10 seconds after graceful stop request"
            $forced = $true
        }

        if ($forced -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -Force
        }
    }

    Wait-Process -Id $process.Id -ErrorAction SilentlyContinue

    for ($i = 0; $i -lt 100; $i++) {
        if (-not (Test-Path "$Drive\")) { break }
        Start-Sleep -Milliseconds 100
    }
    if (Test-Path "$Drive\") {
        throw "mount point still exists after process exit: $Drive"
    }
}
