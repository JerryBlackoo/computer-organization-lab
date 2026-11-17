# RV32 五级流水线

流水线是现有 `RV32Core` 的执行模式，整机 `SoC` 默认启用。它沿用
`Decoder::decode_rv32`、32 位 ALU、寄存器堆、架构状态、MMU、机器中断/CSR 与异步数据总线，
支持 RV32I 基础整数/访存/控制流/异常指令、RV32M 整数乘除，以及六种 Zicsr、FENCE.I、MRET、SRET、SFENCE.VMA 和 WFI；没有另建一套机器指令语义。
乘除先锁存操作数，再逐拍复用 32 位 ALU 完成 32 次位迭代；混合符号乘法和有符号除法可能增加结果修正拍。等待时保持 EX/ID/IF，允许更老指令退休，完成后正常转发。除零和有符号商溢出在启动拍完成；详见 [RV32M 说明](../../../docs/rv32-muldiv.md)。
字节/半字 Load 沿用 Load-use 停顿与 WB 转发；窄 Store 保留真实事务宽度和相邻字节。
原独立 MIPS 流水线、专用译码/编码器、取指与单周期执行模块已移除。

## 选择执行方式

```cpp
#include "soc/soc.hpp"
colab::SoC system(program); // 默认 RV32 五级流水线 + 数据 Cache。

// 保留多周期控制器对照实验。
colab::SoCConfig config;
config.cpu_execution = colab::soc::RV32Core::Execution::MultiCycle;
config.cpu_mode = colab::soc::RV32Core::Mode::Microprogrammed;
colab::SoC comparison(program, config);
```

独立 `RV32Core` 为兼容逐拍实验，构造器仍默认 `MultiCycle`。显式启用流水线：

```cpp
using CPU = colab::soc::RV32Core;
CPU cpu(program, CPU::Mode::Hardwired, 256, {}, CPU::Execution::Pipelined);
cpu.run_instructions(4);
```

流水执行采用硬布线控制，`Pipelined + Microprogrammed` 会明确拒绝。硬布线/微程序是控制方式，
流水/多周期是执行组织；目前实现的是流水硬布线以及原有两种多周期控制，没有微程序流水执行。
执行方式在构造时确定；多周期模式仍支持原有指令边界的 `select_mode()`。

## 五级与级间寄存器

| 阶段 | 工作 | 输出锁存 |
|---|---|---|
| IF | 从只读程序 ROM 取机器字；预测顺序 PC+4 | IF/ID：valid、PC、机器字、取指故障 |
| ID | 共享 RV32 译码、I/S/B 立即数恢复、双端口读寄存器 | ID/EX：译码、源值、PC、故障 |
| EX | 共享 ALU；计算有效地址；条件比较及跳转目标，JALR 清除目标最低位 | EX/MEM：运算值、地址、store 数据、下一 PC、故障 |
| MEM | 数据对齐/权限检查；本地读或 MMU→Cache/总线请求与响应 | MEM/WB：结果、访存状态、故障 |
| WB | 顺序退休，提交架构 PC/GPR、本地 store、CSR/xRET | 退休事件 |

四组 Latch 只向下一个时钟周期传递，不能一拍串穿多级。WB 写回对同拍 ID 可见。
没有数据/控制冒险和访存等待时，N 条简单指令第 N+4 拍完成，第 5 拍首次退休。
流水线提高吞吐率；单条指令的五级延迟与理想 CPI≈1 是不同概念。

`cpu.pc()` 始终是已提交状态的下一指令 PC；`cpu.pipeline().fetch_pc()` 是推测取指地址。
不把二者混作中断返回地址。程序边界由宿主的运行条数控制，没有隐式 HALT；
越界取指作为故障随流水线传递，错误分支上的越界不会提前抛出。

## 冒险处理

EX 输入可以从 EX/MEM 或 MEM/WB 转发，最近生产者优先，覆盖运算两输入、store 数据和分支比较。
load 的 EX 输出是地址，不能当成数据转发。紧邻 load-use 冻结 PC/IF/ID，向 ID/EX 插入一个气泡；
返回数据随后从 MEM/WB 转发。x0 不作为生产者，立即数的编码位不作为第二源寄存器。

`SoCConfig.pipeline.forwarding=false` 可关闭转发：RAW 依赖停到生产者 WB 写回，
无额外内存等待时紧邻依赖停两拍。EX 判断分支，静态预测不跳转；重定向时冲刷 ID 与当前 IF 机会。
分支目标是当前指令 PC 加已译码字节偏移；采用 RV32 IALIGN=32，只有实际选中的目标检查对齐。
算术结果按 32 位回绕，不引入有符号加减溢出陷阱。

## 真实访存与副作用

`SoC` 已有数据路径保持为 `MMU → CpuPort/Cache → 共享总线 → Memory → DIMM`，MMIO 绕过缓存。
同一时间最多一个 CPU 数据请求。背压或响应未就绪时冻结 MEM 及上游，WB 可继续排空；
被停住的 EX 会保存当拍退休生产者的转发值，避免等待期间丢失操作数。
等待由真实 ready/tag 响应决定，没有固定延迟计数器假装 Cache miss。

