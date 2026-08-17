@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem /** Resolves every project artifact relative to this batch file. */
set "SIMODER_ROOT=%~dp0"
set "SIMODER_RELEASE=%SIMODER_ROOT%build\vs2026\bin\Release"
set "SIMODER_WATCHER=%SIMODER_RELEASE%\sc13-launcher.exe"
set "SIMODER_DLL=%SIMODER_RELEASE%\sc13modloader.dll"
set "SIMCITY_URI=origin2://game/launch/?offerIds=71654,71480,71630,71650,71573,71631,71652,71572,71632,1004769,1004768,1004771,1004770,1008749,1008760,1008761,1008762,1008763,1008764,1008776,1008777,1008778,1008779,1015233,1015232,1015226&title=SimCity%%u2122%%u003a%%u0020Complete%%u0020Edition&cmdParams="

rem /** Fails early when the verified Release artifacts are unavailable. */
if not exist "%SIMODER_WATCHER%" (
    echo [BLAD] Nie znaleziono Watchera:
    echo        "%SIMODER_WATCHER%"
    echo.
    echo Zbuduj preset vs2026-release i uruchom ten plik ponownie.
    pause
    exit /b 1
)

if not exist "%SIMODER_DLL%" (
    echo [BLAD] Nie znaleziono biblioteki moda:
    echo        "%SIMODER_DLL%"
    echo.
    echo Zbuduj preset vs2026-release i uruchom ten plik ponownie.
    pause
    exit /b 1
)

rem /** Provides a non-mutating validation mode used by local checks. */
if /I "%~1"=="--check" goto check_only

rem /** Reuses an active watcher so repeated clicks never create competing injectors. */
%SystemRoot%\System32\tasklist.exe /FI "IMAGENAME eq sc13-launcher.exe" /NH 2>NUL | %SystemRoot%\System32\find.exe /I "sc13-launcher.exe" >NUL
if errorlevel 1 (
    echo [Simoder] Uruchamianie Watchera jako administrator...
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "try { $watcherArguments = @('--watch-attach', '4294967295', ([char]34 + $env:SIMODER_DLL + [char]34)); Start-Process -FilePath $env:SIMODER_WATCHER -ArgumentList $watcherArguments -Verb RunAs -WindowStyle Minimized -ErrorAction Stop | Out-Null; exit 0 } catch { Write-Error $_; exit 1 }"
    if errorlevel 1 (
        echo [BLAD] Nie udalo sie uruchomic Watchera. Uruchamianie gry anulowane.
        pause
        exit /b 1
    )
) else (
    echo [Simoder] Watcher juz dziala - wykorzystuje istniejacy proces.
)

rem /** Delegates launch to the EA App using the exact Complete Edition URI recorded locally. */
echo [Simoder] Uruchamianie SimCity przez EA App...
start "" "%SIMCITY_URI%"
if errorlevel 1 (
    echo [BLAD] EA App nie przyjela polecenia uruchomienia gry.
    pause
    exit /b 1
)

echo [Simoder] Gotowe. Watcher przejmie nowy proces SimCity automatycznie.
exit /b 0

:check_only
echo [OK] Watcher: "%SIMODER_WATCHER%"
echo [OK] DLL:     "%SIMODER_DLL%"
echo [OK] URI EA:  "%SIMCITY_URI%"
exit /b 0
