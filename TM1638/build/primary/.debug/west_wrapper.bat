@echo off
REM Wrapper script to run west commands out of Zephyr workbench environment
REM This script is auto-generated -- do not edit

REM Set environment variables
set ZEPHYR_BASE=c:\Users\razie\OneDrive\Documentos\PowerShell\STM32\deps\zephyr
set ZEPHYR_PROJECT_DIRECTORY=c:\Users\razie\OneDrive\Documentos\PowerShell\STM32
set ZEPHYR_TOOLCHAIN_VARIANT=zephyr


REM Source environment and execute West
call C:\Users\razie\.zinstaller\env.bat && west %*
