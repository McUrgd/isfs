/**
 * @file isfs.c
 *
 * @copyright 2015-2025 Bill Zissimopoulos
 * @copyright 2026 Pavlov Sergey
 */
 /*
  * This file is part of WinFsp.
  *
  * You can redistribute it and/or modify it under the terms of the GNU
  * General Public License version 3 as published by the Free Software
  * Foundation.
  *
  * Licensees holding a valid commercial license may use this software
  * in accordance with the commercial license agreement provided in
  * conjunction with the software.  The terms and conditions of any such
  * commercial license agreement shall govern, supersede, and render
  * ineffective any application of the GPLv3 license to this software,
  * notwithstanding of any reference thereto in the software or
  * associated repository.
  */

#include <winfsp/winfsp.h>
#include <strsafe.h>
#include <wctype.h>
#include <sddl.h>
#include <stdlib.h>
#include <string.h>

#include "cjson/cJSON.h"
#include "isfs/atelier.h"
#include "isfs/helper.h"

#define PROGNAME                        "isfs"
#define VOLUME_NAME                     "ISFS"
#define ATELIER_API_PATH                "/api/atelier/v1"
/* the lifetime of a document descriptor cache entry (overview §8) */
#define DESCRIPTOR_CACHE_TTL_MS         30000
#define ALLOCATION_UNIT                 4096
#define FULLPATH_SIZE                   (MAX_PATH + FSP_FSCTL_TRANSACT_PATH_SIZEMAX / sizeof(WCHAR))

#define info(format, ...)               FspServiceLog(EVENTLOG_INFORMATION_TYPE, format, __VA_ARGS__)
#define warn(format, ...)               FspServiceLog(EVENTLOG_WARNING_TYPE, format, __VA_ARGS__)
#define fail(format, ...)               FspServiceLog(EVENTLOG_ERROR_TYPE, format, __VA_ARGS__)

#define ConcatPath(Ptfs, FN, FP)        (0 == StringCbPrintfW(FP, sizeof FP, L"%s%s", Ptfs->BaseUrl, FN))
#define HandleFromContext(FC)           (((PTFS_FILE_CONTEXT *)(FC))->Handle)

/* the categories of the volume are declared in helper.h (Categories and
 * CATEGORY_COUNT)
 */

/* the Atelier client of the mount is declared in atelier.h (REQ-CONN-021) */

/* a document descriptor of a category listing (api.md §9.2) */
typedef struct
{
    PWSTR Name;     /* the canonical document name, e.g. "User.Person.cls" */
    PWSTR Category; /* CLS, CSP, RTN or OTH */
    PWSTR Db;       /* the database that holds the document */
    PWSTR Ts;       /* the server timestamp, "YYYY-MM-DD HH:MM:SS.mmm" */
    BOOLEAN Generated;
} PTFS_DESCRIPTOR;

/* the descriptor cache of one category (overview §8) */
typedef struct
{
    CRITICAL_SECTION Lock;
    BOOLEAN Valid;
    ULONGLONG Expires; /* GetTickCount64 milliseconds */
    PTFS_DESCRIPTOR* Descriptors;
    ULONG Count;
} PTFS_CATEGORY_CACHE;

typedef struct
{
    FSP_FILE_SYSTEM* FileSystem;
    PWSTR BaseUrl; /* http://host:port/api/atelier/v1/<namespace> (REQ-MOUNT-009) */
    PWSTR UserName; /* login from -user, memory only (REQ-MOUNT-010) */
    PWSTR Password; /* password from -pass, memory only (REQ-MOUNT-010) */
    PWSTR Namespace; /* the -ns value, used for the index numbers (REQ-FS-012) */
    ATELIER_CLIENT Client; /* the HTTP client of the mount */
    PTFS_CATEGORY_CACHE Cache[CATEGORY_COUNT]; /* the descriptor caches */
} PTFS;

typedef struct
{
    HANDLE Handle;
    PVOID DirBuffer;
    BOOLEAN IsRoot; /* "\" is the namespace root, a synthetic directory (REQ-MAP-001) */
    BOOLEAN IsSynthetic; /* a synthetic directory: a category or a package level */
    BOOLEAN IsDocument; /* an opened document: the content is held in memory (REQ-FS-013) */
    PWSTR Category; /* the category of a synthetic directory, e.g. "CLS" */
    PWSTR PackagePrefix; /* the package path of a synthetic directory, e.g. "User" */
    PWSTR DocumentName; /* the canonical dotted name of a document */
    PBYTE Content; /* the decoded content of a document (REQ-FS-013) */
    SIZE_T ContentSize; /* the size of Content in bytes (REQ-FS-006) */
    UINT64 Timestamp; /* the newest document timestamp below the directory, or the document ts */
} PTFS_FILE_CONTEXT;

/* the volume root is the -ns namespace: WinFsp passes "\" for it, possibly
 * with leading separators (REQ-MAP-001)
 */
static BOOLEAN IsRootFileName(PCWSTR FileName)
{
    if (0 == FileName)
        return FALSE;

    while (L'\\' == *FileName || L'/' == *FileName)
        FileName++;

    return L'\0' == *FileName;
}

/* synthetic directories have no local backing object, so their metadata is
 * generated instead of read from a handle (REQ-MAP-001). The index number is
 * derived from the name of the directory (REQ-FS-012); a timestamp of 0 means
 * "now".
 */
static NTSTATUS GetSyntheticDirInfo(FSP_FSCTL_FILE_INFO* FileInfo, UINT64 IndexNumber, UINT64 Timestamp)
{
    FILETIME Time;

    if (0 == Timestamp)
        GetSystemTimeAsFileTime(&Time);
    else
        ((PLARGE_INTEGER)&Time)->QuadPart = (LONGLONG)Timestamp;

    memset(FileInfo, 0, sizeof *FileInfo);
    FileInfo->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    FileInfo->FileSize = 0;
    FileInfo->AllocationSize = 0;
    FileInfo->CreationTime = ((PLARGE_INTEGER)&Time)->QuadPart;
    FileInfo->LastAccessTime = FileInfo->CreationTime;
    FileInfo->LastWriteTime = FileInfo->CreationTime;
    FileInfo->ChangeTime = FileInfo->CreationTime;
    FileInfo->IndexNumber = IndexNumber;
    FileInfo->HardLinks = 0;

    return STATUS_SUCCESS;
}

/* the metadata of a document is synthesized from the content that was read
 * and from its server timestamp (REQ-FS-005, REQ-FS-006)
 */
static NTSTATUS GetDocumentInfo(PTFS* Ptfs, PTFS_FILE_CONTEXT* Context,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    FILETIME Time;

    if (0 == Context->Timestamp)
        GetSystemTimeAsFileTime(&Time);
    else
        ((PLARGE_INTEGER)&Time)->QuadPart = (LONGLONG)Context->Timestamp;

    memset(FileInfo, 0, sizeof *FileInfo);
    FileInfo->FileAttributes = FILE_ATTRIBUTE_NORMAL;
    FileInfo->FileSize = (UINT64)Context->ContentSize;
    FileInfo->AllocationSize = (FileInfo->FileSize + ALLOCATION_UNIT - 1)
        / ALLOCATION_UNIT * ALLOCATION_UNIT;
    FileInfo->CreationTime = ((PLARGE_INTEGER)&Time)->QuadPart;
    FileInfo->LastAccessTime = FileInfo->CreationTime;
    FileInfo->LastWriteTime = FileInfo->CreationTime;
    FileInfo->ChangeTime = FileInfo->CreationTime;
    FileInfo->IndexNumber = HashEntryName(Ptfs->Namespace, Context->DocumentName);
    FileInfo->HardLinks = 0;

    return STATUS_SUCCESS;
}

/* case-insensitive wildcard match used for the synthetic root entries;
 * WinFsp passes the search pattern because PassQueryDirectoryPattern is set
 */
static BOOLEAN PtfsMatchPattern(PCWSTR Pattern, PCWSTR Name)
{
    /* Win32 treats "*.*" as "all names"; FindFirstFileW did the same */
    if (L'\0' != Pattern[0] &&
        L'*' == Pattern[0] && L'.' == Pattern[1] && L'*' == Pattern[2] &&
        L'\0' == Pattern[3])
        return TRUE;
    while (L'\0' != *Pattern)
    {
        if (L'*' == *Pattern)
        {
            while (L'*' == *Pattern)
                Pattern++;
            if (L'\0' == *Pattern)
                return TRUE;
            for (; L'\0' != *Name; Name++)
                if (PtfsMatchPattern(Pattern, Name))
                    return TRUE;
            return FALSE;
        }

        if (L'\0' == *Name)
            return FALSE;

        if (L'?' != *Pattern && towupper(*Pattern) != towupper(*Name))
            return FALSE;

        Pattern++;
        Name++;
    }

    return L'\0' == *Name;
}

/* ------------------------------------------------------------------------
 * Strings, names and timestamps
 * ------------------------------------------------------------------------ */

/* the string, name and timestamp helpers are in helper.c */

/* ------------------------------------------------------------------------
 * JSON
 *
 * The answers of the Atelier API are UTF-8 JSON. They are parsed with cJSON
 * (MIT, cjson); the strings of the server are converted to UTF-16 for the
 * WinFsp layer.
 * ------------------------------------------------------------------------ */

/* converts a UTF-8 text of the server into a UTF-16 string; the result is
 * allocated and must be freed with free()
 */
static PWSTR Utf8ToWide(const char* Utf8)
{
    int Length;
    PWSTR Wide;

    if (0 == Utf8)
        return 0;

    Length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Utf8, -1, 0, 0);
    if (0 == Length)
        return 0;

    Wide = malloc((SIZE_T)Length * sizeof(WCHAR));
    if (0 == Wide)
        return 0;

    if (0 == MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Utf8, -1, Wide, Length))
    {
        free(Wide);
        return 0;
    }

    return Wide;
}

/* parses the answer of the server; the whole text must hold one JSON value,
 * trailing whitespace is allowed
 */
static cJSON* JsonParse(const char* Text, SIZE_T Length)
{
    const char* End = 0;
    cJSON* Root;
    SIZE_T Index;

    Root = cJSON_ParseWithLengthOpts(Text, Length, &End, 0);
    if (0 == Root)
        return 0;

    for (Index = (SIZE_T)(End - Text); Index < Length; Index++)
    {
        char Ch = Text[Index];

        if (' ' != Ch && '\t' != Ch && '\r' != Ch && '\n' != Ch)
        {
            cJSON_Delete(Root);
            return 0;
        }
    }

    return Root;
}

/* ------------------------------------------------------------------------
 * The document descriptors of a category
 * ------------------------------------------------------------------------ */

static VOID FreeDescriptors(PTFS_DESCRIPTOR* Descriptors, ULONG Count)
{
    ULONG Index;

    if (0 == Descriptors)
        return;

    for (Index = 0; Index < Count; Index++)
    {
        free(Descriptors[Index].Name);
        free(Descriptors[Index].Category);
        free(Descriptors[Index].Db);
        free(Descriptors[Index].Ts);
    }

    free(Descriptors);
}

