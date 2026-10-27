/**
 * @file atelier.c
 *
 * @copyright 2026 Pavlov Sergey
 */
/*
 * This file is part of ISFS.
 *
 * ISFS is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License version 3 as published by the Free
 * Software Foundation, with the WinFsp FLOSS Exception (see NOTICE).
 *
 * ISFS is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along
 * with ISFS.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <windows.h>
#include <strsafe.h>
#include <winhttp.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <winfsp/winfsp.h>

#include "atelier.h"

/* connect/send/receive timeout for the mount probe (REQ-MOUNT-021) */
#define ATELIER_TIMEOUT_MS              30000
/* the answer of the server is read into memory; the limit protects the
 * process against an endless answer
 */
#define ATELIER_MAX_RESPONSE_SIZE       (64 * 1024 * 1024)
/* the user agent of the HTTP client; it carries the name of the program */
#define ATELIER_USER_AGENT              L"isfs/1.0"

#define info(format, ...)               FspServiceLog(EVENTLOG_INFORMATION_TYPE, format, __VA_ARGS__)
#define fail(format, ...)               FspServiceLog(EVENTLOG_ERROR_TYPE, format, __VA_ARGS__)

/* ------------------------------------------------------------------------
 * The credentials
 *
 * base64 is needed for the HTTP Basic header; Windows has no plain base64 API
 * that does not pull in crypt32, so encode in place
 * ------------------------------------------------------------------------ */

static BOOL Base64EncodeWide(const BYTE* Data, DWORD DataSize, PWSTR Buffer, DWORD BufferCch)
{
    static const WCHAR Alphabet[] =
        L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    DWORD Index, Out = 0, Remaining;

    if ((DataSize + 2) / 3 * 4 + 1 > BufferCch)
        return FALSE;

    for (Index = 0; Index + 3 <= DataSize; Index += 3)
    {
        Buffer[Out++] = Alphabet[Data[Index] >> 2];
        Buffer[Out++] = Alphabet[((Data[Index] & 0x03) << 4) | (Data[Index + 1] >> 4)];
        Buffer[Out++] = Alphabet[((Data[Index + 1] & 0x0f) << 2) | (Data[Index + 2] >> 6)];
        Buffer[Out++] = Alphabet[Data[Index + 2] & 0x3f];
    }

    Remaining = DataSize - Index;
    if (1 == Remaining)
    {
        Buffer[Out++] = Alphabet[Data[Index] >> 2];
        Buffer[Out++] = Alphabet[(Data[Index] & 0x03) << 4];
        Buffer[Out++] = L'=';
        Buffer[Out++] = L'=';
    }
    else if (2 == Remaining)
    {
        Buffer[Out++] = Alphabet[Data[Index] >> 2];
        Buffer[Out++] = Alphabet[((Data[Index] & 0x03) << 4) | (Data[Index + 1] >> 4)];
        Buffer[Out++] = Alphabet[(Data[Index + 1] & 0x0f) << 2];
        Buffer[Out++] = L'=';
    }

    Buffer[Out] = L'\0';

    return TRUE;
}

/* builds "Authorization: Basic ..." from the -user/-pass credentials; the caller
 * must wipe and free the returned string (REQ-MOUNT-010)
 */
static PWSTR BasicAuthHeader(PCWSTR UserName, PCWSTR Password)
{
    SIZE_T CredentialsCch = wcslen(UserName) + 1 + wcslen(Password);
    PWSTR Credentials = 0;
    PWSTR Header = 0;
    PWSTR Encoded = 0;
    BYTE* Utf8 = 0;
    int Utf8Size = 0;
    SIZE_T EncodedCch = 0, HeaderCch;

    Credentials = malloc((CredentialsCch + 1) * sizeof(WCHAR));
    if (0 == Credentials)
        goto exit;
    if (FAILED(StringCchPrintfW(Credentials, CredentialsCch + 1, L"%s:%s", UserName, Password)))
        goto exit;

    Utf8Size = WideCharToMultiByte(CP_UTF8, 0, Credentials, (int)CredentialsCch, 0, 0, 0, 0);
    Utf8 = malloc((SIZE_T)Utf8Size);
    if (0 == Utf8 || 0 == Utf8Size)
        goto exit;
    WideCharToMultiByte(CP_UTF8, 0, Credentials, (int)CredentialsCch,
        (LPSTR)Utf8, Utf8Size, 0, 0);

    EncodedCch = ((SIZE_T)Utf8Size + 2) / 3 * 4 + 1;
    Encoded = malloc(EncodedCch * sizeof(WCHAR));
    if (0 == Encoded || !Base64EncodeWide(Utf8, (DWORD)Utf8Size, Encoded, (DWORD)EncodedCch))
        goto exit;

    /* "Authorization: Basic " */
    HeaderCch = 21 + EncodedCch;
    Header = malloc(HeaderCch * sizeof(WCHAR));
    if (0 == Header)
        goto exit;
    if (FAILED(StringCchPrintfW(Header, HeaderCch, L"Authorization: Basic %s", Encoded)))
    {
        free(Header);
        Header = 0;
    }

exit:
    if (0 != Encoded)
    {
        /* EncodedCch includes the terminator and is set before the allocation */
        SecureZeroMemory(Encoded, EncodedCch * sizeof(WCHAR));
        free(Encoded);
    }
    if (0 != Utf8)
    {
        SecureZeroMemory(Utf8, (SIZE_T)Utf8Size);
        free(Utf8);
    }
    if (0 != Credentials)
    {
        SecureZeroMemory(Credentials, CredentialsCch * sizeof(WCHAR));
        free(Credentials);
    }

    return Header;
}

