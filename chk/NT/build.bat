@echo off
REM fatchk.exe for NT 3.1, 3.5x, 4.0 and later. MSVCDIR = Visual C++ 4.x.
REM Subsystem 3.10 so NT 3.1 loads it. No DDK needed.
set MSVCDIR=C:\MSDEV
set PATH=%MSVCDIR%\BIN;%PATH%
set INCLUDE=%MSVCDIR%\INCLUDE
set LIB=%MSVCDIR%\LIB
if exist *.obj del *.obj
if exist fatchk.exe del fatchk.exe
cl -nologo -c -O2 -W3 -ML -DFAT_USERMODE -D_WIN32 -D_CONSOLE /I..\..\src ..\fatchk.c ..\..\src\fatsup.c
if errorlevel 1 goto error
link -nologo -subsystem:console,3.10 -out:fatchk.exe fatchk.obj fatsup.obj kernel32.lib
if errorlevel 1 goto error
echo fatchk.exe built.
goto end
:error
echo Build FAILED.
:end
