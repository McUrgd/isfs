# Mount / attach lifecycle — requirements

Status: implemented.

Scope: attaching the ISFS volume to Windows, its identity and initial state, the mount-time
checks against the Atelier server, and detaching it again. Content semantics (paths, files,
metadata) are in `atelier-mapping.md` and `filesystem-operations.md`; how credentials are
presented to the server and session handling are in `connection-auth.md`; error/log formats
are covered by the planned `errors-diagnostics.md`.

Source sections of [`docs/architecture/overview.md`](../../architecture/overview.md):
§2 (components), §3 (volume and path model), §6 (metadata synthesis), §9 (concurrency),
§10 (diagnostics), §12 (resolved design decisions).

## Mount-time configuration

Inputs referenced by the requirements below. Names are indicative; the concrete
configuration surface is the command line of `isfs.exe` — a configuration file,
service parameters and an interactive prompt are not supported in v1
([`connection-auth.md`](connection-auth.md), REQ-CONN-018).

| Setting | Required | Default | Used by |
|---|---|---|---|
| Mount point (drive letter `X:`) | yes | — | REQ-MOUNT-002, 003, 005, 022 |
| Atelier server address (`-p host:port`) | yes | — | REQ-MOUNT-009 |
| Namespace (`-ns <namespace>`) | yes | — | REQ-MOUNT-009 |
| Credentials: login (`-user`) | yes | — | REQ-MOUNT-010, 012 |
| Credentials: password (`-pass`) | yes | — | REQ-MOUNT-010, 012 |
| Connect/read timeout | no | 30 s | REQ-MOUNT-021 |

## Requirements

### REQ-MOUNT-001 — The volume is served by the ISFS process

Status: implemented

The mounted volume must be implemented inside the ISFS process (console host or WinFsp
service); no separate helper process may be required to serve file operations.

Acceptance criteria:
- Starting ISFS with a valid configuration mounts the volume without launching any
  additional executable.
- Terminating the ISFS process removes the volume.
- No child or helper process is created to serve file operations: the volume is backed by
  the ISFS process itself, plus the WinFsp driver and in-process `winfsp-x64.dll`.

Whether the mounted volume can actually be opened and browsed is a separate requirement
(REQ-MOUNT-029).

### REQ-MOUNT-002 — A mount point is mandatory

Status: implemented

ISFS must refuse to start the file system when no mount point is supplied.

Acceptance criteria:
- Running ISFS without a mount point prints usage, creates no volume, and exits with a
  non-zero status.

### REQ-MOUNT-003 — Supported mount point form

Status: implemented

The mount point must be a drive letter (`X:`) ; mounting onto a directory path is not supported.

Acceptance criteria:
- With `-m X:`, drive `X:` shows the namespace root.
- A directory path such as `-m C:\mounts\isfs` is rejected with an actionable error
  (REQ-MOUNT-005).

### REQ-MOUNT-005 — Invalid mount points are rejected

Status: implemented

ISFS must reject a mount point that cannot host a volume, without leaving a partially
mounted volume behind.

Acceptance criteria:
- A mount point that is not a single drive letter is rejected with an actionable error.
- A drive letter already in use is rejected with an actionable error.
- After a rejected mount, no new drive letter is visible and the process exits non-zero.

### REQ-MOUNT-006 — Volume label

Status: implemented

The mounted volume must report the label `ISFS`.

Acceptance criteria:
- `vol X:` shows `ISFS` as the volume label.
- The label is reported through `GetVolumeInfo` and does not depend on the server.

### REQ-MOUNT-007 — File system name

Status: implemented

The mounted volume must report the file system name `ISFS` to Windows.

Acceptance criteria:
- Explorer volume properties show `ISFS` as the file system.
- The name comes from the volume parameters and is stable across mounts.

### REQ-MOUNT-008 — Volume size is reported as a synthetic capacity

Status: implemented

Because the Atelier API exposes no quota and the volume has no local backing store,
`GetVolumeInfo` must report a constant synthetic capacity so that Windows and
applications that pre-check free space do not treat the volume as full. It must report
`TotalSize = 1 TiB` and `FreeSize = 1 TiB`. There is no configuration for the values.

Acceptance criteria:
- Explorer volume properties and `fsutil volume diskfree X:` show 1 TiB total and 1 TiB
  free.
- Used space (total minus free) is 0.
- The values are constant: they do not depend on the server, on the mount configuration, or
  on the volume contents.
- No mount setting changes the reported values.

> The values are not a real quota: the server does not enforce them, and a write that fits
> the real server capacity succeeds even when it would exceed the synthetic numbers. They
> exist only to keep Windows and pre-checking applications from refusing to write.

