# build-windows.ps1 - one-shot local build for Windows (MSVC)
# Mirrors the functionality of build.sh / build-linux.sh.
#
# Usage:
#   .\build-windows.ps1                      configure + build (Release; tests off)
#   .\build-windows.ps1 --tests              also run ctest after the build
#   .\build-windows.ps1 --clean              remove build dir first - from-scratch build
#   .\build-windows.ps1 --debug              Debug preset (with debug info)
#   .\build-windows.ps1 --no-loot            -DGMM_WITH_LOOT=OFF
#   .\build-windows.ps1 --update-submodules  git submodule update --init --recursive
#   .\build-windows.ps1 --skip-check         skip the dependency-presence check
#   .\build-windows.ps1 --help               show this help

$ErrorActionPreference = "Stop"

# --- Resolve script directory and ensure we are in the repo root ---
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

if (-not (Test-Path "CMakeLists.txt")) {
    Write-Host "Error: no CMakeLists.txt in $ScriptDir - run this from the Core repo root." -ForegroundColor Red
    exit 1
}

# --- Parse command-line arguments ---
$RunTests     = $false
$CleanFirst   = $false
$BuildType    = "Release"
$NoLoot       = $false
$UpdateSubmodules = $false
$SkipCheck    = $false

foreach ($arg in $args) {
    switch ($arg) {
        "--clean"             { $CleanFirst = $true }
        "--debug"             { $BuildType = "Debug" }
        "--tests"             { $RunTests = $true }
        "--no-loot"           { $NoLoot = $true }
        "--update-submodules" { $UpdateSubmodules = $true }
        "--skip-check"        { $SkipCheck = $true }
        "--help"              { Get-Content -Path $MyInvocation.MyCommand.Path | Select-String "^# " | ForEach-Object { $_.Line.Substring(2) }; exit 0 }
        "-h"                  { Get-Content -Path $MyInvocation.MyCommand.Path | Select-String "^# " | ForEach-Object { $_.Line.Substring(2) }; exit 0 }
        default {
            Write-Host "Unknown option: $arg (see .\build-windows.ps1 --help)" -ForegroundColor Red
            exit 1
        }
    }
}

# --- Dependency check ---
function Test-CommandExists {
    param([string]$Command)
    $null -ne (Get-Command $Command -ErrorAction SilentlyContinue)
}

function Invoke-DependencyCheck {
    Write-Host "==> Checking Windows build dependencies..."
    $missing = @()

    foreach ($tool in @("cmake", "ninja", "git")) {
        if (-not (Test-CommandExists $tool)) {
            $missing += $tool
        }
    }

    # Python - accept python or python3
    if (-not (Test-CommandExists "python") -and -not (Test-CommandExists "python3")) {
        $missing += "python (or python3)"
    }

    # MSVC compiler
    if (-not (Test-CommandExists "cl")) {
        $missing += "cl (MSVC compiler - run vcvars64.bat or open Developer Command Prompt)"
    }

    # Qt6 - check qmake or known CMake config locations
    $qt6Found = $false
    if (Test-CommandExists "qmake") {
        $qt6Found = $true
    }
    if (-not $qt6Found) {
        if (Test-Path "C:\vcpkg\installed\x64-windows\share\Qt6\Qt6Config.cmake") {
            $qt6Found = $true
        }
    }
    if (-not $qt6Found) {
        if (Test-Path "C:\Qt") {
            $qtDirs = Get-ChildItem -Path "C:\Qt" -Directory -Filter "6.*" -ErrorAction SilentlyContinue
            foreach ($d in $qtDirs) {
                $msvcDir = Get-ChildItem -Path $d.FullName -Directory -Filter "msvc2022_64" -ErrorAction SilentlyContinue
                if ($msvcDir) {
                    $qt6Found = $true
                    break
                }
            }
        }
    }
    if (-not $qt6Found) {
        $missing += "Qt6 (not found via qmake, vcpkg, or C:\Qt\6.x.x\msvc2022_64)"
    }

    if ($missing.Count -gt 0) {
        Write-Host "  Missing required dependencies:" -ForegroundColor Red
        foreach ($m in $missing) {
            Write-Host "    - $m" -ForegroundColor Red
        }
        Write-Host ""
        Write-Host "  Ensure MSVC Build Tools are installed and vcvars64.bat has been run."
        Write-Host "  Qt6 can be installed via vcpkg: vcpkg install qt6-base[core,widgets]:x64-windows"
        Write-Host "  Or manually from https://www.qt.io/download-qt-installer"
        Write-Host "  (re-run with --skip-check to bypass)" -ForegroundColor Yellow
        exit 1
    }

    if (-not (Test-CommandExists "sccache")) {
        Write-Host "  WARN: sccache not found - compiler cache disabled" -ForegroundColor Yellow
    }
    if (-not (Test-CommandExists "cargo")) {
        Write-Host "  WARN: cargo not found - LOOT sorting without libloot" -ForegroundColor Yellow
    }

    Write-Host "  OK"
}

