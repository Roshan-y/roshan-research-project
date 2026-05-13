// Throughput benchmark for all four BST+List variants.
//
// Compile:
//   g++ -std=c++17 -O2 -pthread -o bench bench.cpp
// Run:
//   ./bench

#include "bst_list.h"
#include "bst_list_balanced.h"
#include "bst_list_pragmatic.h"
#include "bst_list_balanced_pragmatic.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>
#include <string>

using Clock = std::chrono::steady_clock;
static constexpr int BENCH_SECS = 3;

// -------------------------------------------------------------------------
// Generic mixed benchmark: 40% insert, 40% remove, 20% min+max
// Returns Mops/s
// -------------------------------------------------------------------------
template <class S>
double bench_mixed(int n_threads, int key_range) {
    S s;
    // Pre-fill with key_range/2 random keys
    {
        std::mt19937 rng(42);
        std::uniform_int_distribution<long> kd(0, key_range - 1);
        int inserted = 0;
        while (inserted < key_range / 2) {
            if (s.insert(kd(rng))) ++inserted;
        }
    }

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<long long> total_ops{0};

    auto worker = [&](int tid) {
        std::mt19937 rng(tid * 1234567u + 99991u);
        std::uniform_int_distribution<long> kd(0, key_range - 1);
        std::uniform_int_distribution<int>  od(0, 4);  // 0-1=ins, 2-3=rem, 4=minmax

        while (!go.load(std::memory_order_acquire)) {}

        long long ops = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            int op = od(rng);
            long key = kd(rng);
            if (op <= 1)      { s.insert(key); }
            else if (op <= 3) { s.remove(key); }
            else              { s.min(); s.max(); }
            ++ops;
        }
        total_ops.fetch_add(ops, std::memory_order_relaxed);
    };

    std::vector<std::thread> ths;
    ths.reserve(n_threads);
    for (int t = 0; t < n_threads; ++t) ths.emplace_back(worker, t);

    go.store(true, std::memory_order_release);
    auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(BENCH_SECS));
    stop.store(true, std::memory_order_relaxed);

    for (auto& th : ths) th.join();

    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    return static_cast<double>(total_ops.load()) / elapsed / 1e6;
}

// -------------------------------------------------------------------------
// Insert-only benchmark: all threads insert random keys.
// Returns Mops/s (insert attempts; duplicates are no-ops).
// -------------------------------------------------------------------------
template <class S>
double bench_insert(int n_threads, int key_range) {
    S s;
    // Pre-fill to ~25% so there is room to insert without constantly hitting duplicates.
    {
        std::mt19937 rng(42);
        std::uniform_int_distribution<long> kd(0, key_range - 1);
        int inserted = 0;
        while (inserted < key_range / 4) {
            if (s.insert(kd(rng))) ++inserted;
        }
    }

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<long long> total_ops{0};

    auto worker = [&](int tid) {
        std::mt19937 rng(tid * 2345678u + 11111u);
        std::uniform_int_distribution<long> kd(0, key_range - 1);
        while (!go.load(std::memory_order_acquire)) {}
        long long ops = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            s.insert(kd(rng));
            ++ops;
        }
        total_ops.fetch_add(ops, std::memory_order_relaxed);
    };

    std::vector<std::thread> ths;
    ths.reserve(n_threads);
    for (int t = 0; t < n_threads; ++t) ths.emplace_back(worker, t);
    go.store(true, std::memory_order_release);
    auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(BENCH_SECS));
    stop.store(true, std::memory_order_relaxed);
    for (auto& th : ths) th.join();
    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    return static_cast<double>(total_ops.load()) / elapsed / 1e6;
}

// -------------------------------------------------------------------------
// Remove-only benchmark: all threads remove random keys, refilling when empty.
// Returns Mops/s (remove attempts).
// -------------------------------------------------------------------------
template <class S>
double bench_remove(int n_threads, int key_range) {
    S s;
    {
        std::mt19937 rng(42);
        std::uniform_int_distribution<long> kd(0, key_range - 1);
        int inserted = 0;
        while (inserted < key_range / 2) {
            if (s.insert(kd(rng))) ++inserted;
        }
    }

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<long long> total_ops{0};

    auto worker = [&](int tid) {
        std::mt19937 rng(tid * 3456789u + 22222u);
        std::uniform_int_distribution<long> kd(0, key_range - 1);
        while (!go.load(std::memory_order_acquire)) {}
        long long ops = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            if (!s.remove(kd(rng))) {
                // Refill a few keys so threads always have something to remove.
                for (int i = 0; i < 4; ++i) s.insert(kd(rng));
            }
            ++ops;
        }
        total_ops.fetch_add(ops, std::memory_order_relaxed);
    };

    std::vector<std::thread> ths;
    ths.reserve(n_threads);
    for (int t = 0; t < n_threads; ++t) ths.emplace_back(worker, t);
    go.store(true, std::memory_order_release);
    auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(BENCH_SECS));
    stop.store(true, std::memory_order_relaxed);
    for (auto& th : ths) th.join();
    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    return static_cast<double>(total_ops.load()) / elapsed / 1e6;
}

