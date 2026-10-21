=== ISFS AS Windows FileSystem ===

This project is based on the passthrough file system example from WinFsp (Copyright © Bill Zissimopoulos). 
It is licensed under the GNU General Public License v3 with the FLOSS Exception.
https://github.com/winfsp/winfsp/

This project connects Atelier v1 API by Intersystems as Windows filesystem.

=== Building ===

Requirements: Visual Studio 2022/2026 C++ tools (x64) and WinFsp installed (runtime DLL is needed at run time).

WinFsp SDK parts used by the build are vendored in the repository:

- `passthrough/winfsp/` - headers (`winfsp.h`, `fsctl.h`, `launch.h`)
- `passthrough/lib/winfsp-x64.lib` - import library

The VS Code build task (`.vscode/tasks.json`, Ctrl+Shift+B on `isfs/isfs.c`) compiles and links with these local files, so no external SDK include path is required:

```
cl /nologo /Zi /MT /D_DEBUG /D_CONSOLE /DUNICODE /D_UNICODE ^
   /I passthrough ^
   /Feisfs\isfs.exe /Foisfs\ isfs\isfs.c ^
   /link passthrough\lib\winfsp-x64.lib advapi32.lib winhttp.lib
```

Note: the value of `/Fe` and `/Fo` must be attached to the option (no space). The compiler
from Visual Studio 2026 (18.x) does not accept `/Fe <name>` and reports
`D9024: unrecognized source file type`, treating the name as a source file.

=== Runtime DLL ===

The executable imports `winfsp-x64.dll` at load time. WinFsp 2.x keeps it in `C:\Program Files (x86)\WinFsp\bin`
and does not add that directory to `PATH`, so the loader cannot find it (error 0xc0000135 /
"the system cannot find winfsp-x64.dll").

The VS Code build task solves this by copying the DLL next to the executable
(`isfs\winfsp-x64.dll`), so no `PATH` changes are needed. The file is listed in `.gitignore`.

The copy is done by `scripts\copy-winfsp-dll.ps1`, which searches for the WinFsp installation
(registry `HKLM\SOFTWARE\WOW6432Node\WinFsp`, then the `Program Files (x86)\WinFsp` fallback).
It is called by the `winfsp: copy runtime dll` task via `powershell.exe -File`, i.e. without an
intermediate shell, so that no nested quoting is involved.

Alternatives:

- add `C:\Program Files (x86)\WinFsp\bin` to the `PATH` environment variable, or
- copy `winfsp-x64.dll` next to `isfs.exe` manually.

The WinFsp driver itself must be installed on the machine (it is already registered as a service,
which is required for mounting a drive).
