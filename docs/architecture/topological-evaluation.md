# Topological Evaluation

```mermaid
flowchart TD
    START([Evaluate Graph]) --> PRUNE[Prune unneeded nodes<br/>Walk back from Output / Preview / Output windows]
    PRUNE --> TOPO[Topological Sort<br/>image edges + binding edges + clock dependencies]
    TOPO --> LOOP{Next node?}
    LOOP -->|Yes| RESOLVE[Resolve property bindings<br/>build effective properties map]
    RESOLVE --> CHECK{Node kind?}
    CHECK -->|Source| LOAD[WIC image / video / Flood<br/>via SourceNodeFactory]
    CHECK -->|BuiltIn D2D| D2D[Reuse cached ID2D1Effect<br/>SetValue per dirty property<br/>Wire inputs from edges]
    CHECK -->|PixelShader| PS[CustomPixelShaderEffect<br/>ID2D1DrawTransform]
    CHECK -->|D2D ComputeShader| CS[CustomComputeShaderEffect<br/>ID2D1ComputeTransform per-tile dispatch]
    CHECK -->|D3D11Compute| D3D11[Render upstream to FP32 bitmap<br/>dc->Flush<br/>D3D11 dispatch via D3D11ComputeRunner<br/>Read back analysis fields]
    CHECK -->|Parameter| PARAM[Copy property values into<br/>analysisOutput.fields]
    CHECK -->|Clock| CLK[Advance clockTime, write Time / Progress]
    CHECK -->|NumericExpression| EXPR[Evaluate ExprTk expression<br/>over A..Z float inputs]
    CHECK -->|Output| OUT[Render preview / per-Output-window draw]
    LOAD --> CACHE[Cache ID2D1Image* output<br/>m_outputCache owns reference]
    D2D --> CACHE
    PS --> CACHE
    CS --> CACHE
    D3D11 --> CACHE
    PARAM --> CACHE
    CLK --> CACHE
    EXPR --> CACHE
    OUT --> CACHE
    CACHE --> LOOP
    LOOP -->|No| PRESENT([Present to SwapChain<br/>+ each OutputWindow])
```

The evaluator runs at the **monitor's refresh rate** (clamped to 60–240 Hz) via a `DispatcherQueueTimer` whose `Interval` is set from `EnumDisplaySettings(ENUM_CURRENT_SETTINGS).dmDisplayFrequency`. 60 / 120 / 144 / 165 / 240 Hz panels and high-FPS video sources all run at their native cadence. The body is **dirty-gated**: `OnRenderTick` skips `RenderFrame` entirely unless any node is dirty, an output window is open, the preview wants a fit, or `m_forceRender` was set by user input. The interval is re-applied on every display change (so dragging the window across monitors picks up the new rate).

---


---

Back to [docs/](../README.md) • [Repo root](../../README.md)