# --- Build directory selection (matches CMakePresets.json binaryDir) ---
if ($BuildType -eq "Debug") {
    $PresetName = "windows-debug"
} else {
    $PresetName = "windows-release"
}
$BuildDir = Join-Path $ScriptDir "build\$PresetName"

# --- Pipeline ---

if ($CleanFirst) {
    Write-Host "==> Removing old build directory..."
    $parentBuildDir = Join-Path $ScriptDir "build"
    if (Test-Path $parentBuildDir) {
        Remove-Item -Recurse -Force $parentBuildDir
        Write-Host "  Removed $parentBuildDir"
    }
}

if (-not $SkipCheck) {
    Invoke-DependencyCheck
}

if ($UpdateSubmodules) {
    Write-Host "==> Updating git submodules..."
    & git submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: git submodule update failed" -ForegroundColor Red
        exit 1
    }
}

# CMake configure — use the preset which already has MSVC flags
$CMakeArgs = @("--preset", $PresetName)

# Qt6 prefix path - auto-detect
$cmakePrefixPath = ""
if (Test-Path "C:\vcpkg\installed\x64-windows\share\Qt6\Qt6Config.cmake") {
    # vcpkg - CMake should find it via toolchain
} elseif (Test-Path "C:\Qt") {
    $qtDirs = Get-ChildItem -Path "C:\Qt" -Directory -Filter "6.*" -ErrorAction SilentlyContinue
    foreach ($d in $qtDirs) {
        $msvcDir = Join-Path $d.FullName "msvc2022_64"
        if (Test-Path $msvcDir) {
            $cmakePrefixPath = $msvcDir
            break
        }
    }
}

if ($cmakePrefixPath) {
    $CMakeArgs += "-DCMAKE_PREFIX_PATH=$cmakePrefixPath"
    Write-Host "  Qt6 prefix: $cmakePrefixPath"
}

# vcpkg toolchain - auto-detect
$vcpkgToolchain = "C:\vcpkg\scripts\buildsystems\vcpkg.cmake"
if (Test-Path $vcpkgToolchain) {
    $CMakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain"
    Write-Host "  vcpkg toolchain: $vcpkgToolchain"
}

Write-Host "==> Configuring (preset=$PresetName)"
Write-Host "  CMake args: $CMakeArgs"
& cmake @CMakeArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: CMake configure failed" -ForegroundColor Red
    exit 1
}

# Build
$ParallelJobs = [Environment]::ProcessorCount
Write-Host "==> Building ($ParallelJobs parallel jobs)..."
& cmake --build $BuildDir --config $BuildType --parallel $ParallelJobs
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Build failed" -ForegroundColor Red
    exit 1
}

# Tests
if ($RunTests) {
    Write-Host "==> Running test suite..."
    & ctest --test-dir $BuildDir --output-on-failure --build-config $BuildType
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Tests failed" -ForegroundColor Red
        exit 1
    }
}

# compile_commands.json - symlink (or copy fallback) at repo root
$CompileCommandsSrc = Join-Path $BuildDir "compile_commands.json"
$CompileCommandsDst = Join-Path $ScriptDir "compile_commands.json"
if (Test-Path $CompileCommandsSrc) {
    if (Test-Path $CompileCommandsDst) {
        Remove-Item -Force $CompileCommandsDst
    }
    try {
        New-Item -ItemType SymbolicLink -Path $CompileCommandsDst -Target $CompileCommandsSrc | Out-Null
        Write-Host "  compile_commands.json -> symlinked to $CompileCommandsSrc"
    } catch {
        Copy-Item -Path $CompileCommandsSrc -Destination $CompileCommandsDst -Force
        Write-Host "  compile_commands.json -> copied to repo root (symlink requires Developer Mode or admin)"
    }
}

# --- Summary ---
$BinaryPath = Join-Path $BuildDir "gamemodmanager.exe"
$PluginsPath = Join-Path $BuildDir "plugins"

Write-Host ""
Write-Host "Build complete."
Write-Host "  Config:    $BuildType (preset: $PresetName)"
Write-Host "  Build dir: $BuildDir"
if (Test-Path $BinaryPath) {
    Write-Host "  Binary:    $BinaryPath"
} else {
    Write-Host "  Binary:    $BuildDir\gamemodmanager.exe"
}
Write-Host "  Plugins:   $PluginsPath\"
Write-Host "  Tests:     $(if ($RunTests) { 'ran above' } else { 'skipped' })"
