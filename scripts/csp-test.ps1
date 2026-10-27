<#
.SYNOPSIS
    Manual check of REQ-MAP-020, REQ-MAP-004 and REQ-MAP-005 for CSP: the
    application path of a CSP document becomes a directory level.

.DESCRIPTION
    Mounts isfs.exe against the mock Atelier server on a free drive letter and
    checks the CSP category against the volume and the request log of the mock:

        - REQ-MAP-020: a CSP document whose name is an application path is
          exposed as a package level tree, e.g. the document
          "/csp/gkh/Utils/CreateContracts.csp" is reachable at
          CSP\csp\gkh\Utils\CreateContracts.csp, and its content is served;
        - REQ-MAP-004: the slash is the level separator of CSP only, so dots
          stay inside a segment: a document without a slash such as "index.csp"
          stays at the root of CSP;
        - REQ-MAP-005: a new document CSP\csp\gkh\Utils\NewPage.csp is created
          with the name csp/gkh/Utils/NewPage.csp: one slash between the
          segments, no leading slash and no empty segment in the URL;
        - REQ-MAP-020: a rename of a CSP document writes the new name and
          deletes the old one with the same spelling, and a delete issues
          DELETE of the name of the document;
        - no document of CSP is reported as "not a legal file name".

    The mock server, the free drive letter and the work directory are managed by
    the script; the request log of the mock, the service log and the debug log of
    isfs are kept in the work directory for inspection.

.PARAMETER Exe
    The isfs.exe under test. Default: isfs\isfs.exe next to the repository,
    which has to be rebuilt before the run, because the script reports what
    that binary does.

.PARAMETER Drive
    Drive letter to mount on, for example X:. Default: the first free letter
    from Z: down to D:.

.PARAMETER Port
    Loopback port of the mock server. Default: 18084.

.PARAMETER Work
    Directory for the logs. Default: isfs-csp-test in the temp directory.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\csp-test.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\csp-test.ps1 -Drive W: -Port 18092
#>
[CmdletBinding()]
param(
    [string] $Exe = '',
    [string] $Drive = '',
    [int] $Port = 18084,
    [string] $Work = (Join-Path ([IO.Path]::GetTempPath()) 'isfs-csp-test')
)

$ErrorActionPreference = 'Stop'

$Mock = Join-Path $PSScriptRoot 'mock-atelier.ps1'

if ('' -eq $Exe)
{
    $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'isfs\isfs.exe'
}

