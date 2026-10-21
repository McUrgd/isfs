# Atelier v1 API — Working Reference

Status: reference for `%Api.Atelier.v1` (generator version 25). The API implementation is
the source of truth; this document reorganises it for day-to-day work on ISFS and records
behaviours that are visible in the method implementations but missing from the route
comments.

The architecture that consumes this API is described in
[`docs/architecture/overview.md`](overview.md).

## 1. Scope

The API exposes an InterSystems server (IRIS/Caché) over HTTP:

- namespaces, their databases and features;
- document metadata and document content (classes, routines, CSP files, other files);
- compilation, indexing and SQL queries;
- asynchronous execution of compile/test requests;
- Ensemble class discovery;
- server diagnostics (jobs, debugger) and a metadata ZIP;
- legacy document ⇄ XML conversion.

All paths below are relative to the base URL.

## 2. Base URL, transport and authentication

```
http://<host>:<web-port>/api/atelier/v1
https://<host>:<web-port>/api/atelier/v1
```

- `%` in path segments is percent-encoded: namespace `%SYS` becomes `%25SYS`.
- Document names are passed in the path after `/doc/`; the route is `(.*)`, so it
  captures everything to the end of the URL. Document names are not expected to contain
  `/`; such names should be rejected as invalid.
- Requests and responses are UTF-8 JSON unless stated otherwise.
- Authentication and session handling are provided by the web gateway and the
  `%Atelier.REST` superclass. This export contains no login/logout route: use the
  deployment's scheme (typically HTTP Basic or a web session cookie) and handle
  `401`/`403`. Credentials must never be logged.

## 3. Response envelope

Most endpoints return the same JSON envelope:

```json
{
  "status": {
    "errors": [],
    "summary": ""
  },
  "console": [],
  "result": {}
}
```

| Field | Meaning |
|---|---|
| `status.errors` | array of request-level errors |
| `status.summary` | human-readable summary (often empty) |
| `console` | captured server/console output, mainly from source-control hooks and compilation |
| `result` | endpoint-specific payload |

Two levels of error must be checked:

1. **HTTP status** — transport/request result (`200`, `400`, `404`, `409`, ...).
2. **Payload status** — a document object has its own `result.status` string (for
   example "document does not exist" or "file is locked"). A `200` response can still
   describe a failed operation on a specific document.

Document collections normally put an array in `result.content`; `DELETE /:namespace/docs`
returns the array directly in `result` (see §8.3.6).

## 4. Document names, extensions and categories

A document is identified by its full name including extension, for example
`User.Person.cls`, `MyApp.mac`, `index.csp`. The server normalises and validates names;
invalid names produce `400`.

Categories are derived from the extension/type. The mapping used by the implementation
(`getTypeCat`/`getDlgCat`) is:

| Category | Extensions / types | Meaning |
|---|---|---|
| `CLS` | `.cls` | class definition |
| `RTN` | `.mac`, `.int`, `.inc`, `.bas`, `.mvi`, `.mvb` | routines, includes, BASIC, M routines |
| `CSP` | `.csp`, `.csr` | CSP application files |
| `OTH` | everything else (`.gbl`, `.prj`, `.obj`, `.dfi`, ...) | other documents |

Additional notes:

- Namespace names are case-insensitive; `GetNamespace` uppercases them.
- Document names are used as-is by the API; the server may return a normalised name.
- Generated documents are excluded by default. Pass `generated=1` to include them.
- CSP documents can be filtered by CSP application with the `app` query parameter; if the
  application does not belong to the namespace, `GetDocNames` returns `404`.

## 5. Content encoding

`GET /:namespace/doc/<name>` returns a **document object**:

```json
{
  "name": "User.Person.cls",
  "db": "USER",
  "ts": "2016-01-04 14:00:04.000",
  "cat": "CLS",
  "status": "",
  "enc": false,
  "content": [
    "Class User.Person Extends %Persistent",
    "{",
    "}",
    ""
  ]
}
```

| Field | Meaning |
|---|---|
| `name` | canonical document name |
| `db` | database containing the document |
| `ts` | server timestamp with millisecond precision |
| `cat` | category (`CLS`, `RTN`, `CSP`, `OTH`) |
| `status` | document-level status; empty means OK |
| `enc` | `false` for text, `true` for binary |
| `content` | text lines or base64 chunks |

Encoding rules:

