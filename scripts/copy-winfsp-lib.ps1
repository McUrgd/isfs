<#
.SYNOPSIS
    Copies the WinFsp import library into passthrough\lib.

.DESCRIPTION
    The build links against winfsp-x64.lib, which ships with the WinFsp SDK.
    It is a WinFsp build artifact and is not stored in this repository (see
    NOTICE), so the build task fetches it from the installed SDK.

    The logic lives in a file (and not inline in tasks.json) on purpose: an inline
    "powershell -Command ..." string goes through two levels of quoting (VS Code
    task shell -> powershell.exe) and breaks on the nested single/double quotes.

.PARAMETER DestinationDir
    Directory to copy the import library into. Defaults to "passthrough\lib"
    of the repo.
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
    $DestinationDir = Join-Path $root 'passthrough\lib'
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
    $candidate = Join-Path $dir 'lib\winfsp-x64.lib'
    if (Test-Path -LiteralPath $candidate) {
        $src = $candidate
        break
    }
}

if (-not $src) {
    throw "winfsp-x64.lib not found in [$($installDirs -join ', ')] - install the WinFsp SDK (x64) first"
}

if (-not (Test-Path -LiteralPath $DestinationDir)) {
    New-Item -ItemType Directory -Force -Path $DestinationDir | Out-Null
}

Copy-Item -LiteralPath $src -Destination $DestinationDir -Force
Write-Host "copied $src -> $DestinationDir"
