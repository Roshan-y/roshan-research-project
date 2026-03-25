#!/bin/bash

#########################################################################
#### Experiment configuration
#########################################################################

t="30000"
num_trials=1
key_range_sizes="200000 2000000 20000000"
thread_counts=`cd .. ; ./get_thread_counts.sh`
alg="brown_ext_chromatic_augment_lf"
ops="min max"

# For a quick smoke test, uncomment the following:
# t="3000"
# key_range_sizes="2000000"
# thread_counts=`cd .. ; ./get_thread_count_max.sh`

echo "$thread_counts"
#########################################################################
#### Produce header
#########################################################################

timeout_s=600
exp="`pwd | rev | cut -d'/' -f1 | rev`"

mkdir $exp 2>/dev/null
../parse.sh null > $exp.csv
cat $exp.csv

step=10000
maxstep=$step
pinning_policy=`cd .. ; ./get_pinning_cluster.sh`

#########################################################################
#### Run trials
#########################################################################

started=`date`
for counting in 1 0 ; do
    for ((trial=0;trial<num_trials;++trial)) ; do
        for runtime in $t ; do
            for op in $ops ; do
                for k in $key_range_sizes ; do
                    for n in $thread_counts ; do
                        if ((counting)); then
                            maxstep=$((maxstep+1))
                        else
                            step=$((step+1))
                            if [ "$#" -eq "1" ]; then ## check if user wants to just replay one precise trial
                                if [ "$1" -ne "$step" ]; then
                                    continue
                                fi
                            fi

                            f="$exp/step$step.txt"
                            args="-nwork $n -nprefill $n -prefillfrac 0.5 -i 0 -d 0 -rq 0 -rk 0 -sel 0 -k $k -t $runtime -pin $pinning_policy"
                            if [ "$op" = "min" ]; then
                                args="$args -min"
                            else
                                args="$args -max"
                            fi
                            cmd="LD_PRELOAD=../../../lib/libjemalloc.so timeout $timeout_s numactl --interleave=all time ../../bin/ubench_${alg}.alloc_new.reclaim_debra.pool_none.out $args"
                            echo "cmd=$cmd" > $f
                            echo "step=$step" >> $f
                            echo "fname=$f" >> $f

                            eval $cmd >> $f 2>&1
                            if [ "$?" -ne "0" ]; then
                                cat $f
                            fi

                            ## manually parse the maximum resident size from the output of `time` and add it to the step file
                            maxres=`../grep_maxres.sh $f 2> /dev/null`
                            echo "maxresident_mb=$maxres" >> $f

                            ## parse step file to extract fields of interest
                            ../parse.sh $f | tail -1 >> $exp.csv
                            echo -n "step $step/$maxstep: "
                            cat $exp.csv | tail -1
                        fi
                    done
                 done
            done
        done
    done
done

echo "started: $started" | tee "time_started.txt"
echo "finished:" `date` | tee "time_finished.txt"

zip -r ${exp}.zip ${exp} ${exp}.csv *.sh
rm -f data.csv 2> /dev/null # clean up after parse.sh
