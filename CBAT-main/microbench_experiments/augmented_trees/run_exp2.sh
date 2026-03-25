#!/usr/bin/bash

for file in $(find $PWD -name '*_exp2.py')
do
    python3 $file
done
