<#
.SYNOPSIS
    Build RobRun from a normal PowerShell window.

.DESCRIPTION
    Locates Visual Studio through vswhere, initializes the x64 MSVC
    environment, and builds the overlay, driver, and mapper into build/.
    -Deploy assembles the runtime files in dist/.

.PARAMETER Configuration
    Release (default) or Debug.

.PARAMETER Target
    All (default), Overlay, Driver, or Mapper.

.PARAMETER Clean
    Remove build/ and dist/ before building.

.PARAMETER Deploy
    Copy the runtime files and launchers into dist/.

.PARAMETER VsPath
    Visual Studio installation directory. Detected automatically when omitted.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('All', 'Overlay', 'Driver', 'Mapper')]
    [string]$Target = 'All',

    [switch]$Clean,
    [switch]$Deploy,
    [string]$VsPath
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildRoot = Join-Path $root 'build'
$distRoot = Join-Path $root 'dist'
$overlayDir = Join-Path $root 'src\overlay'
$driverDir = Join-Path $root 'src\driver'
$sharedDir = Join-Path $root 'src\shared'
$resourcesDir = Join-Path $root 'resources'
$webviewDir = Join-Path $root 'third_party\webview2'

function Write-Step([string]$message) {
    Write-Host "==> $message" -ForegroundColor Cyan
}

function Invoke-Native([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$exe failed with exit code $LASTEXITCODE"
    }
}

