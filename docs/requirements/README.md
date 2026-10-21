# Requirements — Index

Entry point for the requirements of **ISFS** — a Windows filesystem that exposes
the InterSystems Atelier v1 API as a drive.

Detailed requirements live in `docs/requirements/functional/`, one file per area.
This index is the single place that lists them; keep it in sync when adding or
retiring a requirement file.

## Functional requirements

| Area | File | Status |
|---|---|---|
| Mount / attach lifecycle (drive letter, connect, disconnect) | [`functional/mount-lifecycle.md`](functional/mount-lifecycle.md) | draft |
| Filesystem operations (read, write, create, delete, rename, directory listing) | [`functional/filesystem-operations.md`](functional/filesystem-operations.md) | draft |
| Mapping of Atelier resources (packages, directories) to filesystem paths | [`functional/atelier-mapping.md`](functional/atelier-mapping.md) | draft |
| Connection and authentication (server, credentials, session handling) | `functional/connection-auth.md` | draft |
| Errors, diagnostics and logging | `functional/errors-diagnostics.md` | planned |
| Caching and performance behaviour | `functional/caching.md` | planned |

Files listed as *planned* are created together with their first requirement.

## Requirement format

Each requirement is a short, testable statement with a stable ID:

```
### REQ-<AREA>-<NNN> — <short title>

Status: draft | approved | implemented | dropped

<Statement: what the system must do. One behaviour per requirement.>

Acceptance criteria:
- <observable, verifiable condition>
```

Rules:

- IDs are never reused or renumbered; dropped requirements keep their ID.
- One behaviour per requirement — split rather than combine with "and".
- Acceptance criteria must be checkable by a test or an explicit manual procedure.
- Behaviour that is intentionally unsupported is also recorded, marked as `dropped`,
  with a one-line rationale.

## Related documents

- Architecture: `docs/architecture/overview.md`
- API reference: `docs/architecture/api.md`
- Project overview: `README.md`
