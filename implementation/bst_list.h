#pragma once
#include "debra.h"
#include "bst_hint.h"
#include "list.h"

// BSTList: a concurrent ordered structure whose ordering is defined by a lock-free
// doubly-linked list and whose search is accelerated by a lock-free BST hint index.
//
// Memory reclamation: DEBRA epoch-based (Trevor Brown, PODC 2015).
// All public methods take an explicit int tid (Option A).

class BSTList {
    Node* head_;
    Node* tail_;
    Debra             debra_;  // destroyed last (declared first)
    bst_hint::HintBST bst_;   // destroyed first → sees debra_ still valid

public:
    explicit BSTList(int num_threads = DEBRA_MAX_THREADS)
        : debra_(num_threads)
    {
        head_ = new Node(KEY_MIN);
        tail_ = new Node(KEY_MAX);

        head_->next_.store(mp_encode({tail_, Ok}), std::memory_order_relaxed);
        head_->prev_.store(nullptr,                std::memory_order_relaxed);

        tail_->next_.store(mp_encode({nullptr, Ok}), std::memory_order_relaxed);
        tail_->prev_.store(new PrevWrapper(head_),   std::memory_order_relaxed);
    }

    ~BSTList() {
        // Walk all physically reachable nodes (live + logically-deleted but not yet
        // physically removed). These were never retired, so DEBRA's ~Debra() won't
        // free them. DEBRA's destructor handles nodes already in epoch bags.
        Node* cur = head_;
        while (cur != nullptr) {
            Node* next = read_next(cur).ptr_;
            CollisionlessPtr pw = cur->prev_.load(std::memory_order_relaxed);
            if (pw) delete pw;
            delete cur;
            if (cur == tail_) break;
            cur = next;
        }
        // ~Debra() runs next and drains epoch bags (retired nodes).
    }

    bool insert(int tid, long key) {
        debra_.leaveQuiescentState(tid);
        Node* hint     = bst_.find_close(key);
        Node* new_node = debra_.allocNode(tid, key);
        bool ok = list_insert(head_, tail_, hint, key, new_node, &debra_, tid);
        if (ok) {
            bst_.try_set(key, new_node);
            // Guard: if a concurrent remove(key) ran its bst_.remove before
            // try_set created the leaf, the hint is now stale. Null it here.
            if (read_next(new_node).state_ != Ok)
                bst_.remove(key, &debra_, tid);
        }
        debra_.enterQuiescentState(tid);
        return ok;
    }

    bool remove(int tid, long key) {
        debra_.leaveQuiescentState(tid);
        Node* hint = bst_.find_close(key);
        bool ok = list_erase(head_, hint, key, &debra_, tid);
        // Null the BST hint AFTER list_erase marks the node Deleted.
        // This closes the race where a concurrent insert's try_set re-populates
        // the hint between bst_.remove and list_erase: by the time try_set runs,
        // the node is already Deleted, so the insert's guard sees Deleted and
        // calls bst_.remove itself.
        bst_.remove(key, &debra_, tid);
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
        long v = list_extract_min(head_, tail_, &debra_, tid);
        if (v != NO_KEY) bst_.remove(v, &debra_, tid);
        debra_.enterQuiescentState(tid);
        return v;
    }

    long extract_max(int tid) {
        debra_.leaveQuiescentState(tid);
        long v = list_extract_max(head_, tail_, &debra_, tid);
        if (v != NO_KEY) bst_.remove(v, &debra_, tid);
        debra_.enterQuiescentState(tid);
        return v;
    }
};
