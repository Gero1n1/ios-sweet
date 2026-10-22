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

"%FASTBOOT%" devices > "%TEMP%\ios-sweet-fastboot-devices.txt" 2>&1
if errorlevel 1 (
  echo ERROR: fastboot could not list devices. Check the USB cable and driver.
  type "%TEMP%\ios-sweet-fastboot-devices.txt"
  exit /b 1
)
powershell -NoProfile -Command "$d = @(Get-Content -LiteralPath $env:TEMP\ios-sweet-fastboot-devices.txt | Where-Object { $_ -match '\S+\s+fastboot' }); if ($d.Count -ne 1) { exit 1 }" >nul 2>&1
if errorlevel 1 (
  echo ERROR: Connect exactly one phone in bootloader fastboot mode.
  type "%TEMP%\ios-sweet-fastboot-devices.txt"
  exit /b 1
)

"%FASTBOOT%" getvar product > "%TEMP%\ios-sweet-fastboot-product.txt" 2>&1
powershell -NoProfile -Command "$x = Get-Content -LiteralPath $env:TEMP\ios-sweet-fastboot-product.txt -Raw; if ($x -notmatch '(?im)^\s*(?:\(bootloader\)\s*)?product:\s*(sweet|sweetin)\s*$') { exit 1 }" >nul 2>&1
if errorlevel 1 (
  echo ERROR: Device product is not exactly sweet or sweetin, or could not be read.
  type "%TEMP%\ios-sweet-fastboot-product.txt"
  exit /b 1
)

"%FASTBOOT%" getvar unlocked > "%TEMP%\ios-sweet-fastboot-unlocked.txt" 2>&1
powershell -NoProfile -Command "$x = Get-Content -LiteralPath $env:TEMP\ios-sweet-fastboot-unlocked.txt -Raw; if ($x -notmatch '(?im)^\s*(?:\(bootloader\)\s*)?unlocked:\s*yes\s*$') { exit 1 }" >nul 2>&1
if errorlevel 1 (
  echo ERROR: fastboot did not report an unlocked bootloader.
  type "%TEMP%\ios-sweet-fastboot-unlocked.txt"
  echo Unlock the bootloader using Xiaomi's official process before continuing.
  exit /b 1
)

echo Target model and unlocked bootloader confirmed.
echo Root image: "%ROOT_IMAGE%"
echo Boot image: "%BOOT_IMAGE%"
echo.
echo Back up your phone first. OrangeFox can back up the calibration and modem
echo partitions listed in README.md. Keep those backups on the PC, not on userdata.
echo This operation will now overwrite the phone's entire userdata partition.
echo Type ERASE userdata to continue, or press Ctrl+C to cancel.
set /p "CONFIRM=Confirmation: "
if /i not "%CONFIRM%"=="ERASE userdata" (
  echo Cancelled. No partition was changed.
  exit /b 1
)

echo.
echo Writing userdata. Keep the cable connected until fastboot finishes.
"%FASTBOOT%" -S 512M flash userdata "%ROOT_IMAGE%"
if errorlevel 1 (
  echo ERROR: userdata flash failed. Do not reboot to Android; return to fastboot or OrangeFox.
  exit /b 1
)

echo.
echo Starting the shim temporarily. No boot partition is being flashed.
"%FASTBOOT%" boot "%BOOT_IMAGE%"
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
