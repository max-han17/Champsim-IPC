#!/bin/bash
#
# Launch script for tracing MPI applications with 2 ranks
# 

RANK=$OMPI_COMM_WORLD_RANK

PIN_BIN="/path/to/intel-pin-tool"
TRACER_SO="/path/to/ChampSim_Simulator/champsim/tracer/obj-intel64/champsim_tracer.so"
TRACE_DIR="/path/to/ChampSim_Simulator/champsim/traces/"
APP="/path/to/traced/app"
#TRACE_INSTR=100000
TRACE_INSTR=$1
SKIP_INSTR=$2
ITERS=$3
# IN_FLIGHT=$4
# PRIV_WS_KB=$5
# CHASE_STEPS=$6

echo "[Rank $RANK] Starting trace with physical address translation..."

if [ "$RANK" -eq 0 ]; then
    exec $PIN_BIN -t $TRACER_SO -o $TRACE_DIR/trace_rank0_$TRACE_INSTR.champsim -s $SKIP_INSTR -t $TRACE_INSTR -- $APP $ITERS $IN_FLIGHT $PRIV_WS_KB $CHASE_STEPS
else
    exec $PIN_BIN -t $TRACER_SO -o $TRACE_DIR/trace_rank1_$TRACE_INSTR.champsim -s $SKIP_INSTR -t $TRACE_INSTR -- $APP $ITERS $IN_FLIGHT $PRIV_WS_KB $CHASE_STEPS
fi
