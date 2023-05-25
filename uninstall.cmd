@echo off

:: detect whether running as admin
:: https://stackoverflow.com/a/21295806
fsutil dirty query %systemdrive% >nul
if not %errorlevel% == 0 (
	echo Error: must run as administrator
	pause
	exit /b 1
)

:: note: `net stop` is used rather than `sc stop` because it should wait until the service is completely stopped
net stop LenovoKeyboardBacklightFix
sc delete LenovoKeyboardBacklightFix

:: note: stop service before deleting registry entries (they are in use by the program)
reg delete "HKLM\SOFTWARE\Lenovo\ShortcutKey\AppLaunch\Ex_11" /f
reg delete "HKLM\SOFTWARE\LenovoKeyboardBacklightFix" /f

echo LenovoKeyboardBacklightFix has been uninstalled
pause
exit


:fail
echo Failed with error #%errorlevel%
pause
exit /b %errorlevel%
