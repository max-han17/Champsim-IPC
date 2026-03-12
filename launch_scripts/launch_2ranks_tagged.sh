#!/bin/bash

RANK=$OMPI_COMM_WORLD_RANK

PIN_BIN="/home/max/pin-external-3.31-98869-gfa6f126a8-gcc-linux/pin"
TRACER_SO="/home/max/ChampSim_Simulator/champsim/tracer/obj-intel64/MyPinTool.so"
TRACE_DIR="/home/max/ChampSim_Simulator/champsim/traces/translated"
APP="/home/max/Desktop/grc/mpi_tests/mpsc_queue/build/mpsc_queue_tagged"
#TRACE_INSTR=100000
TRACE_INSTR=$1

#echo "[Rank $RANK] Starting trace with integrated physical address translation..."
export PIN_LD_LIBRARY_PATH=
export PIN_INJECT_TIMEOUT=1000

PIN_LOG_FILE="$TRACE_DIR/pin_log_rank${RANK}.txt"

PIN_ARGS="-t $TRACER_SO -o $TRACE_DIR/translate_rank${RANK}_$TRACE_INSTR.phys.champsim -s 0 -t $TRACE_INSTR -logfile $PIN_LOG_FILE"

if [ "$RANK" -eq 0 ]; then
    exec $PIN_BIN $PIN_ARGS -- $APP >> /tmp/ipc_tags.txt 2> $PIN_LOG_FILE
else
    exec $PIN_BIN $PIN_ARGS -- $APP >> /tmp/ipc_tags.txt 2> $PIN_LOG_FILE
fi
