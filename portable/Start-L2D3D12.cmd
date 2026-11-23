@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-L2D3D12.ps1" %*
set "L2D3D12_EXIT=%ERRORLEVEL%"
echo.
if not "%L2D3D12_EXIT%"=="0" echo La operacion no se completo. Revisa el error y el reporte mostrado arriba.
pause
exit /b %L2D3D12_EXIT%
