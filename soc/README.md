# CPU 与系统部件导航

本项目包含不同的 CPU 实验；同在一个仓库不表示运行时连接到同一颗 CPU。

| 目录 | 作用 | 从哪里开始 |
|---|---|---|
| `cpu16/` | 16 位、单内部总线、非流水的多周期教学 CPU | [Core](cpu16/core.hpp)、[总线](cpu16/bus.hpp)、[数据通路](cpu16/datapath.hpp) |
| `rv32/` | 32 位 RISC-V CPU；支持多周期和五级流水线 | [RV32Core](rv32/core.hpp)、[多周期数据通路](rv32/datapath/rv32.hpp)、[流水线](rv32/pipeline/pipeline.hpp) |
| `shared/` | 两套实验目前复用或交织的控制、译码、MMU 和教学代码 | [控制器](shared/control_unit/control_unit.hpp)、[译码器](shared/decoder/decoder.hpp)、[MMU](shared/mmu/README.md) |
| `cache/`、`controller/` | 系统 Cache 和控制器部件 | [Cache](cache/README.md)、[内存控制器](controller/memory/README.md) |

`shared/` 表示目前的代码复用边界，不表示每个文件都能独立于两套 CPU 使用：例如共同控制器同时包含 16 位及 RV32 的控制路径。类和命名空间仍保留原有名称；代码的运行行为没有因目录迁移改变。

仓库根目录 `main.cpp` 演示 16 位 `Core`。它使用内部程序数组模拟取指，尚未连接 Cache、DIMM 和外设。`SoC` 使用 RV32，默认选择流水线并连接数据 Cache 与系统互连；其指令取自独立 ROM。SystemC 平台使用 RV32 多周期模式，通过独立指令/数据 Cache 连接共享内存。不同入口的配置不能互相推断。

根目录 `dimm/` 和 `devices/` 是系统内存及设备模型，`rtl/` 是独立 RTL 实验，`tests/` 是验证程序；它们不因目录独立就自动接入 16 位 CPU。两套 C++ CPU 的具体数据通路见[对照说明](CPU_DATAPATH.md)。
