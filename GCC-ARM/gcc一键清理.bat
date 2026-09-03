@echo off
chcp 65001 >nul
title Clean GCC intermediates
rem 本 bat 位于 GCC-ARM/ 内，%~dp0 即 GCC-ARM/，直接定位同目录下的脚本
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\clean_intermediates.ps1"
pause