#include "pin.H"
#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include <unordered_set> 
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <fstream> 

std::unordered_set<uintptr_t> ipc_addrs; 

// Pagemap constants
#define PAGEMAP_ENTRY_SIZE 8
#define PAGE_SHIFT 12
#define PAGE_SIZE_CONST (1ULL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE_CONST - 1))
#define PFN_MASK ((1ULL << 55) - 1)
#define PAGE_PRESENT (1ULL << 63)
#define PAGE_SWAPPED (1ULL << 62)

class AddressTranslator {
private:
    int pagemap_fd;
    pid_t pid;
    uint64_t page_size;
    uint64_t translation_count;
    uint64_t failed_translations;
    uint64_t skipped_translations;
    bool initialized;

public:
    AddressTranslator() : pagemap_fd(-1), translation_count(0), failed_translations(0), skipped_translations(0), initialized(false) {
        pid = getpid();
        page_size = sysconf(_SC_PAGESIZE);
        if (page_size == (uint64_t)-1) {
            page_size = 4096; // default
        }

        std::string pagemap_path = "/proc/" + std::to_string(pid) + "/pagemap";
        pagemap_fd = open(pagemap_path.c_str(), O_RDONLY);
        
        if (pagemap_fd < 0) {
            std::cerr << "Cannot open " << pagemap_path 
                      << " (errno=" << errno << "). Virtual addresses will not be translated." << std::endl;
            std::cerr << "NOTE: You may need to run with appropriate permissions or disable kernel.yama.ptrace_scope" << std::endl;
            initialized = false;
        } else {
            std::cout << "AddressTranslator initialized for PID " << pid << std::endl;
            std::cout << "Page size: " << page_size << " bytes" << std::endl;
            initialized = true;
        }
    }
    
    bool is_initialized() const {
        return initialized;
    }

    ~AddressTranslator() {
        if (pagemap_fd >= 0) {
            close(pagemap_fd);
        }
        if (translation_count > 0) {
            std::cout << "\n=== Address Translation Statistics ===" << std::endl;
            std::cout << "  Total translations attempted: " << translation_count << std::endl;
            std::cout << "  Successful translations: " << (translation_count - failed_translations) << std::endl;
            std::cout << "  Failed translations: " << failed_translations << std::endl;
            std::cout << "  Skipped translations: " << skipped_translations << std::endl;
            std::cout << "  Success rate: " 
                      << (translation_count > 0 ? 
                          (100.0 * (translation_count - failed_translations) / translation_count) : 0.0)
                      << "%" << std::endl;
        }
    }

    bool should_translate_address(uint64_t vaddr) {
        // Skip translation for certain address ranges
        if (vaddr == 0) {
            return false; // NULL address
        }
        
        // Skip very high addresses
        if (vaddr >= 0x800000000000ULL) {
            skipped_translations++;
            return false;
        }
        
        // Skip very low addresses
        if (vaddr < 0x1000) {
            skipped_translations++;
            return false;
        }
        
        return true;
    }

    uint64_t translate_address(uint64_t vaddr) {
        // Dont translate if pagemap is not available
        if (pagemap_fd < 0) {
            return vaddr;
        }

        // Dont translate certain addresses
        if (!should_translate_address(vaddr)) {
            return vaddr;
        }

        translation_count++;

        // Calculate page frame number and offset
        uint64_t page_number = vaddr / page_size;
        uint64_t page_offset = vaddr % page_size;
        
        // Seek to the pagemap entry for this virtual page
        off_t offset = page_number * PAGEMAP_ENTRY_SIZE;
        if (lseek(pagemap_fd, offset, SEEK_SET) != offset) {
            failed_translations++;
            return vaddr; // Return original address on failure
        }

        // Read the pagemap entry
        uint64_t pagemap_entry;
        ssize_t bytes_read = read(pagemap_fd, &pagemap_entry, PAGEMAP_ENTRY_SIZE);
        
        if (bytes_read != PAGEMAP_ENTRY_SIZE) {
            failed_translations++;
            return vaddr; // Return original address on failure
        }

        // Check if page is present in physical memory
        if (!(pagemap_entry & PAGE_PRESENT)) {
            failed_translations++;
            return vaddr; // Return original address - page not in memory
        }

        // Check if page is swapped
        if (pagemap_entry & PAGE_SWAPPED) {
            failed_translations++;
            return vaddr; // Return original address
        }

        // Extract the physical frame number
        uint64_t pfn = pagemap_entry & PFN_MASK;
        
        // Calculate physical address
        uint64_t paddr = (pfn * page_size) + page_offset;
        
        return paddr;
    }
};

