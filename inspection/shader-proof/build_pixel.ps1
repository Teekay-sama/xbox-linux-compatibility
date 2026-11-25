param([string]$VsInstallDir, [string]$Dxc)

$ErrorActionPreference = 'Stop'
if (!$VsInstallDir) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Set -VsInstallDir to the Visual Studio installation.' }
    $VsInstallDir = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$VsInstallDir) { throw 'Visual Studio with the x64 C++ tools was not found.' }
}

$vcvars = Join-Path $VsInstallDir 'VC/Auxiliary/Build/vcvars64.bat'
if (!(Test-Path -LiteralPath $vcvars)) { throw "vcvars64.bat was not found at $vcvars" }
if (!$Dxc) {
    $sdkBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
    $Dxc = Get-ChildItem -LiteralPath $sdkBin -Directory |
        Where-Object { $_.Name -as [version] } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64/dxc.exe' } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    if (!$Dxc) { throw 'Set -Dxc to the Windows SDK DirectX Shader Compiler.' }
}
Push-Location -LiteralPath $PSScriptRoot
try {
    & $Dxc -T vs_6_0 -E main -Fh fullscreen_vs.h -Vn fullscreen_vs fullscreen.hlsl
    if ($LASTEXITCODE -ne 0) { throw "Vertex shader build exited with code $LASTEXITCODE" }
    & cmd.exe /d /c "call `"$vcvars`" && cl /nologo /std:c++17 /EHsc /W4 /O2 run_pixel.cpp /link /out:run_pixel.exe d3d12.lib dxgi.lib"
    if ($LASTEXITCODE -ne 0) { throw "C++ build exited with code $LASTEXITCODE" }
} finally {
    Pop-Location
}
