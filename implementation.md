# Implementation Notes

Implements the "Hinted Ordered List" from Melnikov & Sela: _Concurrent extrema and
successor/predecessor queries in search trees_. The ordered set lives entirely in the
linked list. The BST is a hint index whose only job is `find_close(key) → Node*`.

---

## File layout

```
implementation/
├── node.h            Core types: State, MarkedPtr, CollisionlessPtr, Node, NodeSnapshot
├── list.h            Lock-free doubly-linked list (barrier, normalize, insert, erase, min, max, extractMin, extractMax)
├── bst_hint.h        Lock-free external BST hint index (Natarajan-Mittal, adapted)
└── bst_list.h        BSTList class — public API combining list + BST
```

Build: `g++-14 -std=c++17 -O2 -pthread -o test test.cpp`

---

## Key data structures (node.h)

### Sentinel nodes

`head` (key −∞) and `tail` (key +∞) are permanent; they cannot be deleted.
The list invariant is: `head → [live nodes in strictly ascending key order] → tail`
with approximate backward `prev_` pointers maintained alongside.

### State / MarkedPtr

Each node's `next_` field encodes both the successor pointer and a one-bit state:

```
State ∈ { Ok, Deleted }
  Ok      — node is logically present in the set
  Deleted — node is logically removed (immutable from this point)
```

Deletion is a one-way transition: once `Deleted`, a node's `next_` and `state` never
change. This is Harris's singly-linked-list mark, packed into the low bit of `next_`.

### CollisionlessPtr

`prev_` back-pointers are susceptible to ABA: a CAS might succeed even though the
pointer was replaced and restored between the read and the CAS. To prevent this, each
write of `prev_` allocates a new `PrevWrapper` heap object containing the target `Node*`.
Two writes of the same node produce different wrapper addresses, so any CAS on
`CollisionlessPtr` that sees the same address knows the value truly has not changed.
Old wrappers are leaked (no reclamation — safe since addresses are never reused).

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
operations. It starts from the BST hint (or falls back to `head`) and first walks
backward via `prev_` pointers to find a living node whose key is strictly below the
target — this keeps retries local instead of always restarting from `head`. It then
walks forward, evicting any logically deleted nodes it encounters along the way, until
it brackets the target key between two live nodes `pred` and `curr`. If at any point
`pred` itself becomes deleted during the forward walk, the whole process restarts from
a fresh backward search near that position.

```
list_find(head, hint, key) → (pred, curr):

  start = find_living_backward(hint, key, head)
  // find_living_backward walks backward up to 64 steps via prev_ until it finds
  // a living node with key < target; falls back to head if none found.

  retry:
    pred = take_snapshot(start)

    while true:
      if pred.state == Deleted:
        start = find_living_backward(pred.self_, key, head)
        goto retry

      curr = take_snapshot(pred.next.ptr)
      if not normalize_next_node(pred, curr):  // evict deleted nodes after pred
        start = find_living_backward(pred.self_, key, head)
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
node. The BST hint index is intentionally not updated on deletion to avoid a race with
concurrent re-insertions of the same key.

```
remove(key):
  hint = bst.find_close(key)
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
    // (barriers + cas_next(pred, pred.next, {next.ptr, Ok}) + post_evict_fix)
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

`max` uses `tail.prev_` as a backward hint to avoid scanning the entire list. It first
walks backward past any deleted nodes to find a reasonable starting point near the end,
then does a definitive forward scan all the way to `tail`, tracking the last live node
it sees. The backward step is just an optimisation; the forward scan to `tail` is
mandatory to ensure correctness — a node inserted after the hint but before `tail`
must not be missed. The key recorded during the last live node read before reaching
`tail` is the returned maximum.

```
max() → key | NO_KEY:
  // Use tail.prev_ as a backward hint to start close to the end.
  start = unwrap(tail.prev_)
  if start == null: start = head

  // Walk backward (up to 64 steps) past any deleted nodes.
  for i in 0..64:
    if start == head: break
    if start.next.state == Ok: break
    pp = unwrap(start.prev_)
    if pp == null or pp == start: start = head; break
    start = pp

  // Forward scan from start to tail; track the last alive node.
  last_key = NO_KEY
  node = (start == head) ? head.next.ptr : start

  while node ≠ tail:
    (succ, state) = node.next
    if state == Ok:
      last_key = node.key        // [LP — last such read before reaching tail]
    node = succ

  return last_key
```

