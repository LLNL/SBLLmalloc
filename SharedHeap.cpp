/*!
  @file SharedHeap.cpp
  @author Susmit Biswas
  @version 1.0
  @date 2009-2010
  
  @brief Implementation of memory allocator using shared memory. 
  
  @details The public
  interfaces are defined in \c Globals.h. In this file two functions - \c MPI_Init()
  and \c MPI_Finalize() override the functions from MPI library.
  Also, the malloc calls from the internal malloc (ptmalloc v3 used in this
  package) replace the system \c malloc(),\c free(), \c realloc() calls. If you
  want to use a different internal malloc, you need to make the public routines
  (malloc, calloc, free etc.) as wrappers and move the code to some internal
  routine. After that define those internal routines in a file that you need to
  include in Globals.h. For example, check out \c internal-routines.h which
  defines the \c internal_malloc(), \c internal_free() etc. 
  internal_malloc.h is included in \c Globals.h.

  Memory usage in a demand paging system increases when the pages get written
  for the first time or a shared page becomes private. In the threshold based
  merge technique, at the end of SigSegvHandler, \c MergeByTHRESHOLD() routine
  is called with iteratively calls MergeNode2() by traversing the AVL tree. If
  a AVL tree node is dirty, \c MergeManyPages() routine is called which merges
  identical pages from that node. In \c MergeManyPages() many pages are handled
  at once i.e. their permission bits are changed, they are mapped/unmapped
  in-order to reduce overhead. \c FLUSH_OUTSTANDING_MERGES() is used in this
  state machine which keeps contiguous page addresses using a \em start and \em
  size field.


  @pre 
	  - There are at most x MPI tasks per node where x is the number of cores.
	  - There is no need to rely on MPI task rank. rather use shared memory value as rank.
	  - OS ensures that \c MAP_FIXED or \c MREMAP_FIXED replaces previous mappings (tested to be true in Ubuntu linux).
  @todo 
	- Modify backtrace capability:
		-# Allocate an array for each of the AVL node instead of just creator (Done)
		-# Store 20 entries from the callstack (Done)
		-# At every merge point: find what pages are merged and what is the source i.e. the creator (Done)
		-# Dump out the addresses. (Done)
		-# After exit, run \c addr2line to find out the stack traces (Done)
		-# Report top contributors as a graph.
	- Recover from mmap failure 
		-# We really cannot recover, we can just keep our finger crossed that no other library calls mmap.
	- Support non-MPI apps by changing \c MPI_Init to a routine that runs at program startup 
		-# Use \c __attribute__ \c ((constructor)) \c TheStartRoutineForNonMPIapp();
 */

#include "SharedHeap.h"

/*===============================================================================*/
/*                                Global Variables                               */ 
/*===============================================================================*/

static bool	isMPIInitialized = false; 	/**< Flag indicating whether mpi is initialized */
static bool	isMPIFinalized = false; 	/**< Flag indicating whether mpi is finalized */
static int maxMmapCount = 65536;		/**< OS limit on max mmaps */
static int mmapCount = 0;				/**< Used for keeping track of mmap counts and checking limits */
static int myRank = -1;					/**< Rank of current task */
static int numProc = 0;					/**< Number of processes in local node */
static int sharedFileDescr = -1; 		/**< mmapped file used for sharing */
static int *sharingProcessesInfo = NULL;/**< Bitvectors for indicating sharing status of pages */
static unsigned long currProcMask = 0x01;/**< Used for faster bitwise ops, created from myRank */
static unsigned long currProcMaskInverted =(unsigned long)(-1); /**< Used for faster bitwise ops, created from myRank */
static int notMPIApp = 0;				/**< A stand alone program needs to define corresponding env var */
/*------------------------ Merge Controller ---------------------------------*/
static int mergeMetric = THRESHOLD; 	/**< How to perform merge? allocation frequency/threshold/buffer */
static int mergeMinMemTh = 10000;		/**< Threshold value for threshold based merge */
static int mallocRefFreq = MALLOC_REF_FREQ; /**< Frequency of merging */
static unsigned long mallocRefCounter = 0; 	/**< Counts the # of mallocs to trigger merging */
/* susmit added on 06/29/2009 */
static uintptr_t bufferOfDirtyPages[BUFFER_LENGTH]; /**< In buffer based approach, stores the dirty pages addr */
static int bufferPtr = 0; 				/**< Indicates where we should put the dirty page address in the buffer */
/*------------------------ Profile Controller ---------------------------------*/


#ifdef ENABLE_PROFILER
static int profileMode = NONE; 			/**< Whether we are creating profile or using it for a profile based run */
static FILE *profFile = NULL; 			/**< Profile file */
#endif /* ENABLE_PROFILER */

/*------------------------ Backtracking tools ---------------------------------*/
static uintptr_t lowLoadAddr = (unsigned long)(-1); /**< Max value of lib loaded address*/
static uintptr_t highLoadAddr = 0;	 	/**< Min value of lib loaded address */
static int enableBacktrace = 0; 		/**< Find the sources of merging regions i.e. who created the region*/

/*------------------------ Library management core components ---------------------------------*/
static key_t semKey = 1234;				/**< Key used to create a new semaphore name */
static char	semName[200]; 				/**< Used for posix semaphore */
static sem_t *mutex = NULL; 			/**< Semaphore used for coherence */
static AVLTreeData *allocRecord = NULL; /**< Avl tree used to keep track of allocated regions */
static int *aliveProcs = NULL; 			/**< Number of active processes */
static int PAGE_SIZE = 4096; 			/**< 4 KB default page */
static unsigned log2PAGE_SIZE = 12; 	/**< 4 KB default page */

#ifdef SHARED_STATS
static int *sharedPageCount = NULL; 	/**< Number of pages shared */
static int *allProcPrivatePageCount = NULL; /**< Total number of private pages across all tasks */
static int *baseCaseTotalPageCount = NULL; /**< Total number of pages in base case */
#endif /* !SHARED_STATS */

#ifdef PART_BLOCK_MERGE_STAT
int partBlockStat[8];			 		/**< Address of array if we want to find partial similarity stat */
static int localDiffPageCount = 0;		/**< Difference in number of bytes */
static int localComparedPageCount = 0;	/**< Count of pages different */
static int localSharedPageCount = 0; 	/**< Number of shared pages for current process */
static int localZeroPageCount = 0; 		/**< Number of zero pages for current process */
static int localPageCount = 0; 			/**< Number of pages for current process */
#endif /* !PART_BLOCK_MERGE_STAT */

#ifdef COLLECT_MALLOC_STAT
static char initializedPagesBV[98304]; 	/**< 3GB, 1 bit per page i.e. 0.75/8 MB*/
#endif /* COLLECT_MALLOC_STAT */
static char zeroPagesBV[98304]; 		/**< Is it a zero page, 3GB, 1 bit per page i.e. 0.75/8 MB*/

#if defined __x86_64__
static bool isHeapBoundaryInitialized = false; 	/**< Flag indicating if heap boundaries are intitialized */
static uintptr_t sharedHeapBottom = 0x7fff40000000; /**< (128TB i.e. 0x800000000000) - 3GB */
static uintptr_t sharedHeapTop = 0x7fffffffffff;	/**< (128TB i.e. 0x800000000000) - 3GB */
#endif

static char *zeroPage = NULL;			/**< Addr of zero page */
static int zeroPageCount = 0; 			/**< Number of zero pages for current task */
static FILE	*outFile = NULL; 			/**< Output file for storing results */

#ifdef PRINT_STATS
static int maxBaseCaseTotalPageCount = 0; /**< Total amount of memory used with
											default allocator, counted even if
											merge is disabled */
#endif /* PRINT_STATS */

/*! @brief Profile guided merge: \b disabled*/
//#define PROFILE_BASED_MERGE
#define PROF_MERGE_VERSION 2

#ifdef PROFILE_BASED_MERGE
/* for every page, I need an int for storing the time of last merge and a byte for the history */
/*********************************************************************************
* Time taken to operate on 1GB of memory:
* mmap + many mprotects (1/page)	: 2420512.000000 μs 
* mmap + one mprotect				: 1270981.000000 μs 
* mmap only							: 1227078.000000 μs 
*
* Reducing the mprotect count will benefit a lot. 
*
* Similar results from another set of experiments in the following for every page.
* mmap + mremap + mprotect + memcmp + mprotect: 8.093800 μs
*                       mmap + mremap + memcmp: 1.902600 μs
*                                mmap + mremap: 1.678000 μs
*                                    mmap only: 0.273900 μs
*
* Crux is that reducing the mprotects will save a lot.
* Therefore, using buffers to map and unmap regions and operating on bulk.
**********************************************************************************/


const static uint32_t mergeCostPerPage = 8000; /*< ~5 us for mmap+mprotect+memcmp+mprotect. ~8ms for merging 1GB */
static uint8_t *mergeSuccHist = NULL; /**< Stores merge success as a history */
static uint32_t *lastMergeTime = NULL; /**< Stores the last merge time in
										 microsec. At the sighandler, the time
										 difference is recorded to decide the
										 success of merge.  Supports 1hr gap...
										 good enough */
											
#endif /* PROFILE_BASED_MERGE */

#ifdef MICROTIME_STAT
static unsigned long mergeTime = 0; /**< Time spent in merge operation */
static unsigned long allocTime =0; /**< Time spent in malloc operation */
static unsigned long freeTime =0; /**< Time spent in free operation */
static unsigned long bitOpTime =0; /**< Time spent in bitwise operations */
static unsigned long compareTime =0; /**< Time spent in compare operation */
static unsigned long sigHandlerTime =0; /**< Time spent in unmerge operation */
#endif /*MICROTIME_STAT */

#ifdef REPORT_MERGES
static int totalProcessedPages = 0; /**< Total number of pages checked in current merge */
static int newlyMovedPages = 0; /**< Total number of pages newly moved to shared region */
static int newZeroPages = 0; /**< Total number of new zero pages */
static int newlyMergedPages = 0; /**< Total number of new merged pages */
static int numDirtyPages = 0; /**< Total number of dirty pages */
static int numCleanPages = 0; /**< Total number of clean pages */
#endif /* REPORT_MERGES */


/*! @brief Size of buffer used for storing memory usage stats */
#define MAX_MERGES 10000

/*! @brief Buffer for storing memory usage stats */
static MemStatStruct memStat[MAX_MERGES];

/*! @brief Currnt index in the buffer for storing memory usage stats */
static int memStatCounter = 0;

/*==================================================================*/
/*                  Merge Stats Collection Routines                 */
/*==================================================================*/
/* stores stats in the buffer */
inline void UpdateMergeStat(
		const long int tpm, 
		const long int tptm, 
		const long int tzm, 
		const long int tsm, 
		const long int tum, 
		const long int tmm, 
		const int mtm){

	if(memStatCounter == MAX_MERGES){
		PrintMergeStat();
		memStatCounter = 0;
	}

	memStat[memStatCounter].totalPrivateMem		= tpm;
	memStat[memStatCounter].totalPtmallocMem	= tptm;
	memStat[memStatCounter].totalZeroMem		= tzm;
	memStat[memStatCounter].totalSharedMem		= tsm;
	memStat[memStatCounter].totalUnmergedMem	= tum;
	memStat[memStatCounter].totalMergedMem   	= tmm; 
	memStat[memStatCounter].mergeTimeinMicrosec = mtm;
	memStatCounter++;
}

/* flushes merge stat buffer to file */
inline void PrintMergeStat(){
	if(!outFile)
		return;

	for (int i = 0; i < memStatCounter; i++) {
		fprintf(outFile, "P: %16lu; L: %16lu; Z: %16lu; S: %16lu; U: %16lu; M: %16lu\n",
				memStat[i].totalPrivateMem,
				memStat[i].totalPtmallocMem,
				memStat[i].totalZeroMem,
				memStat[i].totalSharedMem,
				memStat[i].totalUnmergedMem,
				memStat[i].totalMergedMem
			   );
	}
}


/* Stores memory usage stats */
void StoreMemUsageStat(){

#ifdef PRINT_STATS
	if(!outFile)
		return;
	unsigned long private_mem 			= ptmalloc_get_mem_usage();

	unsigned long total_private_mem 	= (long unsigned)private_mem * (*aliveProcs) 
											+ (long unsigned)(*allProcPrivatePageCount) * PAGE_SIZE ;
	unsigned long total_ptmalloc_mem	= (long unsigned)private_mem * (*aliveProcs);
	unsigned long total_zero_mem 		= (long unsigned)zeroPageCount * PAGE_SIZE;
#ifdef SHARED_STATS
	unsigned long total_shared_mem		= (long unsigned)(*sharedPageCount) * PAGE_SIZE;
	unsigned long total_unmerged_mem 	= (long unsigned)private_mem * (*aliveProcs) 
											+ (long unsigned)(*baseCaseTotalPageCount) * PAGE_SIZE;
	unsigned long total_merged_mem 		= (long unsigned)private_mem * (*aliveProcs) 
											+ (long unsigned)(*allProcPrivatePageCount + *sharedPageCount) * PAGE_SIZE;
#else
	unsigned long total_shared_mem		= 0;
	unsigned long total_unmerged_mem 	= (long unsigned)private_mem * (*aliveProcs);
	unsigned long total_merged_mem 		= (long unsigned)private_mem * (*aliveProcs); 
#endif /* !SHARED_STATS */
	UpdateMergeStat(total_private_mem, total_ptmalloc_mem, total_zero_mem, 
			total_shared_mem, total_unmerged_mem, total_merged_mem, 0);
#endif /* PRINT_STATS */
}