// Global translator instance
AddressTranslator* g_translator = nullptr; 

using namespace std;
#define NUM_INSTR_DESTINATIONS 2
#define NUM_INSTR_SOURCES 4

typedef struct trace_instr_format {
    unsigned long long int ip;  // instruction pointer (program counter) value

    unsigned char is_branch;    // is this branch
    unsigned char branch_taken; // if so, is this taken

    unsigned char destination_registers[NUM_INSTR_DESTINATIONS]; // output registers
    unsigned char source_registers[NUM_INSTR_SOURCES];           // input registers

    unsigned long long int destination_memory[NUM_INSTR_DESTINATIONS]; // output memory
    unsigned long long int source_memory[NUM_INSTR_SOURCES];           // input memory
    
    uint8_t ipc_tag; //1 = IPC-related, 0 = not

} trace_instr_format_t;


struct meta_info_t {
    int mpi_rank;
    uint64_t timestamp;
};

FILE* meta_out;

/* ================================================================== */
// Global variables 
/* ================================================================== */

UINT64 instrCount = 0;
UINT64 tracedInstrCount = 0; //how many instructions traced

FILE* out;
FILE* out_va;

bool output_file_closed = false;
bool output_va_file_closed = false;
bool tracing_on = false;

trace_instr_format_t curr_instr;
trace_instr_format_t curr_instr_va;

bool roi_found = false;
bool roi_terminated = false;

/* ===================================================================== */
// Command line switches
/* ===================================================================== */
KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE,  "pintool", "o", "champsim.trace", 
        "specify file name for Champsim tracer output");

KNOB<UINT64> KnobSkipInstructions(KNOB_MODE_WRITEONCE, "pintool", "s", "0", 
        "How many instructions to skip before tracing begins");

KNOB<UINT64> KnobTraceInstructions(KNOB_MODE_WRITEONCE, "pintool", "t", "1000000", 
        "How many instructions to trace");

/* ===================================================================== */
// Utilities
/* ===================================================================== */

/*!
 *  Print out help message.
 */
INT32 Usage()
{
    cerr << "This tool creates a register and memory access trace" << endl 
        << "Specify the output trace file with -o" << endl 
        << "Specify the number of instructions to skip before tracing with -s" << endl
        << "Specify the number of instructions to trace with -t" << endl << endl;

    cerr << KNOB_BASE::StringKnobSummary() << endl;

    return -1;
}

/* ===================================================================== */
// Analysis routines
/* ===================================================================== */

void CloseOutputFiles() {
    if(!output_file_closed) {
        std::cout << "[PinTool] Closing trace files. Total traced: " << tracedInstrCount << std::endl;
        if (out) fclose(out);
        if (out_va) fclose(out_va);
        if (meta_out) fclose(meta_out);
        output_file_closed = true;
        output_va_file_closed = true;
    }
}

void StartROI() { 
    if (!roi_found) {
        std::cout << "[PinTool] ROI Begin Marker Hit. Tracing started." << std::endl;
        roi_found = true;
        tracing_on = true; 
    }
}

void StopROI() { 
    if (roi_found && !roi_terminated) {
        std::cout << "[PinTool] ROI End Marker Hit. Tracing stopped." << std::endl;
        tracing_on = false;
        roi_terminated = true;
        CloseOutputFiles();
    }
}

