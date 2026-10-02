/*
 * FATNT - fatinst: install or remove the FATNT file system driver.
 *
 * FATNT is meant for Windows NT 3.1, 3.5, 3.51 and 4.0. (Windows 2000 and later
 * already ship a FAT driver, so FATNT is not needed there; the same sources do
 * build with the WDK for those systems, but installing it is optional.)
 *
 *   fatinst            Install (default): back up the system Fastfat.sys to
 *                      Fastfat.sav and put FATNT in its place, so FATNT becomes
 *                      the FAT file system for every FAT volume, the boot volume
 *                      included. This "replace" method is uniform across NT 3.1
 *                      - 4.0: the NT 3.1 OS loader loads the boot file system by
 *                      the fixed name Fastfat.sys, and on NT 3.5+ the Fastfat
 *                      service loads that same file, so standing in for it is
 *                      what hands the boot volume to FATNT everywhere.
 *   fatinst /DISABLE   NT 3.5+ alternative that overwrites no file: install
 *                      FATNT as a separate boot-start "fatnt" service and set
 *                      the Fastfat service to disabled. (Not effective for the
 *                      NT 3.1 boot volume - see the README.)
 *   fatinst /SYSTEM    With /DISABLE: system-start instead of boot-start, so it
 *                      does not touch the boot volume (data volumes only).
 *   fatinst /KEEPFAT   With /DISABLE: leave the Fastfat service enabled too.
 *   fatinst /U         Uninstall: restore Fastfat.sav, and undo a /DISABLE
 *                      install (disable fatnt, re-enable Fastfat).
 *
 * Everything is done with a file copy and the registry; the original Fastfat.sys
 * is preserved as Fastfat.sav and restored by /U. fatinst links against no C
 * runtime, so it needs no msvcrt/crtdll on NT 3.1. A restart applies it.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define SVC_FATNT   "SYSTEM\\CurrentControlSet\\Services\\fatnt"
#define SVC_FASTFAT "SYSTEM\\CurrentControlSet\\Services\\Fastfat"

#ifdef FATINST_LOG
static void LOGs(const char *s)
{
    HANDLE h = CreateFileA("C:\\FATINST.LOG", GENERIC_WRITE, 0, 0, OPEN_ALWAYS, 0, 0);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w; SetFilePointer(h, 0, 0, FILE_END);
        WriteFile(h, s, lstrlenA(s), &w, 0);
        CloseHandle(h);
    }
}
#define LOG(s) LOGs(s)
#else
#define LOG(s) ((void)0)
#endif

#define SERVICE_BOOT_START      0
#define SERVICE_SYSTEM_START    1
#define SERVICE_DISABLED        4
#define SERVICE_FILE_SYSTEM     2

static LONG SetDword(HKEY key, const char *name, DWORD val)
{
    return RegSetValueExA(key, name, 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
}

static LONG SetSz(HKEY key, const char *name, const char *val)
{
    return RegSetValueExA(key, name, 0, REG_SZ, (const BYTE *)val, (DWORD)(strlen(val) + 1));
}

static int SetServiceStart(const char *path, DWORD start)
{
    HKEY key;
    LONG r = RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE, &key);
    if (r != ERROR_SUCCESS) {
        return 0;       /* not present */
    }
    r = SetDword(key, "Start", start);
    RegFlushKey(key);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

/* Set EnableWriteSupport=1 on an existing service key (ignored if absent). */
static void SetEnableWrite(const char *path)
{
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        SetDword(key, "EnableWriteSupport", 1);
        RegFlushKey(key);
        RegCloseKey(key);
    }
}

static void DriversPath(const char *leaf, char *out, DWORD cb)
{
    char sysdir[MAX_PATH];
    UINT n = GetWindowsDirectoryA(sysdir, sizeof(sysdir));
    if (n == 0 || n >= sizeof(sysdir)) {
        out[0] = 0;
        return;
    }
    sprintf(out, "%s\\System32\\drivers\\%s", sysdir, leaf);
    (void)cb;
}

/* ------------------------------------------------------------------ */
/* Default install: FATNT replaces Fastfat.sys (original kept as .sav) */
/* ------------------------------------------------------------------ */

