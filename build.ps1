param([switch]$Debug)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$install = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$install) { throw 'Install Visual Studio C++ Desktop build tools and Windows SDK first.' }
$msvcRoot = Join-Path $install 'VC\Tools\MSVC'
$msvc = (Get-ChildItem -LiteralPath $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$kit = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdk = (Get-ChildItem -LiteralPath (Join-Path $kit 'Include') -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'cppwinrt') } | Sort-Object Name -Descending | Select-Object -First 1).Name
$compiler = Join-Path $msvc 'bin\Hostx64\x64\cl.exe'
$env:PATH = (Join-Path $kit "bin\$sdk\x64") + ';' + (Join-Path $msvc 'bin\Hostx64\x64') + ';' + $env:PATH
$env:INCLUDE = @((Join-Path $msvc 'include'), (Join-Path $kit "Include\$sdk\ucrt"), (Join-Path $kit "Include\$sdk\shared"), (Join-Path $kit "Include\$sdk\um"), (Join-Path $kit "Include\$sdk\winrt"), (Join-Path $kit "Include\$sdk\cppwinrt")) -join ';'
$env:LIB = @((Join-Path $msvc 'lib\x64'), (Join-Path $kit "Lib\$sdk\ucrt\x64"), (Join-Path $kit "Lib\$sdk\um\x64")) -join ';'
$buildDir = Join-Path $projectRoot 'build'
$distDir = Join-Path $projectRoot 'dist'
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
New-Item -ItemType Directory -Path $distDir -Force | Out-Null
$outputExe = Join-Path $distDir 'SelfWindow.exe'
$argsList = @('/nologo', '/std:c++17', '/EHsc', '/utf-8', '/W4', '/MT', '/DUNICODE', '/D_UNICODE', '/D_WIN32_WINNT=0x0A00', '/DWINVER=0x0A00', '/bigobj')
if ($Debug) { $argsList += @('/Od', '/Zi') } else { $argsList += '/O2' }
$argsList += @('..\src\main.cpp', '..\src\graphics.cpp', "/Fe:$outputExe", '/link', '/SUBSYSTEM:WINDOWS', '/MANIFEST:EMBED', '/MANIFESTINPUT:..\SelfWindow.manifest', 'user32.lib', 'gdi32.lib', 'comctl32.lib', 'shell32.lib', 'd3d11.lib', 'dxgi.lib', 'd3dcompiler.lib', 'windowsapp.lib', 'Magnification.lib', 'ole32.lib', 'winmm.lib')
Push-Location -LiteralPath $buildDir
try {
    & $compiler @argsList
    $buildExitCode = $LASTEXITCODE
    if ($buildExitCode -ne 0) { throw "Build failed: $buildExitCode" }
} finally { Pop-Location }
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination (Join-Path $distDir '使用说明.md') -Force
Write-Output $outputExe
