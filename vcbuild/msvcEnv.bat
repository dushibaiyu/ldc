@echo off

:: Environment already set up?
if not "%VSINSTALLDIR%"=="" goto :eof

:: Skip detection if an existing LDC_VSDIR environment variable points to an existing folder
if "%LDC_VSDIR%"=="" goto detect
if not "%LDC_VSDIR:~-1%"=="\" set LDC_VSDIR=%LDC_VSDIR%\
if exist "%LDC_VSDIR%" goto setup

:: Try to detect the latest VS installation directory
:detect
set LDC_VSDIR=
:: VC++ 2013
for /F "tokens=1,2*" %%i in ('reg query HKLM\SOFTWARE\Microsoft\VisualStudio\12.0\Setup\VC /v ProductDir 2^> nul') do set LDC_VSDIR=%%k
for /F "tokens=1,2*" %%i in ('reg query HKLM\SOFTWARE\WOW6432Node\Microsoft\VisualStudio\12.0\Setup\VC /v ProductDir 2^> nul') do set LDC_VSDIR=%%k
:: VC++ 2015
for /F "tokens=1,2*" %%i in ('reg query HKLM\SOFTWARE\Microsoft\VisualStudio\14.0\Setup\VC /v ProductDir 2^> nul') do set LDC_VSDIR=%%k
for /F "tokens=1,2*" %%i in ('reg query HKLM\SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\Setup\VC /v ProductDir 2^> nul') do set LDC_VSDIR=%%k
:: (remove 'VC\' suffix)
if not "%LDC_VSDIR%"=="" set LDC_VSDIR=%LDC_VSDIR:~0,-3%
:: VS Build Tools 2017 (default installation path)
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2017\BuildTools\" set LDC_VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\2017\BuildTools\
if exist "%ProgramFiles%\Microsoft Visual Studio\2017\BuildTools\" set LDC_VSDIR=%ProgramFiles%\Microsoft Visual Studio\2017\BuildTools\
:: VS 2017
for /F "tokens=1,2*" %%i in ('reg query HKLM\SOFTWARE\Microsoft\VisualStudio\SxS\VS7 /v 15.0 2^> nul') do (
    if exist "%%kVC\Auxiliary\Build\vcvarsall.bat" set LDC_VSDIR=%%k
)
for /F "tokens=1,2*" %%i in ('reg query HKLM\SOFTWARE\WOW6432Node\Microsoft\VisualStudio\SxS\VS7 /v 15.0 2^> nul') do (
    if exist "%%kVC\Auxiliary\Build\vcvarsall.bat" set LDC_VSDIR=%%k
)

if "%LDC_VSDIR%"=="" (
    echo WARNING: no Visual C++ installation detected
    goto :eof
)

:: Let MSVC set up environment variables
:setup
echo Using Visual Studio: %LDC_VSDIR:~0,-1%
:: VC++ 2013/2015
if exist "%LDC_VSDIR%VC\vcvarsall.bat" (
    call "%LDC_VSDIR%VC\vcvarsall.bat" %1
    goto :eof
)
:: VC++ 2017
if exist "%LDC_VSDIR%Common7\Tools\vsdevcmd.bat" (
    call "%LDC_VSDIR%Common7\Tools\vsdevcmd.bat" -arch=%1 -no_logo
    goto :eof
)
echo WARNING: could not find Visual C++ batch file
