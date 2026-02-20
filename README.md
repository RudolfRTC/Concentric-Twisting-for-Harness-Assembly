# Concentric Twisting Calculator

A browser-based engineering tool for harness assembly concentric twisting calculations. Runs entirely client-side — no backend or build step required.

## Features

1. **Layer Configurator** — Auto-calculates wire layers using the 1-6-12-18 rule. Shows wire counts and bundle outer diameter per layer.

2. **Lay Length Calculator** — Computes recommended lay length (pitch) per layer based on a configurable lay ratio. Supports per-layer RH/LH direction toggling with automatic alternating defaults.

3. **Wire Elongation** — Calculates actual wire length needed due to helical path and shows percentage elongation vs straight length.

4. **Bundle Weight Estimator** — Estimates weight per meter and total weight for copper or aluminium conductors with configurable insulation thickness.

5. **Cross-Section Visualizer** — Renders a live 2D SVG cross-section of the concentric bundle with color-coded layers.

6. **Results Export** — Copy all layer data as formatted text or CSV.

## Usage

Open `index.html` in any modern browser. No dependencies, no build tools.

## Formulas

| Calculation | Formula |
|---|---|
| Wires per layer | `6 × layer_number` (core = 1) |
| Bundle OD | `wire_diameter × (1 + 2 × num_layers)` |
| Lay length | `lay_ratio × layer_OD` |
| Wire length | `L_straight × √(1 + (π × D_bundle / lay_length)²)` |

## Units

Metric (mm, g) by default. Toggle to imperial (in, oz) via the header button. Dark/light theme toggle included.
