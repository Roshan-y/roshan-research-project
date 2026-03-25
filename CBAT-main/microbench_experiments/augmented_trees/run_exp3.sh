#!/usr/bin/bash

if [ $# != 1 ]
  then
    echo "Need exactly one command line argument for maximum number of threads"
    exit 1
fi

for file in $(find $PWD -name '*_exp3.py')
do
    python3 $file $1
done
