@echo off
REM ============================================================
REM  tools/web/open.bat -- open the 206Dash tool index in the default browser
REM
REM  Why this file is PURE ASCII (no Chinese text, no smart quotes):
REM    .bat files are decoded with the OEM/ANSI codepage (GBK on a Chinese
REM    Windows). A UTF-8 .bat with Chinese bytes gets mangled in unpredictable
REM    ways -- a stray byte can even terminate a command early. Keeping it
REM    ASCII-only removes that whole class of bug.
REM
REM  Why "start" and %~dp0:
REM    %~dp0 = the directory this .bat lives in (always ends with a backslash),
REM    so the page opens no matter what the current directory is. It resolves to
REM    a file:// URL, which is all these pages need -- they are plain static
REM    HTML with inline CSS and no network access, so no local server required.
REM
REM  Nothing is installed, downloaded or started: only the default browser.
REM ============================================================
start "" "%~dp0index.html"