void BeginInstruction(VOID *ip, UINT32 op_code, VOID *opstring)
{
    instrCount++;

    // 1. Fallback Start (if ROI hasn't been found)
    if (!roi_found && !roi_terminated) {
        if(instrCount > KnobSkipInstructions.Value()) 
        {
            tracing_on = true;
        }
    }

    // 2. Relative Stop (after tracing KnobTraceInstructions)
    if (tracing_on) {
        if (tracedInstrCount >= KnobTraceInstructions.Value()) {
            tracing_on = false;
            roi_terminated = true;
            CloseOutputFiles();
        }
    }

    if(!tracing_on) 
        return;

    tracedInstrCount++;

    // reset the current instruction
    curr_instr.ip = (uint64_t)ip;
    curr_instr_va.ip = (uint64_t)ip;

    curr_instr.is_branch = 0;
    curr_instr.branch_taken = 0;
    curr_instr_va.is_branch = 0;
    curr_instr_va.branch_taken = 0;

    for(int i=0; i<NUM_INSTR_DESTINATIONS; i++) 
    {
        curr_instr.destination_registers[i] = 0;
        curr_instr.destination_memory[i] = 0;
        curr_instr_va.destination_registers[i] = 0;
        curr_instr_va.destination_memory[i] = 0;
    }

    for(int i=0; i<NUM_INSTR_SOURCES; i++) 
    {
        curr_instr.source_registers[i] = 0;
        curr_instr.source_memory[i] = 0;
        curr_instr_va.source_registers[i] = 0;
        curr_instr_va.source_memory[i] = 0;
    }


    //init ipc tag flag
    curr_instr.ipc_tag = 0; 
    curr_instr_va.ipc_tag = 0; 
}

void EndInstruction()
{
    if(tracing_on)
    {
        // keep tracing
        fwrite(&curr_instr, sizeof(trace_instr_format_t), 1, out);
        fwrite(&curr_instr_va, sizeof(trace_instr_format_t), 1, out_va);

        //write to metadata file
        meta_info_t meta;
        const char* rank_env = getenv("OMPI_COMM_WORLD_RANK");
        meta.mpi_rank = rank_env ? atoi(rank_env) : -1;
        meta.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

        fwrite(&meta, sizeof(meta_info_t), 1, meta_out);
    }
}

void BranchOrNot(UINT32 taken)
{
    //printf("[%d] ", taken);

    curr_instr.is_branch = 1;
    curr_instr_va.is_branch = 1;
    if(taken != 0)
    {
        curr_instr.branch_taken = 1;
        curr_instr_va.branch_taken = 1;
    }
}

void RegRead(UINT32 i, UINT32 index)
{
    if(!tracing_on) return;

    REG r = (REG)i;

    /*
       if(r == 26)
       {
    // 26 is the IP, which is read and written by branches
    return;
    }
    */

    //cout << r << " " << REG_StringShort((REG)r) << " " ;
    //cout << REG_StringShort((REG)r) << " " ;

    //printf("%d ", (int)r);

    // check to see if this register is already in the list
    int already_found = 0;
    for(int i=0; i<NUM_INSTR_SOURCES; i++)
    {
        if(curr_instr.source_registers[i] == ((unsigned char)r))
        {
            already_found = 1;
            break;
        }
    }
    if(already_found == 0)
    {
        for(int i=0; i<NUM_INSTR_SOURCES; i++)
        {
            if(curr_instr.source_registers[i] == 0)
            {
                curr_instr.source_registers[i] = (unsigned char)r;
                curr_instr_va.source_registers[i] = (unsigned char)r;
                break;
            }
        }
    }
}

void RegWrite(REG i, UINT32 index)
{
    if(!tracing_on) return;

    REG r = (REG)i;

    /*
       if(r == 26)
       {
    // 26 is the IP, which is read and written by branches
    return;
    }
    */

    //cout << "<" << r << " " << REG_StringShort((REG)r) << "> ";
    //cout << "<" << REG_StringShort((REG)r) << "> ";

    //printf("<%d> ", (int)r);

    int already_found = 0;
    for(int i=0; i<NUM_INSTR_DESTINATIONS; i++)
    {
        if(curr_instr.destination_registers[i] == ((unsigned char)r))
        {
            already_found = 1;
            break;
        }
    }
    if(already_found == 0)
    {
        for(int i=0; i<NUM_INSTR_DESTINATIONS; i++)
        {
            if(curr_instr.destination_registers[i] == 0)
            {
                curr_instr.destination_registers[i] = (unsigned char)r;
                curr_instr_va.destination_registers[i] = (unsigned char)r;
                break;
            }
        }
    }
    /*
       if(index==0)
       {
               curr_instr.destination_register = (uint64_t)r;
       }
       */
}

