#pragma once
#include "node.h"

// Lock-free doubly-linked ordered list following Melnikov & Sela's Hinted Ordered List.
//
// The authoritative structure is the forward next_ chain. prev_ pointers are
// best-effort backward hints that are kept approximately correct via barriers.
//
// Memory reclamation: removed nodes are not freed. Add epoch-based or hazard-pointer
// reclamation before production use.
//
// Forward declarations (functions are mutually recursive).
static bool normalize_next_node(NodeSnapshot& current, NodeSnapshot& next);
static bool help_deleted_next(NodeSnapshot& current, NodeSnapshot& next);
static bool evict_deleted(NodeSnapshot& prev, NodeSnapshot& current, NodeSnapshot& next);
static inline void help_ext_min(Node* head, Node* tail);

// ---- barrier ----
// Ensures current.marked_next_.ptr_->prev_ points to current.self_.
// to_check is a stability anchor: if its snapshot changed since last read, update it and
// return false so the caller can retry with fresh information.
// Returns true when the back-pointer is already correct or was successfully set.
//
// Special case: if current has no successor (current.marked_next_.ptr_ == nullptr, i.e.
// current is the tail sentinel), the invariant is vacuously satisfied — return true.
static inline bool barrier(NodeSnapshot& current, NodeSnapshot& to_check) {
    Node* next_node = current.marked_next_.ptr_;
    if (next_node == nullptr) return true;  // tail sentinel: no successor to fix

    CollisionlessPtr backptr = read_prev(next_node);
    NodeSnapshot check = take_snapshot(to_check.self_);
    if (to_check != check) {
        to_check = check;
        return false;
    }
    if (unwrap(backptr) == current.self_) return true;
    cas_prev(next_node, backptr, current.self_);
    return unwrap(backptr) == current.self_;
}

static inline bool barrier(NodeSnapshot& current) {
    return barrier(current, current);
}

// After evicting a deleted node, fix the back-pointer of the new successor if it still
// points to the evicted node.
static inline void post_evict_fix(NodeSnapshot& prev, NodeSnapshot& evicted) {
    Node* next_node = prev.marked_next_.ptr_;
    if (next_node == nullptr) return;
    CollisionlessPtr backptr = read_prev(next_node);
    if (unwrap(backptr) == evicted.self_)
        cas_prev(next_node, backptr, prev.self_);
}

// Physically unlink the deleted node 'current' from between 'prev' and 'next'.
// All three barriers must succeed before the CAS to guarantee back-pointer invariants.
// Accepts any non-Ok state as logically removed (Deleted, ExtMin, ExtMax).
static inline bool evict_deleted(NodeSnapshot& prev, NodeSnapshot& current, NodeSnapshot& next) {
    if (!barrier(prev)) return false;
    if (!barrier(current, prev)) return false;
    NodeSnapshot& to_check = (next.marked_next_.state_ == Ok) ? next : prev;
    if (!barrier(next, to_check)) return false;
    return cas_next(prev.self_, prev.marked_next_, {next.self_, Ok});
}

// If 'next' is logically removed (any non-Ok state), evict it from after 'current'
// and repair the successor's back-pointer.
static inline bool help_deleted_next(NodeSnapshot& current, NodeSnapshot& next) {
    if (next.marked_next_.state_ == Ok) return true;
    NodeSnapshot nn = take_snapshot(next.marked_next_.ptr_);
    if (!evict_deleted(current, next, nn)) return false;
    post_evict_fix(current, next);
    return true;
}

// Ensures that 'current' is followed by a non-deleted node.
// Modifies both 'current' and 'next' snapshots as nodes are evicted.
// Returns false if 'current' itself becomes logically removed in the process.
static inline bool normalize_next_node(NodeSnapshot& current, NodeSnapshot& next) {
    while (is_removed(next.marked_next_.state_)) {
        if (is_removed(current.marked_next_.state_)) return false;
        help_deleted_next(current, next);
        if (current.marked_next_.ptr_ != next.self_)
            next = take_snapshot(current.marked_next_.ptr_);
    }
    return current.marked_next_.state_ == Ok;
}

