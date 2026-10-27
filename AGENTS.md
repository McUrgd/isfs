# AGENTS.md

## Project
This project exposes the InterSystems Atelier v1 REST API (the `/api/atelier/v1` API that
any Caché/IRIS installation serves, not the Eclipse-based Atelier IDE) as a Windows
filesystem.

## Documentation map
- Requirements: `docs/requirements/README.md` (index) and `docs/requirements/functional/`
- Architecture: `docs/architecture/overview.md`
- API: `docs/architecture/api.md` (a description of the Atelier v1 API)

## Memory debugging (Application Verifier / PageHeap / cdb)

### Application Verifier and PageHeap

Application Verifier is installed on this system:

- `C:\Windows\System32\appverif.exe`, version `10.0.26100.8249`;
- configuration and querying require an elevated console.

PageHeap can be enabled in two ways (both require elevation).

With Application Verifier:

```cmd
appverif -enable heaps -for isfs.exe -with heaps.full=true
appverif -query heaps -for isfs.exe
appverif -disable * -for isfs.exe
appverif -delete settings -for isfs.exe
```

With `gflags.exe` from Debugging Tools:

```cmd
"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\gflags.exe" /p /enable isfs.exe /full
"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\gflags.exe" /p /disable isfs.exe
```

The settings are bound to the image name (IFEO) and apply to every launch of it,
so they must be disabled after the test. PageHeap catches an invalid free, a double
free and a write past the end of a block at the moment the error happens.

### The cdb debugger

Debugging Tools for Windows is installed, but its directory is not on `PATH`:

- `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe` — the console
  debugger, version `10.0.28000.2957`;
- `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\windbg.exe` — GUI;
- `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\gflags.exe` — PageHeap.

Starting `isfs.exe` under `cdb` (from the directory that holds `isfs.exe` and
`winfsp-x64.dll`):

```cmd
"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe" -g -G -o isfs.exe -p 127.0.0.1:18080 -ns USER -user tester -pass secret -m X:
```

When it stops on an error:

```text
kb
!analyze -v
!heap -p -a <address>
```

`kb` shows the stack, `!analyze -v` the crash summary and `!heap -p -a` the state of
the block (it is meaningful with PageHeap enabled). If needed, add the Debugging Tools
directory to `PATH` or call `cdb.exe` by its full path.
