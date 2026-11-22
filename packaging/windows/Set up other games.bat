@echo off
rem Finds your other Fallout games (Fallout 1, Sonora, Nevada, ...) for gecko's Game list, Browse
rem Maps by game and their stamps: it runs the map workflow's setup (workflow\setup.py) with the
rem Python that comes with gecko. Your games are only read; what it finds goes in workflow\config.json.
cd /d "%~dp0workflow" || goto :eof
set "PY=%~dp0python\python.exe"
if exist "%PY%" (set "PYTHONNOUSERSITE=1") else (set "PY=python")
"%PY%" setup.py
echo.
echo If gecko is open, close it and open it again to see the games you added.
pause
