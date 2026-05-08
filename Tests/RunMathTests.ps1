# Runs the Phase-2 math test suite (HLSL color-math correctness via the
# ShaderTestBench compute harness). Uses WARP so it works on CI runners
# without a discrete GPU. Exits with the failure count, so non-zero
# means the suite is red.

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent $PSScriptRoot
Push-Location $Repo
try {
    $exe = Join-Path $Repo "$Platform\$Configuration\ShaderLabTests\ShaderLabTests.exe"
    if (-not (Test-Path $exe)) {
        Write-Host "Tests exe not built; building now..."
        $msbuild = "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
        if (-not (Test-Path $msbuild)) {
            $msbuild = 'msbuild'
        }
        & $msbuild ShaderLabTests.vcxproj /p:Configuration=$Configuration /p:Platform=$Platform /v:minimal /m /nologo
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    & $exe --adapter warp
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