/*===============================================================================*/
/*                              Initialization Codes                             */
/*===============================================================================*/

//void __attribute__ ((constructor)) Init_Heap_Boundary();
#if defined __x86_64__
/* get the top of the heap and find if it grows upwards or downwards */
void Init_Heap_Boundary(){
	int saved_errno = errno;
	errno = 0;

//	fprintf(stderr, "Init_Heap_Boundary called\n");
	char *ptr1 = (char *) SH_MMAP(NULL, PAGE_SIZE, PROT_READ, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	char *ptr2 = (char *) SH_MMAP(NULL, PAGE_SIZE, PROT_READ, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	bool heap_grows_downwards = true;

	if((uintptr_t)ptr1 > (uintptr_t)ptr2){ /* heap grows downwards */
		sharedHeapTop = (uintptr_t)ptr1 + PAGE_SIZE;
		sharedHeapBottom = sharedHeapTop - 0xc0000000; /* (128TB i.e. 0x800000000000) - 3GB */
	}else{
		sharedHeapBottom = (uintptr_t)ptr1;
		sharedHeapTop = sharedHeapBottom + 0xc0000000; /* (128TB i.e. 0x800000000000) - 3GB */
		heap_grows_downwards = false;
	}

	ASSERTX(SH_UNMAP(ptr1, PAGE_SIZE) == 0);
	ASSERTX(SH_UNMAP(ptr2, PAGE_SIZE) == 0);
	errno = saved_errno;
	isHeapBoundaryInitialized = true;
}
#endif /* __x86_64__ */


/* Overrrides MPI_Init() */
int MPI_Init(int *argc, char ***argv){
	int saved_errno = errno;
	errno = 0;

	setvbuf (stdout, NULL , _IONBF , 1024 ); // very important for keeping OS buffers away from messing up alignments.
	setvbuf (stderr, NULL , _IONBF , 1024 ); // very important for keeping OS buffers away from messing up alignments.
//	fprintf(stderr, "MPI_Init called\n");
#if defined __x86_64__
	if(!isHeapBoundaryInitialized)
		Init_Heap_Boundary();
#endif /* __x86_64__ */

	int ret_val = PMPI_Init(argc, argv);
	InitAddrSpace(); /* set flag here */

	char out_filename[200]; /* output file */
	char hostname[100];
	bool gen_output = false;

	if(gethostname(hostname, 100) == 0){ 
		sprintf(out_filename, "memusage.%s.%d", hostname, myRank);
		if(!myRank)
			gen_output = true;
	}else{ /* not successful*/
		int taskrank;
		MPI_Comm_rank(MPI_COMM_WORLD, &taskrank);
#ifdef PRINT_DEBUG_MSG
		warn("unable to determine hostname, using absolute task rank\n");
#endif /* PRINT_DEBUG_MSG */
		sprintf(out_filename, "memusage.%d", taskrank);
		if(!taskrank)
			gen_output = true;
	}

#ifdef PRINT_STATS
	if(gen_output && (mergeMetric != MERGE_DISABLED))
		outFile = fopen(out_filename, "w");
#endif /* PRINT_STATS */

#ifdef PRINT_DEBUG_MSG
	fprintf(stderr, "  process rank %d done\n", myRank);
	fflush(stderr);
#endif /* PRINT_DEBUG_MSG */
	isMPIInitialized = true;

	errno = saved_errno;
	return ret_val;
}

/* Replaces MPI_Finalize() */
int MPI_Finalize(){
	int saved_errno = errno;
	errno = 0;
	int ret_val = 0;

	isMPIFinalized = true;
#ifdef PRINT_STATS
	if(outFile){
		PrintMergeStat();
		fclose(outFile);
	}
#endif /* PRINT_STATS */
	outFile = NULL;
	ret_val = PMPI_Finalize();

#ifdef PRINT_STATS
	if(!myRank)
		fprintf(stderr, "Max Mem Usage Per Node: %ld\n", (maxBaseCaseTotalPageCount) * (long)PAGE_SIZE);
#endif /* PRINT_STATS */
	errno = saved_errno;
	return ret_val;
}

/* initializes the library */
void InitAddrSpace(){
	int saved_errno = errno;
	errno = 0;

	/* find the max number of mmaps supported by the OS */
	FILE *mmap_lim_file = fopen("/proc/sys/vm/max_map_count", "r");
	if(mmap_lim_file){
		fscanf(mmap_lim_file, "%d", &maxMmapCount);
		fclose(mmap_lim_file);
	}else{
		warn("could not open /proc/sys/vm/max_map_count, using default 64K\n");
	}

	/* find system page size */
	errno = 0;
	PAGE_SIZE = sysconf(_SC_PAGESIZE);
	ASSERTX((PAGE_SIZE > 0) && (PAGE_SIZE <= (0x01<<20)));
	log2PAGE_SIZE = CeilLog2(PAGE_SIZE);

	/* obtain environment variables to set internal knobs */
	InitEnv();
#ifdef PRINT_DEBUG_MSG
	fprintf(stderr, "environment initialized\n");
#endif /* PRINT_DEBUG_MSG */
	CheckForError();

	/* create avl tree for storing allocations */
	allocRecord = (AVLTreeData*) CreateAVL(MyComparator);
#ifdef PRINT_DEBUG_MSG
	fprintf(stderr, "created avl\n");
#endif /* PRINT_DEBUG_MSG */
	ASSERTX(allocRecord != NULL);
	CheckForError();

	/* get the number of cores in the node */
	errno = 0;
	numProc = sysconf( _SC_NPROCESSORS_ONLN );

	if(numProc <= 8) 		numProc = 8;
	else if(numProc <= 16) 	numProc = 16;
	else die("error: More number of cores than supported, rebuild library and link again ... exiting\n");

	if(enableBacktrace){
		/* call to get the memory ranges of loaded library. need to use the addresses in backtrace */
		GetMemRange(); 
#ifdef PRINT_DEBUG_MSG
		fprintf(stderr, "Library loaded between %p and %p\n", (void *)lowLoadAddr, (void *)highLoadAddr);
#endif /* PRINT_DEBUG_MSG */
	}
	CheckForError();

	/* create semaphore */
	sprintf(semName, "/tmpname%d", semKey);
#ifdef PRINT_DEBUG_MSG
	fprintf(stderr, "Semaphore initialization ... ");
#endif /* PRINT_DEBUG_MSG */
	InitSem(semName, &mutex);
#ifdef PRINT_DEBUG_MSG
	fprintf(stderr, "done\n");
#endif /* PRINT_DEBUG_MSG */


	//
	// total time taken in allocating shared metadata is ~4.5 msec for the
	// first time and ~0.15 msec from next call. We can ignore it from our
	// overhead estimation.
	//
	/* open shared file and map pointers */
	AllocateSharedMetadata();
#ifdef PRINT_DEBUG_MSG
	fprintf(stderr, "shared data allocated\n");
	fprintf(stderr, "sharedHeapTop: %20p\n", (void*)sharedHeapTop);
	fprintf(stderr, "shared_heap_bot: %20p\n", (void*)sharedHeapBottom);
#endif /* PRINT_DEBUG_MSG */

	CheckForError();

#ifdef ENABLE_PROFILER
	errno = 0;
	char fileName[32];
	sprintf(fileName, "profile.%d", myRank);
	switch(profileMode){
		case CREATE_PROF:
			profFile = fopen(fileName, "w");
			ASSERTX(profFile);
			setvbuf (profFile, NULL , _IONBF , 1024 );
#ifdef PRINT_DEBUG_MSG
			fprintf(stderr, "%s opened in profiling mode\n", fileName);
#endif /* PRINT_DEBUG_MSG */
			break;
		case USE_PROF:
			profFile = fopen(fileName, "r");
#ifdef PRINT_DEBUG_MSG
			fprintf(stderr, "%s opened for using profile\n", fileName);
#endif /* PRINT_DEBUG_MSG */
			break;
		default: /* no profiling */
#ifdef PRINT_DEBUG_MSG
			fprintf(stderr, "profiling disabled\n");
#endif /* PRINT_DEBUG_MSG */
			break;
	}
#endif /* ENABLE_PROFILER */


#ifdef PROFILE_BASED_MERGE
	mergeSuccHist = (uint8_t*)  SH_MMAP(NULL, 0x03 << (30 - log2PAGE_SIZE),     PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0); // 3GB/4KB i.e. 1B per 4KB page
	lastMergeTime = (uint32_t*) SH_MMAP(NULL, 0x03 << (30 - log2PAGE_SIZE + 2), PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0); // 4B per 4KB page
	ASSERTX(mergeSuccHist != MAP_FAILED);
	ASSERTX(lastMergeTime != MAP_FAILED);
	memset(mergeSuccHist, 0, 0x03 << (30 - log2PAGE_SIZE));
	memset(lastMergeTime, 0, 0x03 << (30 - log2PAGE_SIZE+2));
#endif /* PROFILE_BASED_MERGE */

	/* install SIGSEGV handler */
	errno = 0;
	{ 
		struct sigaction act;
		act.sa_sigaction = SigSegvHandler;
		sigemptyset(&act.sa_mask);
		act.sa_flags = SA_RESTART | SA_SIGINFO;
		ASSERTX(sigaction(SIGSEGV, &act, NULL) == 0);
	}
	
	errno = 0;
	{ 
		struct sigaction act;
		act.sa_sigaction = SigBusHandler;
		sigemptyset(&act.sa_mask);
		act.sa_flags = SA_SIGINFO;
		ASSERTX(sigaction(SIGBUS, &act, NULL) == 0);
	}
	errno = 0;
	/*
	{ 
		struct sigaction act;
		act.sa_sigaction = SigIntHandler;
		sigemptyset(&act.sa_mask);
		act.sa_flags = SA_SIGINFO;
		ASSERTX(sigaction(SIGINT, &act, NULL) == 0);
	}
	*/
#ifdef COLLECT_MALLOC_STAT
	memset(initializedPagesBV, 0, 98304);
#endif
	
	memset(zeroPagesBV, 0, 98304);
	atexit(CleanUpSharedData);

	errno = saved_errno;
}


/*-------------------------------------------------------------------------------*/
/* allocates shared data, metadata and initializes them*/
void AllocateSharedMetadata(){
	bool init_shared = false;

	int saved_errno = errno;
	errno = 0;

	{

		WaitSem(mutex);
#ifdef PRINT_DEBUG_MSG
		fprintf(stderr, "obtained sem\n");
#endif /* PRINT_DEBUG_MSG */

		/* open POSIX shared file */
		char shm_name[] = "/PSMallocTest";
		sharedFileDescr = shm_open(shm_name, O_CREAT|O_EXCL|O_RDWR|O_TRUNC, S_IRUSR|S_IWUSR);

		if(sharedFileDescr != -1){
			init_shared = true; // nobody initialized data yet 
		} else if((sharedFileDescr == -1) 
				&& (errno == EEXIST)){
			sharedFileDescr = shm_open(shm_name, 
					O_RDWR, 
					S_IRUSR|S_IWUSR);
		} else{
			SignalSem(mutex);
			perror("shm_open");
			Fatal();
		}
#ifdef PRINT_DEBUG_MSG
		fprintf(stderr, "opened shared file\n");
#endif /* PRINT_DEBUG_MSG */

		/* initialized POSIX shared file */
		errno = 0;
		if(init_shared){
#ifdef PRINT_DEBUG_MSG
			fprintf(stderr, "initializing shared metadata\n");
#endif /* PRINT_DEBUG_MSG */
			/* Max 3 GB address space available for mmap + 3 MB for shared metadata + 4KB for alive proc stat and other metadata */
			/* if ftruncate64 changes size, it fills the extended part with 0 */
			if (ftruncate64(sharedFileDescr, ((off64_t)0x03 << 30) | ((off64_t)0x03 << 20 ) | ((off64_t)0x01 << 12 )) < 0) { 
				SignalSem(mutex);
				perror("unable to truncate file\n");
				Fatal();
			}
			CheckForError();
			/* use one page to map all zero pages */
			zeroPage = (char*) SH_MMAP(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, sharedFileDescr, 0);
			memset(zeroPage, 0, PAGE_SIZE);
			MakeReadOnlyWrapper(zeroPage, PAGE_SIZE); /* make the zero page read only to prevent errors */
		}else{ /* someone already initialized, so map it as read only */
			zeroPage = (char*) SH_MMAP(NULL, PAGE_SIZE, PROT_READ, MAP_SHARED, sharedFileDescr, 0);
		}

		ASSERTX(zeroPage != MAP_FAILED);

		/* attach 3MB after 3GB user space of the shared file as shared memory
		 * for storing metadata */

		sharingProcessesInfo = (int*) SH_MMAP(NULL, 
				0x03 << 20, /* map 3MB */
				PROT_READ | PROT_WRITE, /* with RDWR */
				MAP_SHARED, /* as shared memory */
				sharedFileDescr, 
				(((off64_t)0x03) << 30) /* at the end of 3GB */
				); /* assuming pagesize of 4096 B*/
		ASSERTX(sharingProcessesInfo != MAP_FAILED);

		/* use the a page for storing alive proc info */
		aliveProcs = (int *) SH_MMAP(NULL, 
				PAGE_SIZE, 
				PROT_READ | PROT_WRITE, 
				MAP_SHARED, 
				sharedFileDescr,
				(((off64_t)0x03 << 30) | ((off64_t)0x03 << 20 ))
				);

		ASSERTX(aliveProcs != MAP_FAILED);

#ifdef SHARED_STATS		
		sharedPageCount 				= (int *) (aliveProcs + 1);
		allProcPrivatePageCount 	= (int *) (aliveProcs + 2);
		baseCaseTotalPageCount 	= (int *) (aliveProcs + 3);
#endif /* !SHARED_STATS	*/

	
		/* uncomment following lines to pin pages in the memory */
//		mlockall(MCL_CURRENT | MCL_FUTURE);	

		CheckForError();

		if(init_shared){
			memset(sharingProcessesInfo, 0, 3 * 1024 * 1024);
			/* num alive process determines how many procs have cleared their data structure */

			if(aliveProcs)
				*aliveProcs = 1;
#ifdef SHARED_STATS	
			if(sharedPageCount)
				*sharedPageCount = 1; /* for the zero page*/

			/* We start with these many shared pages and then grow/shrink according to need*/
			if(allProcPrivatePageCount)
				*allProcPrivatePageCount = 192; /* (3 * 256 * 1024)/4096 */ 

			if(baseCaseTotalPageCount)
				*baseCaseTotalPageCount = 0;
#endif /* !SHARED_STATS */

#ifdef PART_BLOCK_MERGE_STAT
			if(partBlockStat)
				memset(partBlockStat, 0, 8*sizeof(int));
#endif /* !PART_BLOCK_MERGE_STAT */

		}else{
			*aliveProcs +=1;
		}


		{
			myRank = *aliveProcs - 1;
			currProcMask = (0x01) << myRank;
			currProcMaskInverted= ~(currProcMask);
		}
#ifdef PRINT_DEBUG_MSG
		fprintf(stderr, "signalling sem\n");
#endif /* PRINT_DEBUG_MSG */
		SignalSem(mutex);
	}
	errno = saved_errno;
}


/* Aborts execution. Called upon encountering error. */
void Fatal(){
	if(isMPIInitialized)
		CleanUpSharedData();
	fprintf(stderr, "exiting: code %d\n", errno);
	if(isMPIFinalized)
		exit(EXIT_FAILURE);
	else
		MPI_Abort(MPI_COMM_WORLD, MPI_ERR_LASTCODE);
}

/* Gets the memory range where the library is loaded*/
void GetMemRange(){
	int saved_errno = errno;
	errno = 0;
#define LMAX 4096
	char line[LMAX];
	FILE * procmap = fopen("/proc/self/maps", "r");
	uintptr_t number1, number2;
	
	if(procmap != NULL){
		while (fgets(line, LMAX, procmap) != NULL) {
			if(strstr(line, "libptmalloc")){
				sscanf(line, "%lx-%lx", &number1, &number2);
				if(number1 < lowLoadAddr)
					lowLoadAddr = number1;
				if(number2 > highLoadAddr)
					highLoadAddr = number2;
			}
		}
	}else{
		perror("System does not have /proc/self/maps, disabling backtrace\n");
		enableBacktrace = false;
	}
	errno = saved_errno;
}


/* Checks environment variables for sanity */
void CheckEnv(){
	ASSERTX(mergeMetric < NUM_METRIC);
#ifdef ENABLE_PROFILER
	ASSERTX(profileMode < NUM_MODES);
#endif /* ENABLE_PROFILER */
	ASSERTX((mergeMinMemTh > 0) && (mergeMinMemTh < 100000)); /*assuming less than 100GB */
	ASSERTX(mallocRefFreq > 0);
	mergeMinMemTh *= (1000000/PAGE_SIZE);
}


/* Initializes the parameters for the library.*/
void InitEnv(){
	int saved_errno = errno;
	errno = 0;
	commandLineArgument args[] =
	{
#ifdef ENABLE_PROFILER
		{
			"PROFILE_MODE", 
			&profileMode,
			NONE,
			"profiling mode? 0: no profiling(default), 1: create, 2: use profile for merging"
		},
#endif /* ENABLE_PROFILER */
		{
			"ENABLE_BACKTRACE", 
			&enableBacktrace, 
			0,
			"enable backtrace? 1/0(default)"
		},
		{
			"MERGE_METRIC", 
			&mergeMetric, 
			1,
			"merge metric?0(disabled),1(alloc_frequency),2(threshold),3(buffered EXPERIMENTAL): default 1"
		},
		{
			"MIN_MEM_TH", 
			&mergeMinMemTh, 
			10,
			"Minimum amount of memory(in MB) usage to start merging. default 10MB"
		},
		{
			"MALLOC_MERGE_FREQ", /*var id*/
			&mallocRefFreq,   /*var addr*/
			1000,
			"frequency for frequency based merge? default: 1000"
		},
		{
			"NOT_MPI_APP", 
			&notMPIApp, 
			0,
			"define 1 if this does not call MPI_Init(), default 0"
		},
		{
			"SEM_KEY", 
			&semKey, 
			1234,
			"semaphore key, default 1234"
		},
		{ NULL, NULL, 0, NULL}
	};

	for(int i = 0; args[i].name != NULL; i++){
		char *value = NULL;
		if((value = getenv(args[i].name)) != NULL){
			*(args[i].variable) = atoi(value);
		}else{
			*(args[i].variable) = args[i].default_val;
		}
	}

#ifdef PRINT_CONFIG
	for(int i = 0; args[i].name != NULL; i++){
		fprintf(stderr, "%20s:%10d:\t%s\n", args[i].name, *(args[i].variable), (args[i].description));
	}
	fprintf(stderr, "%20s:%10d:\t%s\n", "PAGE_SIZE", PAGE_SIZE, "size of a page");
	fprintf(stderr, "%20s:%10d:\t%s\n", "max mmap count", maxMmapCount, "System limit on the number of mmaps");
#endif /* PRINT_CONFIG */
	CheckEnv();

	errno = saved_errno;
	return;
}



/*===============================================================================*/
/*                                     Misc Routines                             */
/*===============================================================================*/

/* Computes floor(log2(n)) */
inline int FloorLog2(unsigned long n) {
    int p = 0;

    if (n == 0) return -1;

    if (n & 0xffff0000) { p += 16; n >>= 16; }
    if (n & 0x0000ff00)	{ p +=  8; n >>=  8; }
    if (n & 0x000000f0) { p +=  4; n >>=  4; }
    if (n & 0x0000000c) { p +=  2; n >>=  2; }
    if (n & 0x00000002) { p +=  1; }

    return p;
}

/* Computes floor(log2(n)) */
inline int CeilLog2(unsigned long n) {
    return FloorLog2(n - 1) + 1;
}

/*-------------------------------------------------------------------------------*/
/* translate mmapped address to file offset */
inline uintptr_t TranslateMmapAddr(uintptr_t addr){

#if defined __x86_64__
	if((addr > sharedHeapBottom) && (addr < sharedHeapTop)){
		return (addr - sharedHeapBottom);
	} else{
		ReportError(addr);
#ifdef PRINT_DEBUG_MSG
		void *array[100];
		size_t size;
		size = backtrace (array, 10);
		backtrace_symbols_fd(array, size, 2);
#endif /* PRINT_DEBUG_MSG */
		Fatal();
		ASSERTX(false);
		return 0;
	}
#else
	return addr;
#endif
}



/*-------------------------------------------------------------------------------*/
/* Checks if shared malloc can be used */
inline bool CheckMPIInitialized(){
	return (isMPIInitialized || notMPIApp);
}


/* Checks if number of mmap calls are close to system limit.*/
inline bool IsCloseToMmapLimit(int newRequest){
	return false;
//	return ((mmapCount+newRequest) >= maxMmapCount);
}


/*===============================================================================*/
/*                         Semaphore Handling Routines                           */
/*===============================================================================*/



/* Initializes a POSIX semaphore after getting it */
void InitSem(char *SEMKEY, sem_t **mutex){

	int saved_errno = errno;
	errno = 0;
	*mutex = sem_open(SEMKEY, O_CREAT|O_EXCL, S_IRUSR | S_IWUSR, 1);

	if(*mutex == SEM_FAILED){
		if(errno == EEXIST){ /* was created before, just get it */
			*mutex = sem_open(SEMKEY, O_CREAT);
		}else{
			die("unable to open semaphore\n");
		}
#ifdef PRINT_DEBUG_MSG
		fprintf(stderr, " previously created ... ");
#endif /* PRINT_DEBUG_MSG */

	}else{
		int value;
//		ASSERTX(sem_getvalue(*mutex, &value) == 0);
//		ASSERTX(value == 1);
#ifdef PRINT_DEBUG_MSG
		fprintf(stderr, " freshly created ... ");
#endif /* PRINT_DEBUG_MSG */
	}

	if(*mutex == SEM_FAILED)
		die("unable to open semaphore, check code\n");
	ASSERTX(*mutex);

	errno = saved_errno;
}


/*-------------------------------------------------------------------------------*/
/* operation V: used for semaphore handling */
inline void SignalSem (sem_t *mutex) {
//	ASSERTX(mutex);
	int saved_errno = errno;
	errno = 0;
//	int value;
//	ASSERTX(sem_getvalue(mutex, &value) == 0);
//	ASSERTX(value == 0);
	ASSERTX(sem_post(mutex) == 0);
	errno = saved_errno;
} 

/*-------------------------------------------------------------------------------*/
/* operation P: used for semaphore handling */
inline void WaitSem (sem_t *mutex) {
//	ASSERTX(mutex);
	int saved_errno = errno;
	errno = 0;
	ASSERTX(sem_wait(mutex) == 0);
//	int value;
//	ASSERTX(sem_getvalue(mutex, &value) == 0);
//	ASSERTX(value == 0);
	errno = saved_errno;
}



/*===============================================================================*/
/*                 SIGSEGV Handler for Changing Page Permissions                 */
/*===============================================================================*/

/* SIGSEGV signal handler */
void SigSegvHandler(int32_t signo, siginfo_t *si , void *sc) {

	char* faultaddr;
	int writefault;

	faultaddr = (char*) si->si_addr;
	faultaddr = (char*) (((uintptr_t) faultaddr/PAGE_SIZE)*PAGE_SIZE);
	writefault = (int) si->si_code & SEGV_ACCERR;

#ifdef PRINT_DEBUG_MSG
//	fprintf(stderr, "* entered signal handler for %p", faultaddr);
//	fflush(stderr);
#endif /* PRINT_DEBUG_MSG */

//	sigset_t set;
//	sigemptyset(&set);
//	sigaddset(&set,SIGIO);
//	sigprocmask(SIG_UNBLOCK, &set, NULL);

#ifdef PRINT_DEBUG_MSG
	void *array[100];
	size_t size = 0;

	if(!writefault)
	{
		fprintf(stderr, "mmapCount: %d\n", mmapCount);
		fprintf(stderr, "addr: %p\n", faultaddr);
		fflush(stderr);
		size = backtrace (array, 100);
		backtrace_symbols_fd(array, size, 2);
		fflush(stderr);
	}
//	else
//		printf("alright\n");
#endif /* PRINT_DEBUG_MSG */



	/* change page permissions */

	if(writefault){

#ifdef MICROTIME_STAT
		MicroTimer mt;
		mt.Start();
#endif /*MICROTIME_STAT */


		int saved_errno = errno;
		errno = 0;

		if(mergeMetric == BUFFERED){
			if(bufferPtr == BUFFER_LENGTH)
				MergeByBUFFERED();
			bufferOfDirtyPages[bufferPtr++] = ptr2offset(faultaddr);
		}
		else if(mergeMetric != MERGE_DISABLED){
			AVLTreeNode *n = NULL;
			if(allocRecord)
				n = (AVLTreeNode *)AspaceAvlSearchRangeWrapper((uintptr_t)faultaddr);
			//ASSERTX(n);
			if(n)
				n->dirty = 1;
		}


		bool is_initialized_page 	= true;
#ifdef COLLECT_MALLOC_STAT
		is_initialized_page 		= SetAndReturnBit(initializedPagesBV, faultaddr);
#endif /* COLLECT_MALLOC_STAT */


		

		if(!is_initialized_page){
		// writing for the first time. bit has been set already 

#ifdef SHARED_STATS
			WaitSem(mutex);
			(*allProcPrivatePageCount) += 1;
			(*baseCaseTotalPageCount) 	+= 1;

#ifdef PRINT_STATS
			if(myRank == 0){
				if(*baseCaseTotalPageCount - maxBaseCaseTotalPageCount > 1000){
					maxBaseCaseTotalPageCount = *baseCaseTotalPageCount + (ptmalloc_get_mem_usage() * (*aliveProcs))/PAGE_SIZE;
				}
			}
#endif /* PRINT_STATS */
			SignalSem(mutex);
#endif /* SHARED_STATS */
			MakeReadWriteWrapper(faultaddr, PAGE_SIZE);

		} else{

			bool is_zero_page 			= ResetAndReturnBit(zeroPagesBV, faultaddr);
			bool is_shared_page 		= GetSharingBit(faultaddr);

			WaitSem(mutex);

#ifdef PROFILE_BASED_MERGE
			{
				struct timeval tv;
				struct timezone tz;
				if(gettimeofday(&tv, &tz) == -1)
					die("gettimeofday");
				uint32_t time = (uint32_t)(tv.tv_sec * 1000000 + tv.tv_usec);
				UpdateMergeHist(faultaddr, time);
			}
#endif /* PROFILE_BASED_MERGE */

			if(is_zero_page){

#ifdef SHARED_STATS
				(*allProcPrivatePageCount)++;
#endif /* SHARED_STATS */
				zeroPageCount -=1;

			} else if(is_shared_page){

				UnsetSharingBit(faultaddr);

#ifdef SHARED_STATS
				int sh_cnt =0;
				switch(sh_cnt = CountSharingProcs(faultaddr)){
					case 1:
						(*sharedPageCount)--;
						(*allProcPrivatePageCount)+=2;
						break;
					case 0: // it was already private
						break; 
					default: // >=2 procs still sharing it
						// increase private page count
						(*allProcPrivatePageCount)++;
						ASSERTX(sh_cnt <= *aliveProcs);
				}
#endif /* SHARED_STATS */
			}


#ifdef ENABLE_PROFILER
			if(profileMode == CREATE_PROF){
				/* dump in file */
				fprintf(profFile, "%p %d %lu\n", (void*)faultaddr, 0,(unsigned long) time(NULL));
			}
#endif /* ENABLE_PROFILER */

#ifdef MREMAP_FIXED
			//
			// new, optimized solution. Unfortunately all kernels do not support it.
			//
			// allocate the page in some other location. Then copy the
			// content. unmap shared page and then call        
			// void * mremap(void *old_address, size_t old_size , size_t new_size, int
			// 		flags, void *new_address); 
			// with flags MREMAP_MAYMOVE | MREMAP_FIXED and using fault addr as new_address.
			//

			if(is_zero_page){

				// no need for munmap as MAP_FIXED replaced previous mapping 
				void *p =  SH_MMAP(faultaddr, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
				CheckForError();
				if(p == MAP_FAILED)
					SignalSem(mutex);
				ASSERTX(p != MAP_FAILED);
				memset(p, 0, PAGE_SIZE);

			} else if(is_shared_page){

				void *p =  SH_MMAP(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
				CheckForError();
				if(p == MAP_FAILED)
					SignalSem(mutex);
				ASSERTX(p != MAP_FAILED);

				memcpy(p, faultaddr, PAGE_SIZE);
				p = mremap(p, PAGE_SIZE, PAGE_SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, faultaddr);
				CheckForError();
				if(p == MAP_FAILED || p != faultaddr)
					SignalSem(mutex);
				ASSERTX(p != MAP_FAILED);
			}
#else

#error "Gaah! MREMAP_FIXED not supported"

			//
			// old, non-optimal implementation
			//
			warn("unmerging it old way! might be buggy");

			char p[PAGE_SIZE];
			void *ptr;
			memcpy(p, faultaddr, PAGE_SIZE);
			ASSERTX(SH_UNMAP(faultaddr, PAGE_SIZE) == 0);
			CheckForError();
			ptr = SH_MMAP(faultaddr, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
			CheckForError();
			if(ptr == MAP_FAILED || ptr != faultaddr)
				SignalSem(mutex);
			ASSERTX(ptr != MAP_FAILED);
			memcpy(faultaddr, p, PAGE_SIZE);
#endif
			SignalSem(mutex);

		}




#ifdef MICROTIME_STAT
		mt.Stop();
		sigHandlerTime += (mt.GetDiff() ?mt.GetDiff() :1);
#endif /*MICROTIME_STAT */

		if(mergeMetric == THRESHOLD){
//			static int counter = 1000;

//			if(--counter == 0)
			{ // 1000 pages added, may be compress now 
//				counter = mergeMinMemTh/25; // 4% of max mem modified
//				if(counter < 1000)
//					counter = 1000; // at least 4MB page modification
				MergeByTHRESHOLD();
			}
		}
		errno = saved_errno;
	}
	else{
#ifdef PRINT_DEBUG_MSG
		WaitSem(mutex);
		fprintf(stderr, "error code: %d, sigsegv for accessing %p\n",(int) si->si_code, faultaddr);
		fprintf(stderr, "errno: %d\n", (int) si->si_code);
		fflush(stderr);
//		ReportError(faultaddr);
		backtrace_symbols_fd(array, size, 2);
		SignalSem(mutex);
#endif /* PRINT_DEBUG_MSG */
		Fatal();
	}
#ifdef PRINT_DEBUG_MSG
//	fprintf(stderr, " ... done\n");
//	fflush(stderr);
#endif /* PRINT_DEBUG_MSG */
}

/* SIGINT signal handler */
void SigIntHandler(int32_t signo, siginfo_t *si , void *sc) {
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set,SIGIO);
	sigprocmask(SIG_UNBLOCK, &set, NULL);

#ifdef PRINT_DEBUG_MSG
	void *array[10];
	size_t size;
	size = backtrace (array, 10);
	backtrace_symbols_fd(array, size, 2);
#endif /* PRINT_DEBUG_MSG */
	Fatal();
}

/* SIGBUS signal handler */
void SigBusHandler(int32_t signo, siginfo_t *si , void *sc) {
	sigset_t set;
	char* faultaddr;

	sigemptyset(&set);
	sigaddset(&set,SIGIO);
	sigprocmask(SIG_UNBLOCK, &set, NULL);
	
	faultaddr = (char*) si->si_addr;
//	perror("sigbus");
//	fprintf(stderr, "sigbus for accessing %p\n", (void*) faultaddr);
//	fflush(stderr);

#ifdef PRINT_DEBUG_MSG
	void *array[10];
	size_t size;
//	fprintf(stderr, "mmapCount: %d\n", mmapCount);
//	fflush(stderr);
	size = backtrace (array, 10);
	backtrace_symbols_fd(array, size, 2);
//	fflush(stderr);
#endif /* PRINT_DEBUG_MSG */
	Fatal();
}


/*===============================================================================*/
/*            Bit Manipulation Routines for Managing Shared Metadata             */
/*===============================================================================*/

/* Translates page address to page number */
inline uintptr_t Addr2PageIndex(void *address){ 
 	return (TranslateMmapAddr((uintptr_t)address) >> log2PAGE_SIZE);
// 	return TranslateMmapAddr((uintptr_t)address)/PAGE_SIZE;
}

/* Counts the number of tasks sharing a page. */
int CountSharingProcs(void *addr) {
	if(sharingProcessesInfo){
#ifdef MICROTIME_STAT
		MicroTimer mt;
		mt.Start();
#endif /*MICROTIME_STAT */
		uintptr_t index	= Addr2PageIndex(addr);

#ifdef ENABLE_CHECKS
		if(index >= 3* 1024 * 256){
			ReportError(addr);
			return 0;
		}
		ASSERTX(numProc == 8 || numProc == 16);
#endif


		int x = (numProc == 8
				? *((uint8_t *) sharingProcessesInfo + index)
				: *((uint16_t *) sharingProcessesInfo + index)
				);

		int count	= 0;
		for (count=0; x; count++)
			x &= (x-1);
#ifdef MICROTIME_STAT
		mt.Stop();
		bitOpTime += (mt.GetDiff()?mt.GetDiff():1);
#endif /*MICROTIME_STAT */
		return count;
	}
	return 0;
}

/* Sets the sharing bit corresponding to page having address addr for currest
 * process*/
inline void SetSharingBit(void *addr){
	if(sharingProcessesInfo){
#ifdef MICROTIME_STAT
		MicroTimer mt;
		mt.Start();
#endif /*MICROTIME_STAT */
		uintptr_t index	= Addr2PageIndex(addr);

#ifdef ENABLE_CHECKS
		if(index >= 3* 1024 * 256){
			ReportError(addr);
			return ;
		}
#endif

		if(numProc == 8){
			*((uint8_t *) sharingProcessesInfo + index) |= currProcMask;
		}else if(numProc == 16){
			*((uint16_t *) sharingProcessesInfo + index) |= currProcMask;
		}else{
			die("error: these many processors are not supported\n");
		}

#ifdef MICROTIME_STAT
		mt.Stop();
		bitOpTime += (mt.GetDiff()?mt.GetDiff():1);
#endif /*MICROTIME_STAT */
	}
}


/*-------------------------------------------------------------------------------*/
/* Unsets the sharing bit corresponding to page having address addr for currest
 * process*/
inline void UnsetSharingBit(void *addr){
	if(sharingProcessesInfo){
#ifdef MICROTIME_STAT
		MicroTimer mt;
		mt.Start();
#endif /*MICROTIME_STAT */
		uintptr_t index = Addr2PageIndex(addr);
		
#ifdef ENABLE_CHECKS
		if(index >= 3* 1024 * 256){
			ReportError(addr);
			return ;
		}
#endif
		if(numProc == 8){
			*((uint8_t *) sharingProcessesInfo + index) &= currProcMaskInverted;
		}else if(numProc == 16){
			*((uint16_t *) sharingProcessesInfo + index) &= currProcMaskInverted;
		}else{
			die("error: these many processors are not supported\n");
		}
#ifdef MICROTIME_STAT
		mt.Stop();
		bitOpTime += (mt.GetDiff()?mt.GetDiff():1);
#endif /*MICROTIME_STAT */
	}
}

/*-------------------------------------------------------------------------------*/
/* Gets the sharing bit corresponding to page having address addr for currest
 * process*/
inline bool GetSharingBit(void *addr){
	if(sharingProcessesInfo){
#ifdef MICROTIME_STAT
		MicroTimer mt;
		mt.Start();
#endif /*MICROTIME_STAT */
		uintptr_t index	= Addr2PageIndex(addr);

#ifdef ENABLE_CHECKS
		if(index >= 3* 1024 * 256){
			ReportError(addr);
			return false;
		}
#endif


#ifdef MICROTIME_STAT
		mt.Stop();
		bitOpTime += (mt.GetDiff()?mt.GetDiff():1);
#endif /*MICROTIME_STAT */
		if(numProc == 8){
			return (*((uint8_t *) sharingProcessesInfo + index) & currProcMask);
		}else if(numProc == 16){
			return (*((uint16_t *) sharingProcessesInfo + index) & currProcMask);
		}else{
			die("error: these many processors are not supported\n");
		}
	}
	return false;
}

/*-------------------------------------------------------------------------------*/
/* Checks any other process shares the page having address addr by checking
 * sharingProcessesInfo bits */
inline bool IsOtherSharing(void *addr){
	if(sharingProcessesInfo){
#ifdef MICROTIME_STAT
		MicroTimer mt;
		mt.Start();
#endif /*MICROTIME_STAT */
		uintptr_t index	= Addr2PageIndex(addr);

#ifdef ENABLE_CHECKS
		if(index >= 3* 1024 * 256){
			ReportError(addr);
			return false;
		}
#endif
#ifdef MICROTIME_STAT
		mt.Stop();
		bitOpTime += (mt.GetDiff()?mt.GetDiff():1);
#endif /*MICROTIME_STAT */
		if(numProc == 8){
			return (*((uint8_t *) sharingProcessesInfo + index) & currProcMaskInverted);
		}else if(numProc == 16){
			return (*((uint16_t *) sharingProcessesInfo + index) & currProcMaskInverted);
		}else {
			die("error: these many processors are not supported\n");
		}
	}
	return false;
}

/* Gets bit corresponding to an address from bitvector array */
inline bool GetBit(char *array, char *page_address){
#ifdef MICROTIME_STAT
	MicroTimer mt;
	mt.Start();
#endif /*MICROTIME_STAT */
	uintptr_t index	= Addr2PageIndex((void*)page_address);

#ifdef ENABLE_CHECKS
	if(index >= 98304){
		ReportError(page_address);
		return false;
	}
#endif
#ifdef MICROTIME_STAT
	mt.Stop();
	bitOpTime += (mt.GetDiff()?mt.GetDiff():1);
#endif /*MICROTIME_STAT */
	return (array[index >> 3] & (0x01 << (index & 0x07)));
}

/* Sets bit corresponding to an address from bitvector array */
inline void SetBit(char *array, char *page_address){
#ifdef MICROTIME_STAT
	MicroTimer mt;
	mt.Start();
#endif /*MICROTIME_STAT */
	uintptr_t index	= Addr2PageIndex((void*)page_address);

#ifdef ENABLE_CHECKS
	if(index >= 98304){
		ReportError(page_address);
		return ;
	}
#endif
	array[index >> 3] |= (0x01 << (index & 0x07));
#ifdef MICROTIME_STAT
	mt.Stop();
	bitOpTime += (mt.GetDiff()?mt.GetDiff():1);
#endif /*MICROTIME_STAT */
}

/* Resets bit corresponding to an address from bitvector array */
inline void UnsetBit(char *array, void *page_address){
#ifdef MICROTIME_STAT
	MicroTimer mt;
	mt.Start();
#endif /*MICROTIME_STAT */
	uintptr_t index	= Addr2PageIndex((void*)page_address);

#ifdef ENABLE_CHECKS
	if(index >= 98304){
		ReportError(page_address);
		return ;
	}
#endif
	array[index >> 3] &= (~(0x01 << (index & 0x07)));
#ifdef MICROTIME_STAT
	mt.Stop();
	bitOpTime += (mt.GetDiff()?mt.GetDiff():1);
#endif /*MICROTIME_STAT */
}

/* Sets multiple bits corresponding to a region, more than 1 page from bitvector array */
void SetMultiBits(char *array, char *page_address, size_t size){
#ifdef MICROTIME_STAT
	MicroTimer mt;
	mt.Start();
#endif /*MICROTIME_STAT */
	uintptr_t index	= Addr2PageIndex((void*)page_address);
	int bsize = size >> log2PAGE_SIZE;

#ifdef ENABLE_CHECKS
	if((index+bsize) > 98304){
		ReportError(page_address);
		return ;
	}
#endif
	while(bsize > 0){
		if(index %8 == 0 && bsize >= 8){ // aligned
			array[index >> 3] = 0xff; /* set the byte */
			index += 8;
			bsize -= 8;
		}else{
			array[index >> 3] |= (0x01 << (index & 0x07));
			index++;
			bsize--;
		}
	}
#ifdef MICROTIME_STAT
	mt.Stop();
	bitOpTime += (mt.GetDiff()?mt.GetDiff():1);
#endif /*MICROTIME_STAT */
}


/* Sets and returns old value of bit in array corresponding to page address */
inline bool SetAndReturnBit(char *array, char *page_address){
#ifdef MICROTIME_STAT
	MicroTimer mt;
	mt.Start();
#endif /*MICROTIME_STAT */
	uintptr_t index	= Addr2PageIndex((void*)page_address);

#ifdef ENABLE_CHECKS
	if(index >= 98304){
		ReportError(page_address);
		return false;
	}
#endif
	int mask = (0x01 << (index & 0x07));
	int s_index = index >>3;


	int ret_val = (array[s_index] & mask);
	array[s_index] |= mask;
#ifdef MICROTIME_STAT
	mt.Stop();
	bitOpTime += (mt.GetDiff()?mt.GetDiff():1);
#endif /*MICROTIME_STAT */
	return ret_val;
}

/* Resets and returns bit in array corresponding to page address */
inline bool ResetAndReturnBit(char *array, char *page_address){
#ifdef MICROTIME_STAT
	MicroTimer mt;
	mt.Start();
#endif /*MICROTIME_STAT */
	uintptr_t index	= Addr2PageIndex((void*)page_address);

#ifdef ENABLE_CHECKS
	if(index >= 98304){
		ReportError(page_address);
		return false;
	}
#endif
	int mask = (0x01 << (index & 0x07));
	int inv_mask = ~mask;
	int s_index = index >>3;


	int ret_val = (array[s_index] & mask);
	array[s_index] &= inv_mask;
#ifdef MICROTIME_STAT
	mt.Stop();
	bitOpTime += (mt.GetDiff()?mt.GetDiff():1);
#endif /*MICROTIME_STAT */
	return ret_val;
}
/*-------------------------------------------------------------------------------*/

/* Inserts a node with start_addr and size. called upon malloc */
int AspaceAvlInsertWrapper(uintptr_t start_addr, size_t size){
	if(allocRecord)
		InsertAVL((AVLTree*)allocRecord, offset2ptr(start_addr), offset2ptr(size));
	CheckForError();
	return 0;
}

/*-------------------------------------------------------------------------------*/
/* Deletes a node with start_addr. called upon free 
 * returns the size of the region, 0 if not found. */
intptr_t AspaceAvlRemoveWrapper(uintptr_t start_addr){
	int *res = NULL;

	if(allocRecord)
		res = (int *)RemoveAVL((AVLTree*)allocRecord, offset2ptr(start_addr));
	return (intptr_t)res;
}

/*-------------------------------------------------------------------------------*/
/* searches a node with start_addr
 * returns 0 if not found, the size of the region if found */
uintptr_t AspaceAvlSearchWrapper(uintptr_t start_addr){
	int *res = NULL;
	if(allocRecord)
		res = (int*)FindAVL((AVLTree*)allocRecord, offset2ptr(start_addr));
	return ptr2offset(res);
}

/*-------------------------------------------------------------------------------*/
/* Searches a range with start_addr included in an existing range */
uintptr_t AspaceAvlSearchRangeWrapper(uintptr_t start_addr){
	void *res = NULL;
	if(allocRecord)
		res = FindRangeAVL((AVLTree*)allocRecord, offset2ptr(start_addr));
	return ptr2offset(res);
}


/*-------------------------------------------------------------------------------*/
/* AVL data structure compartor
 * returns 0 if identical, otherwise the difference as 1 or -1 */
inline int MyComparator(const void *key1, const void * key2){

	intptr_t diff = (intptr_t)((uintptr_t)key1 - (uintptr_t)key2);
	return (diff == 0? 0:
			(diff >0L)?1:-1);
}


/*===============================================================================*/
/*                                Profile guided merge                           */
/*===============================================================================*/
#ifdef PROFILE_BASED_MERGE
/* Checks merge profile to decide if the page should be merged */
bool CheckIfMergeable(void *pageAddr, uint32_t currTime){
	int index = Addr2PageIndex(pageAddr);
	bool isMergeable = true;
	/* check if it can be merged */

	if((currTime - lastMergeTime[index]) < mergeCostPerPage *10){
		isMergeable = false;
	}
	else{
#if PROF_MERGE_VERSION > 0
		uint32_t mergeTime = 10* mergeCostPerPage;
		uint32_t total_merge_time = mergeTime;

		uint8_t hist = mergeSuccHist[index];
		while(hist){
			if(hist & 0x80){
				total_merge_time += mergeTime;
#if PROF_MERGE_VERSION == 2
				mergeTime <<= 1; /* double it */
			}else {
				if(mergeTime > 1)
					mergeTime >>= 1; /* 1/2 it */
#endif /* PROF_MERGE_VERSION == 2 */
			}
#if PROF_MERGE_VERSION == 1
			mergeTime >>= 1;
#endif /* PROF_MERGE_VERSION == 1*/
			hist <<= 1;

		}
		if((currTime - lastMergeTime[index]) < total_merge_time){
			isMergeable = false;
		}

#endif /* PROF_MERGE_VERSION > 0 */
	}

	if(isMergeable)
		lastMergeTime[index] = currTime; 
	return isMergeable;
}

/* Updates merge profile. */
void UpdateMergeHist(void *pageAddr, uint32_t currTime){
	int index = Addr2PageIndex(pageAddr);
	mergeSuccHist[index] >>= 1;
	if((currTime - lastMergeTime[index]) < mergeCostPerPage * 10)
		mergeSuccHist[index] |= 0x80; /* bit-OR 1 to indicate unstable merge */
	lastMergeTime[index] = currTime; /* update split time so that in next merge we can use it */
}

#endif /* PROFILE_BASED_MERGE */


/*===============================================================================*/
/*                                  Buffer based merge                           */
/*===============================================================================*/
/* merges pages when the buffer of dirty pages becomes full */
void MergeByBUFFERED(){


	for(int i = 0; i < BUFFER_LENGTH; i++){
		uintptr_t t = bufferOfDirtyPages[i];

		if(IsCloseToMmapLimit()){
			warn("close to mmap limit");
			break;
		}

		if(! TranslateMmapAddr(t)){
			continue;
		}

#ifdef COLLECT_MALLOC_STAT
		if(GetBit(initializedPagesBV, (char*) t)) // required as the page might have been freed already
#endif
				MergePages((void *)t, 0); /* pass creator's address */
	}
	StoreMemUsageStat();
	bufferPtr = 0;
}

/*===============================================================================*/
/*                               Allocation Frequency based merge                           */
/*===============================================================================*/
/* Merges pages based on frequency of mallocs.  When the number of outstanding
 * malloc/free becomes more than x where x is the merging frequency, merge
 * operation is triggered. */
void MergeByALLOC_FREQUENCY(){
	if(!(++mallocRefCounter % mallocRefFreq)){
		StoreMemUsageStat();

		/* merge all pages */
		{
			/* if the mode is profiling or none */
#ifdef ENABLE_PROFILER
			if(profileMode != USE_PROF)
#endif /* ENABLE_PROFILER */
			{
#ifdef PART_BLOCK_MERGE_STAT
				localDiffPageCount = 0;
				localComparedPageCount = 0;
				// reset stats
				memset(partBlockStat, 0, 8*sizeof(int32_t));
#endif /* !PART_BLOCK_MERGE_STAT */

				TraverseAVL((AVLTree* )allocRecord, MergeNode2);

#ifdef ENABLE_PROFILER
				if(profileMode == CREATE_PROF){
					if(profFile){
						fprintf(profFile, "0");
#ifdef PART_BLOCK_MERGE_STAT
						fprintf(profFile, " %d", localComparedPageCount?(localDiffPageCount/localComparedPageCount):0);

						/* print part mergeable stats: begin */
						for(int32_t p_index = 0; p_index < 8; p_index++)
							fprintf(profFile, " %d", partBlockStat[p_index]);
						/* print part mergeable stats: end */
#endif /* !PART_BLOCK_MERGE_STAT */
#ifdef SHARED_STATS		
						fprintf(profFile, " %d", *sharedPageCount);
						fprintf(profFile, "\n");
#endif /* !SHARED_STATS	*/
					}
				}
#endif /* ENABLE_PROFILER */
			}
#ifdef ENABLE_PROFILER
			else{
				/* else if the mode is profile guided merging */
				uintptr_t aAddr;
				int32_t status;
				while(true){
					if(profFile){
						fscanf(profFile, "%lx", &aAddr);
						if(!aAddr)
							break; /* end of one epoch */

						fscanf(profFile, "%d", &status);
						if(AspaceAvlSearchRangeWrapper(aAddr))
							MergePages((void*)aAddr, 0);
						else 
							fprintf(stderr, ".");
					}else
						break;
				}
			}
#endif /* ENABLE_PROFILER */
		}
		mallocRefCounter=0;
		StoreMemUsageStat();
	}
}

/*===============================================================================*/
/*                               Threshold based merge                           */
/*===============================================================================*/

/* Merges pages based on threshold */
void MergeByTHRESHOLD(){

	static int countDownTimer = 100;

	if(--countDownTimer == 0)
		countDownTimer = 100;
	else 
		return;


#ifdef SHARED_STATS
	if(
			((*allProcPrivatePageCount + *sharedPageCount) >= mergeMinMemTh)
	  )
	{

		mergeMinMemTh = (*allProcPrivatePageCount + *sharedPageCount);

#ifdef MICROTIME_STAT
		MicroTimer mt;
		mt.Start();
#endif /*MICROTIME_STAT */

		StoreMemUsageStat();
#ifdef REPORT_MERGES
		numDirtyPages = numCleanPages = 0;
		totalProcessedPages = newlyMovedPages = newZeroPages = newlyMergedPages = 0;
#endif /* REPORT_MERGES */

#ifdef PART_BLOCK_MERGE_STAT
		localDiffPageCount = 0;
		localComparedPageCount = 0;
		localSharedPageCount = 0;
		localPageCount = 0;
		localZeroPageCount=0;
		// reset stats
		memset(partBlockStat, 0, 8*sizeof(int32_t));

#endif /* !PART_BLOCK_MERGE_STAT */

		TraverseAVL((AVLTree* )allocRecord, MergeNode2);
		
#ifdef REPORT_MERGES
		fprintf(stderr, "dirty: %d, clean %d ", numDirtyPages, numCleanPages);
		fprintf(stderr, "mov: %d, zer: %d, mer: %d, tot: %d\n", newlyMovedPages, newZeroPages, newlyMergedPages, totalProcessedPages);
#endif /* REPORT_MERGES */

#ifdef MICROTIME_STAT
		mt.Stop();
		fprintf(stderr, "time taken %lu\n", mt.GetDiff());
		mergeTime+= (mt.GetDiff()?mt.GetDiff():1);
#endif /*MICROTIME_STAT */
#ifdef ENABLE_PROFILER
		if(profileMode == CREATE_PROF){
			if(profFile){
				fprintf(profFile, "0");
				fprintf(profFile, " %d", *sharedPageCount);
#ifdef PART_BLOCK_MERGE_STAT
				fprintf(profFile, " %d %d %d", localSharedPageCount, localZeroPageCount, localPageCount);
//				fprintf(profFile, " %d %d", localDiffPageCount, localComparedPageCount);

				/* print part mergeable stats: begin */
				for(int32_t p_index = 0; p_index < 8; p_index++)
					fprintf(profFile, " %d", partBlockStat[p_index]);
				/* print part mergeable stats: end */
#endif /* !PART_BLOCK_MERGE_STAT */
				fprintf(profFile, "\n");
			}
		}
#endif /* ENABLE_PROFILER */
	}
#endif /* !SHARED_STATS */
}


/*===============================================================================*/
/*                       Page Permission Modifier Routines                       */
/*===============================================================================*/

/* makes a region readonly */
inline void MakeReadOnlyWrapper(void *addr, size_t len){ 
	int saved_errno = errno;
	errno = 0;
	ASSERTX(mprotect(addr, len, PROT_READ) == 0);
	errno = saved_errno;
}

/*-------------------------------------------------------------------------------*/
/* gives write permission to a region */
inline void MakeReadWriteWrapper(void *addr, size_t len){ 
	int saved_errno = errno;
	errno = 0;
	ASSERTX(mprotect(addr, len, PROT_READ | PROT_WRITE) == 0);
	errno = saved_errno;
}


/*===============================================================================*/
/*                   AVL Tree Code for Managing Allocated Regions                */
/*===============================================================================*/

/*            borrowed from http://joewing.net/programs/libraries/c/             */
/*-------------------------------------------------------------------------------*/



/*-------------------------------------------------------------------------------*/
/*!  @brief Prints a node of AVL tree (\b unused) */
inline void print_node(const void *key, const void *value, const void *data){
	printf("|%p:%lu|", key, ptr2offset(value));
}

/*-------------------------------------------------------------------------------*/
/* a node corresponding to <key, value> pair is checked so that identical data
 * pages can be merged */
void MergeNode(const void *key, const void *value, const void *data){
	uintptr_t t = ptr2offset(key);
	uintptr_t s = t + ptr2offset(value);
	
//	fprintf(stderr, "Merging Pages ");
//	fprintf(stderr, "%d:MergeNode called with %p and %d\n", myRank, key, (int)ptr2offset(value));
	for(; t < s; t+= PAGE_SIZE ){
		
		if(IsCloseToMmapLimit()){
			warn("close to mmap limit");
			break;
		}

		if(! TranslateMmapAddr(t)){
			continue;
		}

#ifdef COLLECT_MALLOC_STAT
		if(GetBit(initializedPagesBV, (char*) t))
#endif
		{
			//			if(t < (3*1024*1024*1024L))
			MergePages((void*) t, *((uintptr_t *)data)); /* pass creator's address */
		}
	}
//	fprintf(stderr, " Done\n");
}

/*-------------------------------------------------------------------------------*/
/* a node corresponding to <key, value> pair is checked so that identical data
 * pages can be merged. Merges many pages at once. */
void MergeNode2(const void *key, const void *value, const void *data, void *isDirty){
	uintptr_t addr = ptr2offset(key);
	uintptr_t size = ptr2offset(value);

	if(! TranslateMmapAddr(addr)){
		warn("allocated more than 3 GB???");
		return;
	}
	if(IsCloseToMmapLimit((int) size)){
		warn("close to mmap limit");
		return;
	}

#ifdef COLLECT_MALLOC_STAT

#ifdef PART_BLOCK_MERGE_STAT
	if(true)
#else
	if(isDirty && *((int *)isDirty))
#endif /*PART_BLOCK_MERGE_STAT*/

#endif /* COLLECT_MALLOC_STAT */
	{
#ifdef REPORT_MERGES
		totalProcessedPages += size/PAGE_SIZE;
		numDirtyPages++;
#endif /* REPORT_MERGES */
#ifdef ENABLE_PROFILER
		fprintf(profFile, "1 BEGIN MERGE\n");
#endif /* ENABLE_PROFILER */
		int merged_pages = MergeManyPages(addr, (size_t)size, ((void**)data)[0]); /* pass creator's address */
#ifdef ENABLE_PROFILER
		fprintf(profFile, "1 END MERGE %lu\n", (unsigned long)time(NULL));

		if( enableBacktrace && profFile && merged_pages){
			fprintf(profFile, "1 %d", merged_pages);
			fprintf(profFile, "; %p %p; ", (void*)addr, (void*)(addr+size) );
			for(int i = 0; i < MAX_STACK_DEPTH; i++){
				fprintf(profFile, " %p ", ((void**)data)[i]);
			}
			fprintf(profFile, "\n");
		}
#endif /* ENABLE_PROFILER */

#ifdef COLLECT_MALLOC_STAT
		*((int*)isDirty) = 0;
	}else{
#ifdef REPORT_MERGES
		numCleanPages+=size/PAGE_SIZE;
#endif /* REPORT_MERGES */
#endif /* COLLECT_MALLOC_STAT */
	}


#ifdef COLLECT_MALLOC_STAT
#ifdef PART_BLOCK_MERGE_STAT
	for(char *ptr = (char *)addr; ptr < (char*)(addr+size); ptr+=PAGE_SIZE){
		if(GetSharingBit(ptr) && IsOtherSharing(ptr))
			localSharedPageCount++;
		if(GetBit(zeroPagesBV, ptr))
			localZeroPageCount++;
		localPageCount++;
	}
#endif /*PART_BLOCK_MERGE_STAT*/
#endif /* COLLECT_MALLOC_STAT */
}



/* Frees up a node corresponding to <key, value> pair */
inline void FreeNode(const void *key, const void *value, const void *data, void *isDirty){
	int saved_errno = errno;
	errno = 0;
	ASSERTX(SH_UNMAP((void*)key, (size_t) (value)) == 0);
	errno = saved_errno;
}



/*================================================================================================*/
/*                           Shared Page Manipulation Routines                                    */
/*================================================================================================*/

/* Allocates a shared region */
void *GetSharedRegion(void* addr, bool isFixed, size_t size){

	int saved_errno = errno;
	errno = 0;
 	uintptr_t page_address =  TranslateMmapAddr((uintptr_t)addr);
	void * ptr;


	ASSERTX(page_address < 0xc0000000);
	if(page_address >= 0xc0000000){
		warn("allocated more than 3 GB???");
		errno = saved_errno ;
		return MAP_FAILED;
	}

	if(addr){
		ptr = (void*) SH_MMAP(
				(isFixed? addr: NULL),
				size, 
				PROT_READ  | PROT_WRITE, 
				MAP_SHARED | (isFixed? MAP_FIXED: 0), 
				sharedFileDescr, 
				page_address);
		if(ptr == MAP_FAILED){
			warn("mmap failed. If any other library uses mmap anymore, it might fail");
			/* mmap failed, so if the other library uses mmap anymore, it might fail. */
			errno = saved_errno ;
			return MAP_FAILED; 
		}
	}
	else{
		/* NOTE: NOT USED CURRENTLY AS WE DO NOT ALLOCATE BY DEFAULT IN SHARED REGION */
		/* get address where to attach it. In order to do that, mmap, unmap and mmap */
		ptr = (void*) SH_MMAP(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, -1, 0);
		if(ptr == MAP_FAILED){
			warn("mmap failed. If any other library uses mmap anymore, it might fail");
			/* mmap failed, so if the other library uses mmap anymore, it might fail. */
			errno = saved_errno ;
			return MAP_FAILED; 
		}
		/* next unmap and map again should not fail as it is going to keep the counts same */
		ASSERTX(SH_UNMAP(ptr, size) == 0); 
		ptr = (void*) SH_MMAP(ptr, 
				size, 
				PROT_READ | PROT_WRITE, 
				MAP_SHARED|MAP_FIXED, 
				sharedFileDescr, 
				TranslateMmapAddr((uintptr_t)ptr));
		ASSERTX(ptr != MAP_FAILED);
	}
	errno = saved_errno;
	return ptr;
}

/* Allocates a shared page */
inline void *GetSharedPage(void* addr, bool isFixed){
	return GetSharedRegion(addr, isFixed, PAGE_SIZE);
}


/*-------------------------------------------------------------------------------*/
/* Clean up  shared regions at exit */
void CleanUpSharedData(){

#ifdef MICROTIME_STAT
	fprintf(stderr, "merge time = %lu\n", mergeTime);
	fprintf(stderr, "alloc time = %lu\n", allocTime);
	fprintf(stderr, "free time = %lu\n", freeTime);
	fprintf(stderr, "bitwise op time = %lu\n", bitOpTime);
	fprintf(stderr, "compare op time = %lu\n", compareTime);
	fprintf(stderr, "sighandler op time = %lu\n", sigHandlerTime);
#endif /* MICROTIME_STAT*/

#ifdef PROFILE_BASED_MERGE
	ASSERTX(SH_UNMAP(mergeSuccHist, 0x03 << (30 - log2PAGE_SIZE)) == 0);
	ASSERTX(SH_UNMAP(lastMergeTime, 0x03 << (30 - log2PAGE_SIZE + 2)) == 0);
#endif /* PROFILE_BASED_MERGE */
//	return;


	signal(SIGUSR1, SIG_IGN);
	signal(SIGUSR2, SIG_IGN);
//	signal(SIGINT,  SIG_IGN);
	signal(SIGTERM, SIG_IGN);
	signal(SIGSEGV, SIG_IGN);
	signal(SIGBUS, SIG_IGN);

#ifdef PRINT_DEBUG_MSG
	printf("cleaning up the trash ... ");
#endif /* PRINT_DEBUG_MSG */


	WaitSem(mutex);
	if(aliveProcs)
		--(*aliveProcs);

#ifdef PRINT_DEBUG_MSG
	printf("aliveProcs decremented to %d ... ", *aliveProcs);
#endif /* PRINT_DEBUG_MSG */

#ifdef PRINT_DEBUG_MSG
	printf("freeing unfreed mem ...");
#endif /* PRINT_DEBUG_MSG */
//	TraverseAVL((AVLTree* )allocRecord, FreeNode);
	DestroyAVL((AVLTree*)allocRecord);

#ifdef PRINT_DEBUG_MSG
	printf("destroyed AVL tree ... ");
#endif /* PRINT_DEBUG_MSG */
#ifdef ENABLE_PROFILER
	if(profileMode > NONE ){
		fclose(profFile);
		profFile = NULL;
	}
#endif /* ENABLE_PROFILER */

	int alive_procs = (aliveProcs? *aliveProcs: 0);

	if(sharingProcessesInfo){
		ASSERTX(SH_UNMAP(sharingProcessesInfo, 3 * 1024 * 1024) == 0);
		sharingProcessesInfo = NULL;
	}

#ifdef PRINT_DEBUG_MSG
	printf("unmapped shared region ... ");
#endif /* PRINT_DEBUG_MSG */
	SignalSem(mutex);


#ifdef SHARED_STATS
	sharedPageCount = NULL;
	allProcPrivatePageCount = NULL;
	baseCaseTotalPageCount = NULL;
#endif /* !SHARED_STATS */
	/* all the other processes have cleared, so clean up stuff */
	if(!alive_procs){
		if(sharedFileDescr >= 0){
			ftruncate64(sharedFileDescr, 0);
			close(sharedFileDescr);
		}
		shm_unlink("/PSMallocTest");
		sem_close(mutex);
		sem_unlink(semName);
	}else{
		if(sharedFileDescr >= 0){
			close(sharedFileDescr);
		}
		sem_close(mutex);
	}
#ifdef PRINT_DEBUG_MSG
	printf("done\n");
#endif /* PRINT_DEBUG_MSG */
}


#ifdef PART_BLOCK_MERGE_STAT
int compare_pages(void *a, void *b){
	int diff=0;
	char *a1 = (char *)a;
	char *b1 = (char *)b;
	int offset = 0;
	int part_count1 = 0;
	int part_count2 = 0;
	int part_count4 = 0;
	int part_count8 = 0;

	/* partition each page into 16 parts and then count each part */
	/* part block matching stat */
	bool identical_page = true;
	for(int i1 = 0; i1 < 2; i1++){
		bool r1 = true;

		for(int i2 = 0; i2 < 2; i2++){
			bool r2 = true;

			for(int i3 = 0; i3 < 2; i3++){
				bool r3 = true;
				
				for(int i4 = 0; i4 < 2; i4++){
					if(memcmp((const void *)((char *)a+offset), (const void *)((char *)b+offset), PAGE_SIZE/16) == 0)
					{
						part_count1++;
					}else{
						r3 = false;
						r2 = false;
						r1 = false;
						identical_page = false;
					}
					offset += (PAGE_SIZE/16);
				}
				part_count2 += (r3? 1: 0);
			}
			part_count4 += (r2? 1: 0);
		}
		part_count8 += (r1? 1: 0);
	}
	
	partBlockStat[0] += part_count1; // count for matching 1/16th of a page
	partBlockStat[1] += part_count2; // count for matching 1/8th of a page
	partBlockStat[2] += part_count4; // count for matching 1/4th of a page
	partBlockStat[3] += part_count8; // count for matching 1/2 of a page
	partBlockStat[4] += (identical_page? 1: 0); // count for matching of a page

	/* bytewise similarity stats */
	for(int i=0; i< PAGE_SIZE;i++)
		diff += (a1[i] != b1[i]);

	localComparedPageCount++;
	if(diff)
		localDiffPageCount++;
#define PART_MATCH_THRESHOLD_HI 20
#define PART_MATCH_THRESHOLD_LO 10
	if(diff*100.0/PAGE_SIZE <= PART_MATCH_THRESHOLD_HI)
		partBlockStat[6]++;

	if(diff*100.0/PAGE_SIZE <= PART_MATCH_THRESHOLD_LO)
		partBlockStat[5]++;

	return (int)ceil((diff * 100.0)/PAGE_SIZE);
}
#else

#ifdef MICROTIME_STAT
inline int compare_pages(void *a, void *b){
		MicroTimer mt;
		mt.Start();
		int diff = memcmp((const void*) a, (const void*) b, PAGE_SIZE);
		mt.Stop();
		compareTime += (mt.GetDiff()?mt.GetDiff():1);
		return diff;
}
#else
#define compare_pages(a, b) memcmp((const void*) a, (const void*) b, PAGE_SIZE)
#endif /* !MICROTIME_STAT */

#endif /* !PART_BLOCK_MERGE_STAT */

/* stores the call stack when malloc is called. */
void GetCallStack(void **stack, int depth){
	int i, j, nptrs;
	uintptr_t addr;

	for(i = 0; i < depth; i++)
		stack[i] = NULL;

	if(!enableBacktrace)
		return ;
	nptrs = backtrace(stack, depth);
	for(i = 0; i < nptrs; i++){
	 	addr = *(uintptr_t *)(stack[i]);
		if(addr < lowLoadAddr || addr >= highLoadAddr)
			break;
	}
	if(i){
		for(j = 0; i< nptrs;i++, j++){
			stack[j] = stack[i];
			stack[i] = NULL;
		}
	}
}


/* Finds address of the source that called malloc */
uintptr_t GetBacktrace() {
	if(!enableBacktrace)
		return 0;
	int j, nptrs;
	uintptr_t addr;
	
#define SIZE 100
	void *buffer[SIZE];

	nptrs = backtrace(buffer, SIZE);
	for(j = 0; j < nptrs; j++){
	 	addr = *(uintptr_t *)(buffer[j]);
		if(addr < lowLoadAddr || addr >= highLoadAddr){
			return addr;
		}
	}
	return 0;
}

/* Copies and maps pages from private region to shared space */
int CopyAndRemapRegion(void *start, size_t size){
	static int moved_mem = 0;
	moved_mem += size;
	int saved_errno = errno;
	errno = 0;

#ifdef REPORT_MERGES
	newlyMovedPages += size/PAGE_SIZE;
#endif /* REPORT_MERGES */
#ifdef MREMAP_FIXED
	//
	// optimized implementation if mremap supports MREMAP_FIXED
	//
	void *p0 = GetSharedRegion(start, false, size);
	if(p0 == MAP_FAILED){
		errno = saved_errno;
		return -1;
	}

	memcpy(p0, start, size);
	p0 = mremap(p0, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, start);
	ASSERTX(p0 != MAP_FAILED);
#else
#error "Gaah! MREMAP_FIXED not supported"
	//
	// old non-optimal implementation 
	//
	warn("mapping it old way, may be buggy");
	char *p1 = (char*) SH_MMAP(NULL, 
			size, 
			PROT_READ | PROT_WRITE, 
			MAP_PRIVATE|MAP_ANONYMOUS, 
			-1, 
			0);
	if(p1 == MAP_FAILED){
		errno = saved_errno;
		return -1;
	}

	memcpy(p1, start, size);
	ASSERTX(SH_UNMAP(start, size) == 0);
	void *p0 = GetSharedRegion(start, true, size);
	ASSERTX(p0 != MAP_FAILED);

	memcpy(p0, p1, size);
	ASSERTX(SH_UNMAP(p1, size) == 0);
#endif
	for(size_t s = 0; s < size; s+= PAGE_SIZE){
		void *p = (void *)(ptr2offset(start)+s);
		SetSharingBit(p);
//		fprintf(stderr, "%d:%p *\n", myRank, p);
	}
	MakeReadOnlyWrapper(start, size);
	errno = saved_errno;
	return 0;
}


/* Maps pages from private region to shared space */
int RemapRegion(void *start, size_t size){

	void *p0;
	int saved_errno = errno;
	errno = 0;

#ifdef REPORT_MERGES
	newlyMergedPages += size/PAGE_SIZE;
#endif /* REPORT_MERGES */
	p0 = GetSharedRegion(start, true, size);
	if(p0 == MAP_FAILED){
		errno = saved_errno;
		return -1;
	}
	ASSERTX(p0 == start);

	for(size_t s = 0; s < size; s+= PAGE_SIZE){
		void *p = (void *)(ptr2offset(start)+s);
#ifdef SHARED_STATS
		if(CountSharingProcs(p) == 1){ // only 1 process is holding it, so not shared till now
			(*sharedPageCount)++;
			(*allProcPrivatePageCount)--;
		}/* >0 procs are sharing the page */
		(*allProcPrivatePageCount)--;
#endif
		SetSharingBit(p);
	}
	MakeReadOnlyWrapper(start, size); /* FIX 03/05/2009 */
	errno = saved_errno;
	return 0;
}

/* Remaps the pages to the zero page */
int RemapToZero(void *start, size_t size){
	int saved_errno = errno;
	errno = 0;

#ifdef REPORT_MERGES
	newZeroPages += size/PAGE_SIZE;
#endif /* REPORT_MERGES */


	for(size_t s = 0; s < size; s+= PAGE_SIZE){
		if(IsCloseToMmapLimit()){
			warn("close to mmap limit");
			errno = saved_errno;
			return -1; /* pretty close to limit*/
		}
		void *p = (void *)(ptr2offset(start)+s);
		p = (void*) SH_MMAP(
				p,
				PAGE_SIZE, 
				PROT_READ,
				MAP_SHARED | MAP_FIXED, 
				sharedFileDescr, 
				0);
		if(p == MAP_FAILED){
			errno = saved_errno;
			return -1;
		}
		zeroPageCount +=1;
#ifdef SHARED_STATS
		(*allProcPrivatePageCount)--;
#endif
	}

	if(size > (size_t)PAGE_SIZE)
		SetMultiBits(zeroPagesBV, (char*) start, size);
	else
		SetBit(zeroPagesBV, (char *)start);

	errno = saved_errno;
	return 0;
}

#ifdef ENABLE_PROFILER
void PRINT_PROFILE_DATA(void *start,  void *end, uintptr_t caddr) {
	for(char *addr = (char *)start; addr < (char *)end; addr += PAGE_SIZE){
		if(profileMode == CREATE_PROF){
			fprintf(profFile, "%p %d", addr, 1);
//			fprintf(profFile, "%p %d %lu", addr, 1, (unsigned long)time(NULL));
			if(enableBacktrace) fprintf(profFile, " %p", (void *)caddr);
			fprintf(profFile, "\n");
		}
	}
}
#else
#define PRINT_PROFILE_DATA(start, end, caddr)
#endif /* ENABLE_PROFILER */

/*! @brief Map/Move outstanding pages
 * @param start Start addr 
 * @param end End addr 
 * @param lps Boolean flag whether last page shareable 
 * @param lpm Boolean flag whether last page mergeable 
 * @param lpz Boolean flag whether last page zero 
 * @param caddr Creator addr for these set of pages
*/
#define FLUSH_OUTSTANDING_MERGES(start, end, lps, lpm, lpz, caddr) {\
	if(start){\
		if(lps) RemapRegion			(start, (size_t)(ptr2offset(end) - ptr2offset(start)));\
		if(lpm) CopyAndRemapRegion	(start, (size_t)(ptr2offset(end) - ptr2offset(start)));\
		if(lpz) RemapToZero			(start, (size_t)(ptr2offset(end) - ptr2offset(start)));\
		if(lps || lpz){ \
			PRINT_PROFILE_DATA(start, end, caddr);\
			counter_pages_merged += (ptr2offset(end) - ptr2offset(start))/PAGE_SIZE;\
		}\
	}\
	lps = lpm = lpz = false;\
	start = NULL;\
}

/*! Maximum size of the buffer used for mapping and managing pages in bulk */
#define MMAP_BUFFER_SIZE (4*1024*1024)

/* Merges many pages starting from address start_addr for length size */
int MergeManyPages(uintptr_t start_addr, size_t size, const void* data){

	ASSERTX(sharingProcessesInfo);

	int counter_pages_merged = 0;

	uintptr_t creator_addr = (uintptr_t)data;
	WaitSem(mutex);
	void *p;
//	void *q;

	bool last_page_zero 		= false; /* was last pages containing zeros? map to zero page */
	bool last_page_moveable 	= false; /* need to copy data */
	bool last_page_shareable 	= false; /* no data copy needed, just remap */
	void *mergeable_start_addr 	= NULL;

	int  index = 0;
	char *mmap_buffer = NULL;
	void *curr_page_ptr = NULL;

	int saved_errno = errno;
	errno = 0;

#ifdef PROFILE_BASED_MERGE
	struct timeval tv;
	struct timezone tz;
	if(gettimeofday(&tv, &tz) == -1)
		die("gettimeofday");
	uint32_t time = (uint32_t)(tv.tv_sec * 1000000 + tv.tv_usec);
#endif /* PROFILE_BASED_MERGE */

	for(size_t s =0; s < size; s += PAGE_SIZE){
		if(IsCloseToMmapLimit()){
			if(mmap_buffer)
				SH_UNMAP(mmap_buffer, MMAP_BUFFER_SIZE); /* free previously allocated 4MB buffer */
			warn("close to mmap limit");
			SignalSem(mutex);
			errno = saved_errno;
//			return -1; /* pretty close to limit*/
			return counter_pages_merged;
		}

		p = offset2ptr(start_addr + s);

		/* maintain a 4MB buffer */
		if(index == 0 || index == 4*1024*1024){
			index = 0;
			if(mmap_buffer)
				SH_UNMAP(mmap_buffer, MMAP_BUFFER_SIZE); /* free previously allocated 4MB buffer */
			mmap_buffer = (char *)GetSharedRegion(p, false, MMAP_BUFFER_SIZE);
		}
		curr_page_ptr = (void *)(mmap_buffer + index);
		index += PAGE_SIZE;

		/* check */
		if(mergeable_start_addr){
			ASSERTX(last_page_zero || last_page_shareable || last_page_moveable);
			ASSERTX(!(
						(last_page_zero 		&& 	last_page_shareable) ||
						(last_page_zero 		&& 	last_page_moveable) ||
						(last_page_shareable 	&& 	last_page_moveable)
					 ));

		}else{
			ASSERTX(!last_page_zero);
			ASSERTX(!last_page_shareable);
			ASSERTX(!last_page_moveable);
		}


		bool is_initialized_page 	= true;
#ifdef COLLECT_MALLOC_STAT
		is_initialized_page 		= GetBit(initializedPagesBV, (char *)p);
		if(!is_initialized_page){
			FLUSH_OUTSTANDING_MERGES(mergeable_start_addr, p, last_page_shareable, last_page_moveable, last_page_zero, creator_addr);
			continue;
		}
#endif /* COLLECT_MALLOC_STAT */

		bool is_zero_page 			= GetBit(zeroPagesBV, (char *)p);
		if(is_zero_page){
			FLUSH_OUTSTANDING_MERGES(mergeable_start_addr, p, last_page_shareable, last_page_moveable, last_page_zero, creator_addr);
			continue;
		}

		bool is_shared_page 		= GetSharingBit(p);
		if(is_shared_page){
			FLUSH_OUTSTANDING_MERGES(mergeable_start_addr, p, last_page_shareable, last_page_moveable, last_page_zero, creator_addr);
			continue;
		}

#ifdef PROFILE_BASED_MERGE
		{
			if(!CheckIfMergeable(p, time)){
				FLUSH_OUTSTANDING_MERGES(mergeable_start_addr, p, last_page_shareable, last_page_moveable, last_page_zero, creator_addr);
				continue;
			}
		}
#endif /* PROFILE_BASED_MERGE */


		if(compare_pages(p, zeroPage) == 0){ 
			if(mergeable_start_addr){
				if(last_page_zero){
					continue;
				}else{
					FLUSH_OUTSTANDING_MERGES(mergeable_start_addr, p, last_page_shareable, last_page_moveable, last_page_zero, creator_addr);
				}
			}
			mergeable_start_addr 	= p;
			last_page_zero 		= true;
			continue;
		}


		if(!IsOtherSharing(p)){ 
			if(mergeable_start_addr){
				if(last_page_moveable){
					continue;
				}else{
					FLUSH_OUTSTANDING_MERGES(mergeable_start_addr, p, last_page_shareable, last_page_moveable, last_page_zero, creator_addr);
				}
			}
			mergeable_start_addr 	= p;
			last_page_moveable 		= true;
			continue;
		}else{
			if(!last_page_shareable){
				FLUSH_OUTSTANDING_MERGES(mergeable_start_addr, p, last_page_shareable, last_page_moveable, last_page_zero, creator_addr);
			}

			// if contents match, we can mark it as mergeable 
//			q  = GetSharedPage(p, false); /* map the shared page here */
//			ASSERTX(q != MAP_FAILED);
//			int diff = compare_pages(q, p);
			int diff = compare_pages(curr_page_ptr, p);
			CheckForError();
//			ASSERTX(SH_UNMAP(q, PAGE_SIZE) == 0);

			if(diff == 0){ 
				//				fprintf(stderr, "%d:shareable page %p\n", myRank, p);
				if(!mergeable_start_addr){
					mergeable_start_addr = p;
					last_page_shareable = true;
				}
				continue;
			}else{
				// else move any outstanding movable reagion and mark as non-movable

				FLUSH_OUTSTANDING_MERGES(mergeable_start_addr, p, last_page_shareable, last_page_moveable, last_page_zero, creator_addr);
			}
		}
	}

	if(mmap_buffer)
		SH_UNMAP(mmap_buffer, MMAP_BUFFER_SIZE); /* free previously allocated 4MB buffer */

	p = offset2ptr(start_addr + size);
	FLUSH_OUTSTANDING_MERGES(mergeable_start_addr, p, last_page_shareable, last_page_moveable, last_page_zero, creator_addr);

	errno = saved_errno;
	SignalSem(mutex);
	//return 0;
	return counter_pages_merged;
}

/*-------------------------------------------------------------------------------*/
/* try to merge pages of current process with shared region for page of address p  */
int MergePages(void *p, uintptr_t creator_addr){
	void *p0;

	static int moved_mem = 0;

#if 0
	fprintf(stderr, "Merging pages ... ");
	fflush(stderr);
#endif
	ASSERTX(sharingProcessesInfo);
	
	WaitSem(mutex);
	if(GetSharingBit(p)){/* it is already shared, don't try to merge */
#if 0
		fprintf(stderr, "not done\n");
		fflush(stderr);
#endif
		SignalSem(mutex);
		return 0;
	}
	
	int saved_errno = errno;
	errno = 0;
	if(!IsOtherSharing(p)){
//		if(!myRank )
		{ /* if no one is sharing, move the page there */
#ifdef MREMAP_FIXED
			//
			// optimized implementation if mremap supports MREMAP_FIXED
			//
			p0 = GetSharedPage(p, false);
			memcpy(p0, p, PAGE_SIZE);
			p0 = mremap(p0, PAGE_SIZE, PAGE_SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, p); 
			ASSERTX(p0 != MAP_FAILED);
			CheckForError();
#else
#error "Gaah! MREMAP_FIXED not supported"
			//
			// old non-optimal implementation 
			//
			char p1[PAGE_SIZE]; 
			memcpy(p1, p, PAGE_SIZE);
			ASSERTX(SH_UNMAP(p, PAGE_SIZE) == 0);
			p0 = GetSharedPage(p, true);
			ASSERTX(p0 != MAP_FAILED);
			memcpy(p, p1, PAGE_SIZE);
#endif

			moved_mem += PAGE_SIZE;
//			fprintf(stderr, "moved %d\n", moved_mem);
			SetSharingBit(p);
			MakeReadOnlyWrapper(p, PAGE_SIZE); /* FIX 03/05/2009 */
		}
#if 0
		fprintf(stderr, "moved\n");
		fflush(stderr);
#endif
		SignalSem(mutex);
		errno = saved_errno;
		return 0;
	} else { /* if someone is sharing it already */
		p0  = GetSharedPage(p, false); /* map the shared page here */
		ASSERTX(p0 != MAP_FAILED);
		int32_t diff = compare_pages(p0, p);
		CheckForError();
		if(diff == 0){ /* contents matched: move page */

//			fprintf(stderr, ".");
//			fflush(stderr);
#ifdef SHARED_STATS
			if(CountSharingProcs(p) == 1){ // only 1 process is holding it, so not shared till now
				(*sharedPageCount)++;
				(*allProcPrivatePageCount)--;
			}/* >0 procs are sharing the page */
			(*allProcPrivatePageCount)--;
#endif


#ifdef MREMAP_FIXED
			//
			// optimized implementation if mremap supports MREMAP_FIXED
			//
			// void * mremap(void *old_address, size_t old_size , size_t new_size, int flags, void *new_address); 
			// with flags MREMAP_MAYMOVE | MREMAP_FIXED and using p as new_address and p0 as the old.
			p0 = mremap(p0, PAGE_SIZE, PAGE_SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, p); 
			ASSERTX(p0 != MAP_FAILED);
			CheckForError();
#else
#error "Gaah! MREMAP_FIXED not supported"
			//
			// old non-optimal implementation 
			//

			ASSERTX(SH_UNMAP(p, PAGE_SIZE) == 0);
			ASSERTX(SH_UNMAP(p0, PAGE_SIZE) == 0);
			p0 = GetSharedPage(p, true);
#endif

			SetSharingBit(p);
			MakeReadOnlyWrapper(p0, PAGE_SIZE); /* FIX 03/05/2009 */

#ifdef ENABLE_PROFILER
			if(profileMode == CREATE_PROF){
				/* dump in file */
				fprintf(profFile, "%p %d %lu", p, 1, (unsigned long)time(NULL));
				if(enableBacktrace)
					fprintf(profFile, " %p", (void *)creator_addr);

				fprintf(profFile, "\n");
//				fprintf(stderr, "%u %d %u\n", ((unsigned)p), 1, time(NULL));
			}
#endif /* ENABLE_PROFILER */
			/* call backtrace from here */
#if 0	
			fprintf(stderr, "done\n");
			fflush(stderr);
#endif
			SignalSem(mutex);
			ASSERTX(p0 != MAP_FAILED);
			errno = saved_errno;
			return 1;
		}else{
#ifdef PART_BLOCK_MERGE_STAT
			localDiffPageCount += diff;
			localComparedPageCount++;
#endif /* !PART_BLOCK_MERGE_STAT */
			ASSERTX(SH_UNMAP(p0, PAGE_SIZE) == 0);
		}
	}

	errno = saved_errno;
#if 0
	fprintf(stderr, "not done\n");
	fflush(stderr);
#endif
	SignalSem(mutex);
	return 0;
}

/*===============================================================================*/
/*                                 Public Interface                              */
/*===============================================================================*/
/* public interface for malloc using shared pages */
void * ShmMallocWrapper(size_t sz){

	if(!CheckMPIInitialized())
		return NULL;
	if(isMPIFinalized){ /* if MPI_Finalize() called, do not allocate using shared routines */
#ifdef PRINT_DEBUG_MSG
		fprintf(stderr, "malloc called after MPI_Finalize()\n");
#endif /* PRINT_DEBUG_MSG */
		return NULL;
	}



	if(IsCloseToMmapLimit()){
		warn("close to mmap limit");
		return NULL; /* pretty close to limit*/
	}


	void *ptr;
	size_t size = ((sz + PAGE_SIZE -1 )/PAGE_SIZE)*PAGE_SIZE; // faster than bitwise AND
//	size_t size = ((sz + PAGE_SIZE -1 ) >> log2PAGE_SIZE) << log2PAGE_SIZE;

	switch (mergeMetric){
		case ALLOC_FREQUENCY:
			MergeByALLOC_FREQUENCY();
			break;
#ifndef COLLECT_MALLOC_STAT
		case THRESHOLD:
			MergeByTHRESHOLD();
			break;
#endif /* COLLECT_MALLOC_STAT */
		default: /* No merging */
			break;
	}
#ifdef MICROTIME_STAT
	MicroTimer mt;
	mt.Start();
#endif /*MICROTIME_STAT */
	
	int saved_errno = errno;
	errno = 0;

#ifdef COLLECT_MALLOC_STAT
#if 0
	if(mergeMetric != MERGE_DISABLED)
		ptr = (void *) SH_MMAP(NULL, size, PROT_READ, 			 MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	else
		ptr = (void *) SH_MMAP(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
#endif /*0*/
	ptr = (void *) SH_MMAP(NULL, size, PROT_READ, 			 MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
#else
	ptr = (void *) SH_MMAP(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
#endif /* COLLECT_MALLOC_STAT */
	if(ptr == MAP_FAILED){
		 warn("mmap failed, so if any other library uses mmap anymore, it might fail");
		/* mmap failed, so if the other library uses mmap anymore, it might fail. */
		return NULL; 
	}
	
	AspaceAvlInsertWrapper(ptr2offset(ptr), size);
//	TranslateMmapAddr(ptr2offset(ptr));


#ifdef SHARED_STATS
#ifndef COLLECT_MALLOC_STAT
	WaitSem(mutex);
	(*allProcPrivatePageCount) += (size/PAGE_SIZE);
	(*baseCaseTotalPageCount) += (size/PAGE_SIZE);
	SignalSem(mutex);
#endif /* !COLLECT_MALLOC_STAT */
#endif /* !SHARED_STATS */

//	fprintf(stderr, "%p\n", ptr);
	errno = saved_errno;
		
#ifdef MICROTIME_STAT
	mt.Stop();
	allocTime += (mt.GetDiff()?mt.GetDiff():1);
#endif /*MICROTIME_STAT */

//	fprintf(stderr, "malloc %p %ld\n", ptr, size);
	return ptr;
}


/*-------------------------------------------------------------------------------*/
/* public interface for realloc using shared pages */
void * ShmReallocWrapper(void * ptr, size_t size){
	if(!CheckMPIInitialized())
		return NULL;

//	fprintf(stderr, "realloc %p %ld\n", ptr, size);
	size_t old_size;
	void * new_ptr;

	int saved_errno = errno;
	errno = 0;
	/* find current size */
	old_size = AspaceAvlSearchWrapper(ptr2offset(ptr));
	CheckForError();
	if(old_size <= 0){
		/* it is allocated by small allocator. Let it handle this */
		errno = saved_errno;
		return NULL;
	}
	
	/* if current size is good enough, return old ptr */
	if(old_size >= size)
		return ptr;

	/* allocate new region */ 
	new_ptr = ShmMallocWrapper(size); /* cannot allocate shareable region */
	if(!new_ptr)
		new_ptr = ptmalloc(size);

	if(new_ptr == NULL){
		warn("malloc from realloc returned NULL, so returning old pointer, but there might be error\n");
		errno = saved_errno;
		return ptr;
	}
	
	int32_t sz = (old_size < size)? old_size: size;
	/* memcpy region */
	memcpy(new_ptr, ptr, sz);
	CheckForError();
	/* free old region */
	ShmFreeWrapper(ptr);
	/* return new ptr */
	errno = saved_errno;
	return new_ptr;
}

/* Get size of an allocated region */
size_t ShmGetSizeWrapper(void *ptr){
	if(!CheckMPIInitialized())
		return 0;

	return AspaceAvlSearchWrapper(ptr2offset(ptr));
}
/*-------------------------------------------------------------------------------*/
/* public interface for freeing shared pages */
int ShmFreeWrapper(void *ptr){
	if(!CheckMPIInitialized())
		return -1;

	intptr_t size = AspaceAvlRemoveWrapper(ptr2offset(ptr));
	if(size <= 0)
		return -1; /* element not found */

//	fprintf(stderr, "free %p %ld\n", ptr, size);

#ifdef MICROTIME_STAT
	MicroTimer mt;
	mt.Start();
#endif /*MICROTIME_STAT */

	int saved_errno = errno;
	errno = 0;
	ASSERTX(SH_UNMAP(ptr, size) == 0); 
	CheckForError();

	WaitSem(mutex);

//	int old_mmap_count = mmapCount;
	bool last_page_shared = false;

	if(mergeMetric != MERGE_DISABLED){
		for(int i =0; i < size; i += PAGE_SIZE){
			void *p = offset2ptr(ptr2offset(ptr)+i);

//			fprintf(stderr, "%d:%p -\n", myRank, p);
//			bool is_zero_page 			= GetBit(zeroPagesBV, (char *)p);
			bool is_initialized_page 	= true;
#ifdef COLLECT_MALLOC_STAT
//			is_initialized_page = GetBit(initializedPagesBV, (char*) p);
			is_initialized_page = ResetAndReturnBit(initializedPagesBV, (char*) p);
#endif /* COLLECT_MALLOC_STAT */


#if 0
			if(is_initialized_page){
				ASSERTX(!(is_zero_page && is_shared_page));
			}else{
				ASSERTX(!is_zero_page);
				ASSERTX(!is_shared_page);
			}
#endif /* 0 */

			if(is_initialized_page){
				bool is_zero_page 			= ResetAndReturnBit(zeroPagesBV, (char *)p);
				bool is_shared_page 		= GetSharingBit(p);

#ifdef SHARED_STATS
				(*baseCaseTotalPageCount)--;
#endif /* SHARED_STATS */

				if(last_page_shared && !is_shared_page){
					mmapCount -=1;
				}


				if(is_zero_page){
//				if( ResetAndReturnBit(zeroPagesBV, (char *)p)){
					/* no change in shared/unshared  page count*/
					zeroPageCount -=1;
					last_page_shared = false;
					mmapCount -=1;
				}else if(is_shared_page){
//				}else if(GetSharingBit(p)){
#ifdef SHARED_STATS
					int sh_cnt;
					switch(sh_cnt = CountSharingProcs(p)){
						case 1: // no one else in the shared region, decrease private count
							(*allProcPrivatePageCount)--;
							break;
						case 2: // there is only one more processor sharing it, so decrease shared count
							(*sharedPageCount)--;
							(*allProcPrivatePageCount)++;
							break;
						default: // more than 1 processor still sharing the page, nothing to change
							if(sh_cnt > *aliveProcs)
								printf("###################### error %d #######################\n", sh_cnt);
							break;
					}
#endif /* !SHARED_STATS */

#ifdef ENABLE_PROFILER
					if(profileMode == CREATE_PROF){ /* dump in file */
						fprintf(profFile, "%p %d %lu\n", (void*)((uintptr_t)ptr + i), -1, (unsigned long)time(NULL));
					}
#endif /* ENABLE_PROFILER */
					last_page_shared = true;
					UnsetSharingBit(p);
				}else{ /* just change the counter */
					(*allProcPrivatePageCount)--;
					last_page_shared = false;
				}
			}else{ /* probably end of previous map if last one was shared */
				if(last_page_shared){
					mmapCount -=1;
					last_page_shared = false;
				}
			}


#ifdef COLLECT_MALLOC_STAT
//			UnsetBit(initializedPagesBV, p);
#endif /* COLLECT_MALLOC_STAT */

//			UnsetBit(zeroPagesBV, p);

//			UnsetSharingBit(p);
		}
	}else{
#ifdef SHARED_STATS
#ifdef COLLECT_MALLOC_STAT
		for(int i =0; i < size; i += PAGE_SIZE){
			void *p = offset2ptr(ptr2offset(ptr)+i);

//			if(GetBit(initializedPagesBV, (char*) p)){
			if(ResetAndReturnBit(initializedPagesBV, (char*) p)){
				(*baseCaseTotalPageCount)--;
				(*allProcPrivatePageCount)--;
			}
		}
#else /* COLLECT_MALLOC_STAT */
		(*allProcPrivatePageCount) -= size/PAGE_SIZE;
		(*baseCaseTotalPageCount) -= size/PAGE_SIZE;
#endif /* !COLLECT_MALLOC_STAT */
#endif /* !SHARED_STATS */
	}

//	fprintf(stderr, " ******** MMAP COUNT REDUCED BY: %d\n", old_mmap_count - mmapCount);
	SignalSem(mutex);
#ifdef MICROTIME_STAT
	mt.Stop();
	freeTime += (mt.GetDiff()?mt.GetDiff():1);
#endif /*MICROTIME_STAT */

#ifndef COLLECT_MALLOC_STAT
	if(mergeMetric == THRESHOLD)
		MergeByTHRESHOLD();
#endif /* COLLECT_MALLOC_STAT */
	
	errno = saved_errno;
	return 1;
}

