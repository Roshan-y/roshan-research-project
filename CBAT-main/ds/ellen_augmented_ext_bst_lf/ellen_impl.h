/*   
 *   File: bst_ellen.c
 *   Author: Tudor David <tudor.david@epfl.ch>
 *   Description: non-blocking binary search tree
 *      based on "Non-blocking Binary Search Trees"
 *      F. Ellen et al., PODC 2010
 *   bst_ellen.c is part of ASCYLIB
 *
 * Copyright (c) 2014 Vasileios Trigonakis <vasileios.trigonakis@epfl.ch>,
 * 	     	      Tudor David <tudor.david@epfl.ch>
 *	      	      Distributed Programming Lab (LPD), EPFL
 *
 * ASCYLIB is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, version 2
 * of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
/* 
 * File:   ellen.h
 * Author: Trevor Brown
 *
 * Substantial improvements to interface, memory reclamation and bug fixing.
 *
 * Created on June 1, 2017, 3:56 PM
 */
/**
 *  Copyright (C) 2026 Concurrent Balanced Augmented Trees Authors
 *  Added augmentation code
*/

#ifndef ELLEN_H
#define ELLEN_H

#include "record_manager.h"
#include <stack>
#include <vector>

#define STATE_CLEAN 0
#define STATE_DFLAG 1
#define STATE_IFLAG 2
#define STATE_MARK 3

#define GETFLAG(ptr) (((uint64_t) (ptr)) & 3)
#define FLAG(ptr, flag) (info_t<skey_t, sval_t> *) ((((uint64_t) (ptr)) & 0xfffffffffffffffc) | (flag))
#define UNFLAG(ptr) (info_t<skey_t, sval_t> *) (((uint64_t) (ptr)) & 0xfffffffffffffffc)

template <typename skey_t, typename sval_t>
union info_t; 

template <typename skey_t, typename sval_t>
struct node_t;

template <typename skey_t>
struct version_t;

template <typename skey_t, typename sval_t>
struct iinfo_t {
    node_t<skey_t, sval_t> * p;
    node_t<skey_t, sval_t> * new_internal;
    node_t<skey_t, sval_t> * l;
};

template <typename skey_t, typename sval_t>
struct dinfo_t {
    node_t<skey_t, sval_t> * gp;
    node_t<skey_t, sval_t> * p;
    node_t<skey_t, sval_t> * l;
    info_t<skey_t, sval_t> * pupdate;
};

template <typename skey_t, typename sval_t>
union info_t {
    iinfo_t<skey_t, sval_t> iinfo;
    dinfo_t<skey_t, sval_t> dinfo;
#if defined(LARGE_DES)
    uint8_t padding[128 - 4*sizeof(void *)];
#else
    uint8_t padding[64];
#endif
};

template <typename skey_t, typename sval_t>
struct node_t {
    skey_t key;
    sval_t value;
    info_t<skey_t, sval_t> * volatile update;
    version_t<skey_t> * volatile version;
    node_t<skey_t, sval_t> * volatile left;
    node_t<skey_t, sval_t> * volatile right;
#ifdef USE_PADDING
    char pad[128 - sizeof(key) - sizeof(value) - sizeof(update) - sizeof(left) - sizeof(right) - sizeof(version)];
#endif
};

template <typename skey_t>
struct version_t {
    version_t<skey_t> * volatile left;
    version_t<skey_t> * volatile right;
    skey_t key;
    int sum;
#ifdef USE_PADDING
    char pad[64 - sizeof(left) - sizeof(right) - sizeof(key) - sizeof(sum)];
#endif
};

template <typename skey_t, typename sval_t, class RecMgr>
class ellen {
private:
PAD;
    const unsigned int idx_id;
PAD;
    node_t<skey_t, sval_t> * root;
PAD;
    const int NUM_THREADS;
    const skey_t KEY_MIN;
    const skey_t KEY_MAX;
    const sval_t NO_VALUE;
PAD;
    RecMgr * const recmgr;
PAD;
    int init[MAX_THREADS_POW2] = {0,}; // this suffers from false sharing, but is only touched once per thread! so no worries.
    unsigned long long numPropagates[MAX_THREADS_POW2 * PREFETCH_SIZE_WORDS] = {};
    unsigned long long numLevelsPropagated[MAX_THREADS_POW2 * PREFETCH_SIZE_WORDS] = {};
    unsigned long long totCASsAttempted[MAX_THREADS_POW2 * PREFETCH_SIZE_WORDS] = {};
PAD;

