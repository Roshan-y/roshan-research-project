#pragma once
#include "debra.h"  // pulls in node.h transitively; also gives us Debra and retireRaw
#include <atomic>
#include <cassert>

// Lock-free external BST hint index (Natarajan-Mittal algorithm, adapted).
//
// Algorithm: Natarajan & Mittal, "Fast Concurrent Lock-free Binary Search Trees", PPoPP 2014.
// Adapted from Trevor Brown's implementation in SetBench:
//   https://gitlab.com/trbot86/setbench  (ds/natarajan_ext_bst_lf/)
//
// Pure hint layer: find_close(key) returns a list Node* close to the target so
// that list traversal starts there instead of from head. Correctness of the ordered
// set does not depend on this layer being up to date.
//
// Memory reclamation: DEBRA epoch-based (same instance as the list layer).
// When remove() commits a deletion (wins the FLAG_BIT CAS), it retires BOTH
// the leaf AND its parent (the internal node that gets spliced out). This matches
// what CBAT does for its chromatic tree nodes. Only the owning thread (the one
// that set FLAG_BIT) retires; concurrent helpers do not.
//
// At shutdown: ~HintBST walks the live tree and deletes all nodes still reachable
// from root_. DEBRA-retired nodes (spliced out, in epoch bags) are freed by ~Debra().
// There is no overlap: spliced-out nodes are no longer reachable from root_.

namespace bst_hint {

// Low 2 bits of a child pointer word: bit 0 = flag, bit 1 = mark (Natarajan encoding).
static constexpr uintptr_t FLAG_BIT = 1UL;
static constexpr uintptr_t MARK_BIT = 2UL;
static constexpr uintptr_t BITS_MASK = 3UL;

struct alignas(8) BSTNode {
    long key;
    bool is_leaf;
    std::atomic<uintptr_t> children[2]; // encoded: (ptr & ~3) | (mark<<1) | flag
    std::atomic<Node*>     list_ptr;    // non-null for leaves only

    explicit BSTNode(long k) : key(k), is_leaf(false), list_ptr(nullptr) {
        children[0] = children[1] = 0;
    }
    BSTNode(long k, Node* lp) : key(k), is_leaf(true), list_ptr(lp) {
        children[0] = children[1] = 0;
    }
};


inline BSTNode*  bst_addr(uintptr_t w)   { return reinterpret_cast<BSTNode*>(w & ~BITS_MASK); }
inline bool      bst_flagged(uintptr_t w) { return w & FLAG_BIT; }
inline bool      bst_marked(uintptr_t w)  { return w & MARK_BIT; }
inline bool      bst_free(uintptr_t w)    { return (w & BITS_MASK) == 0; }
inline uintptr_t bst_encode(BSTNode* p, bool mark, bool flag) {
    return reinterpret_cast<uintptr_t>(p) | (mark ? MARK_BIT : 0) | (flag ? FLAG_BIT : 0);
}

struct SeekRecord {
    BSTNode*  leaf;
    BSTNode*  parent;
    int       parent_dir;
    uintptr_t parent_child_w;
    BSTNode*  lum;          // last ancestor with an unmarked outgoing child pointer
    uintptr_t lum_child_w;
    int       lum_dir;
};

class HintBST {
    // root_ is a dummy anchor; root_->children[0] points to the real tree root.
    // The real tree always holds two sentinel leaves (KEY_MAX-1 and KEY_MAX) so that
    // the top-level internal node is never the target of a deletion.
    BSTNode* root_;

    // Free all nodes reachable from n (post-order). Called at destruction to
    // free nodes that are still live (never retired via DEBRA).
    static void free_live_nodes(BSTNode* n) {
        if (n == nullptr) return;
        if (!n->is_leaf) {
            free_live_nodes(bst_addr(n->children[0].load(std::memory_order_relaxed)));
            free_live_nodes(bst_addr(n->children[1].load(std::memory_order_relaxed)));
        }
        delete n;
    }

