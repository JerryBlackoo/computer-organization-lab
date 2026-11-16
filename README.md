# CPU 数据通路教学模拟

新增 **SystemC + TLM 2.0 虚拟计算机硬件平台**：独立 CPU、指令/数据 Cache、共享总线、
DIMM、NVMe/HDD/NIC、教学定时器与串口、DMA/IRQ，可上电、复位、暂停、单步、查看状态和输出 VCD。
整机默认 RAM 为 **4 Rank × 64 KiB = 256 KiB**（`[0,0x40000)`）；每 Rank 四颗 x8 Chip，
每 Chip 两个 Bank、每 Bank 64 行 × 128 列。独立教学 DIMM 的大阵列默认与整机小几何不同，详见 [DIMM 配置](dimm/README.md)。
运行 `bash examples/systemc/run.sh --interactive --trace /tmp/computer` 后输入 `on` 启动。
此入口使用多周期 RV32 核、从 DIMM 取指；完整接线、运行方式和当前 ISA 边界见
[SystemC 平台说明](docs/SYSTEMC_PLATFORM.md)。
现已提供 [Clang 生成的 C 程序验收](examples/compiler_smoke/README.md)，覆盖 ELF、嵌套调用、栈、
整数运算、字节/半字读写、机器态 ECALL → 异常处理 → MRET 与 UART；它是平台联调程序，微内核仍需后续实现。
另有用户态 C/PMP 实验：MRET 进入 U-mode，用户 ECALL 返回，并验证用户程序无法读取机器态 CSR、
访问 UART、覆盖内核数据或执行内核代码；地址与验收入口见同一文档。
CPU 功能模型现已覆盖 RV32I 40 条基础指令，并加入六种 Zicsr 和 FENCE.I；
共享 RAM 用例验证修改已缓存的指令后执行新代码，以及 FENCE 后通知设备发起 DMA。
另已接入单核 CLINT 兼容 mtime/mtimecmp/MSIP 与 WFI：CPU 等待时 DMA 和时间继续推进，
定时/软件中断可唤醒 CPU 并经 ISR/MRET 返回，编译器 C 程序也覆盖该路径。
现已加入 S-mode、异常/中断委托和 SRET。监督态 C 实验验证定时中断转发，
以及 U → S → M 的嵌套调用及返回；共享 RAM 多周期核心支持 Sv32/Svade，详见 [分页与启动合同](docs/SV32_HARDWARE_CONTRACT.md)。

旧 `SoC` / `ClockedSoC` 对照实验默认使用 **RV32 五级流水线**，已接现有 MMU、Cache、共享总线、DIMM 与机器中断。
IF/ID、ID/EX、EX/MEM、MEM/WB 分离指令状态，支持转发、load-use 停顿、分支冲刷、
真实访存等待与顺序提交。运行 `bash tests/run_pipeline.sh` 可看逐拍占位和退休结果。
接口与范围见 [RV32 流水线](soc/core/pipeline/README.md)。
原 MIPS 专用执行代码已清理，保留 RV32 共用的 32 位 ALU、寄存器堆和存储部件。

旧 `SoC` / `ClockedSoC` 对照实验默认启用 **CPU 数据 Cache**：`RV32 CPU → MMU → Cache → 共享总线 → Memory → DIMM`。
读命中直接返回，读缺失逐拍等待整行装入；写穿、不按写分配，MMIO 绕过缓存。
主存写监听同步更新已有缓存行，并处理 DMA 与缓存行填充交错的情况。
运行 `bash tests/run_cpu_cache.sh` 可查看流水线与两种多周期控制模式的命中/缺失、总线事务和主存访问。
接线、配置与边界见 [CPU 数据 Cache](docs/cpu-cache.md)。该对照路径从独立 ROM 取指；正式 SystemC 平台使用多周期核并从共享 RAM 取指。

新增 **I/O 六阶段闭环**：Guest RV32 通过 MMIO 驱动 HDD 查询/中断/DMA，
NVMe 通过主存 SQ/CQ 与 Doorbell 提交命令，CPU、HDD、NVMe、回环 NIC 共享事务总线和一份 DIMM。
运行 `bash tests/run_io.sh`，可验证四种传输/通知组合读出的 512 字节一致，以及权限、错误和并发。
寄存器表、十张原图逐图对照、Guest 驱动和教学模型边界见 [I/O 系统说明](docs/io-system.md)。

