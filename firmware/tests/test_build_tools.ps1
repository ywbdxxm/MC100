[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$firmwareRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$shell = (Get-Process -Id $PID).Path
$buildScript = Join-Path $firmwareRoot 'tools/build.ps1'
$hostScript = Join-Path $firmwareRoot 'tools/test-host.ps1'
$localConfig = Join-Path $firmwareRoot 'sdkconfig'
$before = if (Test-Path -LiteralPath $localConfig) { (Get-FileHash -LiteralPath $localConfig).Hash } else { $null }

function Assert-Rejected([string]$Script, [string[]]$Arguments, [string]$Message) {
    $output = & $shell -NoProfile -File $Script @Arguments 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -or $output -notmatch [regex]::Escape($Message)) {
        throw "Expected nonzero rejection containing '$Message'; received: $output"
    }
    Write-Host "PASS rejection: $Message"
}

Assert-Rejected $buildScript @('-Profile', 'unknown') 'ValidateSet'
Assert-Rejected $buildScript @('-Profile', 'release') 'Release is not available'
Assert-Rejected $buildScript @('-Clean', '-OutputDirectory', '..') 'Target output must be a child directory'
Assert-Rejected $buildScript @('-Clean', '-OutputDirectory', 'out') 'Target output must be a child directory'
Assert-Rejected $hostScript @('-Clean', '-OutputDirectory', '..') 'Host output must be a child directory'
Assert-Rejected $hostScript @('-Clean', '-OutputDirectory', 'out') 'Host output must be a child directory'

$keepDirectory = Join-Path $firmwareRoot 'out/tool-contract-keep'
$linkDirectory = Join-Path $firmwareRoot 'out/tool-contract-link'
foreach ($path in @($keepDirectory, $linkDirectory)) {
    if (Test-Path -LiteralPath $path) { throw "Fixture already exists; refusing to alter it: $path" }
}
New-Item -ItemType Directory -Path $keepDirectory | Out-Null
try {
    $savedIdf = $env:IDF_PATH
    $savedTools = $env:IDF_TOOLS_PATH
    $savedPython = $env:IDF_PYTHON_ENV_PATH
    try {
        $env:IDF_PATH = $firmwareRoot
        $env:IDF_TOOLS_PATH = $firmwareRoot
        $env:IDF_PYTHON_ENV_PATH = $firmwareRoot
        Assert-Rejected $buildScript @('-Clean', '-OutputDirectory', 'out/tool-contract-keep') 'ESP-IDF revision does not match'
    } finally {
        $env:IDF_PATH = $savedIdf
        $env:IDF_TOOLS_PATH = $savedTools
        $env:IDF_PYTHON_ENV_PATH = $savedPython
    }
    if (-not (Test-Path -LiteralPath $keepDirectory)) { throw 'Invalid SDK validation deleted the existing output.' }

    if ($env:OS -eq 'Windows_NT') {
        New-Item -ItemType Junction -Path $linkDirectory -Target $keepDirectory | Out-Null
    } else {
        New-Item -ItemType SymbolicLink -Path $linkDirectory -Target $keepDirectory | Out-Null
    }
    Assert-Rejected $buildScript @('-Clean', '-OutputDirectory', 'out/tool-contract-link/missing-leaf') 'Output paths must not contain links'
    Assert-Rejected $hostScript @('-Clean', '-OutputDirectory', 'out/tool-contract-link/missing-leaf') 'Output paths must not contain links'
    if (-not (Test-Path -LiteralPath $keepDirectory)) { throw 'Linked output validation changed the link target.' }
} finally {
    if (Test-Path -LiteralPath $linkDirectory) { Remove-Item -LiteralPath $linkDirectory -Force }
    if (Test-Path -LiteralPath $keepDirectory) { Remove-Item -LiteralPath $keepDirectory -Force }
}

$after = if (Test-Path -LiteralPath $localConfig) { (Get-FileHash -LiteralPath $localConfig).Hash } else { $null }
if ($before -ne $after) { throw 'Build-tool rejection changed the user sdkconfig.' }
Write-Host 'PASS build-tool safety: invalid profiles/paths/SDK/link ancestors rejected; output and user sdkconfig preserved.'
