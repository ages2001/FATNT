/*
 * fatres.c - runtime binding of the executive-resource API for the single
 * universal (NT 3.1 - 4.0) binary.
 *
 * The resource functions split across NT versions with no statically-
 * importable common set:
 *   - NT 3.1 exports the legacy names (ExInitializeResource,
 *     ExAcquireResourceExclusive/Shared, ExReleaseResourceForThread,
 *     ExDeleteResource) and operates the OLD ERESOURCE layout. It has no
 *     Lite forms.
 *   - NT 3.51+ export the Lite names (ExInitializeResourceLite, ...) and
 *     operate the NEW ERESOURCE layout. The legacy names still exist for
 *     source compatibility, but on NT 3.51 routing our Lite-era code through
 *     the legacy compat path mismatches the structure and faults inside the
 *     kernel's resource wait (bugcheck 0xA on a bogus wait object).
 *
 * A static import that the running kernel does not export makes the OS loader
 * reject the driver as "missing or corrupt", so neither name set can be
 * imported outright. Instead the driver imports none of them and, at
 * DriverEntry, finds ntoskrnl in memory and resolves each function by name -
 * preferring the Lite form, falling back to the legacy form. Each NT then
 * runs its own native, structurally-matched resource functions (and true
 * shared acquisition is preserved everywhere).
 */

#include "fat.h"

#ifdef FAT_NT31

FAT_PINITRES FatpInitRes      = NULL;
FAT_PACQRES  FatpAcqExcl      = NULL;
FAT_PACQRES  FatpAcqShared    = NULL;
FAT_PDELRES  FatpDelRes       = NULL;
FAT_PRELRES  FatpRelForThread = NULL;

static int FatpStrEq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (*a == 0 && *b == 0);
}

/*
 * Walk ntoskrnl's export directory (the image is mapped at Base with sections
 * at their RVAs) and return the address of the named export, or NULL.
 */
static PVOID FatpFindExport(ULONG Base, const char *Name)
{
    ULONG   lfanew  = *(ULONG *)(Base + 0x3C);
    ULONG nt    = Base + lfanew;
    ULONG   expRva  = *(ULONG *)(nt + 0x78);     /* DataDirectory[0].VirtualAddress */
    ULONG ed;
    ULONG   nNames, funcsRva, namesRva, ordsRva, i;

    if (*(USHORT *)Base != 0x5A4D) return NULL;   /* 'MZ' */
    if (*(ULONG  *)nt   != 0x00004550) return NULL;/* 'PE\0\0' */
    if (expRva == 0) return NULL;

    ed       = Base + expRva;
    nNames   = *(ULONG *)(ed + 0x18);
    funcsRva = *(ULONG *)(ed + 0x1C);
    namesRva = *(ULONG *)(ed + 0x20);
    ordsRva  = *(ULONG *)(ed + 0x24);

    for (i = 0; i < nNames; i++) {
        ULONG   nameRva = *(ULONG  *)(Base + namesRva + i * 4);
        USHORT  ord     = *(USHORT *)(Base + ordsRva  + i * 2);
        ULONG   funcRva;
        if (FatpStrEq((const char *)(Base + nameRva), Name)) {
            funcRva = *(ULONG *)(Base + funcsRva + ord * 4);
            return (PVOID)(Base + funcRva);
        }
    }
    return NULL;
}

static PVOID FatpResolve2(ULONG Base, const char *Lite, const char *Legacy)
{
    PVOID p = FatpFindExport(Base, Lite);
    if (p == NULL) p = FatpFindExport(Base, Legacy);
    return p;
}

/*
 * Resolve the resource API against the running ntoskrnl. Called first thing in
 * DriverEntry, before any resource is initialised. Returns FALSE if ntoskrnl
 * could not be located or a function is missing (the driver then fails to
 * load cleanly rather than faulting later).
 */
BOOLEAN FatResolveResourceApi(VOID)
{
    /* KeInitializeEvent is imported from ntoskrnl, so its address lands inside
     * the ntoskrnl image; round down to the page and scan back to the 'MZ'. */
    ULONG p = ((ULONG)(PVOID)&KeInitializeEvent) & ~(ULONG)0xFFF;
    ULONG limit = p - 0x02000000;            /* don't scan more than 32 MB */
    ULONG base = 0;

    for (; p > limit; p -= 0x1000) {
        if (*(USHORT *)p == 0x5A4D) {            /* 'MZ' */
            ULONG lfanew = *(ULONG *)(p + 0x3C);
            if (lfanew < 0x1000 && *(ULONG *)(p + lfanew) == 0x00004550) {
                base = p;
                break;
            }
        }
    }
    if (base == 0) return FALSE;

    FatpInitRes      = (FAT_PINITRES)FatpResolve2(base, "ExInitializeResourceLite",        "ExInitializeResource");
    FatpAcqExcl      = (FAT_PACQRES) FatpResolve2(base, "ExAcquireResourceExclusiveLite",  "ExAcquireResourceExclusive");
    FatpAcqShared    = (FAT_PACQRES) FatpResolve2(base, "ExAcquireResourceSharedLite",     "ExAcquireResourceShared");
    FatpDelRes       = (FAT_PDELRES) FatpResolve2(base, "ExDeleteResourceLite",            "ExDeleteResource");
    FatpRelForThread = (FAT_PRELRES) FatpResolve2(base, "ExReleaseResourceForThreadLite",  "ExReleaseResourceForThread");

    return (FatpInitRes && FatpAcqExcl && FatpAcqShared &&
            FatpDelRes && FatpRelForThread);
}

#endif /* FAT_NT31 */
