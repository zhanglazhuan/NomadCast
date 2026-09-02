@echo off
set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.3"
set "IDF_PYTHON_ENV=C:\Espressif\python_env\idf5.5_py3.13_env"
set "PATH=%IDF_PYTHON_ENV%\Scripts;%IDF_PATH%\tools;%PATH%"
cd /d D:\Codes\NomadCast
echo === Building ===
python "%IDF_PATH%\tools\idf.py" build
if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED
    exit /b 1
)
echo === Flashing + Monitor ===
python "%IDF_PATH%\tools\idf.py" flash monitor
