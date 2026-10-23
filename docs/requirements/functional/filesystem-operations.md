# Filesystem operations — requirements

Status: draft.

Scope: what the mounted volume does when Windows reads, writes, creates, deletes,
renames or enumerates. This file covers the behaviour of the file system callbacks
themselves — which Atelier call backs an operation, what a caller observes, how
content is encoded, and which operations are refused. The mapping of paths and
names onto Atelier resources is in `atelier-mapping.md`; the mount lifecycle is in
`mount-lifecycle.md`; the transport and the credentials are in
`connection-auth.md`; the caches and the prefetch modes are in `caching.md`; the
canonical HTTP-to-NTSTATUS table and the log formats are in
`errors-diagnostics.md`.

Source sections of [`docs/architecture/overview.md`](../../architecture/overview.md):
§4 (operations), §5 (document representation), §6 (metadata synthesis),
§9 (concurrency and locking), §11 (not mapped), §12 (open decisions).
Source sections of [`docs/architecture/api.md`](../../architecture/api.md):
§5 (content encoding), §6 (hashing, ETags and conflicts), §8.2 (listing),
§8.3 (documents), §12 (implementation notes and discrepancies).

> Current state: the repository contains the WinFsp passthrough skeleton
> (`isfs/isfs.c`); the Atelier-backed volume source is not implemented yet.
> Every requirement below is therefore `draft`, except REQ-FS-001, REQ-FS-003
> and REQ-FS-004, which the current build already satisfies, and the requirements
> marked `dropped`.

## Operation → endpoint map

The endpoint an operation is built on. This is the reference for the requirements
below and mirrors overview §4.2.

| Filesystem operation | WinFsp callback | Atelier call |
|---|---|---|
| Enumerate a category | `ReadDirectory` | `GET /:namespace/docnames/:cat` |
| Stat / security | `GetFileInfo`, `GetSecurityByName` | none — attributes and the full-access descriptor are synthesized; size 0 until content is fetched |
| Open for read | `Open` | `GET /:namespace/doc/<name>` |
| Read | `Read` | none — served from the handle buffer |
| Create | `Create` | none until flush |
| Write / overwrite | `Write`, `Overwrite` | none — buffered in memory |
| Truncate / extend | `SetFileSize` | none — buffered in memory |
| Flush | `Flush` | `PUT /:namespace/doc/<name>` |
| Delete | `Cleanup` (delete-on-close) | `DELETE /:namespace/doc/<name>` |
| Rename / move | `SetInformation` (`FileName`) | `GET` source → `PUT` target → `DELETE` source |
| Create/delete/rename a directory | `Create`, `Cleanup`, `SetInformation` | none — synthetic directories are read-only |
| Set timestamps | `SetBasicInfo` | none — accepted and ignored |
| Set security | `SetSecurity` | none — refused |
| Lock / sharing | WinFsp sharing callbacks | none — enforced locally |

## Requirements

### REQ-FS-001 — A category listing is served by the category listing endpoint

Status: implemented

Listing the contents of a category directory must be served by
`GET /:namespace/docnames/:cat` and must return exactly the documents of that
category.

