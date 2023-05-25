# Lenovo Keyboard Backlight Fix

This is a tiny/simple Windows service that restores the keyboard backlight state when waking or rebooting your Lenovo laptop. Tested with Windows 10 on a Lenovo X1 Carbon 5th Gen (2017). Requires the Lenovo device driver software to be installed (I think it's called Lenovo Vantage).

## How it works

Installs a Windows service named "LenovoKeyboardBacklightFix" that will run in the background to detect changes to the keyboard backlight state. When the backlight state changes, the service stores the backlight state in the registry. When the system wakes or boots, the service restores the backlight state from the registry after a short delay.

## Dependency: WinRing0

The installation comes with a copy of WinRing0 binaries. This is necessary because the Windows service must access the underlying hardware which requires signed drivers to do so. The WinRing0 driver files are not compiled by github and their origin is unverified. Use this program at your own risk!

## Install

- Download latest release
- Extract archive
- Run "install.cmd" as administrator (in File Explorer, right click and choose "Run as administrator")

## Uninstall

- Run "uninstall.cmd" as administrator
