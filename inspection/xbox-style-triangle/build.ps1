param([string]$VsInstallDir, [string]$Dxc)

$ErrorActionPreference = 'Stop'
if (!$VsInstallDir) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
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
    New-Item -Force -ItemType Directory -Path build, bin | Out-Null
    & $Dxc -T vs_6_0 -E vertex_main -Fh build/triangle_vs.h -Vn triangle_vs triangle.hlsl
    if ($LASTEXITCODE -ne 0) { throw 'Vertex shader compilation failed.' }
    & $Dxc -T ps_6_0 -E pixel_main -Fh build/triangle_ps.h -Vn triangle_ps triangle.hlsl
    if ($LASTEXITCODE -ne 0) { throw 'Pixel shader compilation failed.' }
    & $Dxc -T vs_6_0 -E texture_vertex_main -Fh build/triangle_texture_vs.h -Vn triangle_texture_vs triangle.hlsl
    if ($LASTEXITCODE -ne 0) { throw 'Texture vertex shader compilation failed.' }

    $sample = Join-Path (Split-Path -Parent $PSScriptRoot) 'Xbox-GDK-Samples/Samples/IntroGraphics/SimpleTriangle'
    & $Dxc -T vs_6_0 -E main -Fh build/simpletriangle_vs.h -Vn simpletriangle_vs (Join-Path $sample 'VertexShader.hlsl')
    if ($LASTEXITCODE -ne 0) { throw 'Microsoft reference vertex shader compilation failed.' }
    & $Dxc -T ps_6_0 -E main -Fh build/simpletriangle_ps.h -Vn simpletriangle_ps (Join-Path $sample 'PixelShader.hlsl')
    if ($LASTEXITCODE -ne 0) { throw 'Microsoft reference pixel shader compilation failed.' }

    $copyPoint = Join-Path (Split-Path -Parent $PSScriptRoot) 'texture-proof/copy_point.dxil'
    if (!(Test-Path -LiteralPath $copyPoint -PathType Leaf)) { throw "Missing translated shader: $copyPoint" }
    $bytes = [IO.File]::ReadAllBytes($copyPoint)
    if ($bytes.Length -lt 32 -or [Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne 'DXBC') {
        throw 'Expected a desktop DXIL shader container.'
    }
    # Embed the previously verified bytecode verbatim. The runtime needs only the EXE and shim.
    $shaderHeader = [System.Text.StringBuilder]::new()
    [void]$shaderHeader.AppendLine('inline constexpr unsigned char triangle_copy_point_ps[] = {')
    for ($i = 0; $i -lt $bytes.Length; $i += 16) {
        $last = [Math]::Min($i + 15, $bytes.Length - 1)
        $row = $bytes[$i..$last] | ForEach-Object { '0x{0:x2}' -f $_ }
        [void]$shaderHeader.AppendLine('    ' + ($row -join ', ') + ',')
    }
    [void]$shaderHeader.AppendLine('};')
    $shaderHeader.ToString() | Set-Content -Encoding Ascii 'build/triangle_copy_point_ps.h'

    $shim = 'cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /LD /Fobuild/ /Febin/xbox_style_graphics.dll graphics_shim.cpp /link /implib:build/xbox_style_graphics.lib d3d12.lib dxgi.lib'
    $hostApp = 'cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /I build /Fobuild/ /Febin/xbox_style_triangle.exe main.cpp draw_triangle.cpp verify_triangle.cpp /link build/xbox_style_graphics.lib d3d12.lib'
    & cmd.exe /d /c "call `"$vcvars`" && $shim && $hostApp"
    if ($LASTEXITCODE -ne 0) { throw "C++ build exited with code $LASTEXITCODE." }
} finally {
    Pop-Location
}
