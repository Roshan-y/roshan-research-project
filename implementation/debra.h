#pragma once
// Minimal self-contained DEBRA (Dynamically Escalating Barriers with Recorded
// Announcements) for the BST+List data structure.
//
// Based on: Trevor Brown, "Reclaiming Memory for Lock-Free Data Structures:
// There has to be a Better Way", PODC 2015.
//
// Design choices:
//  - 3 epoch bags per thread (sufficient for our 2-thread-epoch guarantee).
//  - Manages Node and PrevWrapper objects only; BST hint nodes are not reclaimed.
//  - Thread IDs are explicit integers (Option A); callers pass tid to every op.
//  - Epoch counter increments by DEBRA_EPOCH_INC (2); low bit is unused here.
//  - Every DEBRA_CHECK_FREQ operations a thread reads all others' epochs to
//    attempt an epoch advance.

#include <atomic>
#include <cstddef>
#include <vector>

// Forward declarations — debra.h is included before node.h in some paths,
// so we forward-declare rather than include.
struct Node;
struct PrevWrapper;

static constexpr int    DEBRA_MAX_THREADS = 128;
static constexpr int    DEBRA_NUM_BAGS    = 3;
static constexpr int    DEBRA_EPOCH_INC   = 2;
static constexpr int    DEBRA_CHECK_FREQ  = 20; // ops between epoch-advance checks

// ---------------------------------------------------------------------------
// EpochBag: a simple growable list of retired pointers tagged with their type.
// ---------------------------------------------------------------------------
struct EpochBag {
    enum class Tag : uint8_t { Node_, PW_ };
    struct Entry { void* ptr; Tag tag; };

    std::vector<Entry> entries;

    void add(Node* n) {
        entries.push_back({static_cast<void*>(n), Tag::Node_});
    }
    void add(PrevWrapper* pw) {
        entries.push_back({static_cast<void*>(pw), Tag::PW_});
    }

    // Free all objects in this bag. When freeing a Node we also free its
    // current PrevWrapper (the live one stored in prev_), since the Node
    // won't be accessed again.
    void drain();  // defined below after Node/PrevWrapper are complete

    bool empty() const { return entries.empty(); }
    void clear() { entries.clear(); }
};

// ---------------------------------------------------------------------------
// Debra: epoch-based reclamation manager.
// ---------------------------------------------------------------------------
class Debra {
public:
    explicit Debra(int num_threads = DEBRA_MAX_THREADS)
        : num_threads_(num_threads)
    {
        global_epoch_.store(0, std::memory_order_relaxed);
        for (int i = 0; i < DEBRA_MAX_THREADS; ++i) {
            thread_epoch_[i].store(0, std::memory_order_relaxed);
            in_cs_[i].store(false, std::memory_order_relaxed);
            bag_idx_[i] = 0;
            op_count_[i] = 0;
        }
    }

    ~Debra() {
        // Drain all remaining bags across all threads.
        for (int t = 0; t < num_threads_; ++t)
            for (int b = 0; b < DEBRA_NUM_BAGS; ++b)
                bags_[t][b].drain();
    }

    // Called at the start of every public DS operation.
    void leaveQuiescentState(int tid) {
        int ge = global_epoch_.load(std::memory_order_acquire);
        thread_epoch_[tid].store(ge, std::memory_order_release);
        in_cs_[tid].store(true, std::memory_order_release);

        // Periodically try to advance the epoch.
        if (++op_count_[tid] >= DEBRA_CHECK_FREQ) {
            op_count_[tid] = 0;
            tryAdvanceEpoch(tid, ge);
        }
    }

    // Called at the end of every public DS operation.
    void enterQuiescentState(int tid) {
        in_cs_[tid].store(false, std::memory_order_release);
    }

    // Allocate a new Node with the given key.
    Node* allocNode(int tid, long key);  // defined after Node is complete

