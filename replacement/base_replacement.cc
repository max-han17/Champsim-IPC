#include "cache.h"

uint32_t CACHE::find_victim(uint32_t cpu, uint64_t instr_id, uint32_t set, const BLOCK *current_set, uint64_t ip, uint64_t full_addr, uint32_t type)
{
    // IPC-aware replacement policy for L2C
    if (cache_type == IS_L2C) {
        // Fill invalid way first
        for (uint32_t way = 0; way < NUM_WAY; way++) {
            if (block[set][way].valid == false)
                return way;
        }

        // Pure-LRU baseline victim for comparison counters.
        uint32_t pure_lru_way = NUM_WAY;
        for (uint32_t way = 0; way < NUM_WAY; way++) {
            if (block[set][way].lru == NUM_WAY - 1) {
                pure_lru_way = way;
                break;
            }
        }
        if (pure_lru_way == NUM_WAY) {
            cerr << "[" << NAME << "] " << __func__ << " no pure LRU victim! set: " << set << endl;
            assert(0);
        }

        // scan from LRU to MRU position
        // Prefer non-IPC lines first, then unprotected IPC lines;
        // give protected IPC lines a second chance by clearing prot and skipping.
        bool skipped_protected_ipc = false;
        for (uint32_t rank = NUM_WAY - 1; ; rank--) {
            // Find the way that holds this LRU rank
            for (uint32_t way = 0; way < NUM_WAY; way++) {
                if (block[set][way].lru == rank) {
                    if (block[set][way].ipc_tag == 0) {
                        // Non-IPC line: evict immediately
                        if (warmup_complete[cpu]) {
                            if (way == pure_lru_way)
                                ipc_policy_same_as_lru++;
                            else
                                ipc_policy_diff_from_lru++;
                            if (skipped_protected_ipc)
                                ipc_policy_protected_skip_preserved++;
                        }
                        return way;
                    } else if (block[set][way].prot == 0) {
                        // IPC line, already had its second chance: evict
                        if (warmup_complete[cpu]) {
                            if (way == pure_lru_way)
                                ipc_policy_same_as_lru++;
                            else
                                ipc_policy_diff_from_lru++;
                            if (skipped_protected_ipc)
                                ipc_policy_protected_skip_preserved++;
                        }
                        return way;
                    } else {
                        // IPC line, still protected: clear protection and skip
                        block[set][way].prot = 0;
                        skipped_protected_ipc = true;
                    }
                    break;
                }
            }
            if (rank == 0)
                break;
        }

        // all ways were IPC+protected (prot bits now cleared above); evict LRU.
        // Count this as a forced protected-IPC eviction.
        if (warmup_complete[cpu]) {
            prot_ipc_evictions++;
            ipc_policy_fallback_all_prot++;
            ipc_policy_same_as_lru++;
        }
        return pure_lru_way;
    }

    // baseline LRU replacement policy for all other cache levels
    return lru_victim(cpu, instr_id, set, current_set, ip, full_addr, type); 
}

void CACHE::update_replacement_state(uint32_t cpu, uint32_t set, uint32_t way, uint64_t full_addr, uint64_t ip, uint64_t victim_addr, uint32_t type, uint8_t hit)
{
    if (type == WRITEBACK) {
        if (hit) // writeback hit does not update LRU state
            return;
    }

    // IPC-aware protection refresh for L2C: on any hit to an IPC-tagged line,
    // re-arm the protection bit so it gets another second chance on next eviction pass.
    if (cache_type == IS_L2C && hit && block[set][way].ipc_tag == 1) {
        block[set][way].prot = 1;
    }

    return lru_update(set, way);
}

uint32_t CACHE::lru_victim(uint32_t cpu, uint64_t instr_id, uint32_t set, const BLOCK *current_set, uint64_t ip, uint64_t full_addr, uint32_t type)
{
    uint32_t way = 0;

    // fill invalid line first
    for (way=0; way<NUM_WAY; way++) {
        if (block[set][way].valid == false) {

            DP ( if (warmup_complete[cpu]) {
            cout << "[" << NAME << "] " << __func__ << " instr_id: " << instr_id << " invalid set: " << set << " way: " << way;
            cout << hex << " address: " << (full_addr>>LOG2_BLOCK_SIZE) << " victim address: " << block[set][way].address << " data: " << block[set][way].data;
            cout << dec << " lru: " << block[set][way].lru << endl; });

            break;
        }
    }

    // LRU victim
    if (way == NUM_WAY) {
        for (way=0; way<NUM_WAY; way++) {
            if (block[set][way].lru == NUM_WAY-1) {

                DP ( if (warmup_complete[cpu]) {
                cout << "[" << NAME << "] " << __func__ << " instr_id: " << instr_id << " replace set: " << set << " way: " << way;
                cout << hex << " address: " << (full_addr>>LOG2_BLOCK_SIZE) << " victim address: " << block[set][way].address << " data: " << block[set][way].data;
                cout << dec << " lru: " << block[set][way].lru << endl; });

                break;
            }
        }
    }

    if (way == NUM_WAY) {
        cerr << "[" << NAME << "] " << __func__ << " no victim! set: " << set << endl;
        assert(0);
    }

    return way;
}

void CACHE::lru_update(uint32_t set, uint32_t way)
{
    // update lru replacement state
    for (uint32_t i=0; i<NUM_WAY; i++) {
        if (block[set][i].lru < block[set][way].lru) {
            block[set][i].lru++;
        }
    }
    block[set][way].lru = 0; // promote to the MRU position
}

void CACHE::replacement_final_stats()
{

}

#ifdef NO_CRC2_COMPILE
void InitReplacementState()
{
    
}

uint32_t GetVictimInSet (uint32_t cpu, uint32_t set, const BLOCK *current_set, uint64_t PC, uint64_t paddr, uint32_t type)
{
    return 0;
}

void UpdateReplacementState (uint32_t cpu, uint32_t set, uint32_t way, uint64_t paddr, uint64_t PC, uint64_t victim_addr, uint32_t type, uint8_t hit)
{
    
}

void PrintStats_Heartbeat()
{
    
}

void PrintStats()
{

}
#endif
