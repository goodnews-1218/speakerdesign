@echo off
setlocal
cd /d "%~dp0" || exit /b 1

echo Starting Connect Studio...
echo Keep this window open. Press Ctrl+C to stop the server.
python serve.py --open %*
set "exit_code=%errorlevel%"
if not "%exit_code%"=="0" (
    echo Connect Studio stopped with an error. See the message above.
    pause
)
exit /b %exit_code%
