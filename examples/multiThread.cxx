//
// Example 2: One Device Per Process Or Thread
//

#include <stdio.h>
#include "cuda_runtime.h"
#include "nccl.h"
#include "mpi.h"
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>

#define MPICHECK(cmd) do {                          \
  int e = cmd;                                      \
  if( e != MPI_SUCCESS ) {                          \
    printf("Failed: MPI error %s:%d '%d'\n",        \
        __FILE__,__LINE__, e);                      \
    exit(EXIT_FAILURE);                             \
  }                                                 \
} while(0)

#define CUDACHECK(cmd) do {                         \
  cudaError_t e = cmd;                              \
  if( e != cudaSuccess ) {                          \
    printf("Failed: Cuda error %s:%d '%s'\n",       \
        __FILE__,__LINE__,cudaGetErrorString(e));   \
    exit(EXIT_FAILURE);                             \
  }                                                 \
} while(0)

#define NCCLCHECK(cmd) do {                         \
  ncclResult_t r = cmd;                             \
  if (r!= ncclSuccess) {                            \
    printf("Failed, NCCL error %s:%d '%s'\n",       \
        __FILE__,__LINE__,ncclGetErrorString(r));   \
    exit(EXIT_FAILURE);                             \
  }                                                 \
} while(0)

static uint64_t getHostHash(const char *string) {
    // Based on DJB2, result = result * 33 + char
    uint64_t result = 5381;
    for (int c = 0; string[c] != '\0'; c++) {
        result = ((result << 5) + result) + string[c];
    }
    return result;
}

static void getHostName(char *hostname, int maxlen) {
    gethostname(hostname, maxlen);
    for (int i = 0; i < maxlen; i++) {
        if (hostname[i] == '.') {
            hostname[i] = '\0';
            return;
        }
    }
}

int main(int argc, char *argv[]) {

    int size = 5242880; //20MB worth of data
    
    int myRank, nRanks, localRank = 0;

    // initializing MPI
    MPICHECK(MPI_Init(&argc, &argv));
    MPICHECK(MPI_Comm_rank(MPI_COMM_WORLD, &myRank));
    MPICHECK(MPI_Comm_size(MPI_COMM_WORLD, &nRanks));

    // calculating localRank based on hostname which is used in
    // selecting a GPU
    uint64_t hostHashs[nRanks];
    char hostname[1024];
    getHostName(hostname, 1024);
    hostHashs[myRank] = getHostHash(hostname);
    MPICHECK(MPI_Allgather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL,
                           hostHashs, sizeof(uint64_t), MPI_BYTE, MPI_COMM_WORLD));

    for (int p = 0; p < nRanks; p++) {
        if (p == myRank) {
            break;
        }
        if (hostHashs[p] == hostHashs[myRank]) {
            localRank++;
        }
    }

    ncclUniqueId id;
    ncclComm_t comm;
    float *sendbuff, *recvbuff;
    cudaStream_t s;

    // get NCCL unique ID at rank 0 and broadcast it to all others
    if (myRank == 0) { //change here to change the order of operations
        ncclGetUniqueId(&id);
    }
    MPICHECK(MPI_Bcast((void *) &id, sizeof(id), MPI_BYTE, 0,
                       MPI_COMM_WORLD));

    
    //&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&

    
    float* local_vals = (float *)malloc(size*sizeof(float));

    for (int i = 0; i<size; i++){
        local_vals[i] = (float)rand()/RAND_MAX * (10);
    }

    printf("Hello from device %d\n", myRank);
    

    CUDACHECK(cudaSetDevice(localRank));
    CUDACHECK(cudaMalloc(&sendbuff, size * sizeof(float)));
    CUDACHECK(cudaMalloc(&recvbuff, size * sizeof(float)));
    CUDACHECK(cudaStreamCreate(&s));


    NCCLCHECK(ncclCommInitRank(&comm, nRanks, id, myRank));

    NCCLCHECK(ncclAllReduce((const void *) sendbuff, (void *) recvbuff,
                            size, ncclFloat, ncclSum,
                            comm, s));

   
    CUDACHECK(cudaStreamSynchronize(s));

    CUDACHECK(cudaFree(sendbuff));
    CUDACHECK(cudaFree(recvbuff));


    ncclCommDestroy(comm);

    //&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&
    MPICHECK(MPI_Finalize());

    printf("[MPI Rank %d] Success \n", myRank);
    return 0;
}