void MemoryRead(VOID* addr, UINT32 index, UINT32 read_size)
{
    if(!tracing_on) return;

    //printf("0x%llx,%u ", (unsigned long long int)addr, read_size);

    uint64_t vaddr = (uint64_t)addr;
    
    // Translate virtual address to physical address
    uint64_t paddr = vaddr;
    if (g_translator != nullptr) {
        paddr = g_translator->translate_address(vaddr);
    }

    // check to see if this memory read location is already in the list
    int already_found = 0;
    for(int i=0; i<NUM_INSTR_SOURCES; i++)
    {
        if(curr_instr.source_memory[i] == paddr)
        {
            already_found = 1;
            break;
        }
    }
    if(already_found == 0)
    {
        for(int i=0; i<NUM_INSTR_SOURCES; i++)
        {
            if(curr_instr.source_memory[i] == 0)
            {
                curr_instr.source_memory[i] = paddr;  // Store physical address
                curr_instr_va.source_memory[i] = vaddr; // Store virtual address
                break;
            }
        }
    }

    // Check IPC tags using original virtual address
    // if (ipc_addrs.find(vaddr) != ipc_addrs.end()) { 
    //     curr_instr.ipc_tag = 1; 
    //     curr_instr_va.ipc_tag = 1; 
    // } 
}

void MemoryWrite(VOID* addr, UINT32 index)
{
    if(!tracing_on) return;

    //printf("(0x%llx) ", (unsigned long long int) addr);

    uint64_t vaddr = (uint64_t)addr;
    
    // Translate virtual address to physical address
    uint64_t paddr = vaddr;
    if (g_translator != nullptr) {
        paddr = g_translator->translate_address(vaddr);
    }

    // check to see if this memory write location is already in the list
    int already_found = 0;
    for(int i=0; i<NUM_INSTR_DESTINATIONS; i++)
    {
        if(curr_instr.destination_memory[i] == paddr)
        {
            already_found = 1;
            break;
        }
    }
    if(already_found == 0)
    {
        for(int i=0; i<NUM_INSTR_DESTINATIONS; i++)
        {
            if(curr_instr.destination_memory[i] == 0)
            {
                curr_instr.destination_memory[i] = paddr;  // Store physical address
                curr_instr_va.destination_memory[i] = vaddr; // Store virtual address
                break;
            }
        }
    }
    /*
       if(index==0)
       {
       curr_instr.destination_memory = (long long int)addr;
       }
       */

    // Check IPC tags using original virtual address
    // if (ipc_addrs.find(vaddr) != ipc_addrs.end()) { 
    //     curr_instr.ipc_tag = 1; 
    //     curr_instr_va.ipc_tag = 1; 
    // } 
}

/* ===================================================================== */
// Instrumentation callbacks
/* ===================================================================== */

// Is called for every instruction and instruments reads and writes
VOID Instruction(INS ins, VOID *v)
{
    // ROI Markers: xchg bx, bx (Start) and xchg cx, cx (Stop)
    if (INS_IsXchg(ins) && INS_OperandCount(ins) >= 2 &&
        INS_OperandIsReg(ins, 0) && REG_FullRegName(INS_OperandReg(ins, 0)) == REG_GBX &&
        INS_OperandIsReg(ins, 1) && REG_FullRegName(INS_OperandReg(ins, 1)) == REG_GBX) {
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)StartROI, IARG_END);
    }

    if (INS_IsXchg(ins) && INS_OperandCount(ins) >= 2 &&
        INS_OperandIsReg(ins, 0) && REG_FullRegName(INS_OperandReg(ins, 0)) == REG_GCX &&
        INS_OperandIsReg(ins, 1) && REG_FullRegName(INS_OperandReg(ins, 1)) == REG_GCX) {
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)StopROI, IARG_END);
    }

    

    // begin each instruction with this function
    UINT32 opcode = INS_Opcode(ins);
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)BeginInstruction, IARG_INST_PTR, IARG_UINT32, opcode, IARG_END);

    // instrument branch instructions
    if(INS_IsBranch(ins))
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)BranchOrNot, IARG_BRANCH_TAKEN, IARG_END);

    // instrument register reads
    UINT32 readRegCount = INS_MaxNumRRegs(ins);
    for(UINT32 i=0; i<readRegCount; i++) 
    {
        UINT32 regNum = INS_RegR(ins, i);

        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)RegRead,
                IARG_UINT32, regNum, IARG_UINT32, i,
                IARG_END);
    }

    // instrument register writes
    UINT32 writeRegCount = INS_MaxNumWRegs(ins);
    for(UINT32 i=0; i<writeRegCount; i++) 
    {
        UINT32 regNum = INS_RegW(ins, i);

        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)RegWrite,
                IARG_UINT32, regNum, IARG_UINT32, i,
                IARG_END);
    }

    // instrument memory reads and writes
    UINT32 memOperands = INS_MemoryOperandCount(ins);

    // Iterate over each memory operand of the instruction.
    for (UINT32 memOp = 0; memOp < memOperands; memOp++) 
    {
        if (INS_MemoryOperandIsRead(ins, memOp)) 
        {
            UINT32 read_size = INS_MemoryReadSize(ins);

            INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)MemoryRead,
                    IARG_MEMORYOP_EA, memOp, IARG_UINT32, memOp, IARG_UINT32, read_size,
                    IARG_END);
        }
        if (INS_MemoryOperandIsWritten(ins, memOp)) 
        {
            INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)MemoryWrite,
                    IARG_MEMORYOP_EA, memOp, IARG_UINT32, memOp,
                    IARG_END);
        }
    }

    // finalize each instruction with this function
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)EndInstruction, IARG_END);
}

