#include "bst_list.h"
#include <cassert>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <random>

// ---------- sequential tests ----------

/*
scp -r /Users/roshanyogendra/Desktop/Workspace/MA/MA2/Lab/roshan-research-project
  /implementation yogendra@lpdquad.epfl.ch:/localhome/yogendra/
  
ssh yogendra@lpdquad.epfl.ch                                                                    
  cd /localhome/yogendra/                                                                         
  g++ -std=c++17 -O2 -pthread -o test implementation/test.cpp                                     
  ./test
*/

static void test_empty() {
    BSTList s;
    assert(s.min() == NO_KEY);
    assert(s.max() == NO_KEY);
    printf("test_empty         PASS\n");
}

static void test_single() {
    BSTList s;
    assert(s.insert(42));
    assert(!s.insert(42)); // duplicate
    assert(s.min() == 42);
    assert(s.max() == 42);
    assert(s.remove(42));
    assert(!s.remove(42)); // already gone
    assert(s.min() == NO_KEY);
    assert(s.max() == NO_KEY);
    printf("test_single        PASS\n");
}

static void test_ordered_inserts() {
    BSTList s;
    for (int i = 1; i <= 100; ++i)
        assert(s.insert(i));
    assert(s.min() == 1);
    assert(s.max() == 100);
    for (int i = 1; i <= 100; ++i)
        assert(s.remove(i));
    assert(s.min() == NO_KEY);
    assert(s.max() == NO_KEY);
    printf("test_ordered_inserts PASS\n");
}

static void test_reverse_inserts() {
    BSTList s;
    for (int i = 100; i >= 1; --i)
        assert(s.insert(i));
    assert(s.min() == 1);
    assert(s.max() == 100);
    printf("test_reverse_inserts PASS\n");
}

static void test_mixed_ops() {
    BSTList s;
    std::vector<int> keys = {5, 3, 8, 1, 9, 2, 7, 4, 6, 10};
    for (int k : keys) s.insert(k);
    assert(s.min() == 1);
    assert(s.max() == 10);

    s.remove(1);
    assert(s.min() == 2);
    s.remove(10);
    assert(s.max() == 9);

    s.insert(0);
    assert(s.min() == 0);
    s.insert(11);
    assert(s.max() == 11);
    printf("test_mixed_ops     PASS\n");
}

// ---------- concurrent test ----------
// N threads each insert a disjoint range, then all delete their own range.
// After everything finishes the set must be empty.

static void test_concurrent() {
    constexpr int N_THREADS = 8;
    constexpr int PER_THREAD = 1000;

    BSTList s;

    auto worker = [&](int tid) {
        int base = tid * PER_THREAD;
        // Insert phase
        for (int i = 0; i < PER_THREAD; ++i)
            s.insert(base + i);
        // Delete phase
        for (int i = 0; i < PER_THREAD; ++i)
            s.remove(base + i);
    };

    std::vector<std::thread> threads;
    threads.reserve(N_THREADS);
    for (int t = 0; t < N_THREADS; ++t)
        threads.emplace_back(worker, t);
    for (auto& th : threads)
        th.join();

    assert(s.min() == NO_KEY);
    assert(s.max() == NO_KEY);
    printf("test_concurrent    PASS\n");
}

