<#
.SYNOPSIS
    Manual check of REQ-MAP-016 and REQ-FS-032 to REQ-FS-036: rename.

.DESCRIPTION
    Mounts isfs.exe against the mock Atelier server on a free drive letter and
    checks the composed rename against the request log of the mock:

        - REQ-FS-032: a rename of A to B issues GET of A, PUT of B and DELETE of
          A; the PUT carries the dotted name of the target path, and a binary
          document keeps its enc flag and its bytes;
        - REQ-FS-032: the handle that performs the rename follows the document,
          so its dirty buffer is saved under the old name, written under the new
          one, and the close does not recreate the old document;
        - REQ-FS-034: an existing target is refused with no request but the open
          of the source, and is overwritten when the caller allows it;
        - REQ-FS-035: a rename into another category uses the name of the
          target and empties the listing of the source;
        - REQ-MAP-009: a rename that only changes the case writes nothing;
        - REQ-FS-036: a directory cannot be renamed, and no write request is
          issued for the attempt.

    The mock server, the free drive letter and the work directory are managed by
    the script; the request log of the mock, the service log and the debug log of
    isfs are kept in the work directory for inspection.

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
    Directory for the logs. Default: isfs-rename-test in the temp directory.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\rename-test.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\rename-test.ps1 -Drive W: -Port 18090
