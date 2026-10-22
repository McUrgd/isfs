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

/* issues one request against the Atelier server and reads the answer; every
 * request carries the credentials of the mount (REQ-CONN-006, REQ-CONN-009).
 * The answer is allocated with malloc() and must be freed with free().
 */
NTSTATUS AtelierRequest(ATELIER_CLIENT* Client, PCWSTR UserName, PCWSTR Password,
    PCWSTR Method, PCWSTR Path, PBYTE* PResponse, DWORD* PResponseLength, DWORD* PStatusCode);

/* mount-time probe: GET /:namespace against the -p address with the -ns
 * namespace. Fails the mount when the server is unreachable (REQ-MOUNT-021),
 * rejects the credentials (REQ-MOUNT-012) or does not expose the namespace
 * (REQ-MOUNT-009). The password is only used in the Basic header and
 * never logged (REQ-MOUNT-010).
 */
NTSTATUS AtelierProbe(ATELIER_CLIENT* Client, PCWSTR UserName, PCWSTR Password);

#endif /* ISFS_ATELIER_H */
