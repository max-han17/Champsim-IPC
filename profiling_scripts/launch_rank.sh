#!/bin/bash
RANK=$OMPI_COMM_WORLD_RANK

if [ "$RANK" -eq 0 ]; then
exec /home/max/pin-external-3.31-98869-gfa6f126a8-gcc-linux/pin -t /home/max/ChampSim_Simulator/champsim/tracer/obj-intel64/champsim_tracer.so -o /home/max/ChampSim_Simulator/champsim/traces/trace_rank0.champsim -s 0 -t 200000000 -- /home/max/Desktop/grc/mpi_tests/mpsc_queue/build/mpsc_queue_final
else
exec /home/max/Desktop/grc/mpi_tests/mpsc_queue/build/mpsc_queue_final
fi