function Find-VisualStudio {
    if ($VsPath) {
        if (!(Test-Path (Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'))) {
            throw "vcvars64.bat not found under '$VsPath'."
        }
        return $VsPath
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path $vswhere)) {
        throw 'vswhere.exe not found. Install Visual Studio 2019/2022/2026 with the C++ workload.'
    }

    $found = & $vswhere -latest -prerelease -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (!$found) {
        throw 'No Visual Studio installation with the x64 C++ toolset was found.'
    }
    return $found.Trim()
}

function Enter-MsvcEnvironment([string]$vsPath) {
    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    $lines = & cmd.exe /c "`"$vcvars`" >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to initialize the MSVC environment from '$vcvars'."
    }
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path "env:$($matches[1])" -Value $matches[2]
        }
    }
}

function Find-Wdk {
    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
    $includeRoot = Join-Path $kitsRoot 'Include'
    if (!(Test-Path $includeRoot)) {
        throw 'Windows Kits 10 include directory not found. Install the Windows Driver Kit.'
    }

    $versions = Get-ChildItem $includeRoot -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName 'km\ntddk.h') } |
        Sort-Object Name -Descending
    if (!$versions) {
        throw 'No Windows Driver Kit headers found under Windows Kits 10.'
    }

    $version = $versions[0].Name
    $lib = Join-Path $kitsRoot "Lib\$version\km\x64"
    if (!(Test-Path $lib)) {
        throw "WDK x64 libraries not found for $version."
    }

    return @{
        Version = $version
        Include = Join-Path $includeRoot $version
        Lib = $lib
    }
}

function Get-CommonCFlags {
    if ($Configuration -eq 'Debug') {
        return @('/nologo', '/c', '/Od', '/W4', '/MTd', '/utf-8',
                 '/D_CRT_SECURE_NO_WARNINGS', '/D_DEBUG')
    }
    return @('/nologo', '/c', '/O2', '/W4', '/MT', '/utf-8',
             '/D_CRT_SECURE_NO_WARNINGS', '/DNDEBUG')
}

function Build-Overlay {
    Write-Step "Building overlay ($Configuration)"
    $outDir = Join-Path $buildRoot 'overlay'
    $objDir = Join-Path $outDir 'obj'
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null

    $common = Get-CommonCFlags
    $includes = @("/I$overlayDir", "/I$sharedDir", "/I$resourcesDir")
    $cSources = @(
        'rr_esp.c', 'rr_game.c', 'rr_mem.c', 'rr_unity.c', 'rr_hiding.c',
        'rr_config.c', 'rr_cham_model.c', 'rr_vehicle_cham_model.c',
        'rr_draw.c', 'rr_aim.c', 'rr_los_geom.c', 'rr_los_capture.c',
        'rr_los_runtime.c', 'rr_los.c'
    )
    $cxxSources = @('gpu.cpp', 'menu_webview.cpp')
    $objects = @()

    foreach ($source in $cSources) {
        $name = [IO.Path]::GetFileNameWithoutExtension($source)
        Write-Host "  $source"
        $obj = Join-Path $objDir "$name.obj"
        Invoke-Native 'cl.exe' ($common + $includes + @("/Fo$obj", (Join-Path $overlayDir $source)))
        $objects += $obj
    }

    foreach ($source in $cxxSources) {
        $name = [IO.Path]::GetFileNameWithoutExtension($source)
        Write-Host "  $source"
        $obj = Join-Path $objDir "$name.obj"
        $cxxFlags = @($common) + @('/EHsc', '/std:c++20', "/I$webviewDir\include")
        Invoke-Native 'cl.exe' ($cxxFlags + $includes + @("/Fo$obj", (Join-Path $overlayDir $source)))
        $objects += $obj
    }

    Write-Host '  resources'
    Push-Location $resourcesDir
    try {
        Invoke-Native 'rc.exe' @('/nologo', '/fo', (Join-Path $objDir 'app.res'), 'app.rc')
    } finally {
        Pop-Location
    }

    $exe = Join-Path $outDir 'RobRun.exe'
    $linkArgs = @('/nologo', "/OUT:$exe", '/SUBSYSTEM:WINDOWS', '/MACHINE:X64') +
        $objects + @((Join-Path $objDir 'app.res'),
        'kernel32.lib', 'user32.lib', 'gdi32.lib', 'comctl32.lib', 'advapi32.lib',
        'ole32.lib', 'oleaut32.lib', 'uuid.lib', 'shell32.lib', 'winmm.lib',
        'd3d11.lib', 'dxgi.lib', 'd2d1.lib', 'dwrite.lib', 'dcomp.lib',
        'd3dcompiler.lib', 'dbghelp.lib',
        "/LIBPATH:$webviewDir\lib\x64", 'WebView2Loader.dll.lib')
    Invoke-Native 'link.exe' $linkArgs

    Copy-Item (Join-Path $webviewDir 'lib\x64\WebView2Loader.dll') $outDir -Force
    Write-Host "  -> $exe"
}

function Build-Driver {
    Write-Step "Building driver ($Configuration)"
    $wdk = Find-Wdk
    Write-Host "  WDK $($wdk.Version)"
    $outDir = Join-Path $buildRoot 'driver'
    $objDir = Join-Path $outDir 'obj'
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null

    $obj = Join-Path $objDir 'robrun_drv.obj'
    $res = Join-Path $objDir 'driver.res'
    $source = Join-Path $driverDir 'robrun_drv.c'

    $cFlags = @('/nologo', '/c', '/W4', '/GS-', '/MT', '/utf-8',
                '/D_WIN64', '/D_AMD64_', '/DAMD64', '/DNDEBUG',
                '/Zc:wchar_t', '/Zc:forScope', '/GR-', '/kernel',
                "/I$(Join-Path $wdk.Include 'km')",
                "/I$(Join-Path $wdk.Include 'shared')",
                "/I$(Join-Path $wdk.Include 'km\crt')",
                "/I$sharedDir",
                "/Fo$obj", $source)
    if ($Configuration -eq 'Debug') {
        $cFlags = $cFlags | ForEach-Object { if ($_ -eq '/DNDEBUG') { '/D_DEBUG' } else { $_ } }
    } else {
        $cFlags += '/O2'
    }
    Write-Host '  robrun_drv.c'
    Invoke-Native 'cl.exe' $cFlags

    Write-Host '  resources'
    Push-Location $resourcesDir
    try {
        Invoke-Native 'rc.exe' @('/nologo', '/fo', $res, 'driver.rc')
    } finally {
        Pop-Location
    }

    $sys = Join-Path $outDir 'RobRun.sys'
    $linkArgs = @('/NOLOGO', "/OUT:$sys", '/NODEFAULTLIB', "/LIBPATH:$($wdk.Lib)",
                  'ntoskrnl.lib', 'hal.lib', 'wdmsec.lib',
                  '/SUBSYSTEM:NATIVE', '/DRIVER', '/ENTRY:DriverEntry',
                  '/MACHINE:X64', '/KERNEL', $obj, $res)
    Invoke-Native 'link.exe' $linkArgs
    Write-Host "  -> $sys"
}

function Build-Mapper {
    Write-Step "Building mapper ($Configuration)"
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $msbuild = & $vswhere -latest -prerelease -products * `
        -requires Microsoft.Component.MSBuild `
        -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (!$msbuild) {
        throw 'MSBuild was not found in the selected Visual Studio installation.'
    }

    $solution = Join-Path $root 'src\mapper\kdmapper.sln'
    $args = @($solution, '/m', '/v:minimal', "/p:Configuration=$Configuration", '/p:Platform=x64')
    Invoke-Native $msbuild $args
    $exe = Join-Path $buildRoot 'mapper\RobRunMapper.exe'
    if (!(Test-Path $exe)) {
        throw "Mapper output not found at '$exe'."
    }
    Write-Host "  -> $exe"
}

function Deploy-Release {
    Write-Step 'Assembling dist'
    New-Item -ItemType Directory -Force -Path $distRoot | Out-Null

    $files = @(
        @{ Source = (Join-Path $buildRoot 'overlay\RobRun.exe'); Name = 'RobRun.exe' },
        @{ Source = (Join-Path $webviewDir 'lib\x64\WebView2Loader.dll'); Name = 'WebView2Loader.dll' },
        @{ Source = (Join-Path $buildRoot 'mapper\RobRunMapper.exe'); Name = 'RobRunMapper.exe' },
        @{ Source = (Join-Path $buildRoot 'driver\RobRun.sys'); Name = 'RobRun.sys' },
        @{ Source = (Join-Path $root 'scripts\run.cmd'); Name = 'run.cmd' },
        @{ Source = (Join-Path $root 'scripts\map-driver.cmd'); Name = 'map-driver.cmd' }
    )
    if (Test-Path (Join-Path $root 'README.md')) {
        $files += @{ Source = (Join-Path $root 'README.md'); Name = 'README.md' }
    }
    if (Test-Path (Join-Path $root 'THIRD_PARTY_NOTICES.md')) {
        $files += @{ Source = (Join-Path $root 'THIRD_PARTY_NOTICES.md'); Name = 'THIRD_PARTY_NOTICES.md' }
    }

    foreach ($file in $files) {
        if (!(Test-Path $file.Source)) {
            throw "Missing release input: $($file.Source)"
        }
        Copy-Item $file.Source (Join-Path $distRoot $file.Name) -Force
    }
    Write-Host "  -> $distRoot"
}

if ($Clean) {
    Write-Step 'Cleaning build and dist'
    foreach ($path in @($buildRoot, $distRoot)) {
        if (Test-Path $path) { Remove-Item $path -Recurse -Force }
    }
}

$vs = Find-VisualStudio
Enter-MsvcEnvironment $vs
Write-Step "Visual Studio: $vs"

switch ($Target) {
    'Overlay' { Build-Overlay }
    'Driver'  { Build-Driver }
    'Mapper'  { Build-Mapper }
    'All'     { Build-Overlay; Build-Driver; Build-Mapper }
}

if ($Deploy) {
    Deploy-Release
}

Write-Step 'Build complete'
