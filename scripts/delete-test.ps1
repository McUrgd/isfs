<#
.SYNOPSIS
    Manual check of REQ-FS-030: the delete of a document is a delete of that
    document.

.DESCRIPTION
    Mounts isfs.exe against the mock Atelier server on a free drive letter and
    checks the delete of a document against the request log of the mock:

        - REQ-FS-030: a delete of a document issues DELETE of its Atelier name,
          removes the entry from the descriptor cache (the listing is reloaded
          and no longer holds the name) and leaves the volume without the file;
        - REQ-FS-030: a handle that was opened with delete-on-close and holds a
          dirty buffer is deleted, and its buffer is not written, so the close
          does not recreate the document;
        - REQ-FS-037: a synthetic directory cannot be marked for deletion, and
          no request is issued for the attempt.

    The mock server, the free drive letter and the work directory are managed by
    the script; the request log of the mock, the service log and the debug log of
    isfs are kept in the work directory for inspection.

.PARAMETER Exe
    The isfs.exe under test. Default: isfs\isfs.exe next to the repository,
    which has to be rebuilt before the run, because the script reports what
    that binary does.

.PARAMETER Drive
    Drive letter to mount on, for example X:. Default: the first free letter
    from Y: down to D:.

.PARAMETER Port
    Loopback port of the mock server. Default: 18083.

.PARAMETER Work
    Directory for the logs. Default: isfs-delete-test in the temp directory.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\delete-test.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\delete-test.ps1 -Drive W: -Port 18091
