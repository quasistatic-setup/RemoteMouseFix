@echo off
setlocal
title RemoteMouseFix A/B correction test
"%~dp0RemoteMouseFix.exe" "%~dp0config\config-ab-test.json" %*
if errorlevel 1 (
    echo.
    echo RemoteMouseFix could not start. Review the error above.
    pause
)
