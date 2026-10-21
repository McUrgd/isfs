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
#include <winhttp.h>
#include <wctype.h>
#include <sddl.h>

#define PROGNAME                        "isfs"
#define VOLUME_NAME                     "ISFS"
#define ATELIER_API_PATH                "/api/atelier/v1"
/* connect/send/receive timeout for the mount probe (REQ-MOUNT-021) */
#define ATELIER_TIMEOUT_MS              30000
#define ALLOCATION_UNIT                 4096
#define FULLPATH_SIZE                   (MAX_PATH + FSP_FSCTL_TRANSACT_PATH_SIZEMAX / sizeof(WCHAR))

#define info(format, ...)               FspServiceLog(EVENTLOG_INFORMATION_TYPE, format, __VA_ARGS__)
#define warn(format, ...)               FspServiceLog(EVENTLOG_WARNING_TYPE, format, __VA_ARGS__)
#define fail(format, ...)               FspServiceLog(EVENTLOG_ERROR_TYPE, format, __VA_ARGS__)

#define ConcatPath(Ptfs, FN, FP)        (0 == StringCbPrintfW(FP, sizeof FP, L"%s%s", Ptfs->BaseUrl, FN))
#define HandleFromContext(FC)           (((PTFS_FILE_CONTEXT *)(FC))->Handle)

typedef struct
{
    FSP_FILE_SYSTEM* FileSystem;
    PWSTR BaseUrl; /* http://host:port/api/atelier/v1/<namespace> (REQ-MOUNT-009) */
    PWSTR UserName; /* login from -user, memory only (REQ-MOUNT-010) */
    PWSTR Password; /* password from -pass, memory only (REQ-MOUNT-010) */
} PTFS;

typedef struct
{
    HANDLE Handle;
    PVOID DirBuffer;
    BOOLEAN IsRoot; /* "\" is the namespace root, a synthetic directory (REQ-MOUNT-018) */
} PTFS_FILE_CONTEXT;

/* the volume root is the -ns namespace: WinFsp passes "\" for it, possibly
 * with leading separators (REQ-MOUNT-018)
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
 * generated instead of read from a handle (REQ-MOUNT-018)
 */
