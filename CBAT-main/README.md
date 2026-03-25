# Balanced Augmented Trees
Implementation of Concurrent Balanced Augmented Trees.\
ACM SIGPLAN Symposium on Principles and Practice of Parallel Programming (PPoPP 2026)

## Data Structures
* /ds/brown_ext_chromatic_augment_lf (BAT): Augmented chromatic tree with no delegation
* /ds/brown_ext_chromatic_delegateDouble_lf (BAT-Del): Augmented chromatic tree, delegation on double failed refresh
* /ds/brown_ext_chromatic_delegateSingle_lf (BAT-EagerDel): Augmented chromatic tree, delegation on single failed refresh

## Running Individual Experiments
Benchmarking framework based off [this benchmarking library](https://gitlab.com/trbot86/setbench/-/tree/master?ref_type=heads). 
Detailed explanations for compiling and running different experiments found [here](https://gitlab.com/trbot86/setbench/-/wikis/home). See [microbench/main.cpp](microbench/main.cpp) for a full list of all runtime options, but we list a few important ones:
* -nwork and -nprefill control the number of work and prefill threads respectively
* -insdel controls the percentage of operations that are an insert or delete
* -rq and -rqsize control the percentage of operations that are range queries and the size of the range queries respectively
  * Any remaining percentage is given to find operations
* -k controls the number of possible keys that can be inserted
* -t controls the runtime of the experiment

### Example Execution Sequence
`cd microbench`\
`make -j brown_ext_chromatic_delegateSingle_lf.debra has_libpapi=0`\
`LD_PRELOAD=../lib/libmimalloc.so ./bin/brown_ext_chromatic_delegateSingle_lf.debra -nwork 60 -nprefill 60 -insdel 20 20 -rq 30 -rqsize 30000 -k 100000 -t 3000`

## Generating Experimental Charts
Instructions for running the experiments and generating charts from the paper. Can take up to a few hours to run depending on the experiment(s).
### Requirements
* A machine with 40+ hyperthreads and 16+ GB of main memory.
* Experiments were run on Linux but may work on other operating systems as well.
* Docker is installed on the machine.
### Setup
* `docker build --network=host --platform linux/amd64 -t bat .`
* `docker run -v .:/home/ubuntu/project -it --privileged --network=host bat`
* `cd /home/ubuntu/project/microbench_experiments/augmented_trees`
### Sanity Check
Before running any experiments which could take a significant amount of time, run a sanity check to make sure all data structures are functional (replace max_threads with the number of threads on your machine minus 2):\
`python3 sanity_check.py [max_threads]`
### To run an individual experiment and generate chart(s)
`python3 \[experiment name\].py [max_threads]`
### To run all experiments and generate charts
`bash run_exp.sh [max_threads]`
### Notes
* Experiments in the paper are run on an Intel machine with 90+ cores, 2 threads per core (180+ hyperthreads in total)
* A machine with 40+ hyperthreads should be enough to reproduce the main results of the paper.
* All parameters (thread count, range query size, etc.) are set in the python scripts.
* Graphs generated from this command include all graphs in the paper.
* Generated data and charts found in microbench_experiments/augmented_trees/data_* folders.
* Expected generated pdfs found in microbench_experiments/augmented_trees/expected_charts folder.
* An extra scal_size chart is generated. This chart is not included in the paper or the expected_charts folder.
## Modifying Existing Experiments
All experiments are run through python files in the `microbench_experiments/augmented_trees` folder. `plot_func` controls the appearance of the graph. Which run parameters (ex: Max key, # of trials) are varied is controlled by the lists in the `add_run_params` calls. Any varying parameter that is not the data structure name or what is varied on the x-axis has to be included in the `name` and `varying_cols_list` parameters of the `add_plot_set` function. See [here](https://gitlab.com/trbot86/setbench/-/tree/master/microbench_experiments/tutorial) for a detailed explanation of the benchmarking framework. For running individual experiments with various run parameters without creating graphs, it is easier to follow [these steps](#running-individual-experiments).
## Other Experimental Claims
Claims about the average number of nodes seen per propagate call can be viewed in the output of executions (either in the terminal for individual runs or in the dataxxxxxx.txt files inside data_* folders from runs of the python scripts). The result is located in the line starting with `Avg_nodes_per_propagate=`. 
