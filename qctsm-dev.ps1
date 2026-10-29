# PowerShell 5.1+ entry point. Python is a build-time tool only.
$ErrorActionPreference = 'Stop'
if ($IsWindows -or $env:OS -eq 'Windows_NT') {
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue) -or -not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $installation = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($installation -and -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
                Import-Module (Join-Path $installation 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
                Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
            }
            if ($installation -and -not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
                $ninjaDirectory = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
                if (Test-Path -LiteralPath (Join-Path $ninjaDirectory 'ninja.exe')) {
                    $env:PATH = $ninjaDirectory + [IO.Path]::PathSeparator + $env:PATH
                }
            }
        }
    }
}
$pythonCommand = $env:QCOPILOTS_TOOL_SEMANTIC_MATCHING_PYTHON
if (-not $pythonCommand) {
    foreach ($candidate in @('python3', 'python', 'py')) {
        $resolved = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($resolved -and (& $resolved.Source -c 'import sys; print(sys.version_info >= (3, 10))' 2>$null) -eq 'True') {
            $pythonCommand = $resolved.Source
            break
        }
    }
}
if (-not $pythonCommand) { throw 'Python 3.10+ is required for developer scripts. Set QCOPILOTS_TOOL_SEMANTIC_MATCHING_PYTHON to its executable.' }
& $pythonCommand (Join-Path $PSScriptRoot 'tools\qctsm-dev.py') @args
exit $LASTEXITCODE
