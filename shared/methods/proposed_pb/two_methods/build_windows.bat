@echo off
setlocal DisableDelayedExpansion
pushd "%~dp0" || exit /b 1
set "result=1"

if "%~1"=="" goto :cmake_build
if /i "%~1"=="--msvc" if "%~2"=="" goto :msvc_build
echo Usage: build_windows.bat [--msvc]
goto :done

:cmake_build
where cmake >nul 2>&1
if errorlevel 1 (
    echo CMake was not found. Open the x64 Native Tools Command Prompt for Visual Studio.
    goto :failed
)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
set "result=%errorlevel%"
if not "%result%"=="0" goto :failed
cmake --build build --config Release --parallel
set "result=%errorlevel%"
if not "%result%"=="0" goto :failed
goto :built

:msvc_build
where cl >nul 2>&1
if errorlevel 1 (
    echo MSVC was not found. Open the x64 Native Tools Command Prompt for Visual Studio.
    goto :failed
)
if not exist "build\Release\." mkdir "build\Release"
pushd "build\Release" || goto :failed
cl /nologo /std:c++17 /EHsc /O2 /DNDEBUG /utf-8 /W4 /I"..\..\include" /Ferun_all_scales.exe ^
    "..\..\src\main.cpp" "..\..\src\input.cpp" "..\..\src\loading.cpp" ^
    "..\..\src\proposed.cpp" "..\..\src\sar.cpp" "..\..\src\report.cpp"
set "result=%errorlevel%"
popd
if not "%result%"=="0" goto :failed

:built
echo Build complete. Run run_all_scales.bat to start.
goto :done

:failed
echo Build failed. See the error above.

:done
popd
if "%~1"=="" pause
exit /b %result%
