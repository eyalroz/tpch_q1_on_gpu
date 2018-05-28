#include <stdio.h>
#include "helper.hpp"
#include "dates.hpp"
#include "../expl_comp_strat/common.hpp"
#include <cooperative_groups.h>

using namespace cooperative_groups;
using u64_t = unsigned long long int;
namespace cuda{
  
    __global__
    void print() {
        // who am I?
        int wid = global_warp_id();
        int lid = warp_local_thread_id();
        printf(" Global Warp: %d Local Warp: %d \n", wid, lid);
    }
    __device__ 
    /*int atomicAggInc_primitives(int *ctr) {
        unsigned int active = __activemask();
        int leader = __ffs(active) - 1;
        int change = __popc(active);
        unsigned int rank = __popc(active & __lanemask_lt());
        int warp_res;
        if(rank == 0)
            warp_res = atomicAdd(ctr, change);
        warp_res = __shfl_sync(active, warp_res, leader);
        return warp_res + rank;
    }*/

    __device__ 
    int atomicAggInc(int *ctr) {
        auto g = coalesced_threads();
        int warp_res;
        if(g.thread_rank() == 0)
            warp_res = atomicAdd(ctr, g.size());
        return g.shfl(warp_res, 0) + g.thread_rank();
    }

    __global__ 
    void filter_k(int *dst, int *nres, const int *src, int n) {
        int i = threadIdx.x + blockIdx.x * blockDim.x;
        if(i >= n)
            return;
        if(src[i] > 0)
            dst[atomicAggInc(nres)] = src[i];
    }

    __inline__ __device__
    int warpReduceSum(int val) {
        for (int offset = warp_size / 2; offset > 0; offset /= 2) 
            val += __shfl_down(val, offset);
        return val;
    }

    __inline__ __device__
    int blockReduceSum(int val) {

        static __shared__ int shared[32]; // Shared mem for 32 partial sums
        int lane = warp_local_thread_id();
        int wid = block_local_warp_id();

        val = cuda::warpReduceSum(val);     // Each warp performs partial reduction

        if (lane == 0) shared[wid] = val; // Write reduced value to shared memory

        __syncthreads();              // Wait for all partial reductions

        //read from shared memory only if that warp existed
        val = (threadIdx.x < blockDim.x / warpSize) ? shared[lane] : 0;

        if (wid==0) val = cuda::warpReduceSum(val); //Final reduce within first warp

        return val;
    }

    __global__ 
    void deviceReduceKernel(int *in, int* out, int N) {
        int sum = 0;
        //reduce multiple elements per thread
        for (int i = blockIdx.x * blockDim.x + threadIdx.x; 
            i < N; 
            i += blockDim.x * gridDim.x) {
                sum += in[i];
        }
        printf("%d\n", sum);
        sum = cuda::blockReduceSum(sum);
        if (block_local_thread_id() == 0){
            printf("%d %d\n",blockIdx.x, sum);
            out[blockIdx.x] = sum;
        }
    }

    __global__
    void naive_tpchQ01(int *shipdate, int *discount, int *extendedprice, int *tax, 
        char *returnflag, char *linestatus, int *quantity, AggrHashTable *aggregations, size_t cardinality){

        int i = blockIdx.x * blockDim.x + threadIdx.x;
        //int stride = blockDim.x * gridDim.x;

        //for(size_t i = index ; i < cardinality; i++) {
            if (i < cardinality && shipdate[i] <= todate_(2, 9, 1998)) {
                const auto disc = discount[i];
                const auto price = extendedprice[i];
                const auto disc_1 = Decimal64::ToValue(1, 0) - disc;
                const auto tax_1 = tax[i] + Decimal64::ToValue(1, 0);
                const auto disc_price = Decimal64::Mul(disc_1, price);
                const auto charge = Decimal64::Mul(disc_price, tax_1);
                const idx_t idx = returnflag[i] << 8 | linestatus[i];
                atomicAdd((u64_t*)&(aggregations[idx].sum_quantity), (u64_t) quantity[i]);
                atomicAdd((u64_t*)&(aggregations[idx].sum_base_price), (u64_t)price);
                atomicAdd((u64_t*)&(aggregations[idx].sum_disc_price), (u64_t)int128_add64(aggregations[idx].sum_disc_price, disc_price));
                atomicAdd((u64_t*)&(aggregations[idx].sum_charge), (u64_t)int128_add64(aggregations[idx].sum_charge, charge));
                atomicAdd((u64_t*)&(aggregations[idx].sum_disc), (u64_t)disc);
                atomicAdd((u64_t*)&(aggregations[idx].count), (u64_t)1);
                
            }
            //__syncthreads();
        //}

    }
    __device__
    void aggregate() {
    }

