# Implementation Notes

Implements the "Hinted Ordered List" from Melnikov & Sela: _Concurrent extrema and
successor/predecessor queries in search trees_. The ordered set lives entirely in the
linked list. The BST is a hint index whose only job is `find_close(key) → Node*`.

---

## File layout

```
implementation/
├── node.h                    Core types: State, MarkedPtr, CollisionlessPtr, Node, NodeSnapshot
├── list.h                    Lock-free doubly-linked list (barrier, normalize, insert, erase, min, max, extractMin, extractMax)
├── list_pragmatic.h          Pragmatic list variant with plain prev stores
├── bst_hint.h                Lock-free external BST hint index (Natarajan-Mittal, adapted)
├── bst_balanced_hint.h       Balanced hint index based on CBAT / chromatic tree
├── bst_list*.h               Four wrappers combining the list and BST variants
```

Build: `g++ -std=c++17 -O2 -pthread -o bench bench.cpp && ./bench`

---

## Benchmark results (lpdquad, g++ -O2, DEBRA active)

### Per-operation throughput (8 threads, key range 10,000, Mops/s)

| Variant | insert | remove | min+max | extract\_min | extract\_max |
|---|---|---|---|---|---|
| BSTList | **0.053** | **0.050** | 24.972 | 1.367 | **1.602** |
| BSTListBalanced | 0.038 | 0.024 | 25.721 | 0.227 | 0.225 |
| BSTListPragmatic | 0.043 | 0.032 | 26.429 | **1.396** | 1.064 |
| BSTListBalancedPragmatic | 0.038 | 0.023 | **27.974** | 0.239 | 0.253 |

`extract_min` / `extract_max` are much faster than insert/remove because: (a) each
extraction only needs to find the head/tail node (O(1) once the victim is stamped), and
(b) extract operations spend most of their time in the DEBRA CS not doing list traversal.

`min+max` is fast: `list_max_backward` walks backward from `tail->prev_` inside a DEBRA
critical section, making `max()` O(1) amortized. The store-based variants keep
back-pointers more current than the barrier variant, directly benefiting the backward scan.
Note: `min+max` throughput is highly sensitive to server memory-bandwidth availability and
varies significantly across runs on the shared `lpdquad` server.

The balanced BST variants are significantly slower: chromatic-tree LLX/SCX rebalancing
overhead dominates extract\_min/extract\_max cost.

### Mixed throughput (Mops/s, 40% insert / 40% remove / 20% min+max)

Key range 1,000:

| Threads | BSTList | BSTListBalanced | BSTListPragmatic | BSTListBalancedPragmatic |
|---|---|---|---|---|
| 1  | **0.777** | 0.356 | 0.709 | 0.333 |
| 8  | 3.969 | 0.487 | **4.406** | 0.596 |
| 32 | **5.606** | 0.273 | 5.322 | 0.265 |

Key range 10,000:

| Threads | BSTList | BSTListBalanced | BSTListPragmatic | BSTListBalancedPragmatic |
|---|---|---|---|---|
| 1  | **0.041** | 0.026 | 0.039 | 0.025 |
| 8  | **0.255** | 0.156 | 0.251 | 0.156 |
| 32 | 0.850 | 0.191 | **0.887** | 0.220 |

---

## Key data structures (node.h)

### Sentinel nodes

`head` (key −∞) and `tail` (key +∞) are permanent; they cannot be deleted.
The list invariant is: `head → [live nodes in strictly ascending key order] → tail`
with approximate backward `prev_` pointers maintained alongside.

### State / MarkedPtr

Each node's `next_` field encodes both the successor pointer and a 2-bit state:

```
State ∈ { Ok, Deleted, ExtMin, ExtMax }
  Ok      — node is logically present in the set
  Deleted — node is logically removed by remove()
  ExtMin  — node is claimed by extract_min()
  ExtMax  — node is claimed by extract_max()
```

Deletion is a one-way transition for the logical set state: once a node is marked
`Deleted`, `ExtMin`, or `ExtMax`, the node is treated as removed from the set. The
forward pointer is still used for traversal and helping.

### CollisionlessPtr

`prev_` back-pointers are susceptible to ABA: a CAS might succeed even though the
pointer was replaced and restored between the read and the CAS. To prevent this, each
write of `prev_` allocates a new `PrevWrapper` heap object containing the target `Node*`.
Two writes of the same node produce different wrapper addresses, so any CAS on
`CollisionlessPtr` that sees the same address knows the value truly has not changed.
Failed `PrevWrapper` allocations are deleted immediately; wrappers that are later
superseded by new writes are not reclaimed during normal execution.

```
CollisionlessPtr = PrevWrapper*       // pointer to heap object { Node* node }
unwrap(cp) → Node*                    // read cp->node; null-safe
```

### NodeSnapshot

A consistent point-in-time view of a node and its `next_` field, passed by reference
through mutation helpers so that CAS updates propagate back to the caller:

```
NodeSnapshot { self_: Node*, marked_next_: (ptr_, state_) }
take_snapshot(node) → NodeSnapshot    // atomically read node->next_
```

### CAS semantics

`CAS(x, y, z)` — on success: `x := z`, `y := z`, returns true.
                  on failure: `y := current x`, returns false.

Both `cas_next` (on `next_`) and `cas_prev` (on `prev_`) follow this convention,
updating the local variable on every call.

---

## Primitives (list.h)

### barrier(current)
### barrier(current, to_check)

Ensures `current.next.ptr->prev_` points back to `current.self_`.

- Reads the back-pointer of `current`'s successor.
- Checks that `to_check` has not changed since it was last read (stability anchor).
  If it has, refreshes `to_check` and returns false so the caller can retry.
- If the back-pointer already points to `current`, returns true immediately.
- Otherwise attempts `cas_prev(current.next.ptr, backptr, current.self_)`.
- Special case: if `current` is the tail sentinel (no successor), returns true immediately.

The two-argument form is used to atomically check that an anchor node has not moved
before installing a back-pointer — preventing a race where the anchor was concurrently
evicted between the barrier check and the CAS.

### post_evict_fix(prev, evicted)

After `evicted` has been physically unlinked, fixes `prev.next.ptr->prev_` if it still
points to the evicted node, replacing it with `prev.self_`.

### evict_deleted(prev, current, next)

Physically unlinks the logically deleted node `current` from the list.

```
evict_deleted(prev, current, next):
  barrier(prev)                        // prev.next.ptr->prev_ → prev
  barrier(current, prev)               // stable anchor check on prev
  barrier(next, anchor)                // next.ptr->prev_ stable
  cas_next(prev, prev.next, {next.ptr, Ok})
```

