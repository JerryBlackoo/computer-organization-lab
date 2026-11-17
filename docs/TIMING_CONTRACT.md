# 时序合同与参考配置

本文件记录项目当前可验证的时序，不把 C++/SystemC 的组合函数等同于门级延迟。
指令语义采用 RV32IM 与项目文档中的特权子集；没有经过 RISC-V 架构兼容认证。

## 参考配置

系统级主线选 `simulation/systemc/platform.hpp`：单 hart、硬布线多周期 RV32Core，
共享 DIMM，经 I/D Cache、TLM 总线和 MMU 取指/访存；平台的默认 50 μs 周期是教学缩放，
不是目标芯片频率。微程序、多周期本地 ROM/RAM 及旧流水线继续作为独立对照配置。
SystemC 每个有效时钟依次推进总线、部件、IRQ、CPU 和观测相；delta 周期不计额外 CPU 拍。

一拍表示一次 `RV32Core::tick()` 或平台有效 CPU 相。状态在边沿更新；组合译码、寄存器读、
普通 ALU、PMP 检查可在拍内完成。暂停、复位、等待总线与 fault 的实际语义分别见
`docs/SYSTEMC_PLATFORM.md` 和 `docs/control-unit.md`。不能由 C++ 对象大小推导控制字宽。

## 可复现实验基线

下表只适用于独立 Core 的**本地理想指令/数据存储，且没有总线等待或中断**。
周期从开始取指到该指令退休；流水线从空流水线开始。普通算术指令的流水线吞吐量不由单条延迟推断。

| 指令类别 | 硬布线 | 微程序 | 说明 |
|---|---:|---:|---|
| 普通算术、跳转、store | 10 | 通常 11 | 微程序 LUI/AUIPC/JAL、JALR 可走更短序列 |
| load | 15 | 16 | 本地功能存储配固定读等待 |
| 条件分支 | 10 | 12 | 微程序额外条件下址 |
| 普通无符号乘除 | 42 | 43 | 启动 1 拍，32 个迭代拍；含取指和写回 |
| MULHSU 修正 | +0～1 | +0～1 | 无符号乘数最高位为 1 时增加 ALU 修正拍 |
| 有符号除法修正 | +0～2 | +0～2 | 非零商、余数按符号分别经 ALU 修正 |
| 除零/有符号商溢出 | 10 | 11 | 启动拍得到规定结果；不触发算术陷入 |

可实际测量而不依赖手抄数据：

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror examples/timing/baseline.cpp -o /tmp/colab-timing
/tmp/colab-timing
```

SystemC 的 Cache、总线拥塞、页表读取、DMA 和设备响应可能延长上述时间，且背压无承诺的
有限上界；不能把理想 Core 数字当成整机最大周期。`step_instruction` 的 cycle limit 是
宿主超时保护，并不是 ISA 或硬件的最大执行周期。

## 一致性和证据

- 乘除单元保存 X、P/R、Q、计数和 busy/done；每轮复用 32 位 ALU，最多一次加或减，
  输出再经组合移位并在该拍末锁存。额外符号修正各占一拍。
- 硬布线与微程序等待 done，流水线保持 EX/ID/IF，同时允许更老指令退休；中断及较老故障
  可冲刷未退休乘除，返回后重试。详细时序和逐拍命令见 [RV32M](rv32-muldiv.md)。
- SystemC 的 Sv32 每次 CPU 访问最多完成一个 PTE 读取；独立教学配置仍可选择同步走表。
  VCD 的 `walk_busy`、`walk_level`、`walk_pte_address` 和 `walk_pending` 显示当前走表状态。
  默认采用 Svade：A/D 未满足时产生页故障，由软件修复；它不是硬件自动置 A/D。
- 可选 [DRAM 命令模型](DRAM_TIMING.md) 实现 PRE/ACT/CAS 和 tRP/tRCD/tCAS；Cache 命中可选
  一拍或两拍。默认保留旧时序，需显式启用逐拍 DRAM。TLM、Bare 教学 MMU、Cache 阵列及 DIMM
  仍不是门级或完整 JEDEC 时序模型。
  项目的阶段性进度以实现和测试为准，不能把目标合同写成已交付功能。

回归入口：`tests/run_control_units.sh`、`tests/run_pipeline.sh`、
SystemC CTest（含 `systemc_platform_muldiv`、`sv32_translation` 和可选 compiler smoke）。
