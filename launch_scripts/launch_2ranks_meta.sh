#!/bin/bash
#
# Launch script for tracing MPI applications with 2 ranks
# 
# The tracer now includes integrated VA->PA translation, so traces
# will contain physical addresses directly. No external translator needed.
# Traces can be merged using champsim/traces/merge_trace.py based on timestamps.
#

RANK=$OMPI_COMM_WORLD_RANK

PIN_BIN="/home/max/pin-external-3.31-98869-gfa6f126a8-gcc-linux/pin"
TRACER_SO="/home/max/ChampSim_Simulator/champsim/tracer/obj-intel64/MyPinTool.so"
TRACE_DIR="/home/max/ChampSim_Simulator/champsim/traces/translated"
APP="/home/max/Desktop/grc/mpi_tests/mpsc_queue/build/mpsc_queue_t"
#TRACE_INSTR=100000
TRACE_INSTR=$1
SKIP_INSTR=$2

echo "[Rank $RANK] Starting trace with integrated physical address translation..."

if [ "$RANK" -eq 0 ]; then
    exec $PIN_BIN -t $TRACER_SO -o $TRACE_DIR/trace_rank0_$TRACE_INSTR.champsim -s $SKIP_INSTR -t $TRACE_INSTR -- $APP
else
    exec $PIN_BIN -t $TRACER_SO -o $TRACE_DIR/trace_rank1_$TRACE_INSTR.champsim -s $SKIP_INSTR -t $TRACE_INSTR -- $APP
fi
