@echo off
REM fatinst.exe for NT 3.1, 3.5x, 4.0 and later. MSVCDIR = Visual C++ 4.x.
REM -ML links the static C runtime so there is no msvcrt/crtdll dependency.
set MSVCDIR=C:\MSDEV
set PATH=%MSVCDIR%\BIN;%PATH%
set INCLUDE=%MSVCDIR%\INCLUDE
set LIB=%MSVCDIR%\LIB
if exist *.obj del *.obj
if exist fatinst.exe del fatinst.exe
cl -nologo -c -O2 -W3 -ML -D_WIN32 -D_CONSOLE ..\fatinst.c
if errorlevel 1 goto error
link -nologo -subsystem:console,3.10 -out:fatinst.exe fatinst.obj kernel32.lib advapi32.lib
if errorlevel 1 goto error
echo fatinst.exe built.
goto end
:error
echo Build FAILED.
:end
