#ifndef CHROMATIC_ADAPTER_H
#define CHROMATIC_ADAPTER_H

#include <iostream>
#include "errors.h"
#ifdef USE_TREE_STATS
#   define TREE_STATS_BYTES_AT_DEPTH
#   include "tree_stats.h"
#endif
#include "./bst/vcas_bst_impl.h"
#include "./recordmgr/reclaimer_debra.h"

#define RECORD_MANAGER_T RecVcas::record_manager<DebraVcas::reclaimer_debra<K>, Alloc, Pool, vcas_bst_ns::Node<K,V>>
#define DATA_STRUCTURE_T vcas_bst_ns::vcas_bst<K, V, std::less<K>, RECORD_MANAGER_T>

template <typename K, typename V, class Reclaim = DebraVcas::reclaimer_debra<K>, class Alloc = allocator_new<K>, class Pool = pool_none<K>>
class ds_adapter {
private:
    const V NO_VALUE;
    DATA_STRUCTURE_T * const ds;

public:
    ds_adapter(const int NUM_THREADS,
               const K& KEY_RESERVED,
               const K& KEY_MAX,
               const V& VALUE_RESERVED,
               Random64 * const unused2)
    : NO_VALUE(VALUE_RESERVED)
    , ds(new DATA_STRUCTURE_T(KEY_RESERVED, NO_VALUE, NUM_THREADS, SIGQUIT))
    {}
    ~ds_adapter() {
        delete ds;
    }

    V getNoValue() {
        return NO_VALUE;
    }

    void initThread(const int tid) {
        ds->initThread(tid);
    }
    void deinitThread(const int tid) {
        ds->deinitThread(tid);
    }

    bool contains(const int tid, const K& key) {
        return ds->contains(tid, key);
    }
    V insert(const int tid, const K& key, const V& val) {
        return ds->insert(tid, key, val);
    }
    V insertIfAbsent(const int tid, const K& key, const V& val) {
        return ds->insertIfAbsent(tid, key, val);
    }
    V erase(const int tid, const K& key) {
        return ds->erase(tid, key).first;
    }
    V find(const int tid, const K& key) {
        return ds->find(tid, key).first;
    }
    int rangeQuery(const int tid, const K& lo, const K& hi, K * const resultKeys, V * const resultValues) {
        bool unused = false;
        return ds->rangeQuery(tid, lo, hi, resultKeys, resultValues, unused);
    }
    int getRank(const int tid, const K& key, K * const resultKeys, V * const resultValues) {
        return ds->getRank(tid, key, resultKeys, resultValues);
    }
    K selectItem(const int tid, int rank) {
        setbench_error("not implemented");
    }
    void printSummary() {
        // ds->printExpectedSum();
        // auto recmgr = ds->debugGetRecordMgr();
        // recmgr->printStatus();
    }
    bool validateStructure() {
        return 1; //ds->validate(0, false);
    }
    void printObjectSizes() {
        std::cout<<"sizes: node="
                 <<(sizeof(vcas_bst_ns::Node<K,V>))
                 <<std::endl;
    }
    // try to clean up: must only be called by a single thread as part of the test harness!
    void debugGCSingleThreaded() {
        // ds->debugGetRecordMgr()->debugGCSingleThreaded();
    }

#ifdef USE_TREE_STATS
    // class NodeHandler {
    // public:
    //     typedef Node<K,V> * NodePtrType;
    //     K minKey;
    //     K maxKey;

    //     NodeHandler(const K& _minKey, const K& _maxKey) {
    //         minKey = _minKey;
    //         maxKey = _maxKey;
    //     }

    //     class ChildIterator {
    //     private:
    //         bool leftDone;
    //         bool rightDone;
    //         NodePtrType node; // node being iterated over
    //     public:
    //         ChildIterator(NodePtrType _node) {
    //             node = _node;
    //             leftDone = (node->left.load() == (uintptr_t) NULL);
    //             rightDone = (node->right.load() == (uintptr_t) NULL);
    //         }
    //         bool hasNext() {
    //             return !(leftDone && rightDone);
    //         }
    //         NodePtrType next() {
    //             if (!leftDone) {
    //                 leftDone = true;
    //                 return (NodePtrType) node->left.load();
    //             }
    //             if (!rightDone) {
    //                 rightDone = true;
    //                 return (NodePtrType) node->right.load();
    //             }
    //             setbench_error("ERROR: it is suspected that you are calling ChildIterator::next() without first verifying that it hasNext()");
    //         }
    //     };

    //     static bool isLeaf(NodePtrType node) {
    //         return (node->left.load() == (uintptr_t) NULL) && (node->right.load() == (uintptr_t) NULL);
    //     }
    //     static size_t getNumChildren(NodePtrType node) { // TODO
    //         return (node->left.load() != (uintptr_t) NULL) + (node->right.load() != (uintptr_t) NULL);
    //     }
    //     static size_t getNumKeys(NodePtrType node) {
    //         if (!isLeaf(node)) return 0;
    //         //if (node->key == KEY_RESERVED) return 0;
    //         return 1;
    //     }
    //     static size_t getSumOfKeys(NodePtrType node) {
    //         if (!isLeaf(node)) return 0;
    //         //if (node->key == KEY_RESERVED) return 0;
    //         return (size_t) node->key;
    //     }
    //     static ChildIterator getChildIterator(NodePtrType node) {
    //         return ChildIterator(node);
    //     }
    //     static size_t getSizeInBytes(NodePtrType node) { return sizeof(*node); }
    // };
    // TreeStats<NodeHandler> * createTreeStats(const K& _minKey, const K& _maxKey) {
    //     auto lnode = (Node<K,V> *) ds->getRoot()->left.load();
    //     auto llnode = (Node<K,V> *) lnode->left.load();
    //     return new TreeStats<NodeHandler>(new NodeHandler(_minKey, _maxKey), llnode, true);
    // }
#endif
};

#endif
