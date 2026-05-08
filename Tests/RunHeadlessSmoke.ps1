# Smoke test for ShaderLabHeadless.exe.
#
# Renders the test_cli_basic.json fixture's Gamut Source node to a PNG,
# verifies the binary exits cleanly and produces a valid PNG file, then
# cleans up. Pass = exit code 0 and PNG file exists with the PNG magic
# header (89 50 4E 47 ...). Failure = non-zero exit.

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
    $exe = Join-Path $Repo "$Platform\$Configuration\ShaderLabHeadless\ShaderLabHeadless.exe"
    $fixture = Join-Path $Repo 'Tests\fixtures\test_cli_basic.json'
    $out = Join-Path $env:TEMP "shaderlab_headless_smoke_$([guid]::NewGuid().ToString('N')).png"

    if (-not (Test-Path $exe)) {
        Write-Error "Headless exe not built: $exe"
        exit 1
    }
    if (-not (Test-Path $fixture)) {
        Write-Error "Fixture missing: $fixture"
        exit 1
    }

    Write-Host "Rendering $fixture node 1 -> $out"
    & $exe --graph $fixture --node 1 --output $out --width 128 --height 128 --adapter warp
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Headless exited $LASTEXITCODE"
        if (Test-Path $out) { Remove-Item $out }
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $out)) {
        Write-Error "PNG not produced at $out"
        exit 1
    }

    $bytes = [System.IO.File]::ReadAllBytes($out)
    if ($bytes.Length -lt 8 -or
        $bytes[0] -ne 0x89 -or $bytes[1] -ne 0x50 -or
        $bytes[2] -ne 0x4E -or $bytes[3] -ne 0x47) {
        Remove-Item $out
        Write-Error "Output file is not a valid PNG (bad magic header)"
        exit 1
    }

    Write-Host "PASS: PNG size $($bytes.Length) bytes, valid header"
    Remove-Item $out

    # ---- Pixel-region readback mode (FP32 RGBA) ----------------------------
    # Validates the engine-side Rendering::ReadPixelRegion helper used by
    # MCP read_pixel_region. Output format: 8-byte header (uint32 W,
    # uint32 H, little-endian) then floats[W*H*4].
    $bin = Join-Path $env:TEMP "shaderlab_headless_pixels_$([guid]::NewGuid().ToString('N')).bin"
    Write-Host "Pixel readback: 4x4 region from node 1 -> $bin"
    & $exe --graph $fixture --node 1 --pixels 0,0,4,4 --output $bin --adapter warp
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Pixel readback exited $LASTEXITCODE"
        if (Test-Path $bin) { Remove-Item $bin }
        exit $LASTEXITCODE
    }
    if (-not (Test-Path $bin)) {
        Write-Error "Pixel binary not produced at $bin"
        exit 1
    }
    $pbytes = [System.IO.File]::ReadAllBytes($bin)
    $expectedSize = 8 + 4 * 4 * 4 * 4  # header + W*H*RGBA*4-byte-floats
    if ($pbytes.Length -ne $expectedSize) {
        Remove-Item $bin
        Write-Error "Pixel blob size $($pbytes.Length) != expected $expectedSize"
        exit 1
    }
    $w = [BitConverter]::ToUInt32($pbytes, 0)
    $h = [BitConverter]::ToUInt32($pbytes, 4)
    if ($w -ne 4 -or $h -ne 4) {
        Remove-Item $bin
        Write-Error "Pixel header W=$w H=$h, expected W=4 H=4"
        exit 1
    }
    Write-Host "PASS: pixel blob $($pbytes.Length) bytes, W=$w H=$h"
    Remove-Item $bin

    # ---- Script batch mode (--script) -------------------------------------
    # Validates the standard MCP workflow for ad-hoc analysis: insert a
    # Luminance Statistics node, connect upstream, evaluate, read fields,
    # mutate upstream parameter, re-evaluate, read fields again. The 2.5x
    # luminance set-property invariant catches:
    #   * graph mutation routing (add-node, connect, set-property)
    #   * ProcessDeferredCompute dispatching D3D11 compute analysis nodes
    #   * dirty propagation through the evaluator
    #   * /analysis/{id} returning the freshly-populated typed fields
    #   * shorthand op -> route translation in RunScript
    #
    # Fixture has Gamut Source as node id=1 and nextId=2, so the new
    # Luminance Statistics node will be id=2 deterministically.
    $scriptText = @'
{
  "steps": [
    { "method": "POST", "path": "/graph/add-node", "body": {"effectName":"Luminance Statistics"} },
    { "method": "POST", "path": "/graph/connect", "body": {"srcId":1,"srcPin":0,"dstId":2,"dstPin":0} },
    { "op": "render" },
    { "op": "analysis", "nodeId": 2 },
    { "op": "set-property", "nodeId": 1, "key": "Luminance", "value": 200.0 },
    { "op": "render" },
    { "op": "analysis", "nodeId": 2 }
  ]
}
'@
    $scriptPath = Join-Path $env:TEMP "shaderlab_smoke_script_$([guid]::NewGuid().ToString('N')).json"
    $scriptOut  = Join-Path $env:TEMP "shaderlab_smoke_script_out_$([guid]::NewGuid().ToString('N')).json"
    Set-Content -Path $scriptPath -Value $scriptText -Encoding UTF8
    Write-Host "Script batch: 7 steps -> $scriptOut"
    & $exe --graph $fixture --script $scriptPath --script-output $scriptOut --adapter warp
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Script mode exited $LASTEXITCODE"
        if (Test-Path $scriptPath) { Remove-Item $scriptPath }
        if (Test-Path $scriptOut)  { Remove-Item $scriptOut }
        exit $LASTEXITCODE
    }
    if (-not (Test-Path $scriptOut)) {
        Write-Error "Script output not produced at $scriptOut"
        exit 1
    }
    $doc = Get-Content $scriptOut -Raw | ConvertFrom-Json
    if ($doc.stepCount -ne 7 -or $doc.results.Count -ne 7) {
        Remove-Item $scriptPath, $scriptOut
        Write-Error "Script result count $($doc.results.Count) != 7"
        exit 1
    }
    foreach ($i in 0..6) {
        if ($doc.results[$i].status -ne 200) {
            Remove-Item $scriptPath, $scriptOut
            Write-Error "Step $i did not return 200 (got $($doc.results[$i].status))"
            exit 1
        }
    }
    # Pull Mean field from the Luminance Statistics analysis output before
    # and after the set-property. Doubling the source Luminance (80 -> 200)
    # should produce a 2.5x rise in the Mean nit value.
    $meanBefore = ($doc.results[3].body.fields | Where-Object name -eq 'Mean').value[0]
    $meanAfter  = ($doc.results[6].body.fields | Where-Object name -eq 'Mean').value[0]
    if ($meanBefore -le 0 -or $meanAfter -le 0) {
        Remove-Item $scriptPath, $scriptOut
        Write-Error "Mean values were zero (before=$meanBefore, after=$meanAfter)"
        exit 1
    }
    $ratio = $meanAfter / $meanBefore
    if ([math]::Abs($ratio - 2.5) -gt 0.05) {
        Remove-Item $scriptPath, $scriptOut
        Write-Error "Mean ratio $ratio not ~2.5 (set-property -> Luminance Statistics invariant broken)"
        exit 1
    }
    Write-Host "PASS: script batch ratio $('{0:N3}' -f $ratio) ~ 2.5 (graph-node analysis end-to-end)"
    Remove-Item $scriptPath, $scriptOut

    exit 0
}
finally {
    Pop-Location
}