// Insert new_node after current. Normalizes, runs barriers, then CASes into the list.
// Returns false on any conflict; the caller must retry with a fresh list_find.
static inline bool try_insert_after(NodeSnapshot& current, Node* new_node) {
    if (current.marked_next_.state_ != Ok) return false;

    NodeSnapshot next = take_snapshot(current.marked_next_.ptr_);
    if (!normalize_next_node(current, next)) return false;
    if (!barrier(current)) return false;
    if (!barrier(next))    return false;

    new_node->prev_.store(new PrevWrapper(current.self_), std::memory_order_relaxed);
    new_node->next_.store(mp_encode({next.self_, Ok}),    std::memory_order_relaxed);

    if (!cas_next(current.self_, current.marked_next_, {new_node, Ok})) return false;

    NodeSnapshot inserted = take_snapshot(new_node);
    barrier(inserted);  // repair next->prev_ to point to new_node
    return true;
}

// Logically delete 'current' (which is preceded by 'prev') by marking its next_.
// Physical removal is best-effort; any traversal will finish it lazily.
// Returns false if another thread already deleted current or the state changed.
static inline bool try_delete(NodeSnapshot& prev, NodeSnapshot& current) {
    NodeSnapshot next = take_snapshot(current.marked_next_.ptr_);
    if (!normalize_next_node(current, next)) return false;

    if (!cas_next(current.self_, current.marked_next_, {next.self_, Deleted}))
        return false;

    // Logical deletion done. Best-effort physical removal.
    help_deleted_next(prev, current);
    return true;
}

// ---- traversal ----

// Walk backward from hint via prev_ pointers until finding a living node with key < target.
// Bounded walk (64 steps); falls back to head if no suitable node is found.
static inline Node* find_living_backward(Node* hint, long key, Node* head) {
    if (hint == nullptr) return head;
    Node* p = hint;
    for (int i = 0; i < 64; ++i) {
        MarkedPtr pm = read_next(p);
        if (pm.state_ == Ok && (p == head || p->key_ < key))
            return p;
        Node* pp = unwrap(read_prev(p));
        if (pp == nullptr || pp == p) return head;
        p = pp;
    }
    return head;
}

// Active traversal: finds (pred, curr) such that pred.key_ < key <= curr.key_, both alive.
// Uses the hint as a starting point; restarts from a backward walk whenever a node along
// the path is found to be deleted.
static inline void list_find(Node* head, Node* hint, long key,
                             NodeSnapshot* pred_out, NodeSnapshot* curr_out) {
    Node* start = find_living_backward(hint, key, head);

    while (true) {
        NodeSnapshot pred = take_snapshot(start);

        while (true) {
            // If pred itself is logically removed, restart backward
            if (is_removed(pred.marked_next_.state_)) {
                start = find_living_backward(pred.self_, key, head);
                break;
            }

            NodeSnapshot next = take_snapshot(pred.marked_next_.ptr_);
            if (!normalize_next_node(pred, next)) {
                start = find_living_backward(pred.self_, key, head);
                break;
            }

            if (next.self_->key_ >= key) {
                *pred_out = pred;
                *curr_out = next;
                return;
            }

            pred = next;
        }
    }
}

// ---- public list operations ----

// Insert key. Returns true if inserted, false if already present.
// Linearization point: the successful CAS in try_insert_after.
static inline bool list_insert(Node* head, Node* tail, Node* hint, long key) {
    Node* new_node = new Node(key);
    Node* start = hint;

    while (true) {
        NodeSnapshot pred, curr;
        list_find(head, start, key, &pred, &curr);

        if (curr.self_->key_ == key) {
            delete new_node;
            return false;
        }

        // Head-insert gate: if an EXT_MIN epoch is in progress on head->next_,
        // help complete it before inserting right after head.
        if (pred.self_ == head && read_next(head).state_ == ExtMin) {
            help_ext_min(head, tail);
            start = head;
            continue;
        }

        if (try_insert_after(pred, new_node)) return true;

        start = pred.self_;
    }
}