/* ------------------------------------------------------------------------
 * The HTTP client
 * ------------------------------------------------------------------------ */

static NTSTATUS AtelierTransportError(PCWSTR Endpoint, DWORD Error)
{
    PCWSTR Reason;
    NTSTATUS Result;

    switch (Error)
    {
    case ERROR_WINHTTP_TIMEOUT:
        Reason = L"timed out";
        Result = STATUS_IO_TIMEOUT;
        break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        Reason = L"host name could not be resolved";
        Result = STATUS_NETWORK_UNREACHABLE;
        break;
    case ERROR_WINHTTP_CANNOT_CONNECT:
        Reason = L"connection refused or host unreachable";
        Result = STATUS_NETWORK_UNREACHABLE;
        break;
    case ERROR_WINHTTP_SECURE_FAILURE:
        Reason = L"TLS negotiation failed";
        Result = STATUS_INVALID_NETWORK_RESPONSE;
        break;
    default:
        Reason = L"HTTP transport error";
        Result = STATUS_NETWORK_UNREACHABLE;
        break;
    }

    fail(L"cannot connect to %s: %s (WinHTTP error %lu)", Endpoint, Reason, Error);

    return Result;
}

/* opens the HTTP client of the mount: one session, the host and the path of
 * the base URL (REQ-CONN-021, REQ-CONN-003)
 */
