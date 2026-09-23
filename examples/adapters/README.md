# examples/adapters — Pillar 6 end-to-end demos

| Demo | Command | Mandate |
|------|---------|---------|
| `fintech_l3_trading_demo` | `node --expose-gc fintech_l3_trading_demo/run.mjs` | 5,000,000 ITCH 5.0 messages → live L2/L3 book → MDP1 snapshots → 240 FPS `<WeftOrderBook />` HUD, 0 dropped frames, UI zone 0 KiB heap growth |
| `robotics_vision_demo` | `python3 robotics_vision_demo/run.py` | 1,200 × 4K DMA-frame grabs → zero-copy DLPack tensors → detector → BOXES_F32 ring → 120 FPS box HUD, pointer identity 12/12, UI zone 0.4 KiB |

Both demos are fail-closed (exit 2 on any gate violation) and print a
single evidence JSON with every gate metric.
