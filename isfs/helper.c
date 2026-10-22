/**
 * @file helper.c
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

#include <windows.h>
#include <strsafe.h>
#include <wctype.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "helper.h"

/* ------------------------------------------------------------------------
 * Strings, names and timestamps
 * ------------------------------------------------------------------------ */

const PCWSTR Categories[CATEGORY_COUNT] = { L"CLS", L"CSP", L"RTN", L"OTH" };

PWSTR DupString(PCWSTR Text)
{
    SIZE_T Length;
    PWSTR Copy;

    if (0 == Text)
        return 0;

    Length = wcslen(Text);
    Copy = malloc((Length + 1) * sizeof(WCHAR));
    if (0 == Copy)
        return 0;
    memcpy(Copy, Text, (Length + 1) * sizeof(WCHAR));

    return Copy;
}

ULONG SplitByDelimiter(PCWSTR Text, WCHAR Delimiter1, WCHAR Delimiter2, PWSTR** PSegments)
{
    SIZE_T Length = wcslen(Text);
    WCHAR* Buffer;
    PWSTR* Segments;
    WCHAR* Cursor;
    WCHAR* SegmentStart = 0;
    SIZE_T Index;
    ULONG Count = 0;
    BOOLEAN InSegment = FALSE;

    *PSegments = 0;

    Buffer = malloc((Length + 2) * sizeof(WCHAR));
    if (0 == Buffer)
        return 0;
    Segments = malloc((Length + 2) * sizeof(PWSTR));
    if (0 == Segments)
    {
        free(Buffer);
        return 0;
    }

    Cursor = Buffer;
    for (Index = 0; Index <= Length; Index++)
    {
        WCHAR Ch = Text[Index];

        if (L'\0' == Ch || Ch == Delimiter1 || (L'\0' != Delimiter2 && Ch == Delimiter2))
        {
            if (InSegment)
            {
                *Cursor = L'\0';
                Segments[Count++] = SegmentStart;
                Cursor++;
                InSegment = FALSE;
            }
        }
        else
        {
            if (!InSegment)
                SegmentStart = Cursor;
            *Cursor++ = Ch;
            InSegment = TRUE;
        }
    }

    *PSegments = Segments;

    return Count;
}

VOID FreeSegments(PWSTR* Segments, ULONG Count)
{
    if (0 == Segments)
        return;

    /* the segments share one buffer; the first one owns it */
    free(Segments[0]);
    free(Segments);
}

ULONG SplitDocumentName(PCWSTR Name, PWSTR** PSegments)
{
    SIZE_T Length = wcslen(Name);
    ULONG Dots = 0, Count, Index, SegmentIndex = 0;
    const WCHAR* Ch;
    const WCHAR* Start;
    WCHAR* Buffer;
    WCHAR* Cursor;
    PWSTR* Segments;

    *PSegments = 0;

    for (Ch = Name; L'\0' != *Ch; Ch++)
        if (L'.' == *Ch)
            Dots++;

    /* the number of segments is the number of dots, but a name without a dot
     * and a name that begins with a dot are one segment
     */
    Count = (0 == Dots || L'.' == Name[0]) ? 1 : Dots;

    Buffer = malloc((Length + Count + 1) * sizeof(WCHAR));
    if (0 == Buffer)
        return 0;
    Segments = malloc(Count * sizeof *Segments);
    if (0 == Segments)
    {
        free(Buffer);
        return 0;
    }

    Cursor = Buffer;
    Start = Name;
    for (Index = 0;; Index++)
    {
        WCHAR Current = Name[Index];

        if (L'\0' == Current || (L'.' == Current && SegmentIndex + 1 < Count))
        {
            SIZE_T SegmentLength = (SIZE_T)(&Name[Index] - Start);

            memcpy(Cursor, Start, SegmentLength * sizeof(WCHAR));
            Cursor[SegmentLength] = L'\0';
            Segments[SegmentIndex++] = Cursor;
            Cursor += SegmentLength + 1;
            if (L'\0' == Current)
                break;
            Start = &Name[Index + 1];
        }
    }

    *PSegments = Segments;

    return Count;
}

PWSTR JoinDotted(PWSTR* Segments, ULONG Count)
{
    SIZE_T Length = 1, Index;
    PWSTR Text;

    for (Index = 0; Index < Count; Index++)
        Length += wcslen(Segments[Index]) + 1;

    Text = malloc(Length * sizeof(WCHAR));
    if (0 == Text)
        return 0;

    Text[0] = L'\0';
    for (Index = 0; Index < Count; Index++)
    {
        if (Index > 0)
            wcscat_s(Text, Length, L".");
        wcscat_s(Text, Length, Segments[Index]);
    }

    return Text;
}

