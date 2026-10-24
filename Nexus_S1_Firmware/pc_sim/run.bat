@echo off
setlocal
python "%~dp0build.py" --run %*
exit /b %errorlevel%