/*!
 * Print out analysis results.
 * This function is called when the application exits.
 * @param[in]   code            exit code of the application
 * @param[in]   v               value specified by the tool in the 
 *                              PIN_AddFiniFunction function call
 */
VOID Fini(INT32 code, VOID *v)
{
    // close the file if it hasn't already been closed
    std::cout << "Final Instruction Count: " << instrCount << std::endl;
    CloseOutputFiles();
    
    // Clean up the address translator
    if (g_translator != nullptr) {
        delete g_translator;
        g_translator = nullptr;
    }
}

/*!
 * The main procedure of the tool.
 * This function is called when the application image is loaded but not yet started.
 * @param[in]   argc            total number of elements in the argv array
 * @param[in]   argv            array of command line arguments, 
 *                              including pin -t <toolname> -- ...
 */
int main(int argc, char *argv[])
{
    // Initialize PIN library. Print help message if -h(elp) is specified
    // in the command line or the command line is invalid 
    if( PIN_Init(argc,argv) )
        return Usage();

    const char* fileName = KnobOutputFile.Value().c_str();

    out = fopen(fileName, "ab");
    if (!out) 
    {
        cout << "Couldn't open output trace file. Exiting." << endl;
        exit(1);
    }

    std::string va_file_name = std::string(fileName) + ".virtual";
    out_va = fopen(va_file_name.c_str(), "ab");
    if (!out_va) 
    {
        cout << "Couldn't open virtual output trace file. Exiting." << endl;
        exit(1);
    }

    //init meta data
    std::string meta_file_name = std::string(fileName) + ".meta";
    meta_out = fopen(meta_file_name.c_str(), "wb");
    if (!meta_out) {
        cerr << "Couldn't open metadata file. Exiting." << endl;
        exit(1);
    }

    //writte PID to file
    const char* rank_env = getenv("OMPI_COMM_WORLD_RANK");
    int rank = rank_env ? atoi(rank_env) : -1;

    std::string pidfile = "/tmp/app_pid_rank" + std::to_string(rank) + ".txt";
    std::ofstream pf(pidfile);
    if (pf.is_open()) {
        pf << getpid() << std::endl;  // This is the traced application’s PID
        pf.close();
    } else {
        std::cerr << "Could not open PID file for writing: " << pidfile << std::endl;
    }

    // cout << "THIS IS SIZE OF TRACE INSTR FORMAT : " << sizeof(trace_instr_format) << endl;
    g_translator = new AddressTranslator();
    if (!g_translator->is_initialized()) {
        cerr << "WARNING: Address translator initialization incomplete." << endl;
        cerr << "Traces will contain virtual addresses only." << endl;
    }
    cout << "================================================" << endl << endl;

    // Register function to be called to instrument instructions
    INS_AddInstrumentFunction(Instruction, 0);

    // Register function to be called when the application exits
    PIN_AddFiniFunction(Fini, 0);

    //cerr <<  "===============================================" << endl;
    //cerr <<  "This application is instrumented by the Champsim Trace Generator" << endl;
    //cerr <<  "Trace saved in " << KnobOutputFile.Value() << endl;
    //cerr <<  "===============================================" << endl;

    // Start the program, never returns
    PIN_StartProgram();

    return 0;
}

/* ===================================================================== */
/* eof */
/* ===================================================================== */