/**
 * @file helper.h
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

#ifndef ISFS_HELPER_H
#define ISFS_HELPER_H

#include <windows.h>

/* the categories of the volume; the order is the order of the root listing
 * and the order of the per category caches. The array is declared with its
 * size, so that ARRAYSIZE works in the users of this header.
 */
#define CATEGORY_COUNT                 4
extern const PCWSTR Categories[CATEGORY_COUNT];

/* the copy of a string, allocated with malloc(); 0 on failure */
PWSTR DupString(PCWSTR Text);

/* splits a text into its segments; the segments are separated by the given
 * delimiters and the segments are stored in one buffer
 */
ULONG SplitByDelimiter(PCWSTR Text, WCHAR Delimiter1, WCHAR Delimiter2, PWSTR** PSegments);

/* frees the segments of a split, including the buffer they share */
VOID FreeSegments(PWSTR* Segments, ULONG Count);

/* the separator that a category uses inside a document name: the dots of a
 * class, routine or "other" document, the slashes of a CSP application path
 * (REQ-MAP-004)
 */
WCHAR DocumentSeparator(PCWSTR Category);

/* the name of a document in the form that the API expects in a request: no
 * leading separator and forward slashes inside the name, e.g.
 * "/csp/user/index.csp" becomes "csp/user/index.csp". The copy is allocated
 * with malloc(); 0 on failure (REQ-MAP-005).
 */
PWSTR CanonicalDocumentName(PCWSTR Name);

/* splits a document name into its package levels and its file name: the
 * extension stays with the file and every earlier separator becomes a level
 * (REQ-MAP-004). With dots, a name that begins with a dot is not decomposed;
 * with slashes, empty segments are skipped.
 */
ULONG SplitDocumentName(PCWSTR Name, WCHAR Separator, PWSTR** PSegments);

/* a segment that Windows would reject is not exposed (REQ-MAP-013) */
BOOL IsLegalPathSegment(PCWSTR Segment);

/* the index number of an entry is the FNV-1a 64 hash of the namespace, a NUL
 * character and the name of the entry (REQ-FS-012)
 */
UINT64 HashEntryName(PCWSTR Namespace, PCWSTR Name);

/* reads the given count of decimal digits of a text and advances the text */
BOOL ReadDigits(PCWSTR* PText, ULONG Count, UINT32* PValue);

/* the timestamp of a document has no time zone; it is read as a local time
 * (REQ-FS-007)
 */
BOOL ParseTimestamp(PCWSTR Ts, UINT64* PFileTime);

/* the index of a category in Categories, or -1 */
int CategoryIndex(PCWSTR Category);

/* compares two names case-insensitively with the invariant Unicode case table,
 * which is the semantics of NTFS; the C runtime comparison depends on the
 * locale and does not fold non-ASCII names (REQ-FS-004, REQ-MAP-009)
 */
BOOL EqualsNameIgnoreCase(PCWSTR Left, PCWSTR Right);

/* compares two characters case-insensitively with the invariant table; used by
 * the wildcard matcher of the listings (REQ-FS-004)
 */
BOOL EqualsCharIgnoreCase(WCHAR Left, WCHAR Right);

#endif /* ISFS_HELPER_H */
