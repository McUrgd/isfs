# Mount / attach lifecycle — requirements

Status: draft.

Scope: attaching the ISFS volume to Windows, its identity and initial state, the mount-time
checks against the Atelier server, and detaching it again. Content semantics (paths, files,
metadata) are in `atelier-mapping.md` and `filesystem-operations.md`; how credentials are
presented to the server and session handling are in `connection-auth.md`; error/log formats
are in `errors-diagnostics.md`.

Source sections of [`docs/architecture/overview.md`](../../architecture/overview.md):
§2 (components), §3 (volume and path model), §6 (metadata synthesis), §9 (concurrency),
§10 (diagnostics), §12 (open decisions).

## Mount-time configuration

Inputs referenced by the requirements below. Names are indicative; the concrete
configuration surface (command line, config file, service parameters) is decided with
`connection-auth.md`.

| Setting | Required | Default | Used by |
|---|---|---|---|
| Mount point (drive letter `X:`) | yes | — | REQ-MOUNT-002, 003, 005, 022 |
| Atelier server address (`-p host:port`) | yes | — | REQ-MOUNT-009, 011 |
| Credentials: login (`-user`) | yes | — | REQ-MOUNT-010, 012 |
| Credentials: password (`-pass`) | yes | — | REQ-MOUNT-010, 012 |
| Connect/read timeout | no | 30 s | REQ-MOUNT-011 |
| Read-only mode | no | off | REQ-MOUNT-020 |
| Synthetic total/free size | no | configurable constant | REQ-MOUNT-008 |

## Requirements

### REQ-MOUNT-001 — The volume is served by the ISFS process

Status: approved

The mounted volume must be implemented inside the ISFS process (console host or WinFsp
service); no separate helper process may be required to serve file operations.

Acceptance criteria:
- Starting ISFS with a valid configuration makes the volume usable without launching any
  additional executable.
- Terminating the ISFS process removes the volume.

### REQ-MOUNT-002 — A mount point is mandatory

Status: approved

ISFS must refuse to start the file system when no mount point is supplied.

Acceptance criteria:
- Running ISFS without a mount point prints usage, creates no volume, and exits with a
  non-zero status.

### REQ-MOUNT-003 — Supported mount point form

Status: approved

The mount point must be a drive letter (`X:`) and nothing else; mounting onto a directory
path is not supported.

Acceptance criteria:
- With `-m X:`, drive `X:` shows the namespace root.
- A directory path such as `-m C:\mounts\isfs` is rejected with an actionable error
  (REQ-MOUNT-005).

### REQ-MOUNT-005 — Invalid mount points are rejected

Status: approved

ISFS must reject a mount point that cannot host a volume, without leaving a partially
mounted volume behind.

Acceptance criteria:
- A mount point that is not a single drive letter (a directory path, a UNC path, a file
  path) is rejected with an actionable error.
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

### REQ-MOUNT-008 — Synthetic volume size

Status: draft

Because the Atelier API exposes no quota, `GetVolumeInfo` must report configurable
synthetic total and free sizes.

Acceptance criteria:
- With configured values, Explorer and `fsutil volume diskfree X:` show those values.
- Changing the configuration changes the reported values without contacting the server.

### REQ-MOUNT-009 — Atelier server address is required

Status: draft

ISFS must require the address of the Atelier server, passed as the `-p host:port`
parameter of `isfs.exe`. The parameter carries the host and the web port only; the base URL
of the endpoint is composed by ISFS as `http://<host:port>/api/atelier/v1`. The address is
not validated ahead of the mount attempt: it is used as given.

Acceptance criteria:
- Running without `-p` prints usage, creates no volume, and exits with a non-zero status.
- With `-p <host:port>`, requests are issued to
  `http://<host:port>/api/atelier/v1/%SYS/namespaces` and
  `http://<host:port>/api/atelier/v1/:namespace`.
- A malformed or unreachable address is not rejected up front; the mount fails at the
  server probe with an actionable error naming the endpoint (REQ-MOUNT-011).

> Open point: because `-p` carries no scheme, the composed base URL is plain `http`. How TLS
> is selected (a separate switch, a scheme-tolerant `-p`, or a global setting) is decided
> with `connection-auth.md` before TLS-only deployments are supported.

### REQ-MOUNT-010 — Credentials come from launch parameters and are held in memory only

Status: draft

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

### REQ-MOUNT-011 — Fail fast when the server is unreachable

Status: draft

Before exposing the volume, ISFS must probe the server, and a connection failure, DNS/TLS
error, or timeout must abort the mount instead of mounting an unusable volume.

Acceptance criteria:
- With the host unreachable, mount fails within the configured connect timeout.
- The error message identifies the endpoint and the failure kind.
- No drive letter remains after the failure.

### REQ-MOUNT-012 — Mount authentication failure

Status: draft

When the server rejects the credentials during mount, ISFS must fail the mount and report a
logon failure rather than expose the volume.

Acceptance criteria:
- A `401` response during the mount probe aborts the mount with a message naming the
  credential problem.
