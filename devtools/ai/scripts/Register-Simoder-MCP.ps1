<#
.SYNOPSIS
Registers the optional Simoder MCP stdio server in a Codex config file.
.DESCRIPTION
Writes only a clearly delimited Simoder-managed block. Existing user-owned
Simoder MCP tables are rejected instead of being overwritten.
#>
[CmdletBinding()]
param(
    [ValidateSet('Project', 'User')]
    [string]$Scope = 'Project',

    [string]$ProjectPath = (Get-Location).Path
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function ConvertTo-TomlBasicString {
    <# /** Escapes a Windows path for a TOML basic string. */ #>
    param([Parameter(Mandatory)][string]$Value)
    return $Value.Replace('\', '\\').Replace('"', '\"')
}

function Resolve-CodexConfigPath {
    <# /** Resolves the selected Codex configuration scope without mutating it. */ #>
    if ($Scope -eq 'Project') {
        $resolvedProject = (Resolve-Path -LiteralPath $ProjectPath).Path
        return Join-Path $resolvedProject '.codex\config.toml'
    }

    $codexRoot = if ([string]::IsNullOrWhiteSpace($env:CODEX_HOME)) {
        Join-Path $env:USERPROFILE '.codex'
    } else {
        $env:CODEX_HOME
    }
    return Join-Path $codexRoot 'config.toml'
}

$serverPath = Join-Path $PSScriptRoot 'Simoder.McpServer.exe'
if (-not (Test-Path -LiteralPath $serverPath -PathType Leaf)) {
    throw "Simoder.McpServer.exe was not found beside this registration script: $serverPath"
}

$configPath = Resolve-CodexConfigPath
$configDirectory = Split-Path -Parent $configPath
if (-not (Test-Path -LiteralPath $configDirectory)) {
    New-Item -ItemType Directory -Path $configDirectory | Out-Null
}

$beginMarker = '# BEGIN SIMODER MCP - managed by Register-Simoder-MCP.ps1'
$endMarker = '# END SIMODER MCP'
$escapedServerPath = ConvertTo-TomlBasicString -Value (Resolve-Path -LiteralPath $serverPath).Path
$escapedWorkingDirectory = ConvertTo-TomlBasicString -Value $PSScriptRoot
$managedBlock = @"
$beginMarker
[mcp_servers.simoder]
command = "$escapedServerPath"
cwd = "$escapedWorkingDirectory"
startup_timeout_sec = 60
tool_timeout_sec = 310
enabled = true
required = false
default_tools_approval_mode = "writes"
$endMarker
"@

$existing = if (Test-Path -LiteralPath $configPath) {
    Get-Content -Raw -LiteralPath $configPath
} else {
    ''
}

$managedPattern = '(?ms)^' + [regex]::Escape($beginMarker) + '.*?^' + [regex]::Escape($endMarker) + '\s*'
if ($existing -notmatch $managedPattern -and $existing -match '(?m)^\s*\[mcp_servers\.simoder\]\s*$') {
    throw "A non-managed [mcp_servers.simoder] table already exists in $configPath. Remove or rename it manually."
}

$withoutManagedBlock = [regex]::Replace($existing, $managedPattern, '').TrimEnd()
$newContent = if ([string]::IsNullOrWhiteSpace($withoutManagedBlock)) {
    $managedBlock.Trim() + [Environment]::NewLine
} else {
    $withoutManagedBlock + [Environment]::NewLine + [Environment]::NewLine + $managedBlock.Trim() + [Environment]::NewLine
}
[System.IO.File]::WriteAllText($configPath, $newContent, [System.Text.UTF8Encoding]::new($false))
Write-Host "Registered Simoder MCP in: $configPath"
Write-Host 'Restart the Codex host or extension before using the new server.'
