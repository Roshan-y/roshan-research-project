/**
 * Preliminary C++ implementation of chromatic tree using LLX/SCX and DEBRA(+).
 * 
 * Copyright (C) 2017 Trevor Brown
 * This preliminary implementation is CONFIDENTIAL and may not be distributed.
 */

#ifndef NODE_H
#define	NODE_H

#include <iostream>
#include <iomanip>
#include <atomic>
#include <set>
#include "../../common/node_types.h"
//#include "scxrecord.h"
using namespace std;

// Addition
// TODO: Credit to _____ for code


template <class K, class V>
class Node: public Node_Super<K,V> {
public:
    atomic_uintptr_t scxRecord;
};

#endif	/* NODE_H */

