param([string]$VsInstallDir, [string]$Dxc)

$ErrorActionPreference = 'Stop'
if (!$VsInstallDir) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $VsInstallDir = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$VsInstallDir) { throw 'Visual Studio with the x64 C++ tools was not found.' }
}

$renderer = Join-Path $PSScriptRoot '../../xbox-style-triangle'
& (Join-Path $renderer 'build.ps1') -VsInstallDir $VsInstallDir -Dxc $Dxc
$vcvars = Join-Path $VsInstallDir 'VC/Auxiliary/Build/vcvars64.bat'
Push-Location -LiteralPath $PSScriptRoot
try {
    New-Item -Force -ItemType Directory -Path build, bin | Out-Null
    $startup = 'cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /LD /Fobuild/ /Febin/startup_shim.dll startup_shim.cpp /link /implib:build/startup_shim.lib'
    $frame = 'cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /LD /Fobuild/ /Febin/frame_shim.dll frame_shim.cpp /link /implib:build/frame_shim.lib d3d12.lib dxgi.lib user32.lib'
    $fixture = 'cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /I ../../xbox-style-triangle /Fobuild/ /Febin/simpletriangle_shim_test.exe prototype_main.cpp /link build/startup_shim.lib build/frame_shim.lib ../../xbox-style-triangle/build/draw_triangle.obj ../../xbox-style-triangle/build/verify_triangle.obj ../../xbox-style-triangle/build/xbox_style_graphics.lib d3d12.lib user32.lib'
    & cmd.exe /d /c "call `"$vcvars`" && $startup && $frame && $fixture"
    if ($LASTEXITCODE -ne 0) { throw "Shim fixture build exited with code $LASTEXITCODE." }
    Copy-Item -LiteralPath (Join-Path $renderer 'bin/xbox_style_graphics.dll') -Destination 'bin/xbox_style_graphics.dll'
} finally {
    Pop-Location
}
