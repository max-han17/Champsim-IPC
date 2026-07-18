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

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <thread>

#include <hermes_shm/data_structures/ipc/ring_ptr_queue.h>
#include <hermes_shm/data_structures/ipc/string.h>
#include <hermes_shm/util/affinity.h>
#include <hermes_shm/util/error.h>
#include <hermes_shm/util/timer.h>
#include "test_init.h"

#define ITERS_DEFAULT 1000000
#define ROI_BEGIN() __asm__ __volatile__ ("xchg %%bx, %%bx" ::: "bx")
#define ROI_END()   __asm__ __volatile__ ("xchg %%cx, %%cx" ::: "cx")

struct Task {
  char bytes_[128];
  hipc::atomic<bool> complete_;

  Task() : complete_(false) {
    for (size_t i = 0; i < sizeof(bytes_); ++i) {
      bytes_[i] = 0;
    }
  }

  inline void ResetComplete() {
    complete_.store(false, std::memory_order_release);
  }

  inline void SetComplete() {
    complete_.store(true, std::memory_order_release);
  }

  inline bool IsComplete() const {
    return complete_.load(std::memory_order_acquire);
  }
};

struct Future {
  hipc::Pointer task_shm_;
  char other_future_stuff_[32];

  Future() : task_shm_() {
    for (size_t i = 0; i < sizeof(other_future_stuff_); ++i) {
      other_future_stuff_[i] = 0;
    }
  }

  inline void SetComplete() {
    hshm::ipc::FullPtr<Task> task(task_shm_);
    task->SetComplete();
  }

  inline bool IsComplete() const {
    hshm::ipc::FullPtr<Task> task(task_shm_);
    return task->IsComplete();
  }
};

int main(int argc, char **argv) {
  MPI_Init(&argc, &argv);

  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  int iters = ITERS_DEFAULT;
  if (argc > 1) {
    iters = std::atoi(argv[1]);
  }

  MainPretest();
  std::cout << "CHECKPOINT:  INITIALIZED" << std::endl;

  auto *alloc =
      HSHM_MEMORY_MANAGER->GetAllocator<HSHM_DEFAULT_ALLOC_T>(AllocatorId(1, 0));
  if (!alloc) {
    std::cerr << "alloc is null" << std::endl;
    MPI_Finalize();
    return -1;
  }

  auto *queue_ =
      alloc->GetCustomHeader<hipc::delay_ar<sub::ipc::mpsc_ptr_queue<hipc::Pointer>>>();
  if (!queue_) {
    std::cerr << "QUEUE is null!" << std::endl;
    MPI_Finalize();
    return -1;
  }

  hshm::ipc::FullPtr<Task> task;
  hshm::ipc::FullPtr<Future> future;

  if (rank == RANK0) {
    queue_->shm_init(alloc, 100000);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    task = alloc->AllocateLocalPtr<Task>(HSHM_MCTX, sizeof(Task));
    future = alloc->AllocateLocalPtr<Future>(HSHM_MCTX, sizeof(Future));
    future->task_shm_ = task.shm_;
    task->ResetComplete();

    hipc::Pointer task_shm = task.shm_;
    hipc::Pointer future_shm = future.shm_;
    MPI_Send(&task_shm, sizeof(task_shm), MPI_BYTE, 1, 0, MPI_COMM_WORLD);
    MPI_Send(&future_shm, sizeof(future_shm), MPI_BYTE, 1, 1, MPI_COMM_WORLD);

    hshm::ProcessAffiner::SetCpuAffinity(HSHM_SYSTEM_INFO->pid_, 0);
  }

  MPI_Barrier(MPI_COMM_WORLD);
  std::atomic_thread_fence(std::memory_order_seq_cst);

  if (rank != RANK0) {
    hipc::Pointer task_shm;
    hipc::Pointer future_shm;
    MPI_Recv(&task_shm, sizeof(task_shm), MPI_BYTE, 0, 0, MPI_COMM_WORLD,
             MPI_STATUS_IGNORE);
    MPI_Recv(&future_shm, sizeof(future_shm), MPI_BYTE, 0, 1, MPI_COMM_WORLD,
             MPI_STATUS_IGNORE);

    task = hshm::ipc::FullPtr<Task>(task_shm);
    future = hshm::ipc::FullPtr<Future>(future_shm);

    hshm::ProcessAffiner::SetCpuAffinity(HSHM_SYSTEM_INFO->pid_, 1);
  }

  MPI_Barrier(MPI_COMM_WORLD);
  sub::ipc::mpsc_ptr_queue<hipc::Pointer> *queue = queue_->get();

  // address dumping
  if (rank == RANK0) {
    std::ofstream tag_file("/tmp/ipc_tags.txt", std::ios::app);

    if (tag_file.is_open()) {
      uint64_t base = reinterpret_cast<uint64_t>(queue);
      size_t size = 2 * 1024 * 1024;

      // IMPORTANT: Write ONLY hex addresses, one per line (no labels!)
      for (size_t offset = 0; offset < size; offset += 64) {
        tag_file << std::hex << (base + offset) << std::endl;
      }

      tag_file << std::hex << reinterpret_cast<uint64_t>(&task->complete_) << std::endl;

      tag_file.flush();
      if (tag_file.fail()) {
        std::cerr << "[Rank 0 FAILURE] WRITE ERROR after flush on /tmp/ipc_tags.txt."
                  << std::endl;
      }
      tag_file.close();

      if (tag_file.fail()) {
        std::cerr << "[Rank 0 FAILURE] CLOSE/FLUSH ERROR on /tmp/ipc_tags.txt. errno: "
                  << errno << std::endl;
      } else {
        std::cout << "[Rank 0] Dumped " << (size / 64)
                  << " IPC addresses to /tmp/ipc_tags.txt" << std::endl;
        std::cout << "  Queue base: 0x" << std::hex << base << std::dec << std::endl;
        std::cout << "  Task flag: 0x" << std::hex
                  << reinterpret_cast<uint64_t>(&task->complete_) << std::dec
                  << std::endl;
      }
    } else {
      std::cerr << "[Rank 0 FAILURE] FAILED TO OPEN /tmp/ipc_tags.txt. "
                << "errno: " << errno << std::endl;
    }

    std::cout << "STARTING MAIN LOOP ITERATIONS " << std::endl;
  }

  MPI_Barrier(MPI_COMM_WORLD);

  ROI_BEGIN();
  for (int i = 0; i < iters; ++i) {
    if (rank == RANK0) {
      task->ResetComplete();
      queue->emplace(future.shm_);
      while (!task->IsComplete()) {
        continue;
      }
    } else {
      hipc::Pointer future_shm;
      while (queue->pop(future_shm).IsNull()) {
        continue;
      }
      hshm::ipc::FullPtr<Future> client_future(future_shm);
      client_future->other_future_stuff_[0] = static_cast<char>(i & 0x7F);
      client_future->SetComplete();
    }
  }
  ROI_END();

  MPI_Barrier(MPI_COMM_WORLD);
  MainPosttest();
  MPI_Finalize();
  return 0;
}