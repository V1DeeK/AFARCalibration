# Asks for confirmation before irreversible shell commands.
# Wired to the beforeShellExecution hook, see .cursor/hooks.json.
# stdin: JSON with a "command" field. stdout: JSON with a "permission" field.
#
# This file is intentionally ASCII-only: Windows PowerShell 5.1 reads a .ps1
# as ANSI when there is no BOM, which corrupts non-ASCII source. All Russian
# wording lives in guard-shell.json and is read as UTF-8.

$ErrorActionPreference = 'Stop'

function Write-Decision {
    param([string]$Permission, [string]$User, [string]$Agent)

    $payload = [ordered]@{ permission = $Permission }
    if ($User)  { $payload.user_message  = $User }
    if ($Agent) { $payload.agent_message = $Agent }

    # Write UTF-8 bytes directly: PowerShell 5.1 would otherwise encode
    # redirected stdout using the console codepage.
    $json   = ($payload | ConvertTo-Json -Compress)
    $bytes  = [System.Text.Encoding]::UTF8.GetBytes($json)
    $stdout = [Console]::OpenStandardOutput()
    $stdout.Write($bytes, 0, $bytes.Length)
    $stdout.Flush()
    exit 0
}

try {
    $reader  = New-Object System.IO.StreamReader(
        [Console]::OpenStandardInput(), (New-Object System.Text.UTF8Encoding $false))
    $command = ($reader.ReadToEnd() | ConvertFrom-Json).command
} catch {
    Write-Decision 'allow'   # unreadable input must not block the agent
}

if (-not $command) { Write-Decision 'allow' }

try {
    $rulesPath = Join-Path $PSScriptRoot 'guard-shell.json'
    $config    = Get-Content -LiteralPath $rulesPath -Raw -Encoding UTF8 | ConvertFrom-Json
} catch {
    Write-Decision 'allow'
}

foreach ($rule in $config.rules) {
    if ($command -match $rule.pattern) {
        Write-Decision 'ask' `
            ($config.messages.user  -f $rule.reason) `
            ($config.messages.agent -f $rule.reason)
    }
}

Write-Decision 'allow'
