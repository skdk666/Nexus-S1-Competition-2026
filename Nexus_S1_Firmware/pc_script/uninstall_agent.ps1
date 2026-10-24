param(
    [string]$Python = 'python',
    [string]$StateDirectory = (Join-Path $env:LOCALAPPDATA 'NexusS1Agent'),
    [string]$StartupDirectory = [Environment]::GetFolderPath('Startup')
)
$ErrorActionPreference = 'Stop'
$owner = 'NexusS1AgentMonitor/v1'
$shortcutPath = Join-Path $StartupDirectory 'NexusS1Agent.lnk'
$configPath = Join-Path $StateDirectory 'config.json'
$shell = New-Object -ComObject WScript.Shell
if (Test-Path -LiteralPath $shortcutPath) {
    if ($shell.CreateShortcut($shortcutPath).Description -ne $owner) {
        throw 'Unrelated shortcut; refusing to remove it.'
    }
}
if (Test-Path -LiteralPath $configPath) {
    $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    if ($config.owner -ne $owner) { throw 'Unknown state directory; refusing to stop/remove anything.' }
}
& $Python (Join-Path $PSScriptRoot 'agent_monitor.py') --state-dir $StateDirectory --stop
if ($LASTEXITCODE -ne 0) { throw 'Monitor has not stopped; shortcut/config retained for inspection.' }
if (Test-Path -LiteralPath $shortcutPath) { Remove-Item -LiteralPath $shortcutPath }
Write-Host 'Removed only the owned current-user Startup shortcut; monitor/child stopped.'
Write-Host "Configuration and logs retained at $StateDirectory. No service, registry or scheduled task was changed."