All three barriers must succeed before the CAS to guarantee back-pointer invariants
in both directions. Returns false on any conflict; the caller retries.

### help_deleted_next(current, next)

If `next` is deleted, calls `evict_deleted(current, next, nn)` where `nn` is the
snapshot of `next`'s successor, then calls `post_evict_fix(current, next)`.
Returns true if `next` is (now) alive, false on conflict.

### normalize_next_node(current, next)

Calls `help_deleted_next` in a loop, evicting deleted nodes from immediately after
`current`, until `current`'s next is alive. Returns false if `current` itself becomes
deleted in the process.

---

## Pseudocode: list_find

`list_find` is the shared traversal routine used by insert, remove, and the extract
operations. It starts from the BST hint when the hint lies strictly before the target
key; otherwise it falls back to `head`. From there it walks forward, evicting any
logically deleted nodes it encounters along the way, until it brackets the target key
between two live nodes `pred` and `curr`. If `pred` becomes deleted or normalization
fails, the traversal restarts from `head`.

```
list_find(head, hint, key) → (pred, curr):

  start = hint if hint != null and hint != head and hint.key < key else head

  retry:
    pred = take_snapshot(start)

    while true:
      if pred.state == Deleted:
        start = head
        goto retry

      curr = take_snapshot(pred.next.ptr)
      if not normalize_next_node(pred, curr):  // evict deleted nodes after pred
        start = head
        goto retry

      if curr.self_.key >= key:
        return (pred, curr)       // found window: pred.key < key ≤ curr.key

      pred = curr                 // advance one step forward
```

**Invariant on return**: `pred.key < key ≤ curr.key`, both nodes are alive (state Ok).

---

## Pseudocode: insert

`insert` first asks the BST hint index for a node close to the target key, then calls
`list_find` to locate the exact window `(pred, curr)` where the new node belongs. If
the key already exists and is alive it returns false immediately. Otherwise it wires up
the new node's `prev_` and `next_` pointers, runs two barriers to stabilise the
surrounding back-pointers, and atomically links the new node in with a single CAS on
`pred.next`. After a successful insertion it runs one more barrier to repair `curr.prev_`
so it points to the new node, then does a best-effort BST hint update. Any conflict
(concurrent deletion or insertion in the same window) causes a full retry from the
nearest live predecessor.

```
insert(key):
  hint = bst.find_close(key)   // BST hint: node close to key (may be stale)
  new_node = allocate Node(key)
  start = hint

  loop:
    (pred, curr) = list_find(head, start, key)

    if curr.key == key:
      free new_node
      return false              // key already present

    // try_insert_after(pred, new_node):
    if pred.state == Deleted: goto retry
    curr = take_snapshot(pred.next.ptr)
    if not normalize_next_node(pred, curr): goto retry
    barrier(pred)               // curr.prev_ → pred
    barrier(curr)               // curr.next.ptr.prev_ → curr

    new_node.prev_ = pred.self_
    new_node.next_ = {curr.self_, Ok}
    if not cas_next(pred, pred.next, {new_node, Ok}):  // [LP]
      goto retry
    barrier(new_node)           // repair curr.prev_ → new_node

    bst.try_set(key, new_node)  // best-effort BST hint update
    return true

  retry:
    start = pred.self_
    goto loop
```

**Linearization point**: the successful `cas_next(pred, pred.next, {new_node, Ok})`.
At that instant, `new_node` becomes reachable and its state is `Ok`, so `key` enters
the abstract set S.

---

## Pseudocode: remove

`remove` locates the node holding the target key with `list_find`. If the key is absent
it returns false. Deletion proceeds in two steps: first a logical deletion — a CAS that
flips the node's state from `Ok` to `Deleted` in its `next_` field — and then a
best-effort physical removal that unlinks the now-dead node from the list. The logical
deletion CAS is the only step that matters for correctness; the physical removal is
cooperative and can be completed by any thread that later traverses past the deleted
node. After successful logical deletion, `bst.remove(key)` nulls the hint entry. The
call is unconditional (even when the key is absent) to handle a race: a concurrent
`insert(key)` may call `try_set` after the remover checks the list but before it nulls
the hint, leaving a stale pointer. Calling `bst.remove` unconditionally closes that window.

```
remove(key):
  hint = bst.find_close(key)
  ok = list_erase(head, hint, key)
  bst.remove(key)           // null hint whether or not key was found
  return ok
```

`list_erase` (expanded):

```
list_erase(head, hint, key):
  start = hint

  loop:
    (pred, curr) = list_find(head, start, key)

    if curr.key != key:
      return false              // key not in list

    // try_delete(pred, curr):
    next = take_snapshot(curr.next.ptr)
    if not normalize_next_node(curr, next): goto retry

    if not cas_next(curr, curr.next, {next.ptr, Deleted}):  // logical deletion [LP]
      goto retry

    // Logical deletion succeeded. Best-effort physical removal:
    help_deleted_next(pred, curr)
    return true

  retry:
    start = pred.self_
    goto loop
```

**Linearization point**: the successful `cas_next(curr, curr.next, {next.ptr, Deleted})`.
At that instant, `curr.state` becomes `Deleted` and `key` leaves the abstract set S.
Physical removal (`help_deleted_next`) may be done by any thread traversing the list;
it does not affect the linearization point.

---

## Pseudocode: min

`min` is the simplest operation: it scans forward from `head.next` and returns the key
of the first node whose state is `Ok`. Logically deleted nodes (state `Deleted`) are
simply skipped by following their `next_` pointer. Because the list is kept in sorted
order and deletion is an atomic state flip, the first live node encountered is
guaranteed to be the current minimum at the instant it is observed.

```
min() → key | NO_KEY:
  node = head.next.ptr

  while node ≠ tail:
    (succ, state) = node.next
    if state == Ok:
      return node.key            // [LP — this read]
    node = succ                  // skip logically deleted node

  return NO_KEY
```

**Linearization point**: the read of `node.next` that reveals `state == Ok`, returning
`node.key`. That read atomically observes `node` to be in the set. If the loop reaches
`tail`, the LP is the pointer advance `node = succ` that first makes `node == tail`.

---

## Pseudocode: max

`max(tid)` walks backward from `tail->prev_` inside a DEBRA critical section.
DEBRA guarantees that `prev_` pointers are not freed while the thread holds the CS,
making this O(1) amortized (typical case: `tail->prev_` already points at the maximum,
so the loop terminates immediately).

