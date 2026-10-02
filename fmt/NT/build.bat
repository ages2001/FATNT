@echo off
REM fatfmt.exe for NT 3.1, 3.5x, 4.0 and later. MSVCDIR = Visual C++ 4.x.
REM Only the compiler and Win32 headers are needed, not the DDK.
REM Subsystem 3.10 so NT 3.1 loads it.
set MSVCDIR=C:\MSDEV
set PATH=%MSVCDIR%\BIN;%PATH%
set INCLUDE=%MSVCDIR%\INCLUDE
set LIB=%MSVCDIR%\LIB
if exist *.obj del *.obj
if exist fatfmt.exe del fatfmt.exe
cl -nologo -c -O2 -W3 -ML -D_WIN32 -D_CONSOLE /I.. ..\fatfmt.c
if errorlevel 1 goto error
link -nologo -subsystem:console,3.10 -out:fatfmt.exe fatfmt.obj kernel32.lib
if errorlevel 1 goto error
echo fatfmt.exe built.
goto end
:error
echo Build FAILED.
:end
