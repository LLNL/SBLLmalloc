#ifndef __INTERNAL_ROUTINES_H__
#define __INTERNAL_ROUTINES_H__

#ifdef __cplusplus
extern "C" {
#endif
	void* internal_malloc(size_t bytes);
	void internal_free(void* mem);
	void* internal_realloc(void* oldmem, size_t bytes);
	void* internal_memalign(size_t alignment, size_t bytes);
	void* internal_valloc(size_t bytes);
	void* internal_calloc(size_t n_elements, size_t elem_size);
	void** internal_icalloc(size_t n, size_t elem_size, void* chunks[]);
	void** internal_icomalloc(size_t n, size_t sizes[], void* chunks[]);
	int internal_mtrim(size_t s);
	size_t internal_musable(void* mem);
	int internal_mallopt(int p, int v);
	void internal_mstats(void);
	size_t internal_footprint(void);
#ifdef __cplusplus
}
#endif

#endif /*__INTERNAL_ROUTINES_H__*/
