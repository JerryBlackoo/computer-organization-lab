# 垂直型微指令学习模式

多周期 `RV32Core` 提供三种控制方式：`Hardwired`、水平型 `Microprogrammed`、垂直型 `VerticalMicroprogrammed`。可在创建时选择，也可在**机器指令边界**调用 `select_mode()` 切换；流水线仍只支持硬布线。

```cpp
CPU cpu(program, CPU::Mode::VerticalMicroprogrammed);
cpu.step_instruction();
cpu.select_mode(CPU::Mode::Microprogrammed); // 一条指令完成之后切换
cpu.step_instruction();
```

垂直型有自己的控制存储器和 μIR，每条为固定 **48 位**（C++ 用 64 位整数存储，最高 16 位必须为零）。它的微操作码经译码产生数据通路的控制信号；本实现使用水平 ROM 的内容作为汇编输入，转换成垂直编码后存入独立 ROM，运行时从垂直 μIR 译码，不是读取水平控制字执行。取指、操作数、结果、异常等仍使用现有 RV32 数据通路。

| 位 | 字段 |
|---|---|
| 4:0 | 微操作码 μOP，译码为一组控制信号 |
| 21:5 | 参数：访存宽度/符号、ALU 功能、来源、写回来源、PC 来源、比较方式 |
| 40:22 | 顺序控制：3 位下址方式、8 位目标、8 位备选目标 |
| 47:41 | 保留，必须为零 |

例如 ADD 的执行部分分别使用 `ReadOperands`、`Alu`、`WriteRegister` 三个 μOP。读写的 `rs1`、`rs2`、`rd` 来自机器指令 IR；它们没有重复存进垂直微指令。`Alu` 的参数指定加法，译码器输出 `alu_in`、ALU 选择等控制。`WriteRegister` 是本数据通路的复合动作：还包括 PC 更新和退休；有效地址动作也可同时锁存 ALUOut 和 MAR。它因此是适应该模型的编码微操作，并不声称每个 μOP 都是不可再分的单一寄存器传送。

默认微程序的动作及拍数保持一致，便于逐拍对照：每个垂直 μOP 能展开原水平微指令的一组兼容控制信号，因此此实验不展示“垂直型必然更慢”。常见教材中垂直型微程序更长，取决于微操作粒度及数据通路，不是编码方式单独决定的。

使用修改过的水平控制存储器构造 CPU 时，首次切到垂直型会把它转换成垂直 ROM。如果包含当前 μOP 表无法表达的控制组合，会抛出 `invalid_argument`，切换不会完成；原硬布线/水平型仍可使用该映像。独立 RTL 微程序教学核也支持两种格式：实例化时设置 `VERTICAL=1`、`ROM_FILE="rtl/microcoded/rv32_vertical.hex"`，或使用默认 `VERTICAL=0` 和水平 ROM。RTL 是综合时选择，C++ 模拟器支持在指令边界运行时切换；RTL 的 64 位调试端口在垂直模式输出低 48 位有效、高 16 位为零。RTL 的指令与接口范围见 [二进制微指令文档](binary-microcode.md)。

```bash
bash tests/run_control_units.sh
g++ -std=c++17 examples/control_unit/binary_trace.cpp -o /tmp/binary_trace
/tmp/binary_trace horizontal
/tmp/binary_trace vertical
```

跟踪会显示当前 μPC、64 位水平或 48 位垂直 μIR，以及操作数、ALU、寄存器写使能。垂直模式的当前字可通过 `cpu.control_unit().vertical().micro_ir_word()` 读取；只有构造或切换到垂直模式后该接口才可用。

运行 `bash tests/run_rtl_microcoded.sh` 可分别检验两种 RTL 的指令执行、暂停、ROM 位修改和与 C++ 的逐拍对照。两份 ROM 均从 C++ 汇编器生成并在测试中校验一致。
