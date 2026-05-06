#pragma once
#include "node.h"
#include <atomic>
#include <cstdint>

// Lock-free external BST hint index using a Chromatic Tree (relaxed red-black tree).
//
// Adapted from Brown's chromatic tree (CBAT, brown_ext_chromatic_lf/).
// Stripped of: DEBRA memory management, itime/dtime range-query augmentation,
// per-thread node pools, value fields.  All BST nodes and SCX records are leaked.
//
// Weight convention (chromatic / relaxed red-black):
//   weight == 0  →  red node
//   weight == 1  →  black node
//   weight  > 1  →  overweight violation (introduced by insert, fixed lazily)
//   Two consecutive weight-0 nodes (parent + child) = red-red violation
//
// After insert/erase, if more than HINT_N violations are counted on the path,
// fix_all(key) applies one rebalancing step per call until the path is clean.
// Failed rebalancing steps are harmless: a stale/unbalanced hint only lengthens
// list traversal, never produces an incorrect result.

namespace bst_balanced_hint {

// ── Forward declarations ──────────────────────────────────────────────────────
struct BSTNodeC;
struct SCXRecC;

static constexpr int MAX_NODES_C = 6;
static constexpr int HINT_N      = 6;   // max violations before rebalancing

// LLX_IS_LEAF: returned by llx() when the node is a leaf.
// Address of a file-scope sentinel — never a valid SCXRecC* or nullptr.
namespace detail {
    inline void* leaf_sentinel_addr() { static char s; return &s; }
}
static void* const LLX_IS_LEAF = detail::leaf_sentinel_addr();

// null_scxrec(): singleton dummy SCXRecC with state COMMITTED.
// Used as the initial scx_rec value for every newly-created BSTNodeC.
// Returning a stable non-null pointer lets llx() distinguish
// "stable initial node" (returns null_scxrec()) from "retry" (returns nullptr).
SCXRecC* null_scxrec();   // defined after SCXRecC

// ── Memory ordering shorthands ────────────────────────────────────────────────
static constexpr std::memory_order mo_rel     = std::memory_order_release;
static constexpr std::memory_order mo_acq     = std::memory_order_acquire;
static constexpr std::memory_order mo_acqrel  = std::memory_order_acq_rel;
static constexpr std::memory_order mo_relaxed = std::memory_order_relaxed;

// ── BSTNodeC ──────────────────────────────────────────────────────────────────
struct alignas(64) BSTNodeC {
    long                   key;
    int                    weight;  // chromatic weight
    bool                   is_leaf;
    std::atomic<BSTNodeC*> left;
    std::atomic<BSTNodeC*> right;
    std::atomic<SCXRecC*>  scx_rec; // LLX/SCX descriptor; null_scxrec() if no active op
    std::atomic<bool>      marked;  // true after physical retirement
    std::atomic<Node*>     list_ptr;// valid for leaf nodes only; nullptr otherwise

    // Internal node
    BSTNodeC(long k, int w, BSTNodeC* l, BSTNodeC* r)
        : key(k), weight(w), is_leaf(false),
          left(l), right(r), scx_rec(null_scxrec()),
          marked(false), list_ptr(nullptr) {}

    // Leaf node
    BSTNodeC(long k, Node* lp)
        : key(k), weight(1), is_leaf(true),
          left(nullptr), right(nullptr), scx_rec(null_scxrec()),
          marked(false), list_ptr(lp) {}
};

// ── SCXRecC ───────────────────────────────────────────────────────────────────
struct SCXRecC {
    static constexpr int INPROGRESS = 0;
    static constexpr int COMMITTED  = 1;
    static constexpr int ABORTED    = 2;

    std::atomic<int>        state;       // INPROGRESS / COMMITTED / ABORTED
    std::atomic<bool>       all_frozen;  // true after all freeze-CASes succeed
    int                     type;
    int                     num_to_freeze;
    int                     num_of_nodes;
    BSTNodeC*               nodes[MAX_NODES_C];
    void*                   scx_seen[MAX_NODES_C]; // SCXRecC* or LLX_IS_LEAF
    std::atomic<BSTNodeC*>* field;       // child pointer to update atomically
    BSTNodeC*               new_node;    // replacement subtree root

    SCXRecC() : state(INPROGRESS), all_frozen(false), type(0),
                num_to_freeze(0), num_of_nodes(0), field(nullptr), new_node(nullptr) {
        for (int i = 0; i < MAX_NODES_C; i++) { nodes[i] = nullptr; scx_seen[i] = nullptr; }
    }
};

// Meyers singleton: one global dummy record with state COMMITTED.
// BSTNodeC::scx_rec is initialised to this; llx() treats it as "stable, no SCX".
inline SCXRecC* null_scxrec() {
    static SCXRecC dummy;
    // Initialise once; harmless to re-set after construction.
    dummy.state.store(SCXRecC::COMMITTED, mo_relaxed);
    dummy.all_frozen.store(false,         mo_relaxed);
    return &dummy;
}

// ── BalancedHintBST ───────────────────────────────────────────────────────────
class BalancedHintBST {
    BSTNodeC* root_;  // dummy anchor node; root_->left → real tree root

