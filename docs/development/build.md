# Build Instructions

## Prerequisites
- Visual Studio 2022 17.8+ **or** Visual Studio 2026 Insiders (with C++ Desktop and UWP workloads)
- Windows App SDK 1.8
- Windows 10 SDK (10.0.26100+)
- PowerShell 5.1+ (for the pre-build scripts)
- Internet access on first build (so `EnsureExprTk.ps1` can download `exprtk.hpp`)

## Build
1. Open `ShaderLab.slnx` in Visual Studio.
2. Pre-build steps run automatically on first build:
   - `scripts\EnsureDevCert.ps1` — generates and installs the local F5 dev cert.
   - `scripts\EnsureExprTk.ps1` — downloads `exprtk.hpp` (MIT) into `third_party\exprtk\`.
3. NuGet packages restore automatically.
4. Build configurations:
   - `Debug | x64`, `Release | x64`
   - `Debug | ARM64`, `Release | ARM64`
5. Outputs (per arch):
   - `x64\Debug\ShaderLabEngine\ShaderLabEngine.dll`
   - `x64\Debug\ShaderLab\ShaderLab.exe`
   - `x64\Debug\ShaderLabTests\ShaderLabTests.exe`

## Releases

GitHub Actions workflow `.github/workflows/release.yml` runs as a matrix (`x64`, `ARM64`). Just before MSBuild, the workflow injects the unsigned-namespace OID into the manifest's `Publisher` so that the resulting MSIX is installable via `Add-AppxPackage -AllowUnsigned`. The in-repo `Package.appxmanifest` keeps the plain `CN=ShaderLab` publisher so signed F5 deploys keep working.

## Required Libraries (linked via vcxproj)

| Library | Purpose |
|---------|---------|
| `d3d11.lib` | Direct3D 11 device and context |
| `d2d1.lib` | Direct2D rendering and effects |
| `dxgi.lib` | DXGI swap chain, HDR output queries |
| `d3dcompiler.lib` | Runtime HLSL compilation (D3DCompile) |
| `dxguid.lib` | DirectX GUIDs (IID_ID2D1Factory, etc.) |
| `windowscodecs.lib` | WIC image loading |
| `mfplat.lib`, `mfreadwrite.lib`, `mfuuid.lib` | Media Foundation video source decoding |
| `mscms.lib` | ICC profile reading (Image Color Management) |
| `windowsapp.lib` | Windows Graphics Capture interop (CreateDirect3D11DeviceFromDXGIDevice) |

---


---

Back to [docs/](../README.md) • [Repo root](../../README.md)