    bool refresh(const int tid, node_t<skey_t, sval_t>* node, std::vector<version_t<skey_t>*> &toRetire);
    void propagate(const int tid, skey_t key, std::stack<node_t<skey_t, sval_t>*> stack);
    bool bst_cas_child(const int tid, node_t<skey_t, sval_t> * parent, node_t<skey_t, sval_t> * old, node_t<skey_t, sval_t> * nnode);
    void bst_help(const int tid, info_t<skey_t, sval_t>* u);
    void bst_help_marked(const int tid, info_t<skey_t, sval_t>* op);
    bool bst_help_delete(const int tid, info_t<skey_t, sval_t>* op);
    void bst_help_insert(const int tid, info_t<skey_t, sval_t> * op);

    node_t<skey_t, sval_t> * create_node(const int tid, skey_t key, sval_t value, node_t<skey_t, sval_t> * left, node_t<skey_t, sval_t> * right, version_t<skey_t> * version) {
        auto result = recmgr->template allocate<node_t<skey_t, sval_t>>(tid);
        if (result == NULL) setbench_error("out of memory");
        result->key = key;
        result->value = value;
        result->update = NULL;
        result->left = left;
        result->right = right;
        result->version = version;
        return result;
    }

    version_t<skey_t> * create_version(const int tid, skey_t key, version_t<skey_t> * left, version_t<skey_t> * right, int sum) {
        auto result = recmgr->template allocate<version_t<skey_t>>(tid);
        if (result == NULL) setbench_error("out of memory");
        result->key = key;
        result->left = left;
        result->right = right;
        result->sum = sum;
        return result;
    }
    
    info_t<skey_t, sval_t> * create_iinfo_t(const int tid, node_t<skey_t, sval_t> * p, node_t<skey_t, sval_t> * ni, node_t<skey_t, sval_t> * l) {
        auto result = recmgr->template allocate<info_t<skey_t, sval_t>>(tid);
        if (result == NULL) setbench_error("out of memory");
        result->iinfo.p = p;
        result->iinfo.new_internal = ni;
        result->iinfo.l = l;
        return result;
    }

    info_t<skey_t, sval_t> * create_dinfo_t(const int tid, node_t<skey_t, sval_t> * gp, node_t<skey_t, sval_t> * p, node_t<skey_t, sval_t> * l, info_t<skey_t, sval_t> * u) {
        auto result = recmgr->template allocate<info_t<skey_t, sval_t>>(tid);
        if (result == NULL) setbench_error("out of memory");
        result->dinfo.gp = gp;
        result->dinfo.p = p;
        result->dinfo.l = l;
        result->dinfo.pupdate = u;
        return result;
    }
public:

    ellen(const int _NUM_THREADS, const skey_t& _KEY_MIN, const skey_t& _KEY_MAX, const sval_t& _VALUE_RESERVED, unsigned int id)
    : idx_id(id), NUM_THREADS(_NUM_THREADS), KEY_MIN(_KEY_MIN), KEY_MAX(_KEY_MAX), NO_VALUE(_VALUE_RESERVED), recmgr(new RecMgr(NUM_THREADS)) {
        const int tid = 0;
        initThread(tid);

        recmgr->endOp(tid); // enter an initial quiescent state.

        auto v1 = create_version(tid, KEY_MAX, NULL, NULL, 0);
        auto v2 = create_version(tid, KEY_MAX, NULL, NULL, 0);
        auto i1 = create_node(tid, KEY_MAX, NO_VALUE, NULL, NULL, v1);
        auto i2 = create_node(tid, KEY_MAX, NO_VALUE, NULL, NULL, v2);
        auto vr = create_version(tid, KEY_MAX, v1, v2, 0);
        root = create_node(tid, KEY_MAX, NO_VALUE, i1, i2, vr);
    }