    static bool is_sentinel_key(long k) noexcept { return k >= KEY_MAX - 1; }

    // ── Node allocation ──────────────────────────────────────────────────────
    static BSTNodeC* make_int(long k, int w, BSTNodeC* l, BSTNodeC* r) {
        return new BSTNodeC(k, w, l, r);
    }
    static BSTNodeC* make_leaf(long k, Node* lp) { return new BSTNodeC(k, lp); }
    static BSTNodeC* copy_int(const BSTNodeC* s, int w, BSTNodeC* l, BSTNodeC* r) {
        return make_int(s->key, w, l, r);
    }

    // ── LLX/SCX ──────────────────────────────────────────────────────────────

    // help_scx: bring an in-progress SCX to completion (called by any thread).
    static void help_scx(SCXRecC* op) {
        if (op->state.load(mo_acq) != SCXRecC::INPROGRESS) return;

        // Phase 1: freeze all required nodes
        if (!op->all_frozen.load(mo_acq)) {
            for (int i = 0; i < op->num_to_freeze; i++) {
                if (op->scx_seen[i] == LLX_IS_LEAF) continue; // leaves don't get frozen

                SCXRecC* expected = static_cast<SCXRecC*>(op->scx_seen[i]);
                op->nodes[i]->scx_rec.compare_exchange_strong(
                    expected, op, mo_acqrel, mo_acq);

                SCXRecC* cur = op->nodes[i]->scx_rec.load(mo_acq);
                if (cur != op) {
                    // Another SCX owns this node; abort unless all_frozen is set
                    if (!op->all_frozen.load(mo_acq)) {
                        int es = SCXRecC::INPROGRESS;
                        op->state.compare_exchange_strong(
                            es, SCXRecC::ABORTED, mo_acqrel, mo_acq);
                    }
                    return;
                }
            }
            bool ef = false;
            op->all_frozen.compare_exchange_strong(ef, true, mo_acqrel, mo_acq);
        }

        if (op->state.load(mo_acq) != SCXRecC::INPROGRESS) return;

        // Phase 2: apply update + mark retired nodes
        op->field->store(op->new_node, mo_rel);
        for (int i = 1; i < op->num_of_nodes; i++)
            op->nodes[i]->marked.store(true, mo_rel);

        op->state.store(SCXRecC::COMMITTED, mo_rel);
    }

    // llx: snapshot a node's stable state.
    // Returns: LLX_IS_LEAF         → leaf (callers use scx_seen[i] = LLX_IS_LEAF)
    //          non-null SCXRecC*   → stable internal node; use as scx_seen[i]
    //          nullptr             → frozen or retired; outer operation must retry
    static void* llx(BSTNodeC* node, BSTNodeC** left, BSTNodeC** right) {
        SCXRecC* rinfo = node->scx_rec.load(mo_acq);

        // Check if node is currently involved in an in-progress SCX
        if (rinfo != null_scxrec()) {
            int state = rinfo->state.load(mo_acq);
            if (state == SCXRecC::INPROGRESS) {
                if (!rinfo->all_frozen.load(mo_acq)) help_scx(rinfo);
                return nullptr; // frozen → retry
            }
            // state == COMMITTED or ABORTED: node is stable
        }

        if (node->marked.load(mo_relaxed)) return nullptr;

        *left  = node->left.load(mo_relaxed);
        *right = node->right.load(mo_relaxed);

        if (node->scx_rec.load(mo_acq) != rinfo) return nullptr; // snapshot changed

        if (*left == nullptr) return LLX_IS_LEAF;
        return rinfo; // rinfo != nullptr (either null_scxrec() or a committed/aborted record)
    }

    // scx_exec: construct and execute one SCX.
    static bool scx_exec(int type, int ntf, int nof,
                         BSTNodeC** nodes, void** seen,
                         std::atomic<BSTNodeC*>* field, BSTNodeC* new_node) {
        SCXRecC* op = new SCXRecC();
        op->type          = type;
        op->num_to_freeze = ntf;
        op->num_of_nodes  = nof;
        op->field         = field;
        op->new_node      = new_node;
        for (int i = 0; i < ntf; i++) { op->nodes[i] = nodes[i]; op->scx_seen[i] = seen[i]; }
        for (int i = ntf; i < nof; i++) op->nodes[i] = nodes[i];

        help_scx(op);
        return op->state.load(mo_acq) == SCXRecC::COMMITTED;
    }

    // ── Rebalancing operations ────────────────────────────────────────────────
    // Each do_xxx builds new nodes and calls scx_exec.
    // Node array convention (Brown 2014):
    //   nodes[0] = parent of modified subtree (not retired)
    //   nodes[1] = root of modified subtree
    //   nodes[2..] = children/siblings involved in the rotation

    static std::atomic<BSTNodeC*>* cf(BSTNodeC* p, bool left_side) {
        return left_side ? &p->left : &p->right;
    }