// -------------------------------------------------------------------------
// Min+Max-only benchmark: all threads alternate min() and max() calls.
// Returns Mops/s (each call counted as one op).
// -------------------------------------------------------------------------
template <class S>
double bench_min_max(int n_threads, int key_range) {
    S s;
    {
        std::mt19937 rng(42);
        std::uniform_int_distribution<long> kd(0, key_range - 1);
        int inserted = 0;
        while (inserted < key_range / 2) {
            if (s.insert(kd(rng))) ++inserted;
        }
    }

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<long long> total_ops{0};

    auto worker = [&](int) {
        while (!go.load(std::memory_order_acquire)) {}
        long long ops = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            s.min();
            s.max();
            ops += 2;
        }
        total_ops.fetch_add(ops, std::memory_order_relaxed);
    };

    std::vector<std::thread> ths;
    ths.reserve(n_threads);
    for (int t = 0; t < n_threads; ++t) ths.emplace_back(worker, t);
    go.store(true, std::memory_order_release);
    auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(BENCH_SECS));
    stop.store(true, std::memory_order_relaxed);
    for (auto& th : ths) th.join();
    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    return static_cast<double>(total_ops.load()) / elapsed / 1e6;
}

// -------------------------------------------------------------------------
// Extract_min benchmark: threads only call extract_min, refilling as needed
// Returns Mops/s
// -------------------------------------------------------------------------
template <class S>
double bench_extract_min(int n_threads, int key_range) {
    S s;
    {
        std::mt19937 rng(42);
        std::uniform_int_distribution<long> kd(0, key_range - 1);
        int inserted = 0;
        while (inserted < key_range / 2) {
            if (s.insert(kd(rng))) ++inserted;
        }
    }

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<long long> total_ops{0};
    // A shared counter to refill keys
    std::atomic<long> next_key{key_range};

    auto worker = [&](int tid) {
        (void)tid;
        std::mt19937 rng(tid * 7654321u + 11111u);
        std::uniform_int_distribution<long> kd(0, key_range - 1);

        while (!go.load(std::memory_order_acquire)) {}

        long long ops = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            long v = s.extract_min();
            if (v == NO_KEY) {
                // Refill a small batch
                for (int i = 0; i < 8; ++i)
                    s.insert(kd(rng));
            }
            ++ops;
        }
        total_ops.fetch_add(ops, std::memory_order_relaxed);
    };

    std::vector<std::thread> ths;
    ths.reserve(n_threads);
    for (int t = 0; t < n_threads; ++t) ths.emplace_back(worker, t);

    go.store(true, std::memory_order_release);
    auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(BENCH_SECS));
    stop.store(true, std::memory_order_relaxed);

    for (auto& th : ths) th.join();

    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    return static_cast<double>(total_ops.load()) / elapsed / 1e6;
}

// -------------------------------------------------------------------------
// Extract_max benchmark
// -------------------------------------------------------------------------
template <class S>
double bench_extract_max(int n_threads, int key_range) {
    S s;
    {
        std::mt19937 rng(42);
        std::uniform_int_distribution<long> kd(0, key_range - 1);
        int inserted = 0;
        while (inserted < key_range / 2) {
            if (s.insert(kd(rng))) ++inserted;
        }
    }

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<long long> total_ops{0};

    auto worker = [&](int tid) {
        (void)tid;
        std::mt19937 rng(tid * 3456789u + 22222u);
        std::uniform_int_distribution<long> kd(0, key_range - 1);

        while (!go.load(std::memory_order_acquire)) {}

        long long ops = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            long v = s.extract_max();
            if (v == NO_KEY) {
                for (int i = 0; i < 8; ++i)
                    s.insert(kd(rng));
            }
            ++ops;
        }
        total_ops.fetch_add(ops, std::memory_order_relaxed);
    };

    std::vector<std::thread> ths;
    ths.reserve(n_threads);
    for (int t = 0; t < n_threads; ++t) ths.emplace_back(worker, t);

    go.store(true, std::memory_order_release);
    auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(BENCH_SECS));
    stop.store(true, std::memory_order_relaxed);

    for (auto& th : ths) th.join();

    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    return static_cast<double>(total_ops.load()) / elapsed / 1e6;
}