    ~ellen() {
        unsigned long long numProp = 0;
        unsigned long long totRef = 0;
        unsigned long long totCAS = 0;
        for(int i = 0; i < recmgr->NUM_PROCESSES; i++) {
            numProp+=numPropagates[i * PREFETCH_SIZE_WORDS];
            totRef+=numLevelsPropagated[i * PREFETCH_SIZE_WORDS];
            totCAS+=totCASsAttempted[i * PREFETCH_SIZE_WORDS];
        }
        COUTATOMIC("Avg_nodes_per_propagate="<<(((long double)totRef) / numProp)<<std::endl);
        COUTATOMIC("Avg_total_cas_per_propagate="<<(((long double)totCAS) / numProp)<<std::endl);
        recmgr->printStatus();
        delete recmgr;
    }
    
    void printTree(node_t<skey_t, sval_t> * node, int depth) {
        //if (depth > 5) return;
        std::cout<<"depth="<<depth<<" key="<<node->key<<std::endl;
        if (node->left) printTree(node->left, depth+1);
        if (node->right) printTree(node->right, depth+1);
    }
    void printTree() {
        printTree(root, 0);
    }

    void initThread(const int tid) {
        if (init[tid]) return;
        else init[tid] = !init[tid];
        recmgr->initThread(tid);
    }

    void deinitThread(const int tid) {
        if (!init[tid]) return;
        else init[tid] = !init[tid];
        recmgr->deinitThread(tid);
    }

    int bst_rangequery(const int tid, skey_t lo, skey_t hi);
    int do_bst_rangequery(const int tid, skey_t lo, skey_t hi, version_t<skey_t>* curr);
    bool compareKeys(const skey_t& a, bool aIsLo, const skey_t& b, bool bIsLo);
    sval_t bst_find(const int tid, skey_t key);
    sval_t bst_insert(const int tid, skey_t key, sval_t value);
    sval_t bst_delete(const int tid, skey_t key);
    
    node_t<skey_t, sval_t> * get_root(){
        return root; 
    }
    
    RecMgr * debugGetRecMgr() {
        return recmgr;
    }    
};

template <typename skey_t, typename sval_t, class RecMgr>
sval_t ellen<skey_t, sval_t, RecMgr>::bst_find(const int tid, skey_t key) {
    auto guard = recmgr->getGuard(tid, true);
    
    auto l = root->left;
    while (l->left) l = (key < l->key) ? l->left : l->right;
    return (l->key == key) ? l->value : NO_VALUE;
}

template <typename skey_t, typename sval_t, class RecMgr>
int ellen<skey_t, sval_t, RecMgr>::bst_rangequery(const int tid, skey_t lo, skey_t hi) {
    auto guard = recmgr->getGuard(tid, true);
    return do_bst_rangequery(tid, lo, hi, root->version);
}

template <typename skey_t, typename sval_t, class RecMgr>
int ellen<skey_t, sval_t, RecMgr>::do_bst_rangequery(const int tid, skey_t lo, skey_t hi, version_t<skey_t>* curr) {
    if (lo == KEY_MAX && hi == KEY_MAX) {
        return curr->sum;
    }
    if (curr->left == NULL) { // Leaf
        return (curr->key != KEY_MAX && !compareKeys(curr->key, false, lo, true) && !compareKeys(hi, false, curr->key, false)) ? 1 : 0;
    }
    if (curr->key == KEY_MAX || compareKeys(hi, false, curr->key, false)) {
        return do_bst_rangequery(tid, lo, hi, curr->left);
    }
    if (compareKeys(curr->key, false, lo, true)) {
        return do_bst_rangequery(tid, lo, hi, curr->right);
    }
    return do_bst_rangequery(tid, lo, KEY_MAX, curr->left) + do_bst_rangequery(tid, KEY_MAX, hi, curr->right);
}

template <typename skey_t, typename sval_t, class RecMgr>
bool ellen<skey_t, sval_t, RecMgr>::compareKeys(const skey_t& a, bool aIsLo, const skey_t& b, bool bIsLo) {
    if (a == KEY_MAX) {
        return aIsLo ? 1 : 0;
    } else if (b == KEY_MAX) {
        return bIsLo ? 0 : 1;
    }
    return a < b;
}

