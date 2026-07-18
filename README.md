# ChampSim (Modified IPC-Aware Prefetching and Replacement Policy Evaluation)

This repository is a modified version of ChampSim for microarchitecture
research with MPI applications and IPC-aware cache replacement analysis.
(Forked off of https://github.com/Xiaoyang-Lu/ChampSim_Simulator/tree/main)

This fork adds:

- MPI tracing support for 2 ranks
- dual trace output during tracing (physical and virtual memory addresses)
- IPC instruction tagging
- timestamp-based trace merge support
- replacement policy configurability at both `L2C` and `LLC`
- IPC-aware replacement policies and IPC-tag-specific statistics

## Prerequisites

- Intel Pin (3.x series used in this project)
- OpenMPI (5.0.5)
- Python3 (3.12.3 used in this project)

Tracing is expected to be launched with elevated privileges as:

```bash
sudo mpirun --allow-run-as-root ...
```

Before running, update local paths in:

- `launch_champsim_tracer_2ranks.sh`
- `tracer/make_tracer.sh`

Specifically, set paths for your Pin installation, tracer shared object,
trace output directory, and target MPI application.

## Build ChampSim
This fork uses a modified build interface:

```bash
./build_champsim.sh [l2c_pref] [l2c_repl] [llc_repl]
```

Where:

- `l2c_pref` is required (e.g., `no`, `next_line`, `ip_stride`)
- `l2c_repl` is optional (default: `lru`)
- `llc_repl` is optional (default: `ship`)

Examples:

```bash
# Baseline style (no prefetch, LRU in L2C, SHiP in LLC)
./build_champsim.sh no lru ship

# IPC-aware replacement at both cache levels
./build_champsim.sh no ipc ipc
```

Typical binary naming pattern produced by this script is:

```text
bin/<branch>-<l1d_pref>-<l2c_pref>-<l2c_repl>-<llc_pref>-<llc_repl>-<num_core>core
```

## Build the Tracer

Build the Pin tracer shared object:

```bash
cd tracer
./make_tracer.sh
```

## MPI Application IPC-aware Trace Workflow (2 Ranks)

### 1) Generate traces from MPI application

Some simple example applications can be found under ```mpsc_queue/```. 
Run the launcher with exactly 2 ranks (ensure there is sufficient disk space as uncompressed long traces can be tens of gigabytes):

```bash
sudo mpirun --allow-run-as-root -np 2 \
  ./launch_champsim_tracer_2ranks.sh <trace_instr> <skip_instr> <iters>
```

Expected outputs (per rank):

- physical trace: `trace_rankX_<N>.champsim`
- virtual trace: `trace_rankX_<N>.champsim.virtual`
- metadata: `trace_rankX_<N>.champsim.meta`

where `X` is rank (`0` or `1`) and `<N>` is `trace_instr`.

### 2) Tag IPC-related instructions

Use the rank's physical trace, virtual trace, and IPC address list:

```bash
python3 tag_ipc.py <phys_trace> <virt_trace> <ipc_tags_file>
```

Example for rank 0:

```bash
python3 tag_ipc.py \
  ./traces/trace_rank0_10000000.champsim \
  ./traces/trace_rank0_10000000.champsim.virtual \
  ./traces/ipc_tags_rank0.txt
```

Example for rank 1:

```bash
python3 tag_ipc.py \
  ./traces/trace_rank1_10000000.champsim \
  ./traces/trace_rank1_10000000.champsim.virtual \
  ./traces/ipc_tags_rank1.txt
```

Outputs:

- `<phys_trace>.tagged`
- `<virt_trace>.tagged`

### 3) Merge rank traces by timestamp

Merge two rank traces using their metadata:

```bash
python3 merge_trace.py <trace0> <meta0> <trace1> <meta1> <out_trace> <out_meta>
```

Example:

```bash
python3 merge_trace.py \
  ./traces/trace_rank0_10000000.champsim.tagged \
  ./traces/trace_rank0_10000000.champsim.meta \
  ./traces/trace_rank1_10000000.champsim.tagged \
  ./traces/trace_rank1_10000000.champsim.meta \
  ./traces/merged_2ranks.champsim \
  ./traces/merged_2ranks.meta
```


Notes:

- Current merge script usage is for 2 ranks (`trace0/meta0` and `trace1/meta1`).
- Metadata preserves rank and timestamp information used for ordering.

### 4) Compress merged traces before simulating

ChampSim expects compressed traces (`.xz` or `.gz`) for simulation input.
If your merged trace is raw (`.champsim`), compress it:

```bash
xz -T0 ./traces/merged_2ranks.champsim
```

## Run Simulation

This project runs ChampSim binaries directly (not via `run_champsim.sh`).


Example using merged MPI trace:

```bash
./bin/perceptron-no-no-no-ipc-no-ipc-1core \
  -warmup_instructions 5000 \
  -simulation_instructions 9000 \
  -traces ./traces/merged_2ranks.champsim.xz
```

## Metrics Outputted

Some metrics that the simulator outputs:

- overall per-core instructions per cycle (`Core_*_IPC`)
- per-cache IPC-tag stats:
  - `*_ipc_tag_0_access/hit/miss`
  - `*_ipc_tag_1_access/hit/miss`
- IPC eviction behavior:
  - `*_ipc_evictions`
  - `*_prot_ipc_evictions`
  - `*_ipc_policy_same_as_lru`
  - `*_ipc_policy_diff_from_lru`
- LLC reuse and ghost-hit counters split by IPC tag

These are useful to understand how IPC-aware replacement policy changes cache residency and
replacement outcomes relative to baseline policies.
