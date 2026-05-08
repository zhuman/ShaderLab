# ShaderLab

**HDR / WCG / SDR shader effect development & debugging tool**

A WinUI 3 desktop application (C++/WinRT) for developing, testing, and debugging Direct2D and Win2D shader effects with full HDR and wide color gamut support.

---

## Installing a Release Build

Release builds ship as **unsigned MSIX packages** — no signing certificate needed, but Developer Mode must be on.

1. Enable Developer Mode: *Settings → Privacy & security → For developers → Developer Mode*.
2. Download the architecture-matched zip from the GitHub Releases page:
   - `ShaderLab-<version>-x64.zip` for AMD64 / Intel
   - `ShaderLab-<version>-arm64.zip` for ARM64 (Snapdragon X / Surface Pro)
3. Extract and run:
   ```pwsh
   .\Install.ps1
   ```
4. Launch ShaderLab from the Start menu.

`Install.ps1` calls `Add-AppxPackage -AllowUnsigned`, which installs unsigned MSIX packages on systems with Developer Mode enabled (Windows 10 1903+ / Windows 11). The script installs the bundled dependency packages (Microsoft VCLibs, Windows App Runtime) for the host architecture first, then ShaderLab itself.

The release manifest carries the special OID `2.25.311729368913984317654407730594956997722=1` (Windows' "unsigned namespace") that allows `-AllowUnsigned`. The OID is injected by the release workflow only — the in-repo manifest stays plain `CN=ShaderLab` so signed F5 deploys keep working locally.

---

## Local Development

The project ships without a code-signing certificate. On first build, MSBuild auto-runs:

- **`scripts/EnsureDevCert.ps1`** — generates a self-signed cert (`CN=ShaderLab`) and imports it into `TrustedPeople` for F5 deploy.
- **`scripts/EnsureExprTk.ps1`** — downloads `exprtk.hpp` (single-header math expression parser, MIT-licensed) into `third_party/exprtk/`.

After that, F5 (Debug | x64, startup project = `ShaderLab`) deploys and launches the packaged app.

For a one-shot setup on a fresh clone:

```pwsh
.\Bootstrap.ps1            # cert + ExprTk + NuGet restore (no build)
.\Bootstrap.ps1 -Build     # the above + Debug|x64 smoke build
```

See [docs/development/build.md](docs/development/build.md) for full prerequisites, configurations, and the dependency map.

---

## Documentation

The deep technical reference lives under [docs/](docs/README.md), organized by audience.

| Area | Highlights |
|---|---|
| [Architecture](docs/README.md#architecture) | System overview, pipeline format, effect graph model, D2D/D3D11 hybrid compute, engine/host split |
| [Effects](docs/README.md#effects) | Built-in catalog, Effect Designer, parameter / numeric / property-binding mechanics, working space |
| [UI / UX](docs/README.md#ui--ux) | Graph editor, multi-output windows, animation, conditional parameter visibility |
| [Hosts](docs/README.md#hosts) | ShaderLabHeadless console host, MCP server for AI agent integration |
| [Development](docs/README.md#development) | Build instructions, project structure |
| [History](docs/README.md#history) | Decision log (architectural choices with rationale) |

[`CHANGELOG.md`](CHANGELOG.md) tracks every release. The [decision log](docs/history/decision-log.md) tracks architectural choices independently of release boundaries.

---

## Project Identity

ShaderLab is a testbed for HDR / WCG / SDR pixel-effect development. The pipeline is always **scRGB FP16 linear light** (`DXGI_FORMAT_R16G16B16A16_FLOAT`, `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`) — no pipeline format switching; DWM/ACM handles the final display conversion.

Core capabilities:

- **Live effect graph** with D2D + D3D11 compute custom effects, dirty-gated re-evaluation, JSON serialization (`Windows.Data.Json`).
- **Effect Designer** for authoring custom pixel & compute shaders with live HLSL compile + reflection-driven property generation.
- **Analysis viewers** (Luminance / Channel / Chromaticity Statistics, CIE Histogram + Plot, Gamut Coverage, Luminance Heatmap, etc.) — all share the same compute-bridge architecture and route their outputs as SRVs to downstream consumers when possible.
- **Tone-mapping suite** (D2D `HDR Tone Map`, ICtCp Tone Map, ICtCp Inverse Tone Map, ICtCp Gamut Map, etc.) operating in scRGB FP16 with PQ / HLG / sRGB transfer functions.
- **HDR / WCG aware** — DXGI adapter-change tracking, ICC profile parsing, monitor primaries piped into Custom-gamut analysis effects via the Working Space node.
- **MCP server** + **headless host** for AI-agent and CI integration; the MCP route layer lives in `ShaderLabEngine.dll` so headless and GUI hosts share the route implementations.

Build: Visual Studio 2022 17.8+, Windows 10 SDK 10.0.26100+, Win2D 1.3.0, C++/WinRT only (no C#).

---

## License & Contact

See [LICENSE](LICENSE) for the project license and the GitHub repository for issues / PRs.
