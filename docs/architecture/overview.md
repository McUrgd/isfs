# Architecture Overview

Status: design baseline (draft). The repository currently contains the WinFsp passthrough
skeleton (`isfs/isfs.c`); the Atelier integration described below is planned.

This document maps the InterSystems Atelier v1 API base path `/api/atelier/v1` onto Windows filesystem operations exposed through WinFsp.

## 1. Purpose and scope

ISFS mounts a remote InterSystems IRIS/Caché server as a Windows drive. The drive is a
**view** over the server: it does not mirror the server to disk, and there is no local
working copy. The server remains the source of truth for content, timestamps and document
existence.

In scope:

- discover namespaces and expose them as read-write directories;
- expose documents (`CLS`, `CSP`) as files;
- implement read, write, create, delete, rename and metadata queries by translating them into Atelier REST calls;
- map Atelier/server errors to NTSTATUS values that Windows applications understand.
- compile-on-save action;

Out of scope are 

- creating/deleting namespaces or category directories;
- index/query actions;
- local offline mode or a persistent on-disk cache;
- extended attributes, alternate data streams, symlinks, hard links;
- server-side source control management (server hooks still run during `PUT`).

## 2. Source of truth and components

current doc and `requrements` describe URL map, request formats,
response formats and HTTP status codes used by this design. Where the XML documentation is
ambiguous, the XML wins over assumptions in this document.

```
Windows application / Explorer
        │  Win32 file API
        ▼
WinFsp (user-mode file system host)
        │  FSP_FILE_SYSTEM callbacks
        ▼
ISFS core
  ├─ FS adapter          WinFsp callbacks ↔ internal file system model
  ├─ Path resolver       Windows path ↔ (namespace, category, document)
  ├─ Document codec      text line array / base64 ↔ bytes
  ├─ Atelier client      HTTPS, JSON, auth, ETag/hash, retries
  ├─ Caches              namespace, document descriptors, content
  └─ Diagnostics         request log, error mapping
        │  HTTPS  /api/atelier/v1/...
        ▼
InterSystems web gateway → Atelier v1 API
```

The volume is mounted in the ISFS process (WinFsp service or console host). No other process
needs access to the filesystem implementation.

## 3. Volume and path model

Documents are the only real files. Namespaces and categories are synthetic, read-only
directories derived from API data.

| Windows path | Kind | Meaning | Atelier source |
|---|---|---|---|
| `\` | directory | server root | `GET /%SYS/namespaces` (see note) |
| `\<namespace>\` | directory | one namespace | `GET /:namespace` |
| `\<namespace>\<CAT>\` | directory (synthetic) | document category | `GET /:namespace/docnames/:cat` |
| `\<namespace>\<CAT>\<name>` | file | one document | `GET`/`PUT`/`DELETE` `/:namespace/doc/<name>` |

`<CAT>` is one of `CLS`, `CSP`. The file name is the **short Atelier document
name including its extension** (`Person.cls`, `index.csp`). Dots are converted to directory separators: package structure
inside a class is not part of the name.

```
I:\
├── USER\
│   ├── CLS\
│   │   └── User.Person.cls
│   ├── RTN\
│   │   └── MyApp.mac
│   ├── CSP\
│   │   └── index.csp
│   └── OTH\
│       └── settings.json
└── %SYS\
    └── ...
