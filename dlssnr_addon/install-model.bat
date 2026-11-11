@echo off
rem SPDX-FileCopyrightText: Copyright 2026 Robert Bopko
rem SPDX-License-Identifier: MIT
rem Drop your nvngx_dlssnr.dll (or a zip that contains it) on this file. See install-model.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-model.ps1" %*
pause
