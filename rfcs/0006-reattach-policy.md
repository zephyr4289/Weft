---
RFC: 0006
Title: Android Process-Death ReattachPolicy
Status: Draft
Authors: Weft Core Team
Created: 2026-09-15
Supersedes / Superseded-by: None
---

# RFC 0006 — Android Process-Death ReattachPolicy

## Summary
Proposes the state machine and policy contract for handling Android OS background process termination (low memory killer / process death) in `weft-compose` and Android services, contrasting clean re-allocation with shared memory re-hydration.

## Motivation
When an Android process hosting a UI Reader or background Writer is killed by `ActivityManager` due to memory pressure, state restoration via `onSaveInstanceState` or `rememberSaveable` requires a well-defined protocol for Weft handle resurrection.

## Guide-level explanation
Developers configure a `ReattachPolicy` in `WeftCanvas` or `rememberWeftState`:
- `ReattachPolicy.CLEAN_REALLOCATE` (Default): Destroys old dead pointers and constructs a clean new Triad ring.
- `ReattachPolicy.REHYDRATE_PERSISTED`: Attempts to reconnect to an surviving POSIX/ashmem shared memory region if valid.

## Reference-level specification
- **State Transition Matrix**:
  - `RUNNING` -> `KILLED_BACKGROUND` -> `RECREATED_FRESH_PROCESS`
  - In `CLEAN_REALLOCATE`: 0 handles leaked, sequence resets to 1, zero stale pointers dereferenced (I6 safety).
  - In `REHYDRATE_PERSISTED`: Validates 32-byte header magic and CRC before attaching to persistent memory descriptor.

## Boundary of the claim (Law 4)
Handles process recreation gracefully. Does not prevent frame loss during the period the process was dead.

## Alternatives considered
- Unchecked pointer preservation: Leads to fatal `SIGSEGV` when accessing invalid memory addresses after process rebirth.

## Drawbacks
`REHYDRATE_PERSISTED` requires OS-level shared memory handles (`ashmem` / `AHardwareBuffer`).

## Open questions
- Android 14+ strict inter-process memory isolation policies.

## Hardware Deferral List
- Physical Android device LMK (Low Memory Killer) trigger tests are deferred.

## Staff Decision
[EMPTY]