```
max(tid) → key | NO_KEY:
  DEBRA.leaveQuiescentState(tid)       // enter critical section

  node = unwrap(tail.prev_)
  while node ≠ null and node ≠ head:
    if node.next.state == Ok:
      DEBRA.enterQuiescentState(tid)
      return node.key                  // [LP — this read]
    node = unwrap(node.prev_)

  DEBRA.enterQuiescentState(tid)
  return NO_KEY
```

**Linearization point**: the read of `node.next` that reveals `state == Ok`, returning
`node.key`. The backward walk terminates as soon as the first live node is found from
the tail end, which is the current maximum. If the whole list is empty (only sentinels
reachable), NO_KEY is returned; the LP is the last `node.prev_` read that exhausts the
backward chain.

**Why DEBRA is required**: `prev_` pointers may point to nodes that have been logically
deleted and retired to DEBRA bags, but not yet freed. Reading `node.prev_` on such a node
is safe only while inside a critical section — DEBRA prevents draining a bag while any
thread is in a CS.

---

## Pseudocode: extractMin

### State extensions made

The following extensions were made to the base insert/remove/min/max implementation:

- `node.next.state` supports four values encoded in 2 bits:
  `Ok = 0`, `Deleted = 1`, `ExtMin = 2`, `ExtMax = 3`.
- `head.next.state` uses the `ExtMin` value as an epoch flag:
  `head.next.state ∈ { Ok, ExtMin }`.
  `ExtMin` signals that an `extractMin` epoch is in progress.

**EXT_MIN epoch invariant**: while `EXT_MIN` is set, no new `CLEAR` node can appear
before the victim in head's forward chain. This is enforced by gating head inserts on
`head.next.state == CLEAR`.

**Within-epoch pointer lemma**: while our `EXT_MIN` epoch is active,
`head.next.ptr ∈ {orig_first, victim}`. The pointer cannot be an intermediate
`MARKED_REM` node because the bulk-unlink CAS in `list_find` jumps entire prefixes
atomically.

### helpExtMin

`helpExtMin` is a cooperative subroutine that any thread can call to bring an
in-progress `extractMin` epoch to completion. It reads `head.next` to find the node
`first` that was recorded when the epoch started, then scans forward past any
`MARKED_REM` nodes to find the first node that is either still `CLEAR` or already
`MARKED_EXT_MIN`. If all nodes from `first` to `tail` are `MARKED_REM` the set is
effectively empty and the epoch is closed with `head.next` pointing to `tail`. If a
`CLEAR` node is found, `helpExtMin` races to stamp it `MARKED_EXT_MIN` — only one
thread wins this CAS, which is the linearization point for the whole `extractMin`
operation. Once the victim is stamped, the helper advances `head.next` past the victim
(the physical removal step) and clears the `EXT_MIN` flag. Epoch-guard checks ensure
that a helper never accidentally closes a *newer* epoch that started after its own
scan.

```
helpExtMin():
  // Brings any in-progress EXT_MIN epoch to completion.
  // Safe to call when EXT_MIN is not set (returns immediately).

  while true:
    (first, hState) = head.next
    if hState ≠ EXT_MIN: return

    // Victim scan: skip Deleted and ExtMax nodes (both are logically removed);
    // stop at the first Ok or ExtMin node.
    victim = first
    while victim ≠ tail:
      (vsucc, vstate) = victim.next
      if vstate == Ok or vstate == ExtMin: break
      victim = vsucc

    // ── T-case: all nodes from first are logically removed ──────────────
    if victim == tail:
      head.next.CAS((first, EXT_MIN), (tail, Ok))
      continue    // re-read head from top; if CAS lost, another helper cleared it

    // Re-read victim.next after scan
    (vsucc, vstate) = victim.next

    // ── ExtMin already set: advance head past victim ─────────────────────
    if vstate == ExtMin:
      succ = victim.next.ptr   // re-read for fresh successor pointer
      // No barrier() calls here — they caused livelock under high contention:
      // 8 threads simultaneously fixing prev_ each invalidate the others'
      // snapshot anchors, so no thread makes progress.
      // Epoch guard: only advance head if still in the epoch started with 'first'.
      // Prevents a stale helper from clobbering a newer concurrent epoch.
      (hptr, hst) = head.next
      if hst == EXT_MIN and hptr == first:
        if head.next.CAS((hptr, EXT_MIN), (succ, Ok)):
          retire(victim)
      // Single non-retrying prev_ fix. If it loses the race,
      // the next barrier() from any insert/remove will repair it.
      if succ ≠ null: cas_prev(succ, succ.prev_, head)
      continue

    // ── Ok: try to stamp victim ──────────────────────────────────────────
    if vstate == Ok:
      victim.next.CAS((vsucc, Ok), (vsucc, ExtMin))   // [LP candidate]
      // Whether we won or lost, loop back to complete the head advance.
      continue

    // ── Deleted or ExtMax: victim removed concurrently; restart scan ─────
    // (fall through to top of loop)
```

### extractMin

`extractMin` atomically removes and returns the smallest key in the set, or reports
that the set is empty. It works in two phases. First, it claims exclusive ownership of
an *epoch* by flipping `head.next.state` from `Ok` to `EXT_MIN` with a CAS — this
prevents new nodes from being inserted before the victim while the operation is in
flight, and serialises concurrent `extractMin` calls so only one epoch is active at a
time. Second, it scans forward from `head.next.ptr` past any Deleted/ExtMax nodes to
find the first Ok node, stamps it `ExtMin` with a CAS (the linearization point), and
then delegates the entire physical removal and epoch cleanup to `helpExtMin`.

The stamp CAS encodes both the successor pointer and the state bit in one word, so a
concurrent insert-after-victim (which changes only the successor pointer, not the state)
will cause the CAS to fail even though victim is still the minimum. For this reason the
stamp is performed in a retry loop that re-reads the successor pointer after each
failure, exiting only when the state changes (victim stamped or victim deleted).

