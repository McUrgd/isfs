# Caching and performance — requirements

Status: draft.

Scope: how the metadata of the volume is kept in memory, so that enumerating a
directory or resolving a path does not rebuild the synthetic tree from the server
on every call. This file covers the descriptor cache of a category, the derived
package structure, the lifetime and the invalidation of both, the optional
warm-up and the diagnostics of the cache. The operations that read and change
documents are in `filesystem-operations.md`; the path model and the derivation of
the package levels are in `atelier-mapping.md`; the mount lifecycle is in
`mount-lifecycle.md`; the transport is in `connection-auth.md`; the error mapping
and the log formats are covered by the planned `errors-diagnostics.md`.

Source sections of [`docs/architecture/overview.md`](../../architecture/overview.md):
§4.2 (operations), §8 (caching and consistency), §9 (concurrency and locking),
§10 (diagnostics), §12 (resolved design decisions).

> Current state: the project keeps one in-memory descriptor cache per category with
> a fixed 30 s lifetime (`isfs/isfs.c`, `DESCRIPTOR_CACHE_TTL_MS`), together with
> the package structure derived from those descriptors once per refresh. Every
> local change still discards the whole category. REQ-CACHE-001..004, 007 and 008
> describe that behaviour and are implemented; the rest are draft or explicitly
> dropped. A shared content cache is out of scope here — see REQ-FS-017 (dropped)
> in [`filesystem-operations.md`](filesystem-operations.md).

## Cache configuration

Inputs referenced by the requirements below. The concrete configuration surface is
the command line of `isfs.exe`; a configuration file and service parameters are not
supported in v1 ([`connection-auth.md`](connection-auth.md), REQ-CONN-018). The
names of the options are indicative.

| Setting | Required | Default | Used by |
|---|---|---|---|
| Lifetime of a category listing (`-cache-ttl` seconds) | no | 30 s | REQ-CACHE-006, 007 |
| Warm-up of category listings at mount (`-cache-warm` categories) | no | off | REQ-CACHE-010 |
| Document descriptors held per category | — | no limit | REQ-CACHE-001, 004 |

## Requirements

### REQ-CACHE-001 — A category listing is held in memory for the mount

Status: implemented

The descriptors of a category must be kept in memory for the lifetime of the
mount, so that the category, its package levels and its documents can be served
without asking the server again.

Acceptance criteria:
- The first enumeration of a category issues one `GET /:namespace/docnames/:cat`
  and keeps the answer in memory.
