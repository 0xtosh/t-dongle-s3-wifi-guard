@echo off
powershell -ExecutionPolicy Bypass -File "%~dp0scripts\package_deauth_detector.ps1" %*
