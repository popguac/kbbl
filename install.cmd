@echo off

:: detect whether running as admin
:: https://stackoverflow.com/a/21295806
fsutil dirty query %systemdrive% >nul
if not %errorlevel% == 0 (
	echo Error: must run as administrator
	pause
	exit /b 1
)

reg add "HKLM\SOFTWARE\LenovoKeyboardBacklightFix"

reg add "HKLM\SOFTWARE\Lenovo\ShortcutKey\AppLaunch\Ex_11" || goto fail
reg add "HKLM\SOFTWARE\Lenovo\ShortcutKey\AppLaunch\Ex_11" /v AppType /t REG_DWORD /d 1 || goto fail
reg add "HKLM\SOFTWARE\Lenovo\ShortcutKey\AppLaunch\Ex_11\Desktop" || goto fail
reg add "HKLM\SOFTWARE\Lenovo\ShortcutKey\AppLaunch\Ex_11\Desktop" /v File /t REG_SZ /d "%~dp0kbbl.exe" || goto fail
reg add "HKLM\SOFTWARE\Lenovo\ShortcutKey\AppLaunch\Ex_11\Desktop" /v Parameters /t REG_SZ /d "--toggled" || goto fail

sc create LenovoKeyboardBacklightFix start= auto binPath= "%~dp0kbbl.exe --svc" || goto fail
:: TODO: wait until service finishes starting? to see if there is an error - then can print an error to user
sc start LenovoKeyboardBacklightFix || goto fail

echo LenovoKeyboardBacklightFix service has been installed and started
pause
exit


:fail
echo Failed with error #%errorlevel%
pause
exit /b %errorlevel%
