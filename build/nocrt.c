/*
 * Tiny CRT shim so fatinst.c links with -nostdlib and runs on NT 3.1 (which
 * ships CRTDLL, not msvcrt). Build/test use only; the shipping fatinst is
 * built with Visual C++ -ML. Compile fatinst.c with:
 *   -Dmain=appmain -Dprintf=fat_printf -Dsprintf=fat_sprintf
 *   -D_stricmp=fat_stricmp -Dstrlen=fat_strlen
 */
#include <windows.h>
#include <stdarg.h>

int appmain(int, char **);

static void outs(const char *s, unsigned n){ DWORD w; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &w, 0); }

unsigned int fat_strlen(const char *s){ const char *p=s; while(*p) p++; return (unsigned)(p-s); }
int fat_stricmp(const char *a, const char *b){ return lstrcmpiA(a,b); }

static void utoa_(unsigned long v, char *b){ char t[16]; int i=0,j=0;
    if(!v){b[0]='0';b[1]=0;return;} while(v){t[i++]=(char)('0'+v%10);v/=10;} while(i)b[j++]=t[--i]; b[j]=0; }

static void vfmt(char *out, const char *f, va_list ap)
{
    char num[16];
    for(; *f; f++){
        if(*f!='%'){ if(out)*out++=*f; else outs(f,1); continue; }
        f++;
        if(*f=='l') f++;
        if(*f=='s'){ const char *s=va_arg(ap,const char*); if(out){while(*s)*out++=*s++;} else outs(s,fat_strlen(s)); }
        else if(*f=='u'||*f=='d'){ unsigned long v=va_arg(ap,unsigned long); utoa_(v,num);
            char *s=num; if(out){while(*s)*out++=*s++;} else outs(num,fat_strlen(num)); }
        else if(*f=='%'){ if(out)*out++='%'; else outs("%",1); }
    }
    if(out)*out=0;
}

int fat_printf(const char *fmt, ...){ va_list ap; va_start(ap,fmt); vfmt(0,fmt,ap); va_end(ap); return 0; }
int fat_sprintf(char *b, const char *fmt, ...){ va_list ap; va_start(ap,fmt); vfmt(b,fmt,ap); va_end(ap); return 0; }

void mainCRTStartup(void)
{
    char *p = GetCommandLineA();
    static char *argv[16]; int argc=0;
    while(*p && argc<16){
        while(*p==' ') p++;
        if(!*p) break;
        if(*p=='"'){ p++; argv[argc++]=p; while(*p && *p!='"') p++; if(*p)*p++=0; }
        else { argv[argc++]=p; while(*p && *p!=' ') p++; if(*p)*p++=0; }
    }
    ExitProcess((UINT)appmain(argc, argv));
}