    // BLK: absorb two red children; parent weight −1.
    bool do_blk(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nXL = copy_int(ns[2],1,ns[2]->left.load(mo_relaxed),ns[2]->right.load(mo_relaxed));
        BSTNodeC* nXR = copy_int(ns[3],1,ns[3]->left.load(mo_relaxed),ns[3]->right.load(mo_relaxed));
        int w = is_sentinel_key(ns[1]->key) ? 1 : ns[1]->weight-1;
        BSTNodeC* nX = make_int(ns[1]->key,w,nXL,nXR);
        return scx_exec(2,4,4,ns,s,cf(ns[0],fl),nX);
    }
    // RB1: right rotation (left-left red-red).
    bool do_rb1(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nXR = copy_int(ns[1],0,ns[2]->right.load(mo_relaxed),ns[1]->right.load(mo_relaxed));
        BSTNodeC* nX  = make_int(ns[2]->key,ns[1]->weight,ns[2]->left.load(mo_relaxed),nXR);
        return scx_exec(3,3,3,ns,s,cf(ns[0],fl),nX);
    }
    // RB1Sym: left rotation (right-right red-red).
    bool do_rb1s(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nXL = copy_int(ns[1],0,ns[1]->left.load(mo_relaxed),ns[2]->left.load(mo_relaxed));
        BSTNodeC* nX  = make_int(ns[2]->key,ns[1]->weight,nXL,ns[2]->right.load(mo_relaxed));
        return scx_exec(17,3,3,ns,s,cf(ns[0],fl),nX);
    }
    // RB2: double rotation (left-right red-red).
    bool do_rb2(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nXL = copy_int(ns[2],0,ns[2]->left.load(mo_relaxed),ns[3]->left.load(mo_relaxed));
        BSTNodeC* nXR = copy_int(ns[1],0,ns[3]->right.load(mo_relaxed),ns[1]->right.load(mo_relaxed));
        BSTNodeC* nX  = make_int(ns[3]->key,ns[1]->weight,nXL,nXR);
        return scx_exec(4,4,4,ns,s,cf(ns[0],fl),nX);
    }
    // RB2Sym: double rotation (right-left red-red).
    bool do_rb2s(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nXL = copy_int(ns[1],0,ns[1]->left.load(mo_relaxed),ns[3]->left.load(mo_relaxed));
        BSTNodeC* nXR = copy_int(ns[2],0,ns[3]->right.load(mo_relaxed),ns[2]->right.load(mo_relaxed));
        BSTNodeC* nX  = make_int(ns[3]->key,ns[1]->weight,nXL,nXR);
        return scx_exec(18,4,4,ns,s,cf(ns[0],fl),nX);
    }
    // PUSH: push overweight up.
    bool do_push(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nL = copy_int(ns[2],ns[2]->weight-1,ns[2]->left.load(mo_relaxed),ns[2]->right.load(mo_relaxed));
        BSTNodeC* nR = copy_int(ns[3],0,ns[3]->left.load(mo_relaxed),ns[3]->right.load(mo_relaxed));
        int w = is_sentinel_key(ns[1]->key) ? 1 : ns[1]->weight+1;
        BSTNodeC* nX = make_int(ns[1]->key,w,nL,nR);
        return scx_exec(5,4,4,ns,s,cf(ns[0],fl),nX);
    }
    bool do_pushs(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nL = copy_int(ns[2],0,ns[2]->left.load(mo_relaxed),ns[2]->right.load(mo_relaxed));
        BSTNodeC* nR = copy_int(ns[3],ns[3]->weight-1,ns[3]->left.load(mo_relaxed),ns[3]->right.load(mo_relaxed));
        int w = is_sentinel_key(ns[1]->key) ? 1 : ns[1]->weight+1;
        BSTNodeC* nX = make_int(ns[1]->key,w,nL,nR);
        return scx_exec(19,4,4,ns,s,cf(ns[0],fl),nX);
    }
    // W1: overweight, right sibling red.
    bool do_w1(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nLL=copy_int(ns[2],ns[2]->weight-1,ns[2]->left.load(mo_relaxed),ns[2]->right.load(mo_relaxed));
        BSTNodeC* nLR=copy_int(ns[4],ns[4]->weight-1,ns[4]->left.load(mo_relaxed),ns[4]->right.load(mo_relaxed));
        BSTNodeC* nL =make_int(ns[1]->key,1,nLL,nLR);
        BSTNodeC* nX =make_int(ns[3]->key,ns[1]->weight,nL,ns[3]->right.load(mo_relaxed));
        return scx_exec(6,5,5,ns,s,cf(ns[0],fl),nX);
    }
    bool do_w1s(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nRL=copy_int(ns[4],ns[4]->weight-1,ns[4]->left.load(mo_relaxed),ns[4]->right.load(mo_relaxed));
        BSTNodeC* nRR=copy_int(ns[3],ns[3]->weight-1,ns[3]->left.load(mo_relaxed),ns[3]->right.load(mo_relaxed));
        BSTNodeC* nR =make_int(ns[1]->key,1,nRL,nRR);
        BSTNodeC* nX =make_int(ns[2]->key,ns[1]->weight,ns[2]->left.load(mo_relaxed),nR);
        return scx_exec(20,5,5,ns,s,cf(ns[0],fl),nX);
    }
    // W2
    bool do_w2(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nLL=copy_int(ns[2],ns[2]->weight-1,ns[2]->left.load(mo_relaxed),ns[2]->right.load(mo_relaxed));
        BSTNodeC* nLR=copy_int(ns[4],0,ns[4]->left.load(mo_relaxed),ns[4]->right.load(mo_relaxed));
        BSTNodeC* nL =make_int(ns[1]->key,1,nLL,nLR);
        BSTNodeC* nX =make_int(ns[3]->key,ns[1]->weight,nL,ns[3]->right.load(mo_relaxed));
        return scx_exec(7,5,5,ns,s,cf(ns[0],fl),nX);
    }
    bool do_w2s(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nRL=copy_int(ns[4],0,ns[4]->left.load(mo_relaxed),ns[4]->right.load(mo_relaxed));
        BSTNodeC* nRR=copy_int(ns[3],ns[3]->weight-1,ns[3]->left.load(mo_relaxed),ns[3]->right.load(mo_relaxed));
        BSTNodeC* nR =make_int(ns[1]->key,1,nRL,nRR);
        BSTNodeC* nX =make_int(ns[2]->key,ns[1]->weight,ns[2]->left.load(mo_relaxed),nR);
        return scx_exec(21,5,5,ns,s,cf(ns[0],fl),nX);
    }
    // W3 (6 nodes)
    bool do_w3(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nLLL=copy_int(ns[2],ns[2]->weight-1,ns[2]->left.load(mo_relaxed),ns[2]->right.load(mo_relaxed));
        BSTNodeC* nLL =make_int(ns[1]->key,1,nLLL,ns[5]->left.load(mo_relaxed));
        BSTNodeC* nLR =make_int(ns[4]->key,1,ns[5]->right.load(mo_relaxed),ns[4]->right.load(mo_relaxed));
        BSTNodeC* nL  =make_int(ns[5]->key,0,nLL,nLR);
        BSTNodeC* nX  =make_int(ns[3]->key,ns[1]->weight,nL,ns[3]->right.load(mo_relaxed));
        return scx_exec(8,6,6,ns,s,cf(ns[0],fl),nX);
    }
    bool do_w3s(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nRL =make_int(ns[4]->key,1,ns[4]->left.load(mo_relaxed),ns[5]->left.load(mo_relaxed));
        BSTNodeC* nRRR=copy_int(ns[3],ns[3]->weight-1,ns[3]->left.load(mo_relaxed),ns[3]->right.load(mo_relaxed));
        BSTNodeC* nRR =make_int(ns[1]->key,1,ns[5]->right.load(mo_relaxed),nRRR);
        BSTNodeC* nR  =make_int(ns[5]->key,0,nRL,nRR);
        BSTNodeC* nX  =make_int(ns[2]->key,ns[1]->weight,ns[2]->left.load(mo_relaxed),nR);
        return scx_exec(22,6,6,ns,s,cf(ns[0],fl),nX);
    }
    // W4 (6 nodes)
    bool do_w4(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nLL=copy_int(ns[2],ns[2]->weight-1,ns[2]->left.load(mo_relaxed),ns[2]->right.load(mo_relaxed));
        BSTNodeC* nL =make_int(ns[1]->key,1,nLL,ns[4]->left.load(mo_relaxed));
        BSTNodeC* nRL=copy_int(ns[5],1,ns[5]->left.load(mo_relaxed),ns[5]->right.load(mo_relaxed));
        BSTNodeC* nR =make_int(ns[3]->key,0,nRL,ns[3]->right.load(mo_relaxed));
        BSTNodeC* nX =make_int(ns[4]->key,ns[1]->weight,nL,nR);
        return scx_exec(9,6,6,ns,s,cf(ns[0],fl),nX);
    }
    bool do_w4s(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nLR=copy_int(ns[5],1,ns[5]->left.load(mo_relaxed),ns[5]->right.load(mo_relaxed));
        BSTNodeC* nL =make_int(ns[2]->key,0,ns[2]->left.load(mo_relaxed),nLR);
        BSTNodeC* nRR=copy_int(ns[3],ns[3]->weight-1,ns[3]->left.load(mo_relaxed),ns[3]->right.load(mo_relaxed));
        BSTNodeC* nR =make_int(ns[1]->key,1,ns[4]->right.load(mo_relaxed),nRR);
        BSTNodeC* nX =make_int(ns[4]->key,ns[1]->weight,nL,nR);
        return scx_exec(23,6,6,ns,s,cf(ns[0],fl),nX);
    }
    // W5
    bool do_w5(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nLL=copy_int(ns[2],ns[2]->weight-1,ns[2]->left.load(mo_relaxed),ns[2]->right.load(mo_relaxed));
        BSTNodeC* nL =make_int(ns[1]->key,1,nLL,ns[3]->left.load(mo_relaxed));
        BSTNodeC* nR =copy_int(ns[4],1,ns[4]->left.load(mo_relaxed),ns[4]->right.load(mo_relaxed));
        BSTNodeC* nX =make_int(ns[3]->key,ns[1]->weight,nL,nR);
        return scx_exec(10,5,5,ns,s,cf(ns[0],fl),nX);
    }
    bool do_w5s(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nL =copy_int(ns[4],1,ns[4]->left.load(mo_relaxed),ns[4]->right.load(mo_relaxed));
        BSTNodeC* nRR=copy_int(ns[3],ns[3]->weight-1,ns[3]->left.load(mo_relaxed),ns[3]->right.load(mo_relaxed));
        BSTNodeC* nR =make_int(ns[1]->key,1,ns[2]->right.load(mo_relaxed),nRR);
        BSTNodeC* nX =make_int(ns[2]->key,ns[1]->weight,nL,nR);
        return scx_exec(24,5,5,ns,s,cf(ns[0],fl),nX);
    }
    // W6
    bool do_w6(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nLL=copy_int(ns[2],ns[2]->weight-1,ns[2]->left.load(mo_relaxed),ns[2]->right.load(mo_relaxed));
        BSTNodeC* nL =make_int(ns[1]->key,1,nLL,ns[4]->left.load(mo_relaxed));
        BSTNodeC* nR =make_int(ns[3]->key,1,ns[4]->right.load(mo_relaxed),ns[3]->right.load(mo_relaxed));
        BSTNodeC* nX =make_int(ns[4]->key,ns[1]->weight,nL,nR);
        return scx_exec(11,5,5,ns,s,cf(ns[0],fl),nX);
    }
    bool do_w6s(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nL =make_int(ns[2]->key,1,ns[2]->left.load(mo_relaxed),ns[4]->left.load(mo_relaxed));
        BSTNodeC* nRR=copy_int(ns[3],ns[3]->weight-1,ns[3]->left.load(mo_relaxed),ns[3]->right.load(mo_relaxed));
        BSTNodeC* nR =make_int(ns[1]->key,1,ns[4]->right.load(mo_relaxed),nRR);
        BSTNodeC* nX =make_int(ns[4]->key,ns[1]->weight,nL,nR);
        return scx_exec(25,5,5,ns,s,cf(ns[0],fl),nX);
    }
    // W7: both children overweight; push weight up.
    bool do_w7(BSTNodeC** ns, void** s, bool fl) {
        BSTNodeC* nL=copy_int(ns[2],ns[2]->weight-1,ns[2]->left.load(mo_relaxed),ns[2]->right.load(mo_relaxed));
        BSTNodeC* nR=copy_int(ns[3],ns[3]->weight-1,ns[3]->left.load(mo_relaxed),ns[3]->right.load(mo_relaxed));
        int w = is_sentinel_key(ns[1]->key) ? 1 : ns[1]->weight+1;
        BSTNodeC* nX=make_int(ns[1]->key,w,nL,nR);
        return scx_exec(12,4,4,ns,s,cf(ns[0],fl),nX);
    }
    // W7Sym is identical in structure.
    bool do_w7s(BSTNodeC** ns, void** s, bool fl) { return do_w7(ns,s,fl); }

