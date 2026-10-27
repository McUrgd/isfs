# Connection and authentication — requirements

Status: implemented.

Scope: how ISFS reaches the Atelier server and how it authenticates there — the
scheme and the endpoint URL, the HTTP transport, the presentation of the
credentials taken from the command line, the lifetime of the connection and the
mapping of connection failures. The mount lifecycle itself (when the probe runs,
what a failed mount looks like) is in `mount-lifecycle.md`; file and directory
semantics are in `filesystem-operations.md` and `atelier-mapping.md`; the log
and error message formats are in `errors-diagnostics.md`.

Source sections of [`docs/architecture/overview.md`](../../architecture/overview.md):
§2 (components), §7 (error mapping), §9 (concurrency), §10 (diagnostics),
§12 (open decisions).
Source sections of [`docs/architecture/api.md`](../../architecture/api.md):
§2 (base URL, transport and authentication), §10 (HTTP status summary).

## Decisions taken in this area

Two decisions are binding for every requirement below and for the Atelier
client that implements them.

1. **The connection scheme is always `http`.** ISFS never issues an `https`
   request. The scheme is not configurable and there is no switch for it
   (REQ-CONN-001, REQ-CONN-002).
2. **Every request carries HTTP Basic credentials** built from the `-user` and
   `-pass` launch parameters. The credential is presented per request; ISFS
   performs no login and keeps no session state (REQ-CONN-006,
   REQ-CONN-009).

These decisions close the open points that
[`mount-lifecycle.md`](mount-lifecycle.md) deliberately deferred to this file:
the TLS selection noted in REQ-MOUNT-009 and the configuration surface noted
in its "Mount-time configuration" section.

## Connection configuration

| Setting | Required | Default | Used by |
|---|---|---|---|
| Atelier server address (`-p host:port`) | yes | — | REQ-MOUNT-009, REQ-CONN-003 |
| Namespace (`-ns <namespace>`) | yes | — | REQ-MOUNT-009, REQ-CONN-004 |
| Scheme | no | `http`, fixed | REQ-CONN-001, REQ-CONN-002 |
| Login (`-user`) | yes | — | REQ-MOUNT-010, REQ-CONN-006 |
| Password (`-pass`) | yes | — | REQ-MOUNT-010, REQ-CONN-006 |
| Authentication scheme | no | HTTP Basic, fixed | REQ-CONN-006, REQ-CONN-009 |
| Connect/send/receive timeout | no | 30 s | REQ-CONN-005 |
| Proxy | no | none, fixed | REQ-CONN-019 |

## Requirements

### REQ-CONN-001 — The connection scheme is always `http`

Status: implemented

ISFS must compose every Atelier URL with the `http` scheme and must never issue
an `https` request.

Acceptance criteria:
- The composed base URL starts with `http://` (for example
  `http://server:57772/api/atelier/v1/USER`).
- No request is sent with the secure transport flag; the flag is never set
  because the scheme is always `http`.
- There is no configuration switch, command line option or environment
  variable that changes the scheme.

### REQ-CONN-002 — TLS support — dropped

Status: dropped

Connecting to the Atelier web port over TLS is intentionally unsupported in
v1. Rationale: the scheme is fixed to `http` (REQ-CONN-001); a deployment that
requires TLS is served by a TLS-terminating proxy in front of the web gateway,
and the client-side certificate handling it would need is out of scope.

### REQ-CONN-003 — All requests use the mount-time base URL

Status: implemented

Every request issued by the mounted volume must target the base URL composed
at mount time from `-p` and `-ns`; no request may address another host, port
or path.

Acceptance criteria:
- The mount probe and every subsequent request target
  `http://<host:port>/api/atelier/v1/<namespace>`.
- No request is issued to a host, a port or a path other than the composed
  base URL and its subpaths.
- The namespace segment is not re-cased on the way out; the server's
  canonical form is used only in responses.

### REQ-CONN-004 — The namespace segment is percent-encoded

Status: implemented

The `-ns` value must be percent-encoded when it is placed in the URL path
segment, because a literal `%` is not a valid URL escape.

