<#
.SYNOPSIS
    Manual check of REQ-MOUNT-023: unmount discards unsaved data.

.DESCRIPTION
    Mounts isfs.exe against the mock Atelier server on a free drive letter,
    opens a document, writes into it and keeps the handle open, so the volume
    holds a dirty buffer. The mount is then stopped the way Ctrl+C stops it
    (CTRL_BREAK to the process group, which runs SvcStop), and the script
    verifies that:

        - the mount stopped gracefully (the service log shows that SvcStop ran);
        - the write really reached the file system (the WinFsp debug log shows
          a Write operation, so the buffer of the handle was dirty);
        - the server received no PUT or DELETE for the document;
        - the document content is the one from before the write;
        - the drive letter was released and the process exited;
        - the service log has no "saved the document" line, so the loss is
          silent.

    The document under test is User.Person.cls of the mock, exposed as
    <drive>\CLS\User\Person.cls.

    The mount has to stop gracefully, not by killing the process: a killed
    process never runs SvcStop and the check would prove nothing. CTRL_BREAK
    is used because a process group created for the child disables CTRL_C.

    The mock server, the free drive letter and the work directory are managed
    by the script; the request log of the mock, the service log and the debug
    log of isfs are kept in the work directory for inspection.

.PARAMETER Exe
    The isfs.exe under test. Default: isfs\isfs.exe next to the repository,
    which has to be rebuilt before the run, because the script reports what
    that binary does.

.PARAMETER Drive
    Drive letter to mount on, for example X:. Default: the first free letter
    from X: down to D:.

.PARAMETER Port
    Loopback port of the mock server. Default: 18081.

.PARAMETER Work
    Directory for the logs. Default: isfs-unmount-test in the temp directory.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\unmount-test.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\unmount-test.ps1 -Drive W: -Port 18090
#>
[CmdletBinding()]
param(
    [string] $Exe = '',
    [string] $Drive = '',
    [int] $Port = 18081,
    [string] $Work = (Join-Path ([IO.Path]::GetTempPath()) 'isfs-unmount-test')
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

$Document = 'User.Person.cls'
$Category = 'CLS'
$DocumentPath = "${Drive}\$Category\User\Person.cls"

New-Item -ItemType Directory -Force -Path $Work | Out-Null
$MockLog = Join-Path $Work 'mock.log'
$DebugLog = Join-Path $Work 'isfs-debug.log'
$IsfsLog = Join-Path $Work 'isfs.log'
$Marker = Join-Path $Work 'written.marker'
Remove-Item $MockLog, $DebugLog, $IsfsLog, $Marker -ErrorAction SilentlyContinue

if (-not ('UnmountTestProc' -as [type]))
{
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

/* a console process group, so that CTRL_BREAK can stop isfs.exe gracefully */
public static class UnmountTestProc
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

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GenerateConsoleCtrlEvent(uint dwCtrlEvent, uint dwProcessGroupId);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool SetHandleInformation(IntPtr hObject, uint dwMask, uint dwFlags);

    [DllImport("kernel32.dll")]
    public static extern IntPtr GetStdHandle(int nStdHandle);

    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr hObject);
}
'@
}

