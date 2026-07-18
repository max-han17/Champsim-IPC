#include <mpi.h>
#include <hermes_shm/util/affinity.h>
#include <hermes_shm/util/timer.h>
#include "test_init.h"

#define ITERS 100000

struct Task {
    hipc::atomic<bool> done_;
};

int main(int argc, char** argv) { 
    MPI_Init(&argc, &argv);

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    int iters = ITERS;
    if (argc > 1) {
        iters = std::atoi(argv[1]);
    }

    MainPretest();  // Set up shared memory + allocator

    auto *alloc = HSHM_DEFAULT_ALLOC;
    hshm::ipc::FullPtr<Task> task;

    // Allocate and share the Task struct
    if (rank == 0) {

        task = alloc->AllocateLocalPtr<Task>(HSHM_MCTX, sizeof(Task)); //local ptr allocation
        hipc::Pointer task_shm = task.shm_;
        MPI_Send(&task_shm, sizeof(task_shm), MPI_BYTE, 1, 0, MPI_COMM_WORLD); //send task.shm_ to other process


        hshm::ProcessAffiner::SetCpuAffinity(HSHM_SYSTEM_INFO->pid_, 0);
    } else {

        hipc::Pointer task_shm;
        MPI_Recv(&task_shm, sizeof(task_shm), MPI_BYTE, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE); //recv task.shm_ from other process

        task = hshm::ipc::FullPtr<Task>(task_shm); //construct FullPtr with task.shm_ 

        hshm::ProcessAffiner::SetCpuAffinity(HSHM_SYSTEM_INFO->pid_, 1);
    }

    std::cout << "[RANK " << rank << "] task.shm_.off_  = " << task.shm_.off_.load() << "\n";
    MPI_Barrier(MPI_COMM_WORLD);  // Synchronize
    hshm::Timer timer;
    timer.Resume();

    for (int i = 0; i < iters; ++i) {
        if (rank == 0) {
            task->done_ = false;
            while (!task->done_) {
                continue;
            }
        } else {
            while (task->done_) {
                continue;
            }
            task->done_ = true;
        }
        // printf("loop iter : %d\n", i);
    }

    double total_time = timer.Pause();
    // Ensure Rank 0 doesn't exit before Rank 1
    MPI_Barrier(MPI_COMM_WORLD);
    
    if (rank == RANK0) {
        float avg_time_ns = (1.0 * total_time) / (1.0 * iters);
        printf("Number of iterations: %d\n", iters);
        printf("Average RTT: %.10f ns (%.10f us)\n", avg_time_ns, avg_time_ns / 1000.0);
    }
    MainPosttest();
    MPI_Finalize();
    return 0;
}
