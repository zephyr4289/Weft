---------------------------- MODULE wcr1_consensus ---------------------------
(* WEFT PILLAR 8 — Formal model of the WCR1 cluster lease consensus:       *)
(* heartbeats, lease renewal, leader election and partition healing.       *)
(*                                                                          *)
(* WHAT THIS MODEL IS: the WCR1 control plane at the granularity of its    *)
(* atomic protocol events. Elections run THROUGH the ring (vote requests   *)
(* and grants are in-flight messages), a node grants a vote only for a     *)
(* STRICTLY higher epoch than it has promised, an active primary refuses   *)
(* to vote while its lease is valid, and a majority (Quorum) of grants —   *)
(* self vote included — installs a primary. The lease is stamped with the  *)
(* CANDIDACY epoch (candEpoch), not the candidate's current promise: a     *)
(* candidate's promise can rise after its quorum assembled (by granting   *)
(* another candidate), and stamping that risen value would break per-epoch *)
(* uniqueness — this exact defect was caught by the Pillar 8 oracle and    *)
(* is recorded in D-81 §7. Network partitions are modeled by a per-node    *)
(* component map: one link flap at a time (Split moves a single node),     *)
(* and healing reconnects everything while gossiping the max promised      *)
(* epoch into every node atomically.                                       *)
(*                                                                          *)
(*   Safety (invariants over the whole reachable state space):              *)
(*     - TypeOK          — every variable stays in its declared shape      *)
(*     - SinglePrimary   — at most one node holds the valid write lease    *)
(*                         for any partition at any logical epoch: two     *)
(*                         actives can never share a leaseEpoch (the       *)
(*                         quorum-intersection argument, D-81 §4 T3).      *)
(*     - QuorumPromise   — every active primary's epoch was promised by a  *)
(*                         live majority (the mechanism invariant).        *)
(*     - HealFloor       — after any heal, no node's promised epoch ever   *)
(*                         regresses below the pre-heal cluster maximum    *)
(*                         (post-split convergence guarantees monotonic    *)
(*                         epoch advancement — the directive's             *)
(*                         PartitionHealing guarantee).                    *)
(*                                                                          *)
(*   Epoch fencing note (engineering honesty, D-81 §4 T4): two actives    *)
(*   with DIFFERENT epochs can transiently overlap across a partition     *)
(*   event — that is exactly why every WCR1 write carries (epoch, lease)   *)
(*   as a fencing token and why voters refuse epochs they have already     *)
(*   promised. Cross-epoch exclusion is enforced at the quorum ack site,   *)
(*   not by wall-clock lease arithmetic.                                   *)
(*                                                                          *)
(*   Deadlock freedom: Tick is always enabled (a logical clock tick), so   *)
(*   the cluster never freezes even when fully partitioned.                *)
(*                                                                          *)
(* Checked bounds (wcr1_consensus.cfg): 3 nodes, epoch cap 2, lease 2      *)
(* ticks, message buffer 2 — the smallest shape containing a split, a      *)
(* heal, an election through a minority component, a stale-epoch           *)
(* heartbeat refusal, and two successive primaries.                        *)
(*                                                                          *)
(* Checked by TLC 1.8.0 (pinned sha) — see                                  *)
(* docs/reports/D-81-FORMAL-VERIFICATION-AUDIT.md.                          *)
EXTENDS Naturals, FiniteSets

CONSTANTS NN,        \* cluster size (3: smallest quorum-bearing cluster)
          EPOCHCAP,  \* exclusive upper bound for epochs (finite model)
          LEASEMAX,  \* lease length in logical ticks
          MSGBUF     \* bounded in-flight message buffer (backpressure)

Nodes   == 0 .. NN - 1
Quorum  == NN \div 2 + 1
Comps   == 0 .. 1                 \* component labels ({0,1}: all 2+1 splits)

Max(S) == CHOOSE x \in S : \A y \in S : x >= y

MsgType == {"vrq", "vgr", "hb", "hba"}

VARIABLES
    promised,   \* [Nodes -> 0..EPOCHCAP] — highest epoch each node promised
    candEpoch,  \* [Nodes -> 0..EPOCHCAP] — epoch of the current candidacy
    role,       \* [Nodes -> {"F","C","P"}] — follower/candidate/primary
    leaseRem,   \* [Nodes -> 0..LEASEMAX] — remaining lease ticks
    leaseEpoch, \* [Nodes -> 0..EPOCHCAP] — epoch of the current/last lease
    grants,     \* [Nodes -> SUBSET Nodes] — vote grants collected (candidate)
    hbAck,      \* [Nodes -> SUBSET Nodes] — heartbeat acks collected (primary)
    part,       \* [Nodes -> Comps] — partition component of each node
    msgs,       \* SET of in-flight messages (bounded by MSGBUF)
    healedFloor \* pre-heal cluster max epoch (auxiliary history variable)

Vars == <<promised, candEpoch, role, leaseRem, leaseEpoch, grants, hbAck,
          part, msgs, healedFloor>>

SameComp(a, b) == part[a] = part[b]

Comp(n) == {m \in Nodes : SameComp(m, n)}

IsActive(n) == role[n] = "P" /\ leaseRem[n] > 0

------------------------------------------------------------------------------
(*** the logical clock ***)

Tick ==
    /\ leaseRem' = [n \in Nodes |-> IF leaseRem[n] > 0 THEN leaseRem[n] - 1
                                            ELSE 0]
    /\ role' = [n \in Nodes |->
                   IF role[n] = "P" /\ leaseRem'[n] = 0 THEN "F"
                   ELSE role[n]]
    /\ UNCHANGED <<promised, candEpoch, leaseEpoch, grants, hbAck, part,
                   msgs, healedFloor>>

------------------------------------------------------------------------------
(*** elections — through the ring, under partitions ***)

StartElection(n) ==
    /\ role[n] \in {"F", "C"}
    /\ LET e == 1 + Max({promised[m] : m \in Comp(n)})
       IN /\ e <= EPOCHCAP
          /\ Cardinality(msgs) + Cardinality(Comp(n) \ {n}) <= MSGBUF
          /\ role' = [role EXCEPT ![n] = "C"]
          /\ promised' = [promised EXCEPT ![n] = e]   \* the self-promise
          /\ candEpoch' = [candEpoch EXCEPT ![n] = e] \* stamp the candidacy
          /\ grants' = [grants EXCEPT ![n] = {}]
          /\ hbAck' = [hbAck EXCEPT ![n] = {}]
          /\ msgs' = msgs \union
                 { [type |-> "vrq", from |-> n, to |-> m, ep |-> e] :
                   m \in Comp(n) \ {n} }
    /\ UNCHANGED <<leaseRem, leaseEpoch, part, healedFloor>>

\* A vote is granted only for a STRICTLY higher epoch, and an active
\* primary never votes away its own lease.
GrantVote(v) ==
    \E m \in msgs :
        /\ m.type = "vrq"
        /\ m.to = v
        /\ SameComp(m.from, v)
        /\ m.ep > promised[v]
        /\ ~ IsActive(v)
        /\ m.ep <= EPOCHCAP
        /\ promised' = [promised EXCEPT ![v] = m.ep]
        /\ msgs' = (msgs \ {m}) \union
               { [type |-> "vgr", from |-> v, to |-> m.from, ep |-> m.ep] }
        /\ UNCHANGED <<candEpoch, role, leaseRem, leaseEpoch, grants, hbAck,
                       part, healedFloor>>

\* The candidate consumes a grant addressed to it at its CANDIDACY epoch.
\* (Without this delivery step elections can never fire — the grants set
\* would stay empty forever and every primary-related invariant would hold
\* VACUOUSLY. This is what makes the proof non-vacuous; the guard pins the
\* grant to candEpoch so the lease is stamped with the epoch the quorum
\* actually promised.)
DeliverGrant(c) ==
    \E m \in msgs :
        /\ m.type = "vgr"
        /\ m.to = c
        /\ SameComp(m.from, c)
        /\ role[c] = "C"
        /\ m.ep = candEpoch[c]
        /\ grants' = [grants EXCEPT ![c] = @ \union {m.from}]
        /\ msgs' = msgs \ {m}
        /\ UNCHANGED <<promised, candEpoch, role, leaseRem, leaseEpoch,
                       hbAck, part, healedFloor>>

\* Stale vote requests and cross-partition mail are dropped (the lossy
\* network; also models a voter's refusal to regress its promise).
DropMsg(m) ==
    /\ m \in msgs
    /\ msgs' = msgs \ {m}
    /\ UNCHANGED <<promised, candEpoch, role, leaseRem, leaseEpoch, grants,
                   hbAck, part, healedFloor>>

WinElection(c) ==
    /\ role[c] = "C"
    /\ Cardinality(grants[c] \union {c}) >= Quorum
    /\ role' = [role EXCEPT ![c] = "P"]
    /\ leaseRem' = [leaseRem EXCEPT ![c] = LEASEMAX]
    /\ leaseEpoch' = [leaseEpoch EXCEPT ![c] = candEpoch[c]]
    /\ grants' = [grants EXCEPT ![c] = {}]
    /\ UNCHANGED <<promised, candEpoch, hbAck, part, msgs, healedFloor>>

------------------------------------------------------------------------------
(*** heartbeats and lease renewal ***)

Heartbeat(n) ==
    /\ IsActive(n)
    /\ Cardinality(msgs) + Cardinality(Comp(n) \ {n}) <= MSGBUF
    /\ msgs' = msgs \union
           { [type |-> "hb", from |-> n, to |-> m, ep |-> leaseEpoch[n]] :
             m \in Comp(n) \ {n} }
    /\ UNCHANGED <<promised, candEpoch, role, leaseRem, leaseEpoch, grants,
                   hbAck, part, healedFloor>>

\* A heartbeat at or above the receiver's promise is adopted and acked; a
\* stale-epoch heartbeat (below the promise) is dropped — the fencing
\* evidence. Delivery requires same-component connectivity.
DeliverHb(v) ==
    \E m \in msgs :
        /\ m.type = "hb"
        /\ m.to = v
        /\ SameComp(m.from, v)
        /\ m.ep >= promised[v]
        /\ promised' = [promised EXCEPT ![v] = m.ep]
        /\ msgs' = (msgs \ {m}) \union
               { [type |-> "hba", from |-> v, to |-> m.from, ep |-> m.ep] }
        /\ UNCHANGED <<candEpoch, role, leaseRem, leaseEpoch, grants, hbAck,
                       part, healedFloor>>

DeliverAck(n) ==
    \E m \in msgs :
        /\ m.type = "hba"
        /\ m.to = n
        /\ SameComp(m.from, n)
        /\ role[n] = "P"
        /\ m.ep = leaseEpoch[n]
        /\ hbAck' = [hbAck EXCEPT ![n] = @ \union {m.from}]
        /\ msgs' = msgs \ {m}
        /\ UNCHANGED <<promised, candEpoch, role, leaseRem, leaseEpoch,
                       grants, part, healedFloor>>

RenewLease(n) ==
    /\ role[n] = "P"
    /\ Cardinality(hbAck[n] \union {n}) >= Quorum
    /\ leaseRem[n] < LEASEMAX
    /\ leaseRem' = [leaseRem EXCEPT ![n] = LEASEMAX]
    /\ hbAck' = [hbAck EXCEPT ![n] = {}]
    /\ UNCHANGED <<promised, candEpoch, role, leaseEpoch, grants, part,
                   msgs, healedFloor>>

------------------------------------------------------------------------------
(*** partitions: one link flap at a time; healing gossips max epoch ***)

Split(n, v) ==
    /\ part[n] # v
    /\ part' = [part EXCEPT ![n] = v]
    /\ UNCHANGED <<promised, candEpoch, role, leaseRem, leaseEpoch,
                   grants, hbAck, msgs, healedFloor>>

Heal ==
    /\ \E n \in Nodes : part[n] # 0
    /\ LET mx == Max({promised[n] : n \in Nodes})
       IN /\ part' = [n \in Nodes |-> 0]
          /\ promised' = [n \in Nodes |-> mx]
          /\ healedFloor' = mx
    /\ UNCHANGED <<candEpoch, role, leaseRem, leaseEpoch, grants, hbAck,
                   msgs>>

------------------------------------------------------------------------------
Init ==
    /\ promised = [n \in Nodes |-> 0]
    /\ candEpoch = [n \in Nodes |-> 0]
    /\ role = [n \in Nodes |-> "F"]
    /\ leaseRem = [n \in Nodes |-> 0]
    /\ leaseEpoch = [n \in Nodes |-> 0]
    /\ grants = [n \in Nodes |-> {}]
    /\ hbAck = [n \in Nodes |-> {}]
    /\ part = [n \in Nodes |-> 0]
    /\ msgs = {}
    /\ healedFloor = 0

Next ==
    \/ Tick
    \/ \E n \in Nodes : StartElection(n)
    \/ \E v \in Nodes : GrantVote(v)
    \/ \E c \in Nodes : DeliverGrant(c)
    \/ \E m \in msgs : DropMsg(m)
    \/ \E c \in Nodes : WinElection(c)
    \/ \E n \in Nodes : Heartbeat(n)
    \/ \E v \in Nodes : DeliverHb(v)
    \/ \E n \in Nodes : DeliverAck(n)
    \/ \E n \in Nodes : RenewLease(n)
    \/ \E n \in Nodes, v \in Comps : Split(n, v)
    \/ Heal

Spec == Init /\ [][Next]_Vars

------------------------------------------------------------------------------
(*** safety ***)

TypeOK ==
    /\ promised \in [Nodes -> 0 .. EPOCHCAP]
    /\ candEpoch \in [Nodes -> 0 .. EPOCHCAP]
    /\ role \in [Nodes -> {"F", "C", "P"}]
    /\ leaseRem \in [Nodes -> 0 .. LEASEMAX]
    /\ leaseEpoch \in [Nodes -> 0 .. EPOCHCAP]
    /\ grants \in [Nodes -> SUBSET Nodes]
    /\ hbAck \in [Nodes -> SUBSET Nodes]
    /\ part \in [Nodes -> Comps]
    /\ msgs \in SUBSET {[type |-> t, from |-> f, to |-> o, ep |-> e] :
                         t \in MsgType, f \in Nodes, o \in Nodes,
                         e \in 0 .. EPOCHCAP}
    /\ Cardinality(msgs) <= MSGBUF
    /\ healedFloor \in 0 .. EPOCHCAP

\* THE PRIMARY-THEOREM (mandate: "at most one node holds the valid write
\* lease for any partition at any logical epoch"): two simultaneously
\* active primaries can never share an epoch — winning epoch e requires a
\* majority that promised e, and two disjoint e-majorities cannot exist in
\* a 3-node cluster (quorum intersection).
SinglePrimary ==
    \A n, m \in Nodes :
        (n # m /\ IsActive(n) /\ IsActive(m)) => leaseEpoch[n] # leaseEpoch[m]

\* The mechanism invariant: whoever is active was promised by a majority.
QuorumPromise ==
    \A n \in Nodes :
        IsActive(n) =>
            Cardinality({m \in Nodes : promised[m] >= leaseEpoch[n]})
                >= Quorum

\* Partition healing: after a heal, every node's promise is at least the
\* pre-heal cluster maximum, and promises never regress (monotonic epoch
\* advancement through convergence).
HealFloor ==
    \A n \in Nodes : promised[n] >= healedFloor

Inv == TypeOK /\ SinglePrimary /\ QuorumPromise /\ HealFloor

=============================================================================
