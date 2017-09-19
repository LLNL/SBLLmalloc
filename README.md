# SBLLmalloc
SBLLmalloc is a memory allocation library developed by Susmit Biswas, which automatically identifies the replicated data memory blocks and merges them into a single copy. The name of the code is derived from the name of UC Santa Barbara and Lawrence Livermore National Laboratory as the project resulted from a collaboration. SBLLmalloc does not require any application or OS changes since it is implemented as a user-level library that can be linked at runtime. The document distributed with the library provides a clear description of this library along with its internals. This library has been tested on standard linux distribution such as Ubuntu and Chaos kernels with mpich and mvapich distributions.
Using SBLLmalloc
In order to use merge capability, you need to set some environment variables which triggers merge operations. If the original command line is

srun -nx -Ny ,

you will need to change it to the following command.

MPIRUN="srun -nx -Ny" TH= ENV_VAR1= [ENV_VAR2= ...] $TOPDIR/run/submitjob_v2.sh

Please read the document distributed with the software to use with your application.
