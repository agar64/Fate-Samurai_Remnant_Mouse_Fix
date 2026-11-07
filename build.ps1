Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$SrcDir  = Join-Path $RootDir "src"
$ObjDir  = Join-Path $RootDir "build\obj"
$DistDir = Join-Path $RootDir "build\dist"

New-Item -ItemType Directory -Force -Path $ObjDir  | Out-Null
New-Item -ItemType Directory -Force -Path $DistDir | Out-Null

$OutDll = Join-Path $DistDir "dinput8.dll"
$CppSrc = Join-Path $SrcDir  "dllmain.cpp"
$DefSrc = Join-Path $SrcDir  "dinput8.def"

$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$VcVars  = $null

if (Test-Path $VsWhere) {
    $VsPath = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($VsPath) {
        $Candidate = Join-Path $VsPath "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $Candidate) {
            $VcVars = $Candidate
        }
    }
}

if ($VcVars) {
    Write-Host "Building with MSVC ($VcVars)..." -ForegroundColor Cyan
    $CmdLine = "call `"$VcVars`" >nul && cl.exe /nologo /std:c++17 /O2 /EHsc /MT /W4 /LD `"$CppSrc`" /Fe:`"$OutDll`" /Fo:`"$ObjDir\\`" /link /DEF:`"$DefSrc`" /INCREMENTAL:NO user32.lib kernel32.lib"
    cmd.exe /c $CmdLine
    if ($LASTEXITCODE -ne 0) {
        throw "MSVC build failed with exit code $LASTEXITCODE"
    }
} elseif (Get-Command "C:\msys64\ucrt64\bin\g++.exe" -ErrorAction SilentlyContinue) {
    Write-Host "Building with MinGW-w64 g++..." -ForegroundColor Cyan
    & "C:\msys64\ucrt64\bin\g++.exe" -std=c++17 -O2 -Wall -Wextra -shared -static-libgcc -static-libstdc++ $CppSrc $DefSrc -o $OutDll -luser32 -lkernel32
    if ($LASTEXITCODE -ne 0) {
        throw "g++ build failed with exit code $LASTEXITCODE"
    }
} else {
    throw "No suitable x64 C++ compiler found (install Visual Studio C++ Build Tools or MSYS2 UCRT64)."
}

Copy-Item -Force $OutDll (Join-Path $DistDir "FSRMouseFix.asi")
Copy-Item -Force (Join-Path $RootDir "fsr_mouse_fix.ini") (Join-Path $DistDir "fsr_mouse_fix.ini")

$DllInfo = Get-Item $OutDll
$Hash    = (Get-FileHash -Algorithm SHA256 $OutDll).Hash
Write-Host ""
Write-Host "Build succeeded!" -ForegroundColor Green
Write-Host "  Output DLL : $($DllInfo.FullName) ($($DllInfo.Length) bytes)"
Write-Host "  Output ASI : $(Join-Path $DistDir 'FSRMouseFix.asi') ($($DllInfo.Length) bytes)"
Write-Host "  SHA-256    : $Hash"
Write-Host "  Config INI : $(Join-Path $DistDir 'fsr_mouse_fix.ini')"
