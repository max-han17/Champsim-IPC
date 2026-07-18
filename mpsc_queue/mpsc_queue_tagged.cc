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

#define ITERS_DEFAULT 1000000 //1 million main loop
#define ROI_BEGIN() __asm__ __volatile__ ("xchg %%bx, %%bx" ::: "bx")
#define ROI_END()   __asm__ __volatile__ ("xchg %%cx, %%cx" ::: "cx")

// #define TRACE_ACCESS(rank, label, ptr) \
//   std::cout << "[Rank " << rank << "] " << label << " at " << static_cast<void*>(ptr) << std::endl



struct Task {
    hipc::atomic<bool> done_;
};

int main(int argc, char** argv) { 
    MPI_Init(&argc, &argv);

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    int iters = ITERS_DEFAULT;
    if (argc > 1) {
        iters = std::atoi(argv[1]);
    }

    MainPretest(); //set up our allocator and shared mem 
    std::cout << "CHECKPOINT:  INITIALIZED" << std::endl;
    std::ofstream app_log("/tmp/app_progress.log", std::ios::app);
    app_log << "PID " << getpid() << " reached MainPretest at " << time(NULL) << std::endl;
    app_log.close();

    // The allocator was initialized in test_init.c
    // we are getting the "header" of the allocator
    // auto *alloc = HSHM_DEFAULT_ALLOC;
    auto* alloc = HSHM_MEMORY_MANAGER->GetAllocator<HSHM_DEFAULT_ALLOC_T>(AllocatorId(1,0));

    hshm::ipc::FullPtr<Task> task;

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

    // Make the queue uptr
    if (rank == RANK0) {
        // Rank 0 create the pointer queue
        queue_->shm_init(alloc, 100000);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        //create task structure
        task = alloc->AllocateLocalPtr<Task>(HSHM_MCTX, sizeof(Task)); //local ptr allocation
        hipc::Pointer task_shm = task.shm_;
        MPI_Send(&task_shm, sizeof(task_shm), MPI_BYTE, 1, 0, MPI_COMM_WORLD); //send task.shm_ to other process


        // Affine to CPU 0
        hshm::ProcessAffiner::SetCpuAffinity(HSHM_SYSTEM_INFO->pid_, 0);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (rank != RANK0) {

        hipc::Pointer task_shm;
        MPI_Recv(&task_shm, sizeof(task_shm), MPI_BYTE, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE); //recv task.shm_ from other process

        task = hshm::ipc::FullPtr<Task>(task_shm); //construct FullPtr with task.shm_ 

        // Affine to CPU 1
        hshm::ProcessAffiner::SetCpuAffinity(HSHM_SYSTEM_INFO->pid_, 1);
    }

    MPI_Barrier(MPI_COMM_WORLD);
    sub::ipc::mpsc_ptr_queue<int> *queue = queue_->get();

    //address dumping
    if (rank == RANK0) {
        // dump address range, append mode
        std::ofstream tag_file("/tmp/ipc_tags.txt", std::ios::app);

        if (tag_file.is_open()) {
            // Dump full address range of queue (2MB region with 64B stride) 
            uint64_t base = reinterpret_cast<uint64_t>(queue); 
            size_t size = 2 * 1024 * 1024; // Assumed 2MB, adjust if needed

            // IMPORTANT: Write ONLY hex addresses, one per line (no labels!)
            for (size_t offset = 0; offset < size; offset += 64) { 
                tag_file << std::hex << (base + offset) << std::endl; 
            } 

            // Dump the address of the completion flag 
            tag_file << std::hex << reinterpret_cast<uint64_t>(&task->done_) << std::endl; 

            tag_file.flush();
            if (tag_file.fail()) {
                std::cerr << "[Rank 0 FAILURE] WRITE ERROR after flush on /tmp/ipc_tags.txt." << std::endl;
            }
            tag_file.close(); 
            
            if (tag_file.fail()) {
                // Check for errors during close/flush
                std::cerr << "[Rank 0 FAILURE] CLOSE/FLUSH ERROR on /tmp/ipc_tags.txt. errno: " << errno << std::endl;
            } else {
                //Print to console for debugging (with labels)
                std::cout << "[Rank 0] Dumped " << (size / 64) << " IPC addresses to /tmp/ipc_tags.txt" << std::endl;
                std::cout << "  Queue base: 0x" << std::hex << base << std::dec << std::endl;
                std::cout << "  Task flag: 0x" << std::hex << reinterpret_cast<uint64_t>(&task->done_) << std::dec << std::endl;
            }
        } else {
            // Check if the stream failed to open
            std::cerr << "[Rank 0 FAILURE] FAILED TO OPEN /tmp/ipc_tags.txt. " 
                    << "errno: " << errno << std::endl; 
        }

        std::cout << "STARTING MAIN LOOP ITERATIONS " << std::endl;
    }

    MPI_Barrier(MPI_COMM_WORLD);

    ROI_BEGIN();
    for (int i = 0; i < iters; i++) {
        if (rank == RANK0) {
            // Emplace in queue
            task->done_ = false;
            queue->emplace(1);
            while (!task->done_) {
                continue;
            }
        } else {
            // Pop entries from the queue
            int x;
            while (queue->pop(x).IsNull()) {
                continue;
            }
            task->done_ = true;
        }
    }
    ROI_END();

    // The barrier is necessary so that
    // Rank 0 doesn't exit before Rank 1
    // The uptr frees data when rank 0 exits.
    MPI_Barrier(MPI_COMM_WORLD);

    MainPosttest();
    MPI_Finalize();

    return 0;
}
