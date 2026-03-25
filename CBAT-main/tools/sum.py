#!/usr/bin/python3

import sys

sum=0
cnt=0
for line in sys.stdin:
    val = float(line)
    cnt+=1
    sum+=val

print(sum)