$MockProcess = 0
$IsfsProcessId = 0
$Writer = 0
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

    $StartupInfo = New-Object UnmountTestProc+STARTUPINFO
    $StartupInfo.cb = [Runtime.InteropServices.Marshal]::SizeOf($StartupInfo)
    $ProcessInfo = New-Object UnmountTestProc+PROCESS_INFORMATION
    $CommandLine = '"{0}" -p 127.0.0.1:{1} -ns USER -user tester -pass secret -m {2} -d -1 -D "{3}"' -f
        $Exe, $Port, $Drive, $DebugLog
    $CreateNewProcessGroup = 0x00000200

    # the service log of isfs is kept in the work directory, so that the check
    # can prove that the unmount left no trace of the discarded document in it
    $IsfsLogStream = New-Object IO.FileStream($IsfsLog, 'Create', 'Write', 'ReadWrite')

    try
    {
        $IsfsLogHandle = $IsfsLogStream.SafeFileHandle.DangerousGetHandle()
        [UnmountTestProc]::SetHandleInformation($IsfsLogHandle, 1, 1) | Out-Null

        $StartupInfo.dwFlags = 0x00000100
        $StartupInfo.hStdInput = [UnmountTestProc]::GetStdHandle(-10)
        $StartupInfo.hStdOutput = [UnmountTestProc]::GetStdHandle(-11)
        $StartupInfo.hStdError = $IsfsLogHandle

        $Started = [UnmountTestProc]::CreateProcess($Exe, $CommandLine,
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

    [UnmountTestProc]::CloseHandle($ProcessInfo.hThread) | Out-Null
    [UnmountTestProc]::CloseHandle($ProcessInfo.hProcess) | Out-Null
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

    $DocumentUri = "http://127.0.0.1:$Port/api/atelier/v1/USER/doc/$Document"
    $Before = (Invoke-WebRequest -UseBasicParsing -Uri $DocumentUri).Content

    if (-not (Test-Path $DocumentPath))
    {
        throw "the document $DocumentPath is not exposed"
    }

    # the writer keeps the handle open: the buffer of the document is dirty
    $Writer = Start-Job -ArgumentList $DocumentPath, $Marker -ScriptBlock {
        param($Path, $Marker)

        $Stream = [IO.File]::Open($Path, 'Open', 'ReadWrite', 'ReadWrite')

        try
        {
            $Bytes = [Text.Encoding]::UTF8.GetBytes("// dirty change $([DateTime]::Now.Ticks)`r`n")
            $Stream.Write($Bytes, 0, $Bytes.Length)

            # push the data out of the .NET buffer without an fsync: the handle
            # stays open and nothing acknowledges a flush
            $Stream.Flush($false)

            Set-Content -Path $Marker -Value 'written'
            Start-Sleep -Seconds 180
        }
        finally
        {
            $Stream.Dispose()
        }
    }

    $Deadline = (Get-Date).AddSeconds(30)

    while (-not (Test-Path $Marker) -and (Get-Date) -lt $Deadline)
    {
        Start-Sleep -Milliseconds 200
    }

    if (-not (Test-Path $Marker))
    {
        throw "the write did not reach the volume: $(Receive-Job $Writer)"
    }

    Write-Host 'the document is written and its handle is still open'

    [UnmountTestProc]::GenerateConsoleCtrlEvent(1, [uint32] $IsfsProcessId) | Out-Null

    $Deadline = (Get-Date).AddSeconds(60)
    $Process = Get-Process -Id $IsfsProcessId -ErrorAction SilentlyContinue

    while ($Process -and -not $Process.HasExited -and (Get-Date) -lt $Deadline)
    {
        Start-Sleep -Milliseconds 300
        $Process.Refresh()
    }

    $Exited = -not $Process -or $Process.HasExited

    Start-Sleep -Milliseconds 500

    $Released = -not (Test-Path "${Drive}\")
    $After = (Invoke-WebRequest -UseBasicParsing -Uri $DocumentUri).Content
    $Writes = @(Get-Content $DebugLog -ErrorAction SilentlyContinue | Where-Object { $_ -match '>>Write ' })
    $Puts = @(Get-Content $MockLog -ErrorAction SilentlyContinue | Where-Object { $_ -match '\s(PUT|DELETE)\s' })

    if (-not $Exited)
    {
        $Failures += 'the isfs process did not exit after CTRL_BREAK'
    }

    if (-not $Released)
    {
        $Failures += "the drive letter $Drive is still present"
    }

    if (0 -eq $Writes.Count)
    {
        $Failures += 'the write never reached the file system, the buffer was not dirty'
    }

    if (0 -ne $Puts.Count)
    {
        $Failures += "the server received $($Puts.Count) save request(s)"

        foreach ($Line in $Puts)
        {
            Write-Host "    $Line"
        }
    }

    if ($After -ne $Before)
    {
        $Failures += 'the document content changed after the unmount'
    }

    $IsfsText = Get-Content $IsfsLog -Raw -ErrorAction SilentlyContinue

    if ($null -eq $IsfsText)
    {
        $IsfsText = ''
    }

    $Graceful = $IsfsText -match 'has been stopped'
    $Silent = -not ($IsfsText -match 'saved the document')

    if (-not $Graceful)
    {
        $Failures += 'the mount did not stop gracefully: isfs.exe did not run SvcStop'
    }

    if (-not $Silent)
    {
        $Failures += 'isfs logged a save of the document during the unmount'
    }

    $WriteSeen = $Writes.Count -gt 0
    $NoSave = 0 -eq $Puts.Count
    $SameContent = $After -eq $Before

    Write-Host "the service stopped gracefully: $Graceful"
    Write-Host "the write reached the file system: $WriteSeen"
    Write-Host "the server received no save request: $NoSave"
    Write-Host "the content is unchanged: $SameContent"
    Write-Host "the drive letter is released: $Released"
    Write-Host "the discarded buffer left no trace in the log: $Silent"
}
finally
{
    if (0 -ne $Writer)
    {
        Stop-Job $Writer -ErrorAction SilentlyContinue
        Remove-Job $Writer -Force -ErrorAction SilentlyContinue
    }

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
    Write-Host 'REQ-MOUNT-023: PASS'
    exit 0
}

foreach ($Failure in $Failures)
{
    Write-Host "REQ-MOUNT-023: FAIL - $Failure"
}

exit 1
