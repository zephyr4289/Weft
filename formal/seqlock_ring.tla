---------------------------- MODULE seqlock_ring -----------------------------
(* WEFT PILLAR 8 — Formal model of the Weft 64B/128B Seqlock Ring Buffer.  *)
(*                                                                          *)
(* WHAT THIS MODEL IS: the seqlock ring's protocol at the granularity of    *)
(* its atomic memory operations. The single writer's four micro-steps per  *)
(* frame — seq++ (odd, write-begun), write the low half, write the high    *)
(* half, seq++ (even, write-complete) — and every reader's four micro-     *)
(* steps — sample seqBefore, read the low half, read the high half, re-    *)
(* sample seq and validate — are SEPARATE ATOMIC ACTIONS. TLC therefore    *)
(* explores every interleaving, INCLUDING the tear window: a reader's      *)
(* half-by-half copy interleaved with the writer's half-by-half install    *)
(* into the same slot.                                                      *)
(*                                                                          *)
(*   Safety (invariants over the whole reachable state space):              *)
(*     - TypeOK       — every variable stays in its declared shape          *)
(*     - SeqParity    — a slot's sequence number is odd ONLY while the      *)
(*                      writer is mid-install on that slot; an idle writer  *)
(*                      leaves every slot even (stable)                     *)
(*     - NoTornReads  — a reader that validates seqBefore = seqAfter       *)
(*                      (same even value) NEVER committed a half-written    *)
(*                      frame: gotLo/gotHi always equal the ground truth    *)
(*                      expLo/expHi recorded at read-begin. This is THE     *)
(*                      theorem the 64B/128B two-store publishing protocol  *)
(*                      of the kernel ring guarantees.                      *)
(*                                                                          *)
(*   Liveness (PROPERTY ReaderNeverBlocksWriter, under writer-ONLY         *)
(*   weak fairness — readers get no fairness clause, i.e. they may crash,   *)
(*   stall or spin arbitrarily):                                            *)
(*     - wBeat (the writer heartbeat) flips infinitely often: completed     *)
(*       writes and slot-skips keep happening no matter what any reader     *)
(*       does. Formally: []<>(wBeat = 0) /\ []<>(wBeat = 1) under          *)
(*       FairSpec. A writer never waits on, synchronizes with, or observes  *)
(*       a reader; the model's writer action guards reference ONLY         *)
(*       writer-owned state (seq/head/wPhase/wVal), which the C oracle in  *)
(*       tests/verify/core/test_oracle_tla.c re-verifies mechanically by   *)
(*       re-evaluating writer enabledness over the whole reachable space   *)
(*       with the reader fields zeroed.                                     *)
(*                                                                          *)
(* MODELING NOTES:                                                          *)
(*   - Sequence numbers are bounded by SEQCAP (writer guard seq+2 <=       *)
(*     SEQCAP) so the state space is finite without a wraparound artifact: *)
(*     a wrapped counter could alias seqBefore = seqAfter across a full    *)
(*     modulo cycle and fake a "stable" read. Real hardware uses           *)
(*     32/64-bit counters where such a cycle is unreachable; the C oracle  *)
(*     runs the same protocol with full-width counters over 10,000,000     *)
(*     randomized steps to cover the unbounded regime.                     *)
(*   - When the writer's current slot is capped it SKIPS forward           *)
(*     (AdvanceHead) — the ring's overwrite policy — so the writer is      *)
(*     never deadlocked by the bound.                                       *)
(*   - Checked bounds (seqlock_ring.cfg): 2 readers, 3 slots, 2 values,    *)
(*     seq cap 6 — the smallest shape that contains a torn-window          *)
(*     interleaving AND a full ring wrap. seqlock_ring_full.cfg scales     *)
(*     the same spec to the mandated >= 10^7 distinct-state exploration.   *)
(*                                                                          *)
(* Checked by TLC 1.8.0 (pinned sha) — see                                  *)
(* docs/reports/D-81-FORMAL-VERIFICATION-AUDIT.md.                          *)
EXTENDS Naturals, FiniteSets

CONSTANTS READERS,  \* number of concurrent readers
          SLOTS,    \* ring capacity M (64B/128B slots)
          VALUES,   \* abstract payload halves domain
          SEQCAP    \* exclusive upper bound for sequence numbers (even)

Slots   == 0 .. SLOTS - 1
Readers == 1 .. READERS
Vals    == 0 .. VALUES - 1

VARIABLES
    \* per-slot protocol state
    seq,       \* [Slots -> 0..SEQCAP] — even = stable, odd = install begun
    dlo,       \* [Slots -> Vals] — low half of the slot payload
    dhi,       \* [Slots -> Vals] — high half of the slot payload
    \* writer state (the ONLY writer-owned protocol state)
    head,      \* the slot the writer installs into next
    wPhase,    \* "idle" | "lo" | "hi" | "dn"
    wVal,      \* the value being installed in the current write
    wBeat,     \* writer heartbeat: flips on every completed write or skip
    \* reader state (per reader id)
    rPhase,    \* "idle" | "mid" | "gotlo" | "gothi" | "chk"
    rSlot,     \* slot this reader is reading
    rSeqB,     \* seqBefore sampled at read begin
    rExpLo,    \* ground truth: whole payload at read begin (auxiliary)
    rExpHi,
    rGotLo,    \* what the reader actually copied
    rGotHi,
    rCommit,   \* last completed read was committed (sticky until next begin)
    rTorn      \* a torn read was DETECTED and retried (the honest refusal)

Vars == <<seq, dlo, dhi, head, wPhase, wVal, wBeat,
          rPhase, rSlot, rSeqB, rExpLo, rExpHi, rGotLo, rGotHi,
          rCommit, rTorn>>

------------------------------------------------------------------------------
(*** the writer — four atomic micro-steps per install ***)

WSeqUp ==
    /\ wPhase = "idle"
    /\ seq[head] + 2 <= SEQCAP
    /\ wVal' \in Vals
    /\ seq' = [seq EXCEPT ![head] = @ + 1]        \* odd: install begun
    /\ wPhase' = "lo"
    /\ UNCHANGED <<dlo, dhi, head, wBeat>>
    /\ UNCHANGED <<rPhase, rSlot, rSeqB, rExpLo, rExpHi, rGotLo, rGotHi,
                   rCommit, rTorn>>

WLo ==
    /\ wPhase = "lo"
    /\ dlo' = [dlo EXCEPT ![head] = wVal]
    /\ wPhase' = "hi"
    /\ UNCHANGED <<seq, dhi, head, wVal, wBeat>>
    /\ UNCHANGED <<rPhase, rSlot, rSeqB, rExpLo, rExpHi, rGotLo, rGotHi,
                   rCommit, rTorn>>

WHi ==
    /\ wPhase = "hi"
    /\ dhi' = [dhi EXCEPT ![head] = wVal]
    /\ wPhase' = "dn"
    /\ UNCHANGED <<seq, dlo, head, wVal, wBeat>>
    /\ UNCHANGED <<rPhase, rSlot, rSeqB, rExpLo, rExpHi, rGotLo, rGotHi,
                   rCommit, rTorn>>

WSeqDn ==
    /\ wPhase = "dn"
    /\ seq' = [seq EXCEPT ![head] = @ + 1]        \* even: install complete
    /\ head' = (head + 1) % SLOTS
    /\ wPhase' = "idle"
    /\ wBeat' = 1 - wBeat
    /\ UNCHANGED <<dlo, dhi, wVal>>
    /\ UNCHANGED <<rPhase, rSlot, rSeqB, rExpLo, rExpHi, rGotLo, rGotHi,
                   rCommit, rTorn>>

\* The ring's overwrite policy: a capped slot is skipped, never deadlocks.
AdvanceHead ==
    /\ wPhase = "idle"
    /\ seq[head] + 2 > SEQCAP
    /\ head' = (head + 1) % SLOTS
    /\ wBeat' = 1 - wBeat
    /\ UNCHANGED <<seq, dlo, dhi, wVal, wPhase>>
    /\ UNCHANGED <<rPhase, rSlot, rSeqB, rExpLo, rExpHi, rGotLo, rGotHi,
                   rCommit, rTorn>>

WriterNext == WSeqUp \/ WLo \/ WHi \/ WSeqDn \/ AdvanceHead

------------------------------------------------------------------------------
(*** the readers — four atomic micro-steps per attempt, never blocking ***)

RBegin(r) ==
    /\ rPhase[r] = "idle"
    /\ \E s \in Slots :
        /\ rSlot' = [rSlot EXCEPT ![r] = s]
        /\ rSeqB' = [rSeqB EXCEPT ![r] = seq[s]]
        /\ rExpLo' = [rExpLo EXCEPT ![r] = dlo[s]]   \* auxiliary ground truth
        /\ rExpHi' = [rExpHi EXCEPT ![r] = dhi[s]]
        /\ rCommit' = [rCommit EXCEPT ![r] = FALSE]
        /\ rGotLo' = [rGotLo EXCEPT ![r] = 0]
        /\ rGotHi' = [rGotHi EXCEPT ![r] = 0]
        /\ rPhase' = [rPhase EXCEPT ![r] = "mid"]
    /\ UNCHANGED <<seq, dlo, dhi, head, wPhase, wVal, wBeat>>
    /\ UNCHANGED <<rTorn>>

RCopyLo(r) ==
    /\ rPhase[r] = "mid"
    /\ rGotLo' = [rGotLo EXCEPT ![r] = dlo[rSlot[r]]]
    /\ rPhase' = [rPhase EXCEPT ![r] = "gotlo"]
    /\ UNCHANGED <<seq, dlo, dhi, head, wPhase, wVal, wBeat, rTorn>>
    /\ UNCHANGED <<rSlot, rSeqB, rExpLo, rExpHi, rGotHi, rCommit>>

RCopyHi(r) ==
    /\ rPhase[r] = "gotlo"
    /\ rGotHi' = [rGotHi EXCEPT ![r] = dhi[rSlot[r]]]
    /\ rPhase' = [rPhase EXCEPT ![r] = "gothi"]
    /\ UNCHANGED <<seq, dlo, dhi, head, wPhase, wVal, wBeat, rTorn>>
    /\ UNCHANGED <<rSlot, rSeqB, rExpLo, rExpHi, rGotLo, rCommit>>

\* The validation fence: seqBefore = seqAfter (and even) proves no write
\* began during the copy; otherwise the read is TORN and honestly refused.
REnd(r) ==
    /\ rPhase[r] = "gothi"
    /\ IF seq[rSlot[r]] = rSeqB[r] /\ (rSeqB[r] % 2) = 0
       THEN /\ rCommit' = [rCommit EXCEPT ![r] = TRUE]
            /\ rTorn' = rTorn
       ELSE /\ rCommit' = [rCommit EXCEPT ![r] = FALSE]
            /\ rTorn' = [rTorn EXCEPT ![r] = TRUE]
    /\ rPhase' = [rPhase EXCEPT ![r] = "idle"]
    /\ UNCHANGED <<seq, dlo, dhi, head, wPhase, wVal, wBeat>>
    /\ UNCHANGED <<rSlot, rSeqB, rExpLo, rExpHi, rGotLo, rGotHi>>

ReaderNext(r) == RBegin(r) \/ RCopyLo(r) \/ RCopyHi(r) \/ REnd(r)

------------------------------------------------------------------------------
Init ==
    /\ seq = [s \in Slots |-> 0]
    /\ dlo = [s \in Slots |-> 0]
    /\ dhi = [s \in Slots |-> 0]
    /\ head \in Slots
    /\ wPhase = "idle"
    /\ wVal \in Vals
    /\ wBeat \in {0, 1}
    /\ rPhase = [r \in Readers |-> "idle"]
    /\ rSlot = [r \in Readers |-> 0]
    /\ rSeqB = [r \in Readers |-> 0]
    /\ rExpLo = [r \in Readers |-> 0]
    /\ rExpHi = [r \in Readers |-> 0]
    /\ rGotLo = [r \in Readers |-> 0]
    /\ rGotHi = [r \in Readers |-> 0]
    /\ rCommit = [r \in Readers |-> FALSE]
    /\ rTorn = [r \in Readers |-> FALSE]

Next == WriterNext \/ \E r \in Readers : ReaderNext(r)

\* Fairness is WRITER-ONLY by design: readers may crash, stall or spin —
\* the writer's progress must not depend on them in any way. That is the
\* formal content of "ReaderNeverBlocksWriter".
Liveness ==
    /\ WF_Vars(WSeqUp)
    /\ WF_Vars(WLo)
    /\ WF_Vars(WHi)
    /\ WF_Vars(WSeqDn)
    /\ WF_Vars(AdvanceHead)

Spec == Init /\ [][Next]_Vars /\ Liveness

------------------------------------------------------------------------------
(*** safety ***)

TypeOK ==
    /\ seq \in [Slots -> 0 .. SEQCAP]
    /\ dlo \in [Slots -> Vals]
    /\ dhi \in [Slots -> Vals]
    /\ head \in Slots
    /\ wPhase \in {"idle", "lo", "hi", "dn"}
    /\ wVal \in Vals
    /\ wBeat \in {0, 1}
    /\ rPhase \in [Readers -> {"idle", "mid", "gotlo", "gothi"}]
    /\ rSlot \in [Readers -> Slots]
    /\ rSeqB \in [Readers -> 0 .. SEQCAP]
    /\ rExpLo \in [Readers -> Vals]
    /\ rExpHi \in [Readers -> Vals]
    /\ rGotLo \in [Readers -> Vals]
    /\ rGotHi \in [Readers -> Vals]
    /\ rCommit \in [Readers -> BOOLEAN]
    /\ rTorn \in [Readers -> BOOLEAN]

\* A slot's counter is odd ONLY while the writer is mid-install on it, and
\* an idle writer leaves every slot stable (even).
SeqParity ==
    /\ (wPhase # "idle") => (seq[head] % 2) = 1
    /\ (wPhase = "idle") => \A s \in Slots : (seq[s] % 2) = 0

\* THE SAFETY THEOREM (NoTornReads): every COMMITTED read returned exactly
\* the payload that was whole in the slot when the read began. The two-store
\* seq protocol (odd-during-install, monotone per install) makes it
\* impossible for seqBefore = seqAfter to hold while the payload changed.
NoTornReads ==
    \A r \in Readers :
        rCommit[r] => /\ rGotLo[r] = rExpLo[r]
                      /\ rGotHi[r] = rExpHi[r]

Inv == TypeOK /\ SeqParity /\ NoTornReads

------------------------------------------------------------------------------
(*** liveness — the writer heartbeat beats forever, readers be damned ***)

ReaderNeverBlocksWriter ==
    /\ []<>(wBeat = 0)
    /\ []<>(wBeat = 1)

=============================================================================
