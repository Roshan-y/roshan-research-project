#pragma once
#include "debra.h"  // includes node.h transitively

// Lock-free doubly-linked ordered list following Melnikov & Sela's Hinted Ordered List.
//
// The authoritative structure is the forward next_ chain. prev_ pointers are
// best-effort backward hints kept approximately correct via barriers.
//
// Memory reclamation: DEBRA epoch-based (Trevor Brown, PODC 2015).
// Every function that may allocate, retire, or call functions that do so
// receives an explicit (Debra* debra, int tid) pair (Option A).
//
// Forward declarations (functions are mutually recursive).
static bool normalize_next_node(NodeSnapshot& current, NodeSnapshot& next,
                                Debra* debra, int tid);
static bool help_deleted_next(NodeSnapshot& current, NodeSnapshot& next,
                              Debra* debra, int tid);
static bool evict_deleted(NodeSnapshot& prev, NodeSnapshot& current, NodeSnapshot& next,
                          Debra* debra, int tid);
static inline void help_ext_min(Node* head, Node* tail, Debra* debra, int tid);

// ---- barrier ----
// Ensures current.marked_next_.ptr_->prev_ points to current.self_.
// to_check is a stability anchor: if its snapshot changed since last read, update it and
// return false so the caller can retry with fresh information.
static inline bool barrier(NodeSnapshot& current, NodeSnapshot& to_check,
                           Debra* debra, int tid) {
    Node* next_node = current.marked_next_.ptr_;
    if (next_node == nullptr) return true;  // tail sentinel: no successor to fix

    CollisionlessPtr backptr = read_prev(next_node);
    NodeSnapshot check = take_snapshot(to_check.self_);
    if (to_check != check) {
        to_check = check;
        return false;
    }
    if (unwrap(backptr) == current.self_) return true;
    cas_prev(next_node, backptr, current.self_, debra, tid);
    return unwrap(backptr) == current.self_;
}

static inline bool barrier(NodeSnapshot& current, Debra* debra, int tid) {
    return barrier(current, current, debra, tid);
}

// After evicting a deleted node, fix the back-pointer of the new successor.
static inline void post_evict_fix(NodeSnapshot& prev, NodeSnapshot& evicted,
                                  Debra* debra, int tid) {
    Node* next_node = prev.marked_next_.ptr_;
    if (next_node == nullptr) return;
    CollisionlessPtr backptr = read_prev(next_node);
    if (unwrap(backptr) == evicted.self_)
        cas_prev(next_node, backptr, prev.self_, debra, tid);
}

// Physically unlink the deleted node 'current' from between 'prev' and 'next'.
// On success, retires current.self_ via DEBRA.
static inline bool evict_deleted(NodeSnapshot& prev, NodeSnapshot& current, NodeSnapshot& next,
                                 Debra* debra, int tid) {
    if (!barrier(prev, debra, tid)) return false;
    if (!barrier(current, prev, debra, tid)) return false;
    NodeSnapshot& to_check = (next.marked_next_.state_ == Ok) ? next : prev;
    if (!barrier(next, to_check, debra, tid)) return false;
    if (cas_next(prev.self_, prev.marked_next_, {next.self_, Ok})) {
        debra->retireNode(tid, current.self_);
        return true;
    }
    return false;
}

// If 'next' is logically removed, evict it and repair the successor's back-pointer.
static inline bool help_deleted_next(NodeSnapshot& current, NodeSnapshot& next,
                                     Debra* debra, int tid) {
    if (next.marked_next_.state_ == Ok) return true;
    NodeSnapshot nn = take_snapshot(next.marked_next_.ptr_);
    if (!evict_deleted(current, next, nn, debra, tid)) return false;
    post_evict_fix(current, next, debra, tid);
    return true;
}

// Ensures that 'current' is followed by a non-deleted node.
static inline bool normalize_next_node(NodeSnapshot& current, NodeSnapshot& next,
                                       Debra* debra, int tid) {
    while (is_removed(next.marked_next_.state_)) {
        if (is_removed(current.marked_next_.state_)) return false;
        help_deleted_next(current, next, debra, tid);
        if (current.marked_next_.ptr_ != next.self_)
            next = take_snapshot(current.marked_next_.ptr_);
    }
    return current.marked_next_.state_ == Ok;
}

// Insert new_node after current.
static inline bool try_insert_after(NodeSnapshot& current, Node* new_node,
                                    Debra* debra, int tid) {
    if (current.marked_next_.state_ != Ok) return false;

    NodeSnapshot next = take_snapshot(current.marked_next_.ptr_);
    if (!normalize_next_node(current, next, debra, tid)) return false;
    if (!barrier(current, debra, tid)) return false;
    if (!barrier(next, debra, tid))    return false;

    new_node->prev_.store(new PrevWrapper(current.self_), std::memory_order_relaxed);
    new_node->next_.store(mp_encode({next.self_, Ok}),    std::memory_order_relaxed);

    if (!cas_next(current.self_, current.marked_next_, {new_node, Ok})) return false;

    NodeSnapshot inserted = take_snapshot(new_node);
    barrier(inserted, debra, tid);  // repair next->prev_ to point to new_node
    return true;
}

