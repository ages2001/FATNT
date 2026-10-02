/*
 * toolcrt.h - force-included prefix for building fatfmt/fatchk freestanding
 * (see toolcrt.c). Provides the few C-library types and declarations the tools
 * need, so their <stdio.h>/<stdlib.h>/<string.h>/<ctype.h>/<time.h> can be
 * blocked (those pull in CRT-only macros like stderr/toupper). <windows.h> is
 * still used normally for the Win32 API.
 */
#ifndef FAT_TOOLCRT_H
#define FAT_TOOLCRT_H

#ifndef NULL
#define NULL ((void*)0)
#endif
typedef long time_t;

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year,
        tm_wday, tm_yday, tm_isdst;
};

/* stdio-ish */
extern void *stdout, *stderr;
int   fat_printf(const char *, ...);
int   fat_fprintf(void *, const char *, ...);
int   fat_sprintf(char *, const char *, ...);
int   fat_snprintf(char *, unsigned, const char *, ...);

/* stdlib / string / ctype / time */
void *fat_malloc(unsigned);
void  fat_free(void *);
void *fat_calloc(unsigned, unsigned);
void *fat_realloc(void *, unsigned);
unsigned long fat_strtoul(const char *, char **, int);
long  fat_time(long *);
struct tm *fat_localtime(const time_t *);
int   fat_toupper(int);
int   fat_stricmp(const char *, const char *);
int   tolower(int), isdigit(int), isalpha(int), isspace(int);
void *memset(void *, int, unsigned);
void *memcpy(void *, const void *, unsigned);
void *memmove(void *, const void *, unsigned);
int   memcmp(const void *, const void *, unsigned);
unsigned strlen(const char *);
int   strcmp(const char *, const char *);
int   strncmp(const char *, const char *, unsigned);
char *strcpy(char *, const char *);
char *strncpy(char *, const char *, unsigned);
char *strchr(const char *, int);

#endif