MEM 只在前面的 WB 已成功退休后发出请求。因此错误分支或较老故障之后的 store、
有破坏性读语义的 MMIO 都不会发往设备。外部 store 由总线数据拍产生可见写入，
CPU 必须收到确认后才退休；本地数组 store 在 WB 提交。
已提交到总线的请求不能撤销，也不会因停顿重复提交。
取指仍来自独立 ROM，不经过 MMU 或指令 Cache；独立 Core 未接总线时仍使用本地数据数组。

## 中断、CSR 与同步故障

中断保存架构 PC，冲刷尚未提交的指令。若有已发出的外部访存，先完成并退休该请求，
再进入中断，保留其读写效果；更年轻的访存不会开始。中断电平若在等待期间撤销，
已丢弃的年轻指令会从架构 PC 重新取指。已有 Direct/Vectored mtvec、MIE/MPIE 与 MRET 规则复用。

CSR/xRET 在 WB 操作既有 `InterruptSystem`，提交前不改变 CSR。它们阻止后续译码推进，
退休时清除推测状态并按已提交 PC 重新取指，保证 CSR 依赖、开关中断与 xRET 不泄漏年轻指令效果。
GPR 现场仍由 Guest ISR 保存和恢复。
WFI 在 WB 退休并冲刷年轻指令，随后 Core 只计空等待时钟，不运行取指或 MEM。
局部使能的外部/软件/定时中断可唤醒；目标特权级及委托允许时进入 ISR，否则继续执行 WFI 后的指令。
CSR 立即数使用 uimm、不读取同编号寄存器，六种形式的 x0/零 uimm 读写抑制共用 CSR 端口实现。
FENCE/FENCE.I 在 WB 保守串行化，等待先前 CPU 访存完成并清除预取状态，不发出较年轻访存。

流水线沿用同一 M/S/U 状态和 PMP。取指及 MEM 访问检查有效权限，CSR/xRET 在 WB 改变状态后冲刷前端，
因此用户态不能使用机器态指令、访问未授权物理区间或通过 Cache 命中越权。
用户 ECALL 为 cause=8，按 medeleg 进入 M/S 陷入寄存器组，经 MRET/SRET 返回；MPRV 仅改变数据访问的有效权限。
监督态委托、TVM/TW/TSR 与 Bare 模式边界见[平台文档](../../../docs/SYSTEMC_PLATFORM.md#监督态异常委托与返回)。
SFENCE.VMA 在 WB 检查权限、清空 TLB 并冲刷前端；错误路径上的 SRET/SFENCE.VMA 不提交。

取指、译码、执行与访存故障保存在所属指令中，只在它成为 WB 最老指令时报告。
错误路径故障可被分支冲刷。在 HostException 模式中，宿主收到故障时，较老指令已退休，故障及更年轻指令没有架构写回；
流水线清空并保留故障 PC。再次 tick 会重新取指执行，允许宿主修复页表/内存后重试。
失败的 WB 拍不增加时钟或退休计数，但重新填充流水线需要实际周期。
构造 Core 时可选择 `FaultHandling::MachineTrap`：WB 的同步故障先于年轻 MEM 请求处理，
写入 mepc/mcause/mtval 和中断使能栈，清除流水线，下一拍从 mtvec.BASE 取指。
ECALL/EBREAK 同样串行化，但产生异常而不退休；MRET 可在软件修正 mepc 后恢复执行。
独立教学 Core 默认使用 HostException，保留宿主故障重试实验。MMU 的访问位/TLB 等微状态不回滚。

## 观察与验证

`last_pipeline_cycle()` 给出当拍开始的五级 PC、转发来源、数据/访存/系统停顿、
冲刷、重定向、中断入口、同步 `trap` 现场与退休事件。IF PC 表示取指候选，停顿/冲刷时不一定实际读取。
`pipeline().if_id()/id_ex()/ex_mem()/mem_wb()` 返回拍后的只读锁存状态。
`last_retirement()` 沿用 RV32 的 PC、机器字、寄存器/内存写事件接口。
其 `cycles` 是距上次退休的间隔，包含填充/停顿；不是单条指令独占的延迟。
`last_cycle().execution` 区分执行模式；旧 phase/signals/micro_address 与 `control_unit()`
的微拍统计只描述多周期路径，不能用于读取流水级。

```bash
bash tests/run_pipeline.sh
bash tests/run_control_units.sh
bash tests/run_cpu_cache.sh
bash tests/run_io.sh
```

流水线测试逐拍核对占位/退休与 load-use，覆盖关闭转发、最新生产者、访存等待中转发、
总线背压/短响应、错误路径 MMIO、x0 load、同步故障、CSR、中断/撤销/MRET 和页表修复。
固定种子生成两批 2400 条候选指令，分别开/关转发；每次退休均与独立指令级参照、
原硬布线和微程序核心比较 PC、32 个寄存器与全部本地数据内存。
整机 Cache 与 I/O 测试另覆盖真实 DIMM、HDD/NVMe/NIC、DMA 与 Guest ISR。
