#pragma once
#include "debra.h"
#include "bst_hint.h"
#include "list_pragmatic.h"

// BSTListPragmatic: BST hint + pragmatic (plain-store prev_) doubly-linked list.
// Memory reclamation: DEBRA epoch-based (Trevor Brown, PODC 2015).

class BSTListPragmatic {
    Node* head_;
    Node* tail_;
    Debra             debra_;  // destroyed last (declared first)
    bst_hint::HintBST bst_;   // destroyed first → sees debra_ still valid

public:
    explicit BSTListPragmatic(int num_threads = DEBRA_MAX_THREADS)
        : debra_(num_threads)
    {
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
            bst_.try_set(key, new_node);
            if (read_next(new_node).state_ != Ok)
                bst_.remove(key, &debra_, tid);
        }
        debra_.enterQuiescentState(tid);
        return ok;
    }

    bool remove(int tid, long key) {
        debra_.leaveQuiescentState(tid);
        Node* hint = bst_.find_close(key);
        bool ok = list_erase_p(head_, hint, key, &debra_, tid);
        bst_.remove(key, &debra_, tid);  // see bst_list.h for the ordering rationale
        debra_.enterQuiescentState(tid);
        return ok;
    }

    long min() const { return list_min(head_, tail_); }
    long max(int tid) {
        debra_.leaveQuiescentState(tid);
        long v = list_max_backward(head_, tail_);
        debra_.enterQuiescentState(tid);
        return v;
    }

    long extract_min(int tid) {
        debra_.leaveQuiescentState(tid);
        long v = list_extract_min_p(head_, tail_, &debra_, tid);
        if (v != NO_KEY) bst_.remove(v, &debra_, tid);
        debra_.enterQuiescentState(tid);
        return v;
    }

    long extract_max(int tid) {
        debra_.leaveQuiescentState(tid);
        long v = list_extract_max_p(head_, tail_, &debra_, tid);
        if (v != NO_KEY) bst_.remove(v, &debra_, tid);
        debra_.enterQuiescentState(tid);
        return v;
    }
};
