/*
 * FATNT - debug trace to the QEMU debug console (I/O port 0xE9), for
 * test builds made with -DFAT_DBGPORT (EXTRACFLAGS=-DFAT_DBGPORT
 * build/build_cross.sh). Empty in a normal build.
 */

#include "fat.h"

#ifdef FAT_DBGPORT

#include <stdarg.h>

static VOID FatPortChar(char c)
{
    __asm__ __volatile__("outb %b0,%w1" :: "a"(c), "d"((unsigned short)0xE9));
}

static VOID FatPortNum(ULONG v, ULONG base, int width)
{
    char t[12];
    int i = 0;
    do { t[i++] = "0123456789ABCDEF"[v % base]; v /= base; } while (v && i < 11);
    while (i < width) t[i++] = '0';
    while (i) FatPortChar(t[--i]);
}

VOID __cdecl FatPortPrintf(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    for (; *f; f++) {
        int width = 0;
        if (*f != '%') { FatPortChar(*f); continue; }
        f++;
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == 'l') f++;
        switch (*f) {
        case 's': { const char *s = va_arg(ap, const char *); while (s && *s) FatPortChar(*s++); break; }
        case 'c': FatPortChar((char)va_arg(ap, int)); break;
        case 'd': { long v = va_arg(ap, long); if (v < 0) { FatPortChar('-'); v = -v; } FatPortNum((ULONG)v, 10, width); break; }
        case 'u': FatPortNum(va_arg(ap, ULONG), 10, width); break;
        case 'x': case 'X': case 'p': FatPortNum(va_arg(ap, ULONG), 16, width); break;
        case 'w': { PUNICODE_STRING u = va_arg(ap, PUNICODE_STRING); USHORT i;
                    if (f[1] == 'Z') f++;
                    for (i = 0; u && u->Buffer && i < u->Length / 2; i++) FatPortChar((char)u->Buffer[i]);
                    break; }
        default: FatPortChar(*f); break;
        }
    }
    va_end(ap);
}

#endif /* FAT_DBGPORT */
