@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem /** Requests elevation because the installed game directory is protected. */
if /I not "%~1"=="--elevated" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "try { Start-Process -FilePath $env:ComSpec -ArgumentList @('/c', ([char]34 + '%~f0' + [char]34), '--elevated') -Verb RunAs -WindowStyle Normal -Wait -ErrorAction Stop; exit 0 } catch { exit 1 }"
    if errorlevel 1 (
        echo [BLAD] Nie zatwierdzono uprawnien Administratora.
        pause
        exit /b 1
    )
    exit /b 0
)

rem /** Refuses to remove an injected DLL while SimCity is still running. */
%SystemRoot%\System32\tasklist.exe /FI "IMAGENAME eq SimCity.exe" /NH 2>NUL | %SystemRoot%\System32\find.exe /I "SimCity.exe" >NUL
if not errorlevel 1 (
    echo [BLAD] Zamknij SimCity przed odinstalowaniem Simoder.
    pause
    exit /b 1
)

rem /** Refuses to remove optional DevTools while their MCP broker is active. */
%SystemRoot%\System32\tasklist.exe /FI "IMAGENAME eq Simoder.DevBridge.exe" /NH 2>NUL | %SystemRoot%\System32\find.exe /I "Simoder.DevBridge.exe" >NUL
if not errorlevel 1 (
    echo [BLAD] Zamknij Simoder.DevBridge przed odinstalowaniem Simoder.
    echo Jezeli MCP byl zarejestrowany, najpierw uruchom Unregister-Simoder-MCP.ps1.
    pause
    exit /b 1
)

rem /** Resolves exact installed targets relative to this script. */
set "SIMODER_RUNTIME=%~dp0"
for %%I in ("%SIMODER_RUNTIME%..") do set "SIMCITY_ROOT=%%~fI"

rem /** Removes runtime code and generated diagnostics while preserving user content. */
del /Q "%SIMODER_RUNTIME%simoder.exe" 2>NUL
del /Q "%SIMODER_RUNTIME%sc13modloader.dll" 2>NUL
if exist "%SIMODER_RUNTIME%logs" rmdir /S /Q "%SIMODER_RUNTIME%logs"
if exist "%SIMODER_RUNTIME%cache" rmdir /S /Q "%SIMODER_RUNTIME%cache"
if exist "%SIMODER_RUNTIME%DevTools" rmdir /S /Q "%SIMODER_RUNTIME%DevTools"
if exist "%SIMODER_RUNTIME%licenses" rmdir /S /Q "%SIMODER_RUNTIME%licenses"
del /Q "%SIMCITY_ROOT%\Start-Simoder.bat" 2>NUL
del /Q "%SIMCITY_ROOT%\Start-Simoder.sh" 2>NUL

echo Simoder runtime zostal usuniety.
echo Zachowano mods, config.toon oraz state.toon.
echo Ten uninstaller usunie teraz sam siebie.
pause

rem /** Deletes this final script after cmd has parsed the current command block. */
start "" /B cmd.exe /D /C "ping 127.0.0.1 -n 2 ^>NUL ^& del /Q ^"%~f0^""
exit /b 0
