@echo off
powershell -ExecutionPolicy Bypass -File "%~dp0scripts\build_deauth_detector.ps1" %*