// Remove key. Returns true if this call performed the logical deletion.
// Linearization point: the successful CAS in try_delete that marks curr->next_.
static inline bool list_erase(Node* head, Node* hint, long key) {
    Node* start = hint;

    while (true) {
        NodeSnapshot pred, curr;
        list_find(head, start, key, &pred, &curr);

        if (curr.self_->key_ != key) return false;

        if (try_delete(pred, curr)) return true;

        start = pred.self_;
    }
}

// Return the minimum key, or NO_KEY if empty. Scans forward from head.
// Linearization point: the read of the first non-deleted node after head.
static inline long list_min(Node* head, Node* tail) {
    Node* cur = read_next(head).ptr_;
    while (cur != nullptr && cur != tail) {
        MarkedPtr nw = read_next(cur);
        if (nw.state_ == Ok) return cur->key_;
        cur = nw.ptr_;
    }
    return NO_KEY;
}

// Return the maximum key, or NO_KEY if empty.
// Uses tail->prev_ as a backward hint to start the forward scan close to the tail,
// then scans forward to find the last non-deleted node before tail.
// Linearization point: the read of the last non-deleted node found.
static inline long list_max(Node* head, Node* tail) {
    Node* start = unwrap(read_prev(tail));
    if (start == nullptr) start = head;

    // Walk backward past any logically removed nodes to find a good scan start.
    for (int i = 0; i < 64 && start != head; ++i) {
        if (read_next(start).state_ == Ok) break;
        Node* pp = unwrap(read_prev(start));
        if (pp == nullptr || pp == start) { start = head; break; }
        start = pp;
    }

    // Forward scan from start to tail; track the last alive node.
    long  last_key = NO_KEY;
    Node* cur      = (start == head) ? read_next(head).ptr_ : start;
    while (cur != nullptr && cur != tail) {
        MarkedPtr nw = read_next(cur);
        if (nw.state_ == Ok) last_key = cur->key_;
        cur = nw.ptr_;
    }
    return last_key;
}

// ---- helpExtMin ----
// Brings any in-progress EXT_MIN epoch to completion.
// head->next_.state == ExtMin means an epoch is active.
// Upon return, head->next_.state == Ok (CLEAR).
static inline void help_ext_min(Node* head, Node* tail) {
    while (true) {
        MarkedPtr hmp = read_next(head);
        if (hmp.state_ != ExtMin) return;

        Node* first = hmp.ptr_;

        // Victim scan: find first non-logically-removed node reachable from 'first'.
        // Skip Deleted and ExtMax nodes (both are logically gone). Stop at Ok or ExtMin.
        Node* victim = first;
        while (victim != tail) {
            MarkedPtr vmp = read_next(victim);
            if (vmp.state_ == Ok || vmp.state_ == ExtMin) break;
            victim = vmp.ptr_;
        }

        if (victim == tail) {
            // List is effectively empty; clear epoch and advance head past everything.
            MarkedPtr exp = hmp;
            cas_next(head, exp, {tail, Ok});
            // Whether we won or lost, re-check from the top.
            continue;
        }

        // Re-read victim.next to see its current state.
        MarkedPtr vmp = read_next(victim);

        if (vmp.state_ == ExtMin) {
            // Victim already stamped — advance head past victim.
            // Re-read vmp to get the current (fresh) successor pointer.
            MarkedPtr fresh_vmp = read_next(victim);
            Node* succ = fresh_vmp.ptr_;

            NodeSnapshot snap_head   = take_snapshot(head);
            NodeSnapshot snap_victim = take_snapshot(victim);
            NodeSnapshot snap_vsucc  = succ ? take_snapshot(succ) : snap_victim;
            barrier(snap_head);
            barrier(snap_victim, snap_head);
            if (succ) barrier(snap_vsucc, snap_victim);

            MarkedPtr exp = read_next(head);
            // Only advance if we are still in the epoch that started with 'first'.
            // This prevents a stale helper from clobbering a newer epoch on the same node.
            if (exp.state_ == ExtMin && exp.ptr_ == first)
                cas_next(head, exp, {succ, Ok});
            post_evict_fix(snap_head, snap_victim);
            continue;
        }

        if (vmp.state_ == Ok) {
            // Try to stamp victim with ExtMin.
            MarkedPtr exp     = vmp;
            MarkedPtr desired = {vmp.ptr_, ExtMin};
            cas_next(victim, exp, desired);
            // Whether we won or lost, loop again to perform the head advance.
            continue;
        }

        // vmp.state_ is Deleted (or ExtMax): victim was concurrently removed; restart scan.
    }
}