```
extractMin() → key | EMPTY:
  while true:
    (first, hState) = head.next
    if hState == EXT_MIN:
      helpExtMin()
      continue

    // Claim the EXT_MIN epoch.
    if not head.next.CAS((first, Ok), (first, EXT_MIN)):
      continue

    // Scan forward from first past Deleted and ExtMax nodes.
    // Break on Ok (candidate) or ExtMin (a helper already stamped a victim).
    victim = first
    while victim ≠ tail:
      (vsucc, vstate) = victim.next
      if vstate == Ok or vstate == ExtMin: break
      victim = vsucc

    if victim == tail:
      // All nodes are logically removed; set is empty.
      helpExtMin()         // closes epoch: head.next.CAS((first, EXT_MIN), (tail, Ok))
      return EMPTY         // [LP — T-case CAS in helpExtMin]

    // Re-read victim.next
    (vsucc, vstate) = victim.next

    // ── Helper already stamped victim ────────────────────────────────────
    if vstate == ExtMin:
      helpExtMin()         // advance head, clear EXT_MIN
      return victim.key   // [LP — the stamp CAS executed by the helper]

    // ── Ok: stamp victim; retry while vstate == Ok ───────────────────────
    // A concurrent insert-after-victim changes only victim.next.ptr (not the
    // state), causing the CAS to fail even though victim is still the minimum.
    // Loop with the updated ptr rather than abandoning the epoch.
    if vstate == Ok:
      while vstate == Ok:
        if victim.next.CAS((vsucc, Ok), (vsucc, ExtMin)):  // [LP]
          helpExtMin()     // advance head, clear EXT_MIN
          return victim.key
        (vsucc, vstate) = victim.next  // CAS updates to current value on failure

      if vstate == ExtMin:
        // A concurrent helper stamped it; epoch owner returns the key.
        helpExtMin()
        return victim.key

      // Deleted or ExtMax: victim removed; close epoch and retry.
      helpExtMin()
      continue

    // ── Deleted or ExtMax: victim removed between scan and re-read ────────
    helpExtMin()
    continue
```

---

## Pseudocode: extractMax — full epoch design (alternative, not implemented)

> The simplified epoch-free version below was chosen for implementation.
> This section documents the full epoch-based design for reference.

### Required state extensions (in addition to those for extractMin)

- `node.next.state` extended to include `ExtMax` (implemented as part of the 2-bit encoding above).
- A new `tail_anchor: atomic (Node*, TailEpoch)` field in the list struct:
  `TailEpoch ∈ { IDLE, EXT_MAX }`.
  `EXT_MAX` signals that an `extractMax` epoch is in progress.
  `tail_anchor.ptr` tracks the approximate last data node before `tail`.

**EXT_MAX epoch invariant**: while `EXT_MAX` is set, no new `CLEAR` node can appear
after the victim in tail's backward chain. Enforced by gating tail inserts (insertions
where `curr == tail`) through a `tail_anchor.CAS` that checks `TailEpoch == IDLE`.

**Tail insert gate**: inserts that would become the new maximum must go through
`tail_anchor.CAS((tail, IDLE), (new_node, IDLE))` rather than a plain `pred.next.CAS`,
so the `EXT_MAX` flag is checked atomically, mirroring the `head.next.state` gate for
`EXT_MIN`.

### helpExtMax

`helpExtMax` is the symmetric counterpart of `helpExtMin` for the maximum end of the
list. It reads `tail_anchor` to find the node `last` recorded when the epoch started,
then uses `prev_` pointers as a backward hint to get close to the end before doing a
definitive forward scan to `tail` to locate the last `CLEAR` node — the victim. If no
live node is found the set is empty and the epoch is closed. Once a victim is
identified, `helpExtMax` races to stamp it `MARKED_EXT_MAX` (the LP), then physically
unlinks it from the list. Unlike `helpExtMin`, where `head` is always the predecessor
of the victim, the predecessor here is an arbitrary interior node, so the helper must
locate it by walking backward via `prev_` and then forward to verify — the same three
barrier + CAS sequence used by `evict_deleted`. After unlinking, `tail_anchor` is
advanced and the `EXT_MAX` flag is cleared.

```
helpExtMax():
  // Symmetric to helpExtMin. Traversal goes backward from tail; physical
  // removal targets pred.next (an interior pointer), not the head sentinel.

  outer: while true:
    (last, tState) = tail_anchor
    if tState ≠ EXT_MAX: return

    // Victim scan: walk backward from last past MARKED_REM nodes (prev_ hint),
    // then scan forward to tail to find the last CLEAR node.
    candidate = last
    while candidate ≠ head:
      (_, cstate) = candidate.next
      if cstate ≠ MARKED_REM: break
      candidate = unwrap(candidate.prev_)   // best-effort backward hint

    // Forward scan from candidate to tail; track the last CLEAR/MARKED_EXT_MAX node.
    victim = null
    node = candidate
    while node ≠ tail:
      (nsucc, nstate) = node.next
      if nstate == MARKED_EXT_MAX: victim = node; break
      if nstate == CLEAR: victim = node
      node = nsucc

    // ── Empty-set case ───────────────────────────────────────────────
    if victim == null:
      while true:
        (lptr, lst) = tail_anchor
        if lst ≠ EXT_MAX: return
        if lptr ≠ last:
          continue outer            // pointer advanced; re-scan
        if tail_anchor.CAS((lptr, EXT_MAX), (lptr, IDLE)): return

    (vsucc, vstate) = victim.next

    // ── MARKED_EXT_MAX already set ───────────────────────────────────
    if vstate == MARKED_EXT_MAX:
      goto evict_max_and_clear(victim, vsucc, last)

    // ── CLEAR: try to claim victim ───────────────────────────────────
    elif vstate == CLEAR:
      if not victim.next.CAS((vsucc, CLEAR), (vsucc, MARKED_EXT_MAX)):  // [LP candidate]
        (vsucc, vstate) = victim.next
        if vstate ≠ MARKED_EXT_MAX:
          continue outer            // lost to rem() or ABA; restart
      // MARKED_EXT_MAX confirmed.                                    [LP]
      goto evict_max_and_clear(victim, vsucc, last)

    // ── MARKED_REM: concurrently deleted between scan and re-read ────
    else:
      continue outer

    // ── evict_max_and_clear ──────────────────────────────────────────
    // Pre: victim ≠ null, victim.next.state == MARKED_EXT_MAX.
    // Find pred by walking backward via victim.prev_ then forward to verify.
    //
    evict_max_and_clear(victim, vsucc, last):
      while true:
        pred = locate_pred(victim)    // walk backward + forward to find live predecessor
        snap_pred   = take_snapshot(pred)
        snap_victim = take_snapshot(victim)
        snap_vsucc  = take_snapshot(vsucc)

        // DLL: three barriers then CAS (mirrors evict_deleted in list.h)
        if not barrier(snap_pred): continue
        if not barrier(snap_victim, snap_pred): continue
        barrier(snap_vsucc, anchor)
        if not cas_next(pred, snap_pred.next, {vsucc, Ok}): continue
        post_evict_fix(snap_pred, snap_victim)

        // Clear EXT_MAX epoch
        while true:
          (lptr, lst) = tail_anchor
          if lst ≠ EXT_MAX: return
          if lptr ≠ last and lptr ≠ victim:
            continue outer          // new epoch — let it handle itself
          if tail_anchor.CAS((lptr, EXT_MAX), (vsucc, IDLE)): return
```

