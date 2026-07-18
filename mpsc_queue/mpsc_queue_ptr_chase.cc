/*
 * Mixed IPC + pointer-chasing service-state benchmark.
 *
 * Goal:
 *  - keep the original queue-based IPC skeleton
 *  - make IPC path dominant but add bounded non-IPC memory pressure
 *  - use pointer chasing instead of streaming array touches
 *
 * Usage:
 *   ./mpsc_queue_ptr_chase [iters] [inflight] [private_ws_kb] [chase_steps]
 *
 * Suggested runs:
 *   ./mpsc_queue_ptr_chase 100000 1   0    0
 *   ./mpsc_queue_ptr_chase 100000 64  1024 4
 *   ./mpsc_queue_ptr_chase 100000 64  4096 8
 *   ./mpsc_queue_ptr_chase 100000 128 4096 16
 */

#include <mpi.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

#include <hermes_shm/data_structures/ipc/ring_ptr_queue.h>
#include <hermes_shm/data_structures/ipc/string.h>
#include <hermes_shm/util/affinity.h>
#include <hermes_shm/util/error.h>
#include <hermes_shm/util/timer.h>
#include "test_init.h"

#define ITERS_DEFAULT 1000000
#define INFLIGHT_DEFAULT 64
#define PRIVATE_WS_KB_DEFAULT 1024
#define CHASE_STEPS_DEFAULT 4
#define QUEUE_DEPTH_MULTIPLIER 4
#define ROI_BEGIN() __asm__ __volatile__("xchg %%bx, %%bx" ::: "bx")
#define ROI_END() __asm__ __volatile__("xchg %%cx, %%cx" ::: "cx")

// struct Task {
//   char bytes_[128];
//   hipc::atomic<bool> complete_;

struct Task {
  char bytes_[1024];
  hipc::atomic<bool> complete_;

  Task() : complete_(false) {
    for (size_t i = 0; i < sizeof(bytes_); ++i) bytes_[i] = 0;
  }
  inline void ResetComplete() { complete_.store(false, std::memory_order_release); }
  inline void SetComplete() { complete_.store(true, std::memory_order_release); }
  inline bool IsComplete() const { return complete_.load(std::memory_order_acquire); }
};

// struct Future {
//   hipc::Pointer task_shm_;
//   char other_future_stuff_[32];

struct Future {
  hipc::Pointer task_shm_;
  char other_future_stuff_[512];