// -------------------------------------------------------------------------
// Main: print markdown tables
// -------------------------------------------------------------------------
int main() {
    const int thread_counts[] = {1, 2, 4, 8, 16, 32};
    const int key_ranges[]    = {1000, 10000, 100000};
    const int N_TC = 6, N_KR = 3;

    const char* variant_names[] = {
        "BSTList",
        "BSTListBalanced",
        "BSTListPragmatic",
        "BSTListBalancedPragmatic"
    };

    printf("# Throughput Benchmark Results\n\n");
    printf("Benchmark duration per run: %d seconds\n", BENCH_SECS);
    printf("Mix: 40%% insert, 40%% remove, 20%% min+max\n\n");

    // ---- Mixed throughput tables ----
    printf("## Mixed Throughput (Mops/s)\n\n");

    // One table per key_range
    for (int ki = 0; ki < N_KR; ++ki) {
        int kr = key_ranges[ki];
        printf("### Key Range = %d\n\n", kr);
        printf("| Threads | BSTList | BSTListBalanced | BSTListPragmatic | BSTListBalancedPragmatic |\n");
        printf("|---------|---------|-----------------|------------------|-------------------------|\n");

        for (int ti = 0; ti < N_TC; ++ti) {
            int nth = thread_counts[ti];
            double r0 = bench_mixed<BSTList>                  (nth, kr);
            double r1 = bench_mixed<BSTListBalanced>          (nth, kr);
            double r2 = bench_mixed<BSTListPragmatic>         (nth, kr);
            double r3 = bench_mixed<BSTListBalancedPragmatic> (nth, kr);
            printf("| %7d | %7.3f | %15.3f | %16.3f | %24.3f |\n",
                   nth, r0, r1, r2, r3);
            fflush(stdout);
        }
        printf("\n");
    }

    // ---- Per-operation breakdown at 8 threads x 10000 keys ----
    printf("## Per-Operation Throughput at 8 threads, key_range=10000 (Mops/s)\n\n");
    printf("| Variant | insert | remove | min+max | extract_min | extract_max |\n");
    printf("|---------|--------|--------|---------|-------------|-------------|\n");

    auto print_ops = [&](const char* name,
                         double ins, double rem, double mm,
                         double emin, double emax) {
        printf("| %-24s | %6.3f | %6.3f | %7.3f | %11.3f | %11.3f |\n",
               name, ins, rem, mm, emin, emax);
        fflush(stdout);
    };

    {
        double ins  = bench_insert<BSTList>(8, 10000);
        double rem  = bench_remove<BSTList>(8, 10000);
        double mm   = bench_min_max<BSTList>(8, 10000);
        double emin = bench_extract_min<BSTList>(8, 10000);
        double emax = bench_extract_max<BSTList>(8, 10000);
        print_ops("BSTList", ins, rem, mm, emin, emax);
    }
    {
        double ins  = bench_insert<BSTListBalanced>(8, 10000);
        double rem  = bench_remove<BSTListBalanced>(8, 10000);
        double mm   = bench_min_max<BSTListBalanced>(8, 10000);
        double emin = bench_extract_min<BSTListBalanced>(8, 10000);
        double emax = bench_extract_max<BSTListBalanced>(8, 10000);
        print_ops("BSTListBalanced", ins, rem, mm, emin, emax);
    }
    {
        double ins  = bench_insert<BSTListPragmatic>(8, 10000);
        double rem  = bench_remove<BSTListPragmatic>(8, 10000);
        double mm   = bench_min_max<BSTListPragmatic>(8, 10000);
        double emin = bench_extract_min<BSTListPragmatic>(8, 10000);
        double emax = bench_extract_max<BSTListPragmatic>(8, 10000);
        print_ops("BSTListPragmatic", ins, rem, mm, emin, emax);
    }
    {
        double ins  = bench_insert<BSTListBalancedPragmatic>(8, 10000);
        double rem  = bench_remove<BSTListBalancedPragmatic>(8, 10000);
        double mm   = bench_min_max<BSTListBalancedPragmatic>(8, 10000);
        double emin = bench_extract_min<BSTListBalancedPragmatic>(8, 10000);
        double emax = bench_extract_max<BSTListBalancedPragmatic>(8, 10000);
        print_ops("BSTListBalancedPragmatic", ins, rem, mm, emin, emax);
    }
    printf("\n");

    return 0;
}
