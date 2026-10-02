/*
 * FATNT - base integer types for building the shared on-disk helpers
 * (fatsup.c / fatdisk.h) outside the kernel: the format and check tools
 * and the test harness. The kernel build uses ntifs.h instead.
 */

#ifndef _FATUSR_H_
#define _FATUSR_H_

#ifdef _WIN32

/*
 * On Windows (the format/check tools), the NT base types - UCHAR, USHORT,
 * ULONG, LONG, WCHAR, BOOLEAN, ULONGLONG, VOID and the rest - come from the
 * Win32 headers, so pull those in and do not redefine them (defining our own
 * would clash with <windows.h>, which the tools also use for the Win32 API).
 */
#include <windows.h>

#else

/* Off Windows (the Linux user-mode test harness), define them ourselves. */
#include <stdint.h>
typedef void            VOID, *PVOID;
typedef uint8_t         UCHAR, *PUCHAR, BOOLEAN, *PBOOLEAN;
typedef char            CHAR, *PCHAR;
typedef uint16_t        USHORT, *PUSHORT, WCHAR, *PWCHAR, *PWSTR;
typedef int16_t         SHORT, CSHORT;
typedef uint32_t        ULONG, *PULONG;
typedef int32_t         LONG, *PLONG;
typedef uint64_t        ULONGLONG, *PULONGLONG;
typedef int64_t         LONGLONG, *PLONGLONG;

#endif /* _WIN32 */

#include <string.h>

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#ifndef RtlZeroMemory
#define RtlZeroMemory(d, l) memset((d), 0, (l))
#define RtlCopyMemory(d, s, l) memcpy((d), (s), (l))
#endif

#endif /* _FATUSR_H_ */