- **Text** (`enc=false`): `content` is an array of lines; an empty last element means the
  document ends with a newline.
- **Binary** (`enc=true`): `content` is an array of base64-encoded chunks; concatenate then
  decode.
- `?binary=1` on `GET` forces binary encoding even for text documents.
- `?storageOnly=1` on `GET` returns only the storage part of a class.
- `?binary=1` is also accepted by `POST /:namespace/docs`, but that endpoint does **not**
  support `storageOnly` and never returns `304`.
- A write body uses the same shape:
  `{"enc": false, "content": ["line", ...]}` or
  `{"enc": true, "content": ["<base64>"]}`.

## 6. Hashing, ETags and conflicts

Atelier uses a server-side hash (derived from the document timestamp) for optimistic
concurrency:

- `HEAD /:namespace/doc/<name>` returns the current hash in the `ETag` header (the helper
  methods `SetClientServerHash`/`GetClientServerHash` are inherited from `%Atelier.REST`
  and are not part of this export).
- `GET` with `If-None-Match: <hash>` returns `304 Not Modified` when content is unchanged.
- `PUT` with `If-None-Match: <hash>` detects a client/server mismatch and returns `409`.
- `PUT ?ignoreConflict=1` bypasses the check and overwrites the server version.
- `POST /:namespace/docs` (batch read) performs no ETag checking.

Timestamp strings are also used as hashes by the implementation, so `ts` and the ETag value
usually match. Treat the header as opaque.

## 7. Endpoint index

| # | Method and path | Call | Purpose |
|---|---|---|---|
| 1 | `GET /:namespace` | `GetNamespace` | namespace info: databases, features |
| 2 | `GET /%SYS/cspapps` | `GetCSPApps` | list CSP applications (all namespaces) |
| 3 | `GET /%SYS/cspapps/:namespace` | `GetCSPApps` | list CSP applications for one namespace |
| 4 | `GET /:namespace/docnames` | `GetDocNames` | list all documents |
| 5 | `GET /:namespace/docnames/:cat` | `GetDocNames` | list documents by category |
| 6 | `GET /:namespace/docnames/:cat/:type` | `GetDocNames` | list documents by category and type(s) |
| 7 | `POST /:namespace/modified/:type` | `GetModifiedDocNames` | changed documents by database hash |
| 8 | `GET /:namespace/doc/(.*)` | `GetDoc` | read one document |
| 9 | `HEAD /:namespace/doc/(.*)` | `HeadDoc` | document hash/timestamp |
| 10 | `PUT /:namespace/doc/(.*)` | `PutDoc` | create or replace a document |
| 11 | `DELETE /:namespace/doc/(.*)` | `DeleteDoc` | delete one document |
| 12 | `POST /:namespace/docs` | `GetDocs` | batch read |
| 13 | `DELETE /:namespace/docs` | `DeleteDocs` | batch delete |
| 14 | `POST /:namespace/action/compile` | `Compile` | compile one or more documents |
| 15 | `POST /:namespace/action/index` | `Index` | retrieve document index/metadata |
| 16 | `POST /:namespace/action/query` | `Query` | run SQL |
| 17 | `POST /:namespace/work` | `QueueAsync` | queue asynchronous work |
| 18 | `GET /:namespace/work/:id` | `PollAsync` | poll queued work |
| 19 | `DELETE /:namespace/work/:id` | `CancelAsync` | cancel queued work |
| 20 | `GET /:namespace/ens/classes/:type` | `GetEnsClassType` | Ensemble class names by type |
| 21 | `GET /:namespace/ens/adapter/:name` | `GetAdpInputOutputClass` | adapter input/output classes |
| 22 | `POST /:namespace/cvt/doc/xml` | `CvtClientDocToXml` | legacy: text → XML (deprecated) |
| 23 | `POST /:namespace/cvt/xml/doc` | `CvtXmlToClientDoc` | legacy: XML → documents (deprecated) |
| 24 | `GET /%SYS/metadata/:database` | `GetMetaData` | Atelier metadata ZIP |
| 25 | `GET /%SYS/jobs` | `GetJobs` | running jobs |
| 26 | `GET /%SYS/debug` | `RunDebugger` | debugger landing page |

## 8. Endpoint reference

### 8.1 Server and namespaces

#### 8.1.1 `GET /:namespace`

Returns database mappings and feature flags for a namespace.