    // ── update_insert / update_erase / update_rebalance ──────────────────────

    // Returns false to signal "retry" (LLX failed). Returns true on success or
    // "no-op" (key already present / not found).
    bool update_insert(long key, Node* list_node, bool* should_rebal) {
        BSTNodeC *p = root_;
        BSTNodeC *l = root_->left.load(mo_acq);
        if (l == nullptr) return true;

        int count = 0;
        if (l->left.load(mo_relaxed) != nullptr) {
            p = l; l = l->left.load(mo_acq);
            while (l->left.load(mo_relaxed) != nullptr) {
                if (l->weight > 1 || (l->weight == 0 && p->weight == 0)) ++count;
                p = l;
                l = (key < l->key) ? l->left.load(mo_acq) : l->right.load(mo_acq);
            }
        }

        // l is the leaf where key belongs
        if (l->key == key) {
            l->list_ptr.store(list_node, mo_rel);
            *should_rebal = false;
            return true;
        }

        BSTNodeC *pl, *pr;
        void* ip = llx(p, &pl, &pr);
        if (ip == nullptr) return false;
        if (l != pl && l != pr) return false;

        int nw = is_sentinel_key(l->key) ? 1 : (l->weight - 1);
        BSTNodeC* nl  = make_leaf(key, list_node);
        BSTNodeC* cl  = make_leaf(l->key, l->list_ptr.load(mo_relaxed));
        BSTNodeC* ni;
        if (l->key == KEY_MAX || key < l->key)
            ni = make_int(l->key, nw, nl, cl);
        else
            ni = make_int(key,    nw, cl, nl);

        BSTNodeC* ns[] = {p, l};
        void*     sv[] = {ip, LLX_IS_LEAF};
        bool ok = scx_exec(0,1,2,ns,sv,(l==pl?&p->left:&p->right),ni);
        *should_rebal = ok && (count > HINT_N);
        if (!ok) { delete nl; delete cl; delete ni; }
        return true; // tell caller not to outer-retry on CAS fail; it will rebalance
    }