- The mount result is `STATUS_LOGON_FAILURE` in the service log.
- No volume is visible after the failure.

### REQ-MOUNT-013 — Namespace discovery at mount

Status: draft

At mount time, ISFS must obtain the namespace list from `GET /%SYS/namespaces`.

Acceptance criteria:
- The namespaces returned by the endpoint appear as directories under the volume root.
- The list is requested once per mount (subject to the namespace cache TTL) and not per
  directory listing.
- When the endpoint is unavailable (`404`/`500`) the mount fails with an actionable error
  and no volume remains.
- An empty result (`200` with no namespaces) is not an error: the mount succeeds and the
  volume root is empty (REQ-MOUNT-018).

### REQ-MOUNT-015 — Namespace validation at mount

Status: draft

At mount time, ISFS must validate every namespace it is about to expose with
`GET /:namespace`, and must omit namespaces that cannot be accessed.

Acceptance criteria:
- A namespace returning `404` or `403` is not exposed and a warning naming it is logged.
- A namespace returning `200` is exposed with the server's canonical (uppercased) name.
- The mount succeeds even when namespaces are omitted; with none left exposed the root is
  empty (REQ-MOUNT-018).

### REQ-MOUNT-018 — Root directory contents

Status: draft

The volume root must contain only the exposed namespaces as directories.

Acceptance criteria:
- Enumerating `\` returns one directory per exposed namespace and no files.
- The entries use the canonical server casing.
- When no namespace is exposed (empty discovery result, or every namespace omitted by
  REQ-MOUNT-015), enumerating `\` returns no entries and no error, so the drive appears as an
  empty disk that can still be opened and browsed.

### REQ-MOUNT-019 — Case-insensitive path resolution

Status: draft

Name lookup through the mounted volume must be case-insensitive while preserving the
server's canonical casing in listings.

Acceptance criteria:
- `\user\CLS\person.cls` resolves to `\USER\CLS\User.Person.cls`.
- Enumeration shows `USER` and `User.Person.cls`, not the case used by the caller.

### REQ-MOUNT-020 — Read-only mount

Status: draft

ISFS must support a mount-time read-only mode in which all mutating operations are refused.

Acceptance criteria:
- In read-only mode, create, write, rename, and delete requests fail with
  `STATUS_MEDIA_WRITE_PROTECTED` (or the equivalent access-denied status chosen with
  `errors-diagnostics.md`).
- Reads and directory listings behave exactly as in read-write mode.
- Read-only mode is reported in the startup configuration log (overview §10).

### REQ-MOUNT-021 — Actionable mount failure diagnostics

Status: draft

Every mount failure must produce an actionable message that identifies the endpoint and the
cause (authentication, DNS, TLS, timeout, namespace discovery) in the service log or mount
dialog.

Acceptance criteria:
- For each failure mode in REQ-MOUNT-011, 012, and 013, the log line names the endpoint and
  the failure kind.
- No mount failure terminates silently without a message.

### REQ-MOUNT-022 — Unmount releases the mount point

Status: draft

Stopping the mount must release the drive letter and make the volume unavailable.

Acceptance criteria:
- After a stop, the drive letter is gone and further access fails with a normal Windows
  error.
- A repeated stop is harmless and does not fail.

### REQ-MOUNT-023 — Unmount discards unsaved data

Status: draft

Unmounting must not persist unsaved changes. Data buffered in open handles that has not been
acknowledged by a successful flush is discarded, and unmount must not attempt to flush it
(overview §9).

Acceptance criteria:
- With a handle holding a dirty buffer, unmount issues no `PUT` for that document.
- After unmount, `GET` returns the content of the last successful flush, or the document as
  it was before the write when nothing was flushed.
- Discarding pending data does not fail the unmount (REQ-MOUNT-022) and leaves no trace in
  the log: the loss is fully silent.

### REQ-MOUNT-024 — Acknowledged writes survive unmount

Status: draft

Writes already acknowledged by a successful flush must be present on the server before the
unmount completes.

Acceptance criteria:
- Write, flush, unmount, then `GET` the document: the content matches the written content.
- No acknowledged write depends on the ISFS process staying alive.

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

## Open decisions affecting this area

From overview §12, to be resolved and folded into the requirements above:

- read-only namespace policy (REQ-MOUNT-020);
- synthetic volume size defaults (REQ-MOUNT-008).

Resolved: authentication uses a login and password passed as the `-user` and `-pass`
parameters of `isfs.exe`, kept in memory only (REQ-MOUNT-010).

Resolved: the Atelier server address is passed as `-p <host:port>`; ISFS composes
`http://<host:port>/api/atelier/v1` from it (REQ-MOUNT-009). TLS selection is still open.

Resolved: namespace discovery has no fallback. When `GET /%SYS/namespaces` is unavailable,
the mount fails with an actionable error and no volume remains (REQ-MOUNT-013).

## Related documents

- Architecture: [`docs/architecture/overview.md`](../../architecture/overview.md)
- API reference: [`docs/architecture/api.md`](../../architecture/api.md)
- Requirements index: [`docs/requirements/README.md`](../README.md)