```json
{
  "status": { "errors": [], "summary": "" },
  "console": [],
  "result": {
    "content": {
      "name": "DOCBOOK",
      "db": [
        { "name": "DOCBOOK", "crhash": "5046B9BF0DE", "default": true },
        { "name": "CACHESYS", "crhash": "47763751EC", "default": false }
      ],
      "features": [
        { "name": "ENSEMBLE", "enabled": false }
      ]
    }
  }
}
```

- Path: `namespace` (case-insensitive; uppercased by the server).
- Status: `200`, `404` for an empty or inaccessible namespace, `500`.
- `default: true` marks the namespace's default routine database.
- `db[].crhash` is the database change hash used by `GetModifiedDocNames`.
- `features[].name = ENSEMBLE` indicates an Ensemble-enabled namespace.

#### 8.1.2 `GET /%SYS/cspapps` and `GET /%SYS/cspapps/:namespace`

Lists CSP applications. Without a namespace, all accessible namespaces are scanned;
an invalid namespace yields an empty list rather than `404`.

Plain list:

```json
{
  "status": { "errors": [], "summary": "" },
  "console": [],
  "result": { "content": ["/csp/broker", "/csp/user", "/isc/studio/rules"] }
}
```

Query `?detail=1`:

```json
{
  "status": { "errors": [], "summary": "" },
  "console": [],
  "result": {
    "content": [
      { "name": "/csp/broker", "default": false },
      { "name": "/csp/sys", "default": true }
    ]
  }
}
```

- Status: `200`, `500`.
- Applications without a filesystem path, or implemented by a dispatch class, are
  excluded by the implementation.

### 8.2 Document listing and change detection

#### 8.2.1 `GET /:namespace/docnames[/:cat[/:type]]`

Returns document descriptors.

Path parameters:

| Parameter | Values | Notes |
|---|---|---|
| `cat` | `CLS`, `RTN`, `CSP`, `OTH` | omitted/`*` = all categories |
| `type` | category-specific | can be a comma-separated list for `RTN` |

Query parameters:

| Parameter | Default | Meaning |
|---|---|---|
| `generated` | `0` | `1` includes generated documents |
| `filter` | — | substring filter; server builds `Name Like '%<filter>%'` |
| `app` | — | CSP category: restrict to a CSP application path |

Category rules (from the implementation):

- `CLS`: type must be omitted/`*`; anything else returns `400`.
- `RTN`: allowed types are `mac`, `int`, `inc`, `bas`, `mvi`, `mvb`; omitted = all of
  them; a comma-separated subset is accepted.
- `CSP`: types are CSP types; `app` selects a specific application. If `app` is not in
  the namespace's application list → `404`.
- `OTH`: types come from the server's Studio document map; unknown requested types are
  silently dropped.
- Unknown `cat` → `400`.

Response:

```json
{
  "status": { "errors": [], "summary": "" },
  "console": [],
  "result": {
    "content": [
      {
        "name": "%Activate.Enum.cls",
        "cat": "CLS",
        "ts": "2016-01-04 14:00:04.000",
        "db": "CACHELIB",
        "gen": false,
        "upd": false
      }
    ]
  }
}
```

- Descriptor fields: `name`, `cat`, `ts`, `db`, `gen` (generated flag), and `upd`
  ("up to date" flag added by the implementation; not shown in the XML example).
- Status: `200`, `400` (bad category/type), `404` (bad CSP app), `500`.
- There is no size field; size requires reading the document.

#### 8.2.2 `POST /:namespace/modified/:type`

Compares client-known database hashes with the server and returns changed documents.
Intended for incremental synchronisation.

Path parameter `type`: only `CLS` is supported; other values → `400`.

Query parameter `generated`: `0`/`1`, default `0`.

Request body (`Content-Type: application/json`), first call with an empty array:

```json
[
  { "dbname": "USER", "dbhash": "KWAGbOdnRblPzANaiv1Oiu0BZLI" }
]
```

Response (document collection; the XML description shows the array without the envelope):

```json
{
  "status": { "errors": [], "summary": "" },
  "console": [],
  "result": {
    "content": [
      {
        "dbname": "USER",
        "dbhash": "Qx1zuNaulq3b_1yR9ahZAfjkc-",
        "crhash": "47763751EC",
        "docs": [
          { "name": "User.NewClass1.cls", "ts": "2015-05-29 17:24:30.156", "gen": false }
        ]
      }
    ]
  }
}
```

