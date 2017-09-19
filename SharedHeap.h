/*!
  @file SharedHeap.h
  @author Susmit Biswas
  @version 1.0
  @date 2009-2010

  @brief Header file for SBLLmalloc
 */

#ifndef __SHAREDHEAP_H__
#define __SHAREDHEAP_H__

#include <iostream>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <mpi.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/resource.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <sys/types.h>
#include <Globals.h>
#include <AVL.h>

#if defined(_AIX)
#include	<sys/select.h>
#include 	<signal.h>
#endif /* _AIX */

#if defined(linux)
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <syscall.h>
#include <unistd.h>
#endif /* linux */

//#define MICROTIME_STAT

#ifdef MICROTIME_STAT
#include "MicroTimer.h"
#endif /* MICROTIME_STAT*/

/*! @brief Used for frequency based merge as default frequency */
#define MALLOC_REF_FREQ 1000

/*! @brief Used for buffer based merge as default frequency */
#define BUFFER_LENGTH 10000

/*! @brief Converts pointer to uintptr_t */
#define ptr2offset(x) ((uintptr_t)x)

/*! @brief Converts uintptr_t to pointer */
#define offset2ptr(x) ((void *)(x))

/*! @brief Enables shared variables for statistics collection
 * @attention \b REQUIRED if you plan on using threshold based merge (\b recommended) */
#define SHARED_STATS

/*! @brief Enabled statistics collection for mallocs 
  * @attention Requires setting \c SHARED_STATS 
 */
#define COLLECT_MALLOC_STAT

#ifdef COLLECT_MALLOC_STAT
#define SHARED_STATS
#endif

/*! @brief Enables profiling */
//#define ENABLE_PROFILER

/*! @brief Collect sub-block merging stats */
//#define PART_BLOCK_MERGE_STAT

/*! @brief Flags for controlling msg level */
#define PRINT_STATS
//#define PRINT_CONFIG
//#define PRINT_DEBUG_MSG

#ifdef PRINT_DEBUG_MSG
#define PRINT_CONFIG
#endif /* PRINT_DEBUG_MSG */

#ifdef PRINT_CONFIG
#define PRINT_STATS
#endif /* PRINT_CONFIG */


#include <execinfo.h>

/*! @brief Issues warning message */
#define warn(msg) {\
	fprintf(stderr, "%d:warning! %s:%d: %s\n", myRank, __FILE__, __LINE__, msg); \
	fflush(stderr); \
}

/*! @brief Issues warning message \em m and dies with \c MPI_Abort */
#define die(m) {perror(m); fflush(stderr); MPI_Abort(MPI_COMM_WORLD, MPI_ERR_OTHER);}


#define NDEBUG

/*! @brief Reports error by printing error address and backtrace */
#ifndef NDEBUG
#define ReportError(addr) {\
	fprintf(stderr, "%d:error cond %d! %d: %p\n", myRank, errno, __LINE__, (void*)addr); \
	fflush(stderr); \
	void *bt_array[100];\
	size_t arr_size; \
	arr_size = backtrace (bt_array, 100); \
	backtrace_symbols_fd(bt_array, arr_size, 2); \
	fprintf(stderr, "\n\n");\
	fflush(stderr);\
}
#else
#define ReportError(addr)
#endif /*NDEBUG*/

/*! @brief Reports error if errno is set by printing source info and backtrace */
#ifndef NDEBUG
#define CheckForError() {\
	if(errno){ \
		fprintf(stderr, "%d:error! %s from %s:%d\n", myRank, strerror(errno), __FILE__, __LINE__); \
		fflush(stderr); \
		void *bt_array[100];\
		size_t arr_size; \
		arr_size = backtrace (bt_array, 10); \
		backtrace_symbols_fd(bt_array, arr_size, 2); \
		fprintf(stderr, "\n\n");\
		fflush(stderr);\
	} \
}
#else
#define CheckForError()
#endif /*NDEBUG*/

