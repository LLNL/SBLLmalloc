CC = mpicc
CXX = mpiCC
RM        = rm -f
AR        = ar
RANLIB    = ranlib
PTMALLOC_DIR=ptmalloc3

SYS_FLAGS  = -D_GNU_SOURCE=1 -fPIC
OPT_FLAGS  = -g -O2 
WARN_FLAGS = -Wall -Wstrict-prototypes
SH_FLAGS   = -shared -fPIC
INC_FLAGS  = -I. -I$(PTMALLOC_DIR) -I$(PTMALLOC_DIR)/sysdeps/generic
THR_FLAGS = -DUSE_TSD_DATA_HACK -D_REENTRANT
THR_LIBS  = -lpthread
CFLAGS = $(SYS_FLAGS) $(OPT_FLAGS) $(WARN_FLAGS) $(THR_FLAGS) $(INC_FLAGS)

SBLLMALLOC_OBJ = ptmalloc3.o malloc.o SharedHeap.o AVL.o

all:
	make libsbllmalloc

clean:
	$(RM) $(SBLLMALLOC_OBJ) lib/libsbllmalloc.so core core.[0-9]*

.c.o:
	$(CC) -c $(CFLAGS) $<

AVL.o: AVL.cpp
	$(CXX) $(INC_FLAGS) -D_REENTRANT=1 -D__GNU_SOURCE=1 -fPIC -shared -rdynamic -g -Wall -pipe -O3 -finline-limit=65000 -fkeep-inline-functions -finline-functions -ffast-math -fomit-frame-pointer -c -o $@ $<

SharedHeap.o: SharedHeap.cpp
	$(CXX) $(INC_FLAGS) -D_REENTRANT=1 -D__GNU_SOURCE=1 -fPIC -shared -rdynamic -g -Wall -pipe -O3 -finline-limit=65000 -fkeep-inline-functions -finline-functions -ffast-math -fomit-frame-pointer -c -o $@ $<

ptmalloc3.o: $(PTMALLOC_DIR)/ptmalloc3.c $(PTMALLOC_DIR)/malloc-2.8.3.h Globals.h
	$(CC) -c -DENABLE_SHM_MALLOC=1 $(CFLAGS) $(M_FLAGS) -DMSPACES=1 $< -o $@

malloc.o: $(PTMALLOC_DIR)/malloc.c
	$(CC) -c $(CFLAGS) $(M_FLAGS) -DONLY_MSPACES -DUSE_LOCKS=0 $<

libsbllmalloc: $(SBLLMALLOC_OBJ)
	$(CXX) $(SH_FLAGS) $(CFLAGS) $(M_FLAGS) $(SBLLMALLOC_OBJ) -o lib/libsbllmalloc.so

dist:
	make clean
	cd .. && tar zcvf sbllmalloc-1.0.tar.gz sbllmalloc-1.0 && cd -

# dependencies
ptmalloc3.o: $(PTMALLOC_DIR)/malloc-private.h Globals.h
