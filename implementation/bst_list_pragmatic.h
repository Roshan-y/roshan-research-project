#pragma once
#include "node.h"
#include "bst_hint.h"
#include "list_pragmatic.h"

// BSTListPragmatic: ordered set backed by a lock-free doubly-linked list with
// an unbalanced BST hint index.
//
// Identical public API to BSTList but uses the pragmatic prev_-maintenance
// strategy (plain atomic stores, no CAS barriers) from list_pragmatic.h.
// The unbalanced hint BST is the same Natarajan-Mittal tree as in bst_list.h.

class BSTListPragmatic {
    Node* head_;
    Node* tail_;
    bst_hint::HintBST bst_;

public:
    BSTListPragmatic() {
        head_ = new Node(KEY_MIN);
        tail_ = new Node(KEY_MAX);

        head_->next_.store(mp_encode({tail_, Ok}), std::memory_order_relaxed);
        head_->prev_.store(nullptr,                std::memory_order_relaxed);

        tail_->next_.store(mp_encode({nullptr, Ok}), std::memory_order_relaxed);
        tail_->prev_.store(new PrevWrapper(head_),   std::memory_order_relaxed);
    }

    ~BSTListPragmatic() {
        Node* cur = head_;
        while (cur != nullptr) {
            MarkedPtr nw = read_next(cur);
            Node* next   = nw.ptr_;
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
        bool ok = list_insert_p(head_, tail_, hint, key);
        if (ok) {
            NodeSnapshot pred, curr;
            list_find_p(head_, hint, key, &pred, &curr);
            if (curr.self_->key_ == key)
                bst_.try_set(key, curr.self_);
        }
        return ok;
    }

    // Remove key. Returns true if this call performed the logical deletion.
    bool remove(long key) {
        Node* hint = bst_.find_close(key);
        return list_erase_p(head_, hint, key);
    }

    // Return the minimum key, or NO_KEY if the set is empty.
    long min() const { return list_min_p(head_, tail_); }

    // Return the maximum key, or NO_KEY if the set is empty.
    long max() const { return list_max_p(head_, tail_); }

    // Remove and return the minimum key, or NO_KEY if the set is empty.
    long extract_min() { return list_extract_min_p(head_, tail_); }

    // Remove and return the maximum key, or NO_KEY if the set is empty.
    long extract_max() { return list_extract_max_p(head_, tail_); }
};
