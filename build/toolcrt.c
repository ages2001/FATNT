/*
 * toolcrt.c - a tiny freestanding C runtime so fatfmt and fatchk link with
 * -nostdlib and run on Windows NT 3.1 (which ships CRTDLL, not msvcrt) without
 * any C-runtime DLL. It implements only what those two tools use. Build the
 * tool with:
 *   -Dmain=appmain -Dprintf=fat_printf -Dfprintf=fat_fprintf
 *   -Dsprintf=fat_sprintf -Dsnprintf=fat_snprintf
 *   -Dmalloc=fat_malloc -Dfree=fat_free -Dcalloc=fat_calloc -Drealloc=fat_realloc
 *   -Dstrtoul=fat_strtoul -Dtime=fat_time -Dtoupper=fat_toupper
 *   -D_stricmp=fat_stricmp
 * (memset/memcpy/memmove/memcmp/strlen/strcmp/strncmp/strcpy/strchr are the
 * normal names; the compiler/ld resolve them to the versions below.)
 */
#include <windows.h>
#include <stdarg.h>

int appmain(int, char **);

/* ---- raw console output ---- */
static void out_h(DWORD std, const char *s, unsigned n)
{ DWORD w; WriteFile(GetStdHandle(std), s, n, &w, 0); }

/* ---- string / memory ---- */
void *memset(void *d, int c, unsigned n){ unsigned char *p=d; while(n--)*p++=(unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned n){ unsigned char *a=d; const unsigned char *b=s; while(n--)*a++=*b++; return d; }
void *memmove(void *d, const void *s, unsigned n){ unsigned char *a=d; const unsigned char *b=s;
    if(a<b){while(n--)*a++=*b++;} else {a+=n;b+=n;while(n--)*--a=*--b;} return d; }
int memcmp(const void *a, const void *b, unsigned n){ const unsigned char *x=a,*y=b;
    while(n--){ if(*x!=*y) return *x-*y; x++;y++; } return 0; }
unsigned strlen(const char *s){ const char *p=s; while(*p)p++; return (unsigned)(p-s); }
int strcmp(const char *a, const char *b){ while(*a&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }
int strncmp(const char *a, const char *b, unsigned n){ while(n&&*a&&*a==*b){a++;b++;n--;} return n?(unsigned char)*a-(unsigned char)*b:0; }
char *strcpy(char *d, const char *s){ char *r=d; while((*d++=*s++)); return r; }
char *strncpy(char *d, const char *s, unsigned n){ char *r=d; while(n&&(*d=*s)){d++;s++;n--;} while(n--)*d++=0; return r; }
char *strchr(const char *s, int c){ for(;*s;s++) if(*s==(char)c) return (char*)s; return (c==0)?(char*)s:0; }
int fat_stricmp(const char *a, const char *b){ return lstrcmpiA(a,b); }
int strcasecmp(const char *a, const char *b){ return lstrcmpiA(a,b); }
int strncasecmp(const char *a, const char *b, unsigned n){
    while(n){ int ca=*a,cb=*b; if(ca>='A'&&ca<='Z')ca+=32; if(cb>='A'&&cb<='Z')cb+=32;
        if(ca!=cb||!ca) return ca-cb; a++;b++;n--; } return 0; }
int getchar(void){ char c; DWORD r; if(ReadFile(GetStdHandle(STD_INPUT_HANDLE),&c,1,&r,0)&&r==1) return (unsigned char)c; return -1; }

/* ---- ctype ---- */
int fat_toupper(int c){ return (c>='a'&&c<='z')?c-32:c; }
int tolower(int c){ return (c>='A'&&c<='Z')?c+32:c; }
int isdigit(int c){ return c>='0'&&c<='9'; }
int isalpha(int c){ return (c>='A'&&c<='Z')||(c>='a'&&c<='z'); }
int isspace(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }

/* ---- heap (process heap) ---- */
void *fat_malloc(unsigned n){ return HeapAlloc(GetProcessHeap(), 0, n?n:1); }
void  fat_free(void *p){ if(p) HeapFree(GetProcessHeap(), 0, p); }
void *fat_calloc(unsigned a, unsigned b){ return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (a*b)?(a*b):1); }
void *fat_realloc(void *p, unsigned n){ if(!p) return fat_malloc(n);
    return HeapReAlloc(GetProcessHeap(), 0, p, n?n:1); }

/* ---- numeric parse / time ---- */
unsigned long fat_strtoul(const char *s, char **end, int base)
{
    unsigned long v=0; const char *p=s;
    while(*p==' '||*p=='\t') p++;
    if((base==16||base==0) && p[0]=='0' && (p[1]=='x'||p[1]=='X')){ p+=2; base=16; }
    if(base==0) base=10;
    for(;;){ int d; char c=*p;
        if(c>='0'&&c<='9') d=c-'0';
        else if(c>='a'&&c<='f') d=c-'a'+10;
        else if(c>='A'&&c<='F') d=c-'A'+10;
        else break;
        if(d>=base) break;
        v=v*base+d; p++; }
    if(end) *end=(char*)p;
    return v;
}
long fat_time(long *t){ /* seconds since 1970 from the system clock */
    FILETIME ft; ULARGE_INTEGER u; long s;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart=ft.dwLowDateTime; u.HighPart=ft.dwHighDateTime;
    s=(long)((u.QuadPart - 116444736000000000ULL)/10000000ULL);
    if(t)*t=s; return s;
}
struct tm *fat_localtime(const time_t *t){ static struct tm r; SYSTEMTIME s; GetLocalTime(&s);
    r.tm_year=s.wYear-1900; r.tm_mon=s.wMonth-1; r.tm_mday=s.wDay;
    r.tm_hour=s.wHour; r.tm_min=s.wMinute; r.tm_sec=s.wSecond;
    r.tm_wday=s.wDayOfWeek; r.tm_yday=0; r.tm_isdst=0; (void)t; return &r; }

/* ---- formatting ---- */
static void u64toa(unsigned __int64 v, char *b){ char t[24]; int i=0,j=0;
    if(!v){b[0]='0';b[1]=0;return;} while(v){t[i++]=(char)('0'+(int)(v%10));v/=10;} while(i)b[j++]=t[--i]; b[j]=0; }
static void utoa10(unsigned long v, char *b){ u64toa((unsigned __int64)v,b); }
static void utoa16(unsigned long v, char *b, int width){ char t[16]; int i=0,j=0; const char*h="0123456789abcdef";
    if(!v)t[i++]='0'; while(v){t[i++]=h[v&15];v>>=4;} while(i<width)t[i++]='0'; while(i)b[j++]=t[--i]; b[j]=0; }

/* out: buffer (sprintf) or NULL (to stdout/stderr via `std`). Returns length. */
static int vfmt(char *out, unsigned cap, DWORD std, const char *f, va_list ap)
{
    char num[24]; int n=0;
    #define PUT(ch) do{ if(out){ if((unsigned)n<cap-1) out[n]=(ch); } else { char _c=(ch); out_h(std,&_c,1);} n++; }while(0)
    #define PUTS(s) do{ const char*_p=(s); while(*_p){PUT(*_p);_p++;} }while(0)
    for(; *f; f++){
        if(*f!='%'){ PUT(*f); continue; }
        f++;
        int width=0, zero=0;
        if(*f=='0'){ zero=1; f++; }
        while(*f>='0'&&*f<='9'){ width=width*10+(*f-'0'); f++; }
        if(*f=='l' && f[1]=='l'){ f+=2;
            if(*f=='u'||*f=='d'){ unsigned __int64 v=va_arg(ap,unsigned __int64); u64toa(v,num); PUTS(num); }
            continue;
        }
        if(*f=='l') f++;
        if(*f=='s'){ const char*s=va_arg(ap,const char*); if(!s)s="(null)"; PUTS(s); }
        else if(*f=='u'||*f=='d'){ unsigned long v=va_arg(ap,unsigned long); utoa10(v,num); PUTS(num); }
        else if(*f=='x'){ unsigned long v=va_arg(ap,unsigned long); utoa16(v,num,zero?width:0); PUTS(num); }
        else if(*f=='c'){ int c=va_arg(ap,int); PUT((char)c); }
        else if(*f=='%'){ PUT('%'); }
    }
    if(out && cap) out[(unsigned)n<cap?n:cap-1]=0;
    return n;
    #undef PUT
    #undef PUTS
}

int fat_printf(const char *fmt, ...){ va_list ap; int r; va_start(ap,fmt); r=vfmt(0,0,STD_OUTPUT_HANDLE,fmt,ap); va_end(ap); return r; }
/* fprintf: first arg is a stream; send stderr-ish streams to stderr, else stdout */
int fat_fprintf(void *stream, const char *fmt, ...){ va_list ap; int r; DWORD h=STD_OUTPUT_HANDLE;
    if(stream==(void*)2 || stream==GetStdHandle(STD_ERROR_HANDLE)) h=STD_ERROR_HANDLE;
    va_start(ap,fmt); r=vfmt(0,0,h,fmt,ap); va_end(ap); return r; }
int fat_sprintf(char *b, const char *fmt, ...){ va_list ap; int r; va_start(ap,fmt); r=vfmt(b,0x7fffffff,0,fmt,ap); va_end(ap); return r; }
int fat_snprintf(char *b, unsigned cap, const char *fmt, ...){ va_list ap; int r; va_start(ap,fmt); r=vfmt(b,cap,0,fmt,ap); va_end(ap); return r; }

/* stderr/stdout as passed to fprintf - the tools use the symbols `stderr`/`stdout` */
void *stderr = (void*)2;
void *stdout = (void*)1;

void mainCRTStartup(void)
{
    char *p = GetCommandLineA();
    static char *argv[32]; int argc=0;
    while(*p && argc<32){
        while(*p==' ') p++;
        if(!*p) break;
        if(*p=='"'){ p++; argv[argc++]=p; while(*p && *p!='"') p++; if(*p)*p++=0; }
        else { argv[argc++]=p; while(*p && *p!=' ') p++; if(*p)*p++=0; }
    }
    ExitProcess((UINT)appmain(argc, argv));
}
