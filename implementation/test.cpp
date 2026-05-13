// Correctness tests for all four BST+List variants.
//
// Compile:
//   g++ -std=c++17 -O2 -pthread -o test implementation/test.cpp
// Run:
//   ./test

#include "bst_list.h"
#include "bst_list_balanced.h"
#include "bst_list_pragmatic.h"
#include "bst_list_balanced_pragmatic.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

// ============================================================
// Templated correctness tests — run on any type S that exposes:
//   bool insert(long), bool remove(long),
//   long min(), long max(), long extract_min(), long extract_max()
// ============================================================

template <class S>
static void t_empty(const char* name) {
    S s;
    assert(s.min()         == NO_KEY);
    assert(s.max()         == NO_KEY);
    assert(s.extract_min() == NO_KEY);
    assert(s.extract_max() == NO_KEY);
    printf("  [%s] test_empty                      PASS\n", name);
}

template <class S>
static void t_single(const char* name) {
    S s;
    assert( s.insert(42));
    assert(!s.insert(42));     // duplicate
    assert(s.min() == 42);
    assert(s.max() == 42);
    assert( s.remove(42));
    assert(!s.remove(42));     // already gone
    assert(s.min() == NO_KEY);
    assert(s.max() == NO_KEY);
    printf("  [%s] test_single                     PASS\n", name);
}

template <class S>
static void t_ordered_inserts(const char* name) {
    S s;
    for (int i = 1; i <= 100; ++i) assert(s.insert(i));
    assert(s.min() == 1);
    assert(s.max() == 100);
    for (int i = 1; i <= 100; ++i) assert(s.remove(i));
    assert(s.min() == NO_KEY);
    assert(s.max() == NO_KEY);
    printf("  [%s] test_ordered_inserts            PASS\n", name);
}

template <class S>
static void t_reverse_inserts(const char* name) {
    S s;
    for (int i = 100; i >= 1; --i) assert(s.insert(i));
    assert(s.min() == 1);
    assert(s.max() == 100);
    printf("  [%s] test_reverse_inserts            PASS\n", name);
}

template <class S>
static void t_mixed_ops(const char* name) {
    S s;
    std::vector<int> keys = {5, 3, 8, 1, 9, 2, 7, 4, 6, 10};
    for (int k : keys) s.insert(k);
    assert(s.min() == 1);
    assert(s.max() == 10);

    s.remove(1);  assert(s.min() == 2);
    s.remove(10); assert(s.max() == 9);
    s.insert(0);  assert(s.min() == 0);
    s.insert(11); assert(s.max() == 11);
    printf("  [%s] test_mixed_ops                  PASS\n", name);
}

// ---------- concurrent: disjoint ranges ----------
template <class S>
static void t_concurrent(const char* name, int n_threads = 8,
                          int per_thread = 1000) {
    S s;
    auto worker = [&](int tid) {
        int base = tid * per_thread;
        for (int i = 0; i < per_thread; ++i) s.insert(base + i);
        for (int i = 0; i < per_thread; ++i) s.remove(base + i);
    };
    std::vector<std::thread> ths;
    ths.reserve(n_threads);
    for (int t = 0; t < n_threads; ++t) ths.emplace_back(worker, t);
    for (auto& th : ths) th.join();
    assert(s.min() == NO_KEY);
    assert(s.max() == NO_KEY);
    printf("  [%s] test_concurrent(%d thr)         PASS\n", name, n_threads);
}

// ---------- concurrent: contended small range ----------
template <class S>
static void t_concurrent_contended(const char* name, int n_threads = 8) {
    constexpr int OPS = 2000, KEY_RANGE = 64;
    S s;
    auto worker = [&](int tid) {
        std::mt19937 rng(tid * 1234567u);
        std::uniform_int_distribution<int> kd(0, KEY_RANGE - 1), od(0, 1);
        for (int i = 0; i < OPS; ++i) {
            if (od(rng) == 0) s.insert(kd(rng));
            else              s.remove(kd(rng));
        }
    };
    std::vector<std::thread> ths;
    ths.reserve(n_threads);
    for (int t = 0; t < n_threads; ++t) ths.emplace_back(worker, t);
    for (auto& th : ths) th.join();
    long mn = s.min(), mx = s.max();
    assert((mn == NO_KEY) == (mx == NO_KEY));
    if (mn != NO_KEY) assert(mn <= mx);
    printf("  [%s] test_concurrent_contended(%d)   PASS\n", name, n_threads);
}

