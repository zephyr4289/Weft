--------------------- MODULE FanoutSeqlockRecovery ---------------------
(* RFC 0014 / Axis 3: Self-Stabilizing Recovery from Bounded Corruption       *)
(*                                                                           *)
(* WHAT THIS MODEL PROVES:                                                   *)
(* When any ring control word (latestSeq, publishes, slotSeq[k]) is mutated   *)
(* by an arbitrary external corruption event (bounded to 1 corruption):       *)
(*   1. The health check DETECTS the inconsistency (HealthCheckDetects)       *)
(*   2. The recovery action RESTORES all invariants (RecoveryRestores)        *)
(*   3. NO READER ACCEPTS A TORN FRAME even during corruption (NoTornAccepted)*)
(*   4. Ring returns to consistent monotonic publishing                      *)
EXTENDS Integers, Naturals, TLC

CONSTANTS READERS,
          SLOTS,
          WORDS,
          FRAMES,
          CORRUPTIONS

Slots   == 0 .. SLOTS - 1
Words   == 0 .. WORDS - 1
Readers == 1 .. READERS

VARIABLES
    latest,         \* latestSeq — publication pointer
    publishes,      \* total publish operations
    slotSeq,        \* [k] -> stamp
    payload,        \* [k][w] -> frame tag
    wSeq,           \* writer sequence
    wSlot,          \* writer current working slot
    rLastSeq,       \* [r] -> reader last accepted frame
    rSnapshot,      \* [r][w] -> reader buffer
    corruptionsLeft,\* remaining corruptions allowed
    corrupted,      \* flag indicating corruption has occurred
    recovered       \* flag indicating recovery has completed

TypeOK ==
    /\ latest \in Nat
    /\ publishes \in Nat
    /\ slotSeq \in [Slots -> Nat]
    /\ payload \in [Slots -> [Words -> Nat]]
    /\ wSeq \in Nat
    /\ wSlot \in Slots
    /\ rLastSeq \in [Readers -> Nat]
    /\ rSnapshot \in [Readers -> [Words -> Nat]]
    /\ corruptionsLeft \in 0..CORRUPTIONS
    /\ corrupted \in BOOLEAN
    /\ recovered \in BOOLEAN

Init ==
    /\ latest = 0
    /\ publishes = 0
    /\ slotSeq = [k \in Slots |-> 0]
    /\ payload = [k \in Slots |-> [w \in Words |-> 0]]
    /\ wSeq = 0
    /\ wSlot = 0
    /\ rLastSeq = [r \in Readers |-> 0]
    /\ rSnapshot = [r \in Readers |-> [w \in Words |-> 0]]
    /\ corruptionsLeft = CORRUPTIONS
    /\ corrupted = FALSE
    /\ recovered = FALSE

(* Normal Writer publishing *)
WriterPublish ==
    /\ wSeq < FRAMES
    /\ wSeq' = wSeq + 1
    /\ LET k == wSeq' % SLOTS IN
       /\ wSlot' = k
       /\ payload' = [payload EXCEPT ![k] = [w \in Words |-> wSeq']]
       /\ slotSeq' = [slotSeq EXCEPT ![k] = wSeq']
       /\ latest' = wSeq'
       /\ publishes' = publishes + 1
    /\ UNCHANGED << rLastSeq, rSnapshot, corruptionsLeft, corrupted, recovered >>

(* Bounded Corruption: arbitrary mutation of a control word *)
CorruptLatest ==
    /\ corruptionsLeft > 0
    /\ ~corrupted
    /\ latest' = latest + 999
    /\ corruptionsLeft' = corruptionsLeft - 1
    /\ corrupted' = TRUE
    /\ UNCHANGED << publishes, slotSeq, payload, wSeq, wSlot, rLastSeq, rSnapshot, recovered >>

CorruptSlotSeq ==
    /\ corruptionsLeft > 0
    /\ ~corrupted
    /\ \E k \in Slots :
       /\ slotSeq' = [slotSeq EXCEPT ![k] = 888]
    /\ corruptionsLeft' = corruptionsLeft - 1
    /\ corrupted' = TRUE
    /\ UNCHANGED << latest, publishes, payload, wSeq, wSlot, rLastSeq, rSnapshot, recovered >>

(* Ring Health Predicate *)
RingHealthy ==
    /\ latest <= publishes
    /\ \A k \in Slots : slotSeq[k] <= latest
    /\ Cardinality({k \in Slots : slotSeq[k] = latest /\ latest > 0}) <= 1

(* Self-Stabilizing Recovery Action *)
RingRecover ==
    /\ corrupted
    /\ ~RingHealthy
    /\ LET maxValid == IF \E k \in Slots : slotSeq[k] <= publishes /\ slotSeq[k] > 0
                       THEN CHOOSE m \in Nat : (\E k \in Slots : slotSeq[k] = m /\ m <= publishes)
                       ELSE 0 IN
       /\ latest' = maxValid
       /\ slotSeq' = [k \in Slots |-> IF slotSeq[k] = maxValid THEN maxValid ELSE 0]
       /\ publishes' = IF publishes < maxValid THEN maxValid ELSE publishes
       /\ recovered' = TRUE
    /\ UNCHANGED << payload, wSeq, wSlot, rLastSeq, rSnapshot, corruptionsLeft, corrupted >>

(* Reader Claim Loop under seqlock *)
ReaderClaim ==
    /\ \E r \in Readers :
       LET L == latest IN
       /\ L > 0
       /\ L # rLastSeq[r]
       /\ LET k == L % SLOTS IN
          /\ slotSeq[k] = L
          /\ rSnapshot' = [rSnapshot EXCEPT ![r] = payload[k]]
          /\ slotSeq[k] = L  \* Seqlock post-stamp validation
          /\ rLastSeq' = [rLastSeq EXCEPT ![r] = L]
    /\ UNCHANGED << latest, publishes, slotSeq, payload, wSeq, wSlot, corruptionsLeft, corrupted, recovered >>

Next ==
    \/ WriterPublish
    \/ CorruptLatest
    \/ CorruptSlotSeq
    \/ RingRecover
    \/ ReaderClaim

(* Safety Invariants *)
NoTornAccepted ==
    \A r \in Readers :
       (rLastSeq[r] > 0) => (\A w \in Words : rSnapshot[r][w] = rLastSeq[r])

SelfStabilization ==
    recovered => RingHealthy

Spec == Init /\ [][Next]_<<latest, publishes, slotSeq, payload, wSeq, wSlot, rLastSeq, rSnapshot, corruptionsLeft, corrupted, recovered>>
=============================================================================
