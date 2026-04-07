/**
 * Concurrent correctness test for findMin / findMax on the fatorou BST.
 *
 * Test strategy:
 *
 *  1. Sequential sanity test  — single-threaded insert/delete with assert after each op.
 *
 *  2. Pause-and-check concurrent test — workers run freely, then we pause ALL
 *     workers at a barrier, query findMin/findMax in a quiescent state, compare
 *     to ground-truth oracle (std::set), then resume. Pausing ensures no
 *     concurrent modifications so there are zero false positives.
 *
 *  3. Final-state check — after the timed run ends and all threads join, do a
 *     last oracle comparison to confirm the tree settled correctly.
 *
 * Build:
 *   g++ -O2 -std=c++17 -mcx16 -pthread correctness_minmax_test.cpp \
 *       -o correctness_minmax_test
 *
 * Run:
 *   ./correctness_minmax_test [workers] [duration_s] [key_range] [checks_per_s]
 *   e.g. ./correctness_minmax_test 32 20 100000 10
 */

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "./common.h"
#include "./bst_impl_unsafe.h"

// ── config (overridden by argv) ───────────────────────────────────────────────
static int NUM_WORKERS    = 8;
static int DURATION_S     = 10;
static int KEY_RANGE      = 100000;
static int CHECKS_PER_SEC = 5;   // pause-and-check frequency

// ── oracle: ground-truth std::set protected by a mutex ───────────────────────
// Workers update BOTH the oracle AND the tree under the oracle lock.
// This makes the oracle always consistent with the tree's linearized state.
std::mutex oracle_mtx;
std::set<int> oracle;

// ── pause/resume protocol ─────────────────────────────────────────────────────
// Main thread sets pause_flag; workers spin on it between ops.
// paused_workers counts how many workers are currently paused.
std::atomic<bool>  pause_flag{false};
std::atomic<int>   paused_workers{0};
std::atomic<bool>  stop{false};

// ── error / check counters ────────────────────────────────────────────────────
std::atomic<long long> total_errors{0};
std::atomic<long long> total_checks{0};

// ── the shared tree ───────────────────────────────────────────────────────────
SUB_ERIC_TREE::SubEricTree *tree = nullptr;