- Status: `200`, `400` (empty body, non-`CLS` type, bad JSON), `415` (not JSON), `500`.
- `dbhash` is opaque; persist it between calls. `docs` may be omitted/empty when only the
  database hash changed.

### 8.3 Documents

#### 8.3.1 `GET /:namespace/doc/(.*)`

Reads one document. See §5 for the response object.

Query parameters:

| Parameter | Meaning |
|---|---|
| `binary=1` | force base64 encoding |
| `storageOnly=1` | classes only: return storage definition only |

Status: `200`, `304` (hash matches `If-None-Match`), `400` (invalid name), `404` (missing
document or empty hash), `500`.

Notes:

- The server may return modified content from source-control hooks together with `console`
  lines.
- If a soft error occurs, `result.status` contains the message, while HTTP may still be
  `200`.

#### 8.3.2 `HEAD /:namespace/doc/(.*)`

Returns headers only; the document hash is in `ETag`. Used to detect client/server
differences without downloading content.

Status: `200`, `400`, `404`, `500`.

#### 8.3.3 `PUT /:namespace/doc/(.*)`

Creates or replaces a document.

Request body (JSON, same shape as the `content` part of a document):

```json
{
  "enc": false,
  "content": ["ROUTINE xyz", "xyz ;", "   w \"hello\""]
}
```

Query parameters:

| Parameter | Meaning |
|---|---|
| `ignoreConflict=1` | skip ETag/conflict checking and overwrite |

Status:

| Code | Meaning |
|---|---|
| `200` | existing document updated |
| `201` | new document created |
| `400` | invalid document name or malformed body |
| `404` | document/resource not found (per description) |
| `409` | client/server version conflict; response contains current server content |
| `415` | wrong content type |
| `423` | document is locked (implementation; description says `425`) |
| `425` | locked write according to the description; handle both codes |
| `500` | server error |

Notes:

- **Content type discrepancy:** the description says `text/plain`, but the implementation
  requires `application/json`. Use `Content-Type: application/json`.
- Without `ignoreConflict`, send `If-None-Match: <hash>` from `GET`/`HEAD`.
- On success the server returns a document object; `content` may be empty unless the
  source was changed by hooks or a class storage definition is returned.
- `result.status` can contain a soft error even when HTTP is `200`.
- Source-control hooks may modify the saved text; the response then contains the new text
  and timestamp.

#### 8.3.4 `DELETE /:namespace/doc/(.*)`

Deletes one document.

Status: `200`, `400`, `404`, `423` (locked), `500`.

Response is a document object (`result.name`, `db`, `ts`, `cat`, `status`); `content` is
empty.

#### 8.3.5 `POST /:namespace/docs`

Batch read. Request body is a JSON array of document names:

```json
["User.Person.cls", "MyApp.mac"]
```

- `Content-Type: application/json`.
- Query `binary=1` forces base64 for all documents.
- Does not support `storageOnly`.
- Does not perform ETag checking and never returns `304`.
- Status: `200`, `400` (empty/invalid JSON, added by implementation), `415`, `500`.
- Response: a collection of document objects (`result.content`); per-document errors are in
  each object's `status`.

#### 8.3.6 `DELETE /:namespace/docs`

Batch delete. Request body is a JSON array of names:

```json
["xyz.mac", "notexist.cls"]
```

- `Content-Type: application/json`.
- Status: `200`, `400` (not a JSON array), `415`, `500`.
- Response places the array **directly in `result`**:

```json
{
  "status": { "errors": [], "summary": "" },
  "console": [],
  "result": [
    { "name": "xyz.mac", "db": "", "status": "" },
    { "name": "notexist.cls", "db": "", "status": "ERROR #5001: Document Does Not Exist..." }
  ]
}
```

- An empty per-document `status` means deletion succeeded. The overall HTTP status is
  `200` even when individual documents fail.

### 8.4 Actions

#### 8.4.1 `POST /:namespace/action/compile`

Compiles a list of documents.

Request body: JSON array of names.

```json
["Atelier.NewClass1.cls"]
```

Query parameters:

| Parameter | Default | Meaning |
|---|---|---|
| `flags` | `cuk` | compiler qualifiers (surrounding quotes are stripped) |
| `source` | `1` | `0` = do not return compiled source |

Status: `200`, `400` (bad name/body), `404`, `415`, `423` (locked), `500`.