/*! @brief If the evaluated expression is false, prints backtrace and exits */
#ifndef NDEBUG
#define ASSERTX(a) {\
	if(!(a)){ \
		warn("error: assert failure"); \
		fprintf(stderr, "mmap_count: %d\n", mmapCount); \
		{\
		void *bt_array[100];\
		size_t arr_size; \
		arr_size = backtrace (bt_array, 10); \
		backtrace_symbols_fd(bt_array, arr_size, 2); \
		}\
		Fatal();\
	}\
}
#else
#define ASSERTX(a) assert(a)
#endif /*NDEBUG*/




#ifdef  __GNUC__
/*! Wrapper for \c mmap64 to count number of maps */
#define SH_MMAP(args...) ( \
	mmapCount += 1,\
	mmap64(args)\
)

/*! Wrapper for \c munmap to count number of maps */
#define SH_UNMAP(args...) ( \
	mmapCount -= 1,\
	munmap(args)\
)

#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L

#define SH_MMAP(...) ( \
	mmapCount += 1,\
	mmap64(__VA_ARGS__)\
)
#define SH_UNMAP(...) ( \
	mmapCount -= 1,\
	munmap(__VA_ARGS__)\
)

#else /* no __GNUC__, no C99*/

inline void *SH_MMAP(...){
	mmapCount += 1;
	return mmap64( __VA_ARGS__);
}
inline int SH_UNMAP(...) ( \
	mmapCount -= 1;
	return munmap( __VA_ARGS__);
)
#endif /* no __GNUC__ */

/*! @brief Different merge metric. Set 0 to disable */
enum _MERGE_METRICS {
	MERGE_DISABLED, /**< 0:Disable merging */
	ALLOC_FREQUENCY, /**< 1:Frequency based merging */ 
	THRESHOLD, /**< 2:Threshold based merging (recommended) */
	BUFFERED, /**< 3:Buffered merging (\b EXPERIMENTAL, please do not use) */
	NUM_METRIC /**< Number of merge policies */
};

/*! @brief Different profile modes. Set 0 to disable */
enum _PROFILE_MODES {
	NONE, /**< No profiling */
	CREATE_PROF,  /**< Create merge profile */
	USE_PROF,  /**< Use profiles for merging (\b NOT SUPPORTED) */
	NUM_MODES /**< Num profiling modes */
};

/*! @brief The structure for storing merge info */
typedef struct MemStatStruct{
	long int totalPrivateMem; /**< Total memory as private pages */
	long int totalPtmallocMem; /**< Total memory used by internal allocator */
	long int totalZeroMem; /**< Total zero memory in current process */
	long int totalSharedMem; /**< Total shared memory usage */
	long int totalUnmergedMem; /**< Memory footprint if merging is disabled */
	long int totalMergedMem; /**< Memory footprint with merging enabled */
	int mergeTimeinMicrosec; /**< Time used for merging in microsecond */
}MemStatStruct;
/*===============================================================================*/
/*                         forward function declarations                         */ 
/*===============================================================================*/

/*! @brief Collects current memory usage
 * @warn Not locking for aliveProcs as it is updated in the beginning of startup 
 * @return None
 * */
void StoreMemUsageStat();

/*! @brief Stores memory usage in a buffer and flushes to file when the buffer is full
  @param[in] tpm Total memory as private pages
  @param[in] tptm Total memory used by internal allocator
  @param[in] tzm Total zero memory in current process
  @param[in] tsm Total shared memory usage
  @param[in] tum Memory footprint if merging is disabled
  @param[in] tmm Memory footprint with merging enabled
  @param[in] mtm Time used for merging in microsecond
 */
void UpdateMergeStat(
		const long int tpm, 
		const long int tptm, 
		const long int tzm, 
		const long int tsm, 
		const long int tum, 
		const long int tmm, 
		const int mtm);

/*! @brief Flushes merge stat buffer to file */
void PrintMergeStat();

/*! @brief Replaces \c MPI_Init of mpi library and initializes shared memory, metadata etc.
 * param argc argc from main(int argc, char **argv)
 * param argv argv from main(int argc, char **argv)
 * @return Result of PMPI_Init 
 */
