@echo off
"%~dp0bin\simpletriangle_shim_test.exe" --window %*
if errorlevel 1 pause
