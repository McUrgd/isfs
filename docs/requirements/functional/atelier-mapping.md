# Atelier mapping — requirements

Status: draft.

Scope: how the Atelier resources of a namespace become the paths of the mounted
volume, and how a path is turned back into an Atelier request. This file covers
the path model — the volume root, the category directories, the package levels,
the decomposition of a document name and the resolution of a path. What the
file system callbacks do with a resolved path is in `filesystem-operations.md`;
the mount lifecycle is in `mount-lifecycle.md`; the transport and the
credentials are in `connection-auth.md`; the caches are in `caching.md`; the
error mapping and the log formats are in `errors-diagnostics.md`.

Source sections of [`docs/architecture/overview.md`](../../architecture/overview.md):
§3 (volume and path model), §3.1 (name decomposition), §4.2 (operations),
§4.3 (rename semantics), §6 (metadata synthesis), §12 (open decisions).
Source sections of [`docs/architecture/api.md`](../../architecture/api.md):
§4 (names, extensions and categories), §8.2.1 (document listing), §13 (usage in
ISFS).

> Current state: the repository contains the WinFsp passthrough skeleton
> (`isfs/isfs.c`); the Atelier-backed volume source is not implemented yet. Every
> requirement below is therefore `draft`, except REQ-MAP-001 and REQ-MAP-002,
> which the current build already satisfies.

## Path model

The volume is a view over one namespace. The namespace is fixed for the whole
mount by `-ns` and is not part of the path: it is the root of the volume
(REQ-MAP-001). Below the root there are two kinds of synthetic, read-only
directories — the categories and the package levels — and the documents.

| Windows path | Kind | Atelier name |
|---|---|---|
| `\` | directory (volume root) | the namespace of the mount |
| `\<CAT>\` | directory (synthetic) | — |
| `\<CAT>\<PKG>\` | directory (synthetic) | — |
| `\<CAT>\<PKG>\…\<NAME>` | file | `<PKG>.<…>.<NAME>` |

`<CAT>` is one of `CLS`, `CSP`, `RTN`, `OTH`. A document name is split into
its package levels and its file name by the rule of REQ-MAP-004; the file name
keeps the extension.

```
User.Person.cls     →  CLS\User\Person.cls
MyApp.mac           →  RTN\MyApp.mac
%Activate.Enum.cls  →  CLS\%Activate\Enum.cls
index.csp           →  CSP\index.csp
notes.backup.txt    →  OTH\notes\backup.txt
```

```
I:\
├── CLS\
│   ├── Person.cls
│   └── User\
│       └── Person.cls
├── RTN\
│   └── MyApp.mac
├── CSP\
│   └── index.csp
└── OTH\
    └── settings.json