  Future() : task_shm_() {
    for (size_t i = 0; i < sizeof(other_future_stuff_); ++i) other_future_stuff_[i] = 0;
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

struct ChaseNode {
  uint32_t next;
  uint32_t pad;
  uint64_t value;
  char padding[48];
};
static_assert(sizeof(ChaseNode) == 64, "ChaseNode should be one cache line");

static volatile uint64_t g_sink = 0;

static inline uint64_t Mix64(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

static void InitPointerChase(std::vector<ChaseNode> &nodes, uint64_t seed) {
  const size_t n = nodes.size();
  if (n == 0) return;

  std::vector<uint32_t> perm(n);
  for (size_t i = 0; i < n; ++i) perm[i] = static_cast<uint32_t>(i);

  uint64_t state = seed;
  for (size_t i = n - 1; i > 0; --i) {
    state = Mix64(state + i);
    const size_t j = state % (i + 1);
    std::swap(perm[i], perm[j]);
  }

  for (size_t i = 0; i < n; ++i) {
    const uint32_t cur = perm[i];
    const uint32_t nxt = perm[(i + 1) % n];
    nodes[cur].next = nxt;
    nodes[cur].pad = 0;
    nodes[cur].value = Mix64(seed ^ cur);
  }
}

static inline void PointerChaseWork(std::vector<ChaseNode> &nodes,
                                    uint32_t &cursor,
                                    int steps,
                                    uint64_t salt) {
  if (nodes.empty() || steps <= 0) return;
  uint64_t acc = salt;
  for (int s = 0; s < steps; ++s) {
    ChaseNode &node = nodes[cursor];
    acc ^= node.value;
    node.value = Mix64(node.value + acc + static_cast<uint64_t>(s));
    cursor = node.next;
  }
  g_sink ^= acc;
}

static inline void DumpTaggedRange(std::ofstream &tag_file, uint64_t base, size_t bytes) {
  const size_t aligned_bytes = ((bytes + 63) / 64) * 64;
  for (size_t offset = 0; offset < aligned_bytes; offset += 64)
    tag_file << std::hex << (base + offset) << '\n';
}

static inline int PosInt(char **argv, int idx, int def) {
  if (argv[idx] == nullptr) return def;
  return std::max(1, std::atoi(argv[idx]));
}
static inline int NonNegInt(char **argv, int idx, int def) {
  if (argv[idx] == nullptr) return def;
  return std::max(0, std::atoi(argv[idx]));
}

int main(int argc, char **argv) {
  MPI_Init(&argc, &argv);

  int rank = 0, world_size = 0;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);
  if (world_size != 2) {
    if (rank == 0) std::cerr << "This benchmark expects exactly 2 ranks.\n";
    MPI_Finalize();
    return -1;
  }

  const int iters = (argc > 1) ? PosInt(argv, 1, ITERS_DEFAULT) : ITERS_DEFAULT;
  const int inflight = (argc > 2) ? PosInt(argv, 2, INFLIGHT_DEFAULT) : INFLIGHT_DEFAULT;
  const int private_ws_kb = (argc > 3) ? NonNegInt(argv, 3, PRIVATE_WS_KB_DEFAULT) : PRIVATE_WS_KB_DEFAULT;
  const int chase_steps = (argc > 4) ? NonNegInt(argv, 4, CHASE_STEPS_DEFAULT) : CHASE_STEPS_DEFAULT;
  const size_t queue_depth = std::max<size_t>(static_cast<size_t>(inflight) * QUEUE_DEPTH_MULTIPLIER, 1024);

  MainPretest();
  if (rank == 0) {
    std::cout << "CHECKPOINT: INITIALIZED\n";
    std::cout << "iters=" << iters << " inflight=" << inflight
              << " private_ws_kb=" << private_ws_kb
              << " chase_steps=" << chase_steps
              << " queue_depth=" << queue_depth << std::endl;
  }

  auto *alloc = HSHM_MEMORY_MANAGER->GetAllocator<HSHM_DEFAULT_ALLOC_T>(AllocatorId(1, 0));
  if (!alloc) { std::cerr << "alloc is null\n"; MPI_Finalize(); return -1; }

  auto *queue_ = alloc->GetCustomHeader<hipc::delay_ar<sub::ipc::mpsc_ptr_queue<hipc::Pointer>>>();
  if (!queue_) { std::cerr << "QUEUE is null!\n"; MPI_Finalize(); return -1; }

  const size_t node_count = (private_ws_kb > 0)
      ? std::max<size_t>(1, (static_cast<size_t>(private_ws_kb) * 1024) / sizeof(ChaseNode))
      : 0;
  std::vector<ChaseNode> service_state(node_count);
  InitPointerChase(service_state, 0x123456789abcdef0ULL ^ static_cast<uint64_t>(rank));
  uint32_t chase_cursor = service_state.empty() ? 0 : static_cast<uint32_t>(rank % service_state.size());

  std::vector<hshm::ipc::FullPtr<Task>> tasks;
  std::vector<hshm::ipc::FullPtr<Future>> futures;
  std::vector<hipc::Pointer> future_shms(inflight);

  if (rank == RANK0) {
    std::cout << "RANK0 init" << std::endl;
    queue_->shm_init(alloc, queue_depth);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    tasks.resize(inflight);
    futures.resize(inflight);
    for (int slot = 0; slot < inflight; ++slot) {
      tasks[slot] = alloc->AllocateLocalPtr<Task>(HSHM_MCTX, sizeof(Task));
      futures[slot] = alloc->AllocateLocalPtr<Future>(HSHM_MCTX, sizeof(Future));
      futures[slot]->task_shm_ = tasks[slot].shm_;
      tasks[slot]->ResetComplete();
      future_shms[slot] = futures[slot].shm_;
    }
    std::cout << "RANK0 send init" << std::endl;
    MPI_Send(future_shms.data(), static_cast<int>(future_shms.size() * sizeof(hipc::Pointer)),
             MPI_BYTE, 1, 0, MPI_COMM_WORLD);
    std::cout << "RANK0 finish send init" << std::endl;
    hshm::ProcessAffiner::SetCpuAffinity(HSHM_SYSTEM_INFO->pid_, 0);
  }
  std::cout << "EXIT RANK0 send init" << std::endl;
  // MPI_Barrier(MPI_COMM_WORLD);
  // std::atomic_thread_fence(std::memory_order_seq_cst);

  if (rank != RANK0) {
    std::cout << "RANK1 init" << std::endl;
    MPI_Recv(future_shms.data(), static_cast<int>(future_shms.size() * sizeof(hipc::Pointer)),
             MPI_BYTE, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    hshm::ProcessAffiner::SetCpuAffinity(HSHM_SYSTEM_INFO->pid_, 1);
  }

  MPI_Barrier(MPI_COMM_WORLD);
  std::atomic_thread_fence(std::memory_order_seq_cst);

  MPI_Barrier(MPI_COMM_WORLD);
  sub::ipc::mpsc_ptr_queue<hipc::Pointer> *queue = queue_->get();

  if (rank == RANK0) {
    std::cout << "RANK0 address dumping" << std::endl;
    std::ofstream tag_file("/tmp/ipc_tags.txt", std::ios::trunc);
    if (tag_file.is_open()) {
      const uint64_t queue_base = reinterpret_cast<uint64_t>(queue);
      // const size_t queue_tag_bytes = queue_depth * sizeof(hipc::Pointer) + 4096;
      const size_t queue_tag_bytes =
      std::max<size_t>(queue_depth * sizeof(hipc::Pointer) + 4096,
                     static_cast<size_t>(2 * 1024 * 1024));

      DumpTaggedRange(tag_file, queue_base, queue_tag_bytes);
      for (int slot = 0; slot < inflight; ++slot) {
        const uint64_t task_base = reinterpret_cast<uint64_t>(&tasks[slot]->bytes_[0]) - offsetof(Task, bytes_);
        const uint64_t future_base = reinterpret_cast<uint64_t>(&futures[slot]->task_shm_) - offsetof(Future, task_shm_);
        DumpTaggedRange(tag_file, task_base, sizeof(Task));
        DumpTaggedRange(tag_file, future_base, sizeof(Future));
      }
      tag_file.flush();
      if (tag_file.fail()) std::cerr << "[Rank 0 FAILURE] write error on /tmp/ipc_tags.txt\n";
      else std::cout << "[Rank 0] Dumped IPC-tagged queue/task/future addresses to /tmp/ipc_tags.txt\n";
      tag_file.close();
    } else {
      std::cerr << "[Rank 0 FAILURE] failed to open /tmp/ipc_tags.txt, errno=" << errno << "\n";
    }
    std::cout << "STARTING MAIN LOOP ITERATIONS\n";
  }

  MPI_Barrier(MPI_COMM_WORLD);

  ROI_BEGIN();
  if (rank == RANK0) {
    int issued = 0, completed = 0;
    while (completed < iters) {
      while (issued < iters && (issued - completed) < inflight) {
        const int slot = issued % inflight;
        // tasks[slot]->bytes_[issued % static_cast<int>(sizeof(tasks[slot]->bytes_))] = static_cast<char>(issued & 0x7f);
        for (int k = 0; k < static_cast<int>(sizeof(tasks[slot]->bytes_)); k += 64) {
          tasks[slot]->bytes_[k] = static_cast<char>((issued + k) & 0x7f);
        }
        tasks[slot]->ResetComplete();
        PointerChaseWork(service_state, chase_cursor, std::max(0, chase_steps / 2),
                         static_cast<uint64_t>(issued) ^ 0xa5a5a5a5ULL);
        queue->emplace(future_shms[slot]);
        ++issued;
      }

      const int slot = completed % inflight;
      while (!tasks[slot]->IsComplete()) {
        PointerChaseWork(service_state, chase_cursor, (chase_steps > 0) ? 1 : 0,
                         static_cast<uint64_t>(completed) ^ 0xbeefULL);
      }
      ++completed;
    }
  } else {
    for (int processed = 0; processed < iters; ++processed) {
      hipc::Pointer future_shm;
      while (queue->pop(future_shm).IsNull()) {
        PointerChaseWork(service_state, chase_cursor, (chase_steps > 0) ? 1 : 0,
                         static_cast<uint64_t>(processed) ^ 0xdeadULL);
      }
      hshm::ipc::FullPtr<Future> client_future(future_shm);
      PointerChaseWork(service_state, chase_cursor, chase_steps,
                       static_cast<uint64_t>(processed) ^ 0x1234ULL);
      // client_future->other_future_stuff_[processed & 31] =
      //     static_cast<char>((g_sink ^ static_cast<uint64_t>(processed)) & 0x7f);
      for (int k = 0; k < static_cast<int>(sizeof(client_future->other_future_stuff_)); k += 64) {
        client_future->other_future_stuff_[k] =
        static_cast<char>((processed + k) & 0x7f);
      }
      client_future->SetComplete();
    }
  }
  ROI_END();

  MPI_Barrier(MPI_COMM_WORLD);
  if (rank == 0) std::cout << "FINAL_SINK=" << std::hex << g_sink << std::dec << std::endl;
  MainPosttest();
  MPI_Finalize();
  return 0;
}