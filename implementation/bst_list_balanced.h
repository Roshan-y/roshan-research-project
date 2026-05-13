#pragma once
#include "node.h"
#include "bst_balanced_hint.h"
#include "list.h"

// BSTListBalanced: same public API as BSTList but uses a Chromatic (balanced)
// BST hint index instead of the Natarajan-Mittal unbalanced hint index.
//
// The list remains the sole source of correctness.  The balanced hint only
// reduces the average traversal length: because the chromatic tree maintains
// O(log n) height amortized, find_close(key) returns a node O(log n) steps
// from the target — an improvement over the unbalanced BST which degenerates
// to O(n) height under adversarial insertion orders.
//
// Everything else (insert, remove, min, max, extract_min, extract_max)
// is identical to BSTList; only the BST member type changes.

class BSTListBalanced {
    Node* head_;
    Node* tail_;
    bst_balanced_hint::BalancedHintBST bst_;

public:
    BSTListBalanced() {
        head_ = new Node(KEY_MIN);
        tail_ = new Node(KEY_MAX);

        head_->next_.store(mp_encode({tail_, Ok}), std::memory_order_relaxed);
        head_->prev_.store(nullptr,                std::memory_order_relaxed);

        tail_->next_.store(mp_encode({nullptr, Ok}), std::memory_order_relaxed);
        tail_->prev_.store(new PrevWrapper(head_),   std::memory_order_relaxed);
    }

    ~BSTListBalanced() {
        Node* cur = head_;
        while (cur != nullptr) {
            MarkedPtr nw   = read_next(cur);
            Node*     next = nw.ptr_;
            bool is_tail = (cur == tail_);
            CollisionlessPtr cp = read_prev(cur);
            if (cp) delete cp;
            delete cur;
            if (is_tail) break;
            cur = next;
        }
    }

    // Insert key. Returns true if inserted, false if already present.
    bool insert(long key) {
        Node* hint = bst_.find_close(key);
        bool ok = list_insert(head_, tail_, hint, key);
        if (ok) {
            NodeSnapshot pred, curr;
            list_find(head_, hint, key, &pred, &curr);
            if (curr.self_->key_ == key)
                bst_.try_set(key, curr.self_);
        }
        return ok;
    }

    // Remove key. Returns true if this call performed the logical deletion.
    bool remove(long key) {
        Node* hint = bst_.find_close(key);
        return list_erase(head_, hint, key);
        // BST removal intentionally skipped: stale hints are harmless, and
        // removing here risks racing with a concurrent re-insertion.
    }

    // Return the minimum key, or NO_KEY if the set is empty.
    long min() const { return list_min(head_, tail_); }

    // Return the maximum key, or NO_KEY if the set is empty.
    long max() const { return list_max(head_, tail_); }

    // Remove and return the minimum key, or NO_KEY if the set is empty.
    long extract_min() { return list_extract_min(head_, tail_); }

    // Remove and return the maximum key, or NO_KEY if the set is empty.
    long extract_max() { return list_extract_max(head_, tail_); }
};
