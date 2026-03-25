/**
 * Preliminary C++ implementation of chromatic tree using LLX/SCX and DEBRA(+).
 * 
 * Copyright (C) 2017 Trevor Brown
 * This preliminary implementation is CONFIDENTIAL and may not be distributed.
 */

#ifndef NODE_SUPER_H
#define	NODE_SUPER_H

#include <iostream>
#include <iomanip>
#include <atomic>
#include <set>

using namespace std;

template <class K, class V>
class Node_Super {
public:
    K key;
    atomic_uintptr_t left;
    atomic_uintptr_t right;
    int weight;
    V value;
    atomic_bool marked; // might be able to combine this elegantly with scx record pointer... (maybe we can piggyback on the version number mechanism, using the same bit to indicate ver# OR marked)
    uint64_t pad;
    volatile atomic_uintptr_t version; // Addition

    Node_Super() {
        // left blank for efficiency with custom allocator
    }
    Node_Super(const Node_Super& node) {
        // left blank for efficiency with custom allocator
    }

    K getKey() { return key; }
    V getValue() { return value; }
    
    friend ostream& operator<<(ostream& os, const Node_Super<K,V>& obj) {
        ios::fmtflags f( os.flags() );
        os<<"[key="<<obj.key
          <<" weight="<<obj.weight
          <<" marked="<<obj.marked.load(memory_order_relaxed);
        // os<<" scxRecord@0x"<<hex<<(long)(obj.scxRecord.load(memory_order_relaxed));
//        os.flags(f);
        os<<" left@0x"<<hex<<(long)(obj.left.load(memory_order_relaxed));
//        os.flags(f);
        os<<" right@0x"<<hex<<(long)(obj.right.load(memory_order_relaxed));
//        os.flags(f);
        os<<"]"<<"@0x"<<hex<<(long)(&obj);
        os.flags(f);
        return os;
    }
    
    // somewhat slow version that detects cycles in the tree
    void printTreeFile(ostream& os, set< Node_Super<K,V>* > *seen) {
//        os<<"(["<<key<<","<</*(long)(*this)<<","<<*/marked<<","<<scxRecord->state<<"],"<<weight<<",";
        // os<<"(["<<key<<","<<marked.load(memory_order_relaxed)<<"],"<<((SCXRecord<K,V>*) scxRecord.load(memory_order_relaxed))->state.load(memory_order_relaxed)<<",";
        Node_Super<K,V>* __left = (Node_Super<K,V>*) left.load(memory_order_relaxed);
        Node_Super<K,V>* __right = (Node_Super<K,V>*) right.load(memory_order_relaxed);
        if (__left == NULL) {
            os<<"-";
        } else if (seen->find(__left) != seen->end()) {   // for finding cycles
            os<<"!"; // cycle!                          // for finding cycles
        } else {
            seen->insert(__left);
            __left->printTreeFile(os, seen);
        }
        os<<",";
        if (__right == NULL) {
            os<<"-";
        } else if (seen->find(__right) != seen->end()) {  // for finding cycles
            os<<"!"; // cycle!                          // for finding cycles
        } else {
            seen->insert(__right);
            __right->printTreeFile(os, seen);
        }
        os<<")";
    }

    void printTreeFile(ostream& os) {
        set< Node_Super<K,V>* > seen;
        printTreeFile(os, &seen);
    }
    
    // somewhat slow version that detects cycles in the tree
    void printTreeFileWeight(ostream& os, set< Node_Super<K,V>* > *seen) {
//        os<<"(["<<key<<","<</*(long)(*this)<<","<<*/marked<<","<<scxRecord->state<<"],"<<weight<<",";
        os<<"(["<<key<<"],"<<weight<<",";
        Node_Super<K,V>* __left = (Node_Super<K,V>*) left.load(memory_order_relaxed);
        Node_Super<K,V>* __right = (Node_Super<K,V>*) right.load(memory_order_relaxed);
        if (__left == NULL) {
            os<<"-";
        } else if (seen->find(__left) != seen->end()) {   // for finding cycles
            os<<"!"; // cycle!                          // for finding cycles
        } else {
            seen->insert(__left);
            __left->printTreeFileWeight(os, seen);
        }
        os<<",";
        if (__right == NULL) {
            os<<"-";
        } else if (seen->find(__right) != seen->end()) {  // for finding cycles
            os<<"!"; // cycle!                          // for finding cycles
        } else {
            seen->insert(__right);
            __right->printTreeFileWeight(os, seen);
        }
        os<<")";
    }

    void printTreeFileWeight(ostream& os) {
        set< Node_Super<K,V>* > seen;
        printTreeFileWeight(os, &seen);
    }

};

template<class K>
class alignas(32) Version {
    public:
        atomic_uintptr_t left;
        atomic_uintptr_t right;
        atomic_uintptr_t status;
        K key;
        int sum;
        bool isSecondCas;

        Version() {}
};

#endif