**Linearization point**: the final read of `node.next` that reveals `state == Ok` and
updates `last_key` to the returned value. The forward scan to `tail` ensures we do not
miss any node inserted after the backward hint. If no alive node is found, the LP is
the pointer advance that first makes `node == tail`.

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
      snap_head   = take_snapshot(head)
      snap_victim = take_snapshot(victim)
      snap_vsucc  = take_snapshot(succ)   // skipped if succ == null
      barrier(snap_head)
      barrier(snap_victim, snap_head)
      if succ ≠ null: barrier(snap_vsucc, snap_victim)
      // Epoch guard: only advance head if still in the epoch started with 'first'.
      // Prevents a stale helper from clobbering a newer concurrent epoch.
      (hptr, hst) = head.next
      if hst == EXT_MIN and hptr == first:
        head.next.CAS((hptr, EXT_MIN), (succ, Ok))
      post_evict_fix(snap_head, snap_victim)
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
`victim.next.CAS({tail, CLEAR}, {tail, MARKED_EXT_MAX})` — exactly one wins (the LP),
the other sees `MARKED_EXT_MAX` on the victim and returns the same key. Physical
removal still requires finding the predecessor via `prev_` hints and running the three
DLL barriers, but that is unavoidable in a doubly-linked list regardless of the epoch
design.

```
extractMax() → key | EMPTY:
  while true:

    // ── Find last Ok node ────────────────────────────────────────────────
    // Walk backward from tail.prev_ hint past logically removed nodes, then
    // scan forward all the way to tail tracking the last Ok node seen.
    start = unwrap(tail.prev_)
    if start == null: start = head

    for i in 0..64:
      if start == head: break
      if start.next.state == Ok: break
      pp = unwrap(start.prev_)
      if pp == null or pp == start: start = head; break
      start = pp

    victim = null
    scan = (start == head) ? head.next.ptr : start
    while scan ≠ tail:
      (ssucc, sstate) = scan.next

      if sstate == ExtMax:
        // Another extractMax already claimed this node.
        // Help physically remove it, then return its key.
        pred = unwrap(read_prev(scan)) or head
        snap_pred = take_snapshot(pred)
        snap_scan = take_snapshot(scan)
        snap_succ = take_snapshot(ssucc)
        if barrier(snap_pred) and barrier(snap_scan, snap_pred)
                              and barrier(snap_succ, snap_pred):
          if snap_pred.next == {scan, Ok}:
            cas_next(pred, snap_pred.next, {ssucc, Ok})
          post_evict_fix(snap_pred, snap_scan)
        return scan.key        // [LP — the CAS that stamped ExtMax]

      if sstate == Ok: victim = scan
      scan = ssucc

    if victim == null:
      return EMPTY             // no Ok node found; set is empty [LP — this read]

    // ── Verify victim is still the maximum and stamp ExtMax ─────────────
    (vsucc, vstate) = victim.next
    if vsucc ≠ tail or vstate ≠ Ok:
      continue                 // new node inserted after victim, or already removed

    if not victim.next.CAS({tail, Ok}, {tail, ExtMax}):  // [LP]
      continue                 // lost race; rescan

    // ── Physical removal ──────────────────────────────────────────────────
    // Locate predecessor: start from victim.prev_ and walk forward (up to 64
    // steps) until pred.next.ptr == victim; fall back to head if not found.
    // Then run three DLL barriers and CAS pred.next past victim.
    while true:
      pred = unwrap(read_prev(victim)) or head
      for i in 0..64:
        pnw = pred.next
        if pnw.ptr == victim: break
        if pnw.state ≠ Ok or pnw.ptr == null or pnw.ptr.key > victim.key:
          pred = head; break
        pred = pnw.ptr

      snap_pred   = take_snapshot(pred)
      snap_victim = take_snapshot(victim)   // (tail, ExtMax)
      snap_vsucc  = take_snapshot(tail)

      if not barrier(snap_pred): continue
      if not barrier(snap_victim, snap_pred): continue
      barrier(snap_vsucc, snap_victim)

      // Guard: pred must currently point to victim with Ok state.
      if snap_pred.next ≠ {victim, Ok}: continue

      if cas_next(pred, snap_pred.next, {tail, Ok}):
        post_evict_fix(snap_pred, snap_victim)   // fix tail.prev_ if → victim
        return victim.key
```

