# Numeric Expression Node (ExprTk)

A single, expression-driven math node replaces the legacy fixed-op math primitives. Implemented in `Rendering/MathExpression.{h,cpp}` (PCH disabled on the .cpp so the heavy ExprTk template instantiations don't leak into the rest of the engine TU).

- **Dynamic inputs**: starts with one input `A`. Use the **➕ Add Input** button in the Properties panel to add `B`, `C`, … up to `Z` (26-input cap). Each row has an **✕** button to remove just that input.
- **Expression**: a `TextBox` at the top of the Properties panel holds the formula. Examples:
  - `A + B * C`
  - `max(A, B, C)`
  - `if (A > B, A, B)`
  - `sin(A) * 0.5 + 0.5`
  - `pow(clamp(A, 0, 1), 2.2)`
- **Canvas display**: the formula is rendered under the node title as `= <expression>` so it's readable at a glance in the graph.
- **Output**: a single `float` analysis field named `Result`, available on the orange data pin and bindable to any downstream scalar property.
- **Errors**: parse / evaluation errors surface in the per-node log pane (`node_logs` MCP tool). The node short-circuits to `0.0` until the expression compiles cleanly.
- **Persistence**: the expression and the live input list both round-trip through graph JSON.

**ExprTk feature flags.** `Rendering/MathExpression.cpp` defines
`exprtk_disable_string_capabilities`, `exprtk_disable_rtl_io`, `exprtk_disable_rtl_io_file`,
`exprtk_disable_rtl_vecops`, `exprtk_disable_enhanced_features`, and
`exprtk_disable_caseinsensitivity` *before* `#include`-ing `exprtk.hpp`. This trims the
library to its math-only core; without these flags the MSVC Release optimizer was
producing a 0xC0000005 in the regex / IO subsystems on first evaluation. Only finite
scalar expressions are supported — no strings, files, or vector return values.


---

Back to [docs/](../README.md) • [Repo root](../../README.md)