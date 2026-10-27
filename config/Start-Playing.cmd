@echo off
setlocal
title RemoteMouseFix gameplay
"%~dp0RemoteMouseFix.exe" "%~dp0config\config-play.json" %*
if errorlevel 1 (
    echo.
    echo RemoteMouseFix could not start. Review the error above.
    pause
)
