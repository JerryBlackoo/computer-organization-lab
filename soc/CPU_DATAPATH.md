# 数据通路：16 位单总线与 RV32

保留图 5.6 对应的 16 位单总线教学核心；整机使用 RV32 数据通路。
RV32 的多周期与五级流水线共享 32 位基础部件和同一个指令译码器。
原先依赖 MIPS 指令格式的专用单周期执行、控制译码、立即数扩展和跳转模块已移除。

| 部件/组织 | 实现 | 作用 |
|---|---|---|
| 16 位单总线 | `cpu16/datapath.hpp`、`cpu16/bus.hpp` | Y/Z 暂存，三态总线，C1～C10 微操作 |
| 32 位基础部件 | `rv32/datapath/dedicated/` | 与指令编码无关的 ALU、逐位乘除功能单元、双读寄存器堆、程序/数据存储 |
| RV32 多周期 | `rv32/datapath/rv32.hpp` | 接收硬布线或微程序控制线，跨拍保存 MAR/MDR/IR/A/B/ALUOut |
| RV32 流水线 | `rv32/pipeline/pipeline.hpp` | IF/ID、ID/EX、EX/MEM、MEM/WB 保存各指令状态，转发/停顿/冲刷 |
| RV32 译码 | `shared/decoder/decoder.hpp` | 同一 ADD/SUB/ADDI/LW/SW/BEQ 与 CSR/MRET 子集，I/S/B 字节立即数 |

## 单总线的 C1～C10

`colab::soc::Core` 连接 `SingleBusDatapath` 和 `NoC`，每次 `tick()` 推进一拍。
`NoC` 没有跨拍状态；无输出时返回空值，多输出即使数值相同也抛出冲突异常。
`BusSource` 是控制单元的一选一编码；`BusDrivers` 可直接验证各 out 三态门。

| 节拍 | 动作 | 控制 |
|---|---|---|
| C1 | PC→MAR | PCout、MARin |
| C2～C6 | MEM(MAR)→MDR，共五拍 | Read、MDRin 持续有效，C6 数据就绪后锁存 |
| C7 | MDR→IR | MDRout、IRin；并行组合译码 |
| C8 | GPR[rs]→Y | GPRout(rs)、Yin |
| C9 | Y+GPR[rd]→Z，标志→FR | GPRout(rd)、ALUop=000（Add） |
| C10 | Z→GPR[rd] | Zout、GPRin(rd) |

教材两地址加法对应 `Instruction{Opcode::Add, rd, rs, rd}`；原有三地址教学编码仍可用。
Y、Z 和 FR 只在使能时更新；ALU 读取旧 Y，总线只能由一个部件驱动，可以向多个输入扇出。
`SingleBusDatapath::evaluate()` 返回所有寄存器的下一状态，调用者在边沿统一提交。
PCin 和内部总线→MDR 的 MDRin 通路也已接入。

图中忽略 PC 自增；可运行的 16 位示例在 C7 使用独立增量通路令 PC←PC+2。
这项示例约定不占 ALU 或内部总线。主存固定延迟端口仍由程序数组提供，经过现有 MMU
的 Execute 权限检查；未连接 Cache/DIMM。`SystemBusRequest` 给出 MAR 地址、MDR 写数据、
Read/Write，供外部端口接线。现有 Core 自动序列只执行 ADD、SUB、HALT；底层 Write
请求不等于已经增加单总线 STORE 指令。

## RV32 的两种执行组织

`RV32Core` 默认保留原多周期接口，`SoC` 默认选择流水线。
两者的架构约定相同：x0 固定为零、32 位加减回绕、LW/SW 四字节对齐，
BEQ 目标为当前 PC 加已恢复的字节偏移。区别在内部锁存和控制时序。
流水线的内存握手、顺序提交、中断与异常见 [流水线说明](rv32/pipeline/README.md)；
两种多周期控制器的微操作见 [控制器实验](../docs/control-unit.md)。

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror soc/rv32/datapath/example.cpp -o /tmp/rv32-datapath-demo
/tmp/rv32-datapath-demo
bash tests/run_pipeline.sh
```

同一段 `addi x1,0,10; addi x2,0,20; add x3,x1,x2` 在硬布线多周期下用 30 拍，
流水线下用 7 拍，结果均为 x3=30。这里的周期数属于该模拟数据通路，不代表真实芯片频率。