// Logically delete 'current' (preceded by 'prev') by marking its next_.
static inline bool try_delete(NodeSnapshot& prev, NodeSnapshot& current,
                              Debra* debra, int tid) {
    NodeSnapshot next = take_snapshot(current.marked_next_.ptr_);
    if (!normalize_next_node(current, next, debra, tid)) return false;

    if (!cas_next(current.self_, current.marked_next_, {next.self_, Deleted}))
        return false;

    help_deleted_next(prev, current, debra, tid);
    return true;
}

// ---- traversal ----

// DEBRA safety note: Following prev_ pointers is unsafe when memory reclamation
// is active because prev_ can point to already-freed nodes (stale after physical
// removal). BST hints can also be freed nodes. We conservatively return head to
// ensure no freed memory is accessed. All traversals start from head.
static inline Node* find_living_backward(Node* /*hint*/, long /*key*/, Node* head) {
    return head;
}

static inline void list_find(Node* head, Node* hint, long key,
                             NodeSnapshot* pred_out, NodeSnapshot* curr_out,
                             Debra* debra, int tid) {
    Node* start = find_living_backward(hint, key, head);

    while (true) {
        NodeSnapshot pred = take_snapshot(start);

        while (true) {
            if (is_removed(pred.marked_next_.state_)) {
                start = find_living_backward(pred.self_, key, head);
                break;
            }

            NodeSnapshot next = take_snapshot(pred.marked_next_.ptr_);
            if (!normalize_next_node(pred, next, debra, tid)) {
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

// Insert new_node (pre-allocated by caller via debra.allocNode) at key.
// Returns true if inserted, false if already present (new_node is deleted).
static inline bool list_insert(Node* head, Node* tail, Node* hint, long key,
                               Node* new_node, Debra* debra, int tid) {
    Node* start = hint;

    while (true) {
        NodeSnapshot pred, curr;
        list_find(head, start, key, &pred, &curr, debra, tid);

        if (curr.self_->key_ == key) {
            delete new_node;  // never published; safe to delete immediately
            return false;
        }

        if (pred.self_ == head && read_next(head).state_ == ExtMin) {
            help_ext_min(head, tail, debra, tid);
            start = head;
            continue;
        }

        if (try_insert_after(pred, new_node, debra, tid)) return true;

        start = pred.self_;
    }
}

static inline bool list_erase(Node* head, Node* hint, long key,
                              Debra* debra, int tid) {
    Node* start = hint;

    while (true) {
        NodeSnapshot pred, curr;
        list_find(head, start, key, &pred, &curr, debra, tid);

        if (curr.self_->key_ != key) return false;

        if (try_delete(pred, curr, debra, tid)) return true;

        start = pred.self_;
    }
}

// min/max do not allocate or retire — no DEBRA params needed.
static inline long list_min(Node* head, Node* tail) {
    Node* cur = read_next(head).ptr_;
    while (cur != nullptr && cur != tail) {
        MarkedPtr nw = read_next(cur);
        if (nw.state_ == Ok) return cur->key_;
        cur = nw.ptr_;
    }
    return NO_KEY;
}

static inline long list_max(Node* head, Node* tail) {
    // Backward hint via prev_ is unsafe with DEBRA (freed nodes reachable).
    // Forward scan from head; track last Ok node.
    long  last_key = NO_KEY;
    Node* cur      = read_next(head).ptr_;
    while (cur != nullptr && cur != tail) {
        MarkedPtr nw = read_next(cur);
        if (nw.state_ == Ok) last_key = cur->key_;
        cur = nw.ptr_;
    }
    return last_key;
}

// ---- helpExtMin ----
// Retires the victim node via DEBRA after the winning head-advance CAS.
static inline void help_ext_min(Node* head, Node* tail, Debra* debra, int tid) {
    while (true) {
        MarkedPtr hmp = read_next(head);
        if (hmp.state_ != ExtMin) return;

        Node* first = hmp.ptr_;

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
            MarkedPtr fresh_vmp = read_next(victim);
            Node* succ = fresh_vmp.ptr_;

            NodeSnapshot snap_head   = take_snapshot(head);
            NodeSnapshot snap_victim = take_snapshot(victim);
            NodeSnapshot snap_vsucc  = succ ? take_snapshot(succ) : snap_victim;
            barrier(snap_head, debra, tid);
            barrier(snap_victim, snap_head, debra, tid);
            if (succ) barrier(snap_vsucc, snap_victim, debra, tid);

            MarkedPtr exp = read_next(head);
            if (exp.state_ == ExtMin && exp.ptr_ == first) {
                if (cas_next(head, exp, {succ, Ok})) {
                    // We advanced head past victim — retire victim.
                    debra->retireNode(tid, victim);
                }
            }
            post_evict_fix(snap_head, snap_victim, debra, tid);
            continue;
        }

        if (vmp.state_ == Ok) {
            MarkedPtr exp     = vmp;
            MarkedPtr desired = {vmp.ptr_, ExtMin};
            cas_next(victim, exp, desired);
            continue;
        }
    }
}

// ---- extractMin ----
static inline long list_extract_min(Node* head, Node* tail, Debra* debra, int tid) {
    while (true) {
        MarkedPtr hmp = read_next(head);

        if (hmp.state_ == ExtMin) {
            help_ext_min(head, tail, debra, tid);
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
            help_ext_min(head, tail, debra, tid);
            return NO_KEY;
        }

        MarkedPtr vmp = read_next(victim);

        if (vmp.state_ == ExtMin) {
            help_ext_min(head, tail, debra, tid);
            return victim->key_;
        }

        if (vmp.state_ == Ok) {
            while (vmp.state_ == Ok) {
                MarkedPtr vexp = vmp;
                if (cas_next(victim, vexp, {vmp.ptr_, ExtMin})) {
                    help_ext_min(head, tail, debra, tid);
                    return victim->key_;
                }
                vmp = vexp;
            }
            if (vmp.state_ == ExtMin) {
                help_ext_min(head, tail, debra, tid);
                return victim->key_;
            }
            help_ext_min(head, tail, debra, tid);
            continue;
        }

        help_ext_min(head, tail, debra, tid);
    }
}

// ---- extractMax ----
// Retires the victim via DEBRA after the winning physical-removal CAS.
// Note: backward traversal via prev_ is disabled (DEBRA safety); we use
// head as the starting point and a forward scan to locate pred.
static inline long list_extract_max(Node* head, Node* tail, Debra* debra, int tid) {
    while (true) {
        // Forward scan from head to find last Ok node (victim candidate).
        Node* victim = nullptr;
        Node* cur    = read_next(head).ptr_;
        while (cur != nullptr && cur != tail) {
            MarkedPtr nw = read_next(cur);

            if (nw.state_ == ExtMax) {
                // Help physically remove an already-claimed ExtMax node.
                // Walk forward from head to find its live predecessor.
                Node* pred = head;
                for (int i = 0; i < 64; ++i) {
                    MarkedPtr pnw = read_next(pred);
                    if (pnw.ptr_ == cur) break;
                    if (pnw.state_ != Ok || pnw.ptr_ == nullptr ||
                        pnw.ptr_->key_ > cur->key_) { pred = head; break; }
                    pred = pnw.ptr_;
                }
                NodeSnapshot snap_pred = take_snapshot(pred);
                NodeSnapshot snap_cur  = take_snapshot(cur);
                NodeSnapshot snap_succ = take_snapshot(nw.ptr_);
                if (barrier(snap_pred, debra, tid) &&
                    barrier(snap_cur,  snap_pred, debra, tid) &&
                    barrier(snap_succ, snap_pred, debra, tid)) {
                    MarkedPtr pexp = snap_pred.marked_next_;
                    if (pexp.ptr_ == cur && pexp.state_ == Ok)
                        cas_next(pred, pexp, {nw.ptr_, Ok});
                    post_evict_fix(snap_pred, snap_cur, debra, tid);
                }
                cur = nw.ptr_;
                continue;
            }

            if (nw.state_ == Ok) victim = cur;
            cur = nw.ptr_;
        }

        if (victim == nullptr) return NO_KEY;

        MarkedPtr vmp = read_next(victim);
        if (vmp.ptr_ != tail || vmp.state_ != Ok)
            continue;

        MarkedPtr vexp = vmp;
        if (!cas_next(victim, vexp, {tail, ExtMax}))
            continue;

        // We own victim. Physically remove it via forward walk from head.
        while (true) {
            if (read_next(head).state_ == ExtMin)
                help_ext_min(head, tail, debra, tid);

            // Walk forward from head to find live predecessor of victim.
            Node* pred = head;
            bool found_victim = false;
            for (int i = 0; i < 64; ++i) {
                MarkedPtr pnw = read_next(pred);
                if (pnw.ptr_ == victim && pnw.state_ == Ok) { found_victim = true; break; }
                if (pnw.state_ != Ok || pnw.ptr_ == nullptr ||
                    pnw.ptr_->key_ > victim->key_) {
                    pred = head; break;
                }
                pred = pnw.ptr_;
            }

            if (!found_victim) return victim->key_;

            NodeSnapshot snap_pred   = take_snapshot(pred);
            NodeSnapshot snap_victim = take_snapshot(victim);
            NodeSnapshot snap_tail   = take_snapshot(tail);

            if (!barrier(snap_pred, debra, tid))              continue;
            if (!barrier(snap_victim, snap_pred, debra, tid)) continue;
            barrier(snap_tail, snap_victim, debra, tid);

            MarkedPtr pexp = snap_pred.marked_next_;
            if (pexp.ptr_ != victim || pexp.state_ != Ok) continue;

            if (cas_next(pred, pexp, {tail, Ok})) {
                post_evict_fix(snap_pred, snap_victim, debra, tid);
                debra->retireNode(tid, victim);
                return victim->key_;
            }
        }
    }
}