### REQ-MOUNT-009 — Atelier server address and namespace are required

Status: implemented

ISFS must require the address of the Atelier server, passed as the `-p host:port`
parameter of `isfs.exe`. The parameter carries the host and the web port only; the base URL
of the endpoint is composed by ISFS as `http://<host:port>/api/atelier/v1`. The address is
not validated ahead of the mount attempt: it is used as given.

ISFS must also require the namespace, passed as the `-ns <namespace>` parameter of
`isfs.exe`. Its value is substituted for `:namespace` in every Atelier URL. The namespace is
not discovered and is not part of the volume path: it becomes the root of the mounted
volume. Names are case-insensitive and the server's canonical (uppercased) form is used in
responses and listings.

Acceptance criteria:
- Running without `-p` prints usage, creates no volume, and exits with a non-zero status.
- Running without `-ns` prints usage, creates no volume, and exits with a non-zero status
  (same behaviour as a missing mount point, REQ-MOUNT-002).
- With `-p <host:port>` and `-ns <namespace>`, requests are issued to
  `http://<host:port>/api/atelier/v1/<namespace>` and its subpaths; `<namespace>` is exactly
  the `-ns` value.
- A malformed or unreachable address is not rejected up front; the mount fails at the
  server probe with an actionable error naming the endpoint (REQ-MOUNT-021).
- The `-ns` value is not exposed as a directory: the volume root represents the namespace
  itself.

> Resolved: the scheme is always `http` and TLS is not supported in v1. The decision and
> its rationale are in [`connection-auth.md`](connection-auth.md) (REQ-CONN-001,
> REQ-CONN-002).

### REQ-MOUNT-010 — Credentials come from launch parameters and are held in memory only

Status: implemented

ISFS must take the login and the password from the `-user` and `-pass` parameters of
`isfs.exe` and keep them in memory for the lifetime of the mount. They must never be written
to logs, debug output, or any file.

Acceptance criteria:
- `isfs.exe -user <login> -pass <password> -p <host:port> -m <mount point>` mounts the volume
  using exactly those credentials.
- Running without `-user` or without `-pass` prints usage, creates no volume, and exits with
  a non-zero status (same behaviour as a missing mount point, REQ-MOUNT-002).
- The credentials are not read from the environment, from a configuration file, or from
  Windows Credential Manager.
- No log or debug output produced during mount or normal operation contains the password.
- The password is not present in any file created by ISFS.

> Known limitation: a password passed as `-pass` is visible to anyone who can read the
> process command line (Task Manager, WMI). The requirement above forbids logging and files
> written by ISFS, not that exposure; a later version may add a protected configuration file
> or an interactive prompt.

### REQ-MOUNT-012 — Mount authentication failure

Status: implemented

When the server rejects the credentials during mount, ISFS must fail the mount and report a
logon failure rather than expose the volume.

The probe authenticates with HTTP Basic: the `Authorization` header is built from
`-user`/`-pass` in UTF-8 for the duration of the request only and is wiped afterwards.

Acceptance criteria:
- A `401` response during the mount probe aborts the mount with a message naming the
  credential problem.
- The mount result is `STATUS_LOGON_FAILURE` in the service log.
- No volume is visible after the failure.

### REQ-MOUNT-019 — Case-insensitive path resolution

Status: implemented

Name lookup through the mounted volume must be case-insensitive while preserving the
server's canonical casing in listings.

Acceptance criteria:
- `\cls\user\person.cls` resolves to `\CLS\User\Person.cls`.
- Enumeration shows `CLS`, `User` and `Person.cls`, not the case used by the caller.

### REQ-MOUNT-020 — Read-only mount — dropped

Status: dropped

A mount-time read-only mode is intentionally unsupported in v1: the mount is always
read-write. Rationale: the write right is enforced by the server, which answers `403` for a
namespace or a document the account may not modify, and that answer already fails the
operation with `STATUS_ACCESS_DENIED` (REQ-CONN-016); a client-side switch would duplicate
the server's ACL model, could contradict it, and would give a false sense of protection
(REQ-CONN-017), with no v1 consumer that needs it.

### REQ-MOUNT-021 — Actionable mount failure diagnostics

Status: implemented

Every mount failure must produce an actionable message that identifies the endpoint and the
cause (authentication, DNS, TLS, timeout, invalid namespace) in the service log or mount
dialog.

Acceptance criteria:
- For each failure mode in REQ-MOUNT-012, the log line names the endpoint and
  the failure kind.
- No mount failure terminates silently without a message.

### REQ-MOUNT-022 — Unmount releases the mount point

Status: implemented

Stopping the mount must release the drive letter and make the volume unavailable.

