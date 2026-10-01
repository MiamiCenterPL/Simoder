@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem /** Resolves the installed Simoder runtime relative to the game root. */
set "SIMODER_ROOT=%~dp0"
set "SIMODER_RUNTIME=%SIMODER_ROOT%simoder"
set "SIMODER_EXE=%SIMODER_RUNTIME%\simoder.exe"
set "SIMODER_DLL=%SIMODER_RUNTIME%\sc13modloader.dll"
set "SIMCITY_URI=origin2://game/launch/?offerIds=71654,71480,71630,71650,71573,71631,71652,71572,71632,1004769,1004768,1004771,1004770,1008749,1008760,1008761,1008762,1008763,1008764,1008776,1008777,1008778,1008779,1015233,1015232,1015226&title=SimCity%%u2122%%u003a%%u0020Complete%%u0020Edition&cmdParams="

rem /** Fails before UAC when the installed runtime is incomplete. */
if not exist "%SIMODER_EXE%" (
    echo [BLAD] Brak "%SIMODER_EXE%".
    echo Zainstaluj Simoder ponownie w glownym katalogu SimCity.
    pause
    exit /b 1
)
if not exist "%SIMODER_DLL%" (
    echo [BLAD] Brak "%SIMODER_DLL%".
    echo Zainstaluj Simoder ponownie w glownym katalogu SimCity.
    pause
    exit /b 1
)

rem /** Provides a non-mutating installation check. */
if /I "%~1"=="--check" goto check_only

rem /** Starts one elevated watcher because the current EA-launched game requires elevation. */
%SystemRoot%\System32\tasklist.exe /FI "IMAGENAME eq simoder.exe" /NH 2>NUL | %SystemRoot%\System32\find.exe /I "simoder.exe" >NUL
if errorlevel 1 (
    echo [Simoder] Wymagane jest zatwierdzenie UAC dla Watchera.
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "try { $watcherArguments = @('--watch-attach', '4294967295', ([char]34 + $env:SIMODER_DLL + [char]34)); Start-Process -FilePath $env:SIMODER_EXE -ArgumentList $watcherArguments -Verb RunAs -WindowStyle Hidden -ErrorAction Stop | Out-Null; exit 0 } catch { Write-Error $_; exit 1 }"
    if errorlevel 1 (
        echo [BLAD] Nie udalo sie uruchomic podniesionego Watchera.
        pause
        exit /b 1
    )
) else (
    echo [Simoder] Watcher juz dziala.
)

rem /** Delegates the elevated game launch to the installed EA App. */
echo [Simoder] Uruchamianie SimCity przez EA App...
start "" "%SIMCITY_URI%"
if errorlevel 1 (
    echo [BLAD] EA App nie przyjela polecenia uruchomienia gry.
    pause
    exit /b 1
)
exit /b 0

:check_only
echo [OK] Simoder: "%SIMODER_EXE%"
echo [OK] DLL:     "%SIMODER_DLL%"
echo [OK] Mods:    "%SIMODER_ROOT%mods"
echo [OK] Config:  "%SIMODER_RUNTIME%\config.toon"
exit /b 0
