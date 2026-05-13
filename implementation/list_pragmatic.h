#pragma once
#include "node.h"

// Pragmatic lock-free doubly-linked ordered list.
//
// This is an alternative to list.h that replaces CAS-based barrier() calls
// with plain atomic stores for prev_ maintenance.  The approach is inspired
// by Träff & Pöter, "A more Pragmatic Implementation of the Lock-free,
// Ordered, Linked List" (arXiv:2010.15755).
//
// All linearization points are identical to list.h: they sit on next_ CAS
// operations and are unaffected by the change in prev_ update mechanism.
// Lock-freedom is preserved because no operation ever waits on a prev_ store.
//
// All public functions carry a _p suffix to allow list.h and list_pragmatic.h
// to coexist in the same compilation unit without name conflicts.
//
// Differences from list.h:
//   - barrier() is removed; no stability-anchor checks on eviction.
//   - post_evict_fix_p() uses an unconditional store instead of CAS.
//   - evict_deleted_p() skips all barrier calls; just tries the forward CAS.
//   - try_insert_after_p() repairs next->prev_ with a plain store after insert.
//   - ABA on prev_ is not protected; two concurrent stores to the same field
//     result in last-write-wins, which is acceptable because prev_ is an
//     approximate hint — correctness is guaranteed by the forward next_ chain.

// Forward declarations (functions are mutually recursive).
static bool normalize_next_node_p(NodeSnapshot& current, NodeSnapshot& next);
static bool help_deleted_next_p(NodeSnapshot& current, NodeSnapshot& next);
static bool evict_deleted_p(NodeSnapshot& prev, NodeSnapshot& current, NodeSnapshot& next);
static inline void help_ext_min_p(Node* head, Node* tail);

// ---- store_prev_direct ----
// Unconditional atomic store to node->prev_.  Wraps the raw Node* in a fresh
// PrevWrapper for type compatibility with the Node struct; does not attempt
// compare-and-swap and provides no ABA protection (intentionally).
static inline void store_prev_direct(Node* x, Node* val) noexcept {
    PrevWrapper* pw = new PrevWrapper(val);
    x->prev_.store(pw, std::memory_order_release);
    // Old PrevWrapper is leaked — same as in list.h; no reclamation needed for
    // correctness since wrapper addresses are never reused.
}

// ---- post_evict_fix_p ----
// After evicted node has been physically unlinked, fix prev.next.ptr->prev_
// with a plain store if it still points to the evicted node.
static inline void post_evict_fix_p(NodeSnapshot& prev, NodeSnapshot& evicted) {
    Node* next_node = prev.marked_next_.ptr_;
    if (next_node == nullptr) return;
    // Unconditional store: last-writer-wins is acceptable for approximate prev_.
    store_prev_direct(next_node, prev.self_);
}

// ---- evict_deleted_p ----
// Physically unlink the logically deleted node 'current' from between 'prev'
// and 'next'.  No barrier stability checks are performed; the forward CAS is
// attempted directly.  If it fails (concurrent structural change), the caller
// retries via normalize_next_node_p.
static inline bool evict_deleted_p(NodeSnapshot& prev, NodeSnapshot& current,
                                    NodeSnapshot& next) {
    return cas_next(prev.self_, prev.marked_next_, {next.self_, Ok});
}

// ---- help_deleted_next_p ----
static inline bool help_deleted_next_p(NodeSnapshot& current, NodeSnapshot& next) {
    if (next.marked_next_.state_ == Ok) return true;
    NodeSnapshot nn = take_snapshot(next.marked_next_.ptr_);
    if (!evict_deleted_p(current, next, nn)) return false;
    post_evict_fix_p(current, next);
    return true;
}

// ---- normalize_next_node_p ----
static inline bool normalize_next_node_p(NodeSnapshot& current, NodeSnapshot& next) {
    while (is_removed(next.marked_next_.state_)) {
        if (is_removed(current.marked_next_.state_)) return false;
        help_deleted_next_p(current, next);
        if (current.marked_next_.ptr_ != next.self_)
            next = take_snapshot(current.marked_next_.ptr_);
    }
    return current.marked_next_.state_ == Ok;
}

