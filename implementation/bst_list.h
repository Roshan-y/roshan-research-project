#pragma once
#include "node.h"
#include "bst_hint.h"
#include "list.h"

// BSTList: a concurrent ordered set whose ordering is defined by a lock-free
// doubly-linked list and whose search is accelerated by a lock-free BST hint index.
//
// The list is the sole source of correctness. The BST is an auxiliary hint layer
// that returns a Node* close to the target key so that list traversal starts there
// rather than at head. If the BST hint is stale or wrong, the list operation is
// still correct — it just starts farther from the target.
//
// Follows the integration described in Listing 5 of Melnikov & Sela.

class BSTList {
    Node* head_;
    Node* tail_;
    bst_hint::HintBST bst_;

public:
    BSTList() {
        head_ = new Node(KEY_MIN);
        tail_ = new Node(KEY_MAX);

        // head_->next_ = {tail_, Ok}; head has no predecessor
        head_->next_.store(mp_encode({tail_, Ok}), std::memory_order_relaxed);
        head_->prev_.store(nullptr,                std::memory_order_relaxed);

        // tail_->next_ = {nullptr, Ok}; barrier() treats null successor as trivially ok
        tail_->next_.store(mp_encode({nullptr, Ok}), std::memory_order_relaxed);
        // tail_->prev_ is kept approximate by barrier() calls; initialize to head
        tail_->prev_.store(new PrevWrapper(head_),   std::memory_order_relaxed);
    }

    ~BSTList() {
        // Walk and free reachable list nodes via the next_ chain.
        Node* cur = head_;
        while (cur != nullptr) {
            MarkedPtr nw = read_next(cur);
            Node* next = nw.ptr_;
            // Free the current CollisionlessPtr (best-effort; others are leaked).
            CollisionlessPtr cp = read_prev(cur);
            if (cp) delete cp;
            delete cur;
            if (cur == tail_) break;
            cur = next;
        }
    }

    // Insert key. Returns true if inserted, false if already present.
    bool insert(long key) {
        Node* hint = bst_.find_close(key);
        bool ok = list_insert(head_, tail_, hint, key);
        if (ok) {
            // Locate the newly inserted node so the BST hint stays current.
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
        // Intentionally skip bst_.try_remove(key) to avoid a race where a concurrent
        // re-insertion's BST entry would be silently removed. Stale BST hints are harmless.
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