Response: document collection with compiled docs; `content` holds the (modified) source
when `source=1`. `console` contains the compiler output.

#### 8.4.2 `POST /:namespace/action/index`

Returns structural index information. Only classes are supported.

Request body: JSON array of class names.

```json
["%Activate.GenericObject.cls"]
```

Status: `200`, `400` (invalid JSON, added by implementation), `415`, `500`.

Each entry contains the usual document fields plus:

```json
{
  "name": "%Activate.GenericObject.cls",
  "db": "CACHELIB",
  "ts": "2016-01-04 14:00:04.000",
  "gen": false,
  "others": ["%Activate.GenericObject.1.INT"],
  "cat": "CLS",
  "content": {
    "desc": "This class provides ...",
    "depl": false,
    "depr": false,
    "final": false,
    "hidden": false,
    "super": ["%Activate.IDispatch"],
    "methods": [
      {
        "name": "CreateObject",
        "desc": "This method is used ...",
        "depr": false,
        "final": true,
        "internal": false,
        "private": false,
        "scope": "class",
        "returntype": "%Library.RegisteredObject",
        "args": [{ "name": "Progid", "type": "%Library.String" }]
      }
    ],
    "parameters": [],
    "properties": []
  },
  "status": ""
}
```

The exact `content` shape depends on the document category; for classes it is the structure
above.

#### 8.4.3 `POST /:namespace/action/query`

Runs SQL in the namespace.

Request body:

```json
{
  "query": "call %studio.debugger_processlist()",
  "parameters": []
}
```

- `parameters` is optional; when present it must be a JSON array and is passed positionally
  to `%SQL.Statement.%Execute`.
- Status: `200`, `400` (bad JSON/SQL), `415`, `500`.
- Response: `result.content` is an array of row objects keyed by column name.

Column value handling:

- dates/times are strings;
- numeric/boolean columns keep their JSON types;
- a string that looks like JSON (starts with `{` or `[`) is parsed into an object/array when
  possible;
- other values are strings.

### 8.5 Asynchronous work

Used to run long compile/test requests without blocking the HTTP call. All three methods
are marked `<Internal>1</Internal>` in the export.

#### 8.5.1 `POST /:namespace/work`

Request body (`application/json`):

```json
{
  "request": "compile",
  "documents": ["User.Person.cls"],
  "flags": "cuk",
  "source": 1
}
```

or

```json
{
  "request": "testrtn",
  "routine": "MyRoutine",
  "tag": "EntryPoint"
}
```

- `request` must be `compile` or `testrtn`; anything else → `404`.
- `compile` fields: `documents` (array, required), `flags` (default `cuk`), `source`
  (default `1`).
- `testrtn` fields: `routine` and `tag` (required by the worker; missing → server error in
  the result).
- Status: `202` accepted, `400` (empty/bad JSON), `404` (unknown request type), `415`
  (not JSON), `500`.
- Response: `202` with a `Location` header. **Implementation note:** the header contains
  only the numeric ID, not a full URL; build
  `GET /:namespace/work/<id>` yourself.

#### 8.5.2 `GET /:namespace/work/:id`

Polls a queued request.

- While work is pending: `200` with `Retry-After: 3` (seconds) and newly available console
  output.
- When finished: `200` **without** `Retry-After`; `result.content` contains the work result;
  the queue entry is deleted, so a further poll returns `404`.
- Status: `200`, `404` (unknown/consumed), `410` (purged; documented, but the implementation
  returns `404`), `423` (queue locked), `500`.
- Console lines are consumed progressively; each poll returns only new lines.

#### 8.5.3 `DELETE /:namespace/work/:id`

Cancels a queued/running request.

- `200` when the request was removed, finished, or cleaned up;
- `202` when the monitor job was terminated (cancellation issued);
- `404` when the ID does not exist;
- `500` on error.
- `Retry-After` may be returned when the server is in rollback and could not cancel yet.

### 8.6 Ensemble

#### 8.6.1 `GET /:namespace/ens/classes/:type`

Returns Ensemble class names for the requested type.

| `type` | Meaning |
|---|---|
| 1 | Adapters |
| 2 | Inbound adapters |
| 3 | Outbound adapters |
| 4 | Messages |
| 5 | Requests |
| 6 | Responses |
| 7 | Business services |
| 8 | Business processes |
| 9 | Business operations |
| 10 | Data transformations |
| 11 | Production |
| 12 | Business host |
| 13 | Dashboard |
| 14 | Rule |

