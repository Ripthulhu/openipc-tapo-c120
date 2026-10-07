@echo off
setlocal
python -c "import sys; sys.exit(sys.version_info < (3, 11))" >nul 2>&1
if errorlevel 1 (
    echo Python 3.11 or newer must be installed and available as python.
    exit /b 1
)
set "PYTHON=%~dp0.venv-c120-install\Scripts\python.exe"
if not exist "%PYTHON%" (
    python -m venv "%~dp0.venv-c120-install" || exit /b 1
)
"%PYTHON%" -m pip install --disable-pip-version-check -q -r "%~dp0requirements.txt" || exit /b 1
"%PYTHON%" "%~dp0c120_install.py" %*
exit /b %errorlevel%
