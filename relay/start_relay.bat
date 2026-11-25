@echo off
rem Lance le relais GFN 3DS. A la premiere execution, installe les dependances dans .venv
rem vgamepad automatise l'installation du pilote ViGEmBus : accepte la demande Windows si elle apparait.
rem Arguments utiles : --check, --list-windows, --source testpattern ou --source video:fichier.mp4
setlocal
cd /d "%~dp0"

if not exist ".venv\Scripts\python.exe" (
    echo Premiere installation : creation de l'environnement Python...
    py -3 -m venv .venv
    if errorlevel 1 (
        echo Python 3.10 ou plus recent introuvable. Installe-le depuis python.org puis relance.
        pause
        exit /b 1
    )
    ".venv\Scripts\python.exe" -m pip install --upgrade pip
    ".venv\Scripts\python.exe" -m pip install -r requirements.txt
    if errorlevel 1 (
        echo L'installation des dependances a echoue.
        pause
        exit /b 1
    )
)

".venv\Scripts\python.exe" -m gfn3ds_relay %*
pause
