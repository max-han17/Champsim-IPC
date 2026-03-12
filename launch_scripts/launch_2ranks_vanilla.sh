#!/bin/bash

RANK=$OMPI_COMM_WORLD_RANK

PIN_BIN="/home/max/pin-external-3.31-98869-gfa6f126a8-gcc-linux/pin"
TRACER_SO="/home/max/ChampSim_Simulator/champsim/tracer/obj-intel64/champsim_tracer.so"
TRACE_DIR="/home/max/ChampSim_Simulator/champsim/traces"
APP="/home/max/Desktop/grc/mpi_tests/mpsc_queue/build/mpsc_queue_final"
TRACE_INSTR=100000

if [ "$RANK" -eq 0 ]; then
    exec $PIN_BIN -t $TRACER_SO -o $TRACE_DIR/vanilla_rank0.champsim -s 0 -t $TRACE_INSTR -- $APP
else
    exec $PIN_BIN -t $TRACER_SO -o $TRACE_DIR/vanilla_rank1.champsim -s 0 -t $TRACE_INSTR -- $APP
fi
