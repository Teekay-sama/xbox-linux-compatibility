param(
    [Parameter(Mandatory, Position = 0)][string]$Target,
    [string]$Python
)

$ErrorActionPreference = 'Stop'
if (!$Python) {
    $bundled = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
    if (Test-Path -LiteralPath $bundled -PathType Leaf) {
        $Python = $bundled
    } else {
        $command = Get-Command python.exe, py.exe -CommandType Application -ErrorAction SilentlyContinue |
            Where-Object { $_.Source -notlike '*\Microsoft\WindowsApps\*' } |
            Select-Object -First 1
        if (!$command) { throw 'Python 3.9 or newer was not found. Specify its executable with -Python.' }
        $Python = $command.Source
    }
}

& $Python -B (Join-Path $PSScriptRoot 'check_imports.py') -- $Target
exit $LASTEXITCODE
