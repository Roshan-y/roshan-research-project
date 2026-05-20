#pragma once
#include "debra.h"  // includes node.h transitively

// Pragmatic lock-free doubly-linked ordered list.
//
// This is an alternative to list.h that replaces CAS-based barrier() calls
// with plain atomic stores for prev_ maintenance.  The approach is inspired
// by Träff & Pöter, "A more Pragmatic Implementation of the Lock-free,
// Ordered, Linked List" (arXiv:2010.15755).
//
// Memory reclamation: DEBRA epoch-based (Trevor Brown, PODC 2015).
// NOTE: PrevWrapper objects are NOT retired separately in this variant.
//   Because prev_ is updated with plain stores (store_prev_direct), any two
//   concurrent threads can both read the same old PrevWrapper and both attempt
//   to retire it — causing a double-free.  Instead, the live PrevWrapper for a
//   Node is freed inside EpochBag::drain() when the Node itself is reclaimed.
//   The failed-CAS-new PrevWrapper is deleted immediately (never published).
//
// All functions carry a _p suffix to coexist with list.h in one compilation unit.

// Forward declarations (functions are mutually recursive).
static bool normalize_next_node_p(NodeSnapshot& current, NodeSnapshot& next,
                                  Debra* debra, int tid);
static bool help_deleted_next_p(NodeSnapshot& current, NodeSnapshot& next,
                                Debra* debra, int tid);
static bool evict_deleted_p(NodeSnapshot& prev, NodeSnapshot& current, NodeSnapshot& next,
                            Debra* debra, int tid);
static inline void help_ext_min_p(Node* head, Node* tail, Debra* debra, int tid);

// ---- store_prev_direct ----
// Unconditional atomic store to node->prev_. Wraps the Node* in a fresh
// PrevWrapper. The old PrevWrapper is freed when its Node is reclaimed by DEBRA.
static inline void store_prev_direct(Node* x, Node* val) noexcept {
    PrevWrapper* pw = new PrevWrapper(val);
    PrevWrapper* old = x->prev_.exchange(pw, std::memory_order_release);
    // Do NOT retire old via DEBRA here — another thread may still be reading it.
    // It will be freed by EpochBag::drain() when the node x is reclaimed.
    // If x is not retired (e.g. it's a sentinel), old leaks — acceptable for
    // sentinel nodes whose PrevWrapper is freed in the DS destructor.
    (void)old;
}

// ---- post_evict_fix_p ----
static inline void post_evict_fix_p(NodeSnapshot& prev, NodeSnapshot& evicted) {
    Node* next_node = prev.marked_next_.ptr_;
    if (next_node == nullptr) return;
    store_prev_direct(next_node, prev.self_);
}

// ---- evict_deleted_p ----
// Physically unlink current. On success, retires current.self_ via DEBRA.
static inline bool evict_deleted_p(NodeSnapshot& prev, NodeSnapshot& current,
                                   NodeSnapshot& next, Debra* debra, int tid) {
    if (cas_next(prev.self_, prev.marked_next_, {next.self_, Ok})) {
        debra->retireNode(tid, current.self_);
        return true;
    }
    return false;
}

// ---- help_deleted_next_p ----
static inline bool help_deleted_next_p(NodeSnapshot& current, NodeSnapshot& next,
                                       Debra* debra, int tid) {
    if (next.marked_next_.state_ == Ok) return true;
    NodeSnapshot nn = take_snapshot(next.marked_next_.ptr_);
    if (!evict_deleted_p(current, next, nn, debra, tid)) return false;
    post_evict_fix_p(current, next);
    return true;
}

// ---- normalize_next_node_p ----
static inline bool normalize_next_node_p(NodeSnapshot& current, NodeSnapshot& next,
                                         Debra* debra, int tid) {
    while (is_removed(next.marked_next_.state_)) {
        if (is_removed(current.marked_next_.state_)) return false;
        help_deleted_next_p(current, next, debra, tid);
        if (current.marked_next_.ptr_ != next.self_)
            next = take_snapshot(current.marked_next_.ptr_);
    }
    return current.marked_next_.state_ == Ok;
}

