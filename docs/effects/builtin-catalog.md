# ShaderLab Built-in Effects

ShaderLab ships with **20+ built-in ShaderLab effects** implemented in `Effects/ShaderLabEffects.h/.cpp`, on top of the **40+ wrapped built-in D2D effects** in `Effects/EffectRegistry.cpp`. Each ShaderLab effect has its HLSL embedded as a string constant, compiled at first use via `ShaderCompiler`, and shares a common color math library (BT.709 / BT.2020 / DCI-P3 matrices, PQ / HLG transfer functions, CIE xy conversions, ICtCp). Every effect is versioned with `effectId` and `effectVersion` so saved graphs can be upgraded in place — see [Effect Versioning System](#effect-versioning-system).

## Analysis Effects

| Effect | Type | Description |
|--------|------|-------------|
| Luminance Heatmap | Pixel Shader | False-color BT.709 luminance overlay (Turbo / Inferno gradients). |
| Nit Map | Pixel Shader | Display-referred nit visualization with configurable luminance bands. |
| Gamut Highlight | Pixel Shader | Highlights pixels outside a target gamut (sRGB / P3 / BT.2020 / current monitor). |
| CIE Histogram | Compute Shader | 2D histogram of pixel chromaticity on the CIE xy plane. |
| CIE Chromaticity Plot | Pixel Shader | Plots image pixels on a CIE 1931 xy diagram with gamut triangle overlays. |
| Vectorscope | Pixel Shader | YCbCr vectorscope with graticule. |
| Waveform Monitor | Pixel Shader | RGB parade waveform display. |
| Delta E Comparator | Pixel Shader | Two-input CIEDE2000 perceptual difference map. |
| Gamut Coverage | Pixel Shader | Percentage of target gamut volume covered by input. |
| Split Comparison | Pixel Shader | Two-input wipe comparator with adjustable split & divider. |
| Effect | Type | Description |
|--------|------|-------------|
| Channel Statistics | D3D11 Compute | Per-channel R/G/B/A min / max / mean / median / P95 / nonzero%. |
| Luminance Statistics | D3D11 Compute | BT.709 Y stats with HDR-aware extras (log-spaced histogram, AvgLog, ClippedFraction). |
| Chromaticity Statistics | D3D11 Compute | ICtCp Ct/Cp stats (mean / max chroma, mean hue). |

## Color Processing Effects

| Effect | Type | Description |
|--------|------|-------------|
| Gamut Map | Pixel Shader | CIE xy gamut mapping: Clip / Nearest / Compress to White / Fit Gamut. |
| ICtCp Gamut Map | Pixel Shader | Perceptual gamut mapping in BT.2100 ICtCp space: Nearest on Shell / Compress to Neutral / Fit to Shell. (Renamed from `Perceptual Gamut Map` in v1.3.8 — old graphs load via legacy alias.) |

## Tone Mapping Effects (ICtCp suite)

A growing set of HDR↔SDR operators built around BT.2100 ICtCp. The key property: I (intensity) is decoupled from Ct/Cp (chromaticity), so compressing or expanding I alone preserves hue and saturation by construction. This is the design thesis of the suite: things that need to be carefully done in linear RGB or CIE xyY (per-channel hue shifts, gamut excursions, chromaticity drift) reduce to one-line manipulations of I in ICtCp.

| Effect | Type | Description |
|--------|------|-------------|
| ICtCp Round-Trip Validator | Pixel Shader | Diagnostic: outputs `\|scRGB→ICtCp→scRGB - in\| × Gain`. Should render black on a correct image; non-zero output indicates a bug in the ICtCp conversion. |
| ICtCp Tone Map (HDR → SDR) | Pixel Shader | I-channel Reinhard compression. Source / target peaks specified in nits; Ct/Cp pass through unchanged. |
| ICtCp Inverse Tone Map (SDR → HDR) | Pixel Shader | Mirror of the above; inverse Reinhard expands SDR-anchored content into the HDR peak. |

Each tone-mapping effect exposes its nit-target as a regular numeric parameter (`TargetPeakNits`, `SourcePeakNits`, etc.) — wire it from `working_space.SdrWhiteNits` / `PeakNits` to track the OS slider or simulated profile automatically. Future variants (BT.2390, hue-preserving ACES, adaptive) will live in this same subcategory and follow the same convention.

## Source / Generator Effects

| Effect | Type | Description |
|--------|------|-------------|
| Gamut Source | Pixel Shader | Swept gamut fill for a target color space. |
| ICtCp Boundary | Pixel Shader | ICtCp gamut boundary visualization. |
| Color Checker | Pixel Shader | Macbeth ColorChecker pattern with accurate sRGB patches. |
| Zone Plate | Pixel Shader | Sine-wave zone plate for resolution / aliasing testing. |
| Gradient Generator | Pixel Shader | Configurable linear / radial gradient with HDR range. |
| HDR Test Pattern | Pixel Shader | Luminance step wedge from 0 to 10,000 nits. |
| Image Source | Host (WIC) | Static image file (PNG / JPEG / JXR / EXR / HDR). HDR formats decode as FP16 BT.709 scRGB. |
| Video Source | Host (Media Foundation) | Decodes a video file frame-by-frame; advances under the animation timeline / Clock node. |
| DXGI Desktop Duplication | Host (DXGI) | Live capture of an entire monitor via `IDXGIOutputDuplication`. Submenu lists each adapter / output. Outputs raw FP16 scRGB so SDR monitors land at scRGB 1.0 ≈ 80 nits and HDR monitors preserve their full range. |
| Windows Graphics Capture | Host (Windows.Graphics.Capture) | Live capture of an arbitrary window or monitor via the standard WinUI graphics-capture picker. Same FP16 scRGB output convention as DXGI duplication. |

## Data / Parameter Nodes

| Node | Description |
|------|-------------|
| Float Parameter | Continuous slider; teal node with inline canvas slider. |
| Integer Parameter | Discrete slider. |
| Toggle Parameter | Boolean on / off. |
| Gamut Parameter | Gamut-id selector (sRGB / DCI-P3 / BT.2020 / Custom). |
| Clock | Time source: outputs `Time` (seconds) and `Progress` (0–1 over a configurable duration). Drives the animation system. |
| Numeric Expression | Single configurable math node powered by ExprTk; user-supplied formula evaluated against dynamic float inputs `A..Z`. Replaces the older Add / Subtract / Multiply / Divide / Min / Max nodes. See [below](#numeric-expression-node-exprtk). |
| Random | Takes a single `Seed` float input and outputs a deterministic, well-mixed `Result` in `[0, 1)`. The output is a pure function of the seed (SplitMix64-style integer mixer on the float's bit pattern), so identical seeds always reproduce identical values and any change — e.g. a tick from an upstream Clock or Numeric Expression — yields a fresh random number. |
| Working Space | Strict sink (no input pins, no output image pin) that mirrors the active display profile from the top-bar profile selector — live OS-reported caps or whatever simulated preset / ICC the user has applied. Exposes 14 typed analysis output fields: `ActiveColorMode` (0=SDR, 1=WCG/ACM, 2=HDR), `HdrSupported`, `HdrUserEnabled`, `WcgSupported`, `WcgUserEnabled`, `IsSimulated`, `SdrWhiteNits`, `PeakNits`, `MinNits`, `MaxFullFrameNits`, plus the four CIE-xy primaries `RedPrimary` / `GreenPrimary` / `BluePrimary` / `WhitePoint` (each Float2). Bind any downstream property to these fields via the property-binding system to drive an effect from the live working space — e.g. wire a tone-mapper's peak-nits to `working_space.PeakNits` and it will track Display Settings or simulated profile changes automatically without touching the graph. |


---

Back to [docs/](../README.md) • [Repo root](../../README.md)