BOOL IsLegalPathSegment(PCWSTR Segment)
{
    static const PCWSTR Reserved[] =
    {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"
    };
    SIZE_T Length = wcslen(Segment);
    SIZE_T Index;
    WCHAR Name[8];
    SIZE_T NameLength = 0;

    if (0 == Length)
        return FALSE;

    /* "." and ".." */
    if (L'.' == Segment[0] && (1 == Length || (2 == Length && L'.' == Segment[1])))
        return FALSE;

    /* a name that ends with a space or a period */
    if (L' ' == Segment[Length - 1] || L'.' == Segment[Length - 1])
        return FALSE;

    for (Index = 0; Index < Length; Index++)
    {
        WCHAR Ch = Segment[Index];

        if (Ch < 0x20)
            return FALSE;
        if (L'<' == Ch || L'>' == Ch || L':' == Ch || L'"' == Ch || L'/' == Ch ||
            L'\\' == Ch || L'|' == Ch || L'?' == Ch || L'*' == Ch)
            return FALSE;
    }

    /* the reserved device names, with or without an extension */
    for (Index = 0; Index < Length && L'.' != Segment[Index] && NameLength + 1 < ARRAYSIZE(Name); Index++)
        Name[NameLength++] = towupper(Segment[Index]);
    Name[NameLength] = L'\0';
    for (Index = 0; Index < ARRAYSIZE(Reserved); Index++)
        if (0 == wcscmp(Name, Reserved[Index]))
            return FALSE;

    return TRUE;
}

UINT64 HashEntryName(PCWSTR Namespace, PCWSTR Name)
{
    static const BYTE Zero = 0;
    UINT64 Hash = 14695981039346656037ULL;
    SIZE_T Index, Length;

    Length = wcslen(Namespace);
    for (Index = 0; Index < Length; Index++)
    {
        Hash ^= (BYTE)Namespace[Index];
        Hash *= 1099511628211ULL;
    }

    Hash ^= Zero;
    Hash *= 1099511628211ULL;

    Length = wcslen(Name);
    for (Index = 0; Index < Length; Index++)
    {
        Hash ^= (BYTE)Name[Index];
        Hash *= 1099511628211ULL;
    }

    return Hash;
}

BOOL ReadDigits(PCWSTR* PText, ULONG Count, UINT32* PValue)
{
    ULONG Index;
    UINT32 Value = 0;

    for (Index = 0; Index < Count; Index++)
    {
        if ((*PText)[Index] < L'0' || (*PText)[Index] > L'9')
            return FALSE;
        Value = Value * 10 + (UINT32)((*PText)[Index] - L'0');
    }

    *PText += Count;
    *PValue = Value;

    return TRUE;
}

BOOL ParseTimestamp(PCWSTR Ts, UINT64* PFileTime)
{
    SYSTEMTIME SystemTime = { 0 };
    FILETIME FileTime;
    UINT32 Year, Month, Day, Hour, Minute, Second, Milli;
    PCWSTR Text = Ts;

    if (0 == Ts)
        return FALSE;

    if (!ReadDigits(&Text, 4, &Year) || L'-' != *Text++)
        return FALSE;
    if (!ReadDigits(&Text, 2, &Month) || L'-' != *Text++)
        return FALSE;
    if (!ReadDigits(&Text, 2, &Day) || L' ' != *Text++)
        return FALSE;
    if (!ReadDigits(&Text, 2, &Hour) || L':' != *Text++)
        return FALSE;
    if (!ReadDigits(&Text, 2, &Minute) || L':' != *Text++)
        return FALSE;
    if (!ReadDigits(&Text, 2, &Second))
        return FALSE;
    if (L'.' == *Text)
    {
        Text++;
        if (!ReadDigits(&Text, 3, &Milli))
            return FALSE;
    }
    else
    {
        Milli = 0;
    }

    if (L'\0' != *Text)
        return FALSE;

    SystemTime.wYear = (WORD)Year;
    SystemTime.wMonth = (WORD)Month;
    SystemTime.wDay = (WORD)Day;
    SystemTime.wHour = (WORD)Hour;
    SystemTime.wMinute = (WORD)Minute;
    SystemTime.wSecond = (WORD)Second;
    SystemTime.wMilliseconds = (WORD)Milli;

    if (!SystemTimeToFileTime(&SystemTime, &FileTime))
        return FALSE;

    *PFileTime = (UINT64)((PLARGE_INTEGER)&FileTime)->QuadPart;

    return TRUE;
}

int CategoryIndex(PCWSTR Category)
{
    ULONG Index;

    for (Index = 0; Index < CATEGORY_COUNT; Index++)
        if (EqualsNameIgnoreCase(Category, Categories[Index]))
            return (int)Index;

    return -1;
}

/* the comparison is ordinal and invariant, not locale dependent: _wcsicmp in
 * the "C" locale folds only ASCII, so a non-ASCII path in a different casing
 * would not resolve, while NTFS treats it as the same name (REQ-FS-004)
 */
BOOL EqualsNameIgnoreCase(PCWSTR Left, PCWSTR Right)
{
    if (0 == Left || 0 == Right)
        return FALSE;

    return CSTR_EQUAL == CompareStringOrdinal(Left, -1, Right, -1, TRUE);
}

BOOL EqualsCharIgnoreCase(WCHAR Left, WCHAR Right)
{
    return CSTR_EQUAL == CompareStringOrdinal(&Left, 1, &Right, 1, TRUE);
}