static int ReplaceInstall(void)
{
    char ff[MAX_PATH], sav[MAX_PATH];

    LOG("replace install\r\n");

    DriversPath("fastfat.sys", ff, sizeof(ff));
    DriversPath("fastfat.sav", sav, sizeof(sav));
    if (ff[0] == 0) {
        printf("  cannot find the system directory.\n");
        return 1;
    }

    /* Keep the original Fastfat.sys once, as Fastfat.sav */
    if (GetFileAttributesA(sav) == 0xFFFFFFFF && GetFileAttributesA(ff) != 0xFFFFFFFF) {
        if (CopyFileA(ff, sav, TRUE)) {
            printf("  backed up Fastfat.sys to Fastfat.sav\n");
        } else {
            printf("  warning: could not back up Fastfat.sys (error %lu).\n", GetLastError());
        }
    }
    LOG("backed up\r\n");

    if (!CopyFileA("fatnt.sys", ff, FALSE)) {
        printf("  error: could not install FATNT as %s (error %lu).\n", ff, GetLastError());
        printf("  put fatnt.sys next to fatinst.exe and run it again.\n");
        return 1;
    }
    LOG("copied fatnt over fastfat\r\n");

    /* FATNT loads under the Fastfat service name, so its settings live there */
    SetEnableWrite(SVC_FASTFAT);
    LOG("enablewrite set\r\n");

    printf("  installed FATNT as the FAT file system (Fastfat.sys; original saved as Fastfat.sav).\n");
    printf("Done. Restart to load FATNT.\n");
    LOG("done\r\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* /DISABLE: FATNT as its own service, Fastfat service disabled        */
/* (NT 3.5+; overwrites no file)                                       */
/* ------------------------------------------------------------------ */

static int DisableInstall(DWORD start, int disableFastfat)
{
    HKEY key;
    DWORD disp;
    char dst[MAX_PATH];
    LONG r;
    int fastfatDone = 0;

    LOG("disable install\r\n");

    r = RegCreateKeyExA(HKEY_LOCAL_MACHINE, SVC_FATNT, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_ALL_ACCESS, NULL, &key, &disp);
    if (r != ERROR_SUCCESS) {
        printf("  cannot create the fatnt service key (error %ld).\n", r);
        return 1;
    }
    SetDword(key, "Type", SERVICE_FILE_SYSTEM);
    SetDword(key, "Start", start);
    SetDword(key, "ErrorControl", 1);
    SetSz(key, "Group", "Boot file system");
    SetSz(key, "ImagePath", "System32\\DRIVERS\\fatnt.sys");
    SetDword(key, "EnableWriteSupport", 1);
    RegFlushKey(key);
    RegCloseKey(key);

    if (disableFastfat) {
        fastfatDone = SetServiceStart(SVC_FASTFAT, SERVICE_DISABLED);
    }

    DriversPath("fatnt.sys", dst, sizeof(dst));
    if (dst[0] && !CopyFileA("fatnt.sys", dst, FALSE)) {
        printf("  warning: could not copy fatnt.sys to %s (error %lu).\n", dst, GetLastError());
        printf("  copy it there yourself, then restart.\n");
    } else if (dst[0]) {
        printf("  fatnt.sys copied to System32\\drivers.\n");
    }

    printf("  fatnt service created (%s start).\n",
           start == SERVICE_BOOT_START ? "boot" : "system");
    if (!disableFastfat) {
        printf("  Fastfat left enabled.\n");
    } else if (fastfatDone) {
        printf("  Fastfat service disabled.\n");
    } else {
        printf("  note: Fastfat service not found (nothing to disable).\n");
    }
    printf("Done. Restart to load FATNT.\n");
    LOG("done\r\n");
    return 0;
}

/* ------------------------------------------------------------------ */

static int Uninstall(void)
{
    char ff[MAX_PATH], sav[MAX_PATH];

    /* Undo a /DISABLE install */
    if (SetServiceStart(SVC_FATNT, SERVICE_DISABLED)) {
        printf("  fatnt service disabled.\n");
    }
    SetServiceStart(SVC_FASTFAT, SERVICE_SYSTEM_START);

    /* Undo a replace install: restore the original Fastfat.sys */
    DriversPath("fastfat.sys", ff, sizeof(ff));
    DriversPath("fastfat.sav", sav, sizeof(sav));
    if (ff[0] && GetFileAttributesA(sav) != 0xFFFFFFFF) {
        if (CopyFileA(sav, ff, FALSE)) {
            printf("  restored the original Fastfat.sys from Fastfat.sav.\n");
        } else {
            printf("  warning: could not restore Fastfat.sys (error %lu).\n", GetLastError());
        }
    }
    printf("Done. Restart to apply.\n");
    return 0;
}

int main(int argc, char **argv)
{
    DWORD start = SERVICE_BOOT_START;
    int disableMode = 0;
    int disableFastfat = 1;
    int i;

    for (i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "/U") || !_stricmp(argv[i], "-U")) {
            return Uninstall();
        } else if (!_stricmp(argv[i], "/DISABLE")) {
            disableMode = 1;
        } else if (!_stricmp(argv[i], "/KEEPFAT")) {
            disableMode = 1;
            disableFastfat = 0;
        } else if (!_stricmp(argv[i], "/SYSTEM")) {
            disableMode = 1;
            start = SERVICE_SYSTEM_START;
            disableFastfat = 0;     /* a system-start driver never owns the boot volume */
        } else {
            printf("usage: fatinst [/DISABLE [/SYSTEM] [/KEEPFAT]] [/U]\n");
            return 2;
        }
    }

    printf("FATNT installer\n");

    if (disableMode) {
        return DisableInstall(start, disableFastfat);
    }
    return ReplaceInstall();
}
