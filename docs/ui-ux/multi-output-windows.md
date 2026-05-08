# Multi-Output Windows

Each **Output** node in the graph gets its own OS window with an independent SwapChainPanel, pan/zoom, and save-to-file. Implemented in `Controls/OutputWindow.h/.cpp`.

- **Bidirectional sync**: closing the window deletes the Output node from the graph; deleting the Output node closes the window. Window-close path forces a graph repaint so the node disappears from the canvas immediately, even when the dirty-gated render loop is otherwise idle.
- **Shared D2D device context**: all output windows share the same D3D11/D2D1 device stack from `RenderEngine`.
- **Independent viewport**: each window has its own pan/zoom transform, separate from the main preview panel.


---

Back to [docs/](../README.md) • [Repo root](../../README.md)