Acceptance criteria:
- After a stop, the drive letter is gone and further access fails with a normal Windows
  error.
- A repeated stop is harmless and does not fail.

### REQ-MOUNT-023 — Unmount discards unsaved data

Status: implemented

Unmounting must not persist unsaved changes. Data buffered in open handles that has not been
acknowledged by a successful flush is discarded, and unmount must not attempt to flush it
(overview §9).

Acceptance criteria:
- With a handle holding a dirty buffer, unmount issues no `PUT` for that document.
- After unmount, `GET` returns the content of the last successful flush, or the document as
  it was before the write when nothing was flushed.
- Discarding pending data does not fail the unmount (REQ-MOUNT-022) and leaves no trace in
  the log: the loss is fully silent.

> `GET` after the unmount returns whatever the server holds, because ISFS keeps no local
> copy of a document once the handle is gone. The mount neither sends nor reverts anything
> at unmount; the durability of a version the server already accepted is outside its
> control (REQ-MOUNT-024).

### REQ-MOUNT-024 — Acknowledged writes survive unmount — dropped

Status: dropped

A promise that an acknowledged write is present on the server after the unmount is
intentionally unsupported: ISFS does not control the behaviour of the remote server.
Rationale: a successful flush only records that the server accepted the `PUT` and answered
`2xx` (REQ-FS-027); ISFS keeps no local copy of the content, so a rollback, a restore, a
journal replay or a server-side change may replace or discard the accepted version
afterwards, and no client-side promise can prevent that. Unmount itself neither sends nor
reverts anything (REQ-MOUNT-023).

### REQ-MOUNT-027 — Offline mount — dropped

Status: dropped

Mounting and serving the volume while the server is unreachable is intentionally
unsupported: the server is the source of truth and there is no local working copy
(overview §1). Rationale: an offline mount would require a persistent cache and conflict
handling, which are out of scope for v1.

### REQ-MOUNT-028 — Mount as a network share (UNC) — dropped

Status: dropped

Exposing the volume as a network share (`\\Server\Share`) is intentionally unsupported for
v1. Rationale: the requirement is a local Windows drive; a network volume adds share
naming, access control, and gateway concerns with no v1 consumer.

### REQ-MOUNT-029 — The mounted volume is usable

Status: implemented

Once the mount has succeeded, the volume must be openable and browsable through normal
Windows file access. What the volume contains and how names resolve is defined elsewhere
(REQ-MAP-001, REQ-MOUNT-019, `atelier-mapping.md`, `filesystem-operations.md`).

Acceptance criteria:
- `dir X:\` lists the root described by REQ-MAP-001 and completes without an error.
- Explorer opens `X:\` and browses at least one subdirectory without an error prompt.
- A volume whose root cannot be opened is treated as a failed mount: it is not left
  mounted and the process exits with a non-zero status and a log message (REQ-MOUNT-021).

> Note: this requirement is what REQ-MOUNT-001 deliberately does not cover. The volume
> source is the Atelier mapping, so the acceptance criteria above are verified together
> with `atelier-mapping.md` and `filesystem-operations.md`; the manual checks in
> `scripts/delete-test.ps1`, `scripts/rename-test.ps1` and `scripts/unmount-test.ps1`
> mount the volume against the mock server and open, browse and modify documents
> through it.

## Decisions affecting this area

Dropped: a mount-wide read-only mode is not supported in v1; write protection comes from
the server, which answers `403` (REQ-MOUNT-020, REQ-CONN-016).

Resolved: authentication uses a login and password passed as the `-user` and `-pass`
parameters of `isfs.exe`, kept in memory only (REQ-MOUNT-010).

Resolved: the volume reports a constant synthetic capacity (`TotalSize = FreeSize = 1 TiB`)
so that Windows and applications that pre-check free space can write; the values are not
configurable (REQ-MOUNT-008).

Resolved: the Atelier server address is passed as `-p <host:port>`; ISFS composes
`http://<host:port>/api/atelier/v1` from it (REQ-MOUNT-009). The scheme is always `http`
and TLS is not supported in v1 (`connection-auth.md`, REQ-CONN-001, REQ-CONN-002).

Resolved: the mount probe uses HTTP Basic credentials, and a `401` aborts the mount
(REQ-MOUNT-012).

Resolved: namespace rights are decided by the server; ISFS keeps no list of readable or
read-only namespaces (`connection-auth.md`, REQ-CONN-017).

## Related documents

- Architecture: [`docs/architecture/overview.md`](../../architecture/overview.md)
- API reference: [`docs/architecture/api.md`](../../architecture/api.md)
- Requirements index: [`docs/requirements/README.md`](../README.md)
- Connection and authentication: [`connection-auth.md`](connection-auth.md)