```

Rules:

- Document names are used verbatim as file names. `%` is legal on NTFS, so `%SYS` and
  `%`-prefixed documents work without escaping.
- Name lookup is case-insensitive (Windows semantics), but results keep the server's
  canonical casing. If two server names differ only by case, the first is exposed and a
  warning is logged.
- Generated documents are hidden (`generated=0`). 
- There is no escaping scheme. If lossless mapping of illegal names becomes a
  requirement, it must be added as a separate, documented transformation.

## 4. Filesystem operations → Atelier API

### 4.1 Endpoint inventory

Every route from API is accounted for. "None" means the endpoint has no
filesystem semantics and is not called by the filesystem.

| API | Purpose | Filesystem use |
|---|---|---|
| `GET /%SYS/namespaces` | list namespaces | none |
| `GET /:namespace` | namespace info (databases, features) | validate/probe namespace, mount check, optional feature detection |
| `GET /:namespace/docnames` | list all documents | directory enumeration (all categories) |
| `GET /:namespace/docnames/:cat` | list documents by category | category directory enumeration |
| `GET /:namespace/docnames/:cat/:type` | list documents by category and type | optional type-filtered enumeration |
| `GET /:namespace/doc/(.*)` | read a document | `Read`, `Open`, `GetFileInfo`, rename source |
| `HEAD /:namespace/doc/(.*)` | document metadata/hash | `GetFileInfo`, `GetSecurityByName`, conflict detection |
| `PUT /:namespace/doc/(.*)` | create/update a document | `Create`, `Write`/`Flush`, overwrite, rename target |
| `DELETE /:namespace/doc/(.*)` | delete a document | `Cleanup(Delete)`, rename source cleanup |
| `POST /:namespace/docs` | batch read documents | optional prefetch (directory size/content warming) |
| `POST /:namespace/modified/:type` | changed-document hashes | metadata cache invalidation (only `CLS` is supported by the API) |
| `DELETE /:namespace/docs` | batch delete documents | none (single deletes are enough for the FS) |
| `POST /:namespace/action/compile` | compile documents | none in v1 (future control file/IOCTL) |

### 4.2 Operations

| Filesystem operation | WinFsp callback | Atelier sequence | Notes |
|---|---|---|---|
| Mount / volume info | `GetVolumeInfo` | `GET /%SYS/namespaces` (probe) | no quota API; report `TotalSize = FreeSize = 0` ("size unknown") and label `ISFS` |
| Enumerate root | `ReadDirectory` | `GET /%SYS/namespaces` | cached namespace list |
| Enumerate `<ns>\<CAT>` | `ReadDirectory` | `GET /:namespace/docnames/:cat` | descriptors cached; sizes are 0 unless cached/prefetched |
| Stat (path) | `GetFileInfo`, `GetSecurityByName` | `HEAD /:namespace/doc/<name>` then `GET` when size is required | timestamps from `ts`; size requires content |
| Open existing | `Open` | `HEAD` / `GET` document | load content once per handle; serve ranges from memory |
| Read | `Read` | `GET` document (on cache miss) | Atelier has no range read; whole document is fetched |
| Create new | `Create` | none until flush | do not `PUT` on open; an untouched new file is written as empty on cleanup |
| Write / append / update | `Write` | buffer in memory; `PUT` on `Flush`/`Cleanup` | Atelier replaces the whole document |
| Overwrite / truncate | `Overwrite`, `SetFileSize` | `PUT` with empty or truncated content | extending beyond current size is rejected |
| Flush | `Flush` | `PUT /:namespace/doc/<name>` | only when dirty; conditional on the hash obtained at open |
| Delete file | `Cleanup` with delete-on-close, `SetInformation` | `DELETE /:namespace/doc/<name>` | server `423` → lock conflict |
| Rename / move file | `SetInformation` (`FileName`) | `GET` source → `PUT` target → `DELETE` source | non-atomic; see §4.3 |
| Create/delete directory | `Create`/`Cleanup` on a directory | none | synthetic directories are read-only |
| Set timestamps | `SetBasicInfo` | none | accepted and ignored; server `ts` is authoritative |
| Set security | `SetSecurity` | none | unsupported; descriptor is synthesized read-only |
| File locking | WinFsp sharing/lock callbacks | none | no Atelier lock API; server-side document locks surface as `423`/`425` |
| Change notification | none | `POST /:namespace/modified/CLS` | used to invalidate caches, not to raise Windows notifications |
| Batch read/prefetch | `ReadDirectory`/`Open` | `POST /:namespace/docs` | optional; bounded and disabled by default |
| Compile/query/work/Ensemble | none | see endpoint table | control plane, out of scope for v1 |

### 4.3 Rename semantics

Atelier v1 has no rename or copy endpoint. Rename is composed:

1. `GET` the source document (content, `ts`, hash).
2. `PUT` the target name (possibly in another namespace/category), preserving `enc` and
   content.
3. `DELETE` the source document.
4. Update both source and target caches.

Consequences:

- The operation is not atomic. A failure between steps 2 and 3 leaves both documents;
  the caller receives an error and the caches are invalidated so the duplicate is visible.
- Renaming onto an existing file follows Windows `ReplaceIfExists`: the target is
  overwritten by step 2. If the caller does not allow replacement, the request is rejected
  with `STATUS_OBJECT_NAME_COLLISION` before any API call.
- Moving between namespaces or categories uses the target namespace/category in the `PUT`
  URL. This is supported but adds a full download/upload.
- Directory rename is impossible because directories are synthetic; it returns
  `STATUS_NOT_SUPPORTED`.

### 4.4 Write semantics

Windows applications can write at arbitrary offsets, while Atelier replaces the complete
document. The implementation therefore:

1. loads the current content when a file is opened for update (create-new starts empty);
2. applies writes to an in-memory buffer;
3. sends one `PUT` on `Flush`/`Cleanup` if the buffer is dirty;
4. updates the cache from the `PUT` response (the server may return modified content, for
   example after a source control hook).

`PUT` details:

- `Content-Type: text/plain` is mandatory (otherwise the API returns `415`).
- The body is JSON: `{"enc": false, "content": ["line", ...]}` for text, or
  `{"enc": true, "content": ["<base64 chunk>"]}` for binary.
- The document hash/`ts` obtained at open is sent in the conditional header expected by
  `%Atelier.REST` (ETag-style, normally `If-None-Match`); a mismatch produces `409`.
  Verify the exact header name against the target server during implementation.
- `?ignoreConflict=1` is only used when the user has explicitly chosen "overwrite server
  version" in configuration; the default is to fail the write and keep the local buffer.
- `201` means the document was created, `200` means it was updated.
- Empty new files are valid for `OTH`; for `CLS`/`RTN` the server may reject invalid source
  only at compile time, so creation succeeds and the file remains uncompiled.

## 5. Document representation

`GET /:namespace/doc/<name>` returns:

```json
{
  "status": { "errors": [], "summary": "" },
  "console": [],
  "result": {
    "name": "User.Person.cls",
    "db": "USER",
    "ts": "2016-01-04 14:00:04.000",
    "cat": "CLS",
    "status": "",
    "enc": false,
    "content": ["Class User.Person Extends %Persistent", "{", "}", ""]
  }
}
```

Rules used by the filesystem:

- `enc=false`: `content` is an array of lines. Joining with `\n` reproduces the file,
  including a trailing newline when the last array element is empty. No BOM is added.
- `enc=true`: `content` is an array of base64 chunks. Chunks are concatenated and decoded to
  raw bytes. This is also forced by `?binary=1`.
- On write, content that is valid UTF-8 without NUL bytes is treated as text; otherwise it
  is sent as binary with a single base64 chunk. Extensions known to be binary can be
  configured to always use binary mode.
- CRLF written by Windows applications is normalized to LF for text documents, matching the
  line-array model. Existing lone `\r` characters are preserved as data.
- `?storageOnly=1` is not used: the filesystem exposes complete documents.
- `status` inside `result` is a *document* status (for example "file is locked"), not an
  HTTP status. Both layers must be checked.
- Server-side console output (`console`) from hooks is logged at debug level; it is not
  exposed as file content.

## 6. Metadata synthesis

WinFsp requires a full `FSP_FSCTL_FILE_INFO` for every entry. Atelier provides only name,
category, database and timestamp; the rest is synthesized.

| FileInfo field | Source / rule |
|---|---|
| `FileAttributes` | `FILE_ATTRIBUTE_DIRECTORY` for synthetic dirs, `FILE_ATTRIBUTE_NORMAL` (optionally `ARCHIVE`) for documents |
| `FileSize` | UTF-8 byte length of the encoded document; exact after content is fetched, 0 in directory listings until then |
| `AllocationSize` | `FileSize` rounded up to 4096 bytes |
| `CreationTime` | document `ts` converted to FILETIME |
| `LastAccessTime` | document `ts` (or current time when the cache was warmed) |
| `LastWriteTime` | document `ts` |
| `ChangeTime` | document `ts` |
| `IndexNumber` | stable hash (FNV-1a 64) of `namespace + "\0" + document name` |
| `HardLinks` | 0 |
| `ReparseTag` | 0 |

`ts` values from Atelier have no time zone (`YYYY-MM-DD HH:MM:SS.mmm`); they are interpreted
as server local time. Because no offset is available, timestamps may be shifted if client
and server are in different zones. This is an accepted v1 limitation; it is documented in
the requirements for diagnostics.

Accurate sizes are expensive: `GetDocNames` does not return them, and each size requires a
content download. V1 behavior:

- directory enumeration reports size 0 for documents that are not cached;
- opening or stat-ing a file fetches content and reports the exact size;
- an optional `prefetchSizes` mode uses `POST /:namespace/docs` to warm a directory in one
  request, bounded by a configurable byte/file limit.

Security descriptors are synthesized: the mounting user gets full access; the descriptor is
not read from or written to the server.

## 7. Error mapping

Both the HTTP status and the Atelier `status.errors`/`document.status` payload are used.
The first determines the NTSTATUS value; the second is logged and may refine the message.

| HTTP | NTSTATUS | Typical cause |
|---|---|---|
| `200`, `201` | `STATUS_SUCCESS` | success |
| `304` | `STATUS_SUCCESS` | cached content is still current |
| `400` | `STATUS_OBJECT_NAME_INVALID` | invalid document name or malformed request |
| `401` | `STATUS_LOGON_FAILURE` (mount) / `STATUS_ACCESS_DENIED` (operation) | bad credentials or expired session |
| `403` | `STATUS_ACCESS_DENIED` | namespace/document not writable |
| `404` | `STATUS_OBJECT_NAME_NOT_FOUND` / `STATUS_OBJECT_PATH_NOT_FOUND` | missing document or namespace |
| `409` | `STATUS_FILE_LOCK_CONFLICT` | client/server version conflict |
| `415` | `STATUS_INVALID_PARAMETER` | wrong content type (implementation bug) |
| `423` | `STATUS_FILE_LOCK_CONFLICT` | document locked on the server |
| `425` | `STATUS_FILE_LOCK_CONFLICT` | Atelier uses 425 for a locked write; it is not the HTTP "Too Early" semantics |
| `429` | `STATUS_NETWORK_BUSY` | throttling (not in the XML, handled defensively) |
| `500` | `STATUS_UNEXPECTED_IO_ERROR` | server error |
| `502`, `503`, `504` | `STATUS_NETWORK_UNREACHABLE` / `STATUS_IO_TIMEOUT` | gateway/server unavailable |
| connection failure | `STATUS_NETWORK_UNREACHABLE` | host down, DNS/TLS failure |
| timeout | `STATUS_IO_TIMEOUT` | request exceeded the configured timeout |

`STATUS_FILE_LOCK_CONFLICT` is intentional for `409`: Windows surfaces it as a sharing/
version conflict, and the user can retry after refreshing. The server error summary is
logged and, where the WinFsp callback supports it, included in the message shown by
Explorer.

## 8. Caching and consistency

All caches are in memory; the server is authoritative.

- **Namespace cache** — result of `GET /%SYS/namespaces`, TTL 60 s.
- **Document descriptor cache** — per `(namespace, category)`: entries from
  `GetDocNames` (`name`, `cat`, `ts`, `db`, `gen`), TTL 30 s by default.
- **Content cache** — keyed by `(namespace, document name, hash/ts)`, LRU with a configurable
  byte limit (default 64 MiB). A conditional `GET` with the stored hash returns `304` and
  avoids re-downloading unchanged content.
- **Dirty buffers** — per open handle, flushed on demand. Multiple handles to the same
  document can overwrite each other; the conditional `PUT` turns the loser into `409`
  rather than silent data loss.
- **External change detection** — `POST /:namespace/modified/CLS` compares database hashes
  and returns changed class names. It only supports `CLS`; for other categories the TTL and
  conditional `GET` are used. Results invalidate descriptor and content entries.

Local mutations invalidate affected entries immediately: writes update the document cache
from the `PUT` response; deletes remove the entry; renames remove both source and target
entries.

## 9. Concurrency and locking

- One mutex per document serializes flush, rename, delete and cache updates.
- Directory enumeration is serialized per `(namespace, category)` to avoid duplicate
  `GetDocNames` calls; other callbacks may proceed in parallel.
- Writes are write-through at close/flush, not write-behind on a timer.
- The Atelier API exposes no client-side locks. Windows sharing modes are enforced locally
  by WinFsp; server-side locks from Studio/other clients appear as `423`/`425` and are
  reported as `STATUS_FILE_LOCK_CONFLICT`.
- A failed `PUT` leaves the handle's buffer dirty; nothing retries it in the background. On
  close the remaining buffer goes out in one final `PUT`, and whatever is still not
  acknowledged at that point is discarded without an error. Unmount never flushes: pending
  changes are dropped silently, with no `PUT` and no error reported for the discarded data
  (REQ-MOUNT-023). Only content acknowledged by a successful flush is guaranteed to be on the
  server (REQ-MOUNT-024).
- Long-running requests are bounded by connect/read timeouts; mounts fail fast if the
  server is unreachable.

## 10. Diagnostics

- Never log document content or secrets.
- Log cache decisions (hit/miss/304/invalidate) at debug level.
- Expose the effective configuration (server URL, read-only mode, TTLs, cache limits) at
  startup.
- Keep the Atelier request/response correlation in one log scope per filesystem operation,
  so a WinFsp callback can be traced end to end.
- Mount failures (`401`, DNS, TLS, timeout, unavailable `GET /%SYS/namespaces`) must produce
  an actionable message in the mount dialog/service log.

## 11. Not mapped / out of scope

| Capability | Reason |
|---|---|
| Create/delete/rename namespaces or category dirs | synthetic directories; namespace lifecycle is server administration |
| Compile, index, SQL query, async work | control plane; mapping to files would be misleading and unsafe |
| Ensemble class/adapter browsing | could be virtual folders later; no v1 requirement |
| `GetMetaData` ZIP, jobs, debugger, legacy converters | no filesystem semantics |
| Local/offline cache, background sync | conflicts with "server is the source of truth" and adds conflict complexity |
| Extended attributes, ADS, symlinks, reparse points, hard links | not representable in Atelier v1 |
| Server-side file locking | no API; only document-level lock errors are surfaced |
| Editing security descriptors | no ACL model on the server |
| Sparse files, compression, quotas | no API support |
| Truncate/extend via `SetFileSize` beyond current content | replacement upload only; extension is rejected |

## 12. Open decisions

These must be resolved and recorded as requirements before implementation:

1. **Namespace discovery** — resolved in
   [`mount-lifecycle.md`](../requirements/functional/mount-lifecycle.md) (REQ-MOUNT-013):
   `GET /%SYS/namespaces` is the only source (no configured fallback list); an empty result
   yields an empty volume, an unavailable endpoint fails the mount.
2. **Path layout** — flat `\<ns>\<CAT>\<name>` (current proposal) vs nested package
   directories. Flat is lossless and simpler; nested is friendlier but needs an escaping
   scheme.
3. **Size strategy** — default lazy (0 in listings) vs opt-in batch prefetch; measure the
   cost on large namespaces.
4. **Conflict policy** — always fail on `409`, or offer a per-mount "force overwrite"
   option that adds `ignoreConflict=1`.
5. **Binary detection** — extension allow-list vs UTF-8/NUL sniffing; what to do with
   mixed/undecodable text.
6. **Case collisions** — skip duplicates, suffix them, or fail the mount; log format.
7. **Timestamp time zone** — accept server-local `ts` as-is, or add a configurable offset.
8. **Generated/system documents** — default visibility and `hideSystemDocuments` defaults.
9. **Rename across namespaces** — allow (download/upload) or restrict to the same
   namespace for v1.
10. **Authentication** — resolved: the login and password are passed as the `-user` and
    `-pass` parameters of `isfs.exe` and kept in memory only (REQ-MOUNT-010). How they are
    presented to the server (Basic or web session) follows the deployment's scheme.
11. **Read-only namespaces** — configuration list (for example `%SYS`) vs rely on `403`.
12. **Volume size/free size** — resolved: `TotalSize` and `FreeSize` are both 0 ("size
    unknown") and are not configurable
    ([`mount-lifecycle.md`](../requirements/functional/mount-lifecycle.md), REQ-MOUNT-008).

## 13. Requirements traceability

| Requirement area | Sections |
|---|---|
| Mount / attach lifecycle | §3, §6, §9 |
| Filesystem operations | §4, §5 |
| Mapping of paths to Atelier resources | §3, §4, §12 |
| Connection and authentication | §2, §7, §10 |
| Errors, diagnostics and logging | §7, §10 |
| Caching and performance | §6, §8 |

The requirement files themselves are indexed in `docs/requirements/README.md`; IDs will be
assigned when each area's first requirement is written.

## 14. Related documents

- API reference for day-to-day work: `docs/architecture/api.md`
- Requirements index: `docs/requirements/README.md`
- Project overview and build instructions: `README.md`
