param(
    [string]$SQLiteRoot = $env:CONDA_PREFIX,
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build')
)

$ErrorActionPreference = 'Stop'
$sourceDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)

if (-not $SQLiteRoot) {
    $candidate = Join-Path $env:USERPROFILE 'anaconda3'
    if (Test-Path -LiteralPath (Join-Path $candidate 'Library\include\sqlite3.h')) {
        $SQLiteRoot = $candidate
    }
}
if (-not $SQLiteRoot) {
    throw 'Pass -SQLiteRoot or activate a Conda environment containing SQLite.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer vswhere.exe was not found.'
}
$visualStudio = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) {
    throw 'A Visual Studio C++ toolchain was not found.'
}

$vcvars = Join-Path $visualStudio 'VC\Auxiliary\Build\vcvars64.bat'
$cmake = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $vcvars) -or -not (Test-Path -LiteralPath $cmake)) {
    throw 'The Visual Studio C++ or CMake components are incomplete.'
}

$buildCommand = 'call "{0}" >nul && "{1}" -S "{2}" -B "{3}" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DSQLITE3_ROOT="{4}" && "{1}" --build "{3}"' -f `
    $vcvars, $cmake, $sourceDir, $BuildDir, ([System.IO.Path]::GetFullPath($SQLiteRoot))
& cmd.exe /d /s /c $buildCommand
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Output "build passed: $BuildDir"
