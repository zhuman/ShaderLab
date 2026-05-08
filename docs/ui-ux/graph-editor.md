# Graph Editor UX

## Adding nodes

- **Toolbar / context menu**: new nodes are placed in the **center of the current viewport** (in graph coordinates), accounting for the user's pan / zoom — they no longer drop at canvas origin behind off-screen pans.
- **Auto-arrange**: `Ctrl+L` (or toolbar button) sorts nodes by topological depth and lays them out in evenly-spaced columns.

## Removing edges (Alt+click)

- **Alt + Left-click** on any edge — image edge or orange data-binding edge — removes it. Hit-testing is done by sampling the cubic bezier with a small distance tolerance (`Controls/NodeGraphController.cpp::HitTestEdge`). The same path also handles right-click delete in the canvas context menu.

## Copy / Paste

- `Ctrl+C` / `Ctrl+V` copy the selected nodes (and any edges that fall entirely between them) into the clipboard, then paste with a small offset and fresh GUIDs.

## Color coding by node kind

| Color | Node kind |
|-------|-----------|
| Green | Source nodes (images, video, generators) |
| Blue | Built-in D2D effects |
| Red | Custom pixel shader effects |
| Orange | Custom D2D compute shader effects |
| Yellow | D3D11 compute shader effects |
| Teal | Parameter / Clock / Numeric Expression |
| Purple | Data-only analysis nodes (Image Statistics) |
| Gray | Output nodes |

## Inline data display

- **Data pin values** are shown inline on the node canvas (current bound value).
- **Enum labels**: enum-typed properties show their label (not raw integer) on data pins.
- **Image input pin labels** are derived from effect input names (e.g., `Source`, `Destination`).
- **“No Input”** text is displayed on image input pins with broken / missing connections.


---

Back to [docs/](../README.md) • [Repo root](../../README.md)