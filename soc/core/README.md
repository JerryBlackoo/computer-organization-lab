# 指令周期、阶段与时钟脉冲

`RV32Core` 支持五级流水线和原多周期执行，整机 `SoC` 默认启用流水线。
级间锁存、转发、访存等待、中断与精确提交见 [RV32 流水线](pipeline/README.md)。
多周期下可选择硬布线或微程序控制；接口、微地址与周期见 [双控制器实验](../../docs/control-unit.md)。
本页以下说明对应原 16 位教学 `Core`。

指令周期是一条指令从开始取指到执行完成的全部时间。按功能可粗分为取指和执行，
也可细分为 `IF → ID → EX → MEM → WB`。粗分中的“执行”包含取指之后的各阶段，
细分中的 `EX` 只表示运算、有效地址或分支目标的计算。

本核心采用**单总线、多周期、非流水**实现。同一时间只推进一条指令；一个功能阶段
可以占多个时钟周期，不同指令也可以跳过不需要的阶段。五种阶段不意味着每条指令固定五拍。

## 三个层次怎样对应

| 层次 | 本实现中的含义 | 代码 |
|---|---|---|
| 指令周期 | 从本条指令的 IF 第一拍开始，至 WB 或 HALT 完成 | `TimingState::instruction_cycle`，`instruction_complete` |
| 指令阶段 | IF 取指、ID 译码/读寄存器、EX 执行/计算地址、MEM 数据访存、WB 写回 | `InstructionStage` |
| 节拍/时钟周期 | 当前阶段内的一拍，安排一次微操作或等待 | `TimingState::stage_beat`，拍级状态 `Phase` |

`TimingState::instruction_beat` 是本条指令内连续编号的节拍；`stage_beat` 在进入新阶段时
从 1 重新编号。`clock_pulse` 则是核心成功提交的全局有效时钟脉冲编号，不随指令重置。
例如第一条 ADD 的第 8 拍是 `instr 1/T8 ID/T1`（读第一个源寄存器），第 9 拍是
`instr 1/T9 EX/T1`（运算），第 10 拍是 `instr 1/T10 WB/T1`。

教材中的“机器周期—节拍—脉冲”可以描述更早的分层时序结构；本模型由统一系统时钟
直接驱动，每个时钟周期对应一个节拍，没有额外的机器周期计数器。功能阶段也不等于
固定长度的机器周期。

## 时钟怎样推动现有部件

```text
Simulator::step()
  └─ Core::tick()                         一个系统时钟周期 / 节拍
       ├─ TimingGenerator                给出当前阶段、拍级状态和节拍编号
       ├─ ControlSignalGenerator         根据拍级状态和已译码指令产生控制信号
       ├─ SingleBusDatapath / Decoder / MMU 从旧状态计算本拍输入与结果
       ├─ ControlLogic                   决定下一拍状态
       └─ 有效时钟脉冲（周期末的采样边沿）
            ├─ 锁存寄存器、已译码指令、待完成读请求
            └─ TimingGenerator::on_clock_pulse() 推进节拍、累计完成指令
```

这里不模拟高低电平持续时间或一拍内多个工作脉冲。寄存器写使能、Read、Decode 等是
本拍控制信号；它们决定有效时钟边沿提交什么。所有输入读取旧状态，下一状态在边沿
统一提交。因此 ALU 的结果先进入 Z，下一拍才能经内部总线写回 GPR。

等待主存的节拍仍消耗时钟脉冲。若取指地址检查或 MMU 翻译抛出异常，核心寄存器、
节拍编号和完成指令数都不推进，可在外层修复后重试同一拍；MMU 的故障寄存器、页表
访问位等模块自身的诊断/访问状态遵循 MMU 的规则。停机后 `Core::tick()` 不再提交，
核心脉冲数保持不变。`Simulator::cycles()` 统计成功返回的 `step()` 调用，若宿主在
停机后继续调用，它仍会计数；通常应像 `main.cpp` 一样在 HALT 后停止推进。

`CycleTrace::phase` 和 `timing` 描述刚由本次脉冲提交的那一拍；`next_phase` 和
`Core::phase()` 描述下一拍。`stage_complete` 在阶段末拍或指令完成时为真，
`instruction_complete` 只在 WB 写回或 EX 停机完成时为真，二者不等同。

## 当前 ADD/SUB 的指令周期

| 指令内节拍 | 阶段内节拍 | 拍级状态 `Phase` | 本拍动作与已有部件 |
|---|---|---|---|
| T1 | IF/T1 | FetchAddress | PCout、MARin：PC 经单总线送 MAR |
| T2 | IF/T2 | FetchRead | Read：MAR 经 MMU 翻译并检查执行权限，发出取指请求 |
| T3 | IF/T3 | FetchWait1 | 等待主存，无内部总线输出 |
| T4 | IF/T4 | FetchWait2 | 等待主存 |
| T5 | IF/T5 | FetchWait3 | 等待主存 |
| T6 | IF/T6 | FetchWait4 | 主存响应经外部数据通路写 MDR |
| T7 | IF/T7 | FetchLatch | MDRout、IRin；同时组合译码，独立增量通路执行 PC ← PC + 2 |
| T8 | ID/T1 | ReadOperand | GPRout(rs1)、Yin：保存第一个操作数 |
| T9 | EX/T1 | Execute | GPRout(rs2)，ALU 读取 Y 和总线；结果写 Z，SF/OF/CF/ZF 写 FR |
| — | MEM | — | 寄存器加减法没有数据访存，直接跳过 |
| T10 | WB/T1 | WriteBack | Zout、GPRin(rd)：结果写回，指令完成 |

