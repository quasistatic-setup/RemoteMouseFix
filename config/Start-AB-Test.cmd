@echo off
setlocal
title RemoteMouseFix A/B correction test

rem Interactive target selection on a double-click; explicit CLI flags bypass the prompt.
if "%~1"=="" (
    "%~dp0RemoteMouseFix.exe" "%~dp0config\config-ab-test.json" --choose-target
) else (
    "%~dp0RemoteMouseFix.exe" "%~dp0config\config-ab-test.json" %*
)
if errorlevel 1 (
    echo.
    echo RemoteMouseFix could not start. Review the error above.
    pause
)
