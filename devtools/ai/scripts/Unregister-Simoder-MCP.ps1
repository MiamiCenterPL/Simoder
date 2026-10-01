<#
.SYNOPSIS
Removes only the Simoder-managed MCP block from a Codex config file.
#>
[CmdletBinding()]
param(
    [ValidateSet('Project', 'User')]
    [string]$Scope = 'Project',

    [string]$ProjectPath = (Get-Location).Path
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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

$configPath = Resolve-CodexConfigPath
if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
    Write-Host "No Codex config exists at: $configPath"
    exit 0
}

$beginMarker = '# BEGIN SIMODER MCP - managed by Register-Simoder-MCP.ps1'
$endMarker = '# END SIMODER MCP'
$managedPattern = '(?ms)^' + [regex]::Escape($beginMarker) + '.*?^' + [regex]::Escape($endMarker) + '\s*'
$existing = Get-Content -Raw -LiteralPath $configPath
$newContent = [regex]::Replace($existing, $managedPattern, '').TrimEnd()
if ($newContent -eq $existing.TrimEnd()) {
    Write-Host "No Simoder-managed MCP block was found in: $configPath"
    exit 0
}

$serialized = if ([string]::IsNullOrWhiteSpace($newContent)) { '' } else { $newContent + [Environment]::NewLine }
[System.IO.File]::WriteAllText($configPath, $serialized, [System.Text.UTF8Encoding]::new($false))
Write-Host "Unregistered Simoder MCP from: $configPath"