    // Retire a Node (logically removed; will be freed once epoch advances twice).
    void retireNode(int tid, Node* n) {
        bags_[tid][bag_idx_[tid]].add(n);
    }

    // Retire a PrevWrapper (barrier variant only: retired after successful cas_prev).
    void retirePW(int tid, PrevWrapper* pw) {
        if (pw == nullptr) return;
        bags_[tid][bag_idx_[tid]].add(pw);
    }

private:
    int num_threads_;

    alignas(64) std::atomic<int> global_epoch_;

    // Per-thread state — each in its own cache line to avoid false sharing.
    alignas(64) std::atomic<int>  thread_epoch_[DEBRA_MAX_THREADS];
    alignas(64) std::atomic<bool> in_cs_[DEBRA_MAX_THREADS];
    alignas(64) int               bag_idx_[DEBRA_MAX_THREADS];
    alignas(64) int               op_count_[DEBRA_MAX_THREADS];

    EpochBag bags_[DEBRA_MAX_THREADS][DEBRA_NUM_BAGS];

    // Check whether all active threads have announced the current epoch.
    // If so, increment the global epoch and rotate bags (freeing the oldest).
    void tryAdvanceEpoch(int tid, int observed_epoch) {
        for (int t = 0; t < num_threads_; ++t) {
            if (t == tid) continue;
            if (!in_cs_[t].load(std::memory_order_acquire)) continue;
            // Thread t is in a critical section — check its announced epoch.
            if (thread_epoch_[t].load(std::memory_order_acquire) != observed_epoch)
                return;  // not everyone is on this epoch yet
        }

        // All active threads are on observed_epoch. Try to advance.
        int next_epoch = observed_epoch + DEBRA_EPOCH_INC;
        if (!global_epoch_.compare_exchange_strong(observed_epoch, next_epoch,
                std::memory_order_acq_rel, std::memory_order_relaxed))
            return;  // another thread advanced it first

        // Rotate our bag index: free the bag that is 2 epochs old.
        int old_idx  = bag_idx_[tid];
        int free_idx = (old_idx + 2) % DEBRA_NUM_BAGS;
        bags_[tid][free_idx].drain();
        bag_idx_[tid] = (old_idx + 1) % DEBRA_NUM_BAGS;
    }
};

// ---------------------------------------------------------------------------
// Include node.h AFTER the class declaration so that EpochBag::drain() and
// Debra::allocNode() can reference the complete Node type.
// ---------------------------------------------------------------------------
#include "node.h"

// EpochBag::drain — frees each entry; for Node entries also frees the
// currently stored PrevWrapper (which is live in the node's prev_ field).
inline void EpochBag::drain() {
    for (auto& e : entries) {
        if (e.tag == Tag::Node_) {
            Node* n = static_cast<Node*>(e.ptr);
            // Free the PrevWrapper still referenced by this node's prev_ field.
            PrevWrapper* pw = n->prev_.load(std::memory_order_relaxed);
            if (pw) delete pw;
            delete n;
        } else {
            delete static_cast<PrevWrapper*>(e.ptr);
        }
    }
    entries.clear();
}

// Debra::allocNode — plain new; could be a pool in a production implementation.
inline Node* Debra::allocNode(int /*tid*/, long key) {
    return new Node(key);
}

// cas_prev with DEBRA retirement of the old PrevWrapper.
// Defined here (after Debra is complete) because node.h only has a declaration.
inline bool cas_prev(Node* x, CollisionlessPtr& y, Node* z_node,
                     Debra* debra, int tid) noexcept {
    PrevWrapper* pw = new PrevWrapper(z_node);
    PrevWrapper* old_pw = y;  // snapshot the old wrapper before CAS
    if (x->prev_.compare_exchange_strong(y, pw,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
        // Retire old wrapper via DEBRA — another thread may still read it briefly.
        if (old_pw) debra->retirePW(tid, old_pw);
        y = pw;
        return true;
    }
    delete pw;  // never published; safe to delete immediately
    return false;
}
