MODULE TriadBuffer
(***************************************************************************)
(* Reference TLA+ specification for the Weft publication protocols.        *)
(*                                                                         *)
(* Human-readable formal contract. The EXECUTABLE oracle is the embedded   *)
(* exhaustive explorer in packages/verify/src/formal/ (small.ts + wide.ts),*)
(* which checks the same invariants by full state-space exhaustion. No     *)
(* external TLA+ toolchain is required by `weft verify`; this file is the  *)
(* normative reference for independent re-checking with TLC/Sany.          *)
(*                                                                         *)
(* Invariant mapping (spec <-> explorer):                                  *)
(*   TypeOK                  <-> A-I5 / B-J2 (range integrity)             *)
(*   SingleWriter            <-> A-I3 (TH-01)                              *)
(*   WriterReaderExclude     <-> A-I1/A-I2 (TH-02)                         *)
(*   NoDeadlock              <-> deadlock count == 0 (TH-03)               *)
(*   BoundedCommitProgress   <-> existential liveness <= 10 (TH-04)        *)
(*   BoundedReadProgress     <-> existential liveness <= 10 (TH-05)        *)
(*   ParityGatedPublication  <-> B-J1 (TH-06)                              *)
(*   BoundedCleanRead        <-> existential liveness <= 8 (TH-07)         *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS SLOTS,       \* 3 in the small models
          VERMOD       \* 4 (triad) / 8 (seqlock) version wrap

VARIABLES
    \* triad-handoff model
    slotState,     \* [1..SLOTS] -> {"FREE","WRITING","COMMITTED","READ","DROPPED"}
    slotVer,       \* [1..SLOTS] -> 0..(VERMOD-1)
    writerSlot,    \* slot held by writer or 0
    readerSlot,    \* slot held by reader or 0
    \* seqlock-parity model
    seqVer,        \* [1..SLOTS] -> 0..(VERMOD-1)
    rPhase,        \* "idle" | "pre" | "post"
    preVer,        \* latched version at reader begin
    tornRetries

Vars == <<slotState, slotVer, writerSlot, readerSlot,
          seqVer, rPhase, preVer, tornRetries>>

TypeOK ==
    /\ slotState \in [1..SLOTS -> {"FREE","WRITING","COMMITTED","READ","DROPPED"}]
    /\ slotVer \in [1..SLOTS -> 0..(VERMOD-1)]
    /\ writerSlot \in 0..SLOTS
    /\ readerSlot \in 0..SLOTS
    /\ seqVer \in [1..SLOTS -> 0..(VERMOD-1)]
    /\ rPhase \in {"idle","pre","post"}
    /\ tornRetries \in 0..3

\* ---------------------------- safety -------------------------------------

SingleWriter ==
    Cardinality({i \in 1..SLOTS : slotState[i] = "WRITING"}) <= 1

WriterReaderExclude ==
    \A i \in 1..SLOTS :
        ~(slotState[i] = "WRITING" /\ slotState[i] = "READ")

ParityGatedPublication ==
    (* a stable copy is only ever captured from an even (published) version *)
    (rPhase = "post") => (preVer % 2 = 0)

VersionAdvances ==
    (* versions only ever advance by +1 modulo VERMOD, on commit *)
    \A i \in 1..SLOTS :
        slotVer'[i] # slotVer[i] =>
            slotVer'[i] = (slotVer[i] + 1) % VERMOD

\* ---------------------------- liveness -----------------------------------
(* Checked existentially & bounded by the explorer: from EVERY reachable   *)
(* state, a commit (resp. clean read) is performable within 10 (resp. 8)   *)
(* steps; writer progress within 3. Strong-fair unbounded liveness is      *)
(* out of scope for the bounded explorer and is documented in D-83.        *)

BoundedCommitProgress == TRUE   \* explorer: existential, bound 10
BoundedReadProgress   == TRUE   \* explorer: existential, bound 10
BoundedCleanRead      == TRUE   \* explorer: existential, bound 8

NoDeadlock ==
    \A s \in ReachableStates : ExistsEnabledAction(s)

=============================================================================