    void seek(long key, SeekRecord& sr) const {
        uintptr_t lum_cw = root_->children[0].load(std::memory_order_acquire);
        BSTNode*  lum     = root_;
        int       lum_dir = 0;

        BSTNode*  par     = bst_addr(lum_cw);
        int       par_dir = (key < par->key) ? 0 : 1;
        uintptr_t par_cw  = par->children[par_dir].load(std::memory_order_acquire);
        BSTNode*  leaf    = bst_addr(par_cw);

        while (!leaf->is_leaf) {
            int       dir     = (key < leaf->key) ? 0 : 1;
            uintptr_t child_w = leaf->children[dir].load(std::memory_order_acquire);

            if (!bst_marked(par_cw)) {
                lum     = par;
                lum_dir = par_dir;
                lum_cw  = par_cw;
            }
            par     = leaf;
            par_dir = dir;
            par_cw  = child_w;
            leaf    = bst_addr(child_w);
        }

        sr.leaf           = leaf;
        sr.parent         = par;
        sr.parent_dir     = par_dir;
        sr.parent_child_w = par_cw;
        sr.lum            = lum;
        sr.lum_dir        = lum_dir;
        sr.lum_child_w    = lum_cw;
    }

    // Natarajan-Mittal conflicting-flag resolution (their Algorithm 4).
    // When sr.parent's child pointer is flagged (leaf deletion in progress),
    // mark the sibling so it cannot be the target of a concurrent flag, then
    // CAS the last-unmarked-ancestor (lum) to point directly to the sibling,
    // effectively splicing out the parent and the flagged leaf together.
    // If the parent pointer is not flagged, a concurrent removal has already
    // flagged something above; CAS lum to the leaf directly to clean it up.
    void help_conflicting(const SeekRecord& sr) {
        if (bst_flagged(sr.parent_child_w)) {
            int sib_dir = 1 - sr.parent_dir;
            uintptr_t sib_w = sr.parent->children[sib_dir].load(std::memory_order_acquire);
            while (!bst_marked(sib_w)) {
                if (sr.parent->children[sib_dir].compare_exchange_weak(
                        sib_w, sib_w | MARK_BIT,
                        std::memory_order_acq_rel, std::memory_order_acquire))
                    break;
            }
            uintptr_t new_child = bst_encode(bst_addr(sib_w), false, bst_flagged(sib_w));
            uintptr_t exp = sr.lum_child_w;
            sr.lum->children[sr.lum_dir].compare_exchange_strong(
                exp, new_child,
                std::memory_order_acq_rel, std::memory_order_acquire);
        } else {
            BSTNode*  leaf_node = bst_addr(sr.parent_child_w);
            uintptr_t new_child = bst_encode(leaf_node, false, false);
            uintptr_t exp = sr.lum_child_w;
            sr.lum->children[sr.lum_dir].compare_exchange_strong(
                exp, new_child,
                std::memory_order_acq_rel, std::memory_order_acquire);
        }
    }

public:
    HintBST() {
        auto* left_sentinel  = new BSTNode(KEY_MAX - 1, nullptr);
        auto* right_sentinel = new BSTNode(KEY_MAX,     nullptr);
        auto* real_root      = new BSTNode(KEY_MAX);
        real_root->children[0].store(bst_encode(left_sentinel,  false, false), std::memory_order_relaxed);
        real_root->children[1].store(bst_encode(right_sentinel, false, false), std::memory_order_relaxed);

        root_ = new BSTNode(KEY_MAX);
        root_->children[0].store(bst_encode(real_root, false, false), std::memory_order_relaxed);
        root_->children[1].store(0, std::memory_order_relaxed);
        // Nodes are not tracked in any list; live nodes freed by ~HintBST tree walk,
        // retired nodes freed by ~Debra().
    }

    // Destructor: walk the live tree and delete every reachable node.
    // Nodes that were DEBRA-retired (spliced out) are not reachable here;
    // they are freed when ~Debra() drains its epoch bags.
    // All threads must have joined before this runs.
    ~HintBST() {
        free_live_nodes(root_);
    }