### extractMax

`extractMax` is the symmetric counterpart of `extractMin` and follows the same
three-phase structure. It claims an epoch by flipping `tail_anchor.state` from `IDLE`
to `EXT_MAX`, preventing new nodes from being inserted after the victim. It then
locates the victim — the last `CLEAR` node before `tail` — by walking backward from
`tail_anchor.ptr` as a hint and then scanning forward all the way to `tail` to find
the true last live node. It stamps the victim `MARKED_EXT_MAX` (the LP), then
physically unlinks it by finding its predecessor via `prev_` hints, running the three
DLL barriers, and CAS-ing `pred.next` past the victim. Finally it advances
`tail_anchor.ptr` and clears `EXT_MAX`. The key asymmetry with `extractMin` is that
the physical removal CAS targets an interior `pred.next` rather than `head.next`,
because the maximum node can be anywhere in the list.

```
extractMax() → key | EMPTY:
  outer: while true:
    (last, tState) = tail_anchor
    if tState == EXT_MAX:
      helpExtMax()
      continue outer

    orig_last = last

    // Claim the EXT_MAX epoch.
    if not tail_anchor.CAS((last, IDLE), (last, EXT_MAX)):
      continue outer

    victim = null;  vsucc = null

    inner: while true:

      // ── First scan: walk backward from orig_last, then forward to tail ──
      candidate = orig_last
      while candidate ≠ head:
        (_, cstate) = candidate.next
        if cstate ≠ MARKED_REM: break
        candidate = unwrap(candidate.prev_)

      victim = null
      scan = candidate
      while scan ≠ tail:
        (ssucc, sstate) = scan.next
        if sstate == MARKED_EXT_MAX:
          victim = scan;  vsucc = ssucc
          goto clear_ext_max_and_return(scan.key)   // [LP already occurred]
        if sstate == CLEAR:
          victim = scan;  vsucc = ssucc
        scan = ssucc

      if victim == null:
        // No CLEAR node found; set is empty.
        while true:
          (lptr, lst) = tail_anchor
          if lst ≠ EXT_MAX: break
          if tail_anchor.CAS((lptr, EXT_MAX), (lptr, IDLE)): break  // [LP — empty]
        return EMPTY

      // ── Re-read tail_anchor ───────────────────────────────────────
      (curr_last, curr_tState) = tail_anchor

      // ── Epoch-boundary detection ──────────────────────────────────
      if curr_tState ≠ EXT_MAX or curr_last ≠ orig_last:
        helpExtMax()
        // Scan for a node we may have marked
        candidate2 = orig_last
        while candidate2 ≠ head:
          (_, c2state) = candidate2.next
          if c2state ≠ MARKED_REM: break
          candidate2 = unwrap(candidate2.prev_)
        found = null
        scan2 = candidate2
        while scan2 ≠ tail:
          (s2succ, s2state) = scan2.next
          if s2state == MARKED_EXT_MAX: found = scan2; break
          if s2state == CLEAR: found = null   // later CLEAR node exists; reset
          scan2 = s2succ
        if found ≠ null: return found.key    // [LP — MARKED_EXT_MAX CAS]
        return EMPTY                          // [LP — empty-case CAS in help]

      // Our epoch is still active.
      (vsucc, vstate) = victim.next
      if vstate == MARKED_EXT_MAX:
        goto clear_ext_max_and_return(victim.key)

      if vstate == CLEAR:
        if victim.next.CAS((vsucc, CLEAR), (vsucc, MARKED_EXT_MAX)):  // [LP]
          goto clear_ext_max_and_return(victim.key)
        victim = null
        continue inner

      victim = null
      continue inner  // MARKED_REM; rescan

    // ── clear_ext_max_and_return ──────────────────────────────────────────
    // Pre: victim ≠ null, victim.next.state == MARKED_EXT_MAX.
    // By the within-epoch pointer lemma, tail_anchor.ptr ∈ {orig_last, victim}.
    //
    clear_ext_max_and_return(key):
      while true:
        pred = locate_pred(victim)    // walk backward + forward
        snap_pred   = take_snapshot(pred)
        snap_victim = take_snapshot(victim)
        snap_vsucc  = take_snapshot(vsucc)

        if not barrier(snap_pred): continue
        if not barrier(snap_victim, snap_pred): continue
        barrier(snap_vsucc, anchor)
        if not cas_next(pred, snap_pred.next, {vsucc, Ok}): continue
        post_evict_fix(snap_pred, snap_victim)

        while true:
          (lptr, lst) = tail_anchor
          if lst ≠ EXT_MAX: return key
          if lptr ≠ victim and lptr ≠ orig_last:
            break                 // new epoch; let its holder clear it
          if tail_anchor.CAS((lptr, EXT_MAX), (vsucc, IDLE)): return key
```

---

## Pseudocode: extractMax — simplified alternative (no epoch) *(implemented)*

Unlike `extractMin`, `extractMax` does not need an epoch flag on a sentinel because the
maximum node has a unique structural property: its `next_` pointer must point to `tail`.
This fact can be folded directly into the marking CAS as an atomic "am I still the
maximum?" check. If a concurrent insert slips a new node between the victim and `tail`,
the victim's `next_` pointer changes away from `tail`, the CAS fails, and the operation
naturally rescans to the new maximum. No `tail_anchor` epoch, no `helpExtMax`, and no
gating of tail inserts is required.

Two concurrent `extractMax` calls can safely race: both try
`victim.next.CAS({tail, Ok}, {tail, ExtMax})` — exactly one wins (the LP),
the other sees `ExtMax` on the victim and rescans. Physical removal requires finding
the predecessor, which is done by reading `victim.prev_` as a start hint and walking
forward. No `barrier()` CAS calls on `prev_` are made during that traversal — those
caused livelock under high contention. The post-splice back-pointer repair uses a
single non-retrying update. Reading `prev_` for traversal is safe inside a DEBRA
critical section.

