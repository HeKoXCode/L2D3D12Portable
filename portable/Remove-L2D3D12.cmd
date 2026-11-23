@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-L2D3D12.ps1" -Remove %*
set "L2D3D12_EXIT=%ERRORLEVEL%"
echo.
if not "%L2D3D12_EXIT%"=="0" echo No se elimino ningun archivo sin verificar. Revisa el error mostrado arriba.
pause
exit /b %L2D3D12_EXIT%
