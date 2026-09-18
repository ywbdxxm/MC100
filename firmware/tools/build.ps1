[CmdletBinding()]
param(
    [ValidateSet('host', 'evt', 'release')]
    [string]$Profile = 'evt',
    [string]$OutputDirectory = 'out/target',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$firmwareRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if ($Profile -eq 'host') {
    & (Join-Path $PSScriptRoot 'test-host.ps1') -Clean:$Clean
    return
}
if ($Profile -eq 'release') { throw 'Release is not available: USB bench only; automatic VAD, recovery and full hardware qualification are not complete.' }

$outputRoot = [IO.Path]::GetFullPath((Join-Path $firmwareRoot 'out'))
$buildDirectory = [IO.Path]::GetFullPath((Join-Path $firmwareRoot $OutputDirectory))
if (-not $buildDirectory.StartsWith($outputRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Target output must be a child directory of firmware/out.'
}
# Validate every existing ancestor before removal or creation, including junctions.
$cursorPath = $buildDirectory
while ($cursorPath -ne $firmwareRoot) {
    if (Test-Path -LiteralPath $cursorPath) {
        if ((Get-Item -LiteralPath $cursorPath).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Output paths must not contain links.'
        }
    }
    $cursorPath = [IO.Path]::GetDirectoryName($cursorPath)
}

$lock = Get-Content -Raw -LiteralPath (Join-Path $firmwareRoot 'dependencies.lock.json') | ConvertFrom-Json
foreach ($name in @('IDF_PATH', 'IDF_TOOLS_PATH', 'IDF_PYTHON_ENV_PATH')) {
    $value = [Environment]::GetEnvironmentVariable($name)
    if ([string]::IsNullOrWhiteSpace($value) -or -not (Test-Path -LiteralPath $value -PathType Container)) {
        throw "Activate the project's ESP-IDF environment first: missing or invalid $name."
    }
}
$idfRoot = (Resolve-Path -LiteralPath $env:IDF_PATH).Path
$toolsRoot = (Resolve-Path -LiteralPath $env:IDF_TOOLS_PATH).Path
$pythonRoot = (Resolve-Path -LiteralPath $env:IDF_PYTHON_ENV_PATH).Path
$revision = & git -C $idfRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $revision -ne $lock.esp_idf.commit) { throw 'ESP-IDF revision does not match dependencies.lock.json.' }
$dirty = & git -C $idfRoot status --porcelain --untracked-files=no
if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'ESP-IDF tracked files must be clean.' }
$python = (Get-Command python -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$pythonIdentity = & $python -c 'import sys; print(sys.executable)'
if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF Python identity check failed.' }
$pythonIdentity = (Resolve-Path -LiteralPath $pythonIdentity).Path
if (-not $pythonIdentity.StartsWith($pythonRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Active Python is outside IDF_PYTHON_ENV_PATH.'
}
$idfScript = Join-Path $idfRoot 'tools/idf.py'
$idfVersion = & $python $idfScript --version
if ($LASTEXITCODE -ne 0 -or $idfVersion.Trim() -ne "ESP-IDF $($lock.esp_idf.version)") { throw 'ESP-IDF version check failed.' }
foreach ($tool in @('xtensa-esp32s3-elf-gcc', 'cmake', 'ninja')) {
    $toolPath = (Get-Command $tool -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    if (-not $toolPath.StartsWith($toolsRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$tool is outside IDF_TOOLS_PATH. Activate one consistent installation tuple."
    }
    & $toolPath --version | Select-Object -First 1
    if ($LASTEXITCODE -ne 0) { throw "$tool version check failed." }
}
$descriptionPath = Join-Path $buildDirectory 'project_description.json'
if ((Test-Path -LiteralPath $descriptionPath) -and -not $Clean) {
    $old = Get-Content -Raw -LiteralPath $descriptionPath | ConvertFrom-Json
    if ($old.idf_path -ne $idfRoot.Replace('\', '/') -or $old.target -ne $lock.target -or $old.git_revision -ne $lock.esp_idf.version) {
        throw 'Existing output belongs to a different SDK/target. Use a new output directory or -Clean.'
    }
}
if ($Clean -and (Test-Path -LiteralPath $buildDirectory)) { Remove-Item -LiteralPath $buildDirectory -Recurse -Force }
$sdkconfigPath = Join-Path $buildDirectory 'sdkconfig'
$defaultsPath = Join-Path $firmwareRoot 'sdkconfig.defaults'
Write-Host "Validated $idfVersion / $revision / $($lock.target)"
& $python $idfScript -C $firmwareRoot -B $buildDirectory -D "SDKCONFIG=$sdkconfigPath" -D "SDKCONFIG_DEFAULTS=$defaultsPath" -D "IDF_TARGET=$($lock.target)" build
if ($LASTEXITCODE -ne 0) { throw "Target build failed ($LASTEXITCODE)." }

$description = Get-Content -Raw -LiteralPath $descriptionPath | ConvertFrom-Json
if ($description.target -ne $lock.target -or $description.git_revision -ne $lock.esp_idf.version -or
    [IO.Path]::GetFullPath($description.idf_path) -ne $idfRoot -or
    [IO.Path]::GetFullPath($description.config_file) -ne $sdkconfigPath) {
    throw 'Build metadata does not match the validated SDK, target, and isolated configuration.'
}
$config = Get-Content -Raw -LiteralPath (Join-Path $buildDirectory 'config/sdkconfig.json') | ConvertFrom-Json
if ($config.ESPTOOLPY_FLASHSIZE -ne '8MB' -or $config.SPIRAM -or $config.ESP_DEFAULT_CPU_FREQ_MHZ -ne 80 -or
    -not $config.ESP_CONSOLE_USB_SERIAL_JTAG -or -not $config.ESP_BROWNOUT_DET -or
    -not $config.ESP_INT_WDT -or -not $config.ESP_TASK_WDT_EN -or -not $config.ESP_TASK_WDT_INIT -or
    $config.BT_ENABLED -or -not $config.PARTITION_TABLE_CUSTOM -or $config.PARTITION_TABLE_CUSTOM_FILENAME -ne 'partitions.csv') {
    throw 'Resolved configuration violates the MC100 infrastructure baseline.'
}
foreach ($component in @('esp_wifi', 'bt')) {
    if ($description.build_components -contains $component) { throw "Excluded component linked: $component" }
}
# FatFs -> SDSPI -> SPI has an unconditional private esp_psram dependency in
# this SDK. With CONFIG_SPIRAM=n only the MSPI shim compiles, not PSRAM support.
# Keep checking exact sources so an enabled PSRAM implementation cannot slip in.
if ($description.build_components -contains 'esp_psram') {
    $psramSources = @($description.build_component_info.esp_psram.sources)
    if ($config.SPIRAM -or $psramSources.Count -ne 1 -or
        [IO.Path]::GetFileName($psramSources[0]) -ne 'esp_psram_mspi.c') {
        throw 'PSRAM implementation must remain disabled for MC100 N8.'
    }
}
$partitionCsv = & $python (Join-Path $idfRoot 'components/partition_table/gen_esp32part.py') (Join-Path $buildDirectory 'partition_table/partition-table.bin')
if ($LASTEXITCODE -ne 0) { throw 'Generated partition table verification failed.' }
$partitions = @($partitionCsv | Where-Object { $_.Trim() -and -not $_.StartsWith('#') } | ConvertFrom-Csv -Header Name,Type,SubType,Offset,Size,Flags)
if ($partitions.Count -ne 3 -or
    $partitions[0].Name -ne 'nvs' -or $partitions[0].Type -ne 'data' -or $partitions[0].SubType -ne 'nvs' -or $partitions[0].Offset -ne '0x9000' -or $partitions[0].Size -ne '24K' -or
    $partitions[1].Name -ne 'phy_init' -or $partitions[1].Type -ne 'data' -or $partitions[1].SubType -ne 'phy' -or $partitions[1].Offset -ne '0xf000' -or $partitions[1].Size -ne '4K' -or
    $partitions[2].Name -ne 'factory' -or $partitions[2].Type -ne 'app' -or $partitions[2].SubType -ne 'factory' -or $partitions[2].Offset -ne '0x10000' -or $partitions[2].Size -ne '3M') {
    throw 'Generated partition table violates the MC100 layout.'
}
Write-Host 'MC100 target build verified; physical validation is recorded separately from compilation.'