- The cache belongs to the mount and is released when the volume is unmounted.
- The root `\` has no cache entry: it is a constant list of the category
  directories and is never requested from the server (REQ-MAP-001, REQ-MAP-002).
- A document listing is loaded lazily, at the first enumeration of its category;
  the mount itself requests no listing unless the warm-up is enabled
  (REQ-CACHE-010).

### REQ-CACHE-002 — A cache hit costs no request

Status: implemented

While the cache entry of a category is valid, every access below that category
must be answered from memory, without a request to the server.

Acceptance criteria:
- Re-enumerating a category within its lifetime issues no request.
- Enumerating a package level issues no request beyond the refresh of its category
  (REQ-MAP-008).
- Resolving a path to a document or to a package level issues no request of its
  own (REQ-MAP-015).
- Opening a document still fetches its content (REQ-FS-013): the descriptor cache
  never serves document content.

### REQ-CACHE-003 — One refresh of a category at a time

Status: implemented

Concurrent accesses to a category whose cache entry is not valid must not issue
the same listing request more than once.

Acceptance criteria:
- Concurrent enumerations of one category issue one `docnames` request; the
  waiting callers use its result.
- Enumerations of different categories do not block each other.
- A failed refresh releases the category, so that a later enumeration retries
  instead of waiting forever.

### REQ-CACHE-004 — The package structure is derived once per refresh

Status: implemented

The package levels and the documents of a category must be derived from the
descriptors once per refresh and reused by every enumeration and every path
resolution, so that opening a package directory does not rescan the whole
category.

Acceptance criteria:
- After one refresh of a category, enumerating any package level of that category
  reads a prebuilt index instead of walking all descriptors.
- The work of a package enumeration is proportional to the entries of that
  package, not to the number of documents in the category.
- The index is rebuilt only when the category entry is refreshed, i.e. after its
  lifetime expires or after a local change invalidated it; a cache hit reuses it
  unchanged.
- A debug log line reports the build of the index (category, descriptor count) and
  its reuse (category, package prefix, number of entries). Both lines are already
  emitted; the remaining cache decisions of REQ-CACHE-009 are still draft.

> Rationale: the derived structure is built by `BuildPackageIndex` together with
> the descriptors and kept next to them; `BuildCategoryEntries` and
> `ResolveCategoryPath` read it instead of scanning the listing. Before the
> change the entries of a package level were derived by walking every
> descriptor of the category on every `ReadDirectory`, so a large `CLS` made
> each folder open cost O(n) even when the descriptor cache was warm.

### REQ-CACHE-005 — A local change updates the cached listing

Status: draft

A successful save, delete or rename must update the affected entries of the
cached listing instead of discarding the category, so that the next enumeration
does not download the whole listing again.

Acceptance criteria:
- A successful save of a document updates its descriptor in the category entry
  (name, category, timestamp from the answer of the server) and keeps the other
  entries valid.
- A successful delete removes the descriptor from the category entry.
- A successful rename moves the descriptor from the source category entry to the
  target one, replacing an entry of the same name when the replacement was
  allowed (REQ-FS-034).
- An enumeration that follows such a change issues no `docnames` request and
  shows the new state.
- Entries that the change did not touch keep their server timestamp and are not
  re-downloaded.
- When the effect of a change on the cache cannot be established, the affected
  category entries are invalidated as a whole; the fallback must not produce a
  stale listing.

> Cross-reference: REQ-FS-030 requires the delete to leave the cache,
> REQ-FS-032 and REQ-FS-033 require the rename to update or invalidate both names.
> This requirement fixes *how* the update is done for the listing cache.

### REQ-CACHE-006 — The lifetime of a listing is configurable

Status: draft

The time a category listing is reused must be configurable, so that a slow server
can be traded against the freshness of the tree.

Acceptance criteria:
- A mount option sets the lifetime in seconds; the default is 30 s.
- The value `0` disables reuse: every enumeration refreshes the category.
- The lifetime applies per category; an expired entry is refreshed on its next
  use, not by a timer or a background sweep.
- The effective value is reported in the startup log (overview §10).
- The value does not affect the invalidation by a local change (REQ-CACHE-005).

> Rationale: the lifetime is a compile-time constant (`DESCRIPTOR_CACHE_TTL_MS`)
> and no option exists; the requirement makes the documented default real and
> configurable. Overview §8 states that the lifetime is fixed in v1 and refers to
> this requirement.

### REQ-CACHE-007 — External changes become visible within the lifetime

Status: implemented

A change made by another client must become visible no later than one lifetime of
the category listing, and the staleness in between is accepted.

Acceptance criteria:
- A document created, changed or deleted by another client is reflected by an
  enumeration that happens after the lifetime of the entry has expired.
- Until then a listing may show the previous state; this is the documented
  staleness bound, not an error.
- Event-driven invalidation (`POST /:namespace/modified/CLS`) is not required in
  v1: the lifetime is the only freshness mechanism.

### REQ-CACHE-008 — A failed refresh keeps the previous listing

Status: implemented

A failed listing request must not damage the cache entry: the caller gets the
error, and a later attempt can refresh the category normally.

Acceptance criteria:
- A failed `docnames` request returns an error to the caller; the expired entries
  are not served as valid.
- The stored descriptors survive the failure and are replaced by the next
  successful refresh.
- The retry is issued by the next enumeration, not by a background retry loop.

### REQ-CACHE-009 — Cache decisions are logged

Status: draft

The cache must be diagnosable from the debug log without reproducing the load.

Acceptance criteria:
- The debug log records a hit, a miss, a refresh, an update and an invalidation,
  each with the category and, where applicable, the package prefix.
- A refresh reports the number of descriptors and its elapsed time.
- An invalidation reports the reason (local change, fallback).
- The lines contain no document content and no credentials (overview §10).
- The existing information line that reports the number of listed documents is
  kept.

### REQ-CACHE-010 — Category listings may be warmed up at mount

Status: draft

The mount may preload the listings of selected categories, so that the first
navigation does not pay for them.

Acceptance criteria:
- The option names the categories to preload; without it the mount requests no
  listing.
- The warm-up runs after the mount-time probe succeeded (REQ-MOUNT-009) and does
  not delay the completion of the mount.
- A warm-up failure is logged as a warning and does not fail the mount.
- A warm-up fills the same cache entries that a normal enumeration would, so the
  first navigation is a cache hit (REQ-CACHE-002).

> Open point: whether the default should be off (as today) or the three small
> categories, and whether the warm-up of a large `CLS` is worth its traffic.

### REQ-CACHE-011 — A persistent cache across mounts — dropped

Status: dropped

Keeping the descriptors on disk between mounts is intentionally unsupported in
v1. Rationale: the cache would need an invalidation and conflict model of its own,
it would blur the rule that the server is the source of truth, and a cold start is
not the dominant cost while the lifetime is short. Revisit when measurements show
that the first enumeration of a category dominates. An offline mount is rejected
for the same reason (REQ-MOUNT-027).

### REQ-CACHE-012 — A negative cache of missing names — dropped

Status: dropped

Caching the names that do not exist is intentionally unsupported. Rationale: a
path is resolved against the cached listing of its category and costs no request
of its own (REQ-MAP-015), so a missing name is already answered from that listing;
a second cache would only add a way to serve a stale "not found".

## Open points

- **Granularity of the invalidation.** Today every save, delete and rename drops
  the whole category (`InvalidateCategoryCache`); REQ-CACHE-005 asks for a
  per-entry update. The exact set of cases in which the fallback applies is left
  open.
- **Memory bound of the derived index.** REQ-CACHE-004 trades CPU for memory; a
  limit and an eviction rule are not fixed yet. The descriptors themselves are
  bounded by the size of the namespace.
- **Per-category lifetime.** A large `CLS` and a small `OTH` may deserve different
  values; REQ-CACHE-006 defines one value per mount.
- **Event-driven invalidation.** `POST /:namespace/modified/CLS` can replace the
  waiting time of REQ-CACHE-007 for classes; it is not used in v1, and no
  requirement depends on it.
- **Warm-up scope and default.** See REQ-CACHE-010.

## Decisions affecting this area

- The namespace is fixed per mount, so there is no namespace cache (overview §8).
- Content is owned by the open handle and is not cached across handles
  (REQ-FS-013, REQ-FS-017 dropped).
- Cache entries are per category, keyed by the category name; the package levels
  are not separate server resources (REQ-MAP-007).
- A cache miss is answered by the same request that a listing uses; there is no
  separate metadata endpoint (REQ-FS-001).

## Related documents

- Architecture: [`docs/architecture/overview.md`](../../architecture/overview.md),
  §8 "Caching and consistency"
- API reference: [`docs/architecture/api.md`](../../architecture/api.md),
  §8.2 (listing)
- Requirements index: [`docs/requirements/README.md`](../README.md)
- Filesystem operations: [`filesystem-operations.md`](filesystem-operations.md)
- Atelier mapping: [`atelier-mapping.md`](atelier-mapping.md)
- Mount lifecycle: [`mount-lifecycle.md`](mount-lifecycle.md)
- Connection and authentication: [`connection-auth.md`](connection-auth.md)