// ---- try_insert_after_p ----
// Insert new_node after current.  Normalizes to ensure the following node is
// alive, then atomically links new_node in.  Repairs next->prev_ with a plain
// store after the successful CAS.
static inline bool try_insert_after_p(NodeSnapshot& current, Node* new_node) {
    if (current.marked_next_.state_ != Ok) return false;

    NodeSnapshot next = take_snapshot(current.marked_next_.ptr_);
    if (!normalize_next_node_p(current, next)) return false;

    new_node->prev_.store(new PrevWrapper(current.self_), std::memory_order_relaxed);
    new_node->next_.store(mp_encode({next.self_, Ok}),    std::memory_order_relaxed);

    if (!cas_next(current.self_, current.marked_next_, {new_node, Ok})) return false;

    // Repair next->prev_ to point to new_node (plain store, no CAS).
    store_prev_direct(next.self_, new_node);
    return true;
}

// ---- try_delete_p ----
static inline bool try_delete_p(NodeSnapshot& prev, NodeSnapshot& current) {
    NodeSnapshot next = take_snapshot(current.marked_next_.ptr_);
    if (!normalize_next_node_p(current, next)) return false;

    if (!cas_next(current.self_, current.marked_next_, {next.self_, Deleted}))
        return false;

    help_deleted_next_p(prev, current);
    return true;
}

// ---- traversal ----

