# CPU Datapath Teaching Simulator

A cycle-stepped teaching simulator for computer organization. The repository includes a 16-bit single-bus CPU, RV32 execution models, a SystemC virtual computer, and standalone SystemVerilog cores.

| Experiment | What it runs | Start here |
|---|---|---|
| 16-bit single-bus CPU | A fixed-latency instruction ROM, register transfers, and ADD/SUB/HALT | [Build the demo](#16-bit-single-bus-demo) |
| RV32 control units | One multicycle datapath with hardwired, horizontal microprogrammed, or vertical microprogrammed control | `bash tests/run_control_units.sh` |
| RV32 pipeline and I/O | Five pipeline stages, instruction ROM, data cache, interrupts, and shared memory with HDD/NVMe/NIC | `bash tests/run_pipeline.sh` and `bash tests/run_io.sh` |
| SystemC virtual computer | Multicycle RV32 with instruction/data caches backed by shared RAM, MMIO devices, DMA, IRQs, and VCD traces | [SystemC quick start](#systemc-quick-start) |
| Standalone SystemVerilog | RV32IM core and a smaller teaching core that reads horizontal or vertical microcode ROM images | [RTL guide](rtl/README.md) |

## SystemC quick start

Run from the repository root with CMake and a C++17 compiler. The wrapper fetches and builds the pinned SystemC dependency.

```bash
bash examples/systemc/run.sh --interactive --trace /tmp/computer
```

Enter `on` at the simulator prompt. The default RAM is [256 KiB of DIMM memory](dimm/README.md) across four ranks. The [platform guide](docs/SYSTEMC_PLATFORM.md) covers reset, pause, single-stepping, status inspection, and tracing.

The C++ RV32 model implements RV32I, RV32M, Zicsr, FENCE.I, and M/S/U privilege levels. The SystemC platform supports trap delegation, CLINT timers, and Sv32 paging. Its [bare-metal C tests](examples/compiler_smoke/README.md) boot ELF images and exercise ECALL and trap return, PMP, timer interrupts, and paging. A microkernel remains future work.

## Implementation guides

| Area | Documentation |
|---|---|
| CPU layout and datapaths | [Source layout](soc/README.md), [datapath comparison](soc/CPU_DATAPATH.md), [pipeline timing](soc/rv32/pipeline/README.md) |
| Control and microcode | [Controller experiment](docs/control-unit.md), [horizontal binary encoding and RTL](docs/binary-microcode.md), [vertical microcode](docs/vertical-microcode.md) |
| Memory and paging | [Data cache](docs/cpu-cache.md), [Sv32 boot contract](docs/SV32_HARDWARE_CONTRACT.md), [DRAM timing](docs/DRAM_TIMING.md), [interleaved DRAM](docs/INTERLEAVED_DRAM.md) |
| I/O and bus sharing | [PIO, DMA, interrupts, and NVMe queues](docs/io-system.md), [PCIe BAR model](interconnect/pcie/README.md) |
| Cycle counts and arithmetic | [Timing contract](docs/TIMING_CONTRACT.md), [RV32 multiply and divide](docs/rv32-muldiv.md) |

## 16-bit single-bus demo

Run from this directory with a C++17 compiler:

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic main.cpp -o /tmp/cpu-demo
/tmp/cpu-demo
```

The program executes three instructions:

```text
ADD R3, R1, R2    R1 = 0x7FFF, R2 = 1
SUB R4, R5, R2    R5 = 0x8000
HALT
```

Key observations:

| Cycle | Operation | Result |
|---|---|---|
| 9 | ALU computes ADD and saves the result and flags to Z and FR | Z=0x8000, SF=1, OF=1; R3 is not yet updated |
| 10 | Z writes back to R3 | R3=0x8000 |
| 19 | ALU computes SUB and saves the result and flags to Z and FR | Z=0x7FFF, SF=0, OF=1; R4 is not yet updated |
| 20 | Z writes back to R4 | R4=0x7FFF |
| 28 | HALT | The core stops; FR retains the flags from the last operation |

The log shows the active clock pulse, instruction-cycle number, cycle within the instruction, and cycle within the stage, for example `pulse 8 instr 1/T8 ID/T1`. The cycle that completes an instruction is marked `[instruction complete]`; stage transitions show markers such as `[IF -> ID]`. PC, MAR, MDR, IR, Y, Z, and flags in the log reflect the state after the cycle's unified commit. `bus=------` means no component drives the internal bus that cycle. During ALU operations, A15, B15, and F15 are also printed for comparison with the expressions in the problem.

## Data and clock connections

`main -> Simulator::step -> Core::tick` is the call path for each cycle.

`ControlUnit` generates control signals from the current cycle-level state within the stage; `Core` connects the datapath according to those signals. `SingleBusDatapath::evaluate()` computes the next bus, ALU, and register states from the old state, and `Core` commits them together at the end of `tick()`. Each clock cycle corresponds to one step; the active clock pulse at the end latches the state and advances timing. `InstructionStage` represents the IF/ID/EX/MEM/WB functional stages, while the existing `Phase` represents a micro-operation state lasting one cycle.

The internal bus interface is in `soc/cpu16/bus_interface.hpp`: the control unit selects an output source, `Bus::read()` gets the bus value from the current register state, and `SingleBusDatapath` prepares the next state according to the write enables. The bus itself retains no state between cycles and returns an empty value when no source drives it.

The interface directly uses the 16-bit single bus in `soc/cpu16/bus.hpp`. `NoC` resolves the driving source through each component's tri-state output gate and rejects simultaneous drivers; the bus may feed multiple components whose input enables are active.

| Component | Responsibility | File |
|---|---|---|
| ALU | 16-bit addition and subtraction; combinational result and SF/OF/CF/ZF generation | `soc/cpu16/alu/alu.hpp` |
| Bus | Select PC, MDR, GPR, or Z as the internal bus output source | `soc/cpu16/bus_interface.hpp` |
| MMU | TLB, physical page-table reads/writes, R/W/X and privilege checks, A/D bits, fault registers, and cross-page translation; paging is disabled by default in Core | `soc/shared/mmu/mmu.hpp` |
| Cache | K-way set associativity and LRU/FIFO/random/LFU replacement; the RV32 system uses a write-through data cache, while standalone experiments retain write-back and write buffering | `soc/cache/cache.hpp`, `soc/cache/cpu_port.hpp` |
| Memory controller | Physical-window chip selection, base-address translation, and cache-block transfers at rank word width | `soc/controller/memory/memory.hpp` |
| FR | Save flags only when FRin is active | `soc/cpu16/registers/fr/fr.hpp` |
| GPRs | Sixteen 16-bit registers, addressed with four bits | `soc/cpu16/registers/gprs/gprs.hpp` |
| Y | Hold the ALU's first input A | `soc/cpu16/registers/y/y.hpp` |
| Z | Hold the ALU output F for write-back in a later cycle | `soc/cpu16/registers/z/z.hpp` |
| MAR, MDR, IR | Hold the memory address, returned instruction word, and current instruction, respectively | `soc/cpu16/registers/` |
| Decoder | Decode the instruction word in IR into an opcode and register numbers | `soc/shared/decoder/decoder.hpp` |
| TimingGenerator | Hold cycle-level state and advance stages, cycles, and instruction count on active clock pulses | `soc/shared/control_unit/timing_generator/` |
| ControlLogic | Select the next cycle state for the instruction and skip unnecessary stages | `soc/shared/control_unit/control_logic/` |
| ControlSignalGenerator | Generate control signals for the current cycle | `soc/shared/control_unit/control_signal_generator/` |

### Instruction fetch (IF): seven cycles

| Cycle within IF / cycle-level state | Control signals / action |
|---|---|
| T1 / FetchAddress | PCout, MARin: transfer PC to MAR over the internal bus |
| T2 / FetchRead | Read: issue a main-memory read request |
| T3 / FetchWait1 | Wait for main memory |
| T4 / FetchWait2 | Wait for main memory |
| T5 / FetchWait3 | Wait for main memory |
| T6 / FetchWait4 | Write the main-memory response into MDR over the external data path |
| T7 / FetchLatch | MDRout, IRin: transfer MDR to IR over the internal bus and decode combinationally |

From issuing the read command until data enters MDR takes five cycles, T2–T6. Receiving main-memory data in MDR does not occupy the internal bus; MDRout specifically denotes output to that bus.

The problem ignores PC increment when calculating instruction-fetch time. This implementation additionally defines a separate increment path that performs `PC <- PC + 2` during T7 without occupying the ALU or internal bus.

### Register reads and addition/subtraction: three more cycles

| Instruction stage / cycle-level state | Control signals / action |
|---|---|
| ID/T1 / ReadOperand | GPRout(rs1), Yin: save the first operand in Y |
| EX/T1 / Execute | GPRout(rs2), ALUop, Zin, FRin: send the second operand over the bus to ALU input B, use the old Y for input A, write the result to Z and flags to FR |
| WB/T1 / WriteBack | Zout, GPRin(rd): write Z to the destination register over the bus |

An ADD/SUB instruction therefore takes ten cycles, corresponding to C1–C10 in Figure 5.6. C7 latches IR and decodes combinationally, directly preparing control for C8 without an extra decode cycle. HALT takes seven fetch cycles and one halt cycle, for eight total. ADD/SUB skips data-memory access (MEM); HALT completes in EX and has no MEM/WB. This is a non-pipelined, multicycle implementation with variable instruction-cycle lengths, not a fixed cycle count for every CPU.

A `BusSource` selector represents the internal bus, allowing at most one output source per cycle. This corresponds to the problem's output-enable/multiplexer mechanism. The GPR read index selects between rs1 and rs2; the write index uses rd. Y holds the first operand across cycles, and Z holds the ALU result until it can be written back over the bus.

## Arithmetic flags

Interpreted as 16-bit two's-complement numbers, the bit patterns represent values from -32768 to 32767. The calculation first uses a wider unsigned value and then truncates to the low 16 bits to obtain F.

```text
SF     = F15
OF_ADD = !(A15 xor B15) && (A15 xor F15)
OF_SUB =  (A15 xor B15) && (A15 xor F15)
```

Subtraction uses the sign bit of the original B. SF reflects only the most significant bit of the truncated result; OF indicates that the mathematical signed result is out of range. CF records unsigned carry for addition and unsigned borrow for subtraction. ZF indicates a zero result. The arithmetic cycle updates all four flags. Fetch, decode, write-back, and HALT preserve FR.

## Custom instructions and main-memory model

See the [ISA documentation](isa/README.md) for software-visible instructions, registers, address space, and control state. `isa/` contains only that document; runtime instruction definitions are in `soc/shared/decoder/instruction.hpp`.

The problem does not specify an instruction encoding, so this implementation uses a 16-bit teaching format:

```text
15        12 11         8 7          4 3          0
|  opcode  |     rd     |    rs1     |    rs2     |
```

ADD has opcode 1, SUB has opcode 2, and HALT is fixed at `0xF000`. For example, `0x1312` is `ADD R3, R1, R2`. R0 is also an ordinary register; it is not hardwired to zero. PC and MAR use 16-bit byte addresses, and each instruction occupies two bytes.

During fetch, the MAR address passes through the MMU before accessing the program array. Paging is disabled by default and addresses pass through unchanged. Paging can be enabled with `core.mmu().enable_paging(...)`; instruction fetches then check Execute permission. The MMU and cache are independent objects responsible for address translation/protection and physical data caching, respectively. See [MMU](soc/shared/mmu/README.md) and [Cache](soc/cache/README.md) for components and functions. Both are synchronous functional models and do not change the current core's seven-cycle fetch demonstration.

When `Core` is constructed, it encodes the program as instruction words. An internal read-only program array and pending read requests temporarily represent the external main-memory port. Its fixed response latency demonstrates the fetch sequence in the problem. The 16-bit Core is not yet connected to DRAM and has no LOAD/STORE instructions or interrupt sequence. The 32-bit dedicated datapath uses separate data memory for LOAD/STORE; see the datapath documentation above.

## Verification

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/alu_test.cpp -o /tmp/alu-test
/tmp/alu-test
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/core_test.cpp -o /tmp/core-test
/tmp/core-test
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/core_timing_test.cpp -o /tmp/core-timing-test
/tmp/core-timing-test
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/mmu_test.cpp -o /tmp/mmu-test
/tmp/mmu-test
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/tlb_test.cpp -o /tmp/tlb-test
/tmp/tlb-test
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/core_mmu_test.cpp -o /tmp/core-mmu-test
/tmp/core-mmu-test
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/virtual_memory_test.cpp -o /tmp/vm-test
/tmp/vm-test
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/virtual_memory_system_test.cpp -o /tmp/vm-system-test
/tmp/vm-system-test
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/cache_test.cpp -o /tmp/cache-test
/tmp/cache-test
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/mmu_cache_test.cpp -o /tmp/mmu-cache-test
/tmp/mmu-cache-test
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror soc/cache/example.cpp -o /tmp/cache-demo
/tmp/cache-demo
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror examples/memory_system/mmu_cache.cpp -o /tmp/mmu-cache-example
/tmp/mmu-cache-example
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror examples/memory_system/cache_dram.cpp -o /tmp/cache-dram-demo
/tmp/cache-dram-demo
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/cache_dram_test.cpp -o /tmp/cache-dram-test
/tmp/cache-dram-test
```

The ALU test covers all 65,536 A bit patterns, each paired with nine boundary B values for addition and subtraction, for 1,179,648 operations. Results are compared with wider signed-integer arithmetic and range checks. This is not an exhaustive test of all 2^32 input pairs.

Core tests check seven-cycle fetch, C7 combinational decode, Y/Z state across cycles, delayed result write-back, flag retention and clearing, overlapping source and destination registers, halt, and invalid addresses/instructions. Instruction-timing tests check IF/ID/EX/WB cycles pulse by pulse, stage transitions, 10/8-cycle instruction boundaries, wait-cycle counts, retries that do not advance timing, and the absence of further committed pulses after halt.

MMU tests check translation, permission checks after TLB hits, page faults, user/kernel permissions, LRU, mapping invalidation, and Core instruction-fetch execute permission. Standalone cache tests check address fields, set-associative hits, four replacement policies, write policies and buffering, cross-line reads/writes, callback failures, and address boundaries. The MMU/cache boundary test in `tests/mmu_cache_test.cpp` verifies that a permission rejection does not proceed to cache/main memory. A combined example demonstrates an initial TLB/cache miss followed by a hit for the same address, and rejection of a write to a read-only page even after a TLB hit.

The cache/DRAM integration example shows fills, hits, and dirty-line replacement/write-back in set 20 of a 32 KiB, four-way cache with 64-byte lines. The controller connects to an actual `dimm/` object. Integration tests check chip selection, byte/word address conversion, combinations of four replacement and four write policies, write-buffer waiting and read/write ordering, and end-to-end comparison with an independent byte array. See the [cache documentation](soc/cache/README.md) for scope and capacity calculations.

Virtual-memory hardware also has a [standalone MMU example](soc/shared/mmu/example.cpp) and an [end-to-end example](examples/memory_system/virtual_memory.cpp). They demonstrate real page-table byte formats, five TLB/page/cache combinations, fault state, external repair, and retry. The MMU and cache can each be compiled independently; page replacement and loading pages from disk are the responsibility of the surrounding system.
