# ShaderLab — first-time setup script
#
# Runs the per-build helpers that MSBuild would otherwise run on first
# build, plus a NuGet restore and an optional Debug|x64 smoke build, so
# a fresh clone is ready to F5 in one command.
#
# Usage:
#     .\Bootstrap.ps1            # cert + ExprTk + restore (no build)
#     .\Bootstrap.ps1 -Build     # the above + Debug|x64 smoke build

param(
    [switch]$Build = $false,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
$Repo = $PSScriptRoot
Push-Location $Repo
try {
    Write-Host "ShaderLab Bootstrap"
    Write-Host "==================="
    Write-Host "Repo:        $Repo"
    Write-Host "Configuration: $Configuration | Platform: $Platform"
    Write-Host ""

    # 1. Dev cert (signed F5 deploy needs it; release builds don't).
    Write-Host "[1/4] Ensuring dev signing certificate..."
    & "$Repo\scripts\EnsureDevCert.ps1" `
        -PfxPath "$Repo\ShaderLab_TemporaryKey.pfx" `
        -Password "shaderlab"

    # 2. ExprTk single-header download (Numeric Expression node).
    Write-Host "[2/4] Ensuring third_party/exprtk/exprtk.hpp..."
    & "$Repo\scripts\EnsureExprTk.ps1" -TargetDir "$Repo\third_party\exprtk"

    # 3. NuGet restore.
    #    The repo uses packages.config (NOT PackageReference). MSBuild's
    #    /t:Restore target on a .slnx silently no-ops on packages.config
    #    style ("Nothing to do. None of the projects specified contain
    #    packages to restore.") even though the manifest is right there.
    #    Use nuget.exe directly against the root packages.config -- that
    #    path explicitly supports packages.config and is what NuGet
    #    documents for this scenario. Falls back to msbuild if nuget.exe
    #    isn't on PATH.
    Write-Host "[3/4] Restoring NuGet packages..."
    $msbuild = "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
    if (-not (Test-Path $msbuild)) {
        $msbuild = 'msbuild'  # fall back to PATH
    }

    $nuget = Get-Command nuget.exe -ErrorAction SilentlyContinue
    if ($nuget) {
        & $nuget.Source restore "$Repo\packages.config" -PackagesDirectory "$Repo\packages" -NonInteractive
        if ($LASTEXITCODE -ne 0) {
            throw "NuGet restore failed (exit $LASTEXITCODE)"
        }
    } else {
        # MSBuild fallback path. Requires VS to be configured with the
        # NuGet packages.config restore feature; not the default in CI.
        & $msbuild ShaderLab.slnx /t:Restore /p:RestorePackagesConfig=true /p:Configuration=$Configuration /p:Platform=$Platform /v:minimal /m /nologo
        if ($LASTEXITCODE -ne 0) {
            throw "NuGet restore failed (exit $LASTEXITCODE)"
        }
    }

    # 4. Smoke build (optional, off by default).
    if ($Build) {
        Write-Host "[4/4] Smoke build ($Configuration|$Platform)..."
        & $msbuild ShaderLab.slnx /p:Configuration=$Configuration /p:Platform=$Platform /v:minimal /m /nologo
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed (exit $LASTEXITCODE)"
        }
        Write-Host "Smoke build OK."
    }
    else {
        Write-Host "[4/4] Skipping smoke build (use -Build to enable)."
    }

    Write-Host ""
    Write-Host "Bootstrap OK. Open ShaderLab.slnx in Visual Studio and F5 to deploy + run."
}
finally {
    Pop-Location
}
