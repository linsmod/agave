@echo off
setlocal
cd /d "%~dp0"

rem usage: build_windows.bat [Debug/Release/MinSizeRel/RelWithDebInfo]
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Debug

rem /Wv:18 keeps VS2022 17.14+ new warnings (C5286/C5287) from tripping the /WX in build/configs.cmake.
rem With the VS2019 (v142) toolset these two switches can be dropped.
cmake -S . -B vcproj -A Win32 -DOPT_EXAMPLES=ON -DCMAKE_C_FLAGS="/Wv:18" -DCMAKE_CXX_FLAGS="/Wv:18"
if errorlevel 1 goto fail

cmake --build vcproj --config %CONFIG% --parallel
if errorlevel 1 goto fail

echo.
echo OK --^> vcproj\%CONFIG%\
dir /b "vcproj\%CONFIG%\*.exe" "vcproj\%CONFIG%\*.dll"
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
