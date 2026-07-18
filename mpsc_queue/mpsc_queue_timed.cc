/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
* Distributed under BSD 3-Clause license.                                   *
* Copyright by The HDF Group.                                               *
* Copyright by the Illinois Institute of Technology.                        *
* All rights reserved.                                                      *
*                                                                           *
* This file is part of Hermes. The full Hermes copyright notice, including  *
* terms governing use, modification, and redistribution, is contained in    *
* the COPYING file, which can be found at the top directory. If you do not  *
* have access to the file, you may request a copy from help@hdfgroup.org.   *
* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <mpi.h>

// #include "basic_test.h"
#include <hermes_shm/data_structures/ipc/ring_ptr_queue.h>
#include <hermes_shm/data_structures/ipc/string.h>
#include <hermes_shm/util/affinity.h>
#include <hermes_shm/util/error.h>
#include <hermes_shm/util/timer.h>
#include "test_init.h"

#define PINGPONG_DEFAULT 1000000

int main(int argc, char** argv) { 
    MPI_Init(&argc, &argv);

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    int pingpongs = PINGPONG_DEFAULT;
    if (argc > 1) {
        pingpongs = std::atoi(argv[1]);
    }

    MainPretest(); //set up our allocator and shared mem 

    // The allocator was initialized in test_init.c
    // we are getting the "header" of the allocator
    // auto *alloc = HSHM_DEFAULT_ALLOC;
    auto* alloc = HSHM_MEMORY_MANAGER->GetAllocator<HSHM_DEFAULT_ALLOC_T>(AllocatorId(1,0));

    if (!alloc) {
        std::cerr << "alloc  is null" << std::endl;
        MPI_Finalize();
        return -1;
    }
    auto *queue_ = alloc->GetCustomHeader<hipc::delay_ar<sub::ipc::mpsc_ptr_queue<int>>>();
    if (!queue_) {
        std::cerr << "QUEUE is null!" << std::endl;
        MPI_Finalize();
        return -1;
    }

    //init hshm timer
    hshm::Timer timer;  

    // Make the queue uptr
    if (rank == RANK0) {
        // Rank 0 create the pointer queue
        queue_->shm_init(alloc, 100000);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // Affine to CPU 0
        hshm::ProcessAffiner::SetCpuAffinity(HSHM_SYSTEM_INFO->pid_, 0);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (rank != RANK0) {
        // Affine to CPU 1
        hshm::ProcessAffiner::SetCpuAffinity(HSHM_SYSTEM_INFO->pid_, 1);
    }

    MPI_Barrier(MPI_COMM_WORLD);
    sub::ipc::mpsc_ptr_queue<int> *queue = queue_->get();

    std::cout << "[RANK " << rank << "] queue_: " << queue_ << "\n";
    std::cout << "[RANK " << rank << "] queue_->get(): " << queue_->get() << "\n";

    MPI_Barrier(MPI_COMM_WORLD);
    //start the timer after we syncrhonize at barrier
    timer.Resume();

    for (int k = 0; k < pingpongs; k++) {
        if (rank == RANK0) {
            // Emplace values into the queue
            queue->emplace(k);
        } else {
            // Pop entries from the queue
            int x;
            while (!queue->pop(x).IsNull()) {
                // printf("POP %d\n", k);
            }
        }
    }

    double total_time = timer.Pause(); //pause the timer

    // The barrier is necessary so that
    // Rank 0 doesn't exit before Rank 1
    // The uptr frees data when rank 0 exits.
    MPI_Barrier(MPI_COMM_WORLD);
    
    if (rank == RANK0) {
        float avg_time_ns = (1.0 * total_time) / (1.0 * pingpongs);
        printf("Average RTT: %.10f ns (%.10f us)\n", avg_time_ns, avg_time_ns / 1000.0);
    }
    MainPosttest();
    MPI_Finalize();

    return 0;
}
