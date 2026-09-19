---------------------------- MODULE TriadExchange ----------------------------
(* Issue #16 Tier 1 / Task 1 — Formal model of the Triad core kernel:        *)
(* publish / claim / revoke / reclaim over the THREE-buffer, ONE-atomic      *)
(* exchange (02-KERNEL).                                                     *)
(*                                                                           *)
(* WHAT THIS MODEL IS: the kernel protocol at the granularity of its atomic  *)
(* operations. The writer's fill-one-word / envelope / canary / exchange     *)
(* steps, the reader's exchange / observe / verify steps, and the I6         *)
(* revocation handshake (revoke -> ACK -> free) are separate ATOMIC actions, *)
(* so TLC explores every interleaving the protocol permits.                  *)
(*                                                                           *)
(* THE ORDERING QUESTION (the issue's "prove memory_order_acq_rel is        *)
(* sufficient; no seq_cst needed") is answered with a REGIME constant:       *)
(*                                                                           *)
(*   Regime = 1 ("acq_rel", production): the latest.exchange that hands a    *)
(*     buffer to the reader SYNCHRONIZES with it — the reader's loads of    *)
(*     that buffer return the state the writer stored before its release-   *)
(*     half. In the model, observations are deterministic: rObs == the      *)
(*     shared buffer state. Coherent, monotone, canary-true — PROVEN.       *)
(*                                                                           *)
(*   Regime = 2 ("relaxed", the counterfactual): the exchange is STILL ONE  *)
(*     atomic action (per-location coherence of a single RMW is not what    *)
(*     acq_rel buys) — ownership transfer is correct — but the ordering of  *)
(*     the writer's buffer stores before the exchange is DROPPED: each      *)
(*     reader load may independently return any OLDER value that the same   *)
(*     location has ever held (per-location coherence; tracked faithfully   *)
(*     by the *Hist variables — a load may NOT invent a value a different   *)
(*     buffer held, so every counterexample TLC finds is real-relaxed-      *)
(*     legal). TLC FINDS the counterexample: a claim round whose envelope   *)
(*     load sees frame k while its canary load sees an older tag            *)
(*     (CanaryIntegrity violated), or whose payload words mix frames       *)
(*     (TornPayload), or whose observed frame is older than a previously    *)
(*     observed one (ObservedMonotone). The ordering is therefore           *)
(*     NECESSARY — weaker than acq_rel breaks it.                           *)
(*                                                                           *)
(*   Why seq_cst is NOT needed: the kernel synchronizes buffer ownership    *)
(*   through exactly ONE atomic location (02 §2.2 — the single shared       *)
(*   atomic). Per-location coherence totally orders every exchange; buffer  *)
(*   contents are partitioned by ownership (A1: each buffer is touched      *)
(*   only by its current owner), so no invariant ever consults a global     *)
(*   order across two locations. The acq_rel regime model checks clean     *)
(*   with ONLY the per-exchange release/acquire pairing — that is the      *)
(*   sufficiency proof at this granularity. (The revoked load is Relaxed    *)
(*   and the epoch poll is Acquire in production; the model keeps both      *)
(*   faithfully — see wRevSeen / relEpoch, which lag and catch up via       *)
(*   explicit propagation actions in BOTH regimes.)                         *)
(*                                                                           *)
(* THE REVOCATION QUESTION ("prove no use-after-free in the revocation      *)
(* handshake") is answered with a FreeMode constant:                        *)
(*                                                                           *)
(*   FreeMode = 1 ("after_ack", production): free only after the releaser   *)
(*     observed the writer's epoch ACK. NoUseAfterFree holds — the ACK      *)
(*     happens at a publish ENTRY (the in-flight frame's writes completed   *)
(*     before the entry that ACKs), and once wRevSeen is set no new frame   *)
(*     begins, so no writer buffer access can follow the free. PROVEN.      *)
(*                                                                           *)
(*   FreeMode = 2 ("premature", the counterfactual): free immediately after *)
(*     the revoke store, while the writer may be mid-frame. TLC FINDS the   *)
(*     A1 bug: freed while wPhase = "fill" — the use-after-free the I6      *)
(*     handshake exists to prevent.                                         *)
(*                                                                           *)
(* USAGE DISCIPLINE (modeled, per 02 §2.2): the exchange couples the        *)
(* reader's claim rate to the writer's publish rate — a reader that claims  *)
(* twice between publishes just swaps buffers with itself and can observe   *)
(* its own older frame (the kernel's claim returns whatever buffer is in    *)
(* `latest`, including the one the reader handed back). The model gates     *)
(* RClaim on "the buffer in latest carries a frame newer than the last      *)
(* completed observation" — the paced-reader discipline every litmus test   *)
(* and every heddle enforces. The gate's read is application-side (SC in    *)
(* the model, declared); all memory-model-sensitive loads are the           *)
(* regime-parameterized observation actions.                                *)
(*                                                                           *)
(* Safety (invariants over the whole reachable state space):                 *)
(*   - TypeOK             — every variable stays in its declared shape      *)
(*   - OwnershipPartition — I1: latest, w_work, r_work are pairwise         *)
(*                          distinct (the three buffers are partitioned     *)
(*                          among in-exchange / writer / reader at every    *)
(*                          instant — THE construction)                     *)
(*   - CanaryIntegrity    — L12: every COMPLETED observation round has      *)
(*                          envelope seq == canary (the kernel's own        *)
(*                          mid-flight-read detector never fires on legal   *)
(*                          paths)                                          *)
(*   - TornPayload        — L1: every word of every COMPLETED round         *)
(*                          carries the round's envelope tag                *)
(*   - ObservedMonotone   — L4: the reader never observes an OLDER frame    *)
(*                          than one it already observed                    *)
(*   - NoUseAfterFree     — I6/A1: after the free, the writer is not       *)
(*                          mid-frame on any buffer (reader-side post-free  *)
(*                          access is caller discipline, per 02 §3 — the    *)
(*                          kernel guarantees the WRITER handshake only;    *)
(*                          declared, not defended)                         *)
(*   - AckBeforeFree      — the I6 statement itself: free implies the       *)
(*                          releaser observed epoch advance                 *)
(* Liveness (weak fairness on every participant, production cfg only):      *)
(*   - WriterTerminates   — finishes all frames or ACKs out                *)
(*   - DrainUnrevoked    — if the releaser never starts, the reader          *)
(*                          eventually observes the final frame            *)
(*   - HandshakeCompletes — once the releaser starts, the ACK chain and     *)
(*                          free eventually happen (even under the         *)
(*                          faithful Relaxed revoked-load lag — eventual    *)
(*                          visibility + the poll loop are the protocol's   *)
(*                          own answer)                                     *)
(*                                                                           *)
(* Telemetry counters are advisory and never synchronization (AXIOM T); they *)
(* are deliberately absent — they carry no theorem. The null frame (seq=0 on *)
(* all three buffers at init) is modeled faithfully: the reader's first     *)
(* claim returns a well-formed frame 0.                                      *)
(*                                                                           *)
(* Checked bounds (TriadAcqRel.cfg): FRAMES = 3, WORDS = 2 — three full     *)
(* buffer rotations (every buffer is published into at least once) and the  *)
(* null frame. Scale is the litmus/torture/chaos tiers' job; exhaustiveness *)
(* at this shape is THIS model's job.                                       *)
EXTENDS Integers, Naturals, TLC

CONSTANTS FRAMES,  \* frames the writer publishes (1-based; 0 = null frame)
          WORDS,   \* payload words per buffer
          Regime,  \* 1 = acq_rel (production) | 2 = relaxed (counterfactual)
          FreeMode \* 1 = after_ack (production) | 2 = premature (A1 bug)

Buffers == 0 .. 2
Words    == 0 .. WORDS - 1
Tags     == 0 .. FRAMES
IsAcqRel == (Regime = 1)

\* Observation steps within a reader round: 0 = envelope, 1 = canary,
\* 2 + w = payload word w, 2 + WORDS = verify (round complete).
StepEnv    == 0
StepCan    == 1
StepWord(w) == 2 + w
StepVerify == 2 + WORDS

VARIABLES
    \* the kernel's single shared atomic + the two private indices (I1)
    latest,     \* buffer index in exchange (init 0)
    wWork,      \* writer-private buffer index (init 1)
    rWork,      \* reader-private buffer index (init 2)
    \* buffer contents (per field: the frame tag that last wrote it)
    bufEnv,     \* [b] -> envelope seq tag   (written at WEnv)
    bufCan,     \* [b] -> canary tag         (written at WCan)
    bufWord,    \* [b][w] -> payload tag     (written at WFillOne)
    \* per-location write histories — the faithful relaxed-load domain.
    \* A relaxed load of a location may return any value THAT location has
    \* ever held (coherence), never a value only a different location held.
    envHist,    \* [b] -> set of tags bufEnv[b] has held
    canHist,    \* [b] -> set of tags bufCan[b] has held
    wordHist,   \* [b][w] -> set of tags bufWord[b][w] has held
    \* writer state
    wSeq,       \* next frame number (1-based)
    wPhase,     \* "idle" | "fill" | "env" | "can" | "done"
    wWord,      \* fill progress (phase "fill")
    \* I6 handshake state
    revoked,    \* the revoke store (Release in production)
    wRevSeen,   \* writer's view of revoked (Relaxed load — lags, faithful)
    epoch,      \* ACK counter (fetch_add AcqRel in production)
    relEpoch,   \* releaser's view of epoch (Acquire poll — lags, faithful)
    relPhase,   \* "idle" | "revoked"
    e0,         \* epoch value the releaser took before the revoke store
    freed,      \* the free (poison/free licensed by the ACK)
    \* reader state
    rPhase,     \* "idle" | "observing"
    rStep,      \* StepEnv | StepCan | StepWord(w) | StepVerify
    rObsEnv,    \* this round's envelope observation
    rObsCan,    \* this round's canary observation
    rObsWord,   \* [w] -> this round's payload observations
    rDoneTag,   \* envelope tag of the last COMPLETED round
    rBadCanary, \* a completed round failed the canary check (L12 detector)
    rTorn,      \* a completed round mixed payload frames (L1 detector)
    rStale      \* a completed round regressed below a previous one (L4 detector)

Vars == <<latest, wWork, rWork, bufEnv, bufCan, bufWord, envHist, canHist,
          wordHist, wSeq, wPhase, wWord, revoked, wRevSeen, epoch, relEpoch,
          relPhase, e0, freed, rPhase, rStep, rObsEnv, rObsCan, rObsWord,
          rDoneTag, rBadCanary, rTorn, rStale>>

------------------------------------------------------------------------------
Init ==
    /\ latest  = 0
    /\ wWork   = 1
    /\ rWork   = 2
    /\ bufEnv  = [b \in Buffers |-> 0]        \* null frames everywhere
    /\ bufCan  = [b \in Buffers |-> 0]
    /\ bufWord = [b \in Buffers |-> [w \in Words |-> 0]]
    /\ envHist  = [b \in Buffers |-> {0}]
    /\ canHist  = [b \in Buffers |-> {0}]
    /\ wordHist = [b \in Buffers |-> [w \in Words |-> {0}]]
    /\ wSeq    = 1
    /\ wPhase  = "idle"
    /\ wWord   = 0
    /\ revoked  = FALSE
    /\ wRevSeen = FALSE
    /\ epoch    = 0
    /\ relEpoch = 0
    /\ relPhase = "idle"
    /\ e0       = 0
    /\ freed    = FALSE
    /\ rPhase   = "idle"
    /\ rStep    = StepEnv
    /\ rObsEnv  = 0
    /\ rObsCan  = 0
    /\ rObsWord = [w \in Words |-> 0]
    /\ rDoneTag = 0
    /\ rBadCanary = FALSE
    /\ rTorn     = FALSE
    /\ rStale    = FALSE

------------------------------------------------------------------------------
(*** writer — the publish cycle of 02 §2, label by label ***)

\* Publish entry: the revoked check happens FIRST, before any byte write.
\* The load is Relaxed in production: wRevSeen lags (PropWRev below).
WBegin ==
    /\ wPhase = "idle"
    /\ wSeq <= FRAMES
    /\ ~wRevSeen
    /\ wPhase' = "fill"
    /\ wWord'  = 0
    /\ UNCHANGED <<latest, wWork, rWork, bufEnv, bufCan, bufWord, envHist,
                   canHist, wordHist, wSeq,
                   revoked, wRevSeen, epoch, relEpoch, relPhase, e0, freed,
                   rPhase, rStep, rObsEnv, rObsCan, rObsWord, rDoneTag,
                   rBadCanary, rTorn, rStale>>

\* The ACK: publish saw revoked -> epoch.fetch_add(1, AcqRel), no byte write.
\* After the first ACK the writer is modeled "done" — every later publish
\* attempt is protocol-identical (check revoked -> ACK -> DROPPED).
WAck ==
    /\ wPhase = "idle"
    /\ wRevSeen
    /\ epoch'  = epoch + 1
    /\ wPhase' = "done"
    /\ UNCHANGED <<latest, wWork, rWork, bufEnv, bufCan, bufWord, envHist,
                   canHist, wordHist, wSeq, wWord,
                   revoked, wRevSeen, relEpoch, relPhase, e0, freed,
                   rPhase, rStep, rObsEnv, rObsCan, rObsWord, rDoneTag,
                   rBadCanary, rTorn, rStale>>

\* ONE payload word per action — the tear window is real in the model.
WFillOne ==
    /\ wPhase = "fill"
    /\ wWord < WORDS
    /\ bufWord' = [bufWord EXCEPT ![wWork][wWord] = wSeq]
    /\ wordHist' = [wordHist EXCEPT ![wWork][wWord] =
                        wordHist[wWork][wWord] \cup {wSeq}]
    /\ wWord'   = wWord + 1
    /\ UNCHANGED <<latest, wWork, rWork, bufEnv, bufCan, envHist, canHist,
                   wSeq, wPhase,
                   revoked, wRevSeen, epoch, relEpoch, relPhase, e0, freed,
                   rPhase, rStep, rObsEnv, rObsCan, rObsWord, rDoneTag,
                   rBadCanary, rTorn, rStale>>

WEnv ==      \* envelope write (head)
    /\ wPhase = "fill"
    /\ wWord  = WORDS
    /\ bufEnv' = [bufEnv EXCEPT ![wWork] = wSeq]
    /\ envHist' = [envHist EXCEPT ![wWork] = envHist[wWork] \cup {wSeq}]
    /\ wPhase' = "env"
    /\ UNCHANGED <<latest, wWork, rWork, bufCan, bufWord, canHist, wordHist,
                   wSeq, wWord,
                   revoked, wRevSeen, epoch, relEpoch, relPhase, e0, freed,
                   rPhase, rStep, rObsEnv, rObsCan, rObsWord, rDoneTag,
                   rBadCanary, rTorn, rStale>>

WCan ==      \* canary write (tail) — the L12 redundancy, planted after the envelope
    /\ wPhase = "env"
    /\ bufCan' = [bufCan EXCEPT ![wWork] = wSeq]
    /\ canHist' = [canHist EXCEPT ![wWork] = canHist[wWork] \cup {wSeq}]
    /\ wPhase' = "can"
    /\ UNCHANGED <<latest, wWork, rWork, bufEnv, bufWord, envHist, wordHist,
                   wSeq, wWord,
                   revoked, wRevSeen, epoch, relEpoch, relPhase, e0, freed,
                   rPhase, rStep, rObsEnv, rObsCan, rObsWord, rDoneTag,
                   rBadCanary, rTorn, rStale>>

\* THE atomic: latest.exchange(wWork, AcqRel). One action — the RMW is
\* atomic under EVERY regime (that is per-location coherence, not ordering).
WExchange ==
    /\ wPhase = "can"
    /\ latest' = wWork
    /\ wWork'  = latest
    /\ wSeq'   = wSeq + 1
    /\ wPhase' = "idle"
    /\ UNCHANGED <<rWork, bufEnv, bufCan, bufWord, envHist, canHist, wordHist,
                   wWord,
                   revoked, wRevSeen, epoch, relEpoch, relPhase, e0, freed,
                   rPhase, rStep, rObsEnv, rObsCan, rObsWord, rDoneTag,
                   rBadCanary, rTorn, rStale>>

WriterProgress == WBegin \/ WAck \/ WFillOne \/ WEnv \/ WCan \/ WExchange

WriterStutter ==
    /\ \/ wPhase = "done"
       \/ (wPhase = "idle" /\ wSeq > FRAMES)
       \/ (wPhase = "idle" /\ wRevSeen)
    /\ UNCHANGED Vars

------------------------------------------------------------------------------
(*** reader — claim, then observe field by field, then verify ***)

\* THE atomic: latest.exchange(rWork, AcqRel). Always succeeds (02 §2.2:
\* a claim always yields a buffer). Index swap only — no buffer memory is
\* touched. GATED on the paced-reader usage discipline (see header): claim
\* when the buffer in latest carries a frame newer than the last completed
\* observation (or nothing has been observed yet — the null first claim).
RClaim ==
    /\ rPhase = "idle"
    /\ \/ bufEnv[latest] > rDoneTag
       \/ rDoneTag = 0
    /\ rPhase' = "observing"
    /\ rStep'  = StepEnv
    /\ rWork'  = latest
    /\ latest' = rWork
    /\ UNCHANGED <<wWork, bufEnv, bufCan, bufWord, envHist, canHist, wordHist,
                   wSeq, wPhase, wWord,
                   revoked, wRevSeen, epoch, relEpoch, relPhase, e0, freed,
                   rObsEnv, rObsCan, rObsWord, rDoneTag,
                   rBadCanary, rTorn, rStale>>

\* The envelope load. acq_rel: deterministic — the state the writer stored
\* before its release-half (the writer cannot touch the reader's buffer).
\* relaxed: any value this location has ever held (envHist — coherence).
ObsEnvChoices == IF IsAcqRel THEN {bufEnv[rWork]} ELSE envHist[rWork]

RObsEnv ==
    /\ rPhase = "observing"
    /\ rStep  = StepEnv
    /\ ~freed
    /\ \E v \in ObsEnvChoices : rObsEnv' = v
    /\ rStep' = StepCan
    /\ UNCHANGED <<latest, wWork, rWork, bufEnv, bufCan, bufWord, envHist,
                   canHist, wordHist, wSeq, wPhase, wWord,
                   revoked, wRevSeen, epoch, relEpoch, relPhase, e0, freed,
                   rPhase,
                   rObsCan, rObsWord, rDoneTag, rBadCanary, rTorn, rStale>>

ObsCanChoices == IF IsAcqRel THEN {bufCan[rWork]} ELSE canHist[rWork]

RObsCan ==
    /\ rPhase = "observing"
    /\ rStep  = StepCan
    /\ ~freed
    /\ \E v \in ObsCanChoices : rObsCan' = v
    /\ rStep' = StepWord(0)
    /\ UNCHANGED <<latest, wWork, rWork, bufEnv, bufCan, bufWord, envHist,
                   canHist, wordHist, wSeq, wPhase, wWord,
                   revoked, wRevSeen, epoch, relEpoch, relPhase, e0, freed,
                   rPhase,
                   rObsEnv, rObsWord, rDoneTag, rBadCanary, rTorn, rStale>>

ObsWordChoices(w) ==
    IF IsAcqRel THEN {bufWord[rWork][w]} ELSE wordHist[rWork][w]

RObsWord(w) ==
    /\ rPhase = "observing"
    /\ rStep  = StepWord(w)
    /\ ~freed
    /\ \E v \in ObsWordChoices(w) :
           rObsWord' = [rObsWord EXCEPT ![w] = v]
    /\ rStep' = IF w + 1 < WORDS THEN StepWord(w + 1) ELSE StepVerify
    /\ UNCHANGED <<latest, wWork, rWork, bufEnv, bufCan, bufWord, envHist,
                   canHist, wordHist, wSeq, wPhase, wWord,
                   revoked, wRevSeen, epoch, relEpoch, relPhase, e0, freed,
                   rPhase,
                   rObsEnv, rObsCan, rDoneTag, rBadCanary, rTorn, rStale>>

\* Round complete. The three detectors are the litmus checks, executed here
\* as state (so TLC counterexample traces END exactly at the caught fault):
\*   canary: env == can?         (L12 — the kernel's own redundancy check)
\*   torn:   every word == env?  (L1 — pattern validation)
\*   stale:  env >= last round?  (L4 — freshness)
RVerify ==
    /\ rPhase = "observing"
    /\ rStep  = StepVerify
    /\ rDoneTag'    = rObsEnv
    /\ rBadCanary'  = (rBadCanary \/ (rObsEnv /= rObsCan))
    /\ rTorn'       = (rTorn \/ (\E w \in Words : rObsWord[w] /= rObsEnv))
    /\ rStale'      = (rStale \/ (rObsEnv < rDoneTag))
    /\ rPhase'      = "idle"
    /\ rStep'       = StepEnv
    /\ UNCHANGED <<latest, wWork, rWork, bufEnv, bufCan, bufWord, envHist,
                   canHist, wordHist, wSeq, wPhase, wWord,
                   revoked, wRevSeen, epoch, relEpoch, relPhase, e0, freed,
                   rObsEnv, rObsCan, rObsWord>>

ReaderActions == RClaim \/ RObsEnv \/ RObsCan
                 \/ \E w \in Words : RObsWord(w) \/ RVerify

ReaderIdleTick ==
    /\ rPhase = "idle"
    /\ UNCHANGED Vars

------------------------------------------------------------------------------
(*** I6 — the revocation handshake, label by label (02 §6) ***)

\* Propagation of the Relaxed revoked load to the writer. Eventual (WF) —
\* faithful to coherence: the store becomes visible, the advisory load
\* eventually observes it; until then the writer keeps publishing safely.
PropWRev ==
    /\ revoked
    /\ ~wRevSeen
    /\ wRevSeen' = TRUE
    /\ UNCHANGED <<latest, wWork, rWork, bufEnv, bufCan, bufWord, envHist,
                   canHist, wordHist, wSeq, wPhase, wWord,
                   epoch, relEpoch, relPhase, e0, freed,
                   rPhase, rStep, rObsEnv, rObsCan, rObsWord, rDoneTag,
                   rBadCanary, rTorn, rStale, revoked>>

\* Propagation of epoch to the releaser's Acquire poll — the poll loop.
\* Each attempt observes some value not behind its last view; WF eventually
\* catches up to the ACK.
PropRelEpoch ==
    /\ relEpoch < epoch
    /\ \E v \in relEpoch .. epoch : relEpoch' = v
    /\ UNCHANGED <<latest, wWork, rWork, bufEnv, bufCan, bufWord, envHist,
                   canHist, wordHist, wSeq, wPhase, wWord,
                   revoked, wRevSeen, relPhase, e0, freed,
                   rPhase, rStep, rObsEnv, rObsCan, rObsWord, rDoneTag,
                   rBadCanary, rTorn, rStale, epoch>>

\* e0 := epoch (the pre-revoke snapshot the reclaim API takes), then the
\* revoke store (Release).
RelStart ==
    /\ relPhase = "idle"
    /\ e0'       = relEpoch
    /\ revoked'  = TRUE
    /\ relPhase' = "revoked"
    /\ UNCHANGED <<latest, wWork, rWork, bufEnv, bufCan, bufWord, envHist,
                   canHist, wordHist, wSeq, wPhase, wWord,
                   wRevSeen, epoch, relEpoch, freed,
                   rPhase, rStep, rObsEnv, rObsCan, rObsWord, rDoneTag,
                   rBadCanary, rTorn, rStale>>

\* Free AFTER the ACK is observed (the production discipline): poison/free
\* licensed by the handshake.
RelFreeAfterAck ==
    /\ FreeMode = 1
    /\ relPhase = "revoked"
    /\ relEpoch > e0
    /\ freed' = TRUE
    /\ UNCHANGED <<latest, wWork, rWork, bufEnv, bufCan, bufWord, envHist,
                   canHist, wordHist, wSeq, wPhase, wWord,
                   revoked, wRevSeen, epoch, relEpoch, relPhase, e0,
                   rPhase, rStep, rObsEnv, rObsCan, rObsWord, rDoneTag,
                   rBadCanary, rTorn, rStale>>

\* Free WITHOUT the ACK (the A1 bug): the poison-before-ACK the handshake
\* exists to prevent.
RelFreePremature ==
    /\ FreeMode = 2
    /\ relPhase = "revoked"
    /\ freed' = TRUE
    /\ UNCHANGED <<latest, wWork, rWork, bufEnv, bufCan, bufWord, envHist,
                   canHist, wordHist, wSeq, wPhase, wWord,
                   revoked, wRevSeen, epoch, relEpoch, relPhase, e0,
                   rPhase, rStep, rObsEnv, rObsCan, rObsWord, rDoneTag,
                   rBadCanary, rTorn, rStale>>

ReleaserActions == RelStart \/ RelFreeAfterAck \/ RelFreePremature

\* Post-free idling keeps Next enabled (TLC deadlock checking).
FreeStutter ==
    /\ freed
    /\ UNCHANGED Vars

------------------------------------------------------------------------------
Next ==
    \/ WriterProgress
    \/ WriterStutter
    \/ ReaderActions
    \/ ReaderIdleTick
    \/ PropWRev
    \/ PropRelEpoch
    \/ ReleaserActions
    \/ FreeStutter

Liveness ==
    /\ WF_Vars(WriterProgress)
    /\ WF_Vars(ReaderActions)
    /\ WF_Vars(PropWRev)
    /\ WF_Vars(PropRelEpoch)
    /\ WF_Vars(RelStart \/ RelFreeAfterAck)

Spec       == Init /\ [][Next]_Vars
SpecFair   == Init /\ [][Next]_Vars /\ Liveness

------------------------------------------------------------------------------
(*** safety ***)

TypeOK ==
    /\ latest \in Buffers
    /\ wWork  \in Buffers
    /\ rWork  \in Buffers
    /\ bufEnv  \in [Buffers -> Tags]
    /\ bufCan  \in [Buffers -> Tags]
    /\ bufWord \in [Buffers -> [Words -> Tags]]
    /\ envHist  \in [Buffers -> SUBSET Tags]
    /\ canHist  \in [Buffers -> SUBSET Tags]
    /\ wordHist \in [Buffers -> [Words -> SUBSET Tags]]
    /\ wSeq   \in 1 .. FRAMES + 1
    /\ wPhase \in {"idle", "fill", "env", "can", "done"}
    /\ wWord  \in 0 .. WORDS
    /\ revoked \in BOOLEAN
    /\ wRevSeen \in BOOLEAN
    /\ epoch \in 0 .. FRAMES + 1
    /\ relEpoch \in 0 .. FRAMES + 1
    /\ relPhase \in {"idle", "revoked"}
    /\ e0 \in 0 .. FRAMES + 1
    /\ freed \in BOOLEAN
    /\ rPhase \in {"idle", "observing"}
    /\ rStep \in ({StepEnv, StepCan, StepVerify}
                  \union {StepWord(w) : w \in Words})
    /\ rObsEnv \in Tags
    /\ rObsCan \in Tags
    /\ rObsWord \in [Words -> Tags]
    /\ rDoneTag \in Tags
    /\ rBadCanary \in BOOLEAN
    /\ rTorn \in BOOLEAN
    /\ rStale \in BOOLEAN

\* Histories always contain the current value (write-through history).
HistConsistent ==
    /\ \A b \in Buffers : bufEnv[b] \in envHist[b]
    /\ \A b \in Buffers : bufCan[b] \in canHist[b]
    /\ \A b \in Buffers : \A w \in Words : bufWord[b][w] \in wordHist[b][w]

\* I1 — THE construction: the three indices partition the three buffers
\* among in-exchange / writer / reader at every reachable instant.
OwnershipPartition ==
    /\ latest /= wWork
    /\ latest /= rWork
    /\ wWork  /= rWork

\* L12 — the kernel's own mid-flight detector never fires on legal paths.
CanaryIntegrity == ~rBadCanary

\* L1 — no accepted round mixes payload frames.
TornPayload == ~rTorn

\* L4 — the reader never regresses to an older frame.
ObservedMonotone == ~rStale

\* I6/A1 — after the free, the writer is not mid-frame on any buffer.
\* (Reader-side post-free access is caller discipline per 02 §3 — the
\* kernel's handshake guarantees the WRITER side; declared, not defended.)
NoUseAfterFree == ~freed \/ wPhase \in {"idle", "done"}

\* The I6 statement itself: free implies the observed ACK.
AckBeforeFree == ~freed \/ relEpoch > e0

Inv == TypeOK /\ HistConsistent /\ OwnershipPartition /\ CanaryIntegrity
       /\ TornPayload /\ ObservedMonotone /\ NoUseAfterFree /\ AckBeforeFree

------------------------------------------------------------------------------
(*** liveness (production cfg only) ***)

WriterTerminates  == <>(wSeq > FRAMES \/ wPhase = "done")

\* If the releaser never starts, the reader drains the final frame (the
\* app-ordered lifecycle: drain reader, then release writer — behaviors where
\* the releaser intervenes early stop the writer by design and are excluded).
DrainUnrevoked == ([] (relPhase = "idle")) => (<>(rDoneTag = FRAMES))

\* Once the releaser starts, the handshake completes: ACK observed, freed.
HandshakeCompletes == [] (relPhase # "idle" => <> freed)

===============================================================================
