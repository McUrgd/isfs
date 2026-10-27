<#
.SYNOPSIS
    Manual check of REQ-MAP-021: -nosystem hides the system classes of CLS.

.DESCRIPTION
    Mounts isfs.exe against the mock Atelier server on a free drive letter,
    first without an option and then with -nosystem, and checks for each mount:

        - the CLS listing contains the user package User;
        - the user document CLS\User\Person.cls is reachable;
        - without -nosystem the system package %Activate is listed and the
          system document CLS\%Activate\Enum.cls is reachable;
        - with -nosystem neither the %Activate entry nor its document is
          exposed, and the user package is still listed.

    The mock server, the free drive letter and the request log are managed by
    the script.

.PARAMETER Exe
    The isfs.exe under test. Default: isfs\isfs.exe next to the repository,
    which has to be rebuilt before the run, because the script reports what
    that binary does.

.PARAMETER Drive
    Drive letter to mount on, for example X:. Default: the first free letter
    from X: down to D:.

.PARAMETER Port
    Loopback port of the mock server. Default: 18082.

.PARAMETER Work
    Directory for the logs. Default: isfs-nosystem-test in the temp directory.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\nosystem-test.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\nosystem-test.ps1 -Drive W: -Port 18090
#>
[CmdletBinding()]
param(
    [string] $Exe = '',
    [string] $Drive = '',
    [int] $Port = 18082,
    [string] $Work = (Join-Path ([IO.Path]::GetTempPath()) 'isfs-nosystem-test')
)

$ErrorActionPreference = 'Stop'

$mock = Join-Path $PSScriptRoot 'mock-atelier.ps1'

if ('' -eq $Exe)
{
    $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'isfs\isfs.exe'
}

if (-not (Test-Path $mock))
{
    throw "the mock server is not found: $mock"
}

if (-not (Test-Path $Exe))
{
    throw "isfs.exe is not found: $Exe"
}

if (-not (Test-Path (Join-Path (Split-Path -Parent $Exe) 'winfsp-x64.dll')))
{
    throw "winfsp-x64.dll is not next to $Exe"
}

if ('' -eq $Drive)
{
    foreach ($Letter in [char[]] 'XWVUTSRQPONMLKJIHGFED')
    {
        if (-not (Test-Path "${Letter}:\"))
        {
            $Drive = "${Letter}:"
            break
        }
    }
}

if ('' -eq $Drive)
{
    throw 'no free drive letter from X: down to D:'
}

if (Test-Path "${Drive}\")
{
    throw "the drive $Drive is already in use"
}

$Listener = New-Object Net.Sockets.TcpClient

try
{
    $Listener.Connect('127.0.0.1', $Port)
    throw "the port $Port is already in use"
}
catch [Net.Sockets.SocketException]
{
    # nothing listens on the port: this is what the check needs
}
finally
{
    $Listener.Dispose()
}

New-Item -ItemType Directory -Force -Path $Work | Out-Null
$MockLog = Join-Path $Work 'mock.log'
Remove-Item $MockLog -ErrorAction SilentlyContinue

$UserDocument = "${Drive}\CLS\User\Person.cls"
$SystemPackage = "${Drive}\CLS\%Activate"
$SystemDocument = "${Drive}\CLS\%Activate\Enum.cls"

# mounts the volume, reads the CLS listing and stops the process again; the
# checks are made through the volume only, so the server is not asked directly
function Invoke-Mount([string[]] $Options){
    $Arguments = @('-p', "127.0.0.1:$Port", '-ns', 'USER',
        '-user', 'tester', '-pass', 'secret', '-m', $Drive) + $Options
    $Process = Start-Process $Exe -PassThru -WindowStyle Hidden -ArgumentList $Arguments
    $Deadline = (Get-Date).AddSeconds(25)

    while (-not (Test-Path "${Drive}\") -and (Get-Date) -lt $Deadline)
    {
        Start-Sleep -Milliseconds 250
    }

    if (-not (Test-Path "${Drive}\"))
    {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        throw "the volume did not appear on $Drive (options: '$($Options -join ' ')')"
    }

    $Entries = @(Get-ChildItem -LiteralPath "${Drive}\CLS" -Force | ForEach-Object { $_.Name })
    $Result = [pscustomobject]@{
        UserListed      = $Entries -contains 'User'
        UserReachable   = Test-Path -LiteralPath $UserDocument
        SystemListed    = $Entries -contains '%Activate'
        SystemReachable = (Test-Path -LiteralPath $SystemPackage) -and
            (Test-Path -LiteralPath $SystemDocument)
    }

    Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
    $Deadline = (Get-Date).AddSeconds(30)

    while ((Test-Path "${Drive}\") -and (Get-Date) -lt $Deadline)
    {
        Start-Sleep -Milliseconds 250
    }

    if (Test-Path "${Drive}\")
    {
        throw "the drive $Drive was not released after the mount was stopped"
    }

    $Label = if (0 -eq $Options.Count) { 'default' } else { $Options -join ' ' }
    Write-Host ("mount '{0}': CLS entries = {1}" -f $Label, ($Entries -join ', '))

    return $Result
}

$MockProcess = 0
$Failures = @()

try
{
    $MockProcess = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $mock,
        '-Port', "$Port", '-LogFile', $MockLog)

    $Deadline = (Get-Date).AddSeconds(25)
    $MockUp = $false

    do
    {
        Start-Sleep -Milliseconds 300
        $Probe = New-Object Net.Sockets.TcpClient

        try
        {
            $Probe.Connect('127.0.0.1', $Port)
            $MockUp = $true
        }
        catch
        {
            $MockUp = $false
        }
        finally
        {
            $Probe.Dispose()
        }
    }
    while (-not $MockUp -and (Get-Date) -lt $Deadline)

    if (-not $MockUp)
    {
        throw 'the mock server did not start'
    }

    $Default = Invoke-Mount @()
    $NoSystem = Invoke-Mount @('-nosystem')

    if (-not $Default.UserListed)
    {
        $Failures += 'the user package is not listed by default'
    }

    if (-not $Default.UserReachable)
    {
        $Failures += "the document $UserDocument is not reachable by default"
    }

    if (-not $Default.SystemListed)
    {
        $Failures += 'the system package %Activate is not listed by default'
    }

    if (-not $Default.SystemReachable)
    {
        $Failures += "the document $SystemDocument is not reachable by default"
    }

    if (-not $NoSystem.UserListed)
    {
        $Failures += 'the user package is not listed with -nosystem'
    }

    if (-not $NoSystem.UserReachable)
    {
        $Failures += "the document $UserDocument is not reachable with -nosystem"
    }

    if ($NoSystem.SystemListed)
    {
        $Failures += 'the system package %Activate is listed with -nosystem'
    }

    if ($NoSystem.SystemReachable)
    {
        $Failures += "the document $SystemDocument is reachable with -nosystem"
    }

    Write-Host "the user package is visible by default: $($Default.UserListed)"
    Write-Host "the system package is visible by default: $($Default.SystemListed)"
    Write-Host "the user package is visible with -nosystem: $($NoSystem.UserListed)"
    Write-Host "the system package is visible with -nosystem: $($NoSystem.SystemListed)"
}
finally
{
    if (0 -ne $MockProcess)
    {
        Stop-Process -Id $MockProcess.Id -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "mock log: $MockLog"

if (0 -eq $Failures.Count)
{
    Write-Host 'REQ-MAP-021: PASS'
    exit 0
}

foreach ($Failure in $Failures)
{
    Write-Host "REQ-MAP-021: FAIL - $Failure"
}

exit 1