// ---------- extract_min / extract_max sequential ----------
template <class S>
static void t_extract_seq(const char* name) {
    // extract_min on ordered sequence
    {
        S s;
        for (int i = 1; i <= 10; ++i) s.insert(i);
        for (int i = 1; i <= 10; ++i) { long v = s.extract_min(); assert(v == i); }
        assert(s.extract_min() == NO_KEY);
    }
    // extract_max on ordered sequence
    {
        S s;
        for (int i = 1; i <= 10; ++i) s.insert(i);
        for (int i = 10; i >= 1; --i) { long v = s.extract_max(); assert(v == i); }
        assert(s.extract_max() == NO_KEY);
    }
    // extract_min skipping removed nodes
    {
        S s;
        for (int i = 1; i <= 5; ++i) s.insert(i);
        s.remove(2); s.remove(4);
        assert(s.extract_min() == 1);
        assert(s.extract_min() == 3);
        assert(s.extract_min() == 5);
        assert(s.extract_min() == NO_KEY);
    }
    // extract_max skipping removed nodes
    {
        S s;
        for (int i = 1; i <= 5; ++i) s.insert(i);
        s.remove(2); s.remove(4);
        assert(s.extract_max() == 5);
        assert(s.extract_max() == 3);
        assert(s.extract_max() == 1);
        assert(s.extract_max() == NO_KEY);
    }
    printf("  [%s] test_extract_sequential         PASS\n", name);
}

// ---------- concurrent extract_min: all keys extracted exactly once ----------
template <class S>
static void t_concurrent_extract_min(const char* name,
                                     int n_ins = 4, int n_ext = 4,
                                     int per = 500) {
    int total = n_ins * per;
    S s;
    std::atomic<int> ins_done{0};
    std::vector<std::vector<long>> extracted(n_ext);

    std::vector<std::thread> ths;
    ths.reserve(n_ins + n_ext);

    for (int t = 0; t < n_ins; ++t)
        ths.emplace_back([&, t]() {
            for (int i = 0; i < per; ++i) s.insert(t * per + i);
            ins_done.fetch_add(1, std::memory_order_release);
        });

    for (int t = 0; t < n_ext; ++t)
        ths.emplace_back([&, t]() {
            while (true) {
                long v = s.extract_min();
                if (v == NO_KEY) {
                    if (ins_done.load(std::memory_order_acquire) == n_ins) break;
                    continue;
                }
                extracted[t].push_back(v);
            }
        });

    for (auto& th : ths) th.join();

    // Drain remainder
    std::vector<long> all;
    for (auto& vec : extracted) all.insert(all.end(), vec.begin(), vec.end());
    { long v; while ((v = s.extract_min()) != NO_KEY) all.push_back(v); }
    std::sort(all.begin(), all.end());

    assert((int)all.size() == total);
    for (int i = 0; i < total; ++i) assert(all[i] == i);
    printf("  [%s] test_concurrent_extract_min     PASS\n", name);
}

// ---------- concurrent extract_max: all keys extracted exactly once ----------
template <class S>
static void t_concurrent_extract_max(const char* name,
                                     int n_ins = 4, int n_ext = 4,
                                     int per = 500) {
    int total = n_ins * per;
    S s;
    std::atomic<int> ins_done{0};
    std::vector<std::vector<long>> extracted(n_ext);

    std::vector<std::thread> ths;
    ths.reserve(n_ins + n_ext);

    for (int t = 0; t < n_ins; ++t)
        ths.emplace_back([&, t]() {
            for (int i = 0; i < per; ++i) s.insert(t * per + i);
            ins_done.fetch_add(1, std::memory_order_release);
        });

    for (int t = 0; t < n_ext; ++t)
        ths.emplace_back([&, t]() {
            while (true) {
                long v = s.extract_max();
                if (v == NO_KEY) {
                    if (ins_done.load(std::memory_order_acquire) == n_ins) break;
                    continue;
                }
                extracted[t].push_back(v);
            }
        });

    for (auto& th : ths) th.join();

    std::vector<long> all;
    for (auto& vec : extracted) all.insert(all.end(), vec.begin(), vec.end());
    { long v; while ((v = s.extract_max()) != NO_KEY) all.push_back(v); }
    std::sort(all.begin(), all.end());

    assert((int)all.size() == total);
    for (int i = 0; i < total; ++i) assert(all[i] == i);
    printf("  [%s] test_concurrent_extract_max     PASS\n", name);
}