    bool update_erase(long key, bool* should_rebal) {
        BSTNodeC *gp = root_, *p = nullptr;
        BSTNodeC *l = root_->left.load(mo_acq);
        if (l == nullptr || l->left.load(mo_relaxed) == nullptr) return true;

        gp = root_; p = l; l = l->left.load(mo_acq);
        int count = 0;
        while (l->left.load(mo_relaxed) != nullptr) {
            if (l->weight > 1 || (l->weight == 0 && p->weight == 0)) ++count;
            gp = p; p = l;
            l = (key < l->key) ? l->left.load(mo_acq) : l->right.load(mo_acq);
        }
        if (l->key != key) { *should_rebal = false; return true; }

        BSTNodeC *gpl, *gpr; void* igp = llx(gp, &gpl, &gpr); if (!igp) return false;
        if (p != gpl && p != gpr) return false;
        BSTNodeC *pl,  *pr;  void* ip  = llx(p,  &pl,  &pr);  if (!ip)  return false;
        if (l != pl && l != pr) return false;

        BSTNodeC* s = (l == pl) ? pr : pl;
        BSTNodeC *sl, *sr; void* is_ = llx(s, &sl, &sr); if (!is_) return false;

        int nw = is_sentinel_key(p->key) ? 1 : (p->weight + s->weight);
        BSTNodeC* ns_new = make_int(s->key, nw, sl, sr);

        BSTNodeC* nd[] = {gp, p, s, l};
        void*     sv[] = {igp, ip, is_, LLX_IS_LEAF};
        bool ok = scx_exec(1,3,4,nd,sv,(p==gpl?&gp->left:&gp->right),ns_new);
        *should_rebal = ok && (count > HINT_N);
        if (!ok) delete ns_new;
        return true;
    }