Acceptance criteria:
- `-ns USER` produces the path segment `USER`.
- `-ns %SYS` produces the path segment `%25SYS`, so the request path is
  `http://<host:port>/api/atelier/v1/%25SYS`.
- No other character of the namespace value is altered.

### REQ-CONN-005 — Configurable timeouts — dropped

Status: dropped

Separate configurable connect, send, receive and resolve timeouts are
intentionally unsupported in v1: every request is bounded by one fixed
30-second value.

Rationale: the mount has no configuration surface beyond the command line
(REQ-CONN-018) and no v1 consumer needs a per-mount timeout; the fixed value is
enough to fail a hanging request with `STATUS_IO_TIMEOUT`.

> Implemented as one fixed value: `WinHttpSetTimeouts` is called once when the
> client session is opened (`isfs/atelier.c`), so the limit covers the whole
> request, not only the first byte.

### REQ-CONN-006 — Every request carries HTTP Basic credentials

Status: implemented

Every request issued against the Atelier server must carry an HTTP Basic
`Authorization` header built from the `-user` and `-pass` launch parameters.
No request may be sent without credentials, and no request may use another
authentication scheme.

Acceptance criteria:
- The mount probe and every document request carry an `Authorization` header
  of the form `Basic <base64>`.
- The header is present on every request, not only on the first one and not
  only on the probe.
- The credentials used are exactly the `-user` and `-pass` values given at
  launch; they are not re-read, re-derived or changed while the volume is
  mounted.

### REQ-CONN-007 — The Basic credential is base64 of UTF-8 `user:password`

Status: implemented

The `Authorization` header must be built as described by RFC 7617: the
concatenation `login + ":" + password` is encoded as UTF-8 and then base64
encoded.

Acceptance criteria:
- For `-user SYSTEM -pass secret`, the header is
  `Authorization: Basic <base64 of "SYSTEM:secret">`.
- A login or password containing non-ASCII characters is encoded as UTF-8
  before base64, not in the system code page.
- The separator between login and password is a single colon; a colon inside
  the password is not escaped and not rejected.

### REQ-CONN-008 — The credentials travel only in the `Authorization` header

Status: implemented

The login and the password must appear only in the `Authorization` header of a
request.

Acceptance criteria:
- No request URL contains the login or the password.
- No query parameter, request body or custom header carries the credential.
- The credential is not placed in the `Proxy-Authorization` header.

### REQ-CONN-009 — There is no session: the credential is presented per request

Status: implemented

ISFS must not create, keep or refresh a server-side session. The Atelier
export exposes no login or logout route (api.md §2), so the credential is
presented on every request and the server decides on every call whether it is
still valid.

Acceptance criteria:
- No login or logout route is called at mount or during operation.
- Two consecutive requests of the same session carry the same
  `Authorization` header; the credential is not exchanged for a token.
- ISFS keeps no cookie jar and no session identifier between requests.

### REQ-CONN-010 — The credentials are never written to a log

Status: implemented

Neither the login nor the password may appear in any log output, debug output
or diagnostic dump produced by ISFS.

Acceptance criteria:
- No line of the service log, the debug log or the WinFsp debug log contains
  the login or the password.
- The `Authorization` header is never logged, neither in full nor in part
  (the base64 payload is not logged either).
- A request trace may name the scheme (`Basic`) but not the credential.

### REQ-CONN-011 — The credentials are wiped when the volume is destroyed

Status: implemented

The login and the password held in memory must be overwritten before the memory
that holds them is released, when the volume is unmounted or the mount fails.

Acceptance criteria:
- After the volume is unmounted, the memory that held the password contains
  zeros instead of the credential.
- After the volume is unmounted, the memory that held the login contains zeros
  instead of the credential.
- The wipe happens on the mount-failure path as well as on the normal unmount
  path.

> The login is not a secret in the same sense as the password, but leaving
> either credential in freed memory serves no purpose.
>
> Implementation: both the login and the password are overwritten with
> `SecureZeroMemory` before they are freed, on the normal unmount path and on
> the mount-failure path.