static NTSTATUS GetSyntheticDirInfo(FSP_FSCTL_FILE_INFO* FileInfo)
{
    FILETIME Time;

    GetSystemTimeAsFileTime(&Time);

    memset(FileInfo, 0, sizeof *FileInfo);
    FileInfo->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    FileInfo->FileSize = 0;
    FileInfo->AllocationSize = 0;
    FileInfo->CreationTime = ((PLARGE_INTEGER)&Time)->QuadPart;
    FileInfo->LastAccessTime = FileInfo->CreationTime;
    FileInfo->LastWriteTime = FileInfo->CreationTime;
    FileInfo->ChangeTime = FileInfo->CreationTime;
    FileInfo->IndexNumber = 0;
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

/* base64 is needed for the HTTP Basic header; Windows has no plain base64 API
 * that does not pull in crypt32, so encode in place
 */
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
    SIZE_T EncodedCch, HeaderCch;

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

/* mount-time probe: GET /:namespace against the -p address with the -ns
 * namespace. Fails the mount when the server is unreachable (REQ-MOUNT-021),
 * rejects the credentials (REQ-MOUNT-012) or does not expose the namespace
 * (REQ-MOUNT-009). The password is only used in the Basic header and
 * never logged (REQ-MOUNT-010).
 */
static NTSTATUS AtelierProbe(PCWSTR BaseUrl, PCWSTR UserName, PCWSTR Password)
{
    WCHAR HostName[256];
    WCHAR BasePath[1024];
    URL_COMPONENTS UrlComponents;
    HINTERNET Session = 0, Connection = 0, Request = 0;
    PWSTR AuthHeader = 0;
    DWORD StatusCode = 0, Size;
    NTSTATUS Result;

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

    /* connect to host:port directly, without a proxy */
    Session = WinHttpOpen(L"" PROGNAME L"/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (0 == Session)
    {
        fail(L"cannot connect to %s: cannot initialize HTTP client (Win32 error %lu)",
            BaseUrl, GetLastError());
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    /* failure to set a timeout is not fatal, the defaults apply */
    WinHttpSetTimeouts(Session, ATELIER_TIMEOUT_MS, ATELIER_TIMEOUT_MS,
        ATELIER_TIMEOUT_MS, ATELIER_TIMEOUT_MS);

    Connection = WinHttpConnect(Session, HostName, UrlComponents.nPort, 0);
    if (0 == Connection)
    {
        Result = AtelierTransportError(BaseUrl, GetLastError());
        goto exit;
    }

    Request = WinHttpOpenRequest(Connection, L"GET", BasePath, 0,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        INTERNET_SCHEME_HTTPS == UrlComponents.nScheme ? WINHTTP_FLAG_SECURE : 0);
    if (0 == Request)
    {
        Result = AtelierTransportError(BaseUrl, GetLastError());
        goto exit;
    }

    AuthHeader = BasicAuthHeader(UserName, Password);
    if (0 == AuthHeader)
    {
        fail(L"cannot connect to %s: cannot build credentials", BaseUrl);
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    if (!WinHttpAddRequestHeaders(Request, AuthHeader, (DWORD)-1,
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE) ||
        !WinHttpSendRequest(Request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(Request, 0))
    {
        Result = AtelierTransportError(BaseUrl, GetLastError());
        goto exit;
    }

    Size = sizeof StatusCode;
    if (!WinHttpQueryHeaders(Request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &StatusCode, &Size, WINHTTP_NO_HEADER_INDEX))
    {
        Result = AtelierTransportError(BaseUrl, GetLastError());
        goto exit;
    }

    if (StatusCode >= 200 && StatusCode < 300)
    {
        info(L"connected to Atelier server at %s (HTTP %lu)", BaseUrl, StatusCode);
        Result = STATUS_SUCCESS;
    }
    else if (401 == StatusCode)
    {
        fail(L"cannot connect to %s: authentication failed (HTTP 401); "
            L"check -user and -pass", BaseUrl);
        Result = STATUS_LOGON_FAILURE;
    }
    else if (403 == StatusCode)
    {
        fail(L"cannot connect to %s: access denied (HTTP 403); "
            L"the account may not use the Atelier API", BaseUrl);
        Result = STATUS_ACCESS_DENIED;
    }
    else if (404 == StatusCode)
    {
        fail(L"cannot connect to %s: namespace not found (HTTP 404); "
            L"check -ns and that the account may access the namespace", BaseUrl);
        Result = STATUS_OBJECT_NAME_NOT_FOUND;
    }
    else if (StatusCode >= 500)
    {
        fail(L"cannot connect to %s: server error (HTTP %lu)", BaseUrl, StatusCode);
        Result = STATUS_UNEXPECTED_IO_ERROR;
    }
    else
    {
        fail(L"cannot connect to %s: unexpected response (HTTP %lu)", BaseUrl, StatusCode);
        Result = STATUS_UNSUCCESSFUL;
    }

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
    if (0 != Session)
        WinHttpCloseHandle(Session);

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
 * read from or written to the server (overview §6). D:(A;;GA;;;WD) grants
 * full access to everyone, which is the "full access" policy for the volume.
 */
static PSECURITY_DESCRIPTOR BuildFullAccessSecurityDescriptor(VOID)
{
    PSECURITY_DESCRIPTOR SecurityDescriptor = 0;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &SecurityDescriptor, 0))
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
    static PCWSTR Categories[] = { L"CLS", L"CSP", L"RTN", L"OTH" };
    PCWSTR Name, End;
    ULONG Index;
    NTSTATUS Result;

    /* attributes are synthesized as well: nothing is read from the local
     * file system (REQ-MOUNT-018)
     */
    if (0 != PFileAttributes)
    {
        *PFileAttributes = IsRootFileName(FileName) ?
            FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

        for (Name = FileName; L'\\' == *Name || L'/' == *Name; Name++)
            ;
        for (End = Name; L'\0' != *End && L'\\' != *End && L'/' != *End; End++)
            ;

        for (Index = 0; Index < ARRAYSIZE(Categories); Index++)
        {
            if ((SIZE_T)(End - Name) == wcslen(Categories[Index]) &&
                0 == _wcsnicmp(Name, Categories[Index], (SIZE_T)(End - Name)))
            {
                *PFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
                break;
            }
        }
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
    PTFS* Ptfs = (PTFS*)FileSystem->UserContext;
    WCHAR FullPath[FULLPATH_SIZE];
    SECURITY_ATTRIBUTES SecurityAttributes;
    ULONG CreateFlags;
    PTFS_FILE_CONTEXT* FileContext;

    /* the namespace root already exists and cannot be created (REQ-MOUNT-018) */
    if (IsRootFileName(FileName))
        return STATUS_OBJECT_NAME_COLLISION;

    if (!ConcatPath(Ptfs, FileName, FullPath))
        return STATUS_OBJECT_NAME_INVALID;

    FileContext = malloc(sizeof * FileContext);
    if (0 == FileContext)
        return STATUS_INSUFFICIENT_RESOURCES;
    memset(FileContext, 0, sizeof * FileContext);

    SecurityAttributes.nLength = sizeof SecurityAttributes;
    SecurityAttributes.lpSecurityDescriptor = SecurityDescriptor;
    SecurityAttributes.bInheritHandle = FALSE;

    CreateFlags = FILE_FLAG_BACKUP_SEMANTICS;
    if (CreateOptions & FILE_DELETE_ON_CLOSE)
        CreateFlags |= FILE_FLAG_DELETE_ON_CLOSE;

    if (CreateOptions & FILE_DIRECTORY_FILE)
    {
        /*
         * It is not widely known but CreateFileW can be used to create directories!
         * It requires the specification of both FILE_FLAG_BACKUP_SEMANTICS and
         * FILE_FLAG_POSIX_SEMANTICS. It also requires that FileAttributes has
         * FILE_ATTRIBUTE_DIRECTORY set.
         */
        CreateFlags |= FILE_FLAG_POSIX_SEMANTICS;
        FileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    }
    else
        FileAttributes &= ~FILE_ATTRIBUTE_DIRECTORY;

    if (0 == FileAttributes)
        FileAttributes = FILE_ATTRIBUTE_NORMAL;

    FileContext->Handle = CreateFileW(FullPath,
        GrantedAccess, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &SecurityAttributes,
        CREATE_NEW, CreateFlags | FileAttributes, 0);
    if (INVALID_HANDLE_VALUE == FileContext->Handle)
    {
        free(FileContext);
        return FspNtStatusFromWin32(GetLastError());
    }

    *PFileContext = FileContext;

    return GetFileInfoInternal(FileContext->Handle, FileInfo);
}

static NTSTATUS Open(FSP_FILE_SYSTEM* FileSystem,
    PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess,
    PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{
    PTFS* Ptfs = (PTFS*)FileSystem->UserContext;
    WCHAR FullPath[FULLPATH_SIZE];
    ULONG CreateFlags;
    PTFS_FILE_CONTEXT* FileContext;

    /* the root is a synthetic directory backed by the -ns namespace, not by
     * a local path (REQ-MOUNT-018)
     */
    if (IsRootFileName(FileName))
    {
        FileContext = malloc(sizeof * FileContext);
        if (0 == FileContext)
            return STATUS_INSUFFICIENT_RESOURCES;
        memset(FileContext, 0, sizeof * FileContext);
        FileContext->IsRoot = TRUE;
        FileContext->Handle = INVALID_HANDLE_VALUE;

        *PFileContext = FileContext;

        return GetSyntheticDirInfo(FileInfo);
    }

    if (!ConcatPath(Ptfs, FileName, FullPath))
        return STATUS_OBJECT_NAME_INVALID;

    FileContext = malloc(sizeof * FileContext);
    if (0 == FileContext)
        return STATUS_INSUFFICIENT_RESOURCES;
    memset(FileContext, 0, sizeof * FileContext);

    CreateFlags = FILE_FLAG_BACKUP_SEMANTICS;
    if (CreateOptions & FILE_DELETE_ON_CLOSE)
        CreateFlags |= FILE_FLAG_DELETE_ON_CLOSE;

    FileContext->Handle = CreateFileW(FullPath,
        GrantedAccess, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
        OPEN_EXISTING, CreateFlags, 0);
    if (INVALID_HANDLE_VALUE == FileContext->Handle)
    {
        free(FileContext);
        return FspNtStatusFromWin32(GetLastError());
    }

    *PFileContext = FileContext;

    return GetFileInfoInternal(FileContext->Handle, FileInfo);
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

    /* synthetic directories have no local handle and cannot be deleted
     * (REQ-MOUNT-018)
     */
    if (Context->IsRoot)
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

    if (!FileContext->IsRoot)
        CloseHandle(Handle);

    FspFileSystemDeleteDirectoryBuffer(&FileContext->DirBuffer);
    free(FileContext);
}

static NTSTATUS Read(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length,
    PULONG PBytesTransferred)
{
    HANDLE Handle = HandleFromContext(FileContext);
    OVERLAPPED Overlapped = { 0 };

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
    PTFS_FILE_CONTEXT* Context = FileContext;

    if (Context->IsRoot)
        return GetSyntheticDirInfo(FileInfo);

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
     * nothing is read from a local handle (REQ-MOUNT-018)
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
    HANDLE Handle = HandleFromContext(FileContext);
    WCHAR FullPath[FULLPATH_SIZE];
    ULONG Length, PatternLength;
    HANDLE FindHandle;
    WIN32_FIND_DATAW FindData;
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
            /* REQ-MOUNT-018: the root is the -ns namespace and lists only the
             * synthetic category directories; documents and the namespace name
             * itself are never root entries, and no local path is touched.
             */
            static PCWSTR Categories[] = { L"CLS", L"CSP", L"RTN", L"OTH" };
            ULONG Index;

            for (Index = 0; Index < ARRAYSIZE(Categories); Index++)
            {
                if (!PtfsMatchPattern(Pattern, Categories[Index]))
                    continue;

                memset(DirInfo, 0, sizeof * DirInfo);
                Length = (ULONG)wcslen(Categories[Index]);
                DirInfo->Size = (UINT16)(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + Length * sizeof(WCHAR));
                GetSyntheticDirInfo(&DirInfo->FileInfo);
                memcpy(DirInfo->FileNameBuf, Categories[Index], Length * sizeof(WCHAR));

                if (!FspFileSystemFillDirectoryBuffer(&FileContext->DirBuffer, DirInfo, &DirBufferResult))
                    break;
            }
        }
        else
        {
            PatternLength = (ULONG)wcslen(Pattern);

            Length = GetFinalPathNameByHandleW(Handle, FullPath, FULLPATH_SIZE - 1, 0);
            if (0 == Length)
                DirBufferResult = FspNtStatusFromWin32(GetLastError());
            else if (Length + 1 + PatternLength >= FULLPATH_SIZE)
                DirBufferResult = STATUS_OBJECT_NAME_INVALID;
            if (!NT_SUCCESS(DirBufferResult))
            {
                FspFileSystemReleaseDirectoryBuffer(&FileContext->DirBuffer);
                return DirBufferResult;
            }

            if (L'\\' != FullPath[Length - 1])
                FullPath[Length++] = L'\\';
            memcpy(FullPath + Length, Pattern, PatternLength * sizeof(WCHAR));
            FullPath[Length + PatternLength] = L'\0';

            FindHandle = FindFirstFileW(FullPath, &FindData);
            if (INVALID_HANDLE_VALUE != FindHandle)
            {
                do
                {
                    memset(DirInfo, 0, sizeof * DirInfo);
                    Length = (ULONG)wcslen(FindData.cFileName);
                    DirInfo->Size = (UINT16)(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + Length * sizeof(WCHAR));
                    DirInfo->FileInfo.FileAttributes = FindData.dwFileAttributes;
                    DirInfo->FileInfo.ReparseTag = 0;
                    DirInfo->FileInfo.FileSize =
                        ((UINT64)FindData.nFileSizeHigh << 32) | (UINT64)FindData.nFileSizeLow;
                    DirInfo->FileInfo.AllocationSize = (DirInfo->FileInfo.FileSize + ALLOCATION_UNIT - 1)
                        / ALLOCATION_UNIT * ALLOCATION_UNIT;
                    DirInfo->FileInfo.CreationTime = ((PLARGE_INTEGER)&FindData.ftCreationTime)->QuadPart;
                    DirInfo->FileInfo.LastAccessTime = ((PLARGE_INTEGER)&FindData.ftLastAccessTime)->QuadPart;
                    DirInfo->FileInfo.LastWriteTime = ((PLARGE_INTEGER)&FindData.ftLastWriteTime)->QuadPart;
                    DirInfo->FileInfo.ChangeTime = DirInfo->FileInfo.LastWriteTime;
                    DirInfo->FileInfo.IndexNumber = 0;
                    DirInfo->FileInfo.HardLinks = 0;
                    memcpy(DirInfo->FileNameBuf, FindData.cFileName, Length * sizeof(WCHAR));

                    if (!FspFileSystemFillDirectoryBuffer(&FileContext->DirBuffer, DirInfo, &DirBufferResult))
                        break;
                } while (FindNextFileW(FindHandle, &FindData));

                FindClose(FindHandle);
            }
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

static NTSTATUS PtfsCreate(PWSTR BaseUrl, PWSTR UserName, PWSTR Password,
    PWSTR MountPoint, UINT32 DebugFlags,
    PTFS** PPtfs)
{
    FILETIME CreationTime;
    FSP_FSCTL_VOLUME_PARAMS VolumeParams;
    PTFS* Ptfs = 0;
    SIZE_T Length;
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

    Length = (wcslen(BaseUrl) + 1) * sizeof(WCHAR);
    Ptfs->BaseUrl = malloc(Length);
    if (0 == Ptfs->BaseUrl)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }
    memcpy(Ptfs->BaseUrl, BaseUrl, Length);

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
    if (0 != Ptfs->FileSystem)
        FspFileSystemDelete(Ptfs->FileSystem);

    if (0 != Ptfs->BaseUrl)
        free(Ptfs->BaseUrl);

    if (0 != Ptfs->Password)
    {
        /* do not leave the password in freed memory (REQ-MOUNT-010) */
        SecureZeroMemory(Ptfs->Password, wcslen(Ptfs->Password) * sizeof(WCHAR));
        free(Ptfs->Password);
    }

    if (0 != Ptfs->UserName)
        free(Ptfs->UserName);

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
    PTFS* Ptfs = 0;
    NTSTATUS Result;

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

    /* probe the server before exposing the volume: an unreachable server or
     * rejected credentials must abort the mount (REQ-MOUNT-011, REQ-MOUNT-012)
     */
    Result = AtelierProbe(BaseUrlBuf, UserName, Password);
    if (!NT_SUCCESS(Result))
    {
        fail(L"mount of %s aborted: Atelier server is not usable", MountPoint);
        goto exit;
    }

    Result = PtfsCreate(BaseUrlBuf, UserName, Password, MountPoint, DebugFlags, &Ptfs);
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
