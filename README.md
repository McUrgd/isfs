=== ISFS AS Windows FileSystem ===

This project is based on the passthrough file system example from WinFsp.
It is licensed under the GNU General Public License version 3 with the
WinFsp FLOSS Exception.

    WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos
    https://github.com/winfsp/winfsp/

The full text of the GPLv3 is in `LICENSE.txt`; the FLOSS Exception and the
third-party notices are in `NOTICE`.

This project connects Atelier v1 API by Intersystems as Windows filesystem.

=== Building ===

Requirements: Visual Studio 2022/2026 C++ tools (x64) and WinFsp installed
(runtime DLL is needed at run time and the import library at build time).

WinFsp SDK parts used by the build:

- `passthrough/winfsp/` - headers (`winfsp.h`, `fsctl.h`, `launch.h`), vendored
- `passthrough/lib/winfsp-x64.lib` - import library, copied from the installed
  WinFsp SDK by `scripts\copy-winfsp-lib.ps1` (not stored in the repository;
  it is a WinFsp build artifact, see `NOTICE`)

The VS Code build task (`.vscode/tasks.json`, Ctrl+Shift+B) compiles and links with these local files, so no external SDK include path is required:

```
cl /nologo /Zi /MT /D_DEBUG /D_CONSOLE /DUNICODE /D_UNICODE ^
   /I passthrough /I . ^
   /Feisfs\isfs.exe /Foisfs\ isfs\isfs.c isfs\atelier.c isfs\helper.c cjson\cJSON.c ^
   /link passthrough\lib\winfsp-x64.lib advapi32.lib winhttp.lib
```

Note: the value of `/Fe` and `/Fo` must be attached to the option (no space). The compiler
from Visual Studio 2026 (18.x) does not accept `/Fe <name>` and reports
`D9024: unrecognized source file type`, treating the name as a source file.

=== Mock Atelier server ===

For manual tests without a real Atelier server, `scripts\mock-atelier.ps1` serves the
endpoints that the file system uses for reading and writing documents:

- `GET /api/atelier/v1/<namespace>` - namespace information;
- `GET /api/atelier/v1/<namespace>/docnames/<category>` - document listing;
- `GET /api/atelier/v1/<namespace>/doc/<dotted-name>` - document content
  (text with `enc=false`, binary base64 with `enc=true`);
- `PUT /api/atelier/v1/<namespace>/doc/<dotted-name>` - create or replace a
  document, with the `If-None-Match` conflict check;
- `DELETE /api/atelier/v1/<namespace>/doc/<dotted-name>` - delete a document.

Start it:

```
powershell -ExecutionPolicy Bypass -File scripts\mock-atelier.ps1 -Port 18080
```

Mount the volume against it:

```
isfs.exe -p 127.0.0.1:18080 -ns USER -user tester -pass secret -m X:
```

A save is conditional on the hash of the document that was opened. The
`-overwrite` option replaces the server version even when it changed
(`ignoreConflict=1`); it is off by default.

The request log goes to `%TEMP%\isfs-mock-atelier.log` by default; use `-LogFile` to change it.
The namespace and the exposed documents are defined at the top of the script.

=== Manual check: unmount discards unsaved data ===

`scripts\unmount-test.ps1` is a reproducible manual check of REQ-MOUNT-023. It starts the mock
on a free loopback port, mounts `isfs\isfs.exe` on a free drive letter, writes into
`User.Person.cls` and keeps the handle open, so the volume holds a dirty buffer. The mount is
then stopped the way Ctrl+C stops it (a graceful `SvcStop`, not a killed process), and the
script verifies that the write reached the file system, the server received no `PUT` or
`DELETE`, the document content is unchanged, the drive letter was released and the service log
has no `saved the document` line:

```
powershell -ExecutionPolicy Bypass -File scripts\unmount-test.ps1
```

Rebuild `isfs\isfs.exe` first: the check reports what that binary does. `-Drive` and `-Port`
override the free drive letter and the port; the work directory (`%TEMP%\isfs-unmount-test` by
default) keeps the request log of the mock, the service log and the debug log of isfs. The
script exits with 0 only when every check passed.

=== Manual check: renaming documents ===