### REQ-CONN-012 — The login is wiped separately — dropped

Status: dropped

Wiping the login as a requirement of its own is intentionally dropped: it was
merged into REQ-CONN-011, which now covers the login and the password as one
behaviour. Rationale: one behaviour per requirement, and the ID of a merged
requirement is not reused.

### REQ-CONN-013 — The credential has exactly one source

Status: implemented

The login and the password must be taken only from the `-user` and `-pass`
launch parameters and must not be read from the environment, from a
configuration file, from the Windows Credential Manager, from the registry or
from the standard input.

Acceptance criteria:
- A mount without `-user` or without `-pass` fails with a usage message and a
  non-zero exit status (REQ-MOUNT-010).
- Setting the credentials in the environment has no effect on the request.

### REQ-CONN-014 — A `401` during the mount probe aborts the mount

Status: implemented

A `401` response to the mount probe must fail the mount with a logon failure
and must not expose the volume.

Acceptance criteria:
- The mount ends with `STATUS_LOGON_FAILURE` and a message that names the
  credential problem and the endpoint.
- No drive letter is visible after the failure.

> Cross-reference: REQ-MOUNT-012 states the same behaviour from the mount
> lifecycle side.

### REQ-CONN-015 — A `401` during an operation fails the operation only

Status: implemented

A `401` response to a request that is issued while the volume is mounted must
fail that operation with `STATUS_ACCESS_DENIED`. ISFS must not attempt to
re-authenticate, must not try other credentials and must not unmount the
volume.

Acceptance criteria:
- The operation that received the `401` fails with `STATUS_ACCESS_DENIED`.
- No second request is issued with a different credential.
- The volume stays mounted and the drive letter remains visible.

### REQ-CONN-016 — A `403` fails the operation with access denied

Status: implemented

A `403` response must be mapped to `STATUS_ACCESS_DENIED`, both during the
mount probe and during an operation.

Acceptance criteria:
- The mount probe maps `403` to `STATUS_ACCESS_DENIED` and aborts the mount.
- An operation that receives `403` fails with `STATUS_ACCESS_DENIED`.
- The message states that the account may not use the namespace or the
  document.

### REQ-CONN-017 — A client-side list of namespace rights — dropped

Status: dropped

Maintaining a client-side list of readable or read-only namespaces is
intentionally unsupported. Rationale: the namespace rights are enforced by the
server (overview §12.11); a client-side list would duplicate the server's ACL
model, could contradict it after a change on the server, and would give a
false sense of protection.

### REQ-CONN-018 — Other credential sources — dropped

Status: dropped

Reading the endpoint or the credential from a configuration file, from the
service parameters or from an interactive prompt is intentionally unsupported
in v1. Rationale: the endpoint and the credential are launch parameters
(REQ-MOUNT-010, REQ-CONN-013); a file or a prompt would need its own secret
protection, which v1 does not provide. The exposure of `-pass` in the process
command line is recorded as a known limitation in REQ-MOUNT-010.

### REQ-CONN-019 — Proxy support — dropped

Status: dropped

Routing the Atelier requests through an HTTP proxy is intentionally
unsupported. Rationale: the client connects directly to the Atelier web port
of the addressed server; a deployment that needs a proxy is expected to expose
the web port through it, and the proxy's own authentication would add a second
credential to the mount.

### REQ-CONN-020 — The connection is established during the mount probe

Status: implemented

The connection to the server must be established while the volume is mounted,
and the mount must fail when the server cannot be reached. A volume is never
mounted against a server that has not answered.

Acceptance criteria:
- The probe completes before the drive letter becomes visible.
- A DNS failure, a refused connection or a timeout aborts the mount with
  `STATUS_NETWORK_UNREACHABLE` or `STATUS_IO_TIMEOUT` and an actionable
  message (REQ-MOUNT-021).
- The mount leaves no partially mounted volume behind.

### REQ-CONN-021 — One HTTP client session per mount

Status: implemented

The mounted volume must own a single HTTP client session that is created at
mount time and used for all requests, and it must be closed when the volume is
unmounted.

