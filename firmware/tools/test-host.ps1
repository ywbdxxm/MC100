[CmdletBinding()]
param(
    [string]$OutputDirectory = 'out/host',
    [switch]$Clean,
    [switch]$Sanitizers,
    [switch]$FutureRuntimeTests
)

$ErrorActionPreference = 'Stop'
$firmwareRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$outputRoot = [IO.Path]::GetFullPath((Join-Path $firmwareRoot 'out'))
$buildDirectory = [IO.Path]::GetFullPath((Join-Path $firmwareRoot $OutputDirectory))
if (-not $buildDirectory.StartsWith($outputRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Host output must be a child directory of firmware/out.'
}
$cursorPath = $buildDirectory
while ($cursorPath -ne $firmwareRoot) {
    if (Test-Path -LiteralPath $cursorPath) {
        if ((Get-Item -LiteralPath $cursorPath).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Output paths must not contain links.'
        }
    }
    $cursorPath = [IO.Path]::GetDirectoryName($cursorPath)
}

foreach ($tool in @('cmake', 'ninja', 'ctest')) {
    if (-not (Get-Command $tool -CommandType Application -ErrorAction SilentlyContinue)) {
        throw "Missing $tool. Activate a host C11 compiler and CMake/Ninja environment; no tools are installed by this script."
    }
}
if ($Clean -and (Test-Path -LiteralPath $buildDirectory)) { Remove-Item -LiteralPath $buildDirectory -Recurse -Force }
$configureArgs = @('-S', (Join-Path $firmwareRoot 'host'), '-B', $buildDirectory, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug')
if ($Sanitizers) { $configureArgs += '-DMC100_ENABLE_SANITIZERS=ON' }
if ($FutureRuntimeTests) { $configureArgs += '-DMC100_ENABLE_FUTURE_RUNTIME_TESTS=ON' }
& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "Host configure failed ($LASTEXITCODE)." }
& cmake --build $buildDirectory
if ($LASTEXITCODE -ne 0) { throw "Host build failed ($LASTEXITCODE)." }
& ctest --test-dir $buildDirectory --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Host tests failed ($LASTEXITCODE)." }