Status: `200`, `500`. Response: `result.content` array of class names.

#### 8.6.2 `GET /:namespace/ens/adapter/:name`

Returns the adapter's input/output classes.

```json
{
  "status": { "errors": [], "summary": "" },
  "console": [],
  "result": {
    "content": { "input": "%Stream.Object", "output": "%String" }
  }
}
```

- Defaults to `%RegisteredObject` for a missing `SERVICEINPUTCLASS`/`SERVICEOUTPUTCLASS`.
- Status: `200`, `404` (adapter class not found), `500`.

### 8.7 Legacy conversion (deprecated)

Both endpoints are marked as **not public** and are scheduled for removal. Do not build new
features on them.

#### 8.7.1 `POST /:namespace/cvt/doc/xml`

- Request `Content-Type: text/plain` (description says JSON, implementation checks
  `text/plain`): the document text; the name is extracted from the content.
- Response: XML representation; status `200`, `400`, `415`, `500`.

#### 8.7.2 `POST /:namespace/cvt/xml/doc`

- Request `Content-Type: application/xml`: XML export.
- Response: document collection; XML is imported into a temporary area, not saved.
- Status: `200`, `400`, `415`, `500`.

### 8.8 Diagnostics and metadata

#### 8.8.1 `GET /%SYS/metadata/:database`

Returns `METADATA.zip` for the database as raw binary
(`application/octet-stream`, `Content-Disposition: attachment`). No JSON envelope.

- Status: `200`, `404` (database/metadata missing), `500`.

#### 8.8.2 `GET /%SYS/jobs`

Lists running jobs. Query `system=1` includes system routines (hidden by default).

```json
{
  "status": { "errors": [], "summary": "" },
  "console": [],
  "result": {
    "content": [
      {
        "pid": 1394,
        "namespace": "%SYS",
        "routine": "%Studio.Debugger.1",
        "state": "RUN",
        "device": "|TCP|1972|1394"
      }
    ]
  }
}
```

- Status: `200`, `500`.
- Only jobs that can be examined and are not already being debugged are returned.

#### 8.8.3 `GET /%SYS/debug`

Debugger landing point. It redirects I/O and renders the `%Atelier.v1.XDebugAgent` page
(`RunDebugger`). No JSON envelope or documented status codes; intended for the debugger UI.

## 9. Data models

### 9.1 Document object

| Field | Type | Notes |
|---|---|---|
| `name` | string | canonical name |
| `db` | string | database |
| `ts` | string | `YYYY-MM-DD HH:MM:SS.mmm` |
| `cat` | string | `CLS`/`RTN`/`CSP`/`OTH` |
| `status` | string | document-level status, empty = OK |
| `enc` | boolean | base64 encoding flag |
| `content` | array | lines or base64 chunks |

Variants add `gen`, `upd`, `others`, `content` (index payload).

### 9.2 Document descriptor (`GetDocNames`)

| Field | Type | Notes |
|---|---|---|
| `name` | string | document name |
| `cat` | string | category |
| `ts` | string | timestamp |
| `db` | string | database |
| `gen` | boolean | generated document |
| `upd` | boolean | up-to-date flag (implementation) |

### 9.3 Namespace info (`GetNamespace`)

| Field | Type | Notes |
|---|---|---|
| `name` | string | uppercased namespace |
| `db` | array | `{name, crhash, default}` |
| `features` | array | `{name, enabled}`, currently `ENSEMBLE` |

### 9.4 Async work

| Field | Type | Notes |
|---|---|---|
| `request` | string | `compile` or `testrtn` |
| `documents` | array | compile target names |
| `flags` | string | compile qualifiers |
| `source` | boolean | return compiled source |
| `routine`, `tag` | string | testrtn target |

Poll response `content` carries the work result; `console` carries output lines.

## 10. HTTP status summary

| Code | Typical meaning in this API |
|---|---|
| `200` | OK / updated |
| `201` | created (`PUT` on a new document) |
| `202` | async work accepted / cancellation issued |
| `304` | not modified (ETag match) |
| `400` | invalid name, category/type, JSON body or query |
| `401` | not authenticated (gateway) |
| `403` | not authorized / inaccessible namespace |
| `404` | missing document, namespace, CSP app, adapter, work item |
| `409` | version conflict on `PUT` |
| `410` | async results purged (documented) |
| `415` | unsupported content type |
| `423` | document/queue locked |
| `425` | locked write (description; implementation may send `423`) |
| `500` | server error; details in `status.errors` |

