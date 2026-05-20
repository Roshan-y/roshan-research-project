#pragma once
#include "debra.h"
#include "bst_balanced_hint.h"
#include "list_pragmatic.h"

// BSTListBalancedPragmatic: balanced BST hint + pragmatic doubly-linked list.
// Memory reclamation: DEBRA epoch-based (Trevor Brown, PODC 2015).

class BSTListBalancedPragmatic {
    Node* head_;
    Node* tail_;
    bst_balanced_hint::BalancedHintBST bst_;
    Debra debra_;

public:
    explicit BSTListBalancedPragmatic(int num_threads = DEBRA_MAX_THREADS)
        : debra_(num_threads)
    {
        head_ = new Node(KEY_MIN);
        tail_ = new Node(KEY_MAX);

        head_->next_.store(mp_encode({tail_, Ok}), std::memory_order_relaxed);
        head_->prev_.store(nullptr,                std::memory_order_relaxed);

        tail_->next_.store(mp_encode({nullptr, Ok}), std::memory_order_relaxed);
        tail_->prev_.store(new PrevWrapper(head_),   std::memory_order_relaxed);
    }

    ~BSTListBalancedPragmatic() {
        Node* cur = head_;
        while (cur != nullptr) {
            Node* next = read_next(cur).ptr_;
            CollisionlessPtr pw = cur->prev_.load(std::memory_order_relaxed);
            if (pw) delete pw;
            delete cur;
            if (cur == tail_) break;
            cur = next;
        }
    }

    bool insert(int tid, long key) {
        debra_.leaveQuiescentState(tid);
        Node* hint     = bst_.find_close(key);
        Node* new_node = debra_.allocNode(tid, key);
        bool ok = list_insert_p(head_, tail_, hint, key, new_node, &debra_, tid);
        if (ok) {
            NodeSnapshot pred, curr;
            list_find_p(head_, hint, key, &pred, &curr, &debra_, tid);
            if (curr.self_->key_ == key)
                bst_.try_set(key, curr.self_);
        }
        debra_.enterQuiescentState(tid);
        return ok;
    }

    bool remove(int tid, long key) {
        debra_.leaveQuiescentState(tid);
        Node* hint = bst_.find_close(key);
        bool ok = list_erase_p(head_, hint, key, &debra_, tid);
        debra_.enterQuiescentState(tid);
        return ok;
    }

    long min() const { return list_min_p(head_, tail_); }
    long max() const { return list_max_p(head_, tail_); }

    long extract_min(int tid) {
        debra_.leaveQuiescentState(tid);
        long v = list_extract_min_p(head_, tail_, &debra_, tid);
        debra_.enterQuiescentState(tid);
        return v;
    }

    long extract_max(int tid) {
        debra_.leaveQuiescentState(tid);
        long v = list_extract_max_p(head_, tail_, &debra_, tid);
        debra_.enterQuiescentState(tid);
        return v;
    }
};