if (-not (Test-Path $Mock))
{
    throw "the mock server is not found: $Mock"
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
    foreach ($Letter in [char[]] 'ZYXWVUTSRQPONMLKJIHGFED')
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
    throw 'no free drive letter from Z: down to D:'
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
$DebugLog = Join-Path $Work 'isfs-debug.log'
$IsfsLog = Join-Path $Work 'isfs.log'
Remove-Item $MockLog, $DebugLog, $IsfsLog -ErrorAction SilentlyContinue

$Failures = New-Object System.Collections.Generic.List[string]

function Check([string] $Name, [bool] $Ok, [string] $Detail = '')
{
    if ($Ok) { Write-Host "PASS  $Name" }
    else { Write-Host "FAIL  $Name $Detail"; $Failures.Add($Name) }
}

function Get-LogLines { @(Get-Content $MockLog -ErrorAction SilentlyContinue) }
function Set-Mark { (Get-LogLines).Count }
function Get-NewLines([int] $Mark)
{
    $All = Get-LogLines
    if ($Mark -ge $All.Count) { return '' }
    return ($All[$Mark..($All.Count - 1)] -join ' | ')
}
function Get-Document([string] $Name)
{
    (Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$Port/api/atelier/v1/USER/doc/$Name").Content
}
function Count-Lines([string] $Lines, [string] $Pattern)
{
    @($Lines -split '\|' | Where-Object { $_ -match $Pattern }).Count
}

$MockProcess = 0
$IsfsProcessId = 0

try
{
    $MockProcess = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $Mock,
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

    $Arguments = @('-p', "127.0.0.1:$Port", '-ns', 'USER',
        '-user', 'tester', '-pass', 'secret', '-m', $Drive, '-d', '-1', '-D', "`"$DebugLog`"")
    $IsfsProcess = Start-Process $Exe -PassThru -WindowStyle Hidden -ArgumentList $Arguments
    $IsfsProcessId = $IsfsProcess.Id

    Write-Host "isfs: $Exe (pid $IsfsProcessId), drive $Drive, mock on port $Port"

    $Deadline = (Get-Date).AddSeconds(25)

    while (-not (Test-Path "${Drive}\") -and (Get-Date) -lt $Deadline)
    {
        Start-Sleep -Milliseconds 250
    }

    if (-not (Test-Path "${Drive}\"))
    {
        throw "the volume did not appear on $Drive"
    }

    Write-Host 'the volume is mounted'

    # REQ-MAP-020: the application path of a CSP document is a package level
    $Root = "${Drive}\CSP"
    $Entries = @(Get-ChildItem -LiteralPath $Root -Force -ErrorAction SilentlyContinue |
        ForEach-Object { $_.Name })

    Write-Host "CSP entries = $($Entries -join ', ')"

    Check 'the csp package is listed' ($Entries -contains 'csp')
    Check 'the root-level document is listed' ($Entries -contains 'index.csp')
    Check 'the csp package is a directory' (
        (Get-Item -LiteralPath (Join-Path $Root 'csp') -Force).PSIsContainer)

    $Application = "${Drive}\CSP\csp\gkh\Utils\CreateContracts.csp"
    Check 'the document of an application path is reachable' (Test-Path -LiteralPath $Application)
    Check 'the levels of the application path are directories' (
        (Test-Path -LiteralPath "${Drive}\CSP\csp\gkh\Utils" -PathType Container))
    Check 'the content of the document is served' (
        (Get-Content -LiteralPath $Application -Raw) -match 'CreateContracts')

    # REQ-MAP-004: dots stay inside a segment of a slash separated name
    Check 'the document name of the last segment keeps its dots' (
        (Get-ChildItem -LiteralPath "${Drive}\CSP\csp\gkh\Utils" -Force |
            ForEach-Object { $_.Name }) -contains 'CreateContracts.csp')

    # REQ-MAP-005: a created document is addressed as an application path
    $New = "${Drive}\CSP\csp\gkh\Utils\NewPage.csp"
    $Mark = Set-Mark
    [IO.File]::WriteAllText($New, "<html>`r`n<body>new page</body>`r`n</html>`r`n")
    Start-Sleep -Milliseconds 400
    $Lines = Get-NewLines $Mark

    Check 'the PUT carries the name of the document' (
        $Lines -match 'PUT /api/atelier/v1/USER/doc/csp/gkh/Utils/NewPage\.csp -> 201')
    Check 'the PUT has no empty segment' (0 -eq (Count-Lines $Lines 'doc//'))
    Check 'the created document exists on the server' (
        (Get-Document 'csp/gkh/Utils/NewPage.csp') -match 'new page')
    Check 'the created document is listed under its application path' (
        (Get-ChildItem -LiteralPath "${Drive}\CSP\csp\gkh\Utils" -Force |
            ForEach-Object { $_.Name }) -contains 'NewPage.csp')

    $Renamed = "${Drive}\CSP\csp\gkh\Utils\RenamedPage.csp"
    $Mark = Set-Mark
    [IO.File]::Move($New, $Renamed)
    Start-Sleep -Milliseconds 400
    $Lines = Get-NewLines $Mark

    Check 'the rename writes the new application path' (
        $Lines -match 'PUT /api/atelier/v1/USER/doc/csp/gkh/Utils/RenamedPage\.csp -> 201')
    Check 'the rename deletes the old application path' (
        $Lines -match 'DELETE /api/atelier/v1/USER/doc/csp/gkh/Utils/NewPage\.csp -> 200')
    Check 'the renamed document is on the volume' (Test-Path -LiteralPath $Renamed)
    Check 'the renamed document holds the content' (
        (Get-Document 'csp/gkh/Utils/RenamedPage.csp') -match 'new page')

    $Mark = Set-Mark
    [IO.File]::Delete($Renamed)
    Start-Sleep -Milliseconds 400
    $Lines = Get-NewLines $Mark

    Check 'the delete issues DELETE of the application path' (
        $Lines -match 'DELETE /api/atelier/v1/USER/doc/csp/gkh/Utils/RenamedPage\.csp -> 200')
    Check 'the deleted document is gone' (-not (Test-Path -LiteralPath $Renamed))
}
finally
{
    if (0 -ne $IsfsProcessId)
    {
        Stop-Process -Id $IsfsProcessId -Force -ErrorAction SilentlyContinue
    }

    if (0 -ne $MockProcess)
    {
        Stop-Process -Id $MockProcess.Id -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "mock log: $MockLog"
Write-Host "isfs service log: $IsfsLog"
Write-Host "isfs debug log: $DebugLog"

# the debug log is buffered while isfs runs, so the warning is read after the
# process has stopped
$DebugText = '' + (Get-Content $DebugLog -Raw -ErrorAction SilentlyContinue)
Check 'no document of CSP is reported as an illegal name' (
    -not ($DebugText -match 'CSP: the document .* is not a legal file name'))

if (0 -eq $Failures.Count)
{
    Write-Host 'REQ-MAP-004, REQ-MAP-005, REQ-MAP-020: PASS'
    exit 0
}

foreach ($Failure in $Failures)
{
    Write-Host "REQ-MAP-004, REQ-MAP-005, REQ-MAP-020: FAIL - $Failure"
}

exit 1
