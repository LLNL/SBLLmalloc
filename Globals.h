#ifndef __GLOBALS_H__
#define __GLOBALS_H__

/*!
  @file Globals.h
  @author Susmit Biswas
  @version 1.0
  @date 2009-2010

  @brief Contains global definitions and public interface
 */

#include <string.h>
#if !defined(WIN32)
#include <strings.h>
#endif /* WIN32*/

/*-------------------------- public interfaces -----------------------------*/
#ifdef __cplusplus
extern "C" {
#endif
	/*! @brief public interface for allocating shared pages 
	  * @param sz Size of allocation
	  * @return Address of the region if successful, NULL otherwise
	 */
	void*	ShmMallocWrapper(size_t sz);

	/*! @brief public interface for reallocating shared pages 
	  * @param ptr Address of the old allocation
	  * @param sz Size of allocation
	  * @return Address of the region if successful, NULL otherwise
	 */
	void*	ShmReallocWrapper(void *ptr, size_t sz);

	/*!  @brief public interface for freeing shared pages 
	  * @param ptr Address of the allocation
	  * @return -1 if not allocated using SH_MMAP, 1 otherwise
	 */
	int 	ShmFreeWrapper(void *ptr);
	
	/*! @brief Gets size of an allocated region 
	  * @param ptr Address of the allocation
	  * @return size of the region
	 */
	size_t 	ShmGetSizeWrapper(void *ptr);
#ifdef __cplusplus
}
#endif


/* For porting this wrapper with any malloc library, replace the following
 * routines with proper ones and include globals.h in malloc libray and then
 * call the Shm*****Wrapper from malloc hooks 
 * */

#ifdef __cplusplus
extern "C" {
#endif
#define ptmalloc internal_malloc
#define ptfree internal_free
#define ptmalloc_get_mem_usage internal_footprint
#include <internal-routines.h>
#ifdef __cplusplus
}
#endif



#endif /* __GLOBALS_H__*/
