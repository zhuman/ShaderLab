#pragma once

#include "pch.h"

namespace ShaderLab::Controls
{
    // Cross-thread render state for a single OutputWindow.
    //
    // P7 split: the OutputWindow class is a UI-thread shell that owns the
    // XAML window, SwapChainPanel-bound IDXGISwapChain3, and pan/zoom event
    // handlers. The render thread cannot safely deref the OutputWindow
    // (XAML/swap-chain teardown can race), so cross-thread state moves into
    // this struct and is shared via std::shared_ptr.
    //
    // Threading contract:
    //
    //   - UI thread mutates `requestedW/H`, `panX/Y`, `zoom`, `autoFit`,
    //     `needsFit`, `closed` under `viewMutex`. UI thread also creates
    //     the UI-side D2D bitmap source wrappers (`uiSources`) on its own
    //     D2D context, keyed to `bufferGen`.
    //
    //   - Render thread reads view state under `viewMutex`, owns the
    //     offscreen D3D textures + render-side D2D bitmap targets, and
    //     publishes a frame via `publishedIdx` + `publishedVersion` (atomic
    //     release/acquire). On size change it recreates `textures` /
    //     `renderTargets` and bumps `bufferGen` so UI thread knows to
    //     rebuild its `uiSources` wrappers.
    //
    //   - The shared_ptr keeps the struct alive across threads. UI removes
    //     it from MainWindow's sink list only after marking `closed=true`
    //     and waiting one render iteration so the worker has dropped its
    //     transient reference.
    //
    // The two D2D bitmap arrays both wrap the SAME D3D texture pair: the
    // render-side bitmaps have BITMAP_OPTIONS_TARGET | CANNOT_DRAW so the
    // render thread can SetTarget+BeginDraw on them; the UI-side bitmaps
    // have BITMAP_OPTIONS_NONE so the UI thread can use them as DrawImage
    // source. Both UI and render contexts come from the same multi-threaded
    // engine D2D device, so cross-context resource interop works without
    // explicit GPU sync.
    struct OutputSinkRenderState
    {
        // Stable identifier (== EffectGraph node id of the Output node).
        uint32_t nodeId{ 0 };

        // ---- UI-thread-mutated view state (under viewMutex) ----
        std::mutex viewMutex;
        uint32_t   requestedW{ 0 };  // PHYSICAL pixels (offscreen buffer size)
        uint32_t   requestedH{ 0 };  // PHYSICAL pixels
        // Panel's composition scale (1.0 at 100% display scaling, 1.5 at
        // 150%, 2.0 at 200%). Render thread multiplies SetDpi by this to
        // compute fit math in DIP space at 96 DPI canonical, while still
        // rendering into the PHYSICAL-pixel-sized offscreen buffer.
        float      compositionScale{ 1.0f };
        float      panX{ 0.0f };     // DIPs at 96
        float      panY{ 0.0f };     // DIPs at 96
        float      zoom{ 1.0f };
        bool       autoFit{ true };
        bool       needsFit{ true };
        bool       closed{ false };

        // ---- Render-thread-owned buffer set ----
        // Mutated only on render thread. UI thread reads `bufferGen` (atomic)
        // to detect when to rebuild its uiSources wrappers; it can also read
        // `textures[]` raw pointers (com_ptrs are stable until the render
        // thread reseats them on next size change, and UI's own com_ptr in
        // uiSources keeps the previous texture alive long enough).
        uint32_t                        bufW{ 0 };
        uint32_t                        bufH{ 0 };
        winrt::com_ptr<ID3D11Texture2D> textures[2];
        winrt::com_ptr<ID2D1Bitmap1>    renderTargets[2]; // TARGET, on render-thread D2D context

        // Bumps every time the render thread recreates the buffer set
        // (initial create or resize). UI thread compares against
        // `uiObservedGen` to decide whether to rebuild uiSources wrappers.
        std::atomic<uint64_t> bufferGen{ 0 };

        // Publish handshake: render thread writes the just-finished buffer
        // index after EndDraw success (release); UI thread loads it
        // (acquire) and blits that index. -1 means no frame has been
        // published yet.
        std::atomic<int32_t>  publishedIdx{ -1 };
        std::atomic<uint64_t> publishedVersion{ 0 };

        // ---- UI-thread-only cached source wrappers ----
        // Tracks the bufferGen these wrappers were built for. When the
        // render thread bumps bufferGen, UI rebuilds before next blit.
        uint64_t                        uiObservedGen{ 0 };
        winrt::com_ptr<ID2D1Bitmap1>    uiSources[2];
    };
}