新增 **RV32 硬布线 / 微程序控制器对照实验**：两种模式共用 Decoder、逐拍 Datapath、
32 位 ALU、寄存器堆与数据存储器，支持整数运算、分支/跳转和字节/半字/字访存。
执行 `./tests/run_control_units.sh` 可运行四组测试和 `x3=30` 的双模式示例（30 / 33 拍）。
实现入口、控制存储器与时序说明见 [控制器实验](docs/control-unit.md)，
修改前的调用关系见 [阶段 1 检查](docs/control-unit-audit.md)。

根目录 `main.cpp` 保留题 43 中的 16 位 ALU、标志寄存器和单内部总线演示。
每次 `Simulator::step()` 调用一次 `Core::tick()`，推进一个模拟时钟周期。
指令阶段与节拍的对应关系见 [Core 指令周期说明](soc/core/README.md)。

16 位单总线与 RV32 多周期/流水线的部件、接口和运行命令见 [数据通路说明](soc/core/datapath/README.md)。

## 编译运行

在本目录执行，要求支持 C++17 的编译器：

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic main.cpp -o /tmp/cpu-demo
/tmp/cpu-demo
```

程序执行三条指令：

```text
ADD R3, R1, R2    R1 = 0x7FFF，R2 = 1
SUB R4, R5, R2    R5 = 0x8000
HALT
```

关键观察点：

| 周期 | 操作 | 结果 |
|---|---|---|
| 9 | ALU 计算 ADD，将结果和标志保存到 Z、FR | Z=0x8000，SF=1，OF=1，R3 尚未更新 |
| 10 | Z 写回 R3 | R3=0x8000 |
| 19 | ALU 计算 SUB，将结果和标志保存到 Z、FR | Z=0x7FFF，SF=0，OF=1，R4 尚未更新 |
| 20 | Z 写回 R4 | R4=0x7FFF |
| 28 | HALT | 核心停机，FR 保持最后一次运算的标志 |

日志显示有效时钟脉冲编号、指令周期编号、指令内节拍和阶段内节拍，例如
`pulse 8 instr 1/T8 ID/T1`；指令完成的拍标记为 `[instruction complete]`。
阶段切换显示 `[IF -> ID]` 等标记。日志中的 PC、MAR、MDR、IR、Y、Z 和标志是
该拍结束、统一提交后的状态。`bus=------` 表示本拍没有内部总线输出源。
ALU 工作时还会打印 A15、B15、F15，可用题目中的表达式逐项核对。

## 数据和时钟怎样连接

`main -> Simulator::step -> Core::tick` 是每一拍的调用路径。

`ControlUnit` 根据当前阶段内的拍级状态产生控制信号；`Core` 根据这些信号连接数据通路。
`SingleBusDatapath::evaluate()` 从旧状态计算总线、ALU 和寄存器的下一状态，
`Core` 在 `tick()` 末尾统一提交。
每个时钟周期对应一个节拍，周期末的有效时钟脉冲完成锁存并推进时序。
`InstructionStage` 表示 IF/ID/EX/MEM/WB 功能阶段，原有 `Phase` 表示占一拍的微操作状态。

核心内部总线接口放在 `soc/core/datapath/bus.hpp`：控制单元选择输出源，
`Bus::read()` 从当前寄存器状态取出总线值，`SingleBusDatapath` 根据各写使能准备下一状态。
总线本身不保存跨拍状态，无输出源时返回空值。
该接口直接使用 `soc/noc/noc.hpp` 的 16 位单总线。`NoC` 通过各部件的 out 三态门
解析驱动源，拒绝多个输出同时驱动；总线可同时送入多个开启 in 的部件。

| 部件 | 职责 | 文件 |
|---|---|---|
| ALU | 16 位加减法、组合地产生结果和 SF/OF | `soc/core/alu/alu.hpp` |
| Bus | 选择 PC、MDR、GPR 或 Z 作为内部总线输出源 | `soc/core/datapath/bus.hpp` |
| MMU | TLB、物理页表读写、R/W/X/特权级、A/D 位、故障寄存器、跨页翻译；Core 默认关闭分页 | `soc/core/mmu/mmu.hpp` |
| Cache | K 路组相联、LRU/FIFO/随机/LFU；RV32 整机已接写穿数据缓存，独立实验保留写回/写缓冲 | `soc/cache/cache.hpp`、`soc/cache/cpu_port.hpp` |
| 内存控制器 | 物理窗口片选、基址转换、按 Rank 字宽搬运 Cache 块 | `soc/controller/memory/memory.hpp` |
| FR | 仅在 FRin 有效时保存标志 | `soc/core/registers/fr/fr.hpp` |
| GPRs | 16 个 16 位寄存器，编号需要 4 位 | `soc/core/registers/gprs/gprs.hpp` |
| Y | 保存 ALU 的第一个输入 A | `soc/core/registers/y/y.hpp` |
| Z | 保存 ALU 的输出 F，供下一拍写回 | `soc/core/registers/z/z.hpp` |
| MAR、MDR、IR | 分别保存访存地址、返回的指令字、当前指令 | `soc/core/registers/` |
| Decoder | 将 IR 中的指令字解析为操作码和寄存器编号 | `soc/core/decoder/decoder.hpp` |
| TimingGenerator | 保存拍级状态，在有效时钟脉冲推进阶段/节拍和指令计数 | `soc/core/control_unit/timing_generator/` |
| ControlLogic | 按指令决定下一拍状态，跳过不需要的阶段 | `soc/core/control_unit/control_logic/` |
| ControlSignalGenerator | 生成本拍控制信号 | `soc/core/control_unit/control_signal_generator/` |

### 取指 IF：七拍

| IF 阶段内节拍 / 拍级状态 | 控制信号 / 动作 |
|---|---|
| T1 / FetchAddress | PCout、MARin：PC 经内部总线写入 MAR |
| T2 / FetchRead | Read：发出主存读请求 |
| T3 / FetchWait1 | 等待主存 |
| T4 / FetchWait2 | 等待主存 |
| T5 / FetchWait3 | 等待主存 |
| T6 / FetchWait4 | 主存响应经外部数据通路写入 MDR |
| T7 / FetchLatch | MDRout、IRin：MDR 经内部总线写入 IR，同时组合译码 |

从发出读命令到数据进入 MDR 共占 T2～T6 五拍。
MDR 接收主存数据时不占用内部总线，MDRout 则专门表示向内部总线输出。

题目计算取指周期时忽略 PC 自增。本实现另外约定一个独立增量通路，
在 T7 同时执行 `PC <- PC + 2`。这项约定不会占用 ALU 或内部总线。

### 读寄存器和加减法：再三拍

| 指令阶段 / 拍级状态 | 控制信号 / 动作 |
|---|---|
| ID/T1 / ReadOperand | GPRout(rs1)、Yin：第一个操作数存入 Y |
| EX/T1 / Execute | GPRout(rs2)、ALUop、Zin、FRin：第二个操作数经总线进入 ALU 的 B 端，A 端取旧 Y，结果写 Z，标志写 FR |
| WB/T1 / WriteBack | Zout、GPRin(rd)：Z 经总线写回目标寄存器 |

因此一条 ADD/SUB 共十拍，严格对应图 5.6 的 C1～C10。C7 锁存 IR 后组合译码，
直接准备 C8 的控制，不增加译码拍。HALT 在七拍取指后用一拍停机，共八拍。
ADD/SUB 跳过数据访存 MEM；HALT 在 EX 完成，没有 MEM/WB。
这是非流水的多周期实现，采用变长指令周期，并不是所有 CPU 的固定周期数。

内部总线用一个 `BusSource` 选择器表示，同一拍最多只有一个输出源。
这对应题目中的输出使能/多路选择机制；GPR 的读编号在 rs1 和 rs2 之间选择，写编号取 rd。
Y 让第一个操作数跨拍保持；Z 让 ALU 结果跨拍保持，之后才能通过这条总线写回。

## SF 和 OF 的实现

将原始位模式解释为 16 位补码时，可表示范围是 -32768～32767。
计算先使用较宽的无符号数，然后截取低 16 位得到 F。

```text
SF     = F15
OF_ADD = !(A15 xor B15) && (A15 xor F15)
OF_SUB =  (A15 xor B15) && (A15 xor F15)
```

减法使用原始 B 的符号位。SF 只反映截断结果的最高位；OF 表示数学上的有符号结果超出范围。
加减运算拍同时更新两个标志，取指、译码、写回和 HALT 都保留 FR。
这里实现题目指定的 SF、OF，未实现其他标志位。

## 自定义指令与主存模型

软件可见的指令、寄存器、地址空间和控制状态约定见 [ISA 说明](isa/README.md)。
`isa/` 只保存这一份文档，运行时指令定义位于 `soc/core/decoder/instruction.hpp`。

题目未指定指令编码，本实现采用 16 位教学格式：

```text
15        12 11         8 7          4 3          0
|  opcode  |     rd     |    rs1     |    rs2     |
```

ADD 的 opcode 为 1，SUB 为 2，HALT 固定为 `0xF000`。
例如 `0x1312` 是 `ADD R3, R1, R2`。R0 也是普通寄存器，没有硬连零的规定。
PC、MAR 使用 16 位字节地址，每条指令占两个字节。
取指时 MAR 的地址先经过 MMU，再访问程序数组；默认分页关闭，地址直通。
通过 `core.mmu().enable_paging(...)` 可启用分页，取指会检查 Execute 权限。
MMU 与 Cache 是两个独立对象，分别负责地址转换/保护与物理数据缓存。
组件和功能说明见 [MMU](soc/core/mmu/README.md) 与 [Cache](soc/cache/README.md)。
这两个模块是同步功能模型，不改变当前核心的七拍取指演示。

`Core` 构造时将程序编码为指令字，内部的只读程序数组和待完成读请求暂时代表外部主存端口。
其响应延迟固定，用来演示题设取指流程；16 位 Core 尚未连接 DRAM，也没有 LOAD/STORE
指令和中断序列。32 位专用通路的 LOAD/STORE 使用独立的数据存储器，见上方数据通路说明。

## 验证

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

ALU 测试遍历全部 65536 个 A 位模式，分别配对九个边界 B 值进行加减，
总计 1,179,648 次运算，与较宽有符号整数的数学结果及范围判断比较。
这不是全部 2^32 个输入对的穷举。
核心测试检查七拍取指、C7 组合译码、Y/Z 跨拍保存、结果延迟写回、标志保持和清除、
源目标寄存器重叠、停机以及非法地址/指令处理。
指令时序测试检查逐脉冲的 IF/ID/EX/WB 节拍、阶段切换、10/8 拍指令边界、等待拍计数、
异常重试不推进时序，以及停机后不再提交脉冲。

MMU 测试检查转换、TLB 命中后的权限检查、缺页、用户/内核态权限、LRU、映射失效和 Core 取指执行权限。
独立 Cache 测试检查地址字段、组相联命中、四种替换、写策略和写缓冲，以及跨行读写、回调失败和地址边界。
MMU/Cache 的边界测试放在 `tests/mmu_cache_test.cpp`，验证权限拒绝后不会继续访问 Cache/主存。组合示例演示同一地址第一次 TLB/Cache 未命中、
第二次命中，以及 TLB 命中后仍拒绝对只读页写入。

Cache/DRAM 集成示例展示 32 KiB、4 路、64 B 行在第 20 组的装入、命中和脏行替换写回。
控制器接实际 `dimm/` 对象，集成测试验证片选、字节/字地址转换、四种替换和四种写策略组合、
写缓冲等待与读写顺序，以及与独立字节数组的全链路对照。功能范围与容量计算见 [Cache 说明](soc/cache/README.md)。

虚拟存储器硬件另有 [独立 MMU 示例](soc/core/mmu/example.cpp) 和
[全链路示例](examples/memory_system/virtual_memory.cpp)，演示真实页表字节格式、五种 TLB/Page/Cache 组合、
故障状态、外层修复及重试。MMU 与 Cache 均可单独编译；换页策略和磁盘调入由外层负责。