// ── worker thread ─────────────────────────────────────────────────────────────
void worker(int seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> key_dist(1, KEY_RANGE);
    std::uniform_int_distribution<int> op_dist(0, 1);

    while (!stop.load(std::memory_order_relaxed)) {
        // Pause-and-check protocol: if main asks us to pause, do so.
        if (pause_flag.load(std::memory_order_acquire)) {
            paused_workers.fetch_add(1, std::memory_order_release);
            while (pause_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            paused_workers.fetch_sub(1, std::memory_order_release);
        }

        int key = key_dist(rng);

        // Hold oracle lock for the ENTIRE insert/remove so the oracle and tree
        // stay in sync at every moment.
        std::lock_guard<std::mutex> lk(oracle_mtx);

        if (op_dist(rng) == 0) {
            if (oracle.find(key) == oracle.end()) {
                bool inserted = tree->insert(key, key);
                if (inserted) oracle.insert(key);
            }
        } else {
            if (oracle.find(key) != oracle.end()) {
                int val = tree->remove(key);
                if (val != tree->SENTINAL_VALUE) oracle.erase(key);
            }
        }
    }
}

// ── pause all workers, check, resume ─────────────────────────────────────────
void pause_and_check(const char *label) {
    // Signal workers to pause.
    pause_flag.store(true, std::memory_order_release);

    // Wait until all workers are paused.
    while (paused_workers.load(std::memory_order_acquire) < NUM_WORKERS) {
        std::this_thread::yield();
    }

    // Now hold the oracle lock to get a consistent snapshot while all workers
    // are paused. (Workers that haven't entered the pause yet are still inside
    // a lock_guard scope, so they'll release before spinning on pause_flag.)
    // To be safe we also grab the oracle_mtx.
    {
        std::lock_guard<std::mutex> lk(oracle_mtx);

        int expected_min = oracle.empty() ? tree->SENTINAL_VALUE : *oracle.begin();
        int expected_max = oracle.empty() ? tree->SENTINAL_VALUE : *oracle.rbegin();

        int got_min = tree->findMin();
        int got_max = tree->findMax();

        total_checks.fetch_add(2, std::memory_order_relaxed);

        bool ok = true;
        if (got_min != expected_min) {
            ok = false;
            total_errors.fetch_add(1, std::memory_order_relaxed);
            std::ostringstream ss;
            ss << "[ERROR][" << label << "] findMin=" << got_min
               << " expected=" << expected_min
               << " oracle_size=" << oracle.size() << "\n";
            std::cerr << ss.str();
        }
        if (got_max != expected_max) {
            ok = false;
            total_errors.fetch_add(1, std::memory_order_relaxed);
            std::ostringstream ss;
            ss << "[ERROR][" << label << "] findMax=" << got_max
               << " expected=" << expected_max
               << " oracle_size=" << oracle.size() << "\n";
            std::cerr << ss.str();
        }
        (void)ok;
    }

    // Resume workers.
    pause_flag.store(false, std::memory_order_release);
    // Wait until all workers have actually resumed (count back to 0).
    while (paused_workers.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
}

// ── sequential sanity test ────────────────────────────────────────────────────
void sequential_test() {
    std::cout << "=== Sequential sanity test ===" << std::endl;
    SUB_ERIC_TREE::SubEricTree t;

    // Empty tree
    assert(t.findMin() == t.SENTINAL_VALUE);
    assert(t.findMax() == t.SENTINAL_VALUE);

    // Insert 1..20, check after each
    std::set<int> ref;
    for (int i = 1; i <= 20; ++i) {
        t.insert(i, i);
        ref.insert(i);
        assert(t.findMin() == *ref.begin());
        assert(t.findMax() == *ref.rbegin());
    }

    // Remove from the minimum end
    for (int i = 1; i <= 10; ++i) {
        t.remove(i);
        ref.erase(i);
        assert(t.findMin() == *ref.begin());
        assert(t.findMax() == *ref.rbegin());
    }

    // Remove from the maximum end
    for (int i = 20; i > 14; --i) {
        t.remove(i);
        ref.erase(i);
        assert(t.findMin() == *ref.begin());
        assert(t.findMax() == *ref.rbegin());
    }

    // Remove all remaining
    std::vector<int> remaining(ref.begin(), ref.end());
    for (int k : remaining) {
        t.remove(k);
        ref.erase(k);
        if (ref.empty()) {
            assert(t.findMin() == t.SENTINAL_VALUE);
            assert(t.findMax() == t.SENTINAL_VALUE);
        } else {
            assert(t.findMin() == *ref.begin());
            assert(t.findMax() == *ref.rbegin());
        }
    }

    // Insert out-of-order, check
    for (int k : {50, 10, 30, 70, 1, 100}) {
        t.insert(k, k);
        ref.insert(k);
        assert(t.findMin() == *ref.begin());
        assert(t.findMax() == *ref.rbegin());
    }

    // Delete min/max alternately
    while (!ref.empty()) {
        int mn = *ref.begin();
        t.remove(mn); ref.erase(mn);
        if (ref.empty()) {
            assert(t.findMin() == t.SENTINAL_VALUE);
            assert(t.findMax() == t.SENTINAL_VALUE);
            break;
        }
        assert(t.findMin() == *ref.begin());
        assert(t.findMax() == *ref.rbegin());

        int mx = *ref.rbegin();
        t.remove(mx); ref.erase(mx);
        if (ref.empty()) {
            assert(t.findMin() == t.SENTINAL_VALUE);
            assert(t.findMax() == t.SENTINAL_VALUE);
            break;
        }
        assert(t.findMin() == *ref.begin());
        assert(t.findMax() == *ref.rbegin());
    }

    std::cout << "Sequential test PASSED." << std::endl;
}

// ── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char **argv) {
    if (argc > 1) NUM_WORKERS    = std::atoi(argv[1]);
    if (argc > 2) DURATION_S     = std::atoi(argv[2]);
    if (argc > 3) KEY_RANGE      = std::atoi(argv[3]);
    if (argc > 4) CHECKS_PER_SEC = std::atoi(argv[4]);

    int PREFILL_SIZE = KEY_RANGE / 2;

    std::cout << "Config: workers=" << NUM_WORKERS
              << " duration=" << DURATION_S << "s"
              << " key_range=[1," << KEY_RANGE << "]"
              << " pause_checks_per_sec=" << CHECKS_PER_SEC
              << " prefill=" << PREFILL_SIZE << std::endl;

    sequential_test();

    // ── set up shared tree ────────────────────────────────────────────────
    tree = new SUB_ERIC_TREE::SubEricTree();

    // Prefill (single-threaded, both oracle and tree)
    std::cout << "Prefilling with " << PREFILL_SIZE << " elements..." << std::endl;
    {
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> d(1, KEY_RANGE);
        int inserted = 0;
        while (inserted < PREFILL_SIZE) {
            int k = d(rng);
            if (oracle.find(k) == oracle.end() && tree->insert(k, k)) {
                oracle.insert(k);
                ++inserted;
            }
        }
    }
    std::cout << "Prefill done. oracle_size=" << oracle.size()
              << " oracle_min=" << *oracle.begin()
              << " oracle_max=" << *oracle.rbegin() << std::endl;

    // Sanity check right after prefill
    {
        int expected_min = *oracle.begin();
        int expected_max = *oracle.rbegin();
        int got_min = tree->findMin();
        int got_max = tree->findMax();
        if (got_min != expected_min || got_max != expected_max) {
            std::cerr << "[ERROR] Post-prefill: findMin=" << got_min
                      << " expected=" << expected_min
                      << " findMax=" << got_max
                      << " expected=" << expected_max << std::endl;
            total_errors++;
        } else {
            std::cout << "Post-prefill check PASSED (min=" << got_min
                      << " max=" << got_max << ")" << std::endl;
        }
    }

    // ── launch worker threads ─────────────────────────────────────────────
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_WORKERS; ++i)
        threads.emplace_back(worker, i + 1000);

    // ── pause-and-check loop ──────────────────────────────────────────────
    auto run_start = std::chrono::steady_clock::now();
    auto run_end   = run_start + std::chrono::seconds(DURATION_S);

    long long check_interval_ms = (CHECKS_PER_SEC > 0)
        ? (1000 / CHECKS_PER_SEC)
        : 1000;

    int check_idx = 0;
    while (std::chrono::steady_clock::now() < run_end) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(check_interval_ms));

        char label[32];
        std::snprintf(label, sizeof(label), "check#%d", ++check_idx);
        pause_and_check(label);

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - run_start).count();
        std::cout << "[" << label << "] elapsed=" << elapsed << "s"
                  << " errors_so_far=" << total_errors.load()
                  << " total_checks=" << total_checks.load() << std::endl;
    }

    // ── stop workers and join ─────────────────────────────────────────────
    stop.store(true, std::memory_order_relaxed);
    // Release any paused workers first
    pause_flag.store(false, std::memory_order_release);
    for (auto &t : threads) t.join();

    // ── final state check ─────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(oracle_mtx);
        int expected_min = oracle.empty() ? tree->SENTINAL_VALUE : *oracle.begin();
        int expected_max = oracle.empty() ? tree->SENTINAL_VALUE : *oracle.rbegin();
        int got_min = tree->findMin();
        int got_max = tree->findMax();
        total_checks.fetch_add(2);
        bool ok = true;
        if (got_min != expected_min) {
            ok = false; total_errors++;
            std::cerr << "[ERROR][final] findMin=" << got_min
                      << " expected=" << expected_min << std::endl;
        }
        if (got_max != expected_max) {
            ok = false; total_errors++;
            std::cerr << "[ERROR][final] findMax=" << got_max
                      << " expected=" << expected_max << std::endl;
        }
        if (ok) std::cout << "[final] findMin=" << got_min
                          << " findMax=" << got_max << " CORRECT" << std::endl;
    }

    // ── summary ───────────────────────────────────────────────────────────
    std::cout << "\n=== Concurrent test results ===" << std::endl;
    std::cout << "Pause-and-check rounds : " << check_idx   << std::endl;
    std::cout << "Total checks           : " << total_checks.load() << std::endl;
    std::cout << "Errors found           : " << total_errors.load() << std::endl;

    if (total_errors.load() == 0)
        std::cout << "RESULT: ALL CHECKS PASSED" << std::endl;
    else
        std::cout << "RESULT: " << total_errors.load() << " ERRORS" << std::endl;

    delete tree;
    return total_errors.load() > 0 ? 1 : 0;
}