template <typename skey_t, typename sval_t, class RecMgr>
sval_t ellen<skey_t, sval_t, RecMgr>::bst_insert(const int tid, const skey_t key, const sval_t value) {
    while (1) {
        std::stack<node_t<skey_t, sval_t>*> stack;
        stack.push(root);
        auto guard = recmgr->getGuard(tid);
        
        auto p = root;
        auto pupdate = p->update;
        SOFTWARE_BARRIER;
        auto l = p->left;
        while (l->left) {
            stack.push(l);
            p = l;
            pupdate = p->update;
            SOFTWARE_BARRIER;
            l = (key < l->key) ? l->left : l->right;
        }
        if (l->key == key) {
            propagate(tid, key, stack);
            return l->value;
        }
        if (GETFLAG(pupdate) != STATE_CLEAN) {
            bst_help(tid, pupdate);
        } else {
            auto node_ver = create_version(tid, key, NULL, NULL, 1);
            auto sibling_ver = create_version(tid, l->key, NULL, NULL, l->key == KEY_MAX ? 0 : 1);
            auto internal_ver = (key < l->key)
                    ? create_version(tid, l->key, node_ver, sibling_ver, node_ver->sum + sibling_ver->sum)
                    : create_version(tid, key, sibling_ver, node_ver, node_ver->sum + sibling_ver->sum);

            auto new_node = create_node(tid, key, value, NULL, NULL, node_ver);
            auto new_sibling = create_node(tid, l->key, l->value, NULL, NULL, sibling_ver);
            auto new_internal = (key < l->key)
                    ? create_node(tid, l->key, NO_VALUE, new_node, new_sibling, internal_ver)
                    : create_node(tid, key, NO_VALUE, new_sibling, new_node, internal_ver);
            auto op = create_iinfo_t(tid, p, new_internal, l);
            auto result = CASV(&p->update, pupdate, FLAG(op, STATE_IFLAG));
            if (result == pupdate) {
                bst_help_insert(tid, op);
                propagate(tid, key, stack);
                return NO_VALUE;
            } else {
                recmgr->deallocate(tid, new_node);
                recmgr->deallocate(tid, new_sibling);
                recmgr->deallocate(tid, new_internal);
                recmgr->deallocate(tid, op);
                bst_help(tid, result);
            }
        }
    }
}

template <typename skey_t, typename sval_t, class RecMgr>
sval_t ellen<skey_t, sval_t, RecMgr>::bst_delete(const int tid, skey_t key) {
    while (1) {
        std::stack<node_t<skey_t, sval_t>*> stack;
        stack.push(root);
        auto guard = recmgr->getGuard(tid);
        
        node_t<skey_t, sval_t> * gp = NULL;
        info_t<skey_t, sval_t> * gpupdate = NULL;
        auto p = root;
        auto pupdate = p->update;
        SOFTWARE_BARRIER;
        auto l = p->left;
        while (l->left) {
            stack.push(p);
            gp = p;
            p = l;
            gpupdate = pupdate;
            pupdate = p->update;
            SOFTWARE_BARRIER;
            l = (key < l->key) ? l->left : l->right;
        }
        if (l->key != key) {
            propagate(tid, key, stack);
            return NO_VALUE;
        }
        auto found_value = l->value;
        if (GETFLAG(gpupdate) != STATE_CLEAN) {
            bst_help(tid, gpupdate);
        } else if (GETFLAG(pupdate) != STATE_CLEAN) {
            bst_help(tid, pupdate);
        } else {
            auto op = create_dinfo_t(tid, gp, p, l, pupdate);
            auto result = CASV(&gp->update, gpupdate, FLAG(op, STATE_DFLAG));
            if (result == gpupdate) {
                if (bst_help_delete(tid, op)){
                    propagate(tid, key, stack);
                    return found_value;
                }
            } else {
                recmgr->deallocate(tid, op);
                bst_help(tid, result);
            }
        }
    }
}

template <typename skey_t, typename sval_t, class RecMgr>
bool ellen<skey_t, sval_t, RecMgr>::refresh(const int tid, node_t<skey_t, sval_t>* node, std::vector<version_t<skey_t>*> &toRetire) {
    auto old = node->version;

    auto left = node->left;
    auto left_version = left->version;

    auto right = node->right;
    auto right_version = right->version;

    auto new_version = create_version(tid, node->key, left_version, right_version, left_version->sum + right_version->sum);

    bool res = CASB(&node->version, old, new_version);
    totCASsAttempted[tid * PREFETCH_SIZE_WORDS]++;
    if (res) {
        toRetire.push_back(old);
    } else {
        recmgr->deallocate(tid, new_version);
    }
    return res;
}

