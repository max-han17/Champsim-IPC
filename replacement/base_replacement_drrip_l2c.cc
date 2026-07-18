#include "cache.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {

static const uint8_t maxRRPV = 3;
static const uint32_t NUM_POLICY = 2;
static const uint32_t SDM_SIZE = 32;
static const uint32_t BIP_MAX = 32;
static const uint32_t PSEL_WIDTH = 10;
static const uint32_t PSEL_MAX = ((1u << PSEL_WIDTH) - 1);
static const uint32_t PSEL_THRS = (PSEL_MAX / 2);

struct DrripL2CState {
    bool initialized = false;
    uint32_t bip_counter = 0;
    std::vector<uint8_t> rrpv;
    std::vector<uint32_t> psel;
    std::vector<uint32_t> leader_sets;
};

std::unordered_map<const CACHE*, DrripL2CState> drrip_state;

inline uint32_t rrpv_index(const CACHE* cache, uint32_t set, uint32_t way)
{
    return set * cache->NUM_WAY + way;
}

void init_drrip_l2c_state(CACHE* cache, DrripL2CState& state)
{
    state.rrpv.assign(cache->NUM_SET * cache->NUM_WAY, maxRRPV);
    state.psel.assign(NUM_CPUS, 0);
    state.bip_counter = 0;

    uint32_t total_sdm_sets = NUM_CPUS * NUM_POLICY * SDM_SIZE;
    uint32_t selected = std::min(cache->NUM_SET, total_sdm_sets);
    state.leader_sets.clear();
    state.leader_sets.reserve(selected);

    unsigned long rand_seed = 1;
    unsigned long max_rand = 1048576;
    while (state.leader_sets.size() < selected) {
        rand_seed = rand_seed * 1103515245 + 12345;
        uint32_t candidate = ((unsigned)((rand_seed / 65536) % max_rand)) % cache->NUM_SET;
        if (std::find(state.leader_sets.begin(), state.leader_sets.end(), candidate) == state.leader_sets.end())
            state.leader_sets.push_back(candidate);
    }

    state.initialized = true;
}

int is_leader_set(const DrripL2CState& state, uint32_t cpu, uint32_t set)
{
    uint32_t per_cpu_leaders = NUM_POLICY * SDM_SIZE;
    uint32_t start = cpu * per_cpu_leaders;
    if (start >= state.leader_sets.size())
        return -1;

    uint32_t end = std::min<uint32_t>(start + per_cpu_leaders, state.leader_sets.size());
    for (uint32_t i = start; i < end; i++) {
        if (state.leader_sets[i] == set)
            return ((i - start) / SDM_SIZE);
    }
    return -1;
}

void bip_insert(const CACHE* cache, DrripL2CState& state, uint32_t set, uint32_t way)
{
    state.rrpv[rrpv_index(cache, set, way)] = maxRRPV;
    state.bip_counter++;
    if (state.bip_counter == BIP_MAX)
        state.bip_counter = 0;
    if (state.bip_counter == 0)
        state.rrpv[rrpv_index(cache, set, way)] = maxRRPV - 1;
}

} // namespace

uint32_t CACHE::find_victim(uint32_t cpu, uint64_t instr_id, uint32_t set, const BLOCK *current_set, uint64_t ip, uint64_t full_addr, uint32_t type)
{
    if (cache_type != IS_L2C)
        return lru_victim(cpu, instr_id, set, current_set, ip, full_addr, type);

    DrripL2CState& state = drrip_state[this];
    if (!state.initialized)
        init_drrip_l2c_state(this, state);

    // Fill invalid way first.
    for (uint32_t way = 0; way < NUM_WAY; way++) {
        if (block[set][way].valid == false)
            return way;
    }

    while (1) {
        for (uint32_t way = 0; way < NUM_WAY; way++) {
            if (state.rrpv[rrpv_index(this, set, way)] == maxRRPV)
                return way;
        }

        for (uint32_t way = 0; way < NUM_WAY; way++) {
            uint8_t& v = state.rrpv[rrpv_index(this, set, way)];
            if (v < maxRRPV)
                v++;
        }
    }

    assert(0);
    return 0;
}

void CACHE::update_replacement_state(uint32_t cpu, uint32_t set, uint32_t way, uint64_t full_addr, uint64_t ip, uint64_t victim_addr, uint32_t type, uint8_t hit)
{
    if (cache_type != IS_L2C) {
        if (type == WRITEBACK) {
            if (hit)
                return;
        }
        return lru_update(set, way);
    }

    DrripL2CState& state = drrip_state[this];
    if (!state.initialized)
        init_drrip_l2c_state(this, state);

    if (type == WRITEBACK) {
        state.rrpv[rrpv_index(this, set, way)] = maxRRPV - 1;
        return;
    }

    if (hit) {
        state.rrpv[rrpv_index(this, set, way)] = 0;
        return;
    }

    int leader = is_leader_set(state, cpu, set);
    if (leader == -1) {
        if (state.psel[cpu] > PSEL_THRS)
            bip_insert(this, state, set, way);
        else
            state.rrpv[rrpv_index(this, set, way)] = maxRRPV - 1;
    } else if (leader == 0) {
        if (state.psel[cpu] > 0)
            state.psel[cpu]--;
        bip_insert(this, state, set, way);
    } else if (leader == 1) {
        if (state.psel[cpu] < PSEL_MAX)
            state.psel[cpu]++;
        state.rrpv[rrpv_index(this, set, way)] = maxRRPV - 1;
    } else {
        assert(0);
    }
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
