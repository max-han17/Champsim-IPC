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

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <hermes_shm/data_structures/ipc/ring_ptr_queue.h>
#include <hermes_shm/util/affinity.h>
#include "test_init.h"

#define ITERS_DEFAULT 1000000
#define ROI_BEGIN() __asm__ __volatile__("xchg %%bx, %%bx" ::: "bx")
#define ROI_END() __asm__ __volatile__("xchg %%cx, %%cx" ::: "cx")
#define FUTURE_POOL_SIZE 64

using SharedQueue = sub::ipc::mpsc_ptr_queue<hipc::Pointer>;

class Task {
 public:
  char bytes[128];
  std::atomic<bool> complete;

 public:
  Task() : complete(false) {
    std::memset(bytes, 0, sizeof(bytes));
  }

  inline void ResetComplete() {
    complete.store(false, std::memory_order_release);
  }

  inline void SetComplete() {
    complete.store(true, std::memory_order_release);
  }

  inline bool IsComplete() const {
    return complete.load(std::memory_order_acquire);
  }
};

class Future {
 public:
  hshm::ipc::FullPtr<Task> task;
  char other_future_stuff[32];

 public:
  Future() {
    std::memset(other_future_stuff, 0, sizeof(other_future_stuff));
  }

  inline void SetComplete() {
    task->SetComplete();
  }

  inline bool IsComplete() const {
    return task->IsComplete();
  }
};

static_assert(offsetof(Task, complete) >= 128,
              "Task::complete should follow the 128-byte payload");
static_assert(sizeof(Task) >= 129,
              "Task should contain 128 bytes plus completion flag");

int main(int argc, char **argv) {
  MPI_Init(&argc, &argv);

  int rank = 0;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  int iters = ITERS_DEFAULT;
  int pool_size = FUTURE_POOL_SIZE;
  if (argc > 1) {
    iters = std::atoi(argv[1]);
  }
  if (argc > 2) {
    pool_size = std::max(1, std::atoi(argv[2]));
  }

  MainPretest();

  auto *alloc =
      HSHM_MEMORY_MANAGER->GetAllocator<HSHM_DEFAULT_ALLOC_T>(AllocatorId(1, 0));
  if (!alloc) {
    std::cerr << "alloc is null" << std::endl;
    MPI_Finalize();
    return -1;
  }

  auto *queue_ =
      alloc->GetCustomHeader<hipc::delay_ar<SharedQueue>>();
  if (!queue_) {
    std::cerr << "QUEUE is null!" << std::endl;
    MPI_Finalize();
    return -1;
  }

  if (rank == RANK0) {
    queue_->shm_init(alloc, 100000);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    hshm::ProcessAffiner::SetCpuAffinity(HSHM_SYSTEM_INFO->pid_, 0);
  }

  MPI_Barrier(MPI_COMM_WORLD);

  if (rank != RANK0) {
    hshm::ProcessAffiner::SetCpuAffinity(HSHM_SYSTEM_INFO->pid_, 1);
  }

  MPI_Barrier(MPI_COMM_WORLD);
  SharedQueue *queue = queue_->get();
  MPI_Barrier(MPI_COMM_WORLD);

  std::vector<hshm::ipc::FullPtr<Task>> task_pool;
  std::vector<hshm::ipc::FullPtr<Future>> future_pool;

  if (rank == RANK0) {
    task_pool.reserve(pool_size);
    future_pool.reserve(pool_size);
    for (int i = 0; i < pool_size; ++i) {
      auto task = alloc->AllocateLocalPtr<Task>(HSHM_MCTX, sizeof(Task));
      auto future = alloc->AllocateLocalPtr<Future>(HSHM_MCTX, sizeof(Future));
      future->task = task;
      task->ResetComplete();
      task_pool.emplace_back(task);
      future_pool.emplace_back(future);
    }
  }

  MPI_Barrier(MPI_COMM_WORLD);

  ROI_BEGIN();
  for (int i = 0; i < iters; ++i) {
    if (rank == RANK0) {
      const int slot = i % pool_size;
      auto &task = task_pool[slot];
      auto &future = future_pool[slot];
      task->ResetComplete();
      task->bytes[0] = static_cast<char>(i & 0x7F);
      queue->emplace(future.shm_);

      while (!future->IsComplete()) {
        std::this_thread::yield();
      }
    } else {
      hipc::Pointer future_shm;
      while (queue->pop(future_shm).IsNull()) {
        std::this_thread::yield();
      }
      hshm::ipc::FullPtr<Future> future(future_shm);
      future->other_future_stuff[0] = static_cast<char>(rank);
      future->SetComplete();
    }
  }
  ROI_END();

  MPI_Barrier(MPI_COMM_WORLD);
  MainPosttest();
  MainPosttest();
  MPI_Finalize();
  return 0;
}
