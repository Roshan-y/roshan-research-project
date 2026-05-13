#pragma once
#include "node.h"
#include <atomic>
#include <cassert>

// Lock-free external BST hint index (Natarajan-Mittal algorithm, adapted).
//
// Pure hint layer: find_close(key) returns a list Node* close to the target so
// that list traversal starts there instead of from head. Correctness of the ordered
// set does not depend on this layer being up to date.
//
// Memory reclamation: BST nodes are never freed (leaked). Add epoch-based or
// hazard-pointer reclamation before production use.

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
    }

    ~HintBST() = default;

    // Return a list Node* close to key, or nullptr if the BST has no useful hint.
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

    // Insert or update key → list_node in the BST hint index.
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
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return;

            // CAS failed before the new nodes were published — safe to delete them.
            delete new_leaf;
            delete new_int;
        }
    }

};

} // namespace bst_hint