#>
[CmdletBinding()]
param(
    [string] $Exe = '',
    [string] $Drive = '',
    [int] $Port = 18083,
    [string] $Work = (Join-Path ([IO.Path]::GetTempPath()) 'isfs-delete-test')
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
    foreach ($Letter in [char[]] 'YXWVUTSRQPONMLKJIHGFED')
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
    throw 'no free drive letter from Y: down to D:'
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

if (-not ('DeleteTest' -as [type]))
{
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

/* the checks of the script need the two things that .NET does not offer: a
 * console process group for isfs.exe and a delete disposition on a directory */
public static class DeleteTest
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct STARTUPINFO
    {
        public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PROCESS_INFORMATION
    {
        public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool CreateProcess(string lpApplicationName, string lpCommandLine,
        IntPtr lpProcessAttributes, IntPtr lpThreadAttributes, bool bInheritHandles,
        uint dwCreationFlags, IntPtr lpEnvironment, string lpCurrentDirectory,
        ref STARTUPINFO si, out PROCESS_INFORMATION pi);

    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetFileInformationByHandle(SafeFileHandle handle, int infoClass,
        IntPtr info, int size);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share,
        IntPtr security, uint disposition, uint flags, IntPtr template);

    /* marks a directory for deletion through its handle, the way a caller asks
     * for the delete of an empty directory; the file system must refuse it,
     * because the directories of the volume are synthetic. The Win32 error is
     * returned, 0 means that the request was accepted. */
    public static int MarkDirectoryForDeletion(string path)
    {
        SafeFileHandle handle = CreateFile(path,
            0x00010000, /* DELETE */
            0x1 | 0x2 | 0x4, /* share READ | WRITE | DELETE */
            IntPtr.Zero, 3 /* OPEN_EXISTING */, 0x02000000 /* BACKUP_SEMANTICS */, IntPtr.Zero);

        if (handle.IsInvalid)
            return Marshal.GetLastWin32Error();

        try
        {
            IntPtr buffer = Marshal.AllocHGlobal(1);
            try
            {
                Marshal.WriteByte(buffer, 0, 1);
                if (!SetFileInformationByHandle(handle, 4 /* FileDispositionInfo */, buffer, 1))
                    return Marshal.GetLastWin32Error();
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }
        finally
        {
            handle.Dispose();
        }

        return 0;
    }
}
'@
}

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

    $StartupInfo = New-Object DeleteTest+STARTUPINFO
    $StartupInfo.cb = [Runtime.InteropServices.Marshal]::SizeOf($StartupInfo)
    $ProcessInfo = New-Object DeleteTest+PROCESS_INFORMATION
    $CommandLine = '"{0}" -p 127.0.0.1:{1} -ns USER -user tester -pass secret -m {2} -d -1 -D "{3}"' -f
        $Exe, $Port, $Drive, $DebugLog
    $CreateNewProcessGroup = 0x00000200

    $IsfsLogStream = New-Object IO.FileStream($IsfsLog, 'Create', 'Write', 'ReadWrite')

    try
    {
        $StartupInfo.dwFlags = 0x00000100
        $StartupInfo.hStdError = $IsfsLogStream.SafeFileHandle.DangerousGetHandle()

        $Started = [DeleteTest]::CreateProcess($Exe, $CommandLine,
            [IntPtr]::Zero, [IntPtr]::Zero, $true, $CreateNewProcessGroup,
            [IntPtr]::Zero, (Split-Path -Parent $Exe), [ref] $StartupInfo, [ref] $ProcessInfo)
    }
    finally
    {
        $IsfsLogStream.Dispose()
    }

    if (-not $Started)
    {
        throw "cannot start $Exe (Win32 error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
    }

    [DeleteTest]::CloseHandle($ProcessInfo.hThread) | Out-Null
    [DeleteTest]::CloseHandle($ProcessInfo.hProcess) | Out-Null
    $IsfsProcessId = $ProcessInfo.dwProcessId

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

    # fill the descriptor cache of the category, so the delete has a cache entry
    # to remove
    $null = Get-ChildItem "${Drive}\CLS"
    Start-Sleep -Milliseconds 300

    # REQ-FS-030: a clean document is deleted with DELETE and disappears
    $Source = "${Drive}\CLS\User\Person.cls"
    $Mark = Set-Mark
    [IO.File]::Delete($Source)
    Start-Sleep -Milliseconds 400
    $Listing = (Get-ChildItem "${Drive}\CLS" | ForEach-Object { $_.Name }) -join ','
    Start-Sleep -Milliseconds 300
    $Lines = Get-NewLines $Mark

    Check 'the delete issues DELETE of the document' (
        $Lines -match 'DELETE /api/atelier/v1/USER/doc/User\.Person\.cls -> 200')
    Check 'the document is gone from the volume' (-not (Test-Path $Source))
    Check 'the listing is reloaded after the delete' (
        $Lines -match 'GET /api/atelier/v1/USER/docnames/CLS')
    Check 'the deleted name is not listed' ($Listing -notmatch 'Person')

    # REQ-FS-030: a handle with a dirty buffer that is marked delete-on-close
    $Source = "${Drive}\CLS\User\Admin.cls"
    $Mark = Set-Mark
    $Stream = New-Object IO.FileStream($Source,
        [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None,
        4096, [IO.FileOptions]::DeleteOnClose)

    try
    {
        $Bytes = [Text.Encoding]::UTF8.GetBytes("// delete on close`r`n")
        $Stream.Write($Bytes, 0, $Bytes.Length)
        $Stream.Flush($false)
    }
    finally
    {
        $Stream.Dispose()
    }

    Start-Sleep -Milliseconds 500
    $Lines = Get-NewLines $Mark

    Check 'the delete-on-close issues DELETE' (
        $Lines -match 'DELETE /api/atelier/v1/USER/doc/User\.Admin\.cls -> 200')
    Check 'the dirty buffer of the deleted document is not written' (0 -eq (Count-Lines $Lines 'PUT '))
    Check 'the delete-on-close document is gone' (-not (Test-Path $Source))
    Check 'the delete-on-close document is gone from the server' (
        (Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$Port/api/atelier/v1/USER/docnames/CLS").Content -notmatch 'User\.Admin\.cls')

    # REQ-FS-037: a synthetic directory cannot be marked for deletion
    $Mark = Set-Mark
    $DeleteError = [DeleteTest]::MarkDirectoryForDeletion("${Drive}\CLS")
    Start-Sleep -Milliseconds 300
    $Lines = Get-NewLines $Mark

    Check 'the delete of a directory is refused' (0 -ne $DeleteError) "last error: $DeleteError"
    Check 'the refusal issues no DELETE' (0 -eq (Count-Lines $Lines 'DELETE '))
    Check 'the directory is still there' (Test-Path "${Drive}\CLS")
}
finally
{
    Stop-Process -Id $IsfsProcessId -Force -ErrorAction SilentlyContinue

    if (0 -ne $MockProcess)
    {
        Stop-Process -Id $MockProcess.Id -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "mock log: $MockLog"
Write-Host "isfs service log: $IsfsLog"
Write-Host "isfs debug log: $DebugLog"

if (0 -eq $Failures.Count)
{
    Write-Host 'REQ-FS-030, REQ-FS-037: PASS'
    exit 0
}

foreach ($Failure in $Failures)
{
    Write-Host "REQ-FS-030, REQ-FS-037: FAIL - $Failure"
}

exit 1