```
extractMax() → key | EMPTY:
  while true:

    // ── Phase 1: backward hint + forward scan to find last Ok node ───────
    // Read tail.prev_ and walk backward up to 64 steps until we land on an
    // Ok node (bstart).  This gives a start point near the maximum; fall
    // back to head if prev_ is stale or points only to deleted nodes.
    // Only READS of prev_ — no CAS on prev_ (barrier() CAS caused livelock
    // under high contention).  Safe to dereference inside a DEBRA CS.
    bstart = unwrap(tail.prev_)
    if bstart ≠ null and bstart ≠ head:
      for i in 0..64:
        if bstart.next.state == Ok: break
        pp = unwrap(bstart.prev_)
        if pp == null or pp == bstart or pp == head: bstart = null; break
        bstart = pp
      if bstart ≠ null and bstart.next.state ≠ Ok: bstart = null
    else:
      bstart = null

    victim = null
    scan = bstart if bstart ≠ null else head.next.ptr
    while scan ≠ tail:
      (ssucc, sstate) = scan.next

      if sstate == ExtMax:
        // Help physically remove an already-claimed ExtMax node.
        // Walk forward from head to find its predecessor; no barrier() calls.
        pred = head
        while true:
          pnw = pred.next
          if pnw.ptr == scan: break
          if pnw.state ≠ Ok or pnw.ptr == null or pnw.ptr.key > scan.key:
            pred = head; break
          pred = pnw.ptr
        if pred.next == {scan, Ok}:
          cas_next(pred, {scan, Ok}, {ssucc, Ok})
          fix ssucc.prev_ ← pred
        scan = ssucc
        continue

      if sstate == Ok: victim = scan
      scan = ssucc

    if victim == null:
      return EMPTY             // set is empty [LP — the read of scan == tail with no Ok node found]

    // ── Phase 2: verify victim is still the maximum and stamp ExtMax ─────
    (vsucc, vstate) = victim.next
    if vsucc ≠ tail or vstate ≠ Ok:
      // Optimization: if vstate == Ok but vsucc ≠ tail, the node between
      // victim and tail may already be an ExtMax node claimed by another
      // thread. In that case we can directly CAS victim.next to skip it,
      // letting the owner of that ExtMax node retire it later.  This avoids
      // a forward walk from head just to help remove the stuck node.
      // (Implemented as the "vmpfail-escape" path in list.h / list_pragmatic.h.)
      continue                 // new node inserted after victim, or victim removed

    if not victim.next.CAS({tail, Ok}, {tail, ExtMax}):  // [LP]
      continue                 // lost race; rescan

    // ── Phase 3: physical removal ─────────────────────────────────────────
    // Use victim.prev_ as a start hint for the predecessor walk; fall back
    // to head if the hint is invalid or deleted.  No CAS on prev_.
    while true:
      if head.next.state == ExtMin: helpExtMin()  // avoid deadlock with extractMin

      hint = unwrap(victim.prev_)
      pred = hint if hint ≠ null and hint ≠ head and hint ≠ victim
                 and hint.key < victim.key and hint.next.state == Ok
             else head
      found_victim   = false
      victim_removed = false
      while true:
        pnw = pred.next
        if pnw.ptr == null: pred = head; break
        // key-order check first: another thread may have removed victim and
        // inserted a new Ok node with a larger key — that's the victim_removed case.
        if pnw.ptr.key > victim.key: victim_removed = true; break
        if pnw.ptr == victim and pnw.state == Ok: found_victim = true; break
        if pnw.state ≠ Ok: pred = head; break
        pred = pnw.ptr

      if victim_removed:
        retire(victim)         // we own victim; another thread already unlinked it
        return victim.key

      if not found_victim: continue

      if pred.next ≠ {victim, Ok}: continue

      if cas_next(pred, {victim, Ok}, {tail, Ok}):
        fix tail.prev_ ← pred
        retire(victim)
        return victim.key
```

**Linearization point**: the successful `victim.next.CAS({tail, Ok}, {tail, ExtMax})`.
At that instant `victim.key` equals `max(S)` — verified by the fact that `victim.next.ptr == tail`
in the CAS expected value, meaning no node with a larger key exists in the list.

**Why this works but the same trick does not apply to `extractMin`**: the minimum node
is always right after `head`, so `prev_` on the first node always points to `head`. A
concurrent insert of a smaller key would target exactly the same `head.next` CAS word,
creating a race that the `EXT_MIN` epoch resolves by blocking those inserts. For the
maximum, a concurrent insert of a larger key changes `victim.next` away from `tail`,
which the CAS expected value detects automatically — no blocking needed.

---

## Linearization Points

### insert

| Outcome | LP | Pre-condition | Post-condition |
|---|---|---|---|
| Success: key not in S | `cas_next(pred, pred.next, {new_node, Ok})` — calling thread | key ∉ S | S ∪ {key} |
| Failure: key already live | Read of `curr.next` revealing `state == Ok` where `curr.key == key` — calling thread | key ∈ S | S unchanged |

*Note*: if `list_find` returns a `curr` with matching key but state `Deleted`, the insert
falls through and creates a fresh node. The LP is still the insertion CAS, logically
ordered after the deletion LP of the dead node.

### remove

| Outcome | LP | Pre-condition | Post-condition |
|---|---|---|---|
| Success | `cas_next(curr, curr.next, {next.ptr, Deleted})` — calling thread | key ∈ S | S \ {key} |
| Failure: key absent | Return after `list_find` returns `curr.key ≠ key` — calling thread | key ∉ S | S unchanged |
| Failure: already deleted | Normalize step observing `curr` already `Deleted` — calling thread | key ∉ S | S unchanged |

Physical removal (`help_deleted_next`) may be performed by any thread; it does not
constitute a LP.

### min

| Outcome | LP | Pre-condition | Post-condition |
|---|---|---|---|
| Returns key | Read of `node.next` revealing `state == Ok` (first such read) | key = min(S) at that instant | S unchanged |
| Returns NO\_KEY | Pointer advance that first makes `node == tail` | S = ∅ at that instant | S unchanged |

### max

| Outcome | LP | Pre-condition | Post-condition |
|---|---|---|---|
| Returns key | The last read of `node.next` revealing `state == Ok` before `node == tail` | key = max(S) at that instant | S unchanged |
| Returns NO\_KEY | Pointer advance that first makes `node == tail` with no alive node found | S = ∅ at that instant | S unchanged |

### extractMin

