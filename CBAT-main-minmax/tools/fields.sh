#!/bin/bash

if [ "$#" -le "0" ]; then
	echo "USAGE: $(basename $0) TEXTFILE_NAME COLUMN_SEARCH_STRING [[COLUMN_SEARCH_STRING] ...]"
	echo " note: this tool extracts columnar data from lines of the format 'key=data'"
	echo " (can also pipe stdin to this)"
	exit 1
fi

# if [ "$#" -eq "2" ]
# then
#     cat -u $2 | grep --line-buffered "$1" | grep --line-buffered "=" | stdbuf -oL cut -d"=" -f2
# else
#     cat -u /dev/stdin | grep --line-buffered "$1" | grep --line-buffered "=" | stdbuf -oL cut -d"=" -f2
# fi

if [ -t 0 ] ; then
	# echo running interactively (NO PIPE, SO EXPECT FILE!!)
	fname="$1"
	shift
	dodelete=0
else
	# echo NO FILE EXPECTED.. rather, we expect stdin pipe
	fname="/tmp/__temp_fields_$(cat /dev/urandom | tr -cd 'a-f0-9' | head -c 32)"
	cp /dev/stdin $fname
	dodelete=1
fi

#printf "%20s " "$fname"
for ((i=$#;i>0;--i)) ; do
	f="$1"
	shift
	searchstr="^${f}="
#	printf "searchstr=$searchstr\n"
	nlines=`cat $fname | grep -E "$searchstr" | wc -l | tr -d " "`
#	if [ "$nlines" -gt "1" ]; then
#		echo "error: $nlines lines returned for grep query [$searchstr]..."
#		cat $fname | grep "$searchstr"
#		exit 1
#	fi
	val=`cat $fname | grep "$searchstr" | tail -1 | cut -d"=" -f2 | tr -d " "`
	if [ "$nlines" -gt "1" ]; then
		val="$val($nlines)"
	fi
#	printf "%20s " "$val"
	printf "$val"
	if ((i>1)) ; then printf " " ; fi
done
printf "\n"

if (( dodelete == 1 )) ; then
	rm $fname
fi