`scripts\rename-test.ps1` is a reproducible manual check of REQ-MAP-016 and REQ-FS-032..036. It
mounts `isfs\isfs.exe` on a free drive letter against the mock on a free loopback port and
renames documents the way Explorer and the command line do: a text document into another
package, a binary document, a rename over an existing document (refused, then allowed with
`MoveFileEx`), a rename into another category, a rename that only changes the case, a rename of
a package directory (refused) and a rename through a handle with a dirty buffer. The script
checks the requests that reach the server, the content of the target document and the
`enc` flag of binary documents:

```
powershell -ExecutionPolicy Bypass -File scripts\rename-test.ps1
```

Rebuild `isfs\isfs.exe` first. `-Drive` and `-Port` override the free drive letter and the
port; the work directory (`%TEMP%\isfs-rename-test` by default) keeps the request log of the
mock, the service log and the debug log of isfs. The script exits with 0 only when every check
passed.

=== Manual check: deleting documents ===

`scripts\delete-test.ps1` is a reproducible manual check of REQ-FS-030. It mounts
`isfs\isfs.exe` on a free drive letter against the mock on a free loopback port, fills the
descriptor cache of a category, deletes a document with `[IO.File]::Delete`, opens a document
with `FileOptions.DeleteOnClose` and writes into it, and tries to delete a synthetic
directory. The script verifies that the delete is a `DELETE` of the Atelier name, that the
entry leaves the cache (the listing is reloaded and no longer holds the name), that the dirty
buffer of a delete-on-close handle is not written, and that a directory is refused without a
request:

```
powershell -ExecutionPolicy Bypass -File scripts\delete-test.ps1
```

Rebuild `isfs\isfs.exe` first. `-Drive` and `-Port` override the free drive letter and the
port; the work directory (`%TEMP%\isfs-delete-test` by default) keeps the logs. The script
exits with 0 only when every check passed.

=== Third-party ===

The complete list of third-party components and licenses is in `NOTICE`.

- `winfsp/` (headers) and `passthrough/` - WinFsp, Copyright (C) Bill Zissimopoulos,
  https://github.com/winfsp/winfsp/, GPLv3 with the WinFsp FLOSS Exception
  (`passthrough/License.txt`, exception text also in `NOTICE`). `isfs/` is derived
  from the passthrough example and is therefore distributed under the same terms.
- `cjson/` - [cJSON](https://github.com/DaveGamble/cJSON) v1.7.19, MIT license
  (see `cjson/LICENSE`). It parses the UTF-8 JSON answers of the Atelier API;
  the strings of the server are converted to UTF-16 by `Utf8ToWide()` in `isfs/isfs.c`.
  The sources are vendored as they are, so they can be updated by replacing the two files.

=== Runtime DLL ===

The executable imports `winfsp-x64.dll` at load time. WinFsp 2.x keeps it in `C:\Program Files (x86)\WinFsp\bin`
and does not add that directory to `PATH`, so the loader cannot find it (error 0xc0000135 /
"the system cannot find winfsp-x64.dll").

The VS Code build task solves this by copying the DLL next to the executable
(`isfs\winfsp-x64.dll`), so no `PATH` changes are needed. The file is listed in `.gitignore`.
The import library `passthrough\lib\winfsp-x64.lib` is obtained the same way: it is a
WinFsp build artifact and is not stored in the repository.

Both copies are done by scripts that search for the WinFsp installation (registry
`HKLM\SOFTWARE\WOW6432Node\WinFsp`, then the `Program Files (x86)\WinFsp` fallback):
`scripts\copy-winfsp-dll.ps1` for the runtime DLL and `scripts\copy-winfsp-lib.ps1`
for the import library. They are called by the `winfsp: copy runtime dll` and
`winfsp: copy import library` tasks via `powershell.exe -File`, i.e. without an
intermediate shell, so that no nested quoting is involved.

Alternatives:

- add `C:\Program Files (x86)\WinFsp\bin` to the `PATH` environment variable, or
- copy `winfsp-x64.dll` next to `isfs.exe` manually.

The WinFsp driver itself must be installed on the machine (it is already registered as a service,
which is required for mounting a drive).

=== License ===

ISFS is free software: GNU General Public License v3 with the WinFsp FLOSS Exception.
`LICENSE.txt` holds the GPLv3 text, `NOTICE` the exception and the third-party notices.
The exception forbids mixing a WinFsp-based program with proprietary software, and the
parts derived from the passthrough example (GPLv3) cannot be relicensed; a closed-source
distribution needs a commercial WinFsp license and a rewrite of the derived code.

