param([Parameter(ValueFromRemainingArguments)][string[]]$LoaderArgs)

$ErrorActionPreference = 'Stop'
$python = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
if (!(Test-Path -LiteralPath $python -PathType Leaf)) {
    $command = Get-Command python.exe, py.exe -CommandType Application -ErrorAction SilentlyContinue |
        Where-Object { $_.Source -notlike '*\Microsoft\WindowsApps\*' } |
        Select-Object -First 1
    if (!$command) { throw 'Python 3.9 or later is required.' }
    $python = $command.Source
}
& $python -B (Join-Path $PSScriptRoot 'loader.py') @LoaderArgs
exit $LASTEXITCODE