// ---------- mixed concurrent stress ----------
template <class S>
static void t_concurrent_mixed(const char* name, int n_threads = 8) {
    constexpr int OPS = 1000, KEY_RANGE = 64;
    S s;
    std::vector<long> emin_keys, emax_keys;
    std::mutex mu;

    auto worker = [&](int tid) {
        std::mt19937 rng(tid * 9876543u);
        std::uniform_int_distribution<int> kd(0, KEY_RANGE - 1), od(0, 3);
        std::vector<long> lmin, lmax;
        for (int i = 0; i < OPS; ++i) {
            int key = kd(rng), op = od(rng);
            switch (op) {
                case 0: s.insert(key);    break;
                case 1: s.remove(key);    break;
                case 2: { long v = s.extract_min(); if (v != NO_KEY) lmin.push_back(v); break; }
                case 3: { long v = s.extract_max(); if (v != NO_KEY) lmax.push_back(v); break; }
            }
        }
        std::lock_guard<std::mutex> lk(mu);
        emin_keys.insert(emin_keys.end(), lmin.begin(), lmin.end());
        emax_keys.insert(emax_keys.end(), lmax.begin(), lmax.end());
    };

    std::vector<std::thread> ths;
    ths.reserve(n_threads);
    for (int t = 0; t < n_threads; ++t) ths.emplace_back(worker, t);
    for (auto& th : ths) th.join();

    for (long v : emin_keys) assert(v >= 0 && v < KEY_RANGE);
    for (long v : emax_keys) assert(v >= 0 && v < KEY_RANGE);
    long mn = s.min(), mx = s.max();
    assert((mn == NO_KEY) == (mx == NO_KEY));
    if (mn != NO_KEY) assert(mn <= mx);
    printf("  [%s] test_concurrent_mixed(%d thr)   PASS\n", name, n_threads);
}

// ---------- high thread count stress (32 threads) ----------
template <class S>
static void t_high_thread_stress(const char* name) {
    constexpr int N = 32, PER = 500;
    S s;
    auto worker = [&](int tid) {
        int base = tid * PER;
        for (int i = 0; i < PER; ++i) s.insert(base + i);
        for (int i = 0; i < PER; ++i) s.remove(base + i);
    };
    std::vector<std::thread> ths;
    ths.reserve(N);
    for (int t = 0; t < N; ++t) ths.emplace_back(worker, t);
    for (auto& th : ths) th.join();
    assert(s.min() == NO_KEY);
    assert(s.max() == NO_KEY);
    printf("  [%s] test_high_thread_stress(32)     PASS\n", name);
}

// ---------- run all tests for a single type ----------
template <class S>
static void run_all(const char* name) {
    printf("\n=== %s ===\n", name);
    t_empty<S>(name);
    t_single<S>(name);
    t_ordered_inserts<S>(name);
    t_reverse_inserts<S>(name);
    t_mixed_ops<S>(name);
    t_extract_seq<S>(name);
    t_concurrent<S>(name, 4);
    t_concurrent<S>(name, 8);
    t_concurrent<S>(name, 16);
    t_concurrent_contended<S>(name, 8);
    t_concurrent_contended<S>(name, 16);
    t_concurrent_extract_min<S>(name);
    t_concurrent_extract_max<S>(name);
    t_concurrent_mixed<S>(name, 8);
    t_concurrent_mixed<S>(name, 16);
    t_high_thread_stress<S>(name);
    printf("  [%s] ALL PASSED\n", name);
}

int main() {
    run_all<BSTList>                   ("BSTList             (normal BST  + barrier list)");
    run_all<BSTListBalanced>           ("BSTListBalanced     (balanced BST + barrier list)");
    run_all<BSTListPragmatic>          ("BSTListPragmatic    (normal BST  + pragmatic list)");
    run_all<BSTListBalancedPragmatic>  ("BSTListBalPragmatic (balanced BST + pragmatic list)");

    printf("\n*** All variants passed all tests. ***\n");
    return 0;
}
