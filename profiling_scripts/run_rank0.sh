#!/bin/bash

# === CONFIGURATION ===
PIN_PATH="/home/max/pin-external-3.31-98869-gfa6f126a8-gcc-linux/pin"
TRACER_SO="./tracer/obj-intel64/champsim_tracer.so"
TRACE_OUT="./traces/mpsc_queue_final_1M.champsim"
TARGET_BINARY="/home/max/Desktop/grc/mpi_tests/mpsc_queue/build/mpsc_queue_final"
TRACE_SKIP=0
TRACE_LENGTH=1000000

# === MPI WRAPPER ===
mpirun -np 2 bash -c '
  if [ "$OMPI_COMM_WORLD_RANK" -eq 0 ]; then
    echo "[Rank 0] Running using Pin..."
    '"$PIN_PATH"' -t '"$TRACER_SO"' -o '"$TRACE_OUT"' -s '"$TRACE_SKIP"' -t '"$TRACE_LENGTH"' -- '"$TARGET_BINARY"'
  else
    echo "[Rank 1] Running natively..."
    '"$TARGET_BINARY"'
  fi
'
