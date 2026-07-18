#!/usr/bin/env python3
"""
Merge trace files from multiple MPI ranks based on timestamps using a 
memory-efficient, stream-based approach (heapq.merge).

This script takes trace files and metadata files from multiple MPI ranks,
merges them by timestamp as they are read, and outputs a single merged 
trace file and metadata file without loading all data into RAM.

Usage: python merge_trace.py <trace0> <meta0> <trace1> <meta1> <out_trace> <out_meta>
"""

import struct
import sys
import os
import lzma
import heapq
from typing import List, Tuple, Iterator, Generator

# Struct formats for binary data
# TRACE_STRUCT: 72 bytes total (7 bytes padding at end)
# Format: QBB2B4B2Q4QB7x
# - Q: ip (8 bytes)
# - B: is_branch (1 byte)
# - B: branch_taken (1 byte)
# - 2B: destination_registers (2 bytes)
# - 4B: source_registers (4 bytes)
# - 2Q: destination_memory (16 bytes)
# - 4Q: source_memory (32 bytes)
# - B: ipc_tag (1 byte)
# - 7x: padding (7 bytes)
TRACE_STRUCT = struct.Struct("=QBB2B4B2Q4QB7x")# 72 bytes total (7 bytes padding at end)

# META_STRUCT: 16 bytes total (4 bytes padding)
# Format: i4xQ
# - i: mpi_rank (4 bytes)
# - 4x: padding (4 bytes)
# - Q: timestamp (8 bytes)
META_STRUCT = struct.Struct("=i4xQ") # 16 bytes total (4 bytes padding)

class TraceEntry:
    """Represents a single trace entry with instruction and metadata."""
    
    def __init__(self, instr_data: Tuple, meta_data: Tuple, rank: int):
        self.instr_data = instr_data
        self.meta_data = meta_data
        self.rank = rank
        self.timestamp = meta_data[1]# timestamp is the second field in meta_data
    
    # Required for heapq.merge, though we use the 'key' argument, 
    # defining a comparison method is good practice for the class.
    def __lt__(self, other):
        return self.timestamp < other.timestamp


def trace_entry_generator(trace_path: str, meta_path: str, rank: int) -> Generator[TraceEntry, None, None]:
    """
    Generator that yields TraceEntry objects one by one from the files. 
    This is the key to low-memory usage.
    """
    
    if not os.path.exists(trace_path):
        print(f"Warning: Trace file {trace_path} does not exist. Skipping rank {rank}.")
        return # Generator simply finishes

    if not os.path.exists(meta_path):
        print(f"Warning: Metadata file {meta_path} does not exist. Skipping rank {rank}.")
        return # Generator simply finishes
    
    entry_count = 0
    
    try:
        # Read uncompressed .champsim files (not .xz)
        with open(trace_path, "rb") as trace_file, open(meta_path, "rb") as meta_file:
            while True:
                # Read trace instruction
                instr_bytes = trace_file.read(TRACE_STRUCT.size)
                
                # Check for End-of-File (EOF) on trace
                if len(instr_bytes) != TRACE_STRUCT.size:
                    break
                
                # Read metadata
                meta_bytes = meta_file.read(META_STRUCT.size)
                
                # Check for EOF/mismatch on metadata
                if len(meta_bytes) != META_STRUCT.size:
                    print(f"Warning: Metadata file {meta_path} ended before trace file {trace_path} for rank {rank}.")
                    break
                
                # Unpack the data
                instr_data = TRACE_STRUCT.unpack(instr_bytes)
                meta_data = META_STRUCT.unpack(meta_bytes)
                
                # Yield the trace entry instead of appending to a list
                yield TraceEntry(instr_data, meta_data, rank)
                entry_count += 1
                
    except Exception as e:
        print(f"Error reading files for rank {rank}: {e}")
        # The generator will stop here
    
    print(f"Generator finished reading {entry_count} entries from rank {rank}")


def write_merged_trace_stream(entries_stream: Iterator[TraceEntry], trace_out_path: str, meta_out_path: str):
    """
    Write merged trace entries from a stream (iterator) to output files.
    """
    try:
        # Handle .xz compression for output files
        if trace_out_path.endswith('.xz'):
            trace_out = lzma.open(trace_out_path, "wb")
        else:
            trace_out = open(trace_out_path, "wb")

        if meta_out_path.endswith('.xz'):
            meta_out = lzma.open(meta_out_path, "wb")
        else:
            meta_out = open(meta_out_path, "wb")
        
        entry_count = 0
        try:
            for entry in entries_stream:
                # Write trace instruction
                trace_out.write(TRACE_STRUCT.pack(*entry.instr_data))
                
                # Write metadata
                # NOTE: The metadata tuple already contains the correct rank/padding/timestamp
                meta_out.write(META_STRUCT.pack(*entry.meta_data))
                entry_count += 1
        finally:
            trace_out.close()
            meta_out.close()
        
        print(f"Successfully wrote {entry_count} merged entries to output files.")
            
    except Exception as e:
        print(f"Error writing output files: {e}")
        sys.exit(1)


def main():
    """Main function to merge trace files using stream processing."""
    if len(sys.argv) != 7:
        print("Usage: python merge_trace.py <trace0> <meta0> <trace1> <meta1> <out_trace> <out_meta>")
        sys.exit(1)
    
    # Parse command line arguments
    trace0_path = sys.argv[1]
    meta0_path = sys.argv[2]
    trace1_path = sys.argv[3]
    meta1_path = sys.argv[4]
    trace_out_path = sys.argv[5]
    meta_out_path = sys.argv[6]
    
    print("Merging trace files using low-memory stream merge...")
    print(f"Rank 0: {trace0_path} + {meta0_path}")
    print(f"Rank 1: {trace1_path} + {meta1_path}")
    print(f"Output: {trace_out_path} + {meta_out_path}")
    print()
    
    # 1. Create generators (streams) for each rank
    # Data is NOT loaded into RAM here, only the file pointers are set up.
    gen_rank0 = trace_entry_generator(trace0_path, meta0_path, 0)
    gen_rank1 = trace_entry_generator(trace1_path, meta1_path, 1)

    # 2. Use heapq.merge to merge the two already-sorted streams.
    # heapq.merge only keeps the next entry from each stream in memory.
    # The 'key' argument tells it to compare based on the timestamp attribute.
    merged_stream = heapq.merge(gen_rank0, gen_rank1, key=lambda x: x.timestamp)
    
    # 3. Write the merged stream directly to output files
    write_merged_trace_stream(merged_stream, trace_out_path, meta_out_path)
    
    print("\nMerge completed successfully!")
    print(f"Output files:")
    print(f" Trace: {trace_out_path}")
    print(f" Meta: {meta_out_path}")

if __name__ == "__main__":
    main()