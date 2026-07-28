@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /D_CRT_SECURE_NO_WARNINGS main.c /Fe:ModSorter.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib ole32.lib shell32.lib dwmapi.lib winhttp.lib bcrypt.lib uxtheme.lib comctl32.lib uuid.lib gdiplus.lib shlwapi.lib