| Outcome | LP | Thread | Pre-condition | Post-condition |
|---|---|---|---|---|
| Success — calling thread marks victim | `victim.next.CAS((vsucc,Ok),(vsucc,ExtMin))` in extractMin inner loop | Calling thread | S ≠ ∅; victim.key = min(S) | S \ {victim.key} |
| Success — helper marks victim | `victim.next.CAS((vsucc,Ok),(vsucc,ExtMin))` in helpExtMin | Helper thread | S ≠ ∅; victim.key = min(S) | S \ {victim.key} |
| Returns EMPTY — calling thread clears | `head.next.CAS((hptr,ExtMin),(tail,Ok))` when victim == tail | Calling thread | S = ∅ | S = ∅ |
| Returns EMPTY — helper clears | `head.next.CAS((hptr,ExtMin),(tail,Ok))` in helpExtMin | Helper thread | S = ∅ | S = ∅ |

*LP ownership*: the LP belongs to whichever thread executes the winning
`victim.next.CAS`. At most one `MARKED_EXT_MIN` node exists per epoch.
The head-advance CAS `head.next.CAS((hptr,EXT_MIN),(vsucc,CLEAR))` is the
physical-removal step — it is **not** a second LP.

*Concurrent remove*: if `rem(k)` and `extractMin` race on the same victim,
exactly one `CAS` on `victim.next` wins. If `rem` wins, `extractMin` restarts its scan.
If `extractMin` wins, `rem`'s CAS fails and `rem` returns false.

*Concurrent insert at head*: while `EXT_MIN` is set, a head insert checks
`head.next.state` and calls `helpExtMin` before retrying. The insert LP therefore falls
strictly before or strictly after the `extractMin` LP — never concurrently.

### extractMax

The implemented simplified (no-epoch) extractMax has no helper thread and no `tail_anchor`.

| Outcome | LP | Thread | Pre-condition | Post-condition |
|---|---|---|---|---|
| Success | `victim.next.CAS((tail,Ok),(tail,ExtMax))` | Calling thread | S ≠ ∅; victim.key = max(S); victim.next.ptr == tail | S \ {victim.key} |
| Returns EMPTY | The read `victim == null` after the forward scan finds no Ok node | Calling thread | S = ∅ | S = ∅ |

*LP ownership*: symmetric to extractMin. The physical removal `cas_next(pred, pred.next,
{vsucc,Ok})` is not a LP.

*DLL asymmetry*: unlike extractMin (where the head-advance targets `head.next`),
the physical removal for extractMax targets `pred.next` — an interior pointer —
because the maximum is an arbitrary node, not the first after head. The `victim.prev_`
hint and forward walk are used to locate the predecessor before the physical-removal CAS.

*extractMin vs extractMax when |S| = 1*: if the same node is both min and max,
the two `victim.next.CAS` operations race. Exactly one wins. The loser rescans,
finds the set empty (no Ok node in Phase 1), and returns EMPTY.