Acceptance criteria:
- The client session is created once during the mount, not once per request.
- The session is closed on unmount and on the mount-failure path.
- No request is issued on a session that belongs to another mount.

### REQ-CONN-022 — The connection is kept alive between requests

Status: implemented

The HTTP connection must be reused between requests, and a connection that was
closed by the peer must be re-established transparently before the next
request.

Acceptance criteria:
- Two consecutive requests of one operation reuse the same connection when
  the server keeps it open.
- A request whose connection was closed by the server succeeds after the
  client re-establishes the connection.
- The re-establishment is not visible to the caller: the operation succeeds.

### REQ-CONN-023 — The startup log names the authentication scheme — dropped

Status: dropped

Reporting the authentication scheme and the canonical form of the namespace in
the startup log is intentionally unsupported in v1.

Rationale: the startup log already names the base URL, which carries the host,
the port, the API path, the `-ns` namespace and the `http` scheme; the
authentication scheme is a fixed constant (REQ-CONN-006) and the namespace is
not canonicalized before the log line is written, so a separate record would
restate values that cannot be configured.

> Implemented part: the startup log contains the base URL, the namespace and the
> conflict policy, and never the login or the password (REQ-CONN-010).

### REQ-CONN-024 — Transport failures map to a fixed set of NTSTATUS values

Status: implemented

Every transport failure must be mapped to one of a fixed set of NTSTATUS
values: `STATUS_NETWORK_UNREACHABLE` for a name that cannot be resolved, a
refused connection or a generic transport error, `STATUS_IO_TIMEOUT` for a
timeout, and `STATUS_INVALID_NETWORK_RESPONSE` for a failed TLS negotiation.

Acceptance criteria:
- A host name that does not resolve fails the request with
  `STATUS_NETWORK_UNREACHABLE`.
- A connection refused by the host fails the request with
  `STATUS_NETWORK_UNREACHABLE`.
- A server that does not answer within the timeout fails the request with
  `STATUS_IO_TIMEOUT`.
- The mapping is the same for the mount probe and for a request issued while
  the volume is mounted.

### REQ-CONN-025 — A post-mount failure keeps the volume mounted

Status: implemented

A connection failure that occurs while the volume is mounted must fail the
affected operation only. ISFS must not unmount the volume, must not release
the drive letter and must not exit the process.

Acceptance criteria:
- The operation that failed reports the mapped status of REQ-CONN-024.
- The drive letter is still visible after the failure and later operations
  are attempted again.
- The failure is logged with the endpoint and the failure kind.

### REQ-CONN-026 — Automatic retry of a failed request — dropped

Status: dropped

Retrying a failed request automatically is intentionally unsupported.
Rationale: `PUT` is not idempotent for ISFS — a request that failed after the
server applied it but before the answer arrived would write the document a
second time; the caller decides when to retry, and the conditional write
(overview §4.4, `filesystem-operations.md`) turns the second attempt into a
conflict instead of a silent overwrite.

## Open decisions affecting this area

Resolved: the scheme is always `http` and TLS is not supported
(REQ-CONN-001, REQ-CONN-002). This closes the open point of REQ-MOUNT-009.

Resolved: every request carries HTTP Basic credentials taken from `-user` and
`-pass`, and no session is created (REQ-CONN-006, REQ-CONN-009). This closes
the open point 10 of overview §12.

Resolved: the configuration surface is the command line only
(REQ-CONN-018).

Resolved: namespace rights are decided by the server, ISFS keeps no list of
them (REQ-CONN-017).

Resolved: the connect, send and receive timeouts are a single fixed value of
30 seconds and are not configurable in v1 (REQ-CONN-005 is dropped).

## Related documents

- Architecture: [`docs/architecture/overview.md`](../../architecture/overview.md)
- API reference: [`docs/architecture/api.md`](../../architecture/api.md)
- Requirements index: [`docs/requirements/README.md`](../README.md)
- Mount lifecycle: [`mount-lifecycle.md`](mount-lifecycle.md)