// ---- try_insert_after_p ----
static inline bool try_insert_after_p(NodeSnapshot& current, Node* new_node,
                                      Debra* debra, int tid) {
    if (current.marked_next_.state_ != Ok) return false;

    NodeSnapshot next = take_snapshot(current.marked_next_.ptr_);
    if (!normalize_next_node_p(current, next, debra, tid)) return false;

    new_node->prev_.store(new PrevWrapper(current.self_), std::memory_order_relaxed);
    new_node->next_.store(mp_encode({next.self_, Ok}),    std::memory_order_relaxed);

    if (!cas_next(current.self_, current.marked_next_, {new_node, Ok})) return false;

    store_prev_direct(next.self_, new_node);
    return true;
}

// ---- try_delete_p ----
static inline bool try_delete_p(NodeSnapshot& prev, NodeSnapshot& current,
                                 Debra* debra, int tid) {
    NodeSnapshot next = take_snapshot(current.marked_next_.ptr_);
    if (!normalize_next_node_p(current, next, debra, tid)) return false;

    if (!cas_next(current.self_, current.marked_next_, {next.self_, Deleted}))
        return false;

    help_deleted_next_p(prev, current, debra, tid);
    return true;
}

// ---- traversal ----

// DEBRA safety note: backward traversal via prev_ can access freed nodes.
// Conservatively return head; all traversals start from head.
static inline Node* find_living_backward_p(Node* /*hint*/, long /*key*/, Node* head) {
    return head;
}