int MPI_Init(int *argc, char ***argv);

/*! @brief Replaces the \c MPI_Finalize to call \c PMPI_Finalize() */
int MPI_Finalize();

/*! @brief Initializes shared region and sets segfault handler 
 * @return None
 */
void InitAddrSpace();

/*! @brief Cleans up shared data: deletes AVL, closes files, unmaps shared
 * region, destroys semaphores */
void CleanUpSharedData();

/*!  @brief allocates a shared region to account for merged pages. 
 * @return None
 **/
void AllocateSharedMetadata();

/*! @brief Aborts execution. Called upon encountering error. */
void Fatal();

/*! @brief Reads proc maps and finds the address range where this malloc
 * library is allocated */
void GetMemRange(); 					

/*! @brief Structure for parsing arguments */
typedef	struct commandLineArgument {
	const char 	*name; /**< Name of the argument */
	int 		*variable; /**< Address of the variable to store of argument */
	int		 	default_val; /**< Default value */
	const char 	*description; /**< Description of the argument*/
} commandLineArgument;

/*! @brief Checks environment variables for sanity */
void CheckEnv();

/*! @brief Initializes the parameters for the library.*/
void InitEnv();

/*!
 *  @brief Computes \c floor(log2(n))
 *  Works by finding position of MSB set.
 *  @return -1 if n == 0.
 */
int FloorLog2(unsigned long);

/*!
 *  @brief Computes \c ceil(log2(n))
 *  @see FloorLog2
 *  @return -1 if n == 0.
 */
int CeilLog2(unsigned long);

/*! @brief Translate mmapped address to file offset
 * @param addr Address of the page to be translated
 * @return 0 if out of address range, translated address otherwise*/
uintptr_t TranslateMmapAddr(uintptr_t);

/*! @brief Checks if \c MPI_Init has been called or is not a MPI app
 * @return true when MPI has been Initialized/ not MPI app . */
bool CheckMPIInitialized();

/*! @brief Checks if number of mmap calls are close to system limit.
 * @return true if close to limit 
 * @deprecated By setting \c vm.max_map_count to a large value (e.g. 512K), we do not need this check
 */
bool IsCloseToMmapLimit(int newRequest = 0);

/*! @brief Initializes a \c POSIX semaphore after getting it 
 * In linux, semkey has to be an existing filename beginning with / but not
 * having more than 14 chars and more slashes 
 * @param SEMKEY Name of the semaphore
 * @param mutex The semaphore address
 * */
void InitSem(char *SEMKEY, sem_t **mutex);

/*! @brief operation \b V: used for semaphore handling 
 * @param mutex Address of the semaphore * */
void SignalSem (sem_t *mutex);

/*! @brief operation \b P: used for semaphore handling 
 * @param mutex Address of the semaphore */
void WaitSem (sem_t *mutex);

/*! @brief SIGSEGV signal handler.
 * Handles write faults for readonly marked shared pages. If the page was never
 * touched, the permission bit is changed and returned. Otherwise, a copy of
 * the page is created in private region. 
 * @param signo The signal number
 * @param si \c siginfo_t provides the information regarding fault address and type
 * @param sc Not used in this routine
 * */ 
void SigSegvHandler(int signo, siginfo_t * si , void *sc);

/*!  @brief SIGBUS signal handler 
  * @see SigSegvHandler
 */
void SigBusHandler(int signo, siginfo_t *si, void *sc);

/*!  @brief SIGINT signal handler
  * @see SigSegvHandler */
void SigIntHandler(int signo, siginfo_t *si, void *sc);

/*------------------ bit vector manipulation routines ----------------------*/
/*! @brief Translates page address to page number
 * @return Page number */
uintptr_t Addr2PageIndex(void *);

/*! @brief Counts the number of tasks sharing a page.
 * @param addr Address of the page
 * @return the Number of sharing processes */
int CountSharingProcs(void *addr);

/*! @brief Sets the sharing bit corresponding to page having address addr for
 * currest process*/
