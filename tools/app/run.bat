@echo off
REM ============================================================
REM  tools\app\run.bat -- start the 206Dash workbench (local server + app)
REM
REM  ASCII only on purpose: .bat files are decoded with the OEM codepage
REM  (GBK on a Chinese Windows); non-ASCII bytes get mangled in unpredictable
REM  ways, and this project's home directory has a non-ASCII name -- so nothing
REM  here may depend on %USERPROFILE% being spelled out.
REM
REM  What it does:
REM    1. finds the python that can run platformio (the project venv, which sits
REM       on an ASCII path on purpose); falls back to whatever "python" is,
REM    2. starts tools\app\server.py (listening on 127.0.0.1 only),
REM    3. the server itself opens the app.
REM  Close this window (or Ctrl+C) to stop the server.
REM
REM  Pass-through flags, e.g.:  run.bat --port 8899 --app-window
REM ============================================================
setlocal
set HERE=%~dp0

set PY=
if exist "C:\206dash-scratch\pio-core-mix\penv\Scripts\python.exe" set PY=C:\206dash-scratch\pio-core-mix\penv\Scripts\python.exe
if not defined PY if exist "C:\Users\Public\206dash\.pio-core\penv\Scripts\python.exe" set PY=C:\Users\Public\206dash\.pio-core\penv\Scripts\python.exe
if not defined PY set PY=python

echo Using python: %PY%
"%PY%" "%HERE%server.py" %*
echo.
echo Server stopped.
pause >nul