// Contended concurrent test: all threads insert/remove keys from the same range.
static void test_concurrent_contended() {
    constexpr int N_THREADS = 8;
    constexpr int OPS       = 2000;
    constexpr int KEY_RANGE = 64;

    BSTList s;

    auto worker = [&](int tid) {
        std::mt19937 rng(tid * 1234567u);
        std::uniform_int_distribution<int> key_dist(0, KEY_RANGE - 1);
        std::uniform_int_distribution<int> op_dist(0, 1);
        for (int i = 0; i < OPS; ++i) {
            int key = key_dist(rng);
            if (op_dist(rng) == 0)
                s.insert(key);
            else
                s.remove(key);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(N_THREADS);
    for (int t = 0; t < N_THREADS; ++t)
        threads.emplace_back(worker, t);
    for (auto& th : threads)
        th.join();

    // min <= max when the set is non-empty
    long mn = s.min(), mx = s.max();
    assert((mn == NO_KEY) == (mx == NO_KEY));
    if (mn != NO_KEY) assert(mn <= mx);
    printf("test_concurrent_contended PASS\n");
}

// ---------- extractMin / extractMax sequential tests ----------

static void test_extract_min_empty() {
    BSTList s;
    assert(s.extract_min() == NO_KEY);
    printf("test_extract_min_empty PASS\n");
}

static void test_extract_max_empty() {
    BSTList s;
    assert(s.extract_max() == NO_KEY);
    printf("test_extract_max_empty PASS\n");
}

static void test_extract_min_single() {
    BSTList s;
    s.insert(7);
    assert(s.extract_min() == 7);
    assert(s.min() == NO_KEY);
    assert(s.extract_min() == NO_KEY);  // now empty
    printf("test_extract_min_single PASS\n");
}

static void test_extract_max_single() {
    BSTList s;
    s.insert(7);
    assert(s.extract_max() == 7);
    assert(s.max() == NO_KEY);
    assert(s.extract_max() == NO_KEY);  // now empty
    printf("test_extract_max_single PASS\n");
}

static void test_extract_min_ordered() {
    BSTList s;
    for (int i = 1; i <= 10; ++i)
        s.insert(i);

    for (int i = 1; i <= 10; ++i) {
        long v = s.extract_min();
        assert(v == i);
    }
    assert(s.extract_min() == NO_KEY);
    printf("test_extract_min_ordered PASS\n");
}

static void test_extract_max_ordered() {
    BSTList s;
    for (int i = 1; i <= 10; ++i)
        s.insert(i);

    for (int i = 10; i >= 1; --i) {
        long v = s.extract_max();
        assert(v == i);
    }
    assert(s.extract_max() == NO_KEY);
    printf("test_extract_max_ordered PASS\n");
}

static void test_extract_min_interleaved_remove() {
    // Insert 1..5, delete 2 and 4, then extractMin should skip them.
    BSTList s;
    for (int i = 1; i <= 5; ++i) s.insert(i);
    s.remove(2);
    s.remove(4);

    assert(s.extract_min() == 1);
    assert(s.extract_min() == 3);
    assert(s.extract_min() == 5);
    assert(s.extract_min() == NO_KEY);
    printf("test_extract_min_interleaved_remove PASS\n");
}

static void test_extract_max_interleaved_remove() {
    // Insert 1..5, delete 2 and 4, then extractMax should skip them.
    BSTList s;
    for (int i = 1; i <= 5; ++i) s.insert(i);
    s.remove(2);
    s.remove(4);

    assert(s.extract_max() == 5);
    assert(s.extract_max() == 3);
    assert(s.extract_max() == 1);
    assert(s.extract_max() == NO_KEY);
    printf("test_extract_max_interleaved_remove PASS\n");
}

// ---------- extractMin / extractMax concurrent tests ----------

// N threads each repeatedly call extract_min until NO_KEY; meanwhile M threads insert.
// Property: every inserted key is extracted exactly once, OR is still in the list at the end
// (because the extractor threads may finish before all inserts complete).
// Simplified correctness check: the extracted values form a non-decreasing sequence per thread,
// and no value is extracted more than once globally.
static void test_concurrent_extract_min() {
    constexpr int N_INSERTERS = 4;
    constexpr int N_EXTRACTORS = 4;
    constexpr int PER_THREAD  = 500;
    constexpr int TOTAL_KEYS  = N_INSERTERS * PER_THREAD;

    BSTList s;
    std::atomic<int> insert_done{0};

    // Each extractor collects extracted keys.
    std::vector<std::vector<long>> extracted(N_EXTRACTORS);

    std::vector<std::thread> threads;
    threads.reserve(N_INSERTERS + N_EXTRACTORS);

    for (int t = 0; t < N_INSERTERS; ++t) {
        threads.emplace_back([&, t]() {
            int base = t * PER_THREAD;
            for (int i = 0; i < PER_THREAD; ++i)
                s.insert(base + i);
            insert_done.fetch_add(1, std::memory_order_release);
        });
    }

    for (int t = 0; t < N_EXTRACTORS; ++t) {
        threads.emplace_back([&, t]() {
            while (true) {
                long v = s.extract_min();
                if (v == NO_KEY) {
                    // If all inserters are done and list is empty, we're done.
                    if (insert_done.load(std::memory_order_acquire) == N_INSERTERS)
                        break;
                    // Otherwise keep trying.
                    continue;
                }
                extracted[t].push_back(v);
            }
        });
    }

    for (auto& th : threads) th.join();

    // Drain any remaining keys (some may not have been extracted if extractors exited early).
    std::vector<long> remaining;
    {
        long v;
        while ((v = s.extract_min()) != NO_KEY)
            remaining.push_back(v);
    }

    // Merge all extracted keys and remaining into one sorted list.
    std::vector<long> all_keys;
    for (auto& vec : extracted)
        all_keys.insert(all_keys.end(), vec.begin(), vec.end());
    all_keys.insert(all_keys.end(), remaining.begin(), remaining.end());
    std::sort(all_keys.begin(), all_keys.end());

    // Every key in [0, TOTAL_KEYS) must appear exactly once.
    assert((int)all_keys.size() == TOTAL_KEYS);
    for (int i = 0; i < TOTAL_KEYS; ++i)
        assert(all_keys[i] == i);

    printf("test_concurrent_extract_min PASS\n");
}

// Symmetric test for extract_max.
static void test_concurrent_extract_max() {
    constexpr int N_INSERTERS = 4;
    constexpr int N_EXTRACTORS = 4;
    constexpr int PER_THREAD  = 500;
    constexpr int TOTAL_KEYS  = N_INSERTERS * PER_THREAD;

    BSTList s;
    std::atomic<int> insert_done{0};

    std::vector<std::vector<long>> extracted(N_EXTRACTORS);

    std::vector<std::thread> threads;
    threads.reserve(N_INSERTERS + N_EXTRACTORS);

    for (int t = 0; t < N_INSERTERS; ++t) {
        threads.emplace_back([&, t]() {
            int base = t * PER_THREAD;
            for (int i = 0; i < PER_THREAD; ++i)
                s.insert(base + i);
            insert_done.fetch_add(1, std::memory_order_release);
        });
    }

    for (int t = 0; t < N_EXTRACTORS; ++t) {
        threads.emplace_back([&, t]() {
            while (true) {
                long v = s.extract_max();
                if (v == NO_KEY) {
                    if (insert_done.load(std::memory_order_acquire) == N_INSERTERS)
                        break;
                    continue;
                }
                extracted[t].push_back(v);
            }
        });
    }

    for (auto& th : threads) th.join();

    std::vector<long> remaining;
    {
        long v;
        while ((v = s.extract_max()) != NO_KEY)
            remaining.push_back(v);
    }

    std::vector<long> all_keys;
    for (auto& vec : extracted)
        all_keys.insert(all_keys.end(), vec.begin(), vec.end());
    all_keys.insert(all_keys.end(), remaining.begin(), remaining.end());
    std::sort(all_keys.begin(), all_keys.end());

    assert((int)all_keys.size() == TOTAL_KEYS);
    for (int i = 0; i < TOTAL_KEYS; ++i)
        assert(all_keys[i] == i);

    printf("test_concurrent_extract_max PASS\n");
}

// Mixed stress test: concurrent insert, remove, extract_min, extract_max.
// Correctness property: no key is returned by extract_min/max that was never inserted,
// and min <= max for any simultaneous non-empty observation.
static void test_concurrent_extract_mixed() {
    constexpr int N_THREADS  = 8;
    constexpr int OPS        = 1000;
    constexpr int KEY_RANGE  = 64;

    BSTList s;

    // Collect extracted keys with a mutex-protected vector (concurrent writes from many threads).
    std::vector<long> extracted_min_keys, extracted_max_keys;
    std::mutex        extract_mu;

    auto worker = [&](int tid) {
        std::mt19937 rng(tid * 9876543u);
        std::uniform_int_distribution<int> key_dist(0, KEY_RANGE - 1);
        std::uniform_int_distribution<int> op_dist(0, 3);

        std::vector<long> local_min, local_max;

        for (int i = 0; i < OPS; ++i) {
            int  key = key_dist(rng);
            int  op  = op_dist(rng);
            switch (op) {
                case 0: s.insert(key);    break;
                case 1: s.remove(key);    break;
                case 2: { long v = s.extract_min(); if (v != NO_KEY) local_min.push_back(v); break; }
                case 3: { long v = s.extract_max(); if (v != NO_KEY) local_max.push_back(v); break; }
            }
        }

        std::lock_guard<std::mutex> lk(extract_mu);
        extracted_min_keys.insert(extracted_min_keys.end(), local_min.begin(), local_min.end());
        extracted_max_keys.insert(extracted_max_keys.end(), local_max.begin(), local_max.end());
    };

    std::vector<std::thread> threads;
    threads.reserve(N_THREADS);
    for (int t = 0; t < N_THREADS; ++t)
        threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();

    // All extracted keys must be in [0, KEY_RANGE).
    for (long v : extracted_min_keys) assert(v >= 0 && v < KEY_RANGE);
    for (long v : extracted_max_keys) assert(v >= 0 && v < KEY_RANGE);

    // Final state: min <= max when non-empty.
    long mn = s.min(), mx = s.max();
    assert((mn == NO_KEY) == (mx == NO_KEY));
    if (mn != NO_KEY) assert(mn <= mx);

    printf("test_concurrent_extract_mixed PASS\n");
}

int main() {
    test_empty();
    test_single();
    test_ordered_inserts();
    test_reverse_inserts();
    test_mixed_ops();
    test_concurrent();
    test_concurrent_contended();

    // extractMin / extractMax sequential tests
    test_extract_min_empty();
    test_extract_max_empty();
    test_extract_min_single();
    test_extract_max_single();
    test_extract_min_ordered();
    test_extract_max_ordered();
    test_extract_min_interleaved_remove();
    test_extract_max_interleaved_remove();

    // extractMin / extractMax concurrent tests
    test_concurrent_extract_min();
    test_concurrent_extract_max();
    test_concurrent_extract_mixed();

    printf("\nAll tests passed.\n");
    return 0;
}
