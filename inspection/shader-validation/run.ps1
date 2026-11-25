<#
.SYNOPSIS
Checks the existing desktop DXIL translations with DXC, WARP pipeline creation,
and offscreen pixel readback. Writes a JSON report under runs and exits 1 if a
check fails.
#>
param(
    [string]$VsInstallDir,
    [string]$DxCompiler
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

function Invoke-ShaderCheck([string]$Executable, [string[]]$Arguments) {
    # Windows PowerShell surfaces redirected native stderr as error records.
    $ErrorActionPreference = 'Continue'
    $output = & $Executable @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    [ordered]@{
        status = if ($exitCode -eq 0) { 'passed' } else { 'failed' }
        exit_code = $exitCode
        output = @($output | ForEach-Object { $_.ToString() })
    }
}

if (!$VsInstallDir) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Set -VsInstallDir to the Visual Studio installation with x64 C++ tools.'
    }
    $VsInstallDir = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$VsInstallDir) { throw 'Visual Studio with x64 C++ tools was not found.' }
}
$vcvars = (Get-Item -LiteralPath (Join-Path $VsInstallDir 'VC/Auxiliary/Build/vcvars64.bat')).FullName

if (!$DxCompiler) {
    $sdkBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
    $DxCompiler = Get-ChildItem -LiteralPath $sdkBin -Directory |
        Where-Object { $_.Name -as [version] } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64/dxcompiler.dll' } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (!$DxCompiler) { throw 'Set -DxCompiler to the x64 Windows SDK dxcompiler.dll.' }
}
$DxCompiler = (Get-Item -LiteralPath $DxCompiler).FullName

$cases = @(
    @{
        Name = 'PS_clearRipple'
        Shader = 'shader-proof/clear_ripple.dxil'
        Harness = 'run_pixel.exe'
        Arguments = @('0.5', '0.5', '1', '0')
    }
    @{
        Name = 'PS_CopyPoint'
        Shader = 'texture-proof/copy_point.dxil'
        Harness = 'run_texture.exe'
        Arguments = @('warp')
    }
)

$runId = '{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), [guid]::NewGuid().ToString('N').Substring(0, 8)
$runDir = Join-Path $PSScriptRoot "runs/$runId"
$null = New-Item -Path $runDir -ItemType Directory
$buildCommand = @(
    ('call "{0}"' -f $vcvars)
    'cl /nologo /std:c++17 /EHsc /W4 /O2 ..\..\validate_dxil.cpp /link /out:validate_dxil.exe'
    'cl /nologo /std:c++17 /EHsc /W4 /O2 ..\..\..\shader-proof\run_pixel.cpp /link /out:run_pixel.exe d3d12.lib dxgi.lib'
    'cl /nologo /std:c++17 /EHsc /W4 /O2 ..\..\..\texture-proof\run_texture.cpp /link /out:run_texture.exe d3d12.lib dxgi.lib'
) -join ' && '

Push-Location -LiteralPath $runDir
try {
    & cmd.exe /d /v:off /c $buildCommand
    if ($LASTEXITCODE -ne 0) { throw "Validation tools build exited with code $LASTEXITCODE." }
} finally {
    Pop-Location
}

$inspection = Split-Path -Parent $PSScriptRoot
$validator = Join-Path $runDir 'validate_dxil.exe'
$failed = $false
$shaders = @(foreach ($case in $cases) {
    $source = Join-Path $inspection $case.Shader
    $shader = Join-Path $runDir (Split-Path -Leaf $source)
    $hash = $null
    if (Test-Path -LiteralPath $source -PathType Leaf) {
        Copy-Item -LiteralPath $source -Destination $shader
        $hash = (Get-FileHash -LiteralPath $shader -Algorithm SHA256).Hash
    }

    $harness = Join-Path $runDir $case.Harness
    $drawArguments = @($shader) + $case.Arguments
    $stages = @(
        @{ Name = 'dxil_validation'; Executable = $validator; Arguments = @($DxCompiler, $shader) }
        @{ Name = 'warp_pipeline'; Executable = $harness; Arguments = $drawArguments + @('--pipeline-only') }
        @{ Name = 'pixel_readback'; Executable = $harness; Arguments = $drawArguments }
    )
    $checks = [ordered]@{}
    $failedStage = $null
    foreach ($stage in $stages) {
        if ($failedStage) {
            $result = [ordered]@{ status = 'skipped'; reason = "$failedStage failed" }
        } else {
            $result = Invoke-ShaderCheck -Executable $stage.Executable -Arguments $stage.Arguments
            if ($result.status -eq 'failed') {
                $failedStage = $stage.Name
                $failed = $true
            }
        }
        $checks[$stage.Name] = $result
        Write-Host ('{0} {1} {2}' -f $case.Name, $stage.Name, $result.status)
        $result.output | ForEach-Object { Write-Host "  $_" }
    }

    [ordered]@{
        shader = $case.Name
        source = $case.Shader
        sha256 = $hash
        checks = $checks
    }
})

$reportPath = Join-Path $runDir 'report.json'
[ordered]@{
    passed = !$failed
    adapter = 'WARP'
    dxcompiler = $DxCompiler
    shaders = $shaders
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding utf8
Write-Host "Report saved to $reportPath"
exit [int]$failed
