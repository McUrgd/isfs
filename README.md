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
   /Fe isfs\isfs.exe /Fo isfs\ isfs\isfs.c ^
   /link passthrough\lib\winfsp-x64.lib advapi32.lib
```
