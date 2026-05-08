# MCP Server (AI Agent Integration)

ShaderLab includes an embedded HTTP server implementing the **Model Context Protocol (MCP)** JSON-RPC 2.0 for programmatic control by AI agents. The server itself, plus 20 engine-pure routes, ships in the engine DLL — both the GUI host and `ShaderLabHeadless --script` mode register the same routes through the same `IEngineCommandSink` interface (see [Engine / Host Split](#engine--host-split)).

## Connection

- Default port: **47808** (auto-increments if in use)
- Transport: Streamable HTTP (`POST /` for JSON-RPC)
- Enable: MCP toggle in toolbar, `--mcp` flag, or `config.json`

## Tools (27 total)

| Tool | Description |
|------|-------------|
| `graph_add_node` | Add built-in D2D or ShaderLab effect (placed at viewport center). |
| `graph_remove_node` | Remove a node. |
| `graph_rename_node` | Rename a node. |
| `graph_connect` | Connect image pins. |
| `graph_disconnect` | Disconnect image pins. |
| `graph_set_property` | Set a node property. |
| `graph_get_node` | Get node details + analysis results. |
| `graph_save_json` | Serialize graph to JSON. |
| `graph_load_json` | Load graph from JSON. |
| `graph_clear` | Clear entire graph (keeps Output). |
| `graph_overview` | Compact graph summary (nodes, edges, preview). |
| `graph_bind_property` | Bind property to upstream analysis field. |
| `graph_unbind_property` | Remove a property binding. |
| `effect_compile` | Compile HLSL (+ optional analysisFields). |
| `set_preview_node` | Set which node is previewed. |
| `render_capture` | Capture preview as PNG (HDR clipped to SDR). |
| `registry_get_effect` | Get built-in effect metadata. |
| `read_analysis_output` | Read typed analysis fields from a compute / analysis / parameter node. |
| `read_pixel_trace` | Pixel trace at normalized coords (per-node values). |
| `list_effects` | List all effects by category. |
| `get_display_info` | Display caps, active profile, pipeline, app version. |
| `node_logs` | Per-node timestamped info / warning / error log entries. |
| `perf_timings` | Per-node evaluation timings from the most recent frame. |
| `graph_snapshot` | PNG snapshot of the live node-graph editor view. With `inline=true` returns image bytes as MCP image content; otherwise returns the temp file path. |
| `graph_get_view` | Read the editor's current zoom, pan, viewport size, and content bounds. |
| `graph_set_view` | Apply `{zoom?, panX?, panY?}` to the live editor — same effect as user pan/zoom input. |
| `graph_fit_view` | Fit the editor view to all nodes with a viewport-space `padding` (DIPs, default 40). |

## Known Limitations

- **Compile-before-connect**: First-time compile of a compute shader node that's already connected to the render pipeline crashes D2D. Workaround: compile the shader while the node is disconnected, then wire it in. Recompiles of already-compiled nodes work fine.
- **FP16 precision**: Analysis readback values show minor quantization (e.g., 0.1 → 0.099976) due to the D2D output buffer using 16-bit float precision.
- **HLSL optimizer removes unreferenced cbuffer vars**: With `D3DCOMPILE_WARNINGS_ARE_ERRORS`, variables not referenced on ALL code paths are optimized out. Read all cbuffer vars at top of `main()` before branches.
- **ExprTk math-only subset**: Numeric Expression has the regex / IO / enhanced subsystems disabled (see [Numeric Expression Node](#numeric-expression-node-exprtk)). Expressions must produce finite scalar `float` results — no strings, no file I/O, no vector return values.

---

Back to [docs/](../README.md) • [Repo root](../../README.md)