    // Return a list Node* close to key, or nullptr if the BST has no useful hint.
    // Must be called inside a DEBRA critical section so that retired nodes in
    // epoch bags are not freed while this traversal holds pointers to them.
    Node* find_close(long key) const {
        BSTNode* cur = bst_addr(root_->children[0].load(std::memory_order_acquire));
        while (cur != nullptr && !cur->is_leaf) {
            int       dir     = (key < cur->key) ? 0 : 1;
            uintptr_t child_w = cur->children[dir].load(std::memory_order_acquire);
            cur = bst_addr(child_w);
        }
        if (cur == nullptr || !cur->is_leaf) return nullptr;
        return cur->list_ptr.load(std::memory_order_acquire);
    }

    // Remove key from the BST hint index. Nulls the leaf's list_ptr so that
    // find_close stops returning it, physically splices the leaf from the tree,
    // then retires BOTH the leaf and the spliced-out parent node via DEBRA.
    //
    // Only the thread that wins the FLAG_BIT CAS (the "owner") retires nodes.
    // Concurrent helpers call help_conflicting but do not retire.
    // Must be called inside a DEBRA critical section.
    void remove(long key, Debra* debra, int tid) {
        static auto bst_deleter = [](void* p){ delete static_cast<BSTNode*>(p); };
        while (true) {
            SeekRecord sr;
            seek(key, sr);
            if (sr.leaf->key != key) return;
            // Null the list pointer so find_close returns nullptr for this key.
            sr.leaf->list_ptr.store(nullptr, std::memory_order_release);
            if (!bst_free(sr.parent_child_w)) {
                help_conflicting(sr);
                continue;
            }
            uintptr_t expected = sr.parent_child_w;
            if (!sr.parent->children[sr.parent_dir].compare_exchange_strong(
                    expected, expected | FLAG_BIT,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                continue;
            }
            sr.parent_child_w = expected | FLAG_BIT;
            help_conflicting(sr);
            // This thread owns the deletion. Retire both the leaf and the spliced-out
            // parent. DEBRA ensures neither is freed until all threads that could hold
            // pointers to them (via mid-traversal local variables) have exited their CS.
            debra->retireRaw(tid, sr.leaf,   bst_deleter);
            debra->retireRaw(tid, sr.parent, bst_deleter);
            return;
        }
    }

    // Insert or update key → list_node in the BST hint index.
    // Must be called inside a DEBRA critical section.
    void try_set(long key, Node* list_node) {
        while (true) {
            SeekRecord sr;
            seek(key, sr);

            if (sr.leaf->key == key) {
                sr.leaf->list_ptr.store(list_node, std::memory_order_release);
                return;
            }

            if (!bst_free(sr.parent_child_w)) {
                help_conflicting(sr);
                continue;
            }

            BSTNode* exist    = sr.leaf;
            BSTNode* new_leaf = new BSTNode(key, list_node);
            BSTNode* new_int  = new BSTNode(0);

            if (key < exist->key) {
                new_int->key = exist->key;
                new_int->children[0].store(bst_encode(new_leaf, false, false), std::memory_order_relaxed);
                new_int->children[1].store(bst_encode(exist,    false, false), std::memory_order_relaxed);
            } else {
                new_int->key = key;
                new_int->children[0].store(bst_encode(exist,    false, false), std::memory_order_relaxed);
                new_int->children[1].store(bst_encode(new_leaf, false, false), std::memory_order_relaxed);
            }

            uintptr_t expected = sr.parent_child_w;
            if (sr.parent->children[sr.parent_dir].compare_exchange_strong(
                    expected, bst_encode(new_int, false, false),
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                // new_leaf and new_int are now live in the tree. They will be
                // freed either by DEBRA (when their key is later removed) or
                // by ~HintBST's tree walk at shutdown.
                return;
            }

            // CAS failed before the new nodes were published — safe to delete them.
            delete new_leaf;
            delete new_int;
        }
    }

};

} // namespace bst_hint
