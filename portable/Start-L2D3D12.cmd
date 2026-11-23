@echo off
setlocal
cd /d "%~dp0"

fltmc.exe >nul 2>&1
if errorlevel 1 (
    set "L2D3D12_SELF=%~f0"
    echo Solicitando permisos de administrador...
    powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath $env:L2D3D12_SELF -Verb RunAs"
    exit /b %ERRORLEVEL%
)

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-L2D3D12.ps1" -ChooseExecutable %*
set "L2D3D12_EXIT=%ERRORLEVEL%"
echo.
if not "%L2D3D12_EXIT%"=="0" echo La operacion no se completo. Revisa el error y el reporte mostrado arriba.
pause
exit /b %L2D3D12_EXIT%
