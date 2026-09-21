# WeftFintech (Swift / SwiftUI)

Weft FinTech Apple connectors — `WeftOrderBookView` (SwiftUI Canvas depth
ladder) over the MDP1 snapshot wire, with an edge-triggered
`@Observable` model. Part of Pillar 6 (weft-adapters), deliverable A
(Apple lane).

## Architecture

```
native feed (ring attach / synthesizer)
    │  () -> UnsafeRawBufferPointer?    ← reference pass, NO copy
    ▼
WeftOrderBookModel.ingest()   @MainActor @Observable
    │  Mdp1View.validate() fail-closed (short → magic → version → CRC)
    │  edge gate: publish ONLY on seq advance (no rebuild storms)
    │  preallocated level scratch: 20 levels × {price,size,orders}
    ▼
WeftOrderBookView (TimelineView(.animation) + Canvas)
       geometry-only draw from model.levels; labels opt-in
```

- All multi-byte loads pass `.littleEndian` explicitly (`loadUnaligned`
  chains), u64 fields compose with `<< 32` — exact 64-bit math.
- A structurally invalid record latches the model into FALLBACK
  (`bookValid == false`) exactly once; the view paints a warning band
  and never throws (Law 4 / W4-04).
- The draw closure reads the preallocated scratch; the label path is
  opt-in and documented as allocating (Pillar 4 label lesson).

## Layout

| Path | Role |
|------|------|
| `Sources/WeftFintech/Mdp1Wire.swift` | constants, CRC-32/ISO-HDLC, `Mdp1View` flyweight |
| `Sources/WeftFintech/WeftOrderBookModel.swift` | `@Observable` edge-triggered model |
| `Sources/WeftFintech/WeftOrderBookView.swift` | SwiftUI Canvas ladder |
| `Tests/WeftFintechTests/Mdp1WireTests.swift` | XCTest: CRC KATs, decode, corruption matrix |
| `audit/static_audit.mjs` | Node structural audit (27 checks) |

## Verification honesty

No Swift toolchain exists in the managed sandbox: XCTest sources are
CI-lane gated (`swift test` on macOS), and `audit/static_audit.mjs`
mechanically pins the compile-checkable properties — constants parity
vs `packages/fintech/src/mdp1.js`, 100% `.littleEndian` load coverage,
CRC KATs re-derived from `node:zlib`, edge-gate and FALLBACK structure,
geometry-only draw.
