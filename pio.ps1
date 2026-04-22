<#
.SYNOPSIS
    Convenience wrapper for PlatformIO firmware commands.
.DESCRIPTION
    Build, flash, monitor, or clean firmware for specific boards
    using short aliases instead of full PlatformIO environment names.
.EXAMPLE
    .\pio.ps1 build s3
    .\pio.ps1 flash plus2
    .\pio.ps1 monitor xiao
    .\pio.ps1 clean devkit
    .\pio.ps1 list
    .\pio.ps1 build all
#>

param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('build', 'flash', 'monitor', 'clean', 'list', 'devices', 'check')]
    [string]$Command,

    [Parameter(Position = 1)]
    [string]$Board
)

$ErrorActionPreference = 'Stop'

# Locate pio executable: PATH first, then standard PlatformIO install location.
$PioExe = (Get-Command pio -ErrorAction SilentlyContinue).Source
if (-not $PioExe) {
    $PioExe = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
    if (-not (Test-Path $PioExe)) {
        Write-Error "Cannot find pio. Install PlatformIO or add it to PATH."
    }
}

# Board alias -> PlatformIO environment name.
$Boards = [ordered]@{
    'devkit' = 'esp32-s3-devkitc-1'
    'xiao'   = 'xiao_esp32s3_sense'
    'plus2'  = 'm5stick_c_plus2'
    's3'     = 'm5stick_s3'
}

function Resolve-Board {
    param([string]$Name)
    if (-not $Name) {
        Write-Host "Available boards:" -ForegroundColor Cyan
        foreach ($k in $Boards.Keys) {
            Write-Host "  $($k.PadRight(8)) -> $($Boards[$k])"
        }
        Write-Host ""
        Write-Error "Board required. Usage: .\pio.ps1 $Command <board>"
    }
    if ($Name -eq 'all') { return $null }  # sentinel for build-all
    if ($Boards.Contains($Name)) { return $Boards[$Name] }
    # Allow passing the full env name directly.
    if ($Name -in $Boards.Values) { return $Name }
    Write-Error "Unknown board '$Name'. Use one of: $($Boards.Keys -join ', '), all"
}

function Invoke-Pio {
    param([string[]]$PioArgs)
    & $PioExe @PioArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

switch ($Command) {
    'build' {
        if ($Board -eq 'all') {
            foreach ($e in $Boards.Values) {
                Write-Host "`n=== Building $e ===" -ForegroundColor Green
                Invoke-Pio 'run', '-e', $e
            }
        }
        else {
            $e = Resolve-Board $Board
            Write-Host "Building $e ..." -ForegroundColor Green
            Invoke-Pio 'run', '-e', $e
        }
    }
    'flash' {
        $e = Resolve-Board $Board
        Write-Host "Building and flashing $e ..." -ForegroundColor Green
        Invoke-Pio 'run', '-e', $e, '-t', 'upload'
    }
    'monitor' {
        if (-not $Board) {
            Write-Host "Opening serial monitor (115200) ..." -ForegroundColor Green
            Invoke-Pio 'device', 'monitor', '-b', '115200'
        }
        else {
            $e = Resolve-Board $Board
            Write-Host "Opening serial monitor for $e (115200) ..." -ForegroundColor Green
            Invoke-Pio 'device', 'monitor', '-b', '115200', '-e', $e
        }
    }
    'clean' {
        if ($Board -eq 'all') {
            foreach ($e in $Boards.Values) {
                Write-Host "Cleaning $e ..." -ForegroundColor Yellow
                Invoke-Pio 'run', '-e', $e, '-t', 'clean'
            }
        }
        else {
            $e = Resolve-Board $Board
            Write-Host "Cleaning $e ..." -ForegroundColor Yellow
            Invoke-Pio 'run', '-e', $e, '-t', 'clean'
        }
    }
    'list' {
        Write-Host "Configured boards:" -ForegroundColor Cyan
        Write-Host ""
        foreach ($k in $Boards.Keys) {
            Write-Host "  $($k.PadRight(8))  $($Boards[$k])"
        }
        Write-Host ""
        Write-Host "Commands: build, flash, monitor, clean, list, devices, check" -ForegroundColor Cyan
        Write-Host "  build all   - build all environments"
        Write-Host "  clean all   - clean all environments"
    }
    'devices' {
        Write-Host "Detected serial devices:" -ForegroundColor Cyan
        Invoke-Pio 'device', 'list'
    }
    'check' {
        $e = Resolve-Board $Board
        Write-Host "Running static analysis on $e ..." -ForegroundColor Cyan
        Invoke-Pio 'check', '-e', $e
    }
}