当前取指端口从 T2 发出读请求到 T6 接收数据共占五拍，IF 合计七拍。
为严格对应图 5.6 的 C1～C10，T7 锁存 IR 后，模拟器计算 IR→CU 的组合译码输出，
准备 T8 的控制。`decoded_` 只是这一组合输出的软件缓存，不是额外的硬件寄存器。
这里没有读取“尚未更新的 IR”、增加 MDR→CU 旁路或独立译码拍。
阶段标签仅用于观察功能，不要求每个组合功能单独占一拍。`Phase::Decode` 保留给独立
控制实验，正常指令序列不经过它。

由于只有一条内部总线，第一个源寄存器在 ID 阶段读入 Y，第二个源寄存器在 EX 拍
经总线送入 ALU 的 B 端。这是现有数据通路的具体安排，不把教材中 ID 读寄存器的
概括理解为必须在同一拍读完所有源寄存器。FR 在 EX 更新，GPR 在 WB 更新。

```text
ADD/SUB：IF[7 拍，末拍并行译码] → ID[读寄存器 1 拍] → EX[1 拍] → WB[1 拍] → 下一条 IF
HALT：   IF[7 拍，末拍并行译码] → EX[1 拍，停机]
```

ADD/SUB 共 10 拍，HALT 共 8 拍，属于变长指令周期；指令编码本身仍固定为 16 位。
HALT 在译码后不再读操作数，在 EX 阶段完成停机，没有 MEM 或 WB。

## MEM 与其他扩展的边界

IF 访问指令存储器不算数据访存 MEM。`InstructionStage::Memory` 定义了 MEM 的概念，
但当前 ISA 只有 ADD、SUB、HALT，控制逻辑尚无进入 MEM 的路径。
Cache、内存控制器与 DIMM 已有独立实现，Core 的取指端口目前仍使用固定延迟的程序数组，
没有把这些独立部件当成已经接入的核心数据访存通路。

增加 LOAD/STORE 时，需要按已有数据通路补齐有效地址计算、MEM 读写控制和等待/响应；
LOAD 再进入 WB，STORE 通常在 MEM 完成，无须寄存器写回。分支更新 PC、中断响应也尚未实现。
这些阶段不应通过空跑一拍来冒充功能已经实现。

运行根目录的 `main.cpp` 可逐脉冲观察 `instr/T`、`IF/ID/EX/WB`、阶段内节拍、
微操作、控制信号、PC/MAR/MDR/IR/Y/Z 的锁存结果以及指令完成边界。
例如 IF/T7 末尾显示 `[IF -> ID]`，ID/T1 末尾显示 `[ID -> EX]`；
写回和停机拍显示 `[instruction complete]`。演示的 ADD、SUB、HALT 共消耗 28 拍。

## 与 RV32 的关系

这里的 7/1/1/1 拍安排只属于 16 位 `Core` 单总线实现。
RV32 流水线并行推进多条指令，不共用这一套 Y/Z 暂存或单总线控制序列。
可复用部件与两种执行组织见 [数据通路说明](datapath/README.md)。

## 旧 16 位 Core 的 FR 标志

`registers/fr/fr.hpp` 的 `Flags` 按 `sf, of, cf, zf` 顺序存放 bool；后两项追加，
保留 `Flags{sf, of}` 的位置初始化兼容性，新项默认 false。FR 新建时四项全 false；
没有额外 reset 方法，`write({})` 可整组清零。ZF 初始 false 不代表此前执行过零结果运算。

16 位 ALU 的 `F = uint16_t(A ± B)`；SF=F[15]，OF 保持原有有符号溢出规则。
加法 CF=`uint32_t(A)+B > 0xffff`；减法 CF=`A < B`，**1 表示无符号借位**，不是“无借位”。
ZF=`F == 0`，判断的是截断后的结果，与 OF/CF 独立。例如 `0xffff+1` 得 0、CF=ZF=1、SF=OF=0；
`0x7fff+1` 得 0x8000、SF=OF=1、CF=ZF=0；`0-1` 得 0xffff、SF=CF=1、OF=ZF=0。

ADD/SUB 的 Execute 拍由 FRin 一次锁存全部四项，先求 next state，再在 Core 时钟边沿提交；
FRin 关闭、写回拍和 HALT 均保持旧值。CycleTrace 中的 ALUResult 自动携带四项，
`main.cpp` 保留原 `SF=... OF=...` 显示并追加 `CF=... ZF=...`。
这些是旧教学 Core 状态，**不向 RV32 添加 FR、carry/zero ISA 标志或相关新指令**。