**Linearization point**: the successful `victim.next.CAS({tail, CLEAR}, {tail, MARKED_EXT_MAX})`.
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
| Success — calling thread marks victim | `victim.next.CAS((vsucc,CLEAR),(vsucc,MARKED_EXT_MIN))` in extractMin inner loop | Calling thread | S ≠ ∅; victim.key = min(S) | S \ {victim.key} |
| Success — helper marks victim | `victim.next.CAS((vsucc,CLEAR),(vsucc,MARKED_EXT_MIN))` in helpExtMin | Helper thread | S ≠ ∅; victim.key = min(S) | S \ {victim.key} |
| Returns EMPTY — calling thread clears | `head.next.CAS((hptr,EXT_MIN),(tail,CLEAR))` when victim == tail | Calling thread | S = ∅ | S = ∅ |
| Returns EMPTY — helper clears | `head.next.CAS((hptr,EXT_MIN),(tail,CLEAR))` in helpExtMin | Helper thread | S = ∅ | S = ∅ |

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

| Outcome | LP | Thread | Pre-condition | Post-condition |
|---|---|---|---|---|
| Success — calling thread marks victim | `victim.next.CAS((vsucc,CLEAR),(vsucc,MARKED_EXT_MAX))` in extractMax inner loop | Calling thread | S ≠ ∅; victim.key = max(S) | S \ {victim.key} |
| Success — helper marks victim | `victim.next.CAS((vsucc,CLEAR),(vsucc,MARKED_EXT_MAX))` in helpExtMax | Helper thread | S ≠ ∅; victim.key = max(S) | S \ {victim.key} |
| Returns EMPTY — calling thread clears | `tail_anchor.CAS((lptr,EXT_MAX),(lptr,IDLE))` when no CLEAR node found | Calling thread | S = ∅ | S = ∅ |
| Returns EMPTY — helper clears | `tail_anchor.CAS((lptr,EXT_MAX),(lptr,IDLE))` in helpExtMax | Helper thread | S = ∅ | S = ∅ |

*LP ownership*: symmetric to extractMin. The physical removal `cas_next(pred, pred.next,
{vsucc,Ok})` is not a LP.

*DLL asymmetry*: unlike extractMin (where the head-advance targets `head.next`),
the physical removal for extractMax targets `pred.next` — an interior pointer —
because the maximum is an arbitrary node, not the first after head. The barriers and
`locate_pred` walk are the mechanism for finding and stabilising that interior
predecessor before the CAS.

*extractMin vs extractMax when |S| = 1*: if the same node is both min and max,
the two `victim.next.CAS` operations race. Exactly one wins. The loser rescans,
finds the set empty (reaches `tail` / no CLEAR node), and returns EMPTY with LP
at the appropriate epoch-clearing CAS.

*Concurrent insert at tail*: while `EXT_MAX` is set, a tail insert checks
`tail_anchor.state` and calls `helpExtMax` before retrying. The insert LP falls
strictly before or strictly after the `extractMax` LP — never concurrently.

---

## BST hint index (bst_hint.h)

Adapted from Natarajan-Mittal external lock-free BST. Leaf nodes carry a `list_ptr`
pointing to the corresponding list `Node`. `find_close(key)` descends to a leaf and
returns its `list_ptr`. `try_set(key, node*)` is called after a successful list insertion.
`try_remove` is intentionally not called on list deletion to avoid a race where a
concurrent re-insertion's BST entry would be silently removed.

---

## Memory reclamation

Nodes removed from the list and BST are not freed. `PrevWrapper` objects are created on
every back-pointer update and also not freed. This is safe for correctness without ABA
protection because node addresses are never reused. A production version needs
epoch-based or hazard-pointer reclamation before either `list_find` or `find_close` can
safely dereference a pointer that another thread might concurrently free.

---

## Open questions

- Add epoch-based reclamation so removed nodes and PrevWrappers are eventually freed.
- Benchmark BST hint benefit: how much does `find_close(key)` vs always starting from
  `head` reduce average traversal length?
- Evaluate whether tree rebalancing (e.g., Drachsler's relaxed AVL rotations) is worth
  the complexity for the hint layer.
