@echo off
chcp 65001 >nul
echo ========================================
echo  Building SXCL with Nuitka (onefile)
echo ========================================

python -m nuitka ^
    --onefile ^
    --enable-plugin=pyside6 ^
    --windows-console-mode=disable ^
    --include-data-dir=config=config ^
    --output-dir=dist ^
    --product-name="Silent X Craft Launcher" ^
    --file-version="1.0.0" ^
    --file-description="Silent X Craft Launcher" ^
    --copyright="© SilentStudio / SilentCodeTeams" ^
    main.py

echo.
if %ERRORLEVEL% EQU 0 (
    echo ✅ Build successful! dist/main.exe
) else (
    echo ❌ Build failed with code %ERRORLEVEL%
)
pause