// Walk backward via prev_ to find a living node with key < target.
// Bounded to 64 steps; falls back to head if none found.
// Identical to list.h's find_living_backward — no barriers involved.
static inline Node* find_living_backward_p(Node* hint, long key, Node* head) {
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

// Active traversal: finds (pred, curr) with pred.key_ < key <= curr.key_, both Ok.
static inline void list_find_p(Node* head, Node* hint, long key,
                               NodeSnapshot* pred_out, NodeSnapshot* curr_out) {
    Node* start = find_living_backward_p(hint, key, head);

    while (true) {
        NodeSnapshot pred = take_snapshot(start);

        while (true) {
            if (is_removed(pred.marked_next_.state_)) {
                start = find_living_backward_p(pred.self_, key, head);
                break;
            }

            NodeSnapshot next = take_snapshot(pred.marked_next_.ptr_);
            if (!normalize_next_node_p(pred, next)) {
                start = find_living_backward_p(pred.self_, key, head);
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
static inline bool list_insert_p(Node* head, Node* tail, Node* hint, long key) {
    Node* new_node = new Node(key);
    Node* start = hint;

    while (true) {
        NodeSnapshot pred, curr;
        list_find_p(head, start, key, &pred, &curr);

        if (curr.self_->key_ == key) {
            delete new_node;
            return false;
        }

        // Head-insert gate: help complete any active EXT_MIN epoch first.
        if (pred.self_ == head && read_next(head).state_ == ExtMin) {
            help_ext_min_p(head, tail);
            start = head;
            continue;
        }

        if (try_insert_after_p(pred, new_node)) return true;

        start = pred.self_;
    }
}

// Remove key. Returns true if this call performed the logical deletion.
static inline bool list_erase_p(Node* head, Node* hint, long key) {
    Node* start = hint;

    while (true) {
        NodeSnapshot pred, curr;
        list_find_p(head, start, key, &pred, &curr);

        if (curr.self_->key_ != key) return false;

        if (try_delete_p(pred, curr)) return true;

        start = pred.self_;
    }
}

// Return the minimum key, or NO_KEY if empty.
// Identical to list_min — no barriers needed for a forward scan.
static inline long list_min_p(Node* head, Node* tail) {
    Node* cur = read_next(head).ptr_;
    while (cur != nullptr && cur != tail) {
        MarkedPtr nw = read_next(cur);
        if (nw.state_ == Ok) return cur->key_;
        cur = nw.ptr_;
    }
    return NO_KEY;
}

// Return the maximum key, or NO_KEY if empty.
// Uses tail->prev_ as a backward hint, then scans forward to tail.
static inline long list_max_p(Node* head, Node* tail) {
    Node* start = unwrap(read_prev(tail));
    if (start == nullptr) start = head;

    for (int i = 0; i < 64 && start != head; ++i) {
        if (read_next(start).state_ == Ok) break;
        Node* pp = unwrap(read_prev(start));
        if (pp == nullptr || pp == start) { start = head; break; }
        start = pp;
    }

    long  last_key = NO_KEY;
    Node* cur      = (start == head) ? read_next(head).ptr_ : start;
    while (cur != nullptr && cur != tail) {
        MarkedPtr nw = read_next(cur);
        if (nw.state_ == Ok) last_key = cur->key_;
        cur = nw.ptr_;
    }
    return last_key;
}

// ---- helpExtMin_p ----
// Pragmatic variant: no barrier calls in the physical-removal step.
// The head-advance CAS either wins (epoch closed) or another thread already
// closed it; either way the loop re-reads from the top.
static inline void help_ext_min_p(Node* head, Node* tail) {
    while (true) {
        MarkedPtr hmp = read_next(head);
        if (hmp.state_ != ExtMin) return;

        Node* first = hmp.ptr_;

        // Victim scan: skip Deleted/ExtMax; stop at Ok or ExtMin.
        Node* victim = first;
        while (victim != tail) {
            MarkedPtr vmp = read_next(victim);
            if (vmp.state_ == Ok || vmp.state_ == ExtMin) break;
            victim = vmp.ptr_;
        }

        if (victim == tail) {
            MarkedPtr exp = hmp;
            cas_next(head, exp, {tail, Ok});
            continue;
        }

        MarkedPtr vmp = read_next(victim);

        if (vmp.state_ == ExtMin) {
            // Victim already stamped — advance head past victim (no barriers).
            Node* succ = read_next(victim).ptr_;
            MarkedPtr exp = read_next(head);
            if (exp.state_ == ExtMin && exp.ptr_ == first) {
                if (cas_next(head, exp, {succ, Ok}) && succ != nullptr)
                    store_prev_direct(succ, head);
            }
            continue;
        }

        if (vmp.state_ == Ok) {
            MarkedPtr exp     = vmp;
            MarkedPtr desired = {vmp.ptr_, ExtMin};
            cas_next(victim, exp, desired);
            continue;
        }
        // Deleted/ExtMax: victim concurrently removed; restart scan.
    }
}

// ---- extractMin_p ----
// Atomically removes and returns the minimum key.
// Identical epoch design to list_extract_min; only physical removal is simpler.
static inline long list_extract_min_p(Node* head, Node* tail) {
    while (true) {
        MarkedPtr hmp = read_next(head);

        if (hmp.state_ == ExtMin) {
            help_ext_min_p(head, tail);
            continue;
        }

        Node* first  = hmp.ptr_;
        MarkedPtr exp = hmp;
        if (!cas_next(head, exp, {first, ExtMin}))
            continue;

        Node* victim = first;
        while (victim != tail) {
            MarkedPtr vmp = read_next(victim);
            if (vmp.state_ == Ok || vmp.state_ == ExtMin) break;
            victim = vmp.ptr_;
        }

        if (victim == tail) {
            help_ext_min_p(head, tail);
            return NO_KEY;
        }

        MarkedPtr vmp = read_next(victim);

        if (vmp.state_ == ExtMin) {
            help_ext_min_p(head, tail);
            return victim->key_;
        }

        if (vmp.state_ == Ok) {
            while (vmp.state_ == Ok) {
                MarkedPtr vexp = vmp;
                if (cas_next(victim, vexp, {vmp.ptr_, ExtMin})) {
                    help_ext_min_p(head, tail);
                    return victim->key_;
                }
                vmp = vexp;
            }
            if (vmp.state_ == ExtMin) {
                help_ext_min_p(head, tail);
                return victim->key_;
            }
            help_ext_min_p(head, tail);
            continue;
        }

        help_ext_min_p(head, tail);
    }
}

// ---- extractMax_p ----
// Simplified epoch-free extractMax: the CAS atomically checks
// "victim is still maximum AND alive" (vmp.ptr_ == tail && vmp.state_ == Ok).
// Physical removal uses a forward walk to locate pred, then a plain CAS.
// No barriers; a failed CAS simply causes a retry from find_living_backward_p.
static inline long list_extract_max_p(Node* head, Node* tail) {
    while (true) {
        // Backward hint from tail->prev_, then forward scan to tail.
        Node* start = unwrap(read_prev(tail));
        if (start == nullptr) start = head;

        for (int i = 0; i < 64 && start != head; ++i) {
            if (read_next(start).state_ == Ok) break;
            Node* pp = unwrap(read_prev(start));
            if (pp == nullptr || pp == start) { start = head; break; }
            start = pp;
        }

        Node* victim = nullptr;
        Node* cur    = (start == head) ? read_next(head).ptr_ : start;
        while (cur != nullptr && cur != tail) {
            MarkedPtr nw = read_next(cur);

            if (nw.state_ == ExtMax) {
                // Another extractMax already claimed this node.  Help remove it
                // (best-effort) but do NOT return its key — the owning thread returns it.
                Node* pred = unwrap(read_prev(cur));
                if (pred == nullptr) pred = head;
                for (int i = 0; i < 64; ++i) {
                    MarkedPtr pnw = read_next(pred);
                    if (pnw.ptr_ == cur) break;
                    if (pnw.state_ != Ok || pnw.ptr_ == nullptr ||
                        pnw.ptr_->key_ > cur->key_) { pred = head; break; }
                    pred = pnw.ptr_;
                }
                MarkedPtr pexp = read_next(pred);
                if (pexp.ptr_ == cur && pexp.state_ == Ok) {
                    if (cas_next(pred, pexp, {nw.ptr_, Ok}))
                        store_prev_direct(nw.ptr_, pred);
                }
                // Advance past the ExtMax node (nw.ptr_ == tail for the maximum).
                cur = nw.ptr_;
                continue;
            }

            if (nw.state_ == Ok) victim = cur;
            cur = nw.ptr_;
        }

        if (victim == nullptr) return NO_KEY;

        // Atomically verify victim is still maximum and stamp ExtMax.
        MarkedPtr vmp = read_next(victim);
        if (vmp.ptr_ != tail || vmp.state_ != Ok) continue;

        MarkedPtr vexp = vmp;
        if (!cas_next(victim, vexp, {tail, ExtMax})) continue;

        // Physical removal (no barriers): walk forward from pred hint to victim.
        // We own victim (we hold the ExtMax stamp); detect if already removed and return.
        while (true) {
            // A concurrent extract_min epoch (head->next_ == ExtMin) may have already
            // unlinked victim as a side-effect.  Help it complete so we can detect
            // the removal via !found_victim below, ensuring lock-freedom.
            if (read_next(head).state_ == ExtMin)
                help_ext_min_p(head, tail);

            Node* pred = unwrap(read_prev(victim));
            if (pred == nullptr) pred = head;

            // Walk pred forward to find a LIVE node whose next points to victim.
            // Require pnw.state_ == Ok: pred must not itself be logically deleted.
            bool found_victim = false;
            for (int i = 0; i < 64; ++i) {
                MarkedPtr pnw = read_next(pred);
                if (pnw.ptr_ == victim && pnw.state_ == Ok) { found_victim = true; break; }
                if (pnw.state_ != Ok || pnw.ptr_ == nullptr ||
                    pnw.ptr_->key_ > victim->key_) { pred = head; break; }
                pred = pnw.ptr_;
            }

            // If victim is no longer reachable via a live predecessor, another thread's
            // normalize already unlinked it, or only a deleted predecessor references it.
            // We own the ExtMax stamp, so we return the key exactly once.
            if (!found_victim) return victim->key_;

            MarkedPtr pexp = read_next(pred);
            if (pexp.ptr_ != victim || pexp.state_ != Ok) continue;

            if (cas_next(pred, pexp, {tail, Ok})) {
                store_prev_direct(tail, pred);
                return victim->key_;
            }
        }
    }
}