void SetSharingBit(void *addr);

/*! @brief Unsets the sharing bit corresponding to page having address addr for
 * currest process*/
void UnsetSharingBit(void *addr);

/*! @brief Gets the sharing bit corresponding to page having address addr for
 * currest process*/
bool GetSharingBit(void *addr);

/*! @brief Checks any other process shares the page having address addr by
 * checking sharingProcessesInfo bits */
bool IsOtherSharing(void *addr);

/*! @brief Gets bit corresponding to an address from bitvector array 
 * @param array Bit vector
 * @param page_addr Address of the page
 * @return The bit for address page_addr from array bit vector */
bool GetBit(char *array, char *page_addr);

/*! @ Sets bit corresponding to an address from bitvector array 
 * @param array Bit vector
 * @param page_addr Address of the page
 * @return None */
void SetBit(char *array, char *page_addr);

/*! @ Unsets bit corresponding to an address from bitvector array 
 * @param array Bit vector
 * @param page_addr Address of the page
 * @return None */
void UnsetBit(char *array, void *page_addr);

/*! @ Sets multiple bits corresponding to a region, more than 1 page from bitvector array 
 * @param array Bit vector
 * @param page_addr Start address of the region
 * @param size Size of the region
 * @return None */
void SetMultiBits(char *array, char *page_addr, size_t size);

/*! @brief Resets and returns old value of bit in array corresponding to page address */
bool ResetAndReturnBit(char *, char *);

/*! @brief Sets and returns old value of bit in array corresponding to page address */
bool SetAndReturnBit(char *, char *);
/*------------------------------ misc routines ------------------------------*/

/*!  @brief Inserts a node in address space AVL tree with start_addr and size. called upon malloc 
 * @return 0 */
int AspaceAvlInsertWrapper(uintptr_t start_addr, size_t size);

/*!  @brief Deletes a node from AVL tree with start_addr. Called upon free 
 * @return The size of the region, 0 if not found.
 * */
intptr_t AspaceAvlRemoveWrapper(uintptr_t start_addr);

/*! @brief Searches a node with start_addr
 * @return 0 if not found, the size of the region if found */
uintptr_t AspaceAvlSearchWrapper(uintptr_t start_addr);

/*! @brief Searches a range with start_addr included in an existing range
 * @return 0 if not found, the size of the region if found */
uintptr_t AspaceAvlSearchRangeWrapper(uintptr_t start_addr);

/*!  @brief AVL data structure compartor
 * @return 0 if identical, otherwise the difference as 1 or -1 */
int MyComparator(const void *key1, const void *key2);

/*------------------------------ shm routines -------------------------------*/
/*!  @brief Merges pages based on frequency of mallocs.
 * When the number of outstanding malloc/free becomes more than x where x is
 * the merging frequency, merge operation is triggered. */
void MergeByALLOC_FREQUENCY();

/*!  @brief Merges pages based on threshold
 * When the amount of used memory changes by more than 1000 pages i.e. 4MB,
 * merge operation is triggered
 */
void MergeByTHRESHOLD();

/*!  @brief merges pages when the buffer of dirty pages becomes full 
 * @warn Experimental. NOT EXTENSIVELY TESTED */
void MergeByBUFFERED();

/*! @brief Tries to merge pages of current process 
 * @param p Address of the page to be compared 
 * @param creator_addr Address of the creator of page p
 * @deprecated This routine is not tested thouroughly, and \c MergeByTHRESHOLD is more apt.
 * @return 0 if not merged. 1 if merged */
int MergePages(void *p, uintptr_t creator_addr);

/*!  @brief a node corresponding to <key, value> pair is checked so that
 * identical data pages can be merged 
 * @param key Address of start address of the region to check for merging
 * @param value Size of the region
 * @param data Address of the creator of the region
 * @deprecated \c MergeNode2 is more efficiently implemented
 * @see MergeNode2
 */
void MergeNode(const void *key, const void *value, const void *data);

