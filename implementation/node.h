#pragma once
#include <atomic>
#include <climits>
#include <cstdint>

static constexpr long KEY_MIN = LONG_MIN;
static constexpr long KEY_MAX = LONG_MAX;
static constexpr long NO_KEY  = LONG_MIN;

// State occupies the low 2 bits of a tagged pointer.
// ExtMin / ExtMax mean the node has been claimed by an extract operation
// and is logically removed from the list.
enum State : uint8_t { Ok = 0, Deleted = 1, ExtMin = 2, ExtMax = 3 };

// Convenience: any state other than Ok means the node is logically gone.
inline bool is_removed(State s) noexcept { return s != Ok; }

struct Node;

// Indirect pointer for prev_. Every write allocates a new PrevWrapper so that two
// successive writes of the same Node* produce different addresses. A CAS on
// CollisionlessPtr therefore fails if the value was replaced and restored (ABA).
// Old wrappers are never freed — safe without reclamation since addresses are never reused.
struct PrevWrapper {
    Node* node;
    explicit PrevWrapper(Node* n) noexcept : node(n) {}
};
using CollisionlessPtr = PrevWrapper*;

inline Node* unwrap(CollisionlessPtr cp) noexcept { return cp ? cp->node : nullptr; }

// MarkedPtr encodes a Node* and a State in one pointer-sized word (low 2 bits = state).
struct MarkedPtr {
    Node*  ptr_;
    State  state_;
    bool operator==(const MarkedPtr& o) const noexcept {
        return ptr_ == o.ptr_ && state_ == o.state_;
    }
    bool operator!=(const MarkedPtr& o) const noexcept { return !(*this == o); }
};

// 2-bit encoding: bits 1:0 = state, remaining bits = pointer.
// Nodes are alignas(64) so bits 0..5 are always zero in the raw pointer.
inline uintptr_t mp_encode(MarkedPtr mp) noexcept {
    return reinterpret_cast<uintptr_t>(mp.ptr_) | static_cast<uintptr_t>(mp.state_);
}
inline MarkedPtr mp_decode(uintptr_t w) noexcept {
    return { reinterpret_cast<Node*>(w & ~3UL), static_cast<State>(w & 3UL) };
}

// NodeSnapshot: a point-in-time view of a node and its next_ field.
struct NodeSnapshot {
    Node*     self_;
    MarkedPtr marked_next_;
    bool operator==(const NodeSnapshot& o) const noexcept {
        return self_ == o.self_ && marked_next_ == o.marked_next_;
    }
    bool operator!=(const NodeSnapshot& o) const noexcept { return !(*this == o); }
};

struct alignas(64) Node {
    std::atomic<CollisionlessPtr> prev_;
    std::atomic<uintptr_t>        next_;  // encoded MarkedPtr
    long key_;

    explicit Node(long key) noexcept : key_(key) {
        prev_.store(nullptr,              std::memory_order_relaxed);
        next_.store(mp_encode({nullptr, Ok}), std::memory_order_relaxed);
    }
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
};

inline MarkedPtr       read_next(const Node* n) noexcept {
    return mp_decode(n->next_.load(std::memory_order_acquire));
}
inline CollisionlessPtr read_prev(const Node* n) noexcept {
    return n->prev_.load(std::memory_order_acquire);
}
inline NodeSnapshot    take_snapshot(Node* n) noexcept {
    return { n, read_next(n) };
}

// CAS on next_. Matches the report's CAS semantics: on success y = z, on failure y = current x.
inline bool cas_next(Node* x, MarkedPtr& y, MarkedPtr z) noexcept {
    uintptr_t expected = mp_encode(y);
    uintptr_t desired  = mp_encode(z);
    if (x->next_.compare_exchange_strong(expected, desired,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
        y = z;
        return true;
    }
    y = mp_decode(expected);
    return false;
}

// CAS on prev_. Creates a new PrevWrapper for z_node. Old wrapper not freed (no reclamation).
// On success y = new PrevWrapper(z_node); on failure y = current prev_.
inline bool cas_prev(Node* x, CollisionlessPtr& y, Node* z_node) noexcept {
    PrevWrapper* pw = new PrevWrapper(z_node);
    if (x->prev_.compare_exchange_strong(y, pw,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
        y = pw;
        return true;
    }
    delete pw;   // never published; safe to free immediately
    return false;
}