    // Returns true when path is clean.
    bool update_rebalance(long key) {
        BSTNodeC *ggp=root_, *gp=root_, *p=root_;
        BSTNodeC *l = root_->left.load(mo_acq);
        if (l==nullptr || l->left.load(mo_relaxed)==nullptr) return true;

        gp = p = l; l = l->left.load(mo_acq);
        while (l->left.load(mo_relaxed) != nullptr
               && l->weight <= 1
               && !(l->weight==0 && p->weight==0)) {
            ggp=gp; gp=p; p=l;
            l = (key<l->key) ? l->left.load(mo_acq) : l->right.load(mo_acq);
        }
        if (l->weight==1 || l->left.load(mo_relaxed)==nullptr) return true;

        BSTNodeC *u=ggp, *uX=gp, *uXX=p, *uXXX=l;
        void *iu, *iuX, *iuXX, *iuXXL, *iuXXR;
        BSTNodeC *uL,*uR, *uXL,*uXR, *uXXL,*uXXR;

        if (!(iu  = llx(u,  &uL,  &uR)))  return false;
        bool xL = (uX==uL);  if (!xL && uX!=uR) return false;
        if (!(iuX = llx(uX, &uXL, &uXR))) return false;
        bool xxL= (uXX==uXL); if (!xxL && uXX!=uXR) return false;
        if (!(iuXX= llx(uXX,&uXXL,&uXXR)))return false;
        bool xxxL=(uXXX==uXXL); if (!xxxL && uXXX!=uXXR) return false;

        BSTNodeC *uXXLL=nullptr,*uXXLR=nullptr,*uXXRL=nullptr,*uXXRR=nullptr;
        if (!(iuXXL=llx(uXXL,&uXXLL,&uXXLR))) return false;
        if (!(iuXXR=llx(uXXR,&uXXRL,&uXXRR))) return false;

        // ── Overweight ───────────────────────────────────────────────────────
        if (uXXX->weight > 1) {
            if (xxxL) { // overweight on left
                if (uXXR->weight==0) {
                    if (uXX->weight==0) {
                        if (xxL) {
                            void *iuXR; BSTNodeC *a,*b;
                            if (!(iuXR=llx(uXR,&a,&b))) return false;
                            BSTNodeC* ns[]={u,uX,uXX,uXR}; void* sv[]={iu,iuX,iuXX,iuXR};
                            return (uXR->weight==0)?do_blk(ns,sv,xL):do_rb2(ns,sv,xL);
                        } else {
                            void *iuXL; BSTNodeC *a,*b;
                            if (!(iuXL=llx(uXL,&a,&b))) return false;
                            BSTNodeC* ns[]={u,uX,uXL,uXX}; void* sv[]={iu,iuX,iuXL,iuXX};
                            return (uXL->weight==0)?do_blk(ns,sv,xL):do_rb1s(ns,sv,xL);
                        }
                    } else {
                        if (!uXXRL) return false;
                        void *iRL; BSTNodeC *a,*b;
                        if (!(iRL=llx(uXXRL,&a,&b))) return false;
                        if (uXXRL->weight>1) {
                            BSTNodeC* ns[]={uX,uXX,uXXL,uXXR,uXXRL}; void* sv[]={iuX,iuXX,iuXXL,iuXXR,iRL};
                            return do_w1(ns,sv,xxL);
                        } else if (uXXRL->weight==0) {
                            BSTNodeC* ns[]={uX,uXX,uXXR,uXXRL}; void* sv[]={iuX,iuXX,iuXXR,iRL};
                            return do_rb2s(ns,sv,xxL);
                        } else {
                            if (!a) return false; // uXXRLR missing
                            void *iRLR; BSTNodeC *c,*d;
                            if (!(iRLR=llx(a,&c,&d))) return false; // a = uXXRLR... wait, b is right child of uXXRL
                            // a=uXXRLL, b=uXXRLR from llx(uXXRL)
                            if (b==nullptr) return false;
                            void *iRLR2; BSTNodeC *e,*f;
                            if (!(iRLR2=llx(b,&e,&f))) return false;
                            if (b && b->weight==0) { // uXXRLR is red
                                BSTNodeC* ns[]={uX,uXX,uXXL,uXXR,uXXRL,b}; void* sv[]={iuX,iuXX,iuXXL,iuXXR,iRL,iRLR2};
                                return do_w4(ns,sv,xxL);
                            } else {
                                if (!a) return false;
                                void *iRLL; BSTNodeC *g,*h;
                                if (!(iRLL=llx(a,&g,&h))) return false;
                                if (a->weight==0) {
                                    BSTNodeC* ns[]={uX,uXX,uXXL,uXXR,uXXRL,a}; void* sv[]={iuX,iuXX,iuXXL,iuXXR,iRL,iRLL};
                                    return do_w3(ns,sv,xxL);
                                } else {
                                    BSTNodeC* ns[]={uX,uXX,uXXL,uXXR,uXXRL}; void* sv[]={iuX,iuXX,iuXXL,iuXXR,iRL};
                                    return do_w2(ns,sv,xxL);
                                }
                            }
                        }
                    }
                } else if (uXXR->weight==1) {
                    if (!uXXRL||!uXXRR) return false;
                    void *iRR,*iRL2; BSTNodeC *a,*b,*c,*d;
                    if (!(iRR=llx(uXXRR,&a,&b))) return false;
                    if (!(iRL2=llx(uXXRL,&c,&d))) return false;
                    if (uXXRR->weight==0) {
                        BSTNodeC* ns[]={uX,uXX,uXXL,uXXR,uXXRR}; void* sv[]={iuX,iuXX,iuXXL,iuXXR,iRR};
                        return do_w5(ns,sv,xxL);
                    } else if (uXXRL->weight==0) {
                        BSTNodeC* ns[]={uX,uXX,uXXL,uXXR,uXXRL}; void* sv[]={iuX,iuXX,iuXXL,iuXXR,iRL2};
                        return do_w6(ns,sv,xxL);
                    } else {
                        BSTNodeC* ns[]={uX,uXX,uXXL,uXXR}; void* sv[]={iuX,iuXX,iuXXL,iuXXR};
                        return do_push(ns,sv,xxL);
                    }
                } else {
                    BSTNodeC* ns[]={uX,uXX,uXXL,uXXR}; void* sv[]={iuX,iuXX,iuXXL,iuXXR};
                    return do_w7(ns,sv,xxL);
                }
            } else { // overweight on right (symmetric)
                if (uXXL->weight==0) {
                    if (uXX->weight==0) {
                        if (!xxL) {
                            void *iuXL; BSTNodeC *a,*b;
                            if (!(iuXL=llx(uXL,&a,&b))) return false;
                            BSTNodeC* ns[]={u,uX,uXL,uXX}; void* sv[]={iu,iuX,iuXL,iuXX};
                            return (uXL->weight==0)?do_blk(ns,sv,xL):do_rb2s(ns,sv,xL);
                        } else {
                            void *iuXR; BSTNodeC *a,*b;
                            if (!(iuXR=llx(uXR,&a,&b))) return false;
                            BSTNodeC* ns[]={u,uX,uXX,uXR}; void* sv[]={iu,iuX,iuXX,iuXR};
                            return (uXR->weight==0)?do_blk(ns,sv,xL):do_rb1(ns,sv,xL);
                        }
                    } else {
                        if (!uXXLR) return false;
                        void *iLR; BSTNodeC *a,*b;
                        if (!(iLR=llx(uXXLR,&a,&b))) return false;
                        if (uXXLR->weight>1) {
                            BSTNodeC* ns[]={uX,uXX,uXXL,uXXR,uXXLR}; void* sv[]={iuX,iuXX,iuXXL,iuXXR,iLR};
                            return do_w1s(ns,sv,xxL);
                        } else if (uXXLR->weight==0) {
                            BSTNodeC* ns[]={uX,uXX,uXXL,uXXLR}; void* sv[]={iuX,iuXX,iuXXL,iLR};
                            return do_rb2(ns,sv,xxL);
                        } else {
                            if (!a) return false;
                            void *iLRL; BSTNodeC *c,*d;
                            if (!(iLRL=llx(a,&c,&d))) return false;
                            if (a->weight==0) {
                                BSTNodeC* ns[]={uX,uXX,uXXL,uXXR,uXXLR,a}; void* sv[]={iuX,iuXX,iuXXL,iuXXR,iLR,iLRL};
                                return do_w4s(ns,sv,xxL);
                            } else {
                                if (!b) return false;
                                void *iLRR; BSTNodeC *e,*f;
                                if (!(iLRR=llx(b,&e,&f))) return false;
                                if (b->weight==0) {
                                    BSTNodeC* ns[]={uX,uXX,uXXL,uXXR,uXXLR,b}; void* sv[]={iuX,iuXX,iuXXL,iuXXR,iLR,iLRR};
                                    return do_w3s(ns,sv,xxL);
                                } else {
                                    BSTNodeC* ns[]={uX,uXX,uXXL,uXXR,uXXLR}; void* sv[]={iuX,iuXX,iuXXL,iuXXR,iLR};
                                    return do_w2s(ns,sv,xxL);
                                }
                            }
                        }
                    }
                } else if (uXXL->weight==1) {
                    if (!uXXLL||!uXXLR) return false;
                    void *iLL,*iLR2; BSTNodeC *a,*b,*c,*d;
                    if (!(iLL=llx(uXXLL,&a,&b))) return false;
                    if (!(iLR2=llx(uXXLR,&c,&d))) return false;
                    if (uXXLL->weight==0) {
                        BSTNodeC* ns[]={uX,uXX,uXXL,uXXR,uXXLL}; void* sv[]={iuX,iuXX,iuXXL,iuXXR,iLL};
                        return do_w5s(ns,sv,xxL);
                    } else if (uXXLR->weight==0) {
                        BSTNodeC* ns[]={uX,uXX,uXXL,uXXR,uXXLR}; void* sv[]={iuX,iuXX,iuXXL,iuXXR,iLR2};
                        return do_w6s(ns,sv,xxL);
                    } else {
                        BSTNodeC* ns[]={uX,uXX,uXXL,uXXR}; void* sv[]={iuX,iuXX,iuXXL,iuXXR};
                        return do_pushs(ns,sv,xxL);
                    }
                } else {
                    BSTNodeC* ns[]={uX,uXX,uXXL,uXXR}; void* sv[]={iuX,iuXX,iuXXL,iuXXR};
                    return do_w7s(ns,sv,xxL);
                }
            }
        // ── Red-red violation ────────────────────────────────────────────────
        } else {
            if (xxL) {
                void *iuXR; BSTNodeC *a,*b;
                if (!(iuXR=llx(uXR,&a,&b))) return false;
                if (uXR->weight==0) {
                    BSTNodeC* ns[]={u,uX,uXX,uXR}; void* sv[]={iu,iuX,iuXX,iuXR};
                    return do_blk(ns,sv,xL);
                } else if (xxxL) {
                    BSTNodeC* ns[]={u,uX,uXX}; void* sv[]={iu,iuX,iuXX};
                    return do_rb1(ns,sv,xL);
                } else {
                    BSTNodeC* ns[]={u,uX,uXX,uXXR}; void* sv[]={iu,iuX,iuXX,iuXXR};
                    return do_rb2(ns,sv,xL);
                }
            } else {
                void *iuXL; BSTNodeC *a,*b;
                if (!(iuXL=llx(uXL,&a,&b))) return false;
                if (uXL->weight==0) {
                    BSTNodeC* ns[]={u,uX,uXL,uXX}; void* sv[]={iu,iuX,iuXL,iuXX};
                    return do_blk(ns,sv,xL);
                } else if (!xxxL) {
                    BSTNodeC* ns[]={u,uX,uXX}; void* sv[]={iu,iuX,iuXX};
                    return do_rb1s(ns,sv,xL);
                } else {
                    BSTNodeC* ns[]={u,uX,uXX,uXXL}; void* sv[]={iu,iuX,iuXX,iuXXL};
                    return do_rb2s(ns,sv,xL);
                }
            }
        }
    }

