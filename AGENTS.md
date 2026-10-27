# AGENTS.md

## Проект
This project exposes the InterSystems Atelier v1 REST API (the `/api/atelier/v1` API that
any Caché/IRIS installation serves, not the Eclipse-based Atelier IDE) as a Windows
filesystem.

## Карта документации
- Требования: `docs/requirements/README.md` (индекс) и `docs/requirements/functional/`
- Архитектура: `docs/architecture/overview.md`
- API: `docs/architecture/api.md` (описание Atelier API v1)

## Отладка памяти (Application Verifier / PageHeap / cdb)

### Application Verifier и PageHeap

Application Verifier установлен в системе:

- `C:\Windows\System32\appverif.exe`, версия `10.0.26100.8249`;
- настройка и запрос требуют консоли с правами администратора.

PageHeap можно включить двумя способами (оба требуют elevation).

Через Application Verifier:

```cmd
appverif -enable heaps -for isfs.exe -with heaps.full=true
appverif -query heaps -for isfs.exe
appverif -disable * -for isfs.exe
appverif -delete settings -for isfs.exe
```

Через `gflags.exe` из Debugging Tools:

```cmd
"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\gflags.exe" /p /enable isfs.exe /full
"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\gflags.exe" /p /disable isfs.exe
```

Настройки привязаны к имени образа (IFEO) и действуют на все его запуски,
поэтому после теста их нужно отключать. PageHeap ловит invalid free, double
free и выход за границу блока в момент ошибки.

### Отладчик cdb

Debugging Tools for Windows установлены, но их каталог не входит в `PATH`:

- `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe` — консольный
  отладчик, версия `10.0.28000.2957`;
- `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\windbg.exe` — GUI;
- `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\gflags.exe` — PageHeap.

Запуск `isfs.exe` под `cdb` (из каталога, где лежат `isfs.exe` и
`winfsp-x64.dll`):

```cmd
"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe" -g -G -o isfs.exe -p 127.0.0.1:18080 -ns USER -user tester -pass secret -m X:
```

При остановке на ошибке:

```text
kb
!analyze -v
!heap -p -a <address>
```

`kb` показывает стек, `!analyze -v` — сводку падения, `!heap -p -a` — состояние
блока (имеет смысл при включённом PageHeap). При необходимости добавить каталог
Debugging Tools в `PATH` или вызывать `cdb.exe` по полному пути.