Acceptance criteria:
- Listing `\<CAT>\` issues one request to `/docnames/<CAT>` for that category.
- The entries of the listing are the documents of that category and of no other
  category; a document appears as a file or as a package level, according to the
  decomposition of its name (REQ-MAP-004, REQ-MAP-007).
- The root listing is not part of this requirement: the contents of `\` are
  defined by REQ-MAP-001 and REQ-MAP-002.
- The listing is served from the descriptor cache where the cache is valid; the
  cache behaviour itself is defined in `caching.md`.

### REQ-FS-002 — Generated documents are not listed

Status: dropped

Document listings must exclude generated documents, because the server hides them
by default and generated sources are not meant to be edited as files.

Acceptance criteria:
- The listing request does not set `generated=1`; the server default `generated=0`
  applies.
- A generated document does not appear in a category listing.
- A generated document is not reachable by opening its path; the request fails with
  `STATUS_OBJECT_NAME_NOT_FOUND`.

> Rationale: duplicate of REQ-MAP-011 ("Generated documents are not exposed"),
> which states the same behaviour from the mapping side and stays in force. The
> ID is retired and is not reused.

### REQ-FS-003 — The size of a listed document is reported as 0

Status: implemented

A directory listing reports the size of a document as 0, because `GetDocNames`
returns no size and every exact size would cost a content download.

Acceptance criteria:
- A listing of a category reports `FileSize = 0` for every document.
- The size becomes exact only after the content has been fetched (REQ-FS-006).
- No content request is issued solely to compute a size during a listing.

### REQ-FS-004 — Listings keep the canonical casing of the server

Status: implemented

A listing must show the document names in the casing used by the server, not the
casing of the request.

Acceptance criteria:
- The names in a listing are the `name` values of the descriptors returned by the
  server.
- A caller that addresses a path in a different casing resolves it to the
  canonical name (REQ-MOUNT-019).

### REQ-FS-005 — Stat is answered from the document descriptor

Status: dropped

v1 does not collect per-document stat data: no `HEAD` request is issued, and the
attributes, the security descriptor and the size reported for a path are
synthesized locally.

> Rationale: `GetSecurityByName` returns the full-access descriptor and the
> attributes derived from the path; a directory listing reports size 0
> (REQ-FS-003). An exact size is available only for an open handle, whose content
> was fetched (REQ-FS-006). The `HEAD` endpoint of the API is not used in v1.

### REQ-FS-006 — The size of a document is the byte length of its decoded content

Status: draft

The size reported for a document must be the length in bytes of the content as it
is served to the caller, counted after the text or binary decoding.

Acceptance criteria:
- For a text document the size is the length of the UTF-8 bytes of the joined
  content, including a trailing newline when the line array ends with an empty
  element.
- For a binary document the size is the length of the decoded bytes.
- The size is exact once the content has been fetched; before that it is 0
  (REQ-FS-003).
- `AllocationSize` is the size rounded up to a multiple of 4096 bytes.

### REQ-FS-007 — Timestamps are interpreted as server local time

Status: draft

The `ts` value of a document carries no time zone. ISFS must interpret it as
local time and must not apply an offset of its own.

Acceptance criteria:
- The reported time is the `ts` value read as a local time.
- No timezone conversion is applied to `ts` before the conversion to FILETIME.
- When the server and the machine running ISFS are in different time zones, the
  reported time is shifted by the difference between the two zones.

> Known limitation: `ts` has the form `YYYY-MM-DD HH:MM:SS.mmm` without an
> offset, so the time zone of the server cannot be established. A client in a
> different zone sees shifted timestamps. This is accepted for v1 (overview §6);
> overview §12.7 keeps a configurable offset as an open point.

### REQ-FS-008 — Attributes distinguish synthetic directories from documents

Status: draft

The attributes reported for an entry must mark the synthetic category directories
as directories and the documents as ordinary files.

Acceptance criteria:
- A category directory carries `FILE_ATTRIBUTE_DIRECTORY`.
- A document carries `FILE_ATTRIBUTE_NORMAL`; `FILE_ATTRIBUTE_ARCHIVE` may be
  added and must not change the semantics of the entry.
- `FILE_ATTRIBUTE_DIRECTORY` is never reported for a document, and a category
  directory is never reported as a file.

### REQ-FS-009 — The security descriptor is synthesized locally

Status: draft

The security descriptor of every entry must be synthesized by ISFS and must grant
the mounting user full access to it. It is never read from the server and never
written to it.

Acceptance criteria:
- Every entry reports a descriptor that grants the mounting user full control.
- The descriptor is the same for a document and for a synthetic directory.
- No request to the server is issued to read or to write a descriptor.

### REQ-FS-010 — Setting the security descriptor is refused

Status: draft

An attempt to change the security descriptor of an entry must be refused.

Acceptance criteria:
- The `SetSecurity` callback fails with `STATUS_NOT_SUPPORTED`.
- No request is issued to the server for the attempt.
- The refusal does not affect the open handle or the mounted volume.

### REQ-FS-011 — Setting timestamps is accepted and ignored

Status: draft

An attempt to set the timestamps of an entry must succeed, but must not change
the timestamps reported afterwards: the server `ts` stays authoritative.

Acceptance criteria:
- The `SetBasicInfo` callback succeeds.
- A `GetFileInfo` after the attempt reports the unchanged server `ts`.
- No request is issued to the server for the attempt.

### REQ-FS-012 — The index number of an entry is stable

Status: draft

The index number reported for a document must be stable for the lifetime of the
mount and must be derived from the namespace and the document name.

Acceptance criteria:
- The index number of a document is the FNV-1a 64 hash of the namespace, a NUL
  character and the document name.
- Two `GetFileInfo` calls for the same document report the same index number.
- The index number of a document does not depend on the time of the call, on the
  cache state or on the casing used in the path.
- The index number of a synthetic directory is derived from the category name in
  the same way.

### REQ-FS-013 — The content is fetched once per open handle

Status: draft

Opening a document for reading must fetch its content once; all reads on that
handle are then served from memory.

Acceptance criteria:
- The first read on a handle issues the content request for the document.
- Further reads on the same handle issue no further request.
- Two handles on the same document each hold their own buffer; an open handle
  never re-reads the document from the server.

### REQ-FS-014 — Reads are served from memory, never as server byte ranges

Status: draft

The Atelier API has no byte-range read. A read must therefore be served entirely
from the content held in memory, and the server must never be asked for a range.

Acceptance criteria:
- A read at an arbitrary offset returns the bytes of the in-memory content.
- No request with a range header is issued for a read.
- A read that lies entirely beyond the end of the content returns 0 bytes and no
  error.

### REQ-FS-015 — A text document is the line array joined with a line feed

Status: draft

A document with `enc=false` must be reconstructed from its line array by joining
the elements with a line feed.

Acceptance criteria:
- The elements of `content` are joined with `\n`.
- An empty last element produces a trailing line feed at the end of the file.
- No byte order mark is added.
- A document with an empty line array is an empty file.

### REQ-FS-016 — A binary document is the base64 chunks decoded

Status: draft

A document with `enc=true` must be reconstructed by concatenating its base64
chunks and decoding the result.

Acceptance criteria:
- The chunks are concatenated in the order in which they appear in `content`.
- The concatenated base64 is decoded and the bytes are served unchanged.
- The decoded bytes are the content of the file, including NUL bytes and byte
  sequences that are not valid UTF-8.

### REQ-FS-017 — A conditional read that answers 304 is a success

Status: dropped

ISFS does not perform conditional reads: it keeps no shared content cache, so a
`304 Not Modified` answer has no content to serve.

> Rationale: `Open` always fetches the document with `GET` and the content is
> owned by the handle (REQ-FS-013), so there is no cache entry to revalidate or
> to return after a `304`. A `304` is therefore treated as a protocol error.
> Content caching is deferred until a requirement needs it; the ETag of the
> document is still kept on the handle for conditional writes (REQ-FS-025).

### REQ-FS-018 — Creating a document issues no request

Status: draft

Creating a file must not send anything to the server: the document exists on the
server only after a successful flush.

Acceptance criteria:
- The `Create` callback issues no request.
- The file exists on the server only after a successful `PUT`.
- A `GetFileInfo` for the new path before the first flush reports the local
  buffer of the handle (REQ-MAP-014).

### REQ-FS-019 — A new document that is never written is stored empty

Status: draft

A file that was created and never written must be sent to the server as an empty
document when the handle is closed, so that the caller observes the file it asked
for.

Acceptance criteria:
- A created file whose buffer stayed empty is sent with one `PUT` carrying an
  empty content array.
- The server answers `201` for the newly created document.
- The document exists on the server after the handle is closed.

### REQ-FS-020 — Writes are buffered and sent as one document

Status: draft

Writes to a document must be applied to an in-memory buffer, and the buffer must be
sent to the server as a single replacement of the document.

Acceptance criteria:
- The `Write`, `Overwrite` and `SetFileSize` callbacks issue no request.
- The bytes written by the caller are visible to the caller through the handle
  before the flush.
- The document on the server is replaced as a whole; a partial document is never
  written.

### REQ-FS-021 — The write is a JSON document body

Status: draft

The request that saves a document must carry the document content as a JSON body
in the shape of the document object.

Acceptance criteria:
- The body is `{"enc": <bool>, "content": [<line or chunk>, ...]}`.
- The body is sent with `Content-Type: application/json`.
- A text document carries the lines of the buffer; a binary document carries the
  base64 representation of its bytes.
- The request is accepted by the server; a body with another content type is not
  used, because the server answers `415` to it.

> Note: the endpoint description mentions `text/plain`, but the implementation of
> the endpoint requires `application/json` (api.md §12, item 1). Overview §4.4
> still states `text/plain` and is outdated in this point.

### REQ-FS-022 — Line ends written by Windows are normalized

Status: draft

Text written by Windows applications must be normalized to the line feed model of
the Atelier line array.

Acceptance criteria:
- A `\r\n` written by the caller is stored as a single `\n`.
- A lone `\r` is preserved as data and is not turned into a line break.
- The normalization applies to text documents only.

### REQ-FS-023 — The encoding of a write is chosen from the content

Status: draft

The decision whether a document is written as text or as binary must be taken from
the content of the buffer.

Acceptance criteria:
- A buffer that is valid UTF-8 and contains no NUL byte is written as text
  (`enc=false`).
- Any other buffer is written as binary (`enc=true`) with the bytes encoded as
  base64.
- The decision is taken from the buffer, not from the file name.

> Open point: overview §12.5 keeps open whether the detection is replaced by a
> list of binary extensions and what happens with a mixed text document. The
> requirement above implements the sniffing variant for v1.

### REQ-FS-024 — A document cannot be extended beyond its content

Status: draft

The API replaces a document as a whole. A request that would grow a document
beyond its current content must therefore be refused.

Acceptance criteria:
- A `SetFileSize` that enlarges the file fails with `STATUS_NOT_SUPPORTED`.
- A `SetFileSize` that shortens the file succeeds and truncates the buffer.
- A write past the end of the file is refused with `STATUS_NOT_SUPPORTED` unless
  the file was extended first, which is impossible in v1.

### REQ-FS-025 — The save is conditional on the hash of the opened document

Status: draft

A document must be saved under the condition that the server still holds the
version that was read when the handle was opened.

Acceptance criteria:
- The `PUT` carries the hash obtained from the `GET` of that document in the
  `If-None-Match` header.
- A `409` from the server fails the flush with `STATUS_FILE_LOCK_CONFLICT` and
  leaves the buffer dirty; the data is not discarded.
- The buffer content is kept, so the caller can retry after resolving the
  conflict.

### REQ-FS-026 — The server version is overwritten only on explicit request

Status: draft

The check of the previous requirement may be bypassed only when the user has
explicitly asked for it.

Acceptance criteria:
- By default the `PUT` carries the hash, and a conflict fails the write.
- With the "overwrite server version" option enabled, the `PUT` carries
  `ignoreConflict=1` and the server version is replaced.
- The option is off by default and is reported in the startup log.

> Open point: overview §12.4 keeps open whether the conflict policy is a
> per-mount option or a per-operation decision. The requirement above implements
> a per-mount option that is off by default.

### REQ-FS-027 — The cache is updated from the answer of the server

Status: draft

The answer of a successful save must become the current content of the document,
because the server may return content that differs from what was sent.

Acceptance criteria:
- After a successful `PUT`, the content and the timestamp of the document in the
  cache are the ones of the answer.
- Content that a source control hook changed on the server is the content that is
  served by later reads.
- The output of a hook (`console`) is logged and is never served as file content.

### REQ-FS-028 — A flush sends nothing when nothing was written

Status: draft

A flush must send a request to the server only when the buffer of the handle
differs from the content that was read.

Acceptance criteria:
- A handle that was read but not written issues no `PUT` on flush.
- A handle with a modified buffer issues exactly one `PUT` on flush.
- A flush of a dirty buffer that fails leaves the buffer dirty.

### REQ-FS-029 — A failed save is retried once at close and then dropped

Status: draft

When a save fails, the buffer of the handle stays dirty, the close of the handle
sends one last attempt, and data that is still not acknowledged at that point is
discarded without an error.

Acceptance criteria:
- After a failed `PUT` the buffer is still dirty.
- The close of the handle issues one final `PUT` with the buffer content.
- Data that is not acknowledged by that final attempt is discarded; the close
  succeeds and reports no error for the discarded data.
- The discard is silent: nothing is written to the log about it (REQ-MOUNT-023).

### REQ-FS-030 — Deleting a document is a delete of that document

Status: draft

Deleting a file must remove the document with the same name on the server.

Acceptance criteria:
- The close of a handle that was opened with delete-on-close issues
  `DELETE /:namespace/doc/<name>`.
- A successful delete removes the entry from the descriptor cache.
- A `DELETE` for a document that does not exist fails with
  `STATUS_OBJECT_NAME_NOT_FOUND`.

### REQ-FS-031 — A locked document cannot be deleted

Status: draft

A document that is locked on the server must not be deleted, and the caller must
be told that the document is locked.

Acceptance criteria:
- A `423` or `425` from the server fails the delete with
  `STATUS_FILE_LOCK_CONFLICT`.
- The document stays on the server.
- The error message names the document and the lock (overview §7).

### REQ-FS-032 — A rename is a read, a write and a delete

Status: draft

The API has no rename. A rename must therefore be composed of the three calls
that reproduce it.

Acceptance criteria:
- The rename of `A` to `B` issues `GET` of `A`, then `PUT` of `B`, then `DELETE`
  of `A`.
- The `PUT` carries the content of `A` and its `enc` flag.
- The `enc` flag and the content of the target are preserved; a binary document
  stays binary.
- The descriptor cache entries of both names are updated.

### REQ-FS-033 — A rename is not atomic

Status: draft

A rename must be documented as a sequence of three calls that can fail in the
middle, and the state it leaves behind must be visible to the caller.

Acceptance criteria:
- A failure between the `PUT` of the target and the `DELETE` of the source leaves
  both documents on the server.
- The caller receives an error for the failed rename.
- The cache entries of both names are invalidated, so that the duplicate is
  visible in the next listing.

### REQ-FS-034 — A rename follows the replacement rule of Windows

Status: draft

A rename onto an existing name must obey the replacement rule that the caller
selected.

Acceptance criteria:
- With the replacement allowed, the target document is overwritten by the `PUT`.
- With the replacement not allowed, the rename is refused with
  `STATUS_OBJECT_NAME_COLLISION` before any call is issued to the server.
- The refusal is issued before the `GET` of the source as well.

### REQ-FS-035 — A rename may change the category of a document

Status: draft

A rename may move a document into another category of the mounted namespace, and
it may not move it into another namespace.

Acceptance criteria:
- A rename into another category uses the category of the target in the `PUT` URL.
- A rename that would change the namespace is refused, because the namespace is
  fixed for the mount by `-ns`.
- A copy between two namespaces is a copy between two mounts.

### REQ-FS-036 — A directory cannot be renamed

Status: draft

The directories of the volume are synthetic. They cannot be renamed, moved or
deleted.

Acceptance criteria:
- A rename of a directory fails with `STATUS_NOT_SUPPORTED`.
- No call is issued to the server for such a rename.
- The rule applies to the category directories of the root and to every other
  synthetic directory.

### REQ-FS-037 — A directory cannot be created or deleted

Status: draft

The directories of the volume are synthetic and read-only. They cannot be created
or deleted.

Acceptance criteria:
- A create on a directory path fails with `STATUS_NOT_SUPPORTED`.
- A delete of a directory fails with `STATUS_NOT_SUPPORTED`.
- No call is issued to the server for such an attempt.

### REQ-FS-038 — Sharing and lock modes are enforced locally

Status: draft

The sharing and lock modes of Windows are enforced by the local file system
host. The API has no lock, so no call is issued to the server for a lock or for a
unlock.

Acceptance criteria:
- A second open that violates the sharing mode of the first open fails with the
  status of the local file system host.
- No request is issued to the server for a lock or for an unlock.
- A lock that the server holds on the document surfaces as `423` or `425` and is
  reported as `STATUS_FILE_LOCK_CONFLICT` (REQ-FS-031, REQ-FS-025).

### REQ-FS-039 — Extended attributes and alternate data streams — dropped

Status: dropped

Extended attributes and alternate data streams are intentionally unsupported in
v1. Rationale: the API exposes no place to store them, and the overview lists
them as out of scope (§11).

### REQ-FS-040 — Symbolic and hard links — dropped

Status: dropped

Symbolic links and hard links are intentionally unsupported in v1. Rationale: the
API has no link concept, and a link would need a representation of its own in the
document tree.

### REQ-FS-041 — Server-side file locking — dropped

Status: dropped

Locking a document for other clients is intentionally unsupported. Rationale: the
API has no lock call; the locks that the server holds itself are reported as
`423`/`425` and are not managed by ISFS (REQ-FS-038).

### REQ-FS-042 — Sparse files, compression and quotas — dropped

Status: dropped

Sparse files, compression and quotas are intentionally unsupported. Rationale: the
API has no matching capability, and the volume reports its size as unknown
(REQ-MOUNT-008).

### REQ-FS-043 — An atomic rename — dropped

Status: dropped

A rename that is atomic on the server is intentionally unsupported. Rationale: the
API has no rename call; the composed sequence of REQ-FS-032 cannot be made atomic
from the client.

### REQ-FS-044 — Compile on save — dropped

Status: dropped

Compiling a document after it was saved is intentionally unsupported in v1.
Rationale: the endpoint table of the overview classifies `compile` as a
control-plane call with no filesystem semantics (overview §4.2, §11); the
in-scope list of overview §1 still names a compile-on-save action, which
contradicts it.

> Open point: the contradiction between overview §1 and overview §4.2 has to be
> resolved before the requirement above can be closed. A compile on save would
> need its own requirements (when it runs, what a compile error means for the
> flush, how the result is reported to the caller).

## Open decisions affecting this area

From overview §12, to be resolved and folded into the requirements above:

- size strategy — lazy: 0 in listings and stat (REQ-FS-003), exact size for an
  open handle after the content was fetched (REQ-FS-006); batch prefetch is
  deferred together with the content cache.
- conflict policy — resolved for v1 as a per-mount option that is off by default
  (REQ-FS-026).
- binary detection — resolved for v1 as a content sniffing (REQ-FS-023); the
  extension list remains an open option.
- timestamp time zone — the `ts` is used as local time (REQ-FS-007); a
  configurable offset remains an open option.
- compile on save — dropped for v1, with a contradiction in the overview to
  resolve (REQ-FS-044).

Open: the exact status that a refused directory operation reports
(REQ-FS-036, REQ-FS-037) — `STATUS_NOT_SUPPORTED` or an access denied status.
The requirement above uses `STATUS_NOT_SUPPORTED`, because the operation is not
part of the file system model rather than forbidden by the server.

Resolved: a create reports the new file as present, with the content of its
local buffer, before the first flush (REQ-MAP-014).

Resolved: v1 collects no per-document stat data (REQ-FS-005 is dropped).
`GetSecurityByName` returns the synthesized full-access descriptor and the
attributes derived from the path; the size reported for a path is 0. The exact
size appears only for an open handle whose content was fetched (REQ-FS-006).

## Related documents

- Architecture: [`docs/architecture/overview.md`](../../architecture/overview.md)
- API reference: [`docs/architecture/api.md`](../../architecture/api.md)
- Requirements index: [`docs/requirements/README.md`](../README.md)
- Mount lifecycle: [`mount-lifecycle.md`](mount-lifecycle.md)
- Connection and authentication: [`connection-auth.md`](connection-auth.md)