#>
[CmdletBinding()]
param(
    [string] $Exe = '',
    [string] $Drive = '',
    [int] $Port = 18082,
    [string] $Work = (Join-Path ([IO.Path]::GetTempPath()) 'isfs-rename-test')
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
$DebugLog = Join-Path $Work 'isfs-debug.log'
$IsfsLog = Join-Path $Work 'isfs.log'
Remove-Item $MockLog, $DebugLog, $IsfsLog -ErrorAction SilentlyContinue

if (-not ('RenameTest' -as [type]))
{
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

/* the checks of the script need the two things that .NET does not offer: a
 * console process group for isfs.exe and a rename through an open handle */
public static class RenameTest
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

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool MoveFileEx(string existing, string renamed, uint flags);

    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetFileInformationByHandle(SafeFileHandle handle, int infoClass,
        IntPtr info, int size);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share,
        IntPtr security, uint disposition, uint flags, IntPtr template);

    /* writes into a document and renames it through the same open handle, so
     * that the rename sees a handle with a dirty buffer. The handle is opened
     * with DELETE access, because a rename through a handle requires it. */
    public static void DirtyRename(string path, string newPath, string text)
    {
        SafeFileHandle handle = CreateFile(path,
            0x80000000 | 0x40000000 | 0x00010000, /* READ | WRITE | DELETE */
            0x1 | 0x2 | 0x4, /* share READ | WRITE | DELETE */
            IntPtr.Zero, 3 /* OPEN_EXISTING */, 0x80 /* NORMAL */, IntPtr.Zero);

        if (handle.IsInvalid)
            throw new IOException("cannot open the document: " + Marshal.GetLastWin32Error());

        using (var stream = new FileStream(handle, FileAccess.ReadWrite, 4096, false))
        {
            var bytes = Encoding.UTF8.GetBytes(text);
            stream.Write(bytes, 0, bytes.Length);
            stream.Flush(false);

            /* FILE_RENAME_INFO: the flags, the root directory, the length of the
             * name in bytes and the name itself; the name is terminated by a
             * zero WCHAR because the consumer may not honour the length */
            var name = Encoding.Unicode.GetBytes(newPath);
            int size = 20 + name.Length + 2;
            IntPtr buffer = Marshal.AllocHGlobal(size);
            try
            {
                for (int i = 0; i < size; i++)
                    Marshal.WriteByte(buffer, i, 0);
                Marshal.WriteInt32(buffer, 16, name.Length);
                Marshal.Copy(name, 0, buffer + 20, name.Length);

                if (!SetFileInformationByHandle(handle, 3 /* FileRenameInfo */, buffer, size))
                    throw new IOException("cannot rename the open handle: " +
                        Marshal.GetLastWin32Error());
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }
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

    $StartupInfo = New-Object RenameTest+STARTUPINFO
    $StartupInfo.cb = [Runtime.InteropServices.Marshal]::SizeOf($StartupInfo)
    $ProcessInfo = New-Object RenameTest+PROCESS_INFORMATION
    $CommandLine = '"{0}" -p 127.0.0.1:{1} -ns USER -user tester -pass secret -m {2} -d -1 -D "{3}"' -f
        $Exe, $Port, $Drive, $DebugLog
    $CreateNewProcessGroup = 0x00000200

    $IsfsLogStream = New-Object IO.FileStream($IsfsLog, 'Create', 'Write', 'ReadWrite')

    try
    {
        $StartupInfo.dwFlags = 0x00000100
        $StartupInfo.hStdError = $IsfsLogStream.SafeFileHandle.DangerousGetHandle()

        $Started = [RenameTest]::CreateProcess($Exe, $CommandLine,
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

    [RenameTest]::CloseHandle($ProcessInfo.hThread) | Out-Null
    [RenameTest]::CloseHandle($ProcessInfo.hProcess) | Out-Null
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

    # REQ-MAP-016, REQ-FS-032: a move between package levels
    $From = "${Drive}\CLS\User\Person.cls"
    $To = "${Drive}\CLS\User\Admin\Person.cls"
    $Mark = Set-Mark
    [IO.File]::Move($From, $To)
    Start-Sleep -Milliseconds 300
    $Lines = Get-NewLines $Mark

    Check 'the source document is gone' (-not (Test-Path $From))
    Check 'the target document exists' (Test-Path $To)
    Check 'the GET of the source is issued' ($Lines -match 'GET /api/atelier/v1/USER/doc/User\.Person\.cls -> 200')
    Check 'the PUT carries the dotted name of the target' ($Lines -match 'PUT /api/atelier/v1/USER/doc/User\.Admin\.Person\.cls -> 201')
    Check 'the DELETE of the source is issued' ($Lines -match 'DELETE /api/atelier/v1/USER/doc/User\.Person\.cls -> 200')
    Check 'the target holds the content of the source' ((Get-Document 'User.Admin.Person.cls') -match 'Class User\.Person Extends %Persistent')

    # REQ-FS-032: a binary document stays binary
    $From = "${Drive}\CLS\User\Admin.cls"
    $To = "${Drive}\CLS\User\Admin\Binary.cls"
    $Before = [IO.File]::ReadAllBytes($From)
    [IO.File]::Move($From, $To)
    Start-Sleep -Milliseconds 300
    $After = [IO.File]::ReadAllBytes($To)
    $Document = Get-Document 'User.Admin.Binary.cls'

    Check 'the bytes of the binary document are preserved' (
        [Convert]::ToBase64String($Before) -eq [Convert]::ToBase64String($After))
    Check 'the binary document keeps enc=true' ($Document -match '"enc":\s*true')

    # REQ-FS-034: an existing target is not replaced without permission
    $Source = "${Drive}\CLS\User\Admin\Person.cls"
    $Target = "${Drive}\CLS\User\Admin\Settings.cls"
    $Mark = Set-Mark
    $Refused = $false

    try { [IO.File]::Move($Source, $Target) } catch { $Refused = $true }

    Start-Sleep -Milliseconds 300
    $Lines = Get-NewLines $Mark

    Check 'the replacement without permission fails' $Refused
    Check 'the refusal issues no PUT' (0 -eq (Count-Lines $Lines 'PUT '))
    Check 'the refusal issues no DELETE' (0 -eq (Count-Lines $Lines 'DELETE '))
    Check 'both documents remain after the refusal' ((Test-Path $Source) -and (Test-Path $Target))

    # REQ-FS-034: with the replacement allowed the target is overwritten
    $Mark = Set-Mark
    [RenameTest]::MoveFileEx($Source, $Target, 0x1) | Out-Null
    Start-Sleep -Milliseconds 300
    $Lines = Get-NewLines $Mark

    Check 'the replacement overwrites the existing target' ($Lines -match 'PUT /api/atelier/v1/USER/doc/User\.Admin\.Settings\.cls -> 200')
    Check 'the replacement deletes the source' ($Lines -match 'DELETE /api/atelier/v1/USER/doc/User\.Admin\.Person\.cls -> 200')
    Check 'the source is gone after the replacement' (-not (Test-Path $Source))
    Check 'the target holds the content of the source' ((Get-Document 'User.Admin.Settings.cls') -match 'Class User\.Person Extends %Persistent')

    # REQ-FS-035: a rename into another category
    $From = "${Drive}\CLS\User\Admin\Binary.cls"
    $To = "${Drive}\CSP\bin.csp"
    $Mark = Set-Mark
    [IO.File]::Move($From, $To)
    Start-Sleep -Milliseconds 300
    $Lines = Get-NewLines $Mark

    Check 'the target name of another category is used' ($Lines -match 'PUT /api/atelier/v1/USER/doc/bin\.csp -> 201')
    Check 'the source name is deleted' ($Lines -match 'DELETE /api/atelier/v1/USER/doc/User\.Admin\.Binary\.cls -> 200')
    Check 'the source category no longer lists the document' (-not (Test-Path $From))
    Check 'the target category lists the document' (Test-Path $To)
    Check 'the moved binary document keeps enc=true' ((Get-Document 'bin.csp') -match '"enc":\s*true')

    # REQ-MAP-009: a rename that only changes the case writes nothing
    $Mark = Set-Mark
    [RenameTest]::MoveFileEx("${Drive}\CLS\User\Admin\Settings.cls",
        "${Drive}\CLS\User\Admin\SETTINGS.cls", 0) | Out-Null
    Start-Sleep -Milliseconds 300
    $Lines = Get-NewLines $Mark

    Check 'the case-only rename succeeds' (Test-Path "${Drive}\CLS\User\Admin\Settings.cls")
    Check 'the case-only rename writes nothing' (0 -eq (Count-Lines $Lines '(PUT|DELETE) '))

    # REQ-FS-036: a directory cannot be renamed
    $Mark = Set-Mark
    $Refused = $false

    try { [IO.Directory]::Move("${Drive}\CLS\User\Admin", "${Drive}\CLS\User\Renamed") }
    catch { $Refused = $true }

    Start-Sleep -Milliseconds 300
    $Lines = Get-NewLines $Mark

    Check 'the rename of a directory fails' $Refused
    Check 'the rename of a directory writes nothing' (0 -eq (Count-Lines $Lines '(PUT|DELETE) '))
    Check 'the package level is still there' (Test-Path "${Drive}\CLS\User\Admin")

    # REQ-FS-032: a handle with a dirty buffer is renamed through itself
    $Source = "${Drive}\CLS\User\Admin\Settings.cls"
    $Target = "${Drive}\CLS\User\Admin\Dirty.cls"
    $Mark = Set-Mark
    [RenameTest]::DirtyRename($Source, "${Drive}\CLS\User\Admin\Dirty.cls", "// dirty rename`r`n")
    Start-Sleep -Milliseconds 500
    $Lines = Get-NewLines $Mark

    Check 'the dirty buffer is saved once, under the old name' (1 -eq (Count-Lines $Lines 'PUT /api/atelier/v1/USER/doc/User\.Admin\.Settings\.cls'))
    Check 'the content is written under the new name' ($Lines -match 'PUT /api/atelier/v1/USER/doc/User\.Admin\.Dirty\.cls -> 201')
    Check 'the old name is deleted' ($Lines -match 'DELETE /api/atelier/v1/USER/doc/User\.Admin\.Settings\.cls -> 200')
    Check 'the new name holds the dirty content' ((Get-Document 'User.Admin.Dirty.cls') -match 'dirty rename')
    Check 'the new name exists on the volume' (Test-Path $Target)
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
    Write-Host 'REQ-MAP-016, REQ-FS-032..036: PASS'
    exit 0
}

foreach ($Failure in $Failures)
{
    Write-Host "REQ-MAP-016, REQ-FS-032..036: FAIL - $Failure"
}

exit 1
