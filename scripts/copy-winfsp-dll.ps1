<#
.SYNOPSIS
    Copies the WinFsp runtime DLL next to the isfs executable.

.DESCRIPTION
    isfs.exe imports winfsp-x64.dll at load time. WinFsp 2.x keeps the DLL in
    "C:\Program Files (x86)\WinFsp\bin" and does not add that directory to PATH,
    so the loader cannot find it (error 0xc0000135). This script copies the DLL
    next to the executable so no PATH changes are needed.

    The logic lives in a file (and not inline in tasks.json) on purpose: an inline
    "powershell -Command ..." string goes through two levels of quoting (VS Code
    task shell -> powershell.exe) and breaks on the nested single/double quotes.

.PARAMETER DestinationDir
    Directory to copy the DLL into. Defaults to the "isfs" folder of the repo.
#>
[CmdletBinding()]
param(
    [string] $DestinationDir
)

$ErrorActionPreference = 'Stop'

# The default is resolved here, not in the param() block: default values are
# evaluated in the caller's scope, where $PSScriptRoot is not set yet.
$root = Split-Path -Parent $PSScriptRoot
if (-not $DestinationDir) {
    $DestinationDir = Join-Path $root 'isfs'
}

# Possible locations of the WinFsp installation (x86 registry view first, then x64).
$installDirs = @(
    (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\WinFsp' -ErrorAction SilentlyContinue).InstallDir,
    (Get-ItemProperty 'HKLM:\SOFTWARE\WinFsp' -ErrorAction SilentlyContinue).InstallDir,
    (Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'WinFsp'),
    (Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'WinFsp')
) | Where-Object { $_ }

$src = $null
foreach ($dir in $installDirs) {
    $candidate = Join-Path $dir 'bin\winfsp-x64.dll'
    if (Test-Path -LiteralPath $candidate) {
        $src = $candidate
        break
    }
}

if (-not $src) {
    throw "winfsp-x64.dll not found in [$($installDirs -join ', ')] - install WinFsp x64 first"
}

if (-not (Test-Path -LiteralPath $DestinationDir)) {
    throw "destination directory '$DestinationDir' does not exist"
}

Copy-Item -LiteralPath $src -Destination $DestinationDir -Force
Write-Host "copied $src -> $DestinationDir"
