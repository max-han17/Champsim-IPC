import struct
import sys
import os

# Format: <QBB2B4B2Q4QB7x
# Q: uint64 (ip)
# B: uint8 (is_branch)
# B: uint8 (branch_taken)
# 2B: 2x uint8 (destination_registers)
# 4B: 4x uint8 (source_registers)
# 2Q: 2x uint64 (destination_memory)
# 4Q: 4x uint64 (source_memory)
# B: uint8 (ipc_tag)
# 7x: 7 bytes padding
STRUCT_FORMAT = "<QBB2B4B2Q4QB7x"
STRUCT_SIZE = 72

def load_ipc_tags(tag_file_path):
    ipc_tags = set()
    try:
        with open(tag_file_path, 'r') as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        # Addresses are hex in the file
                        addr = int(line, 16)
                        # Align to 64 bytes for matching
                        ipc_tags.add(addr & ~63)
                    except ValueError:
                        continue
    except FileNotFoundError:
        print(f"Error: Tag file {tag_file_path} not found.")
        sys.exit(1)
    return ipc_tags

def process_traces(phys_trace, virt_trace, tag_file):
    ipc_tags = load_ipc_tags(tag_file)
    print(f"Loaded {len(ipc_tags)} unique IPC tags (64-byte aligned).")

    phys_out_path = phys_trace + ".tagged"
    virt_out_path = virt_trace + ".tagged"

    with open(phys_trace, 'rb') as f_phys, \
         open(virt_trace, 'rb') as f_virt, \
         open(phys_out_path, 'wb') as out_phys, \
         open(virt_out_path, 'wb') as out_virt:

        count = 0
        while True:
            phys_data = f_phys.read(STRUCT_SIZE)
            virt_data = f_virt.read(STRUCT_SIZE)

            if not phys_data or not virt_data:
                break

            if len(phys_data) < STRUCT_SIZE or len(virt_data) < STRUCT_SIZE:
                print("Warning: Truncated entry at end of file. Stopping.")
                break

            # Unpack virtual trace to check addresses
            # Format: ip, is_branch, branch_taken, d_reg[2], s_reg[4], d_mem[2], s_mem[4], ipc_tag
            v_unpacked = list(struct.unpack(STRUCT_FORMAT, virt_data))
            p_unpacked = list(struct.unpack(STRUCT_FORMAT, phys_data))

            # Virtual addresses are at indices 5-6 (dest) and 7-10 (source)
            # Struct indices after unpacking:
            # 0: ip
            # 1: is_branch
            # 2: branch_taken
            # 3, 4: destination_registers
            # 5, 6, 7, 8: source_registers
            # 9, 10: destination_memory[0,1]
            # 11, 12, 13, 14: source_memory[0,1,2,3]
            # 15: ipc_tag

            is_ipc = False
            
            # Check destination memory addresses (indices 9, 10)
            for i in range(9, 11):
                addr = v_unpacked[i]
                if addr != 0 and (addr & ~63) in ipc_tags:
                    is_ipc = True
                    break
            
            if not is_ipc:
                # Check source memory addresses (indices 11, 12, 13, 14)
                for i in range(11, 15):
                    addr = v_unpacked[i]
                    if addr != 0 and (addr & ~63) in ipc_tags:
                        is_ipc = True
                        break

            if is_ipc:
                v_unpacked[15] = 1
                p_unpacked[15] = 1
                
                # Repack and write
                out_phys.write(struct.pack(STRUCT_FORMAT, *p_unpacked))
                out_virt.write(struct.pack(STRUCT_FORMAT, *v_unpacked))
            else:
                # If not IPC, we can just write original data (or repacked with original ipc_tag)
                # To be safe and consistent, we always write what we unpacked (with potential 0 ipc_tag)
                out_phys.write(phys_data)
                out_virt.write(virt_data)

            count += 1
            if count % 100000 == 0:
                print(f"Processed {count} entries...", end='\r')

    print(f"\nProcessing complete. Total entries: {count}")
    print(f"Tagged physical trace: {phys_out_path}")
    print(f"Tagged virtual trace: {virt_out_path}")

if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("Usage: python3 tag_ipc.py <phys_trace> <virt_trace> <ipc_tags_file>")
        sys.exit(1)

    phys_trace = sys.argv[1]
    virt_trace = sys.argv[2]
    tag_file = sys.argv[3]

    process_traces(phys_trace, virt_trace, tag_file)
