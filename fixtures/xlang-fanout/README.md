# fixtures/xlang-fanout — the RFC-0004 byte-compatibility gate

One ring layout, three languages. This fixture proves mechanically that a
fan-out ring produced by **TypeScript** (`@weft/core`, SharedArrayBuffer) is
consumable by **C** (`core/c/fanout.h`), and vice versa — no FFI, no
translation layer, just the bytes.

## The contract (normative — see RFC-0004 §Reference-level specification)

```text
byte 0              latestSeq   u64 (atomic)   0 = no frame yet; frames from 1
byte 8              publishes   u64 (atomic)   telemetry
byte 16 + 8k        slotSeq[k]  u64 (atomic)   0 = INVALIDATED (fill in progress)
byte 16 + 8M        payload     M slots × payload_bytes (payload_bytes % 4 == 0)
```

`ring_bytes = 16 + 8·M + M·payload_bytes` — identical formula in
`core/ts/fanout.ts`, `core/c/fanout.{h,c}`, and `core/rust/src/fanout.rs`.
Ctrl values are non-negative and stay `< 2^53` in any real session (the TS
port surfaces seq as Number — declared, not assumed).

## The pattern generator (shared by all sides)

```text
word(seq, w) = mix32(seq · 2654435761 + w)     [u32 arithmetic; 04-LITMUS §0.1 mixer]
```

C uses `weft_mix32` (kernel), TS uses the exported `mix32` from `@weft/core`
— the two are the same function (A5 differential seed), so every byte the
producer writes is checkable by the consumer.

## Running

```bash
make -C ../../core/c fanout-runner   # build the C side
pnpm --filter @weft/core build       # build the TS side (dist/)
bash run.sh                          # both directions, integrity-gated
```

- `writer.mjs` publishes `FRAMES` (default 5000) frames through the SHIPPED
  broadcaster — the protocol ops (begin/publish, invalidate-before-fill) run
  in the production class; only the payload fill goes through a private
  `Uint32Array` view over the same SAB memory (the same trick a
  fixed-schema app uses for typed fills).
- `reader.mjs` attaches the SHIPPED `WeftFanoutReader` to the C-produced
  bytes (bytes → `SharedArrayBuffer` → reader — the worker-thread attach
  path) and validates accounting + every word's BIT pattern (f32 payloads
  are compared through their u32 bits; NaN never equals itself by value, so
  bits are the only honest comparator).
- The C side (`fanout-runner validate-ring` / `dump-ring`) does the mirror
  image.

Both directions assert: geometry match, fresh final frame,
`dropped == frames - 1`, and zero torn words across ports. The committed
evidence lives in `litmus/evidence/fanout/xlang-interop.log`; CI runs the
same gate as the `fanout-native` shard (`ci/scripts/run_fanout_native_shard.sh`).