        __global__
    void blockwise01_tpchQ01(int *shipdate, int *discount, int *extendedprice, int *tax, 
        char *returnflag, char *linestatus, int *quantity, AggrHashTable *aggregations, size_t cardinality) {
        const uint32_t ENTRIES = 40000 / sizeof(AggrHashTableKey); //[49152 / sizeof(AggrHashTableKey)];
        const uint32_t EMPTY = 0;
        __shared__ AggrHashTableKey hashtable[ENTRIES];

        u64_t i = blockIdx.x * blockDim.x + threadIdx.x;
        if (threadIdx.x == 0) {
            memset(hashtable, 0, sizeof(hashtable));
        }
        __syncthreads();
        
        if(i >= cardinality)
            return;

       // for(; i < next; i++) {
            if(shipdate[i] <= todate_(2, 9, 1998)) {
                uint32_t key = (uint32_t) (returnflag[i] << 8 | linestatus[i]);
                uint32_t position = key % ENTRIES;
                uint32_t entry = hashtable[position].key;
                if (entry == EMPTY) {
                    // empty
                    uint32_t old = atomicCAS(&hashtable[position].key, EMPTY, key);
                    if (old != EMPTY) {
                        return;
                    }
                } else if (key != entry) {
                    return;
                }



                //dst[atomicAggInc(nres)] = src[i];
                const auto disc = discount[i];
                const auto price = extendedprice[i];
                const auto disc_1 = Decimal64::ToValue(1, 0) - disc;
                const auto tax_1 = tax[i] + Decimal64::ToValue(1, 0);
                const auto disc_price = Decimal64::Mul(disc_1, price);
                const auto charge = Decimal64::Mul(disc_price, tax_1);
             
                atomicAdd((u64_t*)&(hashtable[position].sum_quantity), (u64_t) quantity[i]);
                atomicAdd((u64_t*)&(hashtable[position].sum_base_price), (u64_t)price);
                atomicAdd((u64_t*)&(hashtable[position].sum_disc_price), disc_price);
                atomicAdd((u64_t*)&(hashtable[position].sum_charge), charge);
                atomicAdd((u64_t*)&(hashtable[position].sum_disc), (u64_t)disc);
                atomicAdd((u64_t*)&(hashtable[position].count), (u64_t)1);
            }
        //}
        __syncthreads();
        if (threadIdx.x == 0) {
            for(uint32_t i = 0; i < ENTRIES; i++) {
                const idx_t idx = hashtable[i].key;
                if (idx != EMPTY) {
                    atomicAdd((u64_t*)&(aggregations[idx].sum_quantity), (u64_t) hashtable[i].sum_quantity);
                    atomicAdd((u64_t*)&(aggregations[idx].sum_base_price), (u64_t) hashtable[i].sum_base_price);
                    atomicAdd((u64_t*)&(aggregations[idx].sum_disc_price), (u64_t) hashtable[i].sum_disc_price);
                    atomicAdd((u64_t*)&(aggregations[idx].sum_charge), (u64_t) hashtable[i].sum_charge);
                    atomicAdd((u64_t*)&(aggregations[idx].sum_disc), (u64_t) hashtable[i].sum_disc);
                    atomicAdd((u64_t*)&(aggregations[idx].count), (u64_t) hashtable[i].count);
                }
            }
        }
    }
}