static NTSTATUS CopyDescriptors(const PTFS_DESCRIPTOR* Descriptors, ULONG Count,
    PTFS_DESCRIPTOR** PDescriptors, ULONG* PCount)
{
    PTFS_DESCRIPTOR* Copy;
    ULONG Index;

    *PDescriptors = 0;
    *PCount = 0;

    Copy = malloc((Count ? Count : 1) * sizeof *Copy);
    if (0 == Copy)
        return STATUS_INSUFFICIENT_RESOURCES;
    memset(Copy, 0, (Count ? Count : 1) * sizeof *Copy);

    for (Index = 0; Index < Count; Index++)
    {
        Copy[Index].Name = DupString(Descriptors[Index].Name);
        Copy[Index].Category = DupString(Descriptors[Index].Category);
        Copy[Index].Db = DupString(Descriptors[Index].Db);
        Copy[Index].Ts = DupString(Descriptors[Index].Ts);
        Copy[Index].Generated = Descriptors[Index].Generated;

        if ((0 != Descriptors[Index].Name && 0 == Copy[Index].Name) ||
            (0 != Descriptors[Index].Category && 0 == Copy[Index].Category) ||
            (0 != Descriptors[Index].Db && 0 == Copy[Index].Db) ||
            (0 != Descriptors[Index].Ts && 0 == Copy[Index].Ts))
        {
            FreeDescriptors(Copy, Index);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    *PDescriptors = Copy;
    *PCount = Count;

    return STATUS_SUCCESS;
}

/* reads the document descriptors of a category from the answer of the server
 * (api.md §8.2.1)
 */
static NTSTATUS ParseCategoryListing(PBYTE Response, DWORD ResponseLength, PCWSTR Category,
    PTFS_DESCRIPTOR** PDescriptors, ULONG* PCount)
{
    cJSON* Root = 0;
    const cJSON* Status;
    const cJSON* Errors;
    const cJSON* Result;
    const cJSON* Content;
    PTFS_DESCRIPTOR* Descriptors = 0;
    ULONG Count = 0, Index;

    *PDescriptors = 0;
    *PCount = 0;

    Root = JsonParse((const char*)Response, ResponseLength);
    if (0 == Root)
    {
        fail(L"cannot list the documents of %s: the answer of the server is not valid JSON", Category);
        return STATUS_UNEXPECTED_IO_ERROR;
    }

    /* the request level errors are reported even when the status is 200 (api.md §3) */
    Status = cJSON_GetObjectItemCaseSensitive(Root, "status");
    Errors = cJSON_GetObjectItemCaseSensitive(Status, "errors");
    if (0 != Errors && cJSON_IsArray(Errors) && 0 != cJSON_GetArraySize(Errors))
    {
        warn(L"the server reported %d error(s) while the documents of %s were listed",
            cJSON_GetArraySize(Errors), Category);
    }

    Result = cJSON_GetObjectItemCaseSensitive(Root, "result");
    Content = cJSON_GetObjectItemCaseSensitive(Result, "content");
    if (0 == Content || !cJSON_IsArray(Content))
    {
        cJSON_Delete(Root);
        fail(L"cannot list the documents of %s: the answer of the server has no content array", Category);
        return STATUS_UNEXPECTED_IO_ERROR;
    }

    Count = (ULONG)cJSON_GetArraySize(Content);
    Descriptors = malloc((Count ? Count : 1) * sizeof *Descriptors);
    if (0 == Descriptors)
    {
        cJSON_Delete(Root);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    memset(Descriptors, 0, (Count ? Count : 1) * sizeof *Descriptors);

    for (Index = 0; Index < Count; Index++)
    {
        const cJSON* Item = cJSON_GetArrayItem(Content, (int)Index);
        const cJSON* Name = cJSON_GetObjectItemCaseSensitive(Item, "name");

        if (!cJSON_IsString(Name))
        {
            warn(L"the server listed a document of %s without a name; it is ignored", Category);
            continue;
        }

        Descriptors[Index].Name = Utf8ToWide(cJSON_GetStringValue(Name));
        Descriptors[Index].Category = Utf8ToWide(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(Item, "cat")));
        Descriptors[Index].Db = Utf8ToWide(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(Item, "db")));
        Descriptors[Index].Ts = Utf8ToWide(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(Item, "ts")));
        Descriptors[Index].Generated = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(Item, "gen"));

        if (0 == Descriptors[Index].Name)
        {
            FreeDescriptors(Descriptors, Index);
            cJSON_Delete(Root);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    cJSON_Delete(Root);

    *PDescriptors = Descriptors;
    *PCount = Count;

    return STATUS_SUCCESS;
}

/* the descriptors of a category, from the cache when the cache is valid
 * (overview §8, REQ-FS-001)
 */
static NTSTATUS GetCategoryDescriptors(PTFS* Ptfs, PCWSTR Category,
    PTFS_DESCRIPTOR** PDescriptors, ULONG* PCount)
{
    PTFS_CATEGORY_CACHE* Cache;
    WCHAR Path[64];
    PBYTE Response = 0;
    DWORD ResponseLength = 0, StatusCode = 0;
    PTFS_DESCRIPTOR* Descriptors = 0;
    ULONG Count = 0;
    NTSTATUS Result;
    int Index;

    *PDescriptors = 0;
    *PCount = 0;

    Index = CategoryIndex(Category);
    if (Index < 0)
        return STATUS_OBJECT_NAME_INVALID;

    Cache = &Ptfs->Cache[Index];

    /* one enumeration of a category at a time, so that a listing is not
     * requested twice for the same category (overview §9)
     */
    EnterCriticalSection(&Cache->Lock);

    if (Cache->Valid && GetTickCount64() < Cache->Expires)
    {
        Result = CopyDescriptors(Cache->Descriptors, Cache->Count, PDescriptors, PCount);
        LeaveCriticalSection(&Cache->Lock);
        return Result;
    }

    if (FAILED(StringCchPrintfW(Path, ARRAYSIZE(Path), L"/docnames/%s", Categories[Index])))
    {
        LeaveCriticalSection(&Cache->Lock);
        return STATUS_OBJECT_NAME_INVALID;
    }

    Result = AtelierRequest(&Ptfs->Client, Ptfs->UserName, Ptfs->Password, L"GET", Path,
        &Response, &ResponseLength, &StatusCode);
    if (!NT_SUCCESS(Result))
    {
        LeaveCriticalSection(&Cache->Lock);
        return Result;
    }

    if (StatusCode < 200 || StatusCode >= 300)
    {
        fail(L"cannot list the documents of %s: the server answered HTTP %lu",
            Categories[Index], StatusCode);
        free(Response);
        LeaveCriticalSection(&Cache->Lock);
        return AtelierStatusToNtStatus(StatusCode);
    }

    Result = ParseCategoryListing(Response, ResponseLength, Categories[Index], &Descriptors, &Count);
    free(Response);
    if (!NT_SUCCESS(Result))
    {
        LeaveCriticalSection(&Cache->Lock);
        return Result;
    }

    info(L"listed %lu document(s) of %s", Count, Categories[Index]);

    FreeDescriptors(Cache->Descriptors, Cache->Count);
    Cache->Descriptors = Descriptors;
    Cache->Count = Count;
    Cache->Valid = TRUE;
    Cache->Expires = GetTickCount64() + DESCRIPTOR_CACHE_TTL_MS;

    Result = CopyDescriptors(Cache->Descriptors, Cache->Count, PDescriptors, PCount);

    LeaveCriticalSection(&Cache->Lock);

    return Result;
}

/* ------------------------------------------------------------------------
 * The content of a document
 * ------------------------------------------------------------------------ */

static int Base64Value(char Ch)
{
    if (Ch >= 'A' && Ch <= 'Z')
        return Ch - 'A';
    if (Ch >= 'a' && Ch <= 'z')
        return Ch - 'a' + 26;
    if (Ch >= '0' && Ch <= '9')
        return Ch - '0' + 52;
    if ('+' == Ch)
        return 62;
    if ('/' == Ch)
        return 63;

    return -1;
}

/* decodes the concatenated base64 chunks of a binary document (REQ-FS-016) */
static NTSTATUS Base64Decode(const char* Text, SIZE_T Length, PBYTE* PData, SIZE_T* PSize)
{
    PBYTE Data;
    SIZE_T Index, Out = 0;
    UINT32 Accumulator = 0;
    ULONG Bits = 0;

    *PData = 0;
    *PSize = 0;

    Data = malloc(Length / 4 * 3 + 3);
    if (0 == Data)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (Index = 0; Index < Length; Index++)
    {
        char Ch = Text[Index];
        int Value;

        /* a base64 text may be wrapped; the whitespace carries no data */
        if (' ' == Ch || '\t' == Ch || '\r' == Ch || '\n' == Ch)
            continue;

        /* the padding only terminates the last chunk */
        if ('=' == Ch)
            break;

        Value = Base64Value(Ch);
        if (Value < 0)
        {
            free(Data);
            return STATUS_INVALID_NETWORK_RESPONSE;
        }

        Accumulator = (Accumulator << 6) | (UINT32)Value;
        Bits += 6;
        if (Bits >= 8)
        {
            Bits -= 8;
            Data[Out++] = (BYTE)(Accumulator >> Bits);
            Accumulator &= (1u << Bits) - 1;
        }
    }

    *PData = Data;
    *PSize = Out;

    return STATUS_SUCCESS;
}

/* joins the lines of a text document with a line feed (REQ-FS-015); the
 * result is UTF-8, as the content is served to the caller
 */
static NTSTATUS JoinTextContent(const cJSON* Content, PBYTE* PData, SIZE_T* PSize)
{
    int Count = cJSON_GetArraySize(Content);
    SIZE_T Total = 0;
    PBYTE Data, Cursor;
    int Index;

    *PData = 0;
    *PSize = 0;

    for (Index = 0; Index < Count; Index++)
    {
        const cJSON* Item = cJSON_GetArrayItem(Content, Index);
        const char* Line;

        if (!cJSON_IsString(Item))
            return STATUS_INVALID_NETWORK_RESPONSE;

        Line = cJSON_GetStringValue(Item);
        Total += strlen(Line);
        if (Index + 1 < Count)
            Total++;
    }

    Data = malloc(Total ? Total : 1);
    if (0 == Data)
        return STATUS_INSUFFICIENT_RESOURCES;

    Cursor = Data;
    for (Index = 0; Index < Count; Index++)
    {
        const char* Line = cJSON_GetStringValue(cJSON_GetArrayItem(Content, Index));
        SIZE_T LineLength = strlen(Line);

        memcpy(Cursor, Line, LineLength);
        Cursor += LineLength;
        if (Index + 1 < Count)
            *Cursor++ = '\n';
    }

    *PData = Data;
    *PSize = Total;

    return STATUS_SUCCESS;
}

/* concatenates the base64 chunks and decodes them (REQ-FS-016) */
static NTSTATUS DecodeBinaryContent(const cJSON* Content, PBYTE* PData, SIZE_T* PSize)
{
    int Count = cJSON_GetArraySize(Content);
    SIZE_T Total = 0;
    PBYTE Text, Cursor;
    NTSTATUS Result;
    int Index;

    *PData = 0;
    *PSize = 0;

    for (Index = 0; Index < Count; Index++)
    {
        const cJSON* Item = cJSON_GetArrayItem(Content, Index);

        if (!cJSON_IsString(Item))
            return STATUS_INVALID_NETWORK_RESPONSE;

        Total += strlen(cJSON_GetStringValue(Item));
    }

    Text = malloc(Total + 1);
    if (0 == Text)
        return STATUS_INSUFFICIENT_RESOURCES;

    Cursor = Text;
    for (Index = 0; Index < Count; Index++)
    {
        const char* Chunk = cJSON_GetStringValue(cJSON_GetArrayItem(Content, Index));
        SIZE_T ChunkLength = strlen(Chunk);

        memcpy(Cursor, Chunk, ChunkLength);
        Cursor += ChunkLength;
    }
    Text[Total] = 0;

    Result = Base64Decode((const char*)Text, Total, PData, PSize);
    free(Text);

    return Result;
}

/* parses the answer of GET /:namespace/doc/<name> (api.md §5, §8.3.1) */
static NTSTATUS ParseDocument(PBYTE Response, DWORD ResponseLength, PCWSTR DocumentName,
    PBYTE* PContent, SIZE_T* PContentSize, UINT64* PTimestamp)
{
    cJSON* Root = 0;
    const cJSON* Status;
    const cJSON* Errors;
    const cJSON* Result;
    const cJSON* DocumentStatus;
    const cJSON* Enc;
    const cJSON* Content;
    const cJSON* Ts;
    PWSTR Text;
    NTSTATUS NtStatus;

    *PContent = 0;
    *PContentSize = 0;
    *PTimestamp = 0;

    Root = JsonParse((const char*)Response, ResponseLength);
    if (0 == Root)
    {
        fail(L"cannot read the document %s: the answer of the server is not valid JSON",
            DocumentName);
        return STATUS_UNEXPECTED_IO_ERROR;
    }

    /* request level errors are logged even when the status is 200 (api.md §3) */
    Status = cJSON_GetObjectItemCaseSensitive(Root, "status");
    Errors = cJSON_GetObjectItemCaseSensitive(Status, "errors");
    if (0 != Errors && cJSON_IsArray(Errors) && 0 != cJSON_GetArraySize(Errors))
        warn(L"the server reported %d error(s) while the document %s was read",
            cJSON_GetArraySize(Errors), DocumentName);

    Result = cJSON_GetObjectItemCaseSensitive(Root, "result");
    if (!cJSON_IsObject(Result))
    {
        cJSON_Delete(Root);
        fail(L"cannot read the document %s: the answer of the server has no result object",
            DocumentName);
        return STATUS_UNEXPECTED_IO_ERROR;
    }

    /* a document level status is a soft error reported with HTTP 200 (api.md §8.3.1) */
    DocumentStatus = cJSON_GetObjectItemCaseSensitive(Result, "status");
    if (cJSON_IsString(DocumentStatus) && '\0' != *cJSON_GetStringValue(DocumentStatus))
    {
        Text = Utf8ToWide(cJSON_GetStringValue(DocumentStatus));
        fail(L"cannot read the document %s: the server reported an error: %s",
            DocumentName, 0 != Text ? Text : L"(no details)");
        free(Text);
        cJSON_Delete(Root);
        return STATUS_UNEXPECTED_IO_ERROR;
    }

    Ts = cJSON_GetObjectItemCaseSensitive(Result, "ts");
    if (cJSON_IsString(Ts))
    {
        Text = Utf8ToWide(cJSON_GetStringValue(Ts));
        if (0 != Text)
        {
            ParseTimestamp(Text, PTimestamp);
            free(Text);
        }
    }

    Content = cJSON_GetObjectItemCaseSensitive(Result, "content");
    if (0 == Content || !cJSON_IsArray(Content))
    {
        cJSON_Delete(Root);
        fail(L"cannot read the document %s: the answer of the server has no content array",
            DocumentName);
        return STATUS_UNEXPECTED_IO_ERROR;
    }

    /* enc=false is a line array, enc=true is a base64 chunk array (REQ-FS-015, REQ-FS-016) */
    Enc = cJSON_GetObjectItemCaseSensitive(Result, "enc");
    if (cJSON_IsTrue(Enc))
        NtStatus = DecodeBinaryContent(Content, PContent, PContentSize);
    else
        NtStatus = JoinTextContent(Content, PContent, PContentSize);

    if (!NT_SUCCESS(NtStatus))
        fail(L"cannot read the document %s: its content is not valid", DocumentName);

    cJSON_Delete(Root);

    return NtStatus;
}

/* reads and decodes one document; the content is allocated and must be freed
 * with free() (REQ-FS-013, REQ-FS-014)
 */
static NTSTATUS LoadDocument(PTFS* Ptfs, PCWSTR DocumentName,
    PBYTE* PContent, SIZE_T* PContentSize, UINT64* PTimestamp)
{
    WCHAR Path[2048];
    PBYTE Response = 0;
    DWORD ResponseLength = 0, StatusCode = 0;
    NTSTATUS Result;

    *PContent = 0;
    *PContentSize = 0;
    *PTimestamp = 0;

    /* the dotted document name is used as-is by the API (api.md §4) */
    if (FAILED(StringCchPrintfW(Path, ARRAYSIZE(Path), L"/doc/%s", DocumentName)))
        return STATUS_OBJECT_NAME_INVALID;

    Result = AtelierRequest(&Ptfs->Client, Ptfs->UserName, Ptfs->Password, L"GET", Path,
        &Response, &ResponseLength, &StatusCode);
    if (!NT_SUCCESS(Result))
        return Result;

    if (StatusCode < 200 || StatusCode >= 300)
    {
        fail(L"cannot read the document %s: the server answered HTTP %lu",
            DocumentName, StatusCode);
        free(Response);
        return AtelierStatusToNtStatus(StatusCode);
    }

    Result = ParseDocument(Response, ResponseLength, DocumentName,
        PContent, PContentSize, PTimestamp);

    free(Response);

    return Result;
}

/* ------------------------------------------------------------------------
 * The entries of a directory
 * ------------------------------------------------------------------------ */

typedef struct
{
    PWSTR Name;         /* the name of the entry in the directory */
    PWSTR PathName;     /* the dotted path of a package level */
    PWSTR DocumentName; /* the dotted name of a document */
    BOOLEAN IsDirectory;
    UINT64 Timestamp;   /* the timestamp of the newest document below the entry */
    UINT64 IndexNumber;
} PTFS_DIR_ENTRY;

static VOID FreeDirEntries(PTFS_DIR_ENTRY* Entries, ULONG Count)
{
    ULONG Index;

    if (0 == Entries)
        return;

    for (Index = 0; Index < Count; Index++)
    {
        free(Entries[Index].Name);
        free(Entries[Index].PathName);
        free(Entries[Index].DocumentName);
    }

    free(Entries);
}

/* adds an entry to a directory; the entries of several documents of one
 * package level are one entry (REQ-MAP-007)
 */
static NTSTATUS AddDirEntry(PTFS_DIR_ENTRY** PEntries, ULONG* PCount, ULONG* PCapacity,
    PCWSTR Name, BOOLEAN IsDirectory, PCWSTR DocumentName, PCWSTR PathName, UINT64 Timestamp)
{
    ULONG Index;
    PTFS_DIR_ENTRY* Entry;

    for (Index = 0; Index < *PCount; Index++)
    {
        Entry = &(*PEntries)[Index];

        if (0 != _wcsicmp(Entry->Name, Name))
            continue;

        if (Entry->IsDirectory && IsDirectory)
        {
            /* the same package level: the newest timestamp wins */
            if (Timestamp > Entry->Timestamp)
                Entry->Timestamp = Timestamp;
            return STATUS_SUCCESS;
        }

        /* a document and a package level with the same name cannot both be
         * exposed; the first one wins
         */
        warn(L"%s is listed twice and is exposed only once", Name);

        return STATUS_SUCCESS;
    }

    if (*PCount == *PCapacity)
    {
        ULONG Capacity = *PCapacity ? *PCapacity * 2 : 16;
        PTFS_DIR_ENTRY* Entries = realloc(*PEntries, Capacity * sizeof *Entries);

        if (0 == Entries)
            return STATUS_INSUFFICIENT_RESOURCES;

        memset(Entries + *PCount, 0, (Capacity - *PCount) * sizeof *Entries);
        *PEntries = Entries;
        *PCapacity = Capacity;
    }

    Entry = &(*PEntries)[(*PCount)++];
    memset(Entry, 0, sizeof *Entry);
    Entry->Name = DupString(Name);
    Entry->DocumentName = DupString(DocumentName);
    Entry->PathName = DupString(PathName);
    Entry->IsDirectory = IsDirectory;
    Entry->Timestamp = Timestamp;

    if (0 == Entry->Name || (0 != DocumentName && 0 == Entry->DocumentName) ||
        (0 != PathName && 0 == Entry->PathName))
    {
        free(Entry->Name);
        free(Entry->DocumentName);
        free(Entry->PathName);
        memset(Entry, 0, sizeof *Entry);
        (*PCount)--;

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_SUCCESS;
}

/* the entries of a category directory or of a package level; a package level
 * is served from the listing of its category, filtered by the prefix
 * (REQ-FS-001, REQ-MAP-008)
 */
static NTSTATUS BuildCategoryEntries(PTFS* Ptfs, PCWSTR Category, PCWSTR Prefix,
    PTFS_DIR_ENTRY** PEntries, ULONG* PCount)
{
    PTFS_DESCRIPTOR* Descriptors = 0;
    ULONG DescriptorCount = 0, Index;
    PWSTR* PrefixSegments = 0;
    ULONG PrefixCount = 0;
    PWSTR* Accepted = 0;
    ULONG AcceptedCount = 0, AcceptedCapacity = 0;
    PWSTR CanonicalPrefix = 0;
    PTFS_DIR_ENTRY* Entries = 0;
    ULONG EntryCount = 0, EntryCapacity = 0;
    NTSTATUS Result;

    *PEntries = 0;
    *PCount = 0;

    if (0 != Prefix && L'\0' != *Prefix)
        PrefixCount = SplitByDelimiter(Prefix, L'.', 0, &PrefixSegments);

    Result = GetCategoryDescriptors(Ptfs, Category, &Descriptors, &DescriptorCount);
    if (!NT_SUCCESS(Result))
        goto exit;

    for (Index = 0; Index < DescriptorCount; Index++)
    {
        PTFS_DESCRIPTOR* Descriptor = &Descriptors[Index];
        PWSTR* Segments = 0;
        ULONG SegmentCount, SegmentIndex;
        ULONG PackageCount;
        BOOLEAN Skip = FALSE, Match = TRUE;
        UINT64 Timestamp = 0;

        /* the generated documents are not exposed (REQ-FS-002, REQ-MAP-011) */
        if (Descriptor->Generated)
            continue;

        /* two documents that differ only by case: the first one is exposed
         * (REQ-MAP-010)
         */
        for (SegmentIndex = 0; SegmentIndex < AcceptedCount; SegmentIndex++)
            if (0 == _wcsicmp(Accepted[SegmentIndex], Descriptor->Name))
            {
                warn(L"%s: the document %s is hidden, because %s differs from it only by case",
                    Category, Descriptor->Name, Accepted[SegmentIndex]);
                Skip = TRUE;
                break;
            }
        if (Skip)
            continue;

        if (AcceptedCount == AcceptedCapacity)
        {
            ULONG Capacity = AcceptedCapacity ? AcceptedCapacity * 2 : 32;
            PWSTR* Names = realloc(Accepted, Capacity * sizeof *Names);

            if (0 == Names)
            {
                Result = STATUS_INSUFFICIENT_RESOURCES;
                goto exit;
            }
            Accepted = Names;
            AcceptedCapacity = Capacity;
        }
        Accepted[AcceptedCount] = Descriptor->Name;
        AcceptedCount++;

        SegmentCount = SplitDocumentName(Descriptor->Name, &Segments);
        if (0 == SegmentCount)
            continue;

        /* every segment must be a legal name of a path segment (REQ-MAP-013) */
        for (SegmentIndex = 0; SegmentIndex < SegmentCount; SegmentIndex++)
        {
            if (!IsLegalPathSegment(Segments[SegmentIndex]))
            {
                warn(L"%s: the document %s is not exposed, because %s is not a legal file name",
                    Category, Descriptor->Name, Segments[SegmentIndex]);
                Skip = TRUE;
                break;
            }
        }
        if (Skip)
        {
            FreeSegments(Segments, SegmentCount);
            continue;
        }

        /* the package levels of the document are the segments in front of the
         * file name (REQ-MAP-004)
         */
        PackageCount = SegmentCount - 1;

        if (PackageCount < PrefixCount)
            Match = FALSE;
        for (SegmentIndex = 0; Match && SegmentIndex < PrefixCount; SegmentIndex++)
            if (0 != _wcsicmp(Segments[SegmentIndex], PrefixSegments[SegmentIndex]))
                Match = FALSE;

        if (Match)
        {
            ParseTimestamp(Descriptor->Ts, &Timestamp);

            /* the canonical spelling of the package path is the one of the
             * server, not the one of the request (REQ-FS-004); it is taken
             * from the first document that matches the prefix
             */
            if (0 == CanonicalPrefix)
            {
                CanonicalPrefix = JoinDotted(Segments, PrefixCount);
                if (0 == CanonicalPrefix)
                {
                    Result = STATUS_INSUFFICIENT_RESOURCES;
                    FreeSegments(Segments, SegmentCount);
                    goto exit;
                }
            }

            if (PackageCount == PrefixCount)
            {
                /* the document itself */
                Result = AddDirEntry(&Entries, &EntryCount, &EntryCapacity,
                    Segments[SegmentCount - 1], FALSE, Descriptor->Name, 0, Timestamp);
            }
            else
            {
                /* the next package level */
                WCHAR PathName[2048];

                if (0 == PrefixCount)
                {
                    if (FAILED(StringCchCopyW(PathName, ARRAYSIZE(PathName), Segments[PrefixCount])))
                    {
                        Result = STATUS_OBJECT_NAME_INVALID;
                        FreeSegments(Segments, SegmentCount);
                        goto exit;
                    }
                }
                else
                {
                    if (FAILED(StringCchPrintfW(PathName, ARRAYSIZE(PathName), L"%s.%s",
                        CanonicalPrefix, Segments[PrefixCount])))
                    {
                        Result = STATUS_OBJECT_NAME_INVALID;
                        FreeSegments(Segments, SegmentCount);
                        goto exit;
                    }
                }

                Result = AddDirEntry(&Entries, &EntryCount, &EntryCapacity,
                    Segments[PrefixCount], TRUE, 0, PathName, Timestamp);
            }

            if (!NT_SUCCESS(Result))
            {
                FreeSegments(Segments, SegmentCount);
                goto exit;
            }
        }

        FreeSegments(Segments, SegmentCount);
    }

    /* the index number of an entry is derived from its name (REQ-FS-012) */
    for (Index = 0; Index < EntryCount; Index++)
    {
        if (Entries[Index].IsDirectory)
            Entries[Index].IndexNumber = HashEntryName(Ptfs->Namespace, Entries[Index].PathName);
        else
            Entries[Index].IndexNumber = HashEntryName(Ptfs->Namespace, Entries[Index].DocumentName);
    }

    *PEntries = Entries;
    *PCount = EntryCount;
    Entries = 0;
    EntryCount = 0;

exit:
    FreeDirEntries(Entries, EntryCount);
    FreeDescriptors(Descriptors, DescriptorCount);
    if (0 != Accepted)
        free(Accepted);
    if (0 != PrefixSegments)
        FreeSegments(PrefixSegments, PrefixCount);
    if (0 != CanonicalPrefix)
        free(CanonicalPrefix);

    return Result;
}

/* ------------------------------------------------------------------------
 * The resolution of a path
 * ------------------------------------------------------------------------ */

typedef enum
{
    VOLUME_PATH_ROOT,     /* the root of the volume */
    VOLUME_PATH_CATEGORY, /* a category directory, e.g. "\CLS" */
    VOLUME_PATH_PACKAGE,  /* a package level, e.g. "\CLS\User" */
    VOLUME_PATH_DOCUMENT, /* a document, e.g. "\CLS\User\Person.cls" */
    VOLUME_PATH_INVALID
} VOLUME_PATH_KIND;

typedef struct
{
    VOLUME_PATH_KIND Kind;
    PWSTR Category;      /* the category of the path */
    PWSTR PackagePrefix; /* the dotted path of a package level */
    PWSTR DocumentName;  /* the dotted name of a document */
} VOLUME_PATH;

static VOID VolumePathFree(VOLUME_PATH* Path)
{
    free(Path->Category);
    free(Path->PackagePrefix);
    free(Path->DocumentName);
    memset(Path, 0, sizeof *Path);
}

/* resolves a path below a category against the listing of the category
 * (REQ-MAP-015)
 */
static NTSTATUS ResolveCategoryPath(PTFS* Ptfs, PCWSTR Category, PWSTR* Segments, ULONG Count,
    VOLUME_PATH* Path)
{
    PTFS_DESCRIPTOR* Descriptors = 0;
    ULONG DescriptorCount = 0, Index;
    NTSTATUS Result;

    Result = GetCategoryDescriptors(Ptfs, Category, &Descriptors, &DescriptorCount);
    if (!NT_SUCCESS(Result))
        return Result;

    for (Index = 0; Index < DescriptorCount; Index++)
    {
        PWSTR* DocumentSegments = 0;
        ULONG DocumentSegmentCount, SegmentIndex;
        ULONG PackageCount;
        BOOLEAN Match = TRUE;

        if (Descriptors[Index].Generated)
            continue;

        DocumentSegmentCount = SplitDocumentName(Descriptors[Index].Name, &DocumentSegments);
        if (0 == DocumentSegmentCount)
            continue;

        /* a document whose name cannot be represented is not exposed and
         * resolves to nothing (REQ-MAP-013)
         */
        for (SegmentIndex = 0; SegmentIndex < DocumentSegmentCount; SegmentIndex++)
            if (!IsLegalPathSegment(DocumentSegments[SegmentIndex]))
                break;
        if (SegmentIndex < DocumentSegmentCount)
        {
            FreeSegments(DocumentSegments, DocumentSegmentCount);
            continue;
        }

        PackageCount = DocumentSegmentCount - 1;

        /* the path is a package level, when a document of the category has it
         * as the beginning of its package path (REQ-MAP-007)
         */
        if (PackageCount >= Count)
        {
            for (SegmentIndex = 0; SegmentIndex < Count; SegmentIndex++)
                if (0 != _wcsicmp(DocumentSegments[SegmentIndex], Segments[SegmentIndex]))
                {
                    Match = FALSE;
                    break;
                }
            if (Match)
            {
                Path->Kind = VOLUME_PATH_PACKAGE;
                /* the canonical spelling of the level is the one of the
                 * server, not the one of the request (REQ-FS-004)
                 */
                Path->PackagePrefix = JoinDotted(DocumentSegments, Count);
                FreeSegments(DocumentSegments, DocumentSegmentCount);
                FreeDescriptors(Descriptors, DescriptorCount);
                return STATUS_SUCCESS;
            }
        }

        /* the path is a document */
        Match = (PackageCount == Count - 1);
        for (SegmentIndex = 0; Match && SegmentIndex + 1 < Count; SegmentIndex++)
            if (0 != _wcsicmp(DocumentSegments[SegmentIndex], Segments[SegmentIndex]))
                Match = FALSE;
        if (Match && 0 == _wcsicmp(DocumentSegments[DocumentSegmentCount - 1], Segments[Count - 1]))
        {
            Path->Kind = VOLUME_PATH_DOCUMENT;
            Path->DocumentName = DupString(Descriptors[Index].Name);
            FreeSegments(DocumentSegments, DocumentSegmentCount);
            FreeDescriptors(Descriptors, DescriptorCount);
            return STATUS_SUCCESS;
        }

        FreeSegments(DocumentSegments, DocumentSegmentCount);
    }

    FreeDescriptors(Descriptors, DescriptorCount);

    Path->Kind = VOLUME_PATH_INVALID;

    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/* resolves a path of the volume: the root, a category, a package level or a
 * document (overview §3, REQ-MAP-015)
 */
static NTSTATUS ResolveVolumePath(PTFS* Ptfs, PCWSTR FileName, VOLUME_PATH* Path)
{
    PWSTR* Segments = 0;
    ULONG Count;
    int Index;
    NTSTATUS Result;

    memset(Path, 0, sizeof *Path);

    if (IsRootFileName(FileName))
    {
        Path->Kind = VOLUME_PATH_ROOT;
        return STATUS_SUCCESS;
    }

    Count = SplitByDelimiter(FileName, L'\\', L'/', &Segments);
    if (0 == Count)
    {
        Path->Kind = VOLUME_PATH_INVALID;
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    /* the first segment of a path is the category of the document
     * (REQ-MAP-003)
     */
    Index = CategoryIndex(Segments[0]);
    if (Index < 0)
    {
        FreeSegments(Segments, Count);
        Path->Kind = VOLUME_PATH_INVALID;
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    Path->Category = DupString(Categories[Index]);
    if (0 == Path->Category)
    {
        FreeSegments(Segments, Count);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (1 == Count)
    {
        Path->Kind = VOLUME_PATH_CATEGORY;
        FreeSegments(Segments, Count);
        return STATUS_SUCCESS;
    }

    Result = ResolveCategoryPath(Ptfs, Path->Category, Segments + 1, Count - 1, Path);

    FreeSegments(Segments, Count);

    return Result;
}

static NTSTATUS GetFileInfoInternal(HANDLE Handle, FSP_FSCTL_FILE_INFO* FileInfo)
{
    BY_HANDLE_FILE_INFORMATION ByHandleFileInfo;

    if (!GetFileInformationByHandle(Handle, &ByHandleFileInfo))
        return FspNtStatusFromWin32(GetLastError());

    FileInfo->FileAttributes = ByHandleFileInfo.dwFileAttributes;
    FileInfo->ReparseTag = 0;
    FileInfo->FileSize =
        ((UINT64)ByHandleFileInfo.nFileSizeHigh << 32) | (UINT64)ByHandleFileInfo.nFileSizeLow;
    FileInfo->AllocationSize = (FileInfo->FileSize + ALLOCATION_UNIT - 1)
        / ALLOCATION_UNIT * ALLOCATION_UNIT;
    FileInfo->CreationTime = ((PLARGE_INTEGER)&ByHandleFileInfo.ftCreationTime)->QuadPart;
    FileInfo->LastAccessTime = ((PLARGE_INTEGER)&ByHandleFileInfo.ftLastAccessTime)->QuadPart;
    FileInfo->LastWriteTime = ((PLARGE_INTEGER)&ByHandleFileInfo.ftLastWriteTime)->QuadPart;
    FileInfo->ChangeTime = FileInfo->LastWriteTime;
    FileInfo->IndexNumber =
        ((UINT64)ByHandleFileInfo.nFileIndexHigh << 32) | (UINT64)ByHandleFileInfo.nFileIndexLow;
    FileInfo->HardLinks = 0;

    return STATUS_SUCCESS;
}

static NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM* FileSystem,
    FSP_FSCTL_VOLUME_INFO* VolumeInfo)
{
    /* the remote volume has no quota API and no local backing store: 0/0 means
     * "size unknown" (REQ-MOUNT-008)
     */
    VolumeInfo->TotalSize = 0;
    VolumeInfo->FreeSize = 0;

    /* the volume label does not depend on the server (REQ-MOUNT-006) */
    memset(VolumeInfo->VolumeLabel, 0, sizeof VolumeInfo->VolumeLabel);
    StringCchCopyW(VolumeInfo->VolumeLabel,
        sizeof VolumeInfo->VolumeLabel / sizeof(WCHAR), L"" VOLUME_NAME);
    VolumeInfo->VolumeLabelLength =
        (UINT16)(wcslen(VolumeInfo->VolumeLabel) * sizeof(WCHAR));

    return STATUS_SUCCESS;
}

static NTSTATUS SetVolumeLabel_(FSP_FILE_SYSTEM* FileSystem,
    PWSTR VolumeLabel,
    FSP_FSCTL_VOLUME_INFO* VolumeInfo)
{
    /* we do not support changing the volume label */
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ISFS has no server-side ACL model: the descriptor is synthesized and never
 * read from or written to the server (overview §6). "O:BAG:BAD:(A;;FA;;;WD)" grants
 * full access to everyone, which is the "full access" policy for the volume.
 * 
 * O:BAG:BA — owner and group (BUILTIN\Administrators), FA = FILE_ALL_ACCESS (0x1F01FF) instead if generic GA.
 */
static PSECURITY_DESCRIPTOR BuildFullAccessSecurityDescriptor(VOID)
{
    PSECURITY_DESCRIPTOR SecurityDescriptor = 0;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:BAG:BAD:(A;;FA;;;WD)", SDDL_REVISION_1, &SecurityDescriptor, 0))
        return 0;

    return SecurityDescriptor;
}

/* shared by GetSecurity and GetSecurityByName so both return the same
 * synthesized descriptor with full access
 */
static NTSTATUS CopyFullAccessSecurityDescriptor(
    PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T* PSecurityDescriptorSize)
{
    PSECURITY_DESCRIPTOR FullAccess;
    SIZE_T Size;
    NTSTATUS Result = STATUS_SUCCESS;

    if (0 == PSecurityDescriptorSize)
        return STATUS_SUCCESS;

    FullAccess = BuildFullAccessSecurityDescriptor();
    if (0 == FullAccess)
        return FspNtStatusFromWin32(GetLastError());

    Size = GetSecurityDescriptorLength(FullAccess);

    if (0 != SecurityDescriptor)
    {
        if (*PSecurityDescriptorSize < Size)
            Result = STATUS_BUFFER_TOO_SMALL;
        else
            memcpy(SecurityDescriptor, FullAccess, Size);
    }

    *PSecurityDescriptorSize = Size;

    LocalFree(FullAccess);

    return Result;
}

static NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM* FileSystem,
    PWSTR FileName, PUINT32 PFileAttributes,
    PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T* PSecurityDescriptorSize)
{
    PTFS* Ptfs = (PTFS*)FileSystem->UserContext;
    VOLUME_PATH Path;
    NTSTATUS Result;

    /* attributes are synthesized from the resolved path: a category or a
     * package level is a directory, a document is a file (REQ-MAP-001,
     * REQ-FS-005). The resolution also rejects names that do not exist.
     */
    if (0 != PFileAttributes)
    {
        Result = ResolveVolumePath(Ptfs, FileName, &Path);
        if (!NT_SUCCESS(Result))
        {
            VolumePathFree(&Path);
            return Result;
        }

        switch (Path.Kind)
        {
        case VOLUME_PATH_ROOT:
        case VOLUME_PATH_CATEGORY:
        case VOLUME_PATH_PACKAGE:
            *PFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            break;
        default:
            *PFileAttributes = FILE_ATTRIBUTE_NORMAL;
            break;
        }

        VolumePathFree(&Path);
    }

    Result = STATUS_SUCCESS;

    if (0 != PSecurityDescriptorSize)
        Result = CopyFullAccessSecurityDescriptor(SecurityDescriptor, PSecurityDescriptorSize);

    return Result;
}

static NTSTATUS Create(FSP_FILE_SYSTEM* FileSystem,
    PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess,
    UINT32 FileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor, UINT64 AllocationSize,
    PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{
    /* the namespace root already exists and cannot be created (REQ-MAP-001) */
    if (IsRootFileName(FileName))
        return STATUS_OBJECT_NAME_COLLISION;

    /* the directories of the volume are synthetic and read-only: they cannot
     * be created (REQ-FS-037)
     */
    if (CreateOptions & FILE_DIRECTORY_FILE)
        return STATUS_NOT_SUPPORTED;

    /* a document is created on the server by the flush of its handle; the
     * write of a document is not implemented yet (REQ-FS-018)
     */
    warn(L"the document %s cannot be created yet", FileName);

    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS Open(FSP_FILE_SYSTEM* FileSystem,
    PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess,
    PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{
    PTFS* Ptfs = (PTFS*)FileSystem->UserContext;
    PTFS_FILE_CONTEXT* FileContext;
    VOLUME_PATH Path;
    NTSTATUS Result;

    /* the root is a synthetic directory backed by the -ns namespace, not by
     * a local path (REQ-MAP-001)
     */
    if (IsRootFileName(FileName))
    {
        FileContext = malloc(sizeof * FileContext);
        if (0 == FileContext)
            return STATUS_INSUFFICIENT_RESOURCES;
        memset(FileContext, 0, sizeof * FileContext);
        FileContext->IsRoot = TRUE;
        FileContext->IsSynthetic = TRUE;
        FileContext->Handle = INVALID_HANDLE_VALUE;

        *PFileContext = FileContext;

        return GetSyntheticDirInfo(FileInfo, HashEntryName(Ptfs->Namespace, L""), 0);
    }

    /* the path is resolved against the listing of its category (REQ-MAP-015) */
    Result = ResolveVolumePath(Ptfs, FileName, &Path);
    if (!NT_SUCCESS(Result))
    {
        VolumePathFree(&Path);
        return NT_SUCCESS(Result) ? STATUS_OBJECT_NAME_NOT_FOUND : Result;
    }

    if (VOLUME_PATH_DOCUMENT == Path.Kind)
    {
        PBYTE Content = 0;
        SIZE_T ContentSize = 0;
        UINT64 Timestamp = 0;

        /* the content is fetched once per open handle and served from memory
         * (REQ-FS-013, REQ-FS-014)
         */
        Result = LoadDocument(Ptfs, Path.DocumentName, &Content, &ContentSize, &Timestamp);
        if (!NT_SUCCESS(Result))
        {
            VolumePathFree(&Path);
            return Result;
        }

        FileContext = malloc(sizeof * FileContext);
        if (0 == FileContext)
        {
            free(Content);
            VolumePathFree(&Path);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        memset(FileContext, 0, sizeof * FileContext);
        FileContext->IsDocument = TRUE;
        FileContext->Handle = INVALID_HANDLE_VALUE;
        FileContext->DocumentName = Path.DocumentName;
        Path.DocumentName = 0;
        FileContext->Content = Content;
        FileContext->ContentSize = ContentSize;
        FileContext->Timestamp = Timestamp;

        VolumePathFree(&Path);

        *PFileContext = FileContext;

        /* the exact size is the byte length of the decoded content (REQ-FS-006) */
        return GetDocumentInfo(Ptfs, FileContext, FileInfo);
    }

    if (VOLUME_PATH_CATEGORY != Path.Kind && VOLUME_PATH_PACKAGE != Path.Kind)
    {
        VolumePathFree(&Path);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    FileContext = malloc(sizeof * FileContext);
    if (0 == FileContext)
    {
        VolumePathFree(&Path);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    memset(FileContext, 0, sizeof * FileContext);
    FileContext->IsSynthetic = TRUE;
    FileContext->Handle = INVALID_HANDLE_VALUE;
    FileContext->Category = Path.Category;
    FileContext->PackagePrefix = Path.PackagePrefix;
    Path.Category = 0;
    Path.PackagePrefix = 0;
    VolumePathFree(&Path);

    *PFileContext = FileContext;

    /* the index number of a synthetic directory is derived from its name
     * (REQ-FS-012)
     */
    return GetSyntheticDirInfo(FileInfo,
        HashEntryName(Ptfs->Namespace,
            0 != FileContext->PackagePrefix ? FileContext->PackagePrefix : FileContext->Category),
        0);
}

static NTSTATUS Overwrite(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, UINT32 FileAttributes, BOOLEAN ReplaceFileAttributes, UINT64 AllocationSize,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    HANDLE Handle = HandleFromContext(FileContext);
    FILE_BASIC_INFO BasicInfo = { 0 };
    FILE_ALLOCATION_INFO AllocationInfo = { 0 };
    FILE_ATTRIBUTE_TAG_INFO AttributeTagInfo;

    if (ReplaceFileAttributes)
    {
        if (0 == FileAttributes)
            FileAttributes = FILE_ATTRIBUTE_NORMAL;

        BasicInfo.FileAttributes = FileAttributes;
        if (!SetFileInformationByHandle(Handle,
            FileBasicInfo, &BasicInfo, sizeof BasicInfo))
            return FspNtStatusFromWin32(GetLastError());
    }
    else if (0 != FileAttributes)
    {
        if (!GetFileInformationByHandleEx(Handle,
            FileAttributeTagInfo, &AttributeTagInfo, sizeof AttributeTagInfo))
            return FspNtStatusFromWin32(GetLastError());

        BasicInfo.FileAttributes = FileAttributes | AttributeTagInfo.FileAttributes;
        if (BasicInfo.FileAttributes ^ FileAttributes)
        {
            if (!SetFileInformationByHandle(Handle,
                FileBasicInfo, &BasicInfo, sizeof BasicInfo))
                return FspNtStatusFromWin32(GetLastError());
        }
    }

    if (!SetFileInformationByHandle(Handle,
        FileAllocationInfo, &AllocationInfo, sizeof AllocationInfo))
        return FspNtStatusFromWin32(GetLastError());

    return GetFileInfoInternal(Handle, FileInfo);
}

static VOID Cleanup(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, PWSTR FileName, ULONG Flags)
{
    PTFS_FILE_CONTEXT* Context = FileContext;
    HANDLE Handle = HandleFromContext(FileContext);

    /* synthetic directories and documents have no local handle and cannot be
     * deleted (REQ-MAP-001, REQ-FS-037)
     */
    if (Context->IsSynthetic || Context->IsDocument)
        return;

    if (Flags & FspCleanupDelete)
    {
        CloseHandle(Handle);

        /* this will make all future uses of Handle to fail with STATUS_INVALID_HANDLE */
        HandleFromContext(FileContext) = INVALID_HANDLE_VALUE;
    }
}

static VOID Close(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext0)
{
    PTFS_FILE_CONTEXT* FileContext = FileContext0;
    HANDLE Handle = HandleFromContext(FileContext);

    if (!FileContext->IsSynthetic && !FileContext->IsDocument)
        CloseHandle(Handle);

    free(FileContext->Category);
    free(FileContext->PackagePrefix);
    free(FileContext->DocumentName);
    free(FileContext->Content);

    FspFileSystemDeleteDirectoryBuffer(&FileContext->DirBuffer);
    free(FileContext);
}

static NTSTATUS Read(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length,
    PULONG PBytesTransferred)
{
    PTFS_FILE_CONTEXT* Context = FileContext;
    HANDLE Handle = HandleFromContext(FileContext);
    OVERLAPPED Overlapped = { 0 };

    /* a document is served from the content held in memory; the API has no
     * byte ranges (REQ-FS-014)
     */
    if (Context->IsDocument)
    {
        UINT64 Size = (UINT64)Context->ContentSize;

        if (Offset >= Size)
        {
            *PBytesTransferred = 0;
            return STATUS_SUCCESS;
        }

        if ((UINT64)Length > Size - Offset)
            Length = (ULONG)(Size - Offset);

        memcpy(Buffer, Context->Content + (SIZE_T)Offset, Length);
        *PBytesTransferred = Length;

        return STATUS_SUCCESS;
    }

    Overlapped.Offset = (DWORD)Offset;
    Overlapped.OffsetHigh = (DWORD)(Offset >> 32);

    if (!ReadFile(Handle, Buffer, Length, PBytesTransferred, &Overlapped))
        return FspNtStatusFromWin32(GetLastError());

    return STATUS_SUCCESS;
}

static NTSTATUS Write(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length,
    BOOLEAN WriteToEndOfFile, BOOLEAN ConstrainedIo,
    PULONG PBytesTransferred, FSP_FSCTL_FILE_INFO* FileInfo)
{
    HANDLE Handle = HandleFromContext(FileContext);
    LARGE_INTEGER FileSize;
    OVERLAPPED Overlapped = { 0 };

    if (ConstrainedIo)
    {
        if (!GetFileSizeEx(Handle, &FileSize))
            return FspNtStatusFromWin32(GetLastError());

        if (Offset >= (UINT64)FileSize.QuadPart)
            return STATUS_SUCCESS;
        if (Offset + Length > (UINT64)FileSize.QuadPart)
            Length = (ULONG)((UINT64)FileSize.QuadPart - Offset);
    }

    Overlapped.Offset = (DWORD)Offset;
    Overlapped.OffsetHigh = (DWORD)(Offset >> 32);

    if (!WriteFile(Handle, Buffer, Length, PBytesTransferred, &Overlapped))
        return FspNtStatusFromWin32(GetLastError());

    return GetFileInfoInternal(Handle, FileInfo);
}

NTSTATUS Flush(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    HANDLE Handle = HandleFromContext(FileContext);

    /* we do not flush the whole volume, so just return SUCCESS */
    if (0 == Handle)
        return STATUS_SUCCESS;

    if (!FlushFileBuffers(Handle))
        return FspNtStatusFromWin32(GetLastError());

    return GetFileInfoInternal(Handle, FileInfo);
}

static NTSTATUS GetFileInfo(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    PTFS* Ptfs = (PTFS*)FileSystem->UserContext;
    PTFS_FILE_CONTEXT* Context = FileContext;

    /* the synthetic directories have no local handle: their metadata is
     * generated (REQ-MAP-001)
     */
    if (Context->IsSynthetic)
        return GetSyntheticDirInfo(FileInfo,
            HashEntryName(Ptfs->Namespace,
                0 != Context->PackagePrefix ? Context->PackagePrefix :
                (0 != Context->Category ? Context->Category : L"")),
            Context->Timestamp);

    /* a document has no local handle: its size and timestamp come from the
     * content buffer and the server ts (REQ-FS-005, REQ-FS-006)
     */
    if (Context->IsDocument)
        return GetDocumentInfo(Ptfs, Context, FileInfo);

    return GetFileInfoInternal(HandleFromContext(FileContext), FileInfo);
}

static NTSTATUS SetBasicInfo(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, UINT32 FileAttributes,
    UINT64 CreationTime, UINT64 LastAccessTime, UINT64 LastWriteTime, UINT64 ChangeTime,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    HANDLE Handle = HandleFromContext(FileContext);
    FILE_BASIC_INFO BasicInfo = { 0 };

    if (INVALID_FILE_ATTRIBUTES == FileAttributes)
        FileAttributes = 0;
    else if (0 == FileAttributes)
        FileAttributes = FILE_ATTRIBUTE_NORMAL;

    BasicInfo.FileAttributes = FileAttributes;
    BasicInfo.CreationTime.QuadPart = CreationTime;
    BasicInfo.LastAccessTime.QuadPart = LastAccessTime;
    BasicInfo.LastWriteTime.QuadPart = LastWriteTime;
    //BasicInfo.ChangeTime = ChangeTime;

    if (!SetFileInformationByHandle(Handle,
        FileBasicInfo, &BasicInfo, sizeof BasicInfo))
        return FspNtStatusFromWin32(GetLastError());

    return GetFileInfoInternal(Handle, FileInfo);
}

static NTSTATUS SetFileSize(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, UINT64 NewSize, BOOLEAN SetAllocationSize,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    HANDLE Handle = HandleFromContext(FileContext);
    FILE_ALLOCATION_INFO AllocationInfo;
    FILE_END_OF_FILE_INFO EndOfFileInfo;

    if (SetAllocationSize)
    {
        /*
         * This file system does not maintain AllocationSize, although NTFS clearly can.
         * However it must always be FileSize <= AllocationSize and NTFS will make sure
         * to truncate the FileSize if it sees an AllocationSize < FileSize.
         *
         * If OTOH a very large AllocationSize is passed, the call below will increase
         * the AllocationSize of the underlying file, although our file system does not
         * expose this fact. This AllocationSize is only temporary as NTFS will reset
         * the AllocationSize of the underlying file when it is closed.
         */

        AllocationInfo.AllocationSize.QuadPart = NewSize;

        if (!SetFileInformationByHandle(Handle,
            FileAllocationInfo, &AllocationInfo, sizeof AllocationInfo))
            return FspNtStatusFromWin32(GetLastError());
    }
    else
    {
        EndOfFileInfo.EndOfFile.QuadPart = NewSize;

        if (!SetFileInformationByHandle(Handle,
            FileEndOfFileInfo, &EndOfFileInfo, sizeof EndOfFileInfo))
            return FspNtStatusFromWin32(GetLastError());
    }

    return GetFileInfoInternal(Handle, FileInfo);
}

static NTSTATUS Rename(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext,
    PWSTR FileName, PWSTR NewFileName, BOOLEAN ReplaceIfExists)
{
    PTFS* Ptfs = (PTFS*)FileSystem->UserContext;
    WCHAR FullPath[FULLPATH_SIZE], NewFullPath[FULLPATH_SIZE];

    if (!ConcatPath(Ptfs, FileName, FullPath))
        return STATUS_OBJECT_NAME_INVALID;

    if (!ConcatPath(Ptfs, NewFileName, NewFullPath))
        return STATUS_OBJECT_NAME_INVALID;

    if (!MoveFileExW(FullPath, NewFullPath, ReplaceIfExists ? MOVEFILE_REPLACE_EXISTING : 0))
        return FspNtStatusFromWin32(GetLastError());

    return STATUS_SUCCESS;
}

static NTSTATUS GetSecurity(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext,
    PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T* PSecurityDescriptorSize)
{
    /* every object exposes the same synthesized descriptor with full access;
     * nothing is read from a local handle (REQ-MAP-001)
     */
    return CopyFullAccessSecurityDescriptor(SecurityDescriptor, PSecurityDescriptorSize);
}

static NTSTATUS SetSecurity(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext,
    SECURITY_INFORMATION SecurityInformation, PSECURITY_DESCRIPTOR ModificationDescriptor)
{
    HANDLE Handle = HandleFromContext(FileContext);

    if (!SetKernelObjectSecurity(Handle, SecurityInformation, ModificationDescriptor))
        return FspNtStatusFromWin32(GetLastError());

    return STATUS_SUCCESS;
}

static NTSTATUS ReadDirectory(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext0, PWSTR Pattern, PWSTR Marker,
    PVOID Buffer, ULONG BufferLength, PULONG PBytesTransferred)
{
    PTFS* Ptfs = (PTFS*)FileSystem->UserContext;
    PTFS_FILE_CONTEXT* FileContext = FileContext0;
    ULONG Length, Index;
    union
    {
        UINT8 B[FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + MAX_PATH * sizeof(WCHAR)];
        FSP_FSCTL_DIR_INFO D;
    } DirInfoBuf;
    FSP_FSCTL_DIR_INFO* DirInfo = &DirInfoBuf.D;
    NTSTATUS DirBufferResult;

    DirBufferResult = STATUS_SUCCESS;
    if (FspFileSystemAcquireDirectoryBuffer(&FileContext->DirBuffer, 0 == Marker, &DirBufferResult))
    {
        if (0 == Pattern)
            Pattern = L"*";

        if (FileContext->IsRoot)
        {
            /* REQ-MAP-001, REQ-MAP-002: the root is the -ns namespace and lists
             * only the synthetic category directories; documents and the
             * namespace name itself are never root entries, and no local path is
             * touched.
             */
            for (Index = 0; Index < CATEGORY_COUNT; Index++)
            {
                if (!PtfsMatchPattern(Pattern, Categories[Index]))
                    continue;

                memset(DirInfo, 0, sizeof * DirInfo);
                Length = (ULONG)wcslen(Categories[Index]);
                DirInfo->Size = (UINT16)(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + Length * sizeof(WCHAR));
                GetSyntheticDirInfo(&DirInfo->FileInfo,
                    HashEntryName(Ptfs->Namespace, Categories[Index]), 0);
                memcpy(DirInfo->FileNameBuf, Categories[Index], Length * sizeof(WCHAR));

                if (!FspFileSystemFillDirectoryBuffer(&FileContext->DirBuffer, DirInfo, &DirBufferResult))
                    break;
            }
        }
        else if (FileContext->IsSynthetic)
        {
            /* REQ-FS-001: the contents of a category directory are the
             * documents of that category, taken from
             * GET /:namespace/docnames/:cat. A package level is served from the
             * listing of its category, filtered by the package prefix
             * (REQ-MAP-008).
             */
            PTFS_DIR_ENTRY* Entries = 0;
            ULONG EntryCount = 0;
            NTSTATUS Result;

            Result = BuildCategoryEntries(Ptfs, FileContext->Category,
                FileContext->PackagePrefix, &Entries, &EntryCount);
            if (!NT_SUCCESS(Result))
            {
                DirBufferResult = Result;
            }
            else
            {
                for (Index = 0; Index < EntryCount; Index++)
                {
                    if (!PtfsMatchPattern(Pattern, Entries[Index].Name))
                        continue;

                    Length = (ULONG)wcslen(Entries[Index].Name);
                    if (Length >= MAX_PATH)
                    {
                        warn(L"the name %s of %s is too long and is not listed",
                            Entries[Index].Name, FileContext->Category);
                        continue;
                    }

                    memset(DirInfo, 0, sizeof * DirInfo);
                    DirInfo->Size = (UINT16)(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + Length * sizeof(WCHAR));
                    /* a document is a file, a package level is a directory
                     * (REQ-FS-008)
                     */
                    DirInfo->FileInfo.FileAttributes = Entries[Index].IsDirectory ?
                        FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                    /* the listing reports no size; the size is exact only
                     * after the content was fetched (REQ-FS-003)
                     */
                    DirInfo->FileInfo.FileSize = 0;
                    DirInfo->FileInfo.AllocationSize = 0;
                    DirInfo->FileInfo.CreationTime = Entries[Index].Timestamp;
                    DirInfo->FileInfo.LastAccessTime = Entries[Index].Timestamp;
                    DirInfo->FileInfo.LastWriteTime = Entries[Index].Timestamp;
                    DirInfo->FileInfo.ChangeTime = Entries[Index].Timestamp;
                    DirInfo->FileInfo.IndexNumber = Entries[Index].IndexNumber;
                    DirInfo->FileInfo.HardLinks = 0;
                    memcpy(DirInfo->FileNameBuf, Entries[Index].Name, Length * sizeof(WCHAR));

                    if (!FspFileSystemFillDirectoryBuffer(&FileContext->DirBuffer, DirInfo, &DirBufferResult))
                        break;
                }

                FreeDirEntries(Entries, EntryCount);
            }
        }
        else
        {
            /* the volume has no local directories: every directory of the
             * tree is synthetic
             */
            DirBufferResult = STATUS_OBJECT_NAME_INVALID;
        }

        FspFileSystemReleaseDirectoryBuffer(&FileContext->DirBuffer);
    }

    if (!NT_SUCCESS(DirBufferResult))
        return DirBufferResult;

    FspFileSystemReadDirectoryBuffer(&FileContext->DirBuffer,
        Marker, Buffer, BufferLength, PBytesTransferred);

    return STATUS_SUCCESS;
}

static NTSTATUS SetDelete(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, PWSTR FileName, BOOLEAN DeleteFile)
{
    HANDLE Handle = HandleFromContext(FileContext);
    FILE_DISPOSITION_INFO DispositionInfo;

    DispositionInfo.DeleteFile = DeleteFile;

    if (!SetFileInformationByHandle(Handle,
        FileDispositionInfo, &DispositionInfo, sizeof DispositionInfo))
        return FspNtStatusFromWin32(GetLastError());

    return STATUS_SUCCESS;
}

static FSP_FILE_SYSTEM_INTERFACE PtfsInterface =
{
    .GetVolumeInfo = GetVolumeInfo,
    .SetVolumeLabel = SetVolumeLabel_,
    .GetSecurityByName = GetSecurityByName,
    .Create = Create,
    .Open = Open,
    .Overwrite = Overwrite,
    .Cleanup = Cleanup,
    .Close = Close,
    .Read = Read,
    .Write = Write,
    .Flush = Flush,
    .GetFileInfo = GetFileInfo,
    .SetBasicInfo = SetBasicInfo,
    .SetFileSize = SetFileSize,
    .Rename = Rename,
    .GetSecurity = GetSecurity,
    .SetSecurity = SetSecurity,
    .ReadDirectory = ReadDirectory,
    .SetDelete = SetDelete,
};

static VOID PtfsDelete(PTFS* Ptfs);

/* the client of the mount is opened and probed by the caller and is handed
 * over to the volume here; the volume owns it from that moment on
 * (REQ-CONN-021)
 */
static NTSTATUS PtfsCreate(PWSTR Namespace, ATELIER_CLIENT* Client, PWSTR BaseUrl,
    PWSTR UserName, PWSTR Password,
    PWSTR MountPoint, UINT32 DebugFlags,
    PTFS** PPtfs)
{
    FILETIME CreationTime;
    FSP_FSCTL_VOLUME_PARAMS VolumeParams;
    PTFS* Ptfs = 0;
    SIZE_T Length;
    ULONG Index;
    NTSTATUS Result;

    *PPtfs = 0;

    Ptfs = malloc(sizeof * Ptfs);
    if (0 == Ptfs)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }
    memset(Ptfs, 0, sizeof * Ptfs);

    /* from now on we must goto exit on failure */

    /* the caches of the categories are guarded by their own lock
     * (overview §9)
     */
    for (Index = 0; Index < CATEGORY_COUNT; Index++)
        InitializeCriticalSection(&Ptfs->Cache[Index].Lock);

    Length = (wcslen(Namespace) + 1) * sizeof(WCHAR);
    Ptfs->Namespace = malloc(Length);
    if (0 == Ptfs->Namespace)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }
    memcpy(Ptfs->Namespace, Namespace, Length);

    Length = (wcslen(BaseUrl) + 1) * sizeof(WCHAR);
    Ptfs->BaseUrl = malloc(Length);
    if (0 == Ptfs->BaseUrl)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }
    memcpy(Ptfs->BaseUrl, BaseUrl, Length);

    /* the volume takes over the client of the mount */
    Ptfs->Client = *Client;
    Client->Session = 0;

    Length = (wcslen(UserName) + 1) * sizeof(WCHAR);
    Ptfs->UserName = malloc(Length);
    if (0 == Ptfs->UserName)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }
    memcpy(Ptfs->UserName, UserName, Length);

    Length = (wcslen(Password) + 1) * sizeof(WCHAR);
    Ptfs->Password = malloc(Length);
    if (0 == Ptfs->Password)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }
    memcpy(Ptfs->Password, Password, Length);

    GetSystemTimeAsFileTime(&CreationTime);

    memset(&VolumeParams, 0, sizeof VolumeParams);
    VolumeParams.SectorSize = ALLOCATION_UNIT;
    VolumeParams.SectorsPerAllocationUnit = 1;
    VolumeParams.VolumeCreationTime = ((PLARGE_INTEGER)&CreationTime)->QuadPart;
    VolumeParams.VolumeSerialNumber = 0;
    VolumeParams.FileInfoTimeout = 1000;
    VolumeParams.CaseSensitiveSearch = 0;
    VolumeParams.CasePreservedNames = 1;
    VolumeParams.UnicodeOnDisk = 1;
    VolumeParams.PersistentAcls = 1;
    VolumeParams.PostCleanupWhenModifiedOnly = 1;
    VolumeParams.PassQueryDirectoryPattern = 1;
    VolumeParams.FlushAndPurgeOnCleanup = 1;
    VolumeParams.UmFileContextIsUserContext2 = 1;
    /* REQ-MOUNT-028: the volume is a local drive only; a UNC prefix (network
     * share) is not supported, so VolumeParams.Prefix stays empty
     */
    wcscpy_s(VolumeParams.FileSystemName, sizeof VolumeParams.FileSystemName / sizeof(WCHAR),
        L"" VOLUME_NAME);

    Result = FspFileSystemCreate(
        L"" FSP_FSCTL_DISK_DEVICE_NAME,
        &VolumeParams,
        &PtfsInterface,
        &Ptfs->FileSystem);
    if (!NT_SUCCESS(Result))
        goto exit;
    Ptfs->FileSystem->UserContext = Ptfs;

    Result = FspFileSystemSetMountPoint(Ptfs->FileSystem, MountPoint);
    if (!NT_SUCCESS(Result))
        goto exit;

    FspFileSystemSetDebugLog(Ptfs->FileSystem, DebugFlags);

    Result = STATUS_SUCCESS;

exit:
    if (NT_SUCCESS(Result))
        *PPtfs = Ptfs;
    else if (0 != Ptfs)
        PtfsDelete(Ptfs);

    return Result;
}

static VOID PtfsDelete(PTFS* Ptfs)
{
    ULONG Index;

    if (0 != Ptfs->FileSystem)
        FspFileSystemDelete(Ptfs->FileSystem);

    for (Index = 0; Index < CATEGORY_COUNT; Index++)
    {
        DeleteCriticalSection(&Ptfs->Cache[Index].Lock);
        FreeDescriptors(Ptfs->Cache[Index].Descriptors, Ptfs->Cache[Index].Count);
    }

    AtelierClientClose(&Ptfs->Client);

    if (0 != Ptfs->BaseUrl)
        free(Ptfs->BaseUrl);

    if (0 != Ptfs->Namespace)
        free(Ptfs->Namespace);

    if (0 != Ptfs->Password)
    {
        /* do not leave the password in freed memory (REQ-MOUNT-010) */
        SecureZeroMemory(Ptfs->Password, wcslen(Ptfs->Password) * sizeof(WCHAR));
        free(Ptfs->Password);
    }

    if (0 != Ptfs->UserName)
    {
        /* the login is wiped as well (REQ-CONN-011) */
        SecureZeroMemory(Ptfs->UserName, wcslen(Ptfs->UserName) * sizeof(WCHAR));
        free(Ptfs->UserName);
    }

    free(Ptfs);
}

static NTSTATUS EnableBackupRestorePrivileges(VOID)
{
    union
    {
        TOKEN_PRIVILEGES P;
        UINT8 B[sizeof(TOKEN_PRIVILEGES) + sizeof(LUID_AND_ATTRIBUTES)];
    } Privileges;
    HANDLE Token;

    Privileges.P.PrivilegeCount = 2;
    Privileges.P.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    Privileges.P.Privileges[1].Attributes = SE_PRIVILEGE_ENABLED;

    if (!LookupPrivilegeValueW(0, SE_BACKUP_NAME, &Privileges.P.Privileges[0].Luid) ||
        !LookupPrivilegeValueW(0, SE_RESTORE_NAME, &Privileges.P.Privileges[1].Luid))
        return FspNtStatusFromWin32(GetLastError());

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &Token))
        return FspNtStatusFromWin32(GetLastError());

    if (!AdjustTokenPrivileges(Token, FALSE, &Privileges.P, 0, 0, 0))
    {
        CloseHandle(Token);

        return FspNtStatusFromWin32(GetLastError());
    }

    CloseHandle(Token);

    return STATUS_SUCCESS;
}

static ULONG wcstol_deflt(wchar_t* w, ULONG deflt)
{
    wchar_t* endp;
    ULONG ul = wcstol(w, &endp, 0);
    return L'\0' != w[0] && L'\0' == *endp ? ul : deflt;
}

/* builds the namespace path segment of the base URL from the -ns value.
 * The value is used as is except for '%', which is percent-encoded because a
 * literal '%' is not a valid URL escape (api.md §2): USER stays USER, %SYS
 * becomes %25SYS.
 */
static BOOL ComposeNamespaceSegment(PCWSTR Namespace, PWSTR Buffer, SIZE_T BufferCch)
{
    WCHAR Ch;

    for (; L'\0' != (Ch = *Namespace); Namespace++)
    {
        if (L'%' == Ch)
        {
            if (BufferCch < 4)
                return FALSE;
            *Buffer++ = L'%';
            *Buffer++ = L'2';
            *Buffer++ = L'5';
            BufferCch -= 3;
        }
        else
        {
            if (BufferCch < 2)
                return FALSE;
            *Buffer++ = Ch;
            BufferCch -= 1;
        }
    }

    *Buffer = L'\0';

    return TRUE;
}

/* REQ-MOUNT-003: the mount point must be a single drive letter (X:).
 * WinFsp would also accept a directory path as a mount point, which is not
 * supported, so the form is checked before anything is mounted.
 */
static BOOL IsDriveLetterMountPoint(PCWSTR MountPoint)
{
    if (0 == MountPoint)
        return FALSE;

    return L'\0' != MountPoint[0] &&
        L':' == MountPoint[1] &&
        L'\0' == MountPoint[2] &&
        ((L'A' <= MountPoint[0] && L'Z' >= MountPoint[0]) ||
         (L'a' <= MountPoint[0] && L'z' >= MountPoint[0]));
}

/* REQ-MOUNT-005: a drive letter that is already in use cannot host the volume.
 * Checked before the server is contacted, so a rejected mount leaves no
 * partially mounted volume behind.
 */
static BOOL IsDriveLetterInUse(PCWSTR MountPoint)
{
    WCHAR DriveName[3];
    WCHAR Target[MAX_PATH];
    DWORD Drives;

    DriveName[0] = MountPoint[0];
    if (L'a' <= DriveName[0] && L'z' >= DriveName[0])
        DriveName[0] -= (L'a' - L'A');
    DriveName[1] = L':';
    DriveName[2] = L'\0';

    /* physical, removable and mapped network drives are reported here */
    Drives = GetLogicalDrives();
    if (0 != Drives && 0 != (Drives & (1UL << (DriveName[0] - L'A'))))
        return TRUE;

    /* subst drives and other per-session DOS device mappings are not part of
     * the drive mask, so ask the DOS device namespace as well; WinFsp creates
     * the mount point with DefineDosDevice, which fails on an existing mapping
     */
    if (0 != QueryDosDeviceW(DriveName, Target, ARRAYSIZE(Target)))
        return TRUE;

    return ERROR_FILE_NOT_FOUND != GetLastError();
}

static NTSTATUS SvcStart(FSP_SERVICE* Service, ULONG argc, PWSTR* argv)
{
#define argtos(v)                       if (arge > ++argp) v = *argp; else goto usage
#define argtol(v)                       if (arge > ++argp) v = wcstol_deflt(*argp, v); else goto usage

    wchar_t** argp, ** arge;
    PWSTR DebugLogFile = 0;
    ULONG DebugFlags = 0;
    PWSTR ServerAddress = 0;
    PWSTR Namespace = 0;
    PWSTR UserName = 0;
    PWSTR Password = 0;
    PWSTR MountPoint = 0;
    HANDLE DebugLogHandle = INVALID_HANDLE_VALUE;
    WCHAR NamespaceSegment[MAX_PATH];
    WCHAR BaseUrlBuf[MAX_PATH];
    ATELIER_CLIENT Client;
    PTFS* Ptfs = 0;
    NTSTATUS Result;

    memset(&Client, 0, sizeof Client);

    for (argp = argv + 1, arge = argv + argc; arge > argp; argp++)
    {
        if (L'-' != argp[0][0])
            break;
        /* REQ-MOUNT-009: the namespace is passed as -ns only */
        if (0 == wcscmp(argp[0], L"-ns"))
        {
            argtos(Namespace);
            continue;
        }
        /* REQ-MOUNT-010: credentials are taken from -user and -pass only */
        if (0 == wcscmp(argp[0], L"-user"))
        {
            argtos(UserName);
            continue;
        }
        if (0 == wcscmp(argp[0], L"-pass"))
        {
            argtos(Password);
            continue;
        }
        switch (argp[0][1])
        {
        case L'?':
            goto usage;
        case L'd':
            argtol(DebugFlags);
            break;
        case L'D':
            argtos(DebugLogFile);
            break;
        case L'm':
            argtos(MountPoint);
            break;
        case L'p':
            argtos(ServerAddress);
            break;
        default:
            goto usage;
        }
    }

    if (arge > argp)
        goto usage;

    if (0 == ServerAddress || 0 == Namespace || L'\0' == Namespace[0] ||
        0 == MountPoint || 0 == UserName || 0 == Password)
        goto usage;

    /* REQ-MOUNT-003: only a drive letter (X:) can host the volume; a directory
     * path is rejected before the server is contacted
     */
    if (!IsDriveLetterMountPoint(MountPoint))
    {
        fail(L"invalid mount point %s: expected a drive letter such as X:; "
            L"mounting on a directory is not supported", MountPoint);
        goto usage;
    }

    /* REQ-MOUNT-005: an occupied drive letter is rejected up front, so the
     * mount does not fail halfway and leave a partially mounted volume behind
     */
    if (IsDriveLetterInUse(MountPoint))
    {
        fail(L"mount point %s is not usable: the drive letter is already in use", MountPoint);
        goto usage;
    }

    /* REQ-MOUNT-009: -p carries host:port and -ns the namespace; the base URL
     * is composed here as http://host:port/api/atelier/v1/<namespace>
     */
    if (!ComposeNamespaceSegment(Namespace, NamespaceSegment, ARRAYSIZE(NamespaceSegment)) ||
        FAILED(StringCbPrintfW(BaseUrlBuf, sizeof BaseUrlBuf, L"http://%s" ATELIER_API_PATH L"/%s",
            ServerAddress, NamespaceSegment)))
        goto usage;

    EnableBackupRestorePrivileges();

    if (0 != DebugLogFile)
    {
        if (0 == wcscmp(L"-", DebugLogFile))
            DebugLogHandle = GetStdHandle(STD_ERROR_HANDLE);
        else
            DebugLogHandle = CreateFileW(
                DebugLogFile,
                FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                0,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                0);
        if (INVALID_HANDLE_VALUE == DebugLogHandle)
        {
            fail(L"cannot open debug log file");
            goto usage;
        }

        FspDebugLogSetHandle(DebugLogHandle);
    }

    /* the client of the mount is created once and is used for every request
     * of the volume (REQ-CONN-021)
     */
    Result = AtelierClientOpen(&Client, BaseUrlBuf);
    if (!NT_SUCCESS(Result))
    {
        fail(L"mount of %s aborted: Atelier server is not usable", MountPoint);
        goto exit;
    }

    /* probe the server before exposing the volume: an unreachable server or
     * rejected credentials must abort the mount (REQ-MOUNT-011, REQ-MOUNT-012)
     */
    Result = AtelierProbe(&Client, UserName, Password);
    if (!NT_SUCCESS(Result))
    {
        fail(L"mount of %s aborted: Atelier server is not usable", MountPoint);
        goto exit;
    }

    Result = PtfsCreate(Namespace, &Client, BaseUrlBuf, UserName, Password, MountPoint, DebugFlags, &Ptfs);
    if (!NT_SUCCESS(Result))
    {
        fail(L"cannot create file system");
        goto exit;
    }

    Result = FspFileSystemStartDispatcher(Ptfs->FileSystem, 0);
    if (!NT_SUCCESS(Result))
    {
        fail(L"cannot start file system");
        goto exit;
    }

    MountPoint = FspFileSystemMountPoint(Ptfs->FileSystem);

    info(L"%s -p %s -ns %s -m %s",
        L"" PROGNAME,
        BaseUrlBuf,
        Namespace,
        MountPoint);

    Service->UserContext = Ptfs;
    Result = STATUS_SUCCESS;

exit:
    if (!NT_SUCCESS(Result) && 0 != Ptfs)
        PtfsDelete(Ptfs);

    /* the client is closed here unless the volume took it over */
    if (0 != Client.Session)
        AtelierClientClose(&Client);

    return Result;

usage:
    {
        static wchar_t usage[] = L""
            "usage: %s OPTIONS\n"
            "\n"
            "options:\n"
            "    -d DebugFlags       [-1: enable all debug logs]\n"
            "    -D DebugLogFile     [file path; use - for stderr]\n"
            "    -p Host:Port        [Atelier server address; base URL is http://Host:Port"
                ATELIER_API_PATH "]\n"
            "    -ns Namespace       [Atelier namespace; becomes the volume root]\n"
            "    -user Login         [Atelier login]\n"
            "    -pass Password      [Atelier password; kept in memory only]\n"
            "    -m DriveLetter      [X: only; mounting on a directory is not supported]\n";

        fail(usage, L"" PROGNAME);
    }

    return STATUS_UNSUCCESSFUL;

#undef argtos
#undef argtol
}

static NTSTATUS SvcStop(FSP_SERVICE* Service)
{
    PTFS* Ptfs = Service->UserContext;

    FspFileSystemStopDispatcher(Ptfs->FileSystem);
    PtfsDelete(Ptfs);

    return STATUS_SUCCESS;
}

int wmain(int argc, wchar_t** argv)
{
    if (!NT_SUCCESS(FspLoad(0)))
        return ERROR_DELAY_LOAD_FAILED;

    return FspServiceRun(L"" PROGNAME, SvcStart, SvcStop, 0);
}
