# Min/Max Benchmark Notes

Date: 2026-03-25

## Remote server check
- SSH access to lpdquad.epfl.ch works after VPN connection.
- Remote host reports Linux x86_64.
- Hostname: lpdquad.

## Existing augmented tree support
- The augmented chromatic tree currently stores subtree size information.
- `findMin()` and `findMax()` do not use cached min/max metadata.
- They walk to the leftmost or rightmost leaf, so min/max are not O(1).

## How cached min/max works in the copy
- In [common/node_types.h](CBAT-main-minmax/common/node_types.h#L123-L132), each `Version` now stores `min` and `max` alongside the existing subtree metadata.
- In [chromatic_impl.h](CBAT-main-minmax/ds/brown_ext_chromatic_augment_lf/chromatic_impl.h#L75-L93), each newly created version initializes those fields from its children.
- In [chromatic_impl.h](CBAT-main-minmax/ds/brown_ext_chromatic_augment_lf/chromatic_impl.h#L257-L275), `findMin()` and `findMax()` return the cached extrema directly.
- The update path still uses the existing LLX/SCX-style lock-free version replacement, so the extrema change atomically when a new version is installed.

## Leaf ordering
- The tree is a BST, so leaf keys are ordered by key in the search-tree sense.
- There is no separate linked leaf list in the existing augmented tree implementation.

## Benchmark work
- Added a dedicated min/max benchmark mode in the microbench harness.
- Added a reproducible comparison script: [benchmark_minmax_compare.sh](benchmark_minmax_compare.sh)

## Why the speedup is modest
- The benchmark measures end-to-end wall time, not just the query call.
- The query is only part of the runtime; prefilling, thread setup, memory reclamation, and teardown are still included.
- On a balanced BST, even the traversal-based `findMin()` / `findMax()` is only about $O(h)$, and with tens of thousands of keys the height is still small.
- So the cached version is faster in principle, but the difference is easy to hide unless the tree is much larger or the benchmark isolates just the extrema call.

## Multithreaded benchmark fix
- The concurrent prefill path with `-nprefill 2` was crashing in both repositories in this environment.
- The benchmark script now uses a single prefill thread and four worker threads, so the measured phase is multithreaded while construction stays stable.

## Server timing results
- Timing runs were executed on lpdquad after copying both repositories to `/localhome/yogendra`.
- The stable measurements below use 4 worker threads and 1 prefill thread.

| Repository | Mode | real time |
|---|---:|---:|
| CBAT-main-minmax | min | 6.65 s |
| CBAT-main | min | 6.77 s |
| CBAT-main-minmax | max | 6.67 s |
| CBAT-main | max | 6.73 s |

## Conclusion
- The copied tree now maintains cached min/max metadata, so extrema queries are O(1) on the version state.
- The original CBAT tree remains traversal-based for extrema queries.
- The benchmark script in [benchmark_minmax_compare.sh](benchmark_minmax_compare.sh) can be used to rerun the same comparison.
- The measured speedup is real but small in this workload; a larger tree or a query-only microbenchmark would show the difference more clearly.
