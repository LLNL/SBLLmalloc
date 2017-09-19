#!/bin/bash

#set -o verbose
RM="/bin/rm -f"
KILL="skill -9 -c"
BASE=/home/biswas3/LLNL_WORK/ptmalloc/tests/misc/sbllmalloc
DATE=`date +%H%M%S`

MALLOCLIB="$BASE/lib/libsbllmalloc.so"

############ create run script
function create_run_script()
{
	cmdfile=$1;
	$RM $cmdfile;
	echo "#!/bin/bash

LD_PRELOAD=$MALLOCLIB PROFILE_MODE=$PROF PTDEBUG=$DEBUG MERGE_METRIC=$MERGE_METHOD MALLOC_MERGE_FREQ=$FREQ MIN_MEM_TH=$TH ENABLE_BACKTRACE=$ENABLE_BACKTRACE $COMMANDLINE
#$COMMANDLINE
	" > $cmdfile;
	chmod +x $cmdfile;
}

# take env vars
#DEBUG : set to 1 for enabling debug
#MERGE_METHOD: 0 for disabling merges, 1 for frequency based merges and 2 for threshold based merges
#FREQ: define merge frequency if merge method is 1
#TH: define threshold if merge nethod is 2
#PROF: use 1 for creating profile, 2 for profile guided run (DO NOT USE NOW)
#ENABLE_BACKTRACE: Enable backtrace option to find the source locations (DO NOT USE NOW)
#MPIRUN: define mpirun command. Default is srun -n16 -N4


if [ "x$DEBUG" == "x" ];			then DEBUG=0; fi
if [ "x$MERGE_METHOD" == "x" ];		then MERGE_METHOD=0; fi
if [ "x$FREQ" == "x" ];				then FREQ=10; fi
if [ "x$TH" == "x" ];				then TH=10; fi
if [ "x$PROF" == "x" ];				then PROF=0; fi
if [ "x$ENABLE_BACKTRACE" == "x" ]; then ENABLE_BACKTRACE=0; fi
if [ "x$MPIRUN" == "x" ]; 			then MPIRUN="srun -n16 -N4"; fi
#change MPIRUN accordingly in env var

COMMANDLINE=$@


MPIRUN_PROG=`echo $MPIRUN | awk '{print $1}'`

if [ $MPIRUN_PROG == "srun" ]
then
NumCores=`echo $MPIRUN | grep -o -w "\-n[[:digit:]]*"`;
NumNodes=`echo $MPIRUN | grep -o -w "\-N[[:digit:]]*"`;

export VIADEV_USE_SHARED_MEM=0
export VIADEV_NUM_RDMA_BUFFER=0
export VIADEV_USE_SRQ=0
create_run_script "runcmd.$DATE.sh"
echo "#!/bin/bash
pkill -9 $1;
rm -f /dev/shm/*
" > cleanup.sbllmalloc.sh
chmod +x cleanup.sbllmalloc.sh

echo "#!/bin/bash

export VIADEV_USE_SHARED_MEM=0
export VIADEV_NUM_RDMA_BUFFER=0 
export VIADEV_USE_SRQ=0
#export MV_USE_SHARED_MEMORY=0 
$MPIRUN ./cleanup.sbllmalloc.sh
#sleep 5
$MPIRUN runcmd.$DATE.sh;
#sleep 5
$MPIRUN ./cleanup.sbllmalloc.sh
rm -f runcmd.$DATE.sh
rm -f cleanup.sbllmalloc.sh
" > /tmp/runcmd.$DATE.sh
chmod +x /tmp/runcmd.$DATE.sh
salloc $NumCores $NumNodes /tmp/runcmd.$DATE.sh
rm -f /tmp/runcmd.$DATE.sh

exit;

else # something else which is not implemented yet

echo "yet to implement wrapper for $MPIRUN_PROG!";
echo "failed to execute $MPIRUN $COMMANDLINE";
exit 1;

fi

exit
