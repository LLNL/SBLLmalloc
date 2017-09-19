#!/bin/bash
RM="rm -f"
KILL="skill -9 -c"
BASE=/home/biswas3/LLNL_WORK/ptmalloc/tests/misc/sbllmalloc

# take env vars
#DEBUG : set to 1 for enabling debug
#MERGE_METHOD: 0 for disabling merges, 1 for frequency based merges and 2 for threshold based merges
#FREQ: define merge frequency if merge method is 1
#TH: define threshold if merge nethod is 2
#PROF: use 1 for creating profile, 2 for profile guided run (DO NOT USE NOW)
#ENABLE_BACKTRACE: Enable backtrace option to find the source locations (DO NOT USE NOW)
#MPIRUN: define mpirun command. Default is srun -n16 -N4


if [ "x$DEBUG" == "x" ]; then DEBUG=0; fi
if [ "x$MERGE_METHOD" == "x" ]; then MERGE_METHOD=0; fi
if [ "x$FREQ" == "x" ]; then FREQ=10; fi
if [ "x$TH" == "x" ]; then TH=10; fi
if [ "x$PROF" == "x" ]; then PROF=0; fi
if [ "x$ENABLE_BACKTRACE" == "x" ]; then ENABLE_BACKTRACE=0; fi
if [ "x$MPIRUN" == "x" ]; then MPIRUN="srun -n16 -N4"; fi
#change MPIRUN accordingly in env var

SETARCH="setarch `uname -m` -3 -R "
COMMANDLINE=$@

date=`date +%H%M%S`
cmdfile="runcmd.$date.sh"

MALLOCLIB="$BASE/libsbllmalloc.so"

############ create run script
$RM $cmdfile
echo "#!/bin/bash

$KILL $1
$RM /dev/shm/PSMallocTest /dev/shm/sem.*
sleep 2
LD_PRELOAD=$MALLOCLIB PROFILE_MODE=$PROF PTDEBUG=$DEBUG MERGE_METRIC=$MERGE_METHOD MALLOC_MERGE_FREQ=$FREQ MIN_MEM_TH=$TH ENABLE_BACKTRACE=$ENABLE_BACKTRACE $COMMANDLINE
$KILL $1
$RM /dev/shm/PSMallocTest /dev/shm/sem.*

" > $cmdfile

chmod +x $cmdfile


############ create run script
COMMANDLINE="./$cmdfile"

$MPIRUN $COMMANDLINE
$RM $cmdfile