```

## Requirements

### REQ-MAP-001 — The volume root is the mounted namespace

Status: implemented

The root of the mounted volume must be the namespace given by `-ns`, and the
namespace must not appear in the tree of the volume. The volume exposes only
that one namespace: it is not a server-wide view and does not list namespaces.

Acceptance criteria:
- The root `\` corresponds to `:namespace` in every Atelier URL.
- No directory named after the namespace exists in the tree; `-ns USER` gives
  `\CLS\`, not `\USER\CLS\`.
- No other namespace is queried or listed (REQ-CONN-003).
- The category entries of the root keep the server's canonical casing.

> Merged: this requirement absorbed REQ-MOUNT-018 ("Root directory contents"),
> which was retired from `mount-lifecycle.md` as its duplicate. The ID
> REQ-MOUNT-018 is not reused.

### REQ-MAP-002 — The root contains only category directories

Status: implemented

The root of the volume must contain only the synthetic category directories and
no files.

Acceptance criteria:
- A listing of `\` reports the fixed set of category directories — `CLS`, `CSP`,
  `RTN` and `OTH` — and nothing else; the set does not depend on the documents
  of the namespace.
- No file is reported at the root: every document of the namespace belongs to a
  category.

### REQ-MAP-003 — The first segment of a document path is its category

Status: draft

The first segment of the path of a document must be the category that the
server assigns to it.

Acceptance criteria:
- The category of a document is the `cat` value of its descriptor in the
  listing; the extension rules of api.md §4 explain how the server derives it
  (`CLS` for `.cls`, `RTN` for `.mac`/`.int`/`.inc`/`.bas`/`.mvi`/`.mvb`, `CSP`
  for `.csp`/`.csr`, `OTH` for everything else).
- A document of category `CLS` is reachable at `\<CLS>\…` and at no other
  first segment.
- A path whose first segment is not the category of the document does not
  resolve; the lookup fails with `STATUS_OBJECT_NAME_NOT_FOUND`.

### REQ-MAP-004 — The dots of a document name become directory levels

Status: draft

The name of a document must be split into package levels and a file name
according to the dots it contains.

Acceptance criteria:
- The extension is the part of the name after its last dot; it stays with the
  file.
- Every dot before that one becomes a directory separator; the segments in
  front of the file name are the package levels of the document.
- A name without a dot has no extension and stays a single segment.
- A name that begins with a dot is not decomposed; the whole name is one
  segment.
- The name is never changed in any other way: no case is folded, no character
  is substituted.

### REQ-MAP-005 — The Atelier name of a path is reconstructed from its segments

Status: draft

The Atelier name of a document must be reconstructed from the segments of its
path, and every request for a document must carry the reconstructed name.

Acceptance criteria:
- The segments of a path are joined with dots, the extension staying with the
  last segment: `CLS\User\Person.cls` → `User.Person.cls`.
- The URL of a request for that document is
  `/:namespace/doc/User.Person.cls`, never the path.
- The reconstruction is the inverse of the decomposition of REQ-MAP-004: a
  name that was decomposed is restored exactly, including its case.

### REQ-MAP-006 — A document whose name contains a dot has no flat alias

Status: draft

A document whose name contains a dot must be reachable only through the
nested path that its name describes.

Acceptance criteria:
- `CLS\User.Person.cls` is not a valid path for the document `User.Person.cls`
  and resolves to nothing.
- The document is reachable at `CLS\User\Person.cls`.
- The lookup of the flat form fails with `STATUS_OBJECT_NAME_NOT_FOUND`.

> Rationale: the flat form would be indistinguishable from the package
> directory `User` followed by the file `Person.cls`. The mapping stays
> unambiguous in both directions (overview §3.1).

### REQ-MAP-007 — Package levels are synthetic directories

Status: draft

The package levels of the tree must be synthetic, read-only directories
derived from the names of a category listing.

Acceptance criteria:
- A package level is reported as a directory, with `FILE_ATTRIBUTE_DIRECTORY`
  (REQ-FS-008).
- A package level exists only as long as a document below it exists; a listing
  of a category reports no empty package level.
- A package level cannot be created, deleted or renamed
  (REQ-FS-036, REQ-FS-037).
- No request is issued to the server for a package level itself; it is derived
  from the listing of its category.

### REQ-MAP-008 — A package level is listed from the listing of its category

Status: draft

The contents of a package level must be served from the listing of its
category, filtered by the package prefix.

Acceptance criteria:
- A listing of `CLS\User\` issues the request for the listing of `CLS` and
  reports the documents whose name starts with `User.`.
- The listing of a package level costs no additional request beyond the
  listing of its category.
- The entries of a package level are the files and the next package levels
  derived from the names that match the prefix.
- The package prefix is matched case-insensitively, like every other level
  (REQ-MAP-009).

### REQ-MAP-009 — The lookup is case-insensitive on every level

Status: implemented

The resolution of a path must be case-insensitive on every level of the path,
while the canonical casing of the server is kept in the results.

Acceptance criteria:
- `\cls\user\person.cls` resolves to `\CLS\User\Person.cls`.
- A listing reports the casing of the server, not the casing of the request
  (REQ-FS-004).
- The comparison is performed on the segments of the path, not on the dotted
  name: `cls\user\person.cls` and `CLS\USER\PERSON.CLS` resolve to the same
  document.

> Cross-reference: REQ-MOUNT-019 states the same rule from the mount lifecycle
> side.

### REQ-MAP-010 — Names that differ only by case: the first one is exposed

Status: draft

Two documents of one category whose names differ only by case must not both be
exposed; the first one wins and the collision is logged.

Acceptance criteria:
- A listing of the category reports one of the two names and not the other.
- A warning naming the category and both names is logged.
- The lookup of the exposed name succeeds; the lookup of the hidden one
  resolves to the exposed document, because the comparison is
  case-insensitive (REQ-MAP-009).

> Resolved: overview §12.6 keeps the choice between skipping the duplicate,
> suffixing it and failing the mount open. The requirement above implements
> skipping the duplicate, because suffixing would produce a name that does not
> exist on the server and failing the mount would make the whole namespace
> unusable for one pair of documents.

### REQ-MAP-011 — Generated documents are not exposed

Status: draft

A generated document must not appear in the tree of the volume.

Acceptance criteria:
- The listing request does not set `generated=1`; the server default
  `generated=0` applies.
- A generated document appears neither in a category listing nor in a package
  level listing.
- Opening the path of a generated document fails with
  `STATUS_OBJECT_NAME_NOT_FOUND`.

> Cross-reference: REQ-FS-002 stated the same behaviour from the file system
> side; it is dropped as a duplicate, so this requirement is the single place
> that defines the behaviour.

### REQ-MAP-012 — The segments of a name are used verbatim

Status: draft

The segments of a document name must be used as path segments exactly as the
server returns them.

Acceptance criteria:
- `%` is legal as a path segment on NTFS, so a document named
  `%Activate.Enum.cls` is exposed at `CLS\%Activate\Enum.cls` without any
  escaping.
- No segment is shortened, suffixed or otherwise altered to make it fit a
  path.
- The reconstruction of the name (REQ-MAP-005) yields the name of the server
  for every exposed document.

### REQ-MAP-013 — A name that cannot be represented is not exposed

Status: draft

A document whose name does not decompose into path segments that are legal
NTFS names must not be exposed.

Acceptance criteria:
- A document whose name contains a segment that Windows rejects (an empty
  segment, a reserved device name, a character that is illegal in a file name)
  is absent from the listing.
- A warning naming the document and the offending segment is logged.
- No other document of the category is affected by the skipped document.

### REQ-MAP-014 — A created file is reported before the first flush

Status: draft

A file that was created but not yet flushed must be reported by the file
system as an existing file with the content of its local buffer, even though
it does not exist on the server yet.

Acceptance criteria:
- A `GetFileInfo` for the path of a freshly created file succeeds and reports
  the size of the buffer (0 for a file that was not written) and the time of
  the creation.
- The document does not exist on the server before the flush; no request for
  it is issued to the server before the flush.
- The file does not appear in a directory listing before the flush: a listing
  is built from the server and does not know the pending file.
- After the flush succeeded, the file appears in the listing like any other
  document.

> Resolved: REQ-FS-018 leaves the decision to this file. The requirement above
> implements the "local buffer" variant, because Windows expects a created
> file to exist; the gap between the stat and the listing is the accepted
> consequence of a server that is the source of truth.

### REQ-MAP-015 — A path is resolved against the listing of its category

Status: draft

A path below the root must be resolved against the listing of the category
that its first segment names.

Acceptance criteria:
- The resolution uses the cached listing of the category; the decision what a
  path is costs no request of its own.
- The matching is performed segment by segment: the first segment selects the
  category, the next ones select the package levels, the last one selects the
  document.
- A path that matches no entry of the listing fails with
  `STATUS_OBJECT_NAME_NOT_FOUND`; a path that names an existing package level
  succeeds as a directory.
- The kind of the result — directory or file — follows from the match: a
  segment that is the prefix of several names is a package level, a segment that
  matches a name is a document.
- The path model decides what a path is. Stat does not collect per-document
  metadata (REQ-FS-005 is dropped): the attributes and the full-access descriptor
  are synthesized, and the size is 0 until the content is fetched
  (`filesystem-operations.md`, REQ-FS-003, REQ-FS-006).

### REQ-MAP-016 — A move between package levels changes the document name

Status: draft

A rename that changes the package level of a document must change the Atelier
name of that document.

Acceptance criteria:
- The `PUT` of the rename carries the dotted name reconstructed from the
  target path (REQ-MAP-005), e.g. `Admin.Person.cls` for the target
  `CLS\Admin\Person.cls`.
- The source document is deleted under its old name.
- A rename that keeps the category and the package level keeps the name and
  only replaces the content of the document.

> Cross-reference: REQ-FS-032 to REQ-FS-035 describe the sequence of the
> rename and the rules for the replacement of an existing target.

### REQ-MAP-017 — The namespace as a directory — dropped

Status: dropped

Exposing the namespace as a directory of the volume, or discovering the
namespaces of the server, is intentionally unsupported. Rationale: the
namespace is fixed for the mount by `-ns` and becomes the root (REQ-MAP-001);
the API exposes no listing of namespaces, and the tree of the volume must
describe exactly one namespace (overview §12.1).

### REQ-MAP-018 — An escaping scheme for illegal names — dropped

Status: dropped

Escaping the illegal characters of a document name is intentionally
unsupported in v1. Rationale: the decomposition of the name already turns it
into path segments, and a document whose segments are not legal NTFS names is
skipped with a warning instead (REQ-MAP-013); an escaping scheme would need a
documented transformation and an inverse transformation on every request
(overview §3.1).

### REQ-MAP-019 — A flat alias for a dotted name — dropped

Status: dropped

Addressing a document whose name contains a dot by a single flat path segment
is intentionally unsupported. Rationale: the flat form cannot be told apart
from a package directory followed by a file, so the mapping would stop being
unambiguous (REQ-MAP-006).

### REQ-MAP-020 — The CSP application hierarchy — dropped

Status: dropped

Exposing the CSP application hierarchy as directories of the `CSP` category is
intentionally unsupported in v1. Rationale: a document name carries no path
(the API expects no `/` in a name, api.md §2), so the hierarchy is not part of
the data that ISFS receives; the `app` parameter of the listing is a
server-side filter and is not used by the file system (api.md §8.2.1). The CSP
documents of the namespace are exposed under `CSP\`, decomposed by the rule of
REQ-MAP-004 like every other document.

## Open decisions affecting this area

From overview §12, to be resolved and folded into the requirements above:

- path layout — resolved: the package levels are directories, the dots of the
  name are the levels (REQ-MAP-004, REQ-MAP-005).
- case collisions — resolved: the first name is exposed, the second is skipped
  with a warning (REQ-MAP-010).
- generated documents — resolved: they are not exposed (REQ-MAP-011).
- rename across namespaces — resolved: not applicable, the namespace is fixed
  for the mount (REQ-MAP-001).
- the visibility of a file that was created but not yet flushed — resolved: it
  is reported by a stat, but it is not part of a listing (REQ-MAP-014).

Open: whether the server returns documents without an extension. Such a
document belongs to the category `OTH` and is exposed as a single segment below
it (REQ-MAP-004); the point is left open because the category rules of
api.md §4 define `OTH` as "everything else" and do not name that case.

## Related documents

- Architecture: [`docs/architecture/overview.md`](../../architecture/overview.md)
- API reference: [`docs/architecture/api.md`](../../architecture/api.md)
- Requirements index: [`docs/requirements/README.md`](../README.md)
- Filesystem operations: [`filesystem-operations.md`](filesystem-operations.md)
- Mount lifecycle: [`mount-lifecycle.md`](mount-lifecycle.md)
- Connection and authentication: [`connection-auth.md`](connection-auth.md)
