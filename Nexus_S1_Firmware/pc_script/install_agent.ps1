param(
    [string]$Python,
    [string]$SerialNumber,
    [string]$StateDirectory = (Join-Path $env:LOCALAPPDATA 'NexusS1Agent'),
    [string]$StartupDirectory = [Environment]::GetFolderPath('Startup'),
    [switch]$SkipStart
)
$ErrorActionPreference = 'Stop'
$owner = 'NexusS1AgentMonitor/v1'
function Get-PythonCandidates {
    param([string]$ExplicitPython)
    # Add each path separately: never cast/join an array into an executable name.
    $candidates = New-Object 'System.Collections.Generic.List[string]'
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPython)) {
        $candidates.Add($ExplicitPython)
    } else {
        if ($env:USERPROFILE) {
            $candidates.Add((Join-Path $env:USERPROFILE 'anaconda3\python.exe'))
            $candidates.Add((Join-Path $env:USERPROFILE 'miniconda3\python.exe'))
        }
        $pathPython = Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($pathPython) { $candidates.Add([string]$pathPython.Source) }
    }
    return $candidates.ToArray() | Select-Object -Unique
}
function Resolve-AgentPython {
    param([string[]]$Candidates)
    foreach ($candidate in $Candidates) {
        $path = $candidate
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $command = Get-Command $candidate -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($command) { $path = [string]$command.Source }
        }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            Write-Warning "Python candidate does not exist: $candidate"
            continue
        }
        [string]$resolved = (Resolve-Path -LiteralPath $path).ProviderPath
        $windowed = Join-Path (Split-Path -Parent $resolved) 'pythonw.exe'
        if (-not (Test-Path -LiteralPath $windowed -PathType Leaf)) {
            Write-Warning "No sibling pythonw.exe for: $resolved"
            continue
        }
        try {
            # Capture native output so banners/errors cannot pollute our returned path.
            $dependencyOutput = & $resolved -c 'import psutil; import serial; import requests; import pygetwindow' 2>&1
            $dependencyExit = $LASTEXITCODE
        } catch {
            Write-Warning "Python dependency check failed for ${resolved}: $_"
            continue
        }
        if ($dependencyExit -ne 0) {
            Write-Warning "Python dependency check failed for ${resolved}: $dependencyOutput"
            continue
        }
        return $resolved
    }
    throw 'No usable Python with pythonw.exe and Agent dependencies. Use -Python with one complete interpreter path; install pc_script/requirements.txt in that environment.'
}
[string]$pythonExe = Resolve-AgentPython -Candidates @(Get-PythonCandidates -ExplicitPython $Python)
$pythonw = Join-Path (Split-Path -Parent $pythonExe) 'pythonw.exe'
Write-Host "Using Python: $pythonExe"
$StateDirectory = [IO.Path]::GetFullPath($StateDirectory)
$monitor = Join-Path $PSScriptRoot 'agent_monitor.py'
$shortcutPath = Join-Path $StartupDirectory 'NexusS1Agent.lnk'
$shell = New-Object -ComObject WScript.Shell
if (Test-Path -LiteralPath $shortcutPath) {
    if ($shell.CreateShortcut($shortcutPath).Description -ne $owner) {
        throw 'An unrelated NexusS1Agent.lnk already exists; refusing to overwrite it.'
    }
}
$configPath = Join-Path $StateDirectory 'config.json'
if (Test-Path -LiteralPath $configPath) {
    $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    if ($config.owner -ne $owner) { throw 'Unknown state directory; refusing to overwrite configuration.' }
}
& $pythonExe $monitor --state-dir $StateDirectory --stop
if ($LASTEXITCODE -ne 0) { throw 'Previous monitor did not stop; no replacement was installed.' }
$arguments = @('--state-dir', $StateDirectory, '--configure')
if ($SerialNumber) { $arguments += @('--serial', $SerialNumber) }
& $pythonExe $monitor @arguments
if ($LASTEXITCODE -ne 0) { throw 'USB enrollment failed; no shortcut was installed.' }
New-Item -ItemType Directory -Path $StartupDirectory -Force | Out-Null
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $pythonw
$shortcut.Arguments = '"{0}" --state-dir "{1}"' -f $monitor, $StateDirectory
$shortcut.WorkingDirectory = $PSScriptRoot
$shortcut.Description = $owner
$shortcut.WindowStyle = 7
$shortcut.Save()
if (-not $SkipStart) {
    Start-Process -FilePath $pythonw -ArgumentList $shortcut.Arguments -WorkingDirectory $PSScriptRoot -WindowStyle Hidden
}
Write-Host "Installed current-user Startup shortcut: $shortcutPath"
Write-Host "Logs/config: $StateDirectory. Keep the project and Python at these paths."
Write-Host 'Login/device/CDC behavior still requires physical acceptance.'