// ---- extractMin ----
// Atomically removes and returns the minimum key.
// Returns NO_KEY if the list is empty.
// Linearization point: the CAS that stamps victim->next_ with ExtMin.
//
// Design: only the thread that wins the stamp CAS returns the key.
// The epoch (ExtMin on head->next_) prevents concurrent extractMin threads from
// interleaving their victim selections.
static inline long list_extract_min(Node* head, Node* tail) {
    while (true) {
        MarkedPtr hmp = read_next(head);

        if (hmp.state_ == ExtMin) {
            // Another thread's epoch is open; help complete it, then retry.
            help_ext_min(head, tail);
            continue;
        }

        // head->next_ is Ok; try to open an EXT_MIN epoch.
        Node* first  = hmp.ptr_;
        MarkedPtr exp = hmp;
        if (!cas_next(head, exp, {first, ExtMin}))
            continue;  // lost epoch CAS; retry

        // We own the epoch. Scan from 'first' past Deleted/ExtMax nodes to find victim.
        Node* victim = first;
        while (victim != tail) {
            MarkedPtr vmp = read_next(victim);
            // Skip nodes already logically removed by delete or extractMax.
            // Stop at Ok (our candidate) or ExtMin (a helper already stamped it).
            if (vmp.state_ == Ok || vmp.state_ == ExtMin) break;
            victim = vmp.ptr_;
        }

        if (victim == tail) {
            // List is empty. Close epoch and return NO_KEY.
            help_ext_min(head, tail);
            return NO_KEY;
        }

        // victim is our candidate. Try to stamp its next_ with ExtMin.
        MarkedPtr vmp = read_next(victim);

        if (vmp.state_ == ExtMin) {
            // A helper already stamped victim. We are the epoch owner so this key
            // belongs to us — complete the epoch and return victim->key_.
            help_ext_min(head, tail);
            return victim->key_;
        }

        if (vmp.state_ == Ok) {
            // Retry the stamp while victim remains Ok.
            // A concurrent insert-after-victim changes only victim->next_.ptr
            // (not the state), so the CAS can fail even though victim is still
            // the minimum.  Loop with the updated ptr instead of abandoning.
            while (vmp.state_ == Ok) {
                MarkedPtr vexp = vmp;
                if (cas_next(victim, vexp, {vmp.ptr_, ExtMin})) {
                    // We stamped it. Complete and return.
                    help_ext_min(head, tail);
                    return victim->key_;
                }
                vmp = vexp;  // updated by cas_next to the current next_ value
            }
            if (vmp.state_ == ExtMin) {
                // A concurrent helper stamped it; we own the epoch so the key
                // belongs to this extractMin call.
                help_ext_min(head, tail);
                return victim->key_;
            }
            // victim was concurrently Deleted or ExtMax'd by another operation;
            // close the epoch and retry to find the new minimum.
            help_ext_min(head, tail);
            continue;
        }

        // victim->next_.state_ is Deleted or ExtMax — victim was removed concurrently.
        // Close epoch and retry.
        help_ext_min(head, tail);
    }
}

