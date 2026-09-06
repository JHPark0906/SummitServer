#Requires -Version 5.1
<#
.SYNOPSIS
Builds and tests SummitServer with the Visual Studio 2026 x64 preset.
.DESCRIPTION
Defaults to Debug and Release. Builds every requested configuration before
running tests, and fails if an integration test is not registered.
.PARAMETER CMakePath
Optional cmake.exe path. Otherwise uses PATH or the Visual Studio 2026 bundle.
.PARAMETER ServerCoreSourceDir
Optional ServerCore checkout. The default is the sibling ServerCore directory.
#>
[CmdletBinding()]
param(
    [ValidateSet('All', 'Debug', 'Release')]
    [string]$Configuration = 'All',
    [string]$ServerCoreSourceDir,
    [string]$CMakePath,
    [ValidateRange(1, 64)]
    [int]$Parallel = 8
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $CMakePath) {
    $onPath = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($onPath) { $CMakePath = $onPath.Source }
    else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $installation = & $vswhere -latest -version '[18.0,19.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($LASTEXITCODE -eq 0 -and $installation) {
                $CMakePath = Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
            }
        }
    }
}
if (-not $CMakePath -or -not (Test-Path -LiteralPath $CMakePath -PathType Leaf)) {
    throw 'CMake was not found. Install the Visual Studio 2026 C++ tools or pass -CMakePath.'
}
$CMakePath = (Resolve-Path -LiteralPath $CMakePath).Path
$ctestPath = Join-Path (Split-Path -Parent $CMakePath) 'ctest.exe'
if (-not (Test-Path -LiteralPath $ctestPath -PathType Leaf)) { throw 'ctest.exe must be installed beside cmake.exe.' }
if (-not $ServerCoreSourceDir) { $ServerCoreSourceDir = Join-Path $repositoryRoot '../ServerCore' }
$ServerCoreSourceDir = (Resolve-Path -LiteralPath $ServerCoreSourceDir).Path

function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw ('Build verification failed with exit code {0}.' -f $LASTEXITCODE) }
}

$configurations = @('Debug', 'Release')
if ($Configuration -ne 'All') { $configurations = @($Configuration) }
Push-Location -LiteralPath $repositoryRoot
try {
    Write-Host ('Source: {0}' -f $repositoryRoot)
    Write-Host ('ServerCore: {0}' -f $ServerCoreSourceDir)
    Invoke-Checked $CMakePath @('--preset', 'vs', "-DSERVERCORE_SOURCE_DIR=$ServerCoreSourceDir",
        '-DSERVERCORE_BUILD_TESTS=OFF', '-DSUMMITSERVER_BUILD_TESTS=ON', '-DSUMMITSERVER_BUILD_LOAD_TOOL=ON')
    $requiredTests = @('SummitLoadToolSelfCheck', 'SummitServerBackend', 'SummitServer.Aoi',
        'SummitServerConsoleInput', 'SummitServerLoopback', 'SummitServerOptions',
        'SummitServerAnnouncements', 'SummitServer.Udp')
    $inventoryJson = & $ctestPath --test-dir build/vs -C $configurations[0] --show-only=json-v1
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read the CTest inventory.' }
    $inventory = ($inventoryJson -join "`n") | ConvertFrom-Json
    foreach ($testName in $requiredTests) {
        if ($testName -notin @($inventory.tests | ForEach-Object { $_.name })) {
            throw "Required CTest item is missing: $testName. Check the PowerShell installation and build options."
        }
    }
    foreach ($configurationName in $configurations) {
        Invoke-Checked $CMakePath @('--build', '--preset', $configurationName.ToLowerInvariant(), '--parallel', "$Parallel")
    }
    foreach ($configurationName in $configurations) {
        $reportPath = Join-Path $repositoryRoot "build/vs/ctest-$($configurationName.ToLowerInvariant()).xml"
        Invoke-Checked $ctestPath @('--preset', $configurationName.ToLowerInvariant(), '--no-tests=error',
            '--output-junit', $reportPath)
    }
    Write-Host ('PASS: built and tested {0}.' -f ($configurations -join ', '))
}
finally { Pop-Location }