    void fix_all(long key) {
        bool done = false;
        while (!done) done = update_rebalance(key);
    }

public:
    BalancedHintBST() {
        auto* lsent    = make_leaf(KEY_MAX - 1, nullptr);
        auto* rsent    = make_leaf(KEY_MAX,     nullptr);
        auto* real_root= make_int(KEY_MAX, 1, lsent, rsent);
        root_ = make_int(KEY_MAX, 1, real_root, nullptr);
    }

    // Return a list Node* close to key, or nullptr.  Read-only; no LLX needed.
    Node* find_close(long key) const {
        BSTNodeC* cur = root_->left.load(mo_acq);
        while (cur != nullptr && !cur->is_leaf)
            cur = (key < cur->key) ? cur->left.load(mo_acq) : cur->right.load(mo_acq);
        if (cur == nullptr || !cur->is_leaf) return nullptr;
        return cur->list_ptr.load(mo_acq);
    }

    // Insert or update key → list_node in the hint index; rebalance if needed.
    void try_set(long key, Node* list_node) {
        bool rebal = false;
        while (!update_insert(key, list_node, &rebal)) { /* retry on LLX fail */ }
        if (rebal) fix_all(key);
    }

    // Remove key from the hint index.  Best-effort; stale hints are harmless.
    void try_remove(long key) {
        bool rebal = false;
        while (!update_erase(key, &rebal)) { /* retry */ }
        if (rebal) fix_all(key);
    }
};

} // namespace bst_balanced_hint
