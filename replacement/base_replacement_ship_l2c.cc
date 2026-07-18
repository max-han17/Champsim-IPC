#include "cache.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {

static const uint8_t maxRRPV = 3;
static const uint32_t SHCT_SIZE = 16384;
static const uint32_t SHCT_PRIME = 16381;
static const uint8_t SHCT_MAX = 7;

struct SamplerEntry {
    uint8_t valid = 0;
    uint8_t type = 0;
    uint8_t used = 0;
    uint64_t tag = 0;
    uint64_t cl_addr = 0;
    uint64_t ip = 0;
    uint32_t lru = 0;
};

struct ShipL2CState {
    bool initialized = false;
    uint32_t sampler_set_count = 0;
    std::vector<uint8_t> rrpv;
    std::vector<uint32_t> rand_sets;
    std::vector<SamplerEntry> sampler;
    std::vector<uint8_t> shct;
};

std::unordered_map<const CACHE*, ShipL2CState> ship_state;

inline uint32_t rrpv_index(const CACHE* cache, uint32_t set, uint32_t way)
{
    return set * cache->NUM_WAY + way;
}

inline uint32_t shct_index(uint32_t cpu, uint32_t idx)
{
    return cpu * SHCT_SIZE + idx;
}

inline SamplerEntry& sampler_at(const CACHE* cache, ShipL2CState& state, uint32_t sampled_set_idx, uint32_t way)
{
    return state.sampler[sampled_set_idx * cache->NUM_WAY + way];
}

void init_ship_l2c_state(CACHE* cache, ShipL2CState& state)
{
    state.rrpv.assign(cache->NUM_SET * cache->NUM_WAY, maxRRPV);
    state.shct.assign(NUM_CPUS * SHCT_SIZE, 0);

    state.sampler_set_count = std::min<uint32_t>(cache->NUM_SET, 256 * NUM_CPUS);
    state.rand_sets.clear();
    state.rand_sets.reserve(state.sampler_set_count);
    state.sampler.assign(state.sampler_set_count * cache->NUM_WAY, SamplerEntry{});

    for (uint32_t i = 0; i < state.sampler_set_count; i++) {
        for (uint32_t way = 0; way < cache->NUM_WAY; way++)
            sampler_at(cache, state, i, way).lru = way;
    }

    unsigned long rand_seed = 1;
    unsigned long max_rand = 1048576;
    while (state.rand_sets.size() < state.sampler_set_count) {
        rand_seed = rand_seed * 1103515245 + 12345;
        uint32_t candidate = ((unsigned)((rand_seed / 65536) % max_rand)) % cache->NUM_SET;
        if (std::find(state.rand_sets.begin(), state.rand_sets.end(), candidate) == state.rand_sets.end())
            state.rand_sets.push_back(candidate);
    }

    state.initialized = true;
}

uint32_t sampled_set_index(const ShipL2CState& state, uint32_t set)
{
    for (uint32_t i = 0; i < state.sampler_set_count; i++) {
        if (state.rand_sets[i] == set)
            return i;
    }
    return state.sampler_set_count;
}

void update_sampler(CACHE* cache, ShipL2CState& state, uint32_t cpu, uint32_t sampled_idx, uint64_t full_addr, uint64_t ip, uint8_t type)
{
    uint64_t cl_addr = (full_addr >> LOG2_BLOCK_SIZE);
    uint64_t tag = cl_addr / cache->NUM_SET;
    int match = -1;

    // check hit
    for (match = 0; match < static_cast<int>(cache->NUM_WAY); match++) {
        SamplerEntry& entry = sampler_at(cache, state, sampled_idx, match);
        if (entry.valid && (entry.tag == tag)) {
            uint32_t idx = ip % SHCT_PRIME;
            uint8_t& counter = state.shct[shct_index(cpu, idx)];
            if (counter > 0)
                counter--;

            entry.type = type;
            entry.used = 1;
            entry.cl_addr = cl_addr;
            break;
        }
    }

    // check invalid
    if (match == static_cast<int>(cache->NUM_WAY)) {
        for (match = 0; match < static_cast<int>(cache->NUM_WAY); match++) {
            SamplerEntry& entry = sampler_at(cache, state, sampled_idx, match);
            if (entry.valid == 0) {
                entry.valid = 1;
                entry.tag = tag;
                entry.cl_addr = cl_addr;
                entry.ip = ip;
                entry.type = type;
                entry.used = 0;
                break;
            }
        }
    }

    // miss with replacement
    if (match == static_cast<int>(cache->NUM_WAY)) {
        for (match = 0; match < static_cast<int>(cache->NUM_WAY); match++) {
            SamplerEntry& entry = sampler_at(cache, state, sampled_idx, match);
            if (entry.lru == (cache->NUM_WAY - 1)) {
                if (entry.used == 0) {
                    uint32_t idx = entry.ip % SHCT_PRIME;
                    uint8_t& counter = state.shct[shct_index(cpu, idx)];
                    if (counter < SHCT_MAX)
                        counter++;
                }

                entry.tag = tag;
                entry.cl_addr = cl_addr;
                entry.ip = ip;
                entry.type = type;
                entry.used = 0;
                break;
            }
        }
    }

    // update sampler LRU
    uint32_t curr_pos = sampler_at(cache, state, sampled_idx, match).lru;
    for (uint32_t way = 0; way < cache->NUM_WAY; way++) {
        SamplerEntry& entry = sampler_at(cache, state, sampled_idx, way);
        if (entry.lru < curr_pos)
            entry.lru++;
    }
    sampler_at(cache, state, sampled_idx, match).lru = 0;
}

} // namespace

uint32_t CACHE::find_victim(uint32_t cpu, uint64_t instr_id, uint32_t set, const BLOCK *current_set, uint64_t ip, uint64_t full_addr, uint32_t type)
{
    if (cache_type != IS_L2C)
        return lru_victim(cpu, instr_id, set, current_set, ip, full_addr, type);

    ShipL2CState& state = ship_state[this];
    if (!state.initialized)
        init_ship_l2c_state(this, state);

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

    ShipL2CState& state = ship_state[this];
    if (!state.initialized)
        init_ship_l2c_state(this, state);

    if (type == WRITEBACK) {
        if (hit)
            return;
        state.rrpv[rrpv_index(this, set, way)] = maxRRPV - 1;
        return;
    }

    uint32_t s_idx = sampled_set_index(state, set);
    if (s_idx < state.sampler_set_count)
        update_sampler(this, state, cpu, s_idx, full_addr, ip, type);

    if (hit) {
        state.rrpv[rrpv_index(this, set, way)] = 0;
        return;
    }

    uint32_t idx = ip % SHCT_PRIME;
    uint8_t predicted_dead = state.shct[shct_index(cpu, idx)];
    state.rrpv[rrpv_index(this, set, way)] = maxRRPV - 1;
    if (predicted_dead == SHCT_MAX)
        state.rrpv[rrpv_index(this, set, way)] = maxRRPV;
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