// ---- extractMax (simplified, no epoch) ----
// Uses ExtMax=3 to stamp the victim's next_ field.
// The CAS atomically verifies "still the maximum and alive" and stamps ExtMax.
// Returns NO_KEY if the list is empty.
// Linearization point: the CAS that stamps victim->next_ with ExtMax.
static inline long list_extract_max(Node* head, Node* tail) {
    while (true) {
        // Use tail->prev_ as a backward hint, then walk forward to find the last Ok node.
        Node* start = unwrap(read_prev(tail));
        if (start == nullptr) start = head;

        // Walk backward past logically removed nodes to a good starting point.
        for (int i = 0; i < 64 && start != head; ++i) {
            if (read_next(start).state_ == Ok) break;
            Node* pp = unwrap(read_prev(start));
            if (pp == nullptr || pp == start) { start = head; break; }
            start = pp;
        }

        // Forward scan to find the last Ok node before tail, noting any ExtMax-stamped node.
        Node* victim = nullptr;
        Node* cur    = (start == head) ? read_next(head).ptr_ : start;
        while (cur != nullptr && cur != tail) {
            MarkedPtr nw = read_next(cur);

            if (nw.state_ == ExtMax) {
                // Another extractMax already claimed this node — help physically remove it
                // and return its key.
                Node* pred = unwrap(read_prev(cur));
                if (pred == nullptr) pred = head;
                NodeSnapshot snap_pred   = take_snapshot(pred);
                NodeSnapshot snap_cur    = take_snapshot(cur);
                NodeSnapshot snap_succ   = take_snapshot(nw.ptr_);
                if (barrier(snap_pred) &&
                    barrier(snap_cur,  snap_pred) &&
                    barrier(snap_succ, snap_pred)) {
                    MarkedPtr pexp = snap_pred.marked_next_;
                    if (pexp.ptr_ == cur && pexp.state_ == Ok)
                        cas_next(pred, pexp, {nw.ptr_, Ok});
                    post_evict_fix(snap_pred, snap_cur);
                }
                return cur->key_;
            }

            if (nw.state_ == Ok) victim = cur;
            cur = nw.ptr_;
        }

        if (victim == nullptr) return NO_KEY;

        // Atomically verify victim is still the maximum and stamp ExtMax.
        MarkedPtr vmp = read_next(victim);
        if (vmp.ptr_ != tail || vmp.state_ != Ok)
            continue;  // not the maximum anymore, or already removed

        MarkedPtr vexp = vmp;
        if (!cas_next(victim, vexp, {tail, ExtMax}))
            continue;  // lost the stamp race

        // We own victim. Physically remove it by updating pred->next_ to skip victim.
        while (true) {
            Node* pred = unwrap(read_prev(victim));
            if (pred == nullptr) pred = head;

            // Walk pred forward to find the node that currently points to victim.
            for (int i = 0; i < 64; ++i) {
                MarkedPtr pnw = read_next(pred);
                if (pnw.ptr_ == victim) break;
                if (pnw.state_ != Ok || pnw.ptr_ == nullptr ||
                    pnw.ptr_->key_ > victim->key_) {
                    pred = head; break;
                }
                pred = pnw.ptr_;
            }

            NodeSnapshot snap_pred   = take_snapshot(pred);
            NodeSnapshot snap_victim = take_snapshot(victim);
            NodeSnapshot snap_tail   = take_snapshot(tail);

            if (!barrier(snap_pred))              continue;
            if (!barrier(snap_victim, snap_pred)) continue;
            barrier(snap_tail, snap_victim);

            MarkedPtr pexp = snap_pred.marked_next_;
            if (pexp.ptr_ != victim || pexp.state_ != Ok) continue;

            if (cas_next(pred, pexp, {tail, Ok})) {
                post_evict_fix(snap_pred, snap_victim);
                return victim->key_;
            }
        }
    }
}