template <typename skey_t, typename sval_t, class RecMgr>
void ellen<skey_t, sval_t, RecMgr>::propagate(const int tid, skey_t key, std::stack<node_t<skey_t, sval_t>*> stack) {
    std::vector<version_t<skey_t>*> toRetire;
    toRetire.reserve(50);
    numPropagates[tid * PREFETCH_SIZE_WORDS]++;
    while (!stack.empty())
    {
        node_t<skey_t, sval_t>* n = stack.top();
        stack.pop();
        numLevelsPropagated[tid * PREFETCH_SIZE_WORDS]++;

        if (!refresh(tid, n, toRetire)) {
            refresh(tid, n, toRetire);
        }
    }
    for (int i = 0; i < toRetire.size(); i++) {
        recmgr->retire(tid, toRetire[i]);
    }
}

template <typename skey_t, typename sval_t, class RecMgr>
bool ellen<skey_t, sval_t, RecMgr>::bst_cas_child(const int tid, node_t<skey_t, sval_t> * parent, node_t<skey_t, sval_t> * old, node_t<skey_t, sval_t> * nnode) {
    if (old == parent->left) {
        return CASB(&parent->left, old, nnode);
    } else if (old == parent->right) {
        return CASB(&parent->right, old, nnode);
    } else {
        return false;
    }
}

template <typename skey_t, typename sval_t, class RecMgr>
void ellen<skey_t, sval_t, RecMgr>::bst_help_insert(const int tid, info_t<skey_t, sval_t> * op) {
    if (bst_cas_child(tid, op->iinfo.p, op->iinfo.l, op->iinfo.new_internal)) {
        recmgr->retire(tid, op->iinfo.l);
    }
    if (CASB(&op->iinfo.p->update, FLAG(op, STATE_IFLAG), FLAG(op, STATE_CLEAN))) {
        recmgr->retire(tid, op);
    }
}
 
template <typename skey_t, typename sval_t, class RecMgr>
bool ellen<skey_t, sval_t, RecMgr>::bst_help_delete(const int tid, info_t<skey_t, sval_t> * op) {
    auto result = CASV(&op->dinfo.p->update, op->dinfo.pupdate, FLAG(op, STATE_MARK));
    if ((result == op->dinfo.pupdate) || (result == FLAG(op, STATE_MARK))) {
        bst_help_marked(tid, op);
        return true;
    } else {
        bst_help(tid, result);
        if (CASB(&op->dinfo.gp->update, FLAG(op, STATE_DFLAG), FLAG(op, STATE_CLEAN))) {
            recmgr->retire(tid, op);
        }
        return false;
    }
}

template <typename skey_t, typename sval_t, class RecMgr>
void ellen<skey_t, sval_t, RecMgr>::bst_help_marked(const int tid, info_t<skey_t, sval_t> * op) {
    node_t<skey_t, sval_t> * other;
    if (op->dinfo.p->right == op->dinfo.l) {
        other = op->dinfo.p->left;
    } else {
        other = op->dinfo.p->right;
    }
    if (bst_cas_child(tid, op->dinfo.gp, op->dinfo.p, other)) {
        recmgr->retire(tid, op->dinfo.l);
        recmgr->retire(tid, op->dinfo.p);
    }
    if (CASB(&op->dinfo.gp->update, FLAG(op, STATE_DFLAG), FLAG(op, STATE_CLEAN))) {
        recmgr->retire(tid, op);
    }
}

template <typename skey_t, typename sval_t, class RecMgr>
void ellen<skey_t, sval_t, RecMgr>::bst_help(const int tid, info_t<skey_t, sval_t> * u) {
    if (GETFLAG(u) == STATE_DFLAG) {
        bst_help_delete(tid, UNFLAG(u));
    } else if (GETFLAG(u) == STATE_IFLAG) {
        bst_help_insert(tid, UNFLAG(u));
    } else if (GETFLAG(u) == STATE_MARK) {
        bst_help_marked(tid, UNFLAG(u));
    }
}

#endif /* ELLEN_H */