static inline void list_find_p(Node* head, Node* hint, long key,
                               NodeSnapshot* pred_out, NodeSnapshot* curr_out,
                               Debra* debra, int tid) {
    Node* start = find_living_backward_p(hint, key, head);

    while (true) {
        NodeSnapshot pred = take_snapshot(start);

        while (true) {
            if (is_removed(pred.marked_next_.state_)) {
                start = find_living_backward_p(pred.self_, key, head);
                break;
            }

            NodeSnapshot next = take_snapshot(pred.marked_next_.ptr_);
            if (!normalize_next_node_p(pred, next, debra, tid)) {
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

// Insert new_node (pre-allocated by caller via debra.allocNode) at key.
static inline bool list_insert_p(Node* head, Node* tail, Node* hint, long key,
                                 Node* new_node, Debra* debra, int tid) {
    Node* start = hint;

    while (true) {
        NodeSnapshot pred, curr;
        list_find_p(head, start, key, &pred, &curr, debra, tid);

        if (curr.self_->key_ == key) {
            delete new_node;
            return false;
        }

        if (pred.self_ == head && read_next(head).state_ == ExtMin) {
            help_ext_min_p(head, tail, debra, tid);
            start = head;
            continue;
        }

        if (try_insert_after_p(pred, new_node, debra, tid)) return true;

        start = pred.self_;
    }
}

static inline bool list_erase_p(Node* head, Node* hint, long key,
                                Debra* debra, int tid) {
    Node* start = hint;

    while (true) {
        NodeSnapshot pred, curr;
        list_find_p(head, start, key, &pred, &curr, debra, tid);

        if (curr.self_->key_ != key) return false;

        if (try_delete_p(pred, curr, debra, tid)) return true;

        start = pred.self_;
    }
}

static inline long list_min_p(Node* head, Node* tail) {
    Node* cur = read_next(head).ptr_;
    while (cur != nullptr && cur != tail) {
        MarkedPtr nw = read_next(cur);
        if (nw.state_ == Ok) return cur->key_;
        cur = nw.ptr_;
    }
    return NO_KEY;
}

static inline long list_max_p(Node* head, Node* tail) {
    // Backward hint via prev_ is unsafe with DEBRA. Forward scan from head.
    long  last_key = NO_KEY;
    Node* cur      = read_next(head).ptr_;
    while (cur != nullptr && cur != tail) {
        MarkedPtr nw = read_next(cur);
        if (nw.state_ == Ok) last_key = cur->key_;
        cur = nw.ptr_;
    }
    return last_key;
}

// ---- helpExtMin_p ----
// Retires the victim via DEBRA after winning head-advance CAS.
static inline void help_ext_min_p(Node* head, Node* tail, Debra* debra, int tid) {
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
            Node* succ = read_next(victim).ptr_;
            MarkedPtr exp = read_next(head);
            if (exp.state_ == ExtMin && exp.ptr_ == first) {
                if (cas_next(head, exp, {succ, Ok})) {
                    debra->retireNode(tid, victim);
                    if (succ != nullptr)
                        store_prev_direct(succ, head);
                }
            }
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

// ---- extractMin_p ----
static inline long list_extract_min_p(Node* head, Node* tail, Debra* debra, int tid) {
    while (true) {
        MarkedPtr hmp = read_next(head);

        if (hmp.state_ == ExtMin) {
            help_ext_min_p(head, tail, debra, tid);
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
            help_ext_min_p(head, tail, debra, tid);
            return NO_KEY;
        }

        MarkedPtr vmp = read_next(victim);

        if (vmp.state_ == ExtMin) {
            help_ext_min_p(head, tail, debra, tid);
            return victim->key_;
        }

        if (vmp.state_ == Ok) {
            while (vmp.state_ == Ok) {
                MarkedPtr vexp = vmp;
                if (cas_next(victim, vexp, {vmp.ptr_, ExtMin})) {
                    help_ext_min_p(head, tail, debra, tid);
                    return victim->key_;
                }
                vmp = vexp;
            }
            if (vmp.state_ == ExtMin) {
                help_ext_min_p(head, tail, debra, tid);
                return victim->key_;
            }
            help_ext_min_p(head, tail, debra, tid);
            continue;
        }

        help_ext_min_p(head, tail, debra, tid);
    }
}

// ---- extractMax_p ----
// Retires the victim via DEBRA after the winning physical-removal CAS.
// Backward traversal via prev_ is disabled (DEBRA safety). Forward scan from head.
static inline long list_extract_max_p(Node* head, Node* tail, Debra* debra, int tid) {
    while (true) {
        // Forward scan from head to find last Ok node (victim candidate).
        Node* victim = nullptr;
        Node* cur    = read_next(head).ptr_;
        while (cur != nullptr && cur != tail) {
            MarkedPtr nw = read_next(cur);

            if (nw.state_ == ExtMax) {
                // Help physically remove already-claimed ExtMax node.
                Node* pred = head;
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
                cur = nw.ptr_;
                continue;
            }

            if (nw.state_ == Ok) victim = cur;
            cur = nw.ptr_;
        }

        if (victim == nullptr) return NO_KEY;

        MarkedPtr vmp = read_next(victim);
        if (vmp.ptr_ != tail || vmp.state_ != Ok) continue;

        MarkedPtr vexp = vmp;
        if (!cas_next(victim, vexp, {tail, ExtMax})) continue;

        // We own victim. Physically remove it via forward walk from head.
        while (true) {
            if (read_next(head).state_ == ExtMin)
                help_ext_min_p(head, tail, debra, tid);

            Node* pred = head;
            bool found_victim = false;
            for (int i = 0; i < 64; ++i) {
                MarkedPtr pnw = read_next(pred);
                if (pnw.ptr_ == victim && pnw.state_ == Ok) { found_victim = true; break; }
                if (pnw.state_ != Ok || pnw.ptr_ == nullptr ||
                    pnw.ptr_->key_ > victim->key_) { pred = head; break; }
                pred = pnw.ptr_;
            }

            if (!found_victim) return victim->key_;

            MarkedPtr pexp = read_next(pred);
            if (pexp.ptr_ != victim || pexp.state_ != Ok) continue;

            if (cas_next(pred, pexp, {tail, Ok})) {
                store_prev_direct(tail, pred);
                debra->retireNode(tid, victim);
                return victim->key_;
            }
        }
    }
}
