@echo off
powershell -ExecutionPolicy Bypass -File "%~dp0scripts\flash_deauth_detector.ps1" %*