## 11. Quick examples

```bash
# Namespace info
curl -u user:pass "https://server:57772/api/atelier/v1/USER"

# List classes (including generated)
curl -u user:pass "https://server:57772/api/atelier/v1/USER/docnames/CLS?generated=1"

# Read a class, forcing text
curl -u user:pass "https://server:57772/api/atelier/v1/USER/doc/User.Person.cls"

# Read metadata/hash
curl -u user:pass -I "https://server:57772/api/atelier/v1/USER/doc/User.Person.cls"

# Save a class (body is JSON, even though the description says text/plain)
curl -u user:pass -X PUT \
  -H "Content-Type: application/json" \
  -H "If-None-Match: <hash-from-GET>" \
  --data '{"enc":false,"content":["Class User.Person Extends %Persistent","{","}",""]}' \
  "https://server:57772/api/atelier/v1/USER/doc/User.Person.cls"

# Compile
curl -u user:pass -X POST \
  -H "Content-Type: application/json" \
  --data '["User.Person.cls"]' \
  "https://server:57772/api/atelier/v1/USER/action/compile?flags=cuk&source=0"

# Async compile
curl -u user:pass -X POST \
  -H "Content-Type: application/json" \
  --data '{"request":"compile","documents":["User.Person.cls"],"flags":"cuk"}' \
  "https://server:57772/api/atelier/v1/USER/work"
```

## 12. Implementation notes and discrepancies

These are places where the XML description, the exported route map and the method bodies do
not fully agree. Treat the method bodies as authoritative for the server version that
produced the export, and verify against a live server when in doubt.

| # | Item | Observation |
|---|---|---|
| 1 | `PUT /doc/...` content type | Description says `text/plain`; implementation requires `application/json`. |
| 2 | `PUT` lock status | Description says `425`; implementation returns `423`. Handle both. |
| 3 | `GetDocNames` `upd` field | Returned by the implementation but not shown in the XML example. |
| 4 | `GetModifiedDocNames` response | Description shows a bare array; implementation builds a document collection. |
| 5 | `GetDocs` bad body | Implementation returns `400`; description lists only `415`/`500`. |
| 6 | `Index` bad JSON | Implementation returns `400`; description lists only `415`/`500`. |
| 7 | Async `Location` header | Implementation sets the bare numeric ID, not a URL. |
| 8 | Async `410` | Documented for purged results; implementation returns `404`. |
| 9 | `CvtClientDocToXml` content type | Description says JSON; implementation requires `text/plain`. |
| 10 | `DeleteDocs` result shape | Array directly in `result`, unlike other collections. |
| 11 | CSP `app` filter | Not mentioned in the main description; used by `GetDocNames`. |
| 12 | `GetCSPApps` invalid namespace | Returns an empty list, not `404`. |
| 13 | Compile on locked document | Description mentions `423`/`404`; the actual per-document outcome may be a `200` with a document `status`. |
| 14 | `GetDoc` ETag | Requires `If-None-Match`; response uses `ETag` (helper methods are inherited, not in this export). |
| 15 | Source-control hooks | Can alter content on `GET`/`PUT`; `console` carries their output. |

## 13. Usage in ISFS

For the filesystem mapping, the essential subset is:

| ISFS operation | Endpoint(s) |
|---|---|
| Mount/probe | `GET /:namespace` |
| Root/namespace/category listing | `GET /:namespace/docnames[/:cat[/:type]]` |
| Stat / hash | `HEAD /:namespace/doc/<name>` |
| Read | `GET /:namespace/doc/<name>` |
| Write / create | `PUT /:namespace/doc/<name>` |
| Delete | `DELETE /:namespace/doc/<name>` |
| Change detection | `POST /:namespace/modified/CLS` |

The endpoints not used by the filesystem (`compile`, `index`, `query`, async work, Ensemble,
conversion, jobs, debugger, metadata) are control-plane APIs; see
[`docs/architecture/overview.md`](overview.md) §4 for the rationale.

## 14. Related documents

- Architecture and filesystem mapping: `docs/architecture/overview.md`
- Requirements index: `docs/requirements/README.md`
