[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "Tune")]
    [string]$Configuration = "Debug",

    [string]$DataRoot = $env:SHAR_DATA_ROOT,

    [string]$VcpkgRoot = $env:VCPKG_ROOT,

    [switch]$Clean,
    [switch]$CheckOnly,
    [switch]$SkipPrerequisiteCheck,
    [switch]$Run
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = $PSScriptRoot
$sourceRoot = Join-Path $repoRoot "src"
$buildPath = Join-Path $sourceRoot "srr2\SRR2.vcxproj"
$msbuildConfiguration = "$($Configuration)Windows"
$platform = "x64"
$executablePath = Join-Path $repoRoot "build\WINDOWS-bin-x64-$msbuildConfiguration\SRR2.exe"

function Find-VcpkgRoot {
    param([string]$RequestedRoot)
    $candidates = @()
    if ($RequestedRoot) { $candidates += $RequestedRoot }
    $candidates += (Join-Path $repoRoot "tools\vcpkg")
    if ($HOME) { $candidates += (Join-Path $HOME "vcpkg") }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $candidate "vcpkg.exe") -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

$VcpkgRoot = Find-VcpkgRoot $VcpkgRoot

$requiredBuildPaths = @($buildPath, (Join-Path $sourceRoot "srr2\SRR2.vcxproj"))
$missingBuildPaths = @($requiredBuildPaths | Where-Object { -not (Test-Path -LiteralPath $_) })

$msbuildPath = $null
$programFilesX86 = [Environment]::GetFolderPath("ProgramFilesX86")
$vswherePath = Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path -LiteralPath $vswherePath) {
    $visualStudioPath = & $vswherePath -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -eq 0 -and $visualStudioPath) {
        $msbuildCandidate = Join-Path $visualStudioPath "MSBuild\Current\Bin\MSBuild.exe"
        if (Test-Path -LiteralPath $msbuildCandidate) { $msbuildPath = $msbuildCandidate }
    }
}

if ($missingBuildPaths.Count -gt 0) {
    Write-Host "Missing build paths:"; $missingBuildPaths | ForEach-Object { Write-Host "  - $_" }
}
if (-not $VcpkgRoot) { Write-Host "vcpkg.exe not found" }
if (-not $msbuildPath) { Write-Host "MSBuild not found" }

$hasErrors = ($missingBuildPaths.Count -gt 0) -or (-not $VcpkgRoot) -or (-not $msbuildPath)
if ($hasErrors -and -not $SkipPrerequisiteCheck) { exit 2 }

if ($CheckOnly) {
    if ($hasErrors) { exit 2 }
    Write-Host "Prerequisites OK. DataRoot=$DataRoot Vcpkg=$VcpkgRoot"
    exit 0
}

if (-not $msbuildPath) { throw "MSBuild required" }
if (-not $VcpkgRoot) { throw "vcpkg required" }

$buildTarget = if ($Clean) { "Rebuild" } else { "Build" }
$msbuildArguments = @(
    $buildPath,
    "/m",
    "/t:$buildTarget",
    "/p:Configuration=$msbuildConfiguration",
    "/p:Platform=$platform",
    "/p:WindowsTargetPlatformVersion=10.0.22621.0"
)

& $msbuildPath $msbuildArguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Run -and (Test-Path -LiteralPath $executablePath)) {
    Push-Location $DataRoot
    try { & $executablePath; exit $LASTEXITCODE }
    finally { Pop-Location }
}
