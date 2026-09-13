@echo off
setlocal
title RemoteMouseFix A/B correction test
"%~dp0RemoteMouseFix.exe" "%~dp0config-ab-test.json" %*