*Concurrent insert at tail*: a concurrent insert of a new maximum changes
`victim.next` away from `tail`, causing the stamp CAS to fail. The insert LP
and the (failed) stamp CAS are serialized by `victim.next`: either the insert
succeeds first (stamp CAS sees wrong expected value, rescan finds new maximum)
or the stamp CAS wins first (insert's CAS on pred.next fails, insert retries).

---

## BST hint index (bst_hint.h and bst_balanced_hint.h)

Two BST hint implementations are provided:

- **HintBST** (`bst_hint.h`): adapted from the Natarajan-Mittal external lock-free BST.
  Uses DEBRA epoch-based retirement for BST nodes (same DEBRA instance as the list layer).
  When `remove(key, debra, tid)` commits a deletion (wins the FLAG_BIT CAS on the parent's
  child pointer), it retires **both** the leaf node and the spliced-out parent (internal)
  node via `debra.retireRaw`. Retiring only the leaf was unsafe: the parent persisted
  in memory (via `alloc_head_`) and still held a stale child pointer to the retired leaf,
  allowing `find_close` to reach a freed leaf through the live parent — heap-use-after-free.
  Retiring both together is safe because `find_close` and all BST operations run inside a
  DEBRA critical section, so DEBRA guarantees neither node is freed while any thread holds
  a pointer obtained during its current CS.
  At shutdown: `~HintBST` walks the live tree (`free_live_nodes`) to delete all nodes
  still reachable from the root. DEBRA-retired (spliced-out) nodes are not reachable from
  the root and are freed by `~Debra()`. No overlap, no double-free.

- **BalancedHintBST** (`bst_balanced_hint.h`): adapted from Brown's chromatic tree
  (PPoPP 2014). Uses LLX/SCX primitives to rebalance after inserts and removals.
  All `BSTNodeC` and `SCXRecC` objects are tracked via intrusive lists (`alloc_head_` and
  `alloc_recs_`) and freed in bulk by `~BalancedHintBST()`. No runtime DEBRA retirement
  for tree nodes.

Both share the same logical interface, but `remove` signatures differ:
- `find_close(key)` — descend to a leaf and return its `list_ptr`. Read-only; no LLX.
- `try_set(key, node*)` — insert or update the leaf for `key` to point to `node`.
  Called by the inserter after a successful `list_insert`.
- `remove(key, debra, tid)` (`HintBST`) / `remove(key)` (`BalancedHintBST`) — null the
  leaf's `list_ptr` and splice it from the tree.
  Called by the remover/extractor to prevent `find_close` from returning a stale hint.

### BST hint lifecycle and ordering discipline

The BST hint layer is advisory. Correctness of the ordered set depends only on the list;
the BST only affects traversal start position. However, the hint's `list_ptr` must not
point to a freed list node, because `find_close` is called at the start of each
operation and the returned pointer is dereferenced inside a DEBRA critical section.

**Safe ordering in `remove(key)`**:

```
// Wrong: bst.remove nulls hint before list_erase retires the node.
// A concurrent insert's try_set then re-populates the hint between
// bst.remove and list_erase, and the insert guard may see state=Ok
// (list_erase hasn't marked the node Deleted yet), leaving the hint
// pointing to a node that will soon be retired and freed.

// Correct:
hint = bst.find_close(key)
ok   = list_erase(key)        // marks node Deleted; retires it via DEBRA
bst.remove(key)               // nulls hint AFTER retirement, still in CS
```

After `list_erase` marks the node Deleted, any concurrent inserter's guard sees
`state != Ok` and calls `bst.remove` itself. The retired node cannot be freed until the
retiring thread exits its critical section and two more epochs pass — by that point
`bst.remove` has already been called, so the hint is null before the node is freed.

**Safe ordering in `extract_min / extract_max`**:

```
v = list_extract_min()   // retires victim internally, still in CS
bst.remove(v)            // nulls hint after retirement, still in CS
```

Same reasoning applies: the hint is nulled while the victim is still in the DEBRA bag
(protected by the critical section). No thread can free the victim until after both the
extractor's `enterQuiescentState` and two epoch advances, by which time the hint is null.

**Insert guard**:

After `list_insert` succeeds and `try_set` stores the new node in the BST, a concurrent
remove may have already called `bst.remove(key)` (nulling the hint) and `list_erase`
(marking the node Deleted) before the insert's `try_set` ran — re-populating the hint
with a newly-retired node. The guard catches this:

```
ok = list_insert(key, new_node)
if ok:
    bst.try_set(key, new_node)
    if read_next(new_node).state != Ok:   // node was removed while we were in try_set
        bst.remove(key)                   // null the hint we just set
```

Because `new_node` is only marked `Deleted`/`ExtMin`/`ExtMax` after the CAS in
`try_delete`/`extract_min`/`extract_max`, and DEBRA prevents the node from being freed
while the inserter is still in its critical section, the guard can safely read
`new_node.state` and call `bst.remove` if needed.

### BalancedHintBST: leaf-sibling in update_erase

When `update_erase` removes a leaf `l` with parent `p` and sibling `s`, it replaces `p`
with a new node that absorbs both weights:

```
ns_new = make_int(s->key, p->weight + s->weight, sl, sr)
```

If `s` is itself a leaf, `sl = sr = nullptr`, and `make_int` produces an internal node
with null children. When `update_rebalance` later traverses through `ns_new` and loads
`ns_new->left = nullptr` into its loop variable `l`, the next iteration dereferences `l`
→ null pointer dereference → SIGSEGV.

Fix: when `s` is a leaf (detected by `sl == nullptr`), create a leaf instead:

```cpp
BSTNodeC* ns_new = (sl == nullptr)
    ? make_leaf_w(s->key, nullptr, nw)   // leaf sibling; nullptr: don't copy s->list_ptr
    : make_int(s->key, nw, sl, sr);      // internal sibling
```

`nullptr` is used for the new leaf's `list_ptr` (not `s->list_ptr`): `s->list_ptr` may
point to a list node that was already retired and placed in a DEBRA bag; copying it
would allow `find_close` to return a freed pointer after the next epoch drain.
The new leaf `make_leaf_w` accepts an explicit weight so the coalesced weight `nw` is
preserved. The BST is advisory so any temporary weight violation on the resulting leaf
is harmless — `update_rebalance` returns early when it encounters a leaf.

### BalancedHintBST: leaf-split race

When `update_insert` splits a leaf `l` (key B) to make room for a new key A, it creates
a new leaf for B to replace the old one:

```
// Incorrect: copies the old list_ptr; races with a concurrent remove(B)
// that may have already nulled the old leaf's list_ptr and retired the node.
cl = make_leaf(B, l.list_ptr.load())   // BUG: stale pointer copied to new leaf

// Correct: the new leaf starts with a null hint for B.
cl = make_leaf(B, nullptr)
```

If the old leaf's list_ptr is copied, a concurrent `remove(B)` that nulled the old
leaf's pointer does not see the new leaf, so the new leaf retains the stale pointer even
after the node is retired and freed. Using `nullptr` for the new leaf is safe because
the BST is advisory: the hint for B will be null until the next insert of B calls
`try_set`.

---

## Memory reclamation (DEBRA)

Memory reclamation uses **DEBRA** (Dynamically Escalating Barriers with Recorded
Announcements), implemented in `debra.h` following Brown's PODC 2015 paper.

### Design

Each thread maintains three epoch bags. At the start of every operation a thread:
1. Sets `in_cs[tid] = true` (seq-cst) to announce it is in a critical section.
2. Reads the global epoch `ge` (seq-cst) and stores it in `thread_epoch[tid]`.
3. Every `DEBRA_CHECK_FREQ` operations it calls `tryAdvanceEpoch`.

`tryAdvanceEpoch` checks that every active thread (those with `in_cs=true`) has
announced the current global epoch. If so, it advances the epoch by 2 and drains the
bag that is 2 epochs old — freeing the nodes retired during that epoch.

At the end of every operation the thread sets `in_cs[tid] = false`.

### What is reclaimed

- **List nodes** (`Node*`): retired by `evict_deleted` (for Deleted nodes),
  `help_ext_min`/`list_extract_max` (for ExtMin/ExtMax nodes), via `debra.retireNode`.
  Freed in `EpochBag::drain`. The drain loop also frees the live PrevWrapper still
  stored in `prev_` at drain time (the one not yet superseded by a new write).

- **PrevWrapper objects**: retired via `debra.retirePW` whenever a PrevWrapper is
  replaced by a new one. In the barrier variant this happens inside `cas_prev`
  (after a successful CAS). In the pragmatic variant it happens inside
  `store_prev_direct` (after each plain exchange). Freed in `EpochBag::drain`.

### Critical ordering: only retire Deleted nodes in evict_deleted

`evict_deleted` physically removes a logically-deleted node from the list. It must only
retire nodes with state `Deleted` — not `ExtMin` or `ExtMax`. Nodes in those states are
owned by their extraction protocol; retiring them in `evict_deleted` too would cause a
double-retire and eventual double-free:

```cpp
// In evict_deleted / evict_deleted_p:
if (cas_next(prev, prev.next, {next.ptr, Ok})) {
    if (current.marked_next.state == Deleted)   // ONLY Deleted
        debra.retireNode(tid, current.self)
    return true
}
```

ExtMin/ExtMax nodes are always retired by their own extraction protocol after the
head/pred advance CAS succeeds.

### victim_removed path in extractMax

When `list_extract_max` walks forward from head to physically remove the claimed victim
(state ExtMax), it may find that another thread's `evict_deleted` has already unlinked
the victim. In that case `victim_removed = true` and the extractor must still retire the
victim (it won the stamp CAS and owns the node):

```cpp
if (victim_removed) {
    debra.retireNode(tid, victim)
    return victim.key
}
```

Without this, the victim leaks (never freed).

### in_cs announcement ordering

The `in_cs[tid]` flag must be set to `true` **before** reading and announcing
`thread_epoch[tid]`. If the order were reversed, another thread's `tryAdvanceEpoch`
could see this thread as quiescent (in_cs=false), advance the epoch, and free a node
that this thread is about to read. The seq-cst ordering on the `in_cs` store closes
this window:

```cpp
void leaveQuiescentState(int tid) {
    in_cs[tid].store(true, seq_cst);     // visible before epoch read
    int ge = global_epoch.load(seq_cst); // read after in_cs is set
    thread_epoch[tid].store(ge, release);
    ...
}
```
