# Multi-Output Windows

Each **Output** node in the graph gets its own OS window with an independent `SwapChainPanel`, pan/zoom, and save-to-file. Implemented in `Controls/OutputWindow.{h,cpp}`.

- **Bidirectional sync**: closing the window deletes the Output node from the graph; deleting the Output node closes the window. Window-close path forces a graph repaint so the node disappears from the canvas immediately, even when the dirty-gated render loop is otherwise idle.
- **Render thread eval, UI thread blit (P12)**: the render worker produces each window's offscreen at the source image's native size into a double-buffered `ID3D11Texture2D` pair owned by a shared `OutputSinkRenderState`. The UI thread wraps the latest published offscreen as a UI-context-bound `ID2D1Bitmap1`, applies the fit-to-panel transform (`Scale(zoom) * Translation(pan)` with `SetDpi(96 × compositionScale)`), and `Present1`s into the window's swap chain. This keeps the UI Present cost sub-ms regardless of graph eval throughput. See [Threading Model → Output windows](../architecture/threading-model.md#output-windows-p12).
- **Shared multi-threaded D2D device**: all output windows + the main preview + the render worker share the same multi-threaded D2D device. Each consumer creates its own `ID2D1DeviceContext5` so state (target/transform/DPI) is independent.
- **Cross-thread sink**: `Controls/OutputSinkRenderState.h` carries the view state (`requestedW/H`, `compositionScale`, `panX/Y`, `zoom`, `autoFit`, `closed`) under a mutex plus atomic publication state (`bufferGen`, `publishedIdx`, `publishedVersion`). UI mutates view-state under the mutex; render thread snapshots and reads. A buffer-generation handshake lets UI rebuild source-bitmap wrappers when the offscreen recreates on a size change without locking the read path.
- **MCP**: `/graph/apply` accepts `effect="Output"` and round-trips Output nodes through the protocol. `OnNodeAdded` auto-spawns a window when an Output node lands in the graph from any source (toolbar click, MCP, file load).
- **Independent viewport**: each window has its own pan/zoom transform, separate from the main preview panel and from every other output window.


---

Back to [docs/](../README.md) • [Repo root](../../README.md)