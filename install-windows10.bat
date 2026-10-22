@echo off
setlocal EnableExtensions
title ios-sweet installer for Windows 10

rem Place this file beside ios16-root.simg and shim-boot.img.
rem The userdata flash below erases all Android user data.

set "ROOT_IMAGE=%~dp0ios16-root.simg"
set "BOOT_IMAGE=%~dp0shim-boot.img"
set "FASTBOOT="

echo.
echo ios-sweet Windows 10 installer
echo =============================
echo This is an experimental iOS 16 bring-up, not a daily-use phone OS.
echo Flashing userdata erases all data on the phone, including Android data.
echo It does not flash boot, recovery, modem, persist, or other partitions.
echo.

if not exist "%ROOT_IMAGE%" (
  echo ERROR: Missing "%ROOT_IMAGE%".
  echo Copy the generated ios16-root.simg beside this script.
  exit /b 1
)
if not exist "%BOOT_IMAGE%" (
  echo ERROR: Missing "%BOOT_IMAGE%".
  echo Copy the generated shim boot image here and name it shim-boot.img.
  exit /b 1
)

for %%F in (fastboot.exe) do set "FASTBOOT=%%~$PATH:F"
if not defined FASTBOOT if exist "%~dp0platform-tools\fastboot.exe" set "FASTBOOT=%~dp0platform-tools\fastboot.exe"
if not defined FASTBOOT (
  echo ERROR: fastboot.exe was not found in PATH or .\platform-tools.
  echo Install Android Platform-Tools or place platform-tools beside this script.
  exit /b 1
)

set "SESSION_DIR=%TEMP%\ios-sweet-%RANDOM%-%RANDOM%"
if exist "%SESSION_DIR%" (
  echo ERROR: Temporary log directory already exists. Run the installer again.
  exit /b 1
)
mkdir "%SESSION_DIR%" >nul 2>&1
if errorlevel 1 (
  echo ERROR: Cannot create the temporary log directory.
  exit /b 1
)

"%FASTBOOT%" devices > "%SESSION_DIR%\devices.txt" 2>&1
if errorlevel 1 (
  echo ERROR: fastboot could not list devices. Check the USB cable and driver.
  type "%SESSION_DIR%\devices.txt"
  exit /b 1
)
powershell -NoProfile -Command "$d = @(Get-Content -LiteralPath (Join-Path $env:SESSION_DIR 'devices.txt') | Where-Object { $_ -match '\S+\s+fastboot(?:\s|$)' }); if ($d.Count -ne 1 -or $d[0] -notmatch '^\s*([A-Za-z0-9_-]+)\s+fastboot\s*$') { exit 1 }; Set-Content -LiteralPath (Join-Path $env:SESSION_DIR 'target.txt') -Value $Matches[1] -Encoding Ascii" >nul 2>&1
if errorlevel 1 (
  echo ERROR: Connect exactly one phone in bootloader fastboot mode.
  type "%SESSION_DIR%\devices.txt"
  exit /b 1
)

set "TARGET_SERIAL="
set /p "TARGET_SERIAL="< "%SESSION_DIR%\target.txt"
if not defined TARGET_SERIAL exit /b 1
call :CHECK_TARGET
if errorlevel 1 exit /b 1

echo Target model and unlocked bootloader confirmed.
echo Target serial: %TARGET_SERIAL%
echo Diagnostic logs: "%SESSION_DIR%"
echo Root image: "%ROOT_IMAGE%"
echo Boot image: "%BOOT_IMAGE%"
echo.
echo Back up your phone first. OrangeFox can back up the calibration and modem
echo partitions listed in README.md. Keep those backups on the PC, not on userdata.
echo This operation will now overwrite the phone's entire userdata partition.
echo Type ERASE userdata to continue, or press Ctrl+C to cancel.
set "CONFIRM="
set /p "CONFIRM=Confirmation: "
setlocal EnableDelayedExpansion
if not "!CONFIRM!"=="ERASE userdata" (
  echo Cancelled. No partition was changed.
  exit /b 1
)
endlocal

rem Recheck the same serial after the user has finished reading the prompt.
call :CHECK_TARGET
if errorlevel 1 exit /b 1

echo.
echo Writing userdata. Keep the cable connected until fastboot finishes.
"%FASTBOOT%" -s "%TARGET_SERIAL%" -S 512M flash userdata "%ROOT_IMAGE%"
if errorlevel 1 (
  echo ERROR: userdata flash failed. Do not reboot to Android; return to fastboot or OrangeFox.
  exit /b 1
)

echo.
echo Starting the shim temporarily. No boot partition is being flashed.
"%FASTBOOT%" -s "%TARGET_SERIAL%" boot "%BOOT_IMAGE%"
if errorlevel 1 (
  echo ERROR: fastboot boot failed. The root image was written to userdata.
  echo You can still boot OrangeFox or return to the bootloader.
  exit /b 1
)

echo.
echo fastboot accepted the temporary boot image.
echo This confirms transfer only; it does not confirm that iOS finished booting.
echo To return to the bootloader, hold Power and Volume Down.
exit /b 0

:CHECK_TARGET
"%FASTBOOT%" -s "%TARGET_SERIAL%" getvar product > "%SESSION_DIR%\product.txt" 2>&1
if errorlevel 1 (
  echo ERROR: Cannot read the selected phone's product.
  type "%SESSION_DIR%\product.txt"
  exit /b 1
)
powershell -NoProfile -Command "$x = Get-Content -LiteralPath (Join-Path $env:SESSION_DIR 'product.txt') -Raw; if ($x -notmatch '(?im)^\s*(?:\(bootloader\)\s*)?product:\s*(sweet|sweetin)\s*$') { exit 1 }" >nul 2>&1
if errorlevel 1 (
  echo ERROR: Selected phone is not sweet or sweetin.
  type "%SESSION_DIR%\product.txt"
  exit /b 1
)
"%FASTBOOT%" -s "%TARGET_SERIAL%" getvar unlocked > "%SESSION_DIR%\unlocked.txt" 2>&1
if errorlevel 1 (
  echo ERROR: Cannot read the selected phone's unlock state.
  exit /b 1
)
powershell -NoProfile -Command "$x = Get-Content -LiteralPath (Join-Path $env:SESSION_DIR 'unlocked.txt') -Raw; if ($x -notmatch '(?im)^\s*(?:\(bootloader\)\s*)?unlocked:\s*yes\s*$') { exit 1 }" >nul 2>&1
if errorlevel 1 (
  echo ERROR: Selected phone did not report an unlocked bootloader.
  type "%SESSION_DIR%\unlocked.txt"
  exit /b 1
)
"%FASTBOOT%" -s "%TARGET_SERIAL%" getvar is-userspace > "%SESSION_DIR%\mode.txt" 2>&1
if errorlevel 1 (
  echo ERROR: Cannot verify bootloader fastboot mode.
  exit /b 1
)
powershell -NoProfile -Command "$x = Get-Content -LiteralPath (Join-Path $env:SESSION_DIR 'mode.txt') -Raw; if ($x -notmatch '(?im)^\s*(?:\(bootloader\)\s*)?is-userspace:\s*no\s*$') { exit 1 }" >nul 2>&1
if errorlevel 1 (
  echo ERROR: Use bootloader fastboot, not recovery fastbootd.
  type "%SESSION_DIR%\mode.txt"
  exit /b 1
)
exit /b 0