/*!  @brief Frees up a node corresponding to <key, value> pair 
 * @param key Address of start address of the region to check for merging
 * @param value Size of the region
 * @param data Address of the creator of the region (not used)
 * @param isDirty Flag indicating whether the region is dirty (not used)
 */
void FreeNode(const void *key, const void *value, const void *data, void *isDirty);

/*!  @brief Allocates a shared region
 * @param isFixed If set, map at a fixed address
 * @param addr 	@if addr != NULL 
 					@if isFixed == true
						map the shared region for address addr at fixed address addr
					@else
						map the shared region for address addr without fixing it
					@endif
				@else 
				@note //Not used
					first map a private region.
					Then map a shared region at the address of the private region
				@endif
 * @param size Size of the region
 * @return Address of the mapped region or MAP_FAILED if error encountered */
void *GetSharedRegion(void* addr, bool isFixed, size_t size);

/*!  @brief Allocates a shared page 
 * Calls GetSharedRegion with addr, isFixed, PAGE_SIZE.
 * @see GetSharedRegion
 * @return Address of the page */
void *GetSharedPage(void* addr, bool isFixed);

/*! @brief A node corresponding to <key, value> pair is checked so that
 * identical data pages can be merged. Merges many pages at once. 
 * @param key Address of start address of the region to check for merging
 * @param value Size of the region
 * @param data Call stack when the region was created
 * @param isDirty Flag indicating if the region is dirty
 */
void MergeNode2(const void *key, const void *value, const void *data, void *isDirty);

/*!  @brief makes a region readonly 
  * @param addr Start address of the region
  * @param len Size of the region
 */
void MakeReadOnlyWrapper(void *addr, size_t len); 

/*!  @brief makes a region writeable
  * @param addr Start address of the region
  * @param len Size of the region
 */
void MakeReadWriteWrapper(void *addr, size_t len);

/*! @brief Finds address of the source that called malloc 
 * @return The source of the malloc call i.e. the source address outside this library
 */
uintptr_t GetBacktrace();

/*! @brief Stores the call stack when malloc is called. 
 * It excludes the library addresses from the call trace.
 * @return None
 */
void GetCallStack(void **stack, int depth);
/*! @brief Copies and maps pages from private region to shared space
  * @param start Address of the start of the region
  * @param size Size of the region
 * @return -1 if failure, 0 if successful */ 
int CopyAndRemapRegion(void *start, size_t size);

/*! @brief Maps pages from private region to shared space
 * Some other process shared this memory region, so just remapping required here
  * @param start Address of the start of the region
  * @param size Size of the region
 * @return -1 if failure, 0 if successful */ 
int RemapRegion(void *start, size_t size);

/*! @brief Remaps the pages to the zero page 
  * @param start Address of the start of the region
  * @param size Size of the region
 * @return 0 if successful, -1 otherwise
 * */
int RemapToZero(void *start, size_t size);

/*! @brief Merges many pages
 * This is the main routine that identifies the type of merge and invokes
 * appropriate functions. It is a state machine based implementation to reduce
 * fragmentation on the number of mmaps
  * @param start Address of the start of the region
  * @param size Size of the region
  * @param data Call stack of the region
 * @return 0 if successful, -1 otherwise
 * */
int MergeManyPages(uintptr_t start_addr, size_t size, const void* data);

/*------------------------------ Profile based merge routines -------------------------------*/
/*! 
 * @brief Checks merge profile to decide if the page should be merged
 * If the page is mergeable, updates time.
 * @param pageAddr Address of the page under consideration for merge.
 * @param currTime Current Time. lastMergeTime is updated if the page can be merged.
 * @return Whether the page is mergeable
 * */
bool CheckIfMergeable(void *pageAddr, uint32_t currTime);

/*! 
 * @brief Updates merge profile.
 * If the split is before a threshold of time, it is considered a failure.
 * @param pageAddr Address of the page having segfault.
 * @param currTime Current Time. lastMergeTime is compared with it to see if the last merge was successful.
 * */
void UpdateMergeHist(void *pageAddr, uint32_t currTime);

#endif /* __SHAREDHEAP_H__ */
