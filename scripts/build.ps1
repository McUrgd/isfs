<#
.SYNOPSIS
    Builds isfs.exe with MSVC.

.DESCRIPTION
    Single build entry point for the VS Code tasks and for CI. It

        - locates Visual Studio (vswhere + Enter-VsDevShell) when cl.exe is not
          already on PATH, so a Developer Command Prompt is not required;
        - copies the WinFsp import library and runtime DLL from the installed
          SDK (see NOTICE: they are build artifacts and are not stored here);
        - compiles and links the sources with the flags of the configuration.

    The compilation runs with the repository root as the working directory and
    uses relative paths, so a checkout below a path with spaces works.

.PARAMETER Configuration
    Debug    Default flags; output goes to isfs\isfs.exe.
    Strict   Adds /W4 /WX (C4100 and the vendored WinFsp headers are exempt);
             output goes to isfs\w4\isfs.exe.
    Analyze  Adds /analyze with /analyze:WX- (analyzer findings are warnings);
             output goes to isfs\analyze\isfs.exe. Slow.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Configuration Strict
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Strict', 'Analyze')]
    [string] $Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot

# Compiler flags per configuration. Everything is relative to the repository
# root: the sources, the include paths, the outputs and the import library.
$sources = @('isfs\isfs.c', 'isfs\atelier.c', 'isfs\helper.c', 'cjson\cJSON.c')
$common = @(
    '/nologo'
    '/Zi'
    '/MT'
    '/D_DEBUG'
    '/D_CONSOLE'
    '/DUNICODE'
    '/D_UNICODE'
)
$link = @('/link', 'passthrough\lib\winfsp-x64.lib', 'advapi32.lib', 'winhttp.lib')

switch ($Configuration)
{
    'Debug'
    {
        $outDir = 'isfs'
        $flags = @()
    }
    'Strict'
    {
        $outDir = 'isfs\w4'
        $flags = @('/W4', '/WX', '/wd4100', '/external:Ipassthrough', '/external:W0')
    }
    'Analyze'
    {
        $outDir = 'isfs\analyze'
        $flags = @(
            '/W4'
            '/analyze'
            '/analyze:WX-'
            '/analyze:external-'
            '/wd4100'
            '/external:Ipassthrough'
            '/external:W0'
        )
    }
}

$outPath = Join-Path $root $outDir
if (-not (Test-Path -LiteralPath $outPath))
{
    New-Item -ItemType Directory -Force -Path $outPath | Out-Null
}

# WinFsp SDK artifacts: the import library for the linker, the runtime DLL next
# to the executable (WinFsp does not add its bin directory to PATH).
& (Join-Path $PSScriptRoot 'copy-winfsp-lib.ps1') `
    -DestinationDir (Join-Path $root 'passthrough\lib')
& (Join-Path $PSScriptRoot 'copy-winfsp-dll.ps1') `
    -DestinationDir $outPath

# Locate MSVC unless cl.exe is already available (Developer Command Prompt).
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue))
{
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere))
    {
        throw "cl.exe is not on PATH and vswhere.exe was not found at '$vswhere'; install Visual Studio with the 'Desktop development with C++' workload"
    }

    $vs = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $vs)
    {
        throw 'vswhere did not find a Visual Studio with the MSVC x64 tools; install the ''Desktop development with C++'' workload'
    }
    $vs = @($vs)[0]

    $devShell = Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    if (-not (Test-Path -LiteralPath $devShell))
    {
        throw "Visual Studio at '$vs' has no DevShell module at '$devShell'"
    }

    Import-Module $devShell
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation `
        -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue))
    {
        throw "Enter-VsDevShell did not put cl.exe on PATH (Visual Studio at '$vs')"
    }

    Write-Host "using Visual Studio at $vs"
}

$clArgs = @($flags) + $common + @('/Ipassthrough', '/I.') +
    @("/Fe$outDir\isfs.exe", "/Fo$outDir\", "/Fd$outDir\") + $sources + $link

Push-Location $root
try
{
    # A stale executable must not survive a failed build: the manual checks
    # report what the binary does, and they would report the old one.
    $exe = Join-Path $outPath 'isfs.exe'
    if (Test-Path -LiteralPath $exe)
    {
        Remove-Item -LiteralPath $exe -Force
    }

    Write-Host "cl $($clArgs -join ' ')"
    & cl.exe @clArgs
    if ($LASTEXITCODE -ne 0)
    {
        throw "build failed: cl.exe exited with code $LASTEXITCODE"
    }

    if (-not (Test-Path -LiteralPath $exe))
    {
        throw "cl.exe reported success but '$exe' does not exist"
    }

    $built = Get-Item -LiteralPath $exe
    Write-Host ('built {0} ({1:N0} bytes, {2:yyyy-MM-dd HH:mm:ss})' -f
        $built.FullName, $built.Length, $built.LastWriteTime)
}
finally
{
    Pop-Location
}
