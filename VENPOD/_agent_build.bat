@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul 2>&1
if errorlevel 1 (echo VSDEVCMD_FAILED & exit /b 2)
set "PATH=C:\Program Files\Ninja;%PATH%"
cd /d "z:\328\CMPUT328-A2\codexworks\301\3d\VENPOD\build"
ninja
exit /b %errorlevel%
