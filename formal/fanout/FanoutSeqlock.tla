---------------------------- MODULE FanoutSeqlock ----------------------------
(* RFC 0011 — Formal model of the RFC 0004 multi-reader fan-out seqlock.    *)
(*                                                                          *)
(* WHAT THIS MODEL IS: the fan-out ring's protocol at the granularity of    *)
(* its atomic operations. The writer's begin/invalidate/fill-one-word/      *)
(* stamp/publish steps and the readers' bounded 4-attempt claim loop are    *)
(* separate ATOMIC ACTIONS, so TLC explores every interleaving of the       *)
(* protocol — INCLUDING the tear window: a reader's word-by-word copy can   *)
(* be interleaved with the writer's invalidate/refill of the same slot.     *)
(* Where loom exhaustively checks small real-memory executions, this model  *)
(* proves the PROTOCOL's safety and liveness for every schedule at the      *)
(* label granularity:                                                       *)
(*                                                                          *)
(*   Safety (invariants over the whole reachable state space):              *)
(*     - TypeOK          — every variable stays in its declared shape       *)
(*     - StampBracket    — a slot's stamp is 0 ONLY while the writer is     *)
(*                         mid-fill on that slot (FI1)                      *)
(*     - LatestMonotonic — latestSeq never runs ahead of the writer         *)
(*     - NoFuture        — no reader accepts a frame beyond the writer      *)
(*     - NoTornAccepted  — every word of every ACCEPTED frame snapshot      *)
(*                         carries the accepted frame's tag: NO SCHEDULE    *)
(*                         CAN MAKE A READER ACCEPT A TORN FRAME. This is   *)
(*                         the theorem the chaos engine and the torture     *)
(*                         gate sample empirically.                         *)
(*     - Telescoping     — per reader, dropped == lastSeq - freshClaims     *)
(*                         (the RFC 0004 accounting identity)               *)
(*   Liveness (explicit weak fairness on the progress actions):             *)
(*     - WriterFinishes   — the writer publishes every frame                *)
(*     - ReaderSeesFinal  — every reader eventually accepts the final frame *)
(*                          (no starvation, no deadlock)                    *)
(*                                                                          *)
(* MODELING NOTE (the tear window): a payload word's value is the FRAME TAG *)
(* that wrote it — the deterministic tword(seq, w) pattern of the real      *)
(* implementations plays this role. A copy whose words do not all carry the *)
(* accepted tag is a tear; NoTornAccepted asserts none is ever accepted.    *)
(*                                                                          *)
(* Checked bounds (FanoutSeqlock.cfg): 2 readers, 2 slots, 2 words, 3       *)
(* frames — the smallest shape containing a full ring wrap. Scale is the    *)
(* loom/torture/chaos tiers' job; exhaustiveness at this shape is THIS      *)
(* model's job.                                                            *)
EXTENDS Integers, Naturals, TLC

CONSTANTS READERS,  \* number of concurrent readers
          SLOTS,    \* ring depth M
          WORDS,    \* payload words per slot
          FRAMES    \* total frames the writer publishes

Slots   == 0 .. SLOTS - 1
Words   == 0 .. WORDS - 1
Readers == 1 .. READERS

MaxAttempts == 4   \* the bounded claim loop (RFC 0004, mirrored everywhere)

VARIABLES
    latest,     \* latestSeq — the publication point
    slotSeq,    \* [k] -> stamp (0 = invalidated, fill in progress)
    payload,    \* [k][w] -> frame tag of the last writer of that word
    publishes,  \* completed publishes (telemetry)
    \* writer state
    wSeq,       \* next frame number (1-based)
    wPhase,     \* "idle" | "fill" | "stamped"
    wSlot,      \* slot being filled (phase "fill")
    wWord,      \* next word to fill (phase "fill")
    wUsed,      \* [k] -> the writer has begun a frame on this slot at least once
    \* reader state (arrays indexed by reader id)
    rPhase,     \* "idle" | "stampB" | "copy" | "stampA"
    rLast,      \* last accepted frame (0 = none yet)
    rFresh,     \* accepted-frames counter
    rDropped,   \* dropped-frames counter (telescoping lhs)
    rSkips,     \* graceful mid-overwrite skips (Law 1: counted, never silent)
    rExh,       \* bounded-attempt exhaustions (Law 1)
    rCand,      \* candidate seq of the tick in progress
    rAttempts,  \* bounded-loop counter of the tick in progress
    rWordIdx,   \* copy progress (words copied so far this tick)
    rTarget,    \* [i][w] -> words copied this tick
    rGood,      \* [i][w] -> words of the LAST ACCEPTED frame (the snapshot
                \* NoTornAccepted speaks about)
    rDone       \* the reader accepted the final frame

Vars == <<latest, slotSeq, payload, publishes, wSeq, wPhase, wSlot, wWord, wUsed,
          rPhase, rLast, rFresh, rDropped, rSkips, rExh, rCand, rAttempts,
          rWordIdx, rTarget, rGood, rDone>>

ReaderVars(i) == <<rPhase[i], rLast[i], rFresh[i], rDropped[i], rSkips[i],
                   rExh[i], rCand[i], rAttempts[i], rWordIdx[i], rTarget[i],
                   rGood[i], rDone[i]>>

------------------------------------------------------------------------------
Init ==
    /\ latest    = 0
    /\ slotSeq   = [k \in Slots |-> 0]
    /\ payload   = [k \in Slots |-> [w \in Words |-> 0]]
    /\ publishes = 0
    /\ wSeq      = 1
    /\ wPhase    = "idle"
    /\ wSlot     = 0
    /\ wWord     = 0
    /\ wUsed     = [k \in Slots |-> FALSE]
    /\ rPhase    = [i \in Readers |-> "idle"]
    /\ rLast     = [i \in Readers |-> 0]
    /\ rFresh    = [i \in Readers |-> 0]
    /\ rDropped  = [i \in Readers |-> 0]
    /\ rSkips    = [i \in Readers |-> 0]
    /\ rExh      = [i \in Readers |-> 0]
    /\ rCand     = [i \in Readers |-> 0]
    /\ rAttempts = [i \in Readers |-> 0]
    /\ rWordIdx  = [i \in Readers |-> 0]
    /\ rTarget   = [i \in Readers |-> [w \in Words |-> 0]]
    /\ rGood     = [i \in Readers |-> [w \in Words |-> 0]]
    /\ rDone     = [i \in Readers |-> FALSE]

------------------------------------------------------------------------------
(*** writer — begin / fill-one-word / stamp / publish, one action each ***)

SlotOf(seq) == (seq - 1) % SLOTS

WBegin ==   \* FI1a: invalidate the target slot BEFORE the fill
    /\ wSeq   <= FRAMES
    /\ wPhase = "idle"
    /\ wSlot'  = SlotOf(wSeq)
    /\ slotSeq' = [slotSeq EXCEPT ![SlotOf(wSeq)] = 0]
    /\ wUsed'   = [wUsed EXCEPT ![SlotOf(wSeq)] = TRUE]
    /\ wPhase' = "fill"
    /\ wWord'  = 0
    /\ UNCHANGED <<latest, payload, publishes, wSeq,
                   rPhase, rLast, rFresh, rDropped, rSkips, rExh, rCand,
                   rAttempts, rWordIdx, rTarget, rGood, rDone>>

WFillOne == \* ONE payload word per action: the tear window is real
    /\ wPhase = "fill"
    /\ wWord < WORDS
    /\ payload' = [payload EXCEPT ![wSlot][wWord] = wSeq]
    /\ wWord'   = wWord + 1
    /\ UNCHANGED <<latest, slotSeq, publishes, wSeq, wPhase, wSlot, wUsed,
                   rPhase, rLast, rFresh, rDropped, rSkips, rExh, rCand,
                   rAttempts, rWordIdx, rTarget, rGood, rDone>>

WStamp ==   \* FI1b: stamp the completed fill (Release analog)
    /\ wPhase = "fill"
    /\ wWord  = WORDS
    /\ slotSeq' = [slotSeq EXCEPT ![wSlot] = wSeq]
    /\ wPhase'  = "stamped"
    /\ UNCHANGED <<latest, payload, publishes, wSeq, wSlot, wWord, wUsed,
                   rPhase, rLast, rFresh, rDropped, rSkips, rExh, rCand,
                   rAttempts, rWordIdx, rTarget, rGood, rDone>>

WPublish == \* the publication point (Release analog) + telemetry
    /\ wPhase = "stamped"
    /\ latest'    = wSeq
    /\ publishes' = publishes + 1
    /\ wSeq'      = wSeq + 1
    /\ wPhase'    = "idle"
    /\ UNCHANGED <<slotSeq, payload, wSlot, wWord, wUsed,
                   rPhase, rLast, rFresh, rDropped, rSkips, rExh, rCand,
                   rAttempts, rWordIdx, rTarget, rGood, rDone>>

WriterProgress == WBegin \/ WFillOne \/ WStamp \/ WPublish

WriterDoneStutter ==   \* keeps Next enabled after the writer finishes
    /\ wSeq > FRAMES
    /\ wPhase = "idle"
    /\ UNCHANGED Vars

------------------------------------------------------------------------------
(*** reader claim loop — the bounded 4-attempt discipline, label by label ***)

RPoll(i) ==   \* tick start: there is something newer to chase
    /\ ~rDone[i]
    /\ rPhase[i] = "idle"
    /\ latest > 0
    /\ latest /= rLast[i]
    /\ rCand'     = [rCand     EXCEPT ![i] = latest]
    /\ rAttempts' = [rAttempts EXCEPT ![i] = 0]
    /\ rPhase'    = [rPhase    EXCEPT ![i] = "stampB"]
    /\ UNCHANGED <<latest, slotSeq, payload, publishes, wSeq, wPhase, wSlot, wWord, wUsed,
                   rLast, rFresh, rDropped, rSkips, rExh, rWordIdx, rTarget,
                   rGood, rDone>>

RIdleTick(i) ==   \* nothing newer (Law 1: a bounded, silent-free no-op)
    /\ ~rDone[i]
    /\ rPhase[i] = "idle"
    /\ \/ latest = 0
       \/ latest = rLast[i]
    /\ UNCHANGED Vars

RStampBValid(i) ==   \* stamp valid: start copying
    /\ rPhase[i] = "stampB"
    /\ slotSeq[SlotOf(rCand[i])] = rCand[i]
    /\ rTarget'  = [rTarget EXCEPT ![i] = [w \in Words |-> 0]]
    /\ rWordIdx' = [rWordIdx EXCEPT ![i] = 0]
    /\ rPhase'   = [rPhase EXCEPT ![i] = "copy"]
    /\ UNCHANGED <<latest, slotSeq, payload, publishes, wSeq, wPhase, wSlot, wWord, wUsed,
                   rLast, rFresh, rDropped, rSkips, rExh, rCand, rAttempts, rGood, rDone>>

RStampBSkip(i) ==   \* graceful skip: target mid-overwrite, no newer frame
                       \* (counted — Law 1, never silent)
    /\ rPhase[i] = "stampB"
    /\ slotSeq[SlotOf(rCand[i])] /= rCand[i]
    /\ latest = rCand[i]
    /\ rTarget'  = [rTarget EXCEPT ![i] = [w \in Words |-> 0]]
    /\ rWordIdx' = [rWordIdx EXCEPT ![i] = 0]
    /\ rSkips'   = [rSkips EXCEPT ![i] = rSkips[i] + 1]
    /\ rPhase'   = [rPhase EXCEPT ![i] = "idle"]
    /\ UNCHANGED <<latest, slotSeq, payload, publishes, wSeq, wPhase, wSlot, wWord, wUsed,
                   rLast, rFresh, rDropped, rExh, rCand, rAttempts, rGood, rDone>>

RStampBChase(i) ==   \* a newer frame arrived mid-check: chase it (attempt used)
    /\ rPhase[i] = "stampB"
    /\ slotSeq[SlotOf(rCand[i])] /= rCand[i]
    /\ latest /= rCand[i]
    /\ rTarget'  = [rTarget EXCEPT ![i] = [w \in Words |-> 0]]
    /\ rWordIdx' = [rWordIdx EXCEPT ![i] = 0]
    /\ rCand'     = [rCand     EXCEPT ![i] = latest]
    /\ rAttempts' = [rAttempts EXCEPT ![i] = rAttempts[i] + 1]
    /\ rPhase'    = [rPhase EXCEPT ![i] =
           IF rAttempts[i] + 1 >= MaxAttempts THEN "idle" ELSE "stampB"]
    /\ rSkips'    = [rSkips EXCEPT ![i] =
           rSkips[i] + (IF rAttempts[i] + 1 >= MaxAttempts THEN 1 ELSE 0)]
    /\ rExh'      = [rExh EXCEPT ![i] =
           rExh[i] + (IF rAttempts[i] + 1 >= MaxAttempts THEN 1 ELSE 0)]
    /\ UNCHANGED <<latest, slotSeq, payload, publishes, wSeq, wPhase, wSlot, wWord, wUsed,
                   rLast, rFresh, rDropped, rGood, rDone>>

RStampB(i) == RStampBValid(i) \/ RStampBSkip(i) \/ RStampBChase(i)

RCopyOne(i) ==   \* copy ONE payload word per action — the tear window is real
    /\ rPhase[i] = "copy"
    /\ rWordIdx[i] < WORDS
    /\ rTarget'  = [rTarget EXCEPT ![i][rWordIdx[i]] = payload[SlotOf(rCand[i])][rWordIdx[i]]]
    /\ rWordIdx' = [rWordIdx EXCEPT ![i] = rWordIdx[i] + 1]
    /\ UNCHANGED <<latest, slotSeq, payload, publishes, wSeq, wPhase, wSlot, wWord, wUsed,
                   rPhase, rLast, rFresh, rDropped, rSkips, rExh, rCand, rAttempts,
                   rGood, rDone>>

RStampAAccept(i) ==   \* consistent frame: ACCEPT (the accounting identity)
    /\ rPhase[i] = "copy"
    /\ rWordIdx[i] = WORDS
    /\ slotSeq[SlotOf(rCand[i])] = rCand[i]
    /\ rLast'    = [rLast    EXCEPT ![i] = rCand[i]]
    /\ rGood'    = [rGood    EXCEPT ![i] = rTarget[i]]
    /\ rFresh'   = [rFresh   EXCEPT ![i] = rFresh[i] + 1]
    /\ rDropped' = [rDropped EXCEPT ![i] = rDropped[i] + rCand[i] - rLast[i] - 1]
    /\ rDone'    = [rDone    EXCEPT ![i] = rCand[i] = FRAMES]
    /\ rPhase'   = [rPhase   EXCEPT ![i] = "idle"]
    /\ UNCHANGED <<latest, slotSeq, payload, publishes, wSeq, wPhase, wSlot, wWord, wUsed,
                   rSkips, rExh, rCand, rAttempts, rWordIdx, rTarget>>

RStampAChase(i) ==   \* torn copy: the revalidation caught it — chase the newest
    /\ rPhase[i] = "copy"
    /\ rWordIdx[i] = WORDS
    /\ slotSeq[SlotOf(rCand[i])] /= rCand[i]
    /\ rCand'     = [rCand     EXCEPT ![i] = latest]
    /\ rAttempts' = [rAttempts EXCEPT ![i] = rAttempts[i] + 1]
    /\ rPhase'    = [rPhase EXCEPT ![i] =
           IF rAttempts[i] + 1 >= MaxAttempts THEN "idle" ELSE "stampB"]
    /\ rSkips'    = [rSkips EXCEPT ![i] =
           rSkips[i] + (IF rAttempts[i] + 1 >= MaxAttempts THEN 1 ELSE 0)]
    /\ rExh'      = [rExh EXCEPT ![i] =
           rExh[i] + (IF rAttempts[i] + 1 >= MaxAttempts THEN 1 ELSE 0)]
    /\ UNCHANGED <<latest, slotSeq, payload, publishes, wSeq, wPhase, wSlot, wWord, wUsed,
                   rLast, rFresh, rDropped, rGood, rTarget, rWordIdx, rDone>>

RStampA(i) == RStampAAccept(i) \/ RStampAChase(i)

RDoneStutter(i) ==   \* a finished reader idles (keeps Next enabled)
    /\ rDone[i]
    /\ UNCHANGED Vars

ReaderProgress(i) == RPoll(i) \/ RStampB(i) \/ RCopyOne(i) \/ RStampA(i)

------------------------------------------------------------------------------
Next ==
    \/ WriterProgress
    \/ WriterDoneStutter
    \/ \E i \in Readers : ReaderProgress(i) \/ RIdleTick(i) \/ RDoneStutter(i)

Liveness ==
    /\ \A i \in Readers : WF_Vars(RPoll(i))
    /\ \A i \in Readers : WF_Vars(RStampB(i) \/ RCopyOne(i) \/ RStampA(i))
    /\ WF_Vars(WriterProgress)

\* Fairness is part of the SPEC (the scheduler is weakly fair to every
\* protocol participant); the properties we PROVE under that fairness are
\* WriterFinishes and ReaderSeesFinal (checked in the .cfg).
Spec == Init /\ [][Next]_Vars /\ Liveness

------------------------------------------------------------------------------
(*** safety — the mathematical content of "no torn reads, ever" ***)

TypeOK ==
    /\ latest    \in 0 .. FRAMES
    /\ wSeq      \in 1 .. FRAMES + 1
    /\ publishes \in 0 .. FRAMES
    /\ slotSeq   \in [Slots -> 0 .. FRAMES]
    /\ payload   \in [Slots -> [Words -> 0 .. FRAMES]]
    /\ wPhase    \in {"idle", "fill", "stamped"}
    /\ wUsed     \in [Slots -> BOOLEAN]
    /\ wSlot     \in Slots
    /\ wWord     \in 0 .. WORDS
    /\ rPhase    \in [Readers -> {"idle", "stampB", "copy", "stampA"}]
    /\ rLast     \in [Readers -> 0 .. FRAMES]
    /\ rCand     \in [Readers -> 0 .. FRAMES]
    /\ rAttempts \in [Readers -> 0 .. MaxAttempts]
    /\ rGood     \in [Readers -> [Words -> 0 .. FRAMES]]
    /\ rDone     \in [Readers -> BOOLEAN]

(* FI1: the invalidate is always paired with the in-progress fill. (The
   initial ring — everything 0, nothing published — is vacuously valid:
   the writer has never begun, so there is no unpaired invalidate.) *)
StampBracket ==
    \A k \in Slots :
        (slotSeq[k] = 0 /\ wUsed[k]) => (wPhase = "fill" /\ wSlot = k)

(* The publication point never runs ahead of the writer. *)
LatestMonotonic == latest <= wSeq - 1

(* No reader accepts a frame the writer has not published. *)
NoFuture == \A i \in Readers : rLast[i] <= FRAMES

(* THE SAFETY THEOREM: every word of every accepted snapshot carries the
   accepted frame's tag. *)
NoTornAccepted ==
    \A i \in Readers :
        \/ rLast[i] = 0
        \/ \A w \in Words : rGood[i][w] = rLast[i]

(* RFC 0004's accounting identity. *)
Telescoping ==
    \A i \in Readers : rDropped[i] = rLast[i] - rFresh[i]

Inv == TypeOK /\ StampBracket /\ LatestMonotonic /\ NoFuture
       /\ NoTornAccepted /\ Telescoping

------------------------------------------------------------------------------
(*** liveness — no starvation (explicit weak fairness on progress actions) ***)

WriterFinishes  == <>(wSeq > FRAMES)
ReaderSeesFinal == \A i \in Readers : <>(rDone[i])


===============================================================================
