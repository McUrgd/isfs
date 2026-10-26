/**
 * @file atelier.h
 *
 * @copyright 2026 Pavlov Sergey
 */
/*
 * This file is part of ISFS.
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

#ifndef ISFS_ATELIER_H
#define ISFS_ATELIER_H

#include <windows.h>
#include <winhttp.h>
#include <winfsp/winfsp.h>

/* the Atelier client of the mount: one HTTP session, the host and the path
 * of the base URL (REQ-CONN-021, REQ-CONN-003)
 */
typedef struct
{
    HINTERNET Session;
    WCHAR HostName[256];
    WCHAR BasePath[1024]; /* the path of the base URL, e.g. "/api/atelier/v1/USER" */
    INTERNET_PORT Port;
    DWORD Flags; /* WINHTTP_FLAG_SECURE; always 0, the scheme is http (REQ-CONN-001) */
} ATELIER_CLIENT;

/* opens the HTTP client of the mount: one session, the host and the path of
 * the base URL (REQ-CONN-021, REQ-CONN-003)
 */
NTSTATUS AtelierClientOpen(ATELIER_CLIENT* Client, PCWSTR BaseUrl);

/* closes the HTTP client of the mount and releases its session */
VOID AtelierClientClose(ATELIER_CLIENT* Client);

/* the HTTP status of the server becomes an NTSTATUS (overview §7) */
NTSTATUS AtelierStatusToNtStatus(DWORD StatusCode);

/* additional headers of one request, its body and the hash of its answer
 * (api.md §6): Headers is a CRLF separated block, for example
 * L"If-None-Match: <hash>\r\n"; Body is sent as the request body, for example
 * the JSON document of a PUT (api.md §8.3.3); ETag receives the ETag of the
 * answer, which is the document hash.
 */
typedef struct
{
    PCWSTR Headers; /* additional request headers; 0 for none */
    const VOID* Body; /* the request body; 0 when the request has none */
    DWORD BodyLength; /* the size of Body in bytes; 0 when there is no body */
    PWSTR ETag;     /* receives the ETag of the answer; 0 when not needed */
    SIZE_T ETagCch; /* capacity of ETag in characters, including the terminator */
} ATELIER_REQUEST_OPTIONS;

/* issues one request against the Atelier server and reads the answer; every
 * request carries the credentials of the mount (REQ-CONN-006, REQ-CONN-009).
 * The answer is allocated with malloc() and must be freed with free().
 */
NTSTATUS AtelierRequest(ATELIER_CLIENT* Client, PCWSTR UserName, PCWSTR Password,
    PCWSTR Method, PCWSTR Path, PBYTE* PResponse, DWORD* PResponseLength, DWORD* PStatusCode);

/* like AtelierRequest, but sends the headers and the body of Options and
 * returns the ETag of the answer in Options->ETag (REQ-FS-013, REQ-FS-025,
 * REQ-FS-021). An answer without ETag is not an error: the string is left
 * empty. HEAD and 304 answers have no body; the caller decides what a 304
 * means.
 */
NTSTATUS AtelierRequestEx(ATELIER_CLIENT* Client, PCWSTR UserName, PCWSTR Password,
    PCWSTR Method, PCWSTR Path, ATELIER_REQUEST_OPTIONS* Options,
    PBYTE* PResponse, DWORD* PResponseLength, DWORD* PStatusCode);

/* mount-time probe: GET /:namespace against the -p address with the -ns
 * namespace. Fails the mount when the server is unreachable (REQ-MOUNT-021),
 * rejects the credentials (REQ-MOUNT-012) or does not expose the namespace
 * (REQ-MOUNT-009). The password is only used in the Basic header and
 * never logged (REQ-MOUNT-010).
 */
NTSTATUS AtelierProbe(ATELIER_CLIENT* Client, PCWSTR UserName, PCWSTR Password);

#endif /* ISFS_ATELIER_H */