NTSTATUS AtelierClientOpen(ATELIER_CLIENT* Client, PCWSTR BaseUrl)
{
    URL_COMPONENTS UrlComponents;
    WCHAR HostName[256];
    WCHAR BasePath[1024];

    memset(Client, 0, sizeof *Client);

    memset(&UrlComponents, 0, sizeof UrlComponents);
    UrlComponents.dwStructSize = sizeof UrlComponents;
    UrlComponents.lpszHostName = HostName;
    UrlComponents.dwHostNameLength = ARRAYSIZE(HostName);
    UrlComponents.lpszUrlPath = BasePath;
    UrlComponents.dwUrlPathLength = ARRAYSIZE(BasePath);

    if (!WinHttpCrackUrl(BaseUrl, 0, 0, &UrlComponents))
    {
        fail(L"cannot connect to %s: invalid server address (WinHTTP error %lu)",
            BaseUrl, GetLastError());
        return STATUS_INVALID_PARAMETER;
    }

    /* connect to host:port directly, without a proxy (REQ-CONN-019) */
    Client->Session = WinHttpOpen(ATELIER_USER_AGENT, WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (0 == Client->Session)
    {
        fail(L"cannot connect to %s: cannot initialize HTTP client (Win32 error %lu)",
            BaseUrl, GetLastError());
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* failure to set a timeout is not fatal, the defaults apply */
    WinHttpSetTimeouts(Client->Session, ATELIER_TIMEOUT_MS, ATELIER_TIMEOUT_MS,
        ATELIER_TIMEOUT_MS, ATELIER_TIMEOUT_MS);

    wcscpy_s(Client->HostName, ARRAYSIZE(Client->HostName), HostName);
    wcscpy_s(Client->BasePath, ARRAYSIZE(Client->BasePath), BasePath);
    Client->Port = UrlComponents.nPort;
    Client->Flags = INTERNET_SCHEME_HTTPS == UrlComponents.nScheme ? WINHTTP_FLAG_SECURE : 0;

    return STATUS_SUCCESS;
}

VOID AtelierClientClose(ATELIER_CLIENT* Client)
{
    if (0 != Client->Session)
        WinHttpCloseHandle(Client->Session);

    Client->Session = 0;
}

/* the HTTP status of the server becomes an NTSTATUS (overview §7) */
NTSTATUS AtelierStatusToNtStatus(DWORD StatusCode)
{
    switch (StatusCode)
    {
    case 200:
    case 201:
    case 202:
    case 304:
        return STATUS_SUCCESS;
    case 400:
        return STATUS_OBJECT_NAME_INVALID;
    case 401:
        return STATUS_ACCESS_DENIED;
    case 403:
        return STATUS_ACCESS_DENIED;
    case 404:
        return STATUS_OBJECT_NAME_NOT_FOUND;
    case 409:
        return STATUS_FILE_LOCK_CONFLICT;
    case 415:
        return STATUS_INVALID_PARAMETER;
    case 423:
    case 425:
        return STATUS_FILE_LOCK_CONFLICT;
    case 429:
        return STATUS_NETWORK_BUSY;
    case 500:
        return STATUS_UNEXPECTED_IO_ERROR;
    case 502:
    case 503:
        return STATUS_NETWORK_UNREACHABLE;
    case 504:
        return STATUS_IO_TIMEOUT;
    default:
        return STATUS_UNEXPECTED_IO_ERROR;
    }
}

/* issues one request against the Atelier server and reads the answer; every
 * request carries the credentials of the mount (REQ-CONN-006, REQ-CONN-009)
 */
NTSTATUS AtelierRequestEx(ATELIER_CLIENT* Client, PCWSTR UserName, PCWSTR Password,
    PCWSTR Method, PCWSTR Path, ATELIER_REQUEST_OPTIONS* Options,
    PBYTE* PResponse, DWORD* PResponseLength, DWORD* PStatusCode)
{
    HINTERNET Connection = 0, Request = 0;
    PWSTR AuthHeader = 0;
    LPVOID Body = WINHTTP_NO_REQUEST_DATA;
    DWORD BodyLength = 0;
    WCHAR FullPath[2048];
    PBYTE Response = 0;
    DWORD Total = 0;
    NTSTATUS Result;

    *PResponse = 0;
    *PResponseLength = 0;
    *PStatusCode = 0;

    if (0 != Options && 0 != Options->ETag && 0 != Options->ETagCch)
        Options->ETag[0] = L'\0';

    if (0 != Options && 0 != Options->Body && 0 != Options->BodyLength)
    {
        Body = (LPVOID)Options->Body;
        BodyLength = Options->BodyLength;
    }

    if (FAILED(StringCchPrintfW(FullPath, ARRAYSIZE(FullPath), L"%s%s", Client->BasePath, Path)))
        return STATUS_OBJECT_NAME_INVALID;

    Connection = WinHttpConnect(Client->Session, Client->HostName, Client->Port, 0);
    if (0 == Connection)
    {
        Result = AtelierTransportError(FullPath, GetLastError());
        goto exit;
    }

    Request = WinHttpOpenRequest(Connection, Method, FullPath, 0,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, Client->Flags);
    if (0 == Request)
    {
        Result = AtelierTransportError(FullPath, GetLastError());
        goto exit;
    }

    AuthHeader = BasicAuthHeader(UserName, Password);
    if (0 == AuthHeader)
    {
        fail(L"cannot connect to %s: cannot build credentials", FullPath);
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    if (!WinHttpAddRequestHeaders(Request, AuthHeader, (DWORD)-1,
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE) ||
        (0 != Options && 0 != Options->Headers && L'\0' != Options->Headers[0] &&
            !WinHttpAddRequestHeaders(Request, Options->Headers, (DWORD)-1,
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) ||
        !WinHttpSendRequest(Request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            Body, BodyLength, BodyLength, 0) ||
        !WinHttpReceiveResponse(Request, 0))
    {
        Result = AtelierTransportError(FullPath, GetLastError());
        goto exit;
    }

    {
        DWORD Size = sizeof *PStatusCode;

        if (!WinHttpQueryHeaders(Request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, PStatusCode, &Size, WINHTTP_NO_HEADER_INDEX))
        {
            Result = AtelierTransportError(FullPath, GetLastError());
            goto exit;
        }
    }

    /* the document hash is the ETag of the answer (api.md §6); a server that
     * sends no hash is not an error, the caller keeps its own state then
     */
    if (0 != Options && 0 != Options->ETag && 0 != Options->ETagCch)
    {
        DWORD Size = (DWORD)(Options->ETagCch * sizeof(WCHAR));

        if (!WinHttpQueryHeaders(Request, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX,
            Options->ETag, &Size, WINHTTP_NO_HEADER_INDEX))
            Options->ETag[0] = L'\0';
        else
            Options->ETag[Options->ETagCch - 1] = L'\0';
    }

    for (;;)
    {
        DWORD Available = 0, Read = 0;

        if (!WinHttpQueryDataAvailable(Request, &Available))
        {
            Result = AtelierTransportError(FullPath, GetLastError());
            goto exit;
        }
        if (0 == Available)
            break;

        if (Total + Available > ATELIER_MAX_RESPONSE_SIZE)
        {
            fail(L"cannot read the answer of %s: it is too large", FullPath);
            Result = STATUS_INVALID_BUFFER_SIZE;
            goto exit;
        }

        {
            PBYTE Buffer = realloc(Response, Total + Available + 1);

            if (0 == Buffer)
            {
                Result = STATUS_INSUFFICIENT_RESOURCES;
                goto exit;
            }
            Response = Buffer;
        }

        if (!WinHttpReadData(Request, Response + Total, Available, &Read))
        {
            Result = AtelierTransportError(FullPath, GetLastError());
            goto exit;
        }
        Total += Read;
    }

    if (0 != Response)
        Response[Total] = 0;

    *PResponse = Response;
    *PResponseLength = Total;
    Response = 0;
    Result = STATUS_SUCCESS;

exit:
    if (0 != AuthHeader)
    {
        SecureZeroMemory(AuthHeader, wcslen(AuthHeader) * sizeof(WCHAR));
        free(AuthHeader);
    }
    if (0 != Request)
        WinHttpCloseHandle(Request);
    if (0 != Connection)
        WinHttpCloseHandle(Connection);
    free(Response);

    return Result;
}

/* a request without additional headers and without the hash of the answer */
NTSTATUS AtelierRequest(ATELIER_CLIENT* Client, PCWSTR UserName, PCWSTR Password,
    PCWSTR Method, PCWSTR Path, PBYTE* PResponse, DWORD* PResponseLength, DWORD* PStatusCode)
{
    return AtelierRequestEx(Client, UserName, Password, Method, Path, 0,
        PResponse, PResponseLength, PStatusCode);
}

/* mount-time probe: GET /:namespace against the -p address with the -ns
 * namespace. Fails the mount when the server is unreachable (REQ-MOUNT-021),
 * rejects the credentials (REQ-MOUNT-012) or does not expose the namespace
 * (REQ-MOUNT-009). The password is only used in the Basic header and
 * never logged (REQ-MOUNT-010).
 */
NTSTATUS AtelierProbe(ATELIER_CLIENT* Client, PCWSTR UserName, PCWSTR Password)
{
    PBYTE Response = 0;
    DWORD ResponseLength = 0, StatusCode = 0;
    NTSTATUS Result;

    Result = AtelierRequest(Client, UserName, Password, L"GET", L"", &Response, &ResponseLength, &StatusCode);
    if (!NT_SUCCESS(Result))
    {
        /* the transport error was already reported */
        return Result;
    }

    free(Response);

    if (StatusCode >= 200 && StatusCode < 300)
    {
        info(L"connected to Atelier server at %s (HTTP %lu)", Client->BasePath, StatusCode);
        Result = STATUS_SUCCESS;
    }
    else if (401 == StatusCode)
    {
        fail(L"cannot connect to %s: authentication failed (HTTP 401); "
            L"check -user and -pass", Client->BasePath);
        Result = STATUS_LOGON_FAILURE;
    }
    else if (403 == StatusCode)
    {
        fail(L"cannot connect to %s: access denied (HTTP 403); "
            L"the account may not use the Atelier API", Client->BasePath);
        Result = STATUS_ACCESS_DENIED;
    }
    else if (404 == StatusCode)
    {
        fail(L"cannot connect to %s: namespace not found (HTTP 404); "
            L"check -ns and that the account may access the namespace", Client->BasePath);
        Result = STATUS_OBJECT_NAME_NOT_FOUND;
    }
    else if (StatusCode >= 500)
    {
        fail(L"cannot connect to %s: server error (HTTP %lu)", Client->BasePath, StatusCode);
        Result = STATUS_UNEXPECTED_IO_ERROR;
    }
    else
    {
        fail(L"cannot connect to %s: unexpected response (HTTP %lu)", Client->BasePath, StatusCode);
        Result = STATUS_UNSUCCESSFUL;
    }

    return Result;
}
