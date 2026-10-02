@echo off
REM fatnt.sys for NT 3.1, 3.5, 3.51 and 4.0. MSVCDIR = Visual C++ 4.x,
REM DDKDIR = NT4 DDK.
REM The NT4 DDK has no ntifs.h: the free one (release 58, Bo Branten,
REM http://www.acc.umu.se/~bosse/) is in this folder.
REM libcntpr.lib comes before ntoskrnl.lib so the 64-bit arithmetic helpers
REM (_allmul, _aulldiv, ...) are linked in: NT 3.x does not export them.
REM
REM Subsystem version 3.10 and -base 0x10000 so the NT 3.1 loader accepts it.
REM FAT_NT31 selects the universal profile: the executive-resource API is bound
REM to the running kernel at load time (fatres.c), so one binary loads on NT 3.1
REM (legacy resources) through 4.0 (Lite). -release writes the PE checksum the
REM NT 3.51+ loader validates.

set MSVCDIR=C:\MSDEV
set DDKDIR=C:\NT4DDK

if not exist ntifs.h echo ntifs.h not found in this folder -- see above.
if not exist ntifs.h goto error

set PATH=%MSVCDIR%\BIN;%DDKDIR%\BIN;%PATH%
set INCLUDE=.;%DDKDIR%\inc;%DDKDIR%\inc\crt;%MSVCDIR%\INCLUDE
set LIB=%DDKDIR%\lib\i386;%MSVCDIR%\LIB;%DDKDIR%\lib\i386\free;%LIB%

if exist *.obj del *.obj
if exist fatnt.sys del fatnt.sys

cl -nologo -c -Gz -Ox -W3 -Zp8 -Zi -Zl -DFAT_NT4 -DFAT_NT31 -D_X86_=1 -Di386=1 -DSTD_CALL -DCONDITION_HANDLING=1 -DNT_UP=0 -DNT_INST=0 -DWIN32=100 -D_NT1X_=100 -DWINNT=1 -D_WIN32_WINNT=0x0400 -DDEVL=1 -DFPO=1 /I. /I.. ..\fatalloc.c ..\fatclose.c ..\fatcreat.c ..\fatdir.c ..\fatdirw.c ..\fatdisp.c ..\fatfast.c ..\fatflush.c ..\fatfsctl.c ..\fatinfo.c ..\fatinit.c ..\fatio.c ..\fatmisc.c ..\fatread.c ..\fatres.c ..\fatsetin.c ..\fatstruc.c ..\fatsup.c ..\fatvol.c ..\fatwrite.c
if errorlevel 1 goto error

link -nologo -release -debug -debugtype:both -subsystem:native,3.10 -entry:DriverEntry@8 -driver -base:0x10000 -align:0x200 -nodefaultlib -out:fatnt.sys fatalloc.obj fatclose.obj fatcreat.obj fatdir.obj fatdirw.obj fatdisp.obj fatfast.obj fatflush.obj fatfsctl.obj fatinfo.obj fatinit.obj fatio.obj fatmisc.obj fatread.obj fatres.obj fatsetin.obj fatstruc.obj fatsup.obj fatvol.obj fatwrite.obj libcntpr.lib ntoskrnl.lib hal.lib
if errorlevel 1 goto error

echo fatnt.sys built.
goto end

:error
echo Build FAILED.

:end
