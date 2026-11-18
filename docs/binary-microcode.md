# RV32 定长二进制微指令与数字电路实验

## 范围

`RV32Core` 的多周期微程序模式现将控制存储器和 μIR 实际存为 `uint64_t` 的 64 位控制字，所有已实现的多周期 RV32 微程序均使用这一表示。硬布线模式仍从当前阶段和译码结果生成同一组控制字段；两个模式复用原有数据通路。已有机器码译码器从 32 位 IR 得到操作及寄存器字段。

`rtl/microcoded/rv32_microcoded.sv` 是支持水平及垂直控制字、共享其对应 ROM 映像的**独立可综合教学 RTL**：有物理意义上的二进制 μIR、μPC、IR、控制 ROM、组合译码、寄存器堆、ALU 和写使能。它不是已经制造的物理器件，亦未接入既有 AXI/Cache/SoC。RTL 目前只实现 ADD、ADDI、SUB、LW、SW、BEQ，使用独立零等待指令/数据读端口，只支持对齐 32 位访存；不实现其他 RV32 指令、总线错误/对齐异常、中断、特权、乘除或 Cache。遇到不支持的机器码在分派时置 fault。现有 `rtl/rv32im/core.sv` 则是另一套硬布线 RTL，**尚未改成这个微程序核**。更完整的 RV32 功能目前仍在 C++ 模型和原有硬布线 RTL 各自的范围内。

课设文档的 24 位格式属于其 6 位微地址、特定 ALU 与实验箱的数据通路；不可直接声称与本 RV32 核兼容。本格式参考其“操作控制 + 判断/后继地址”思想，专为项目现有功能设计。王道图中水平型允许直接控制和字段编码，此设计即采用独立使能加互斥字段编码的水平型组织；C++ 模拟器另提供 [垂直型 μOP 模式](vertical-microcode.md)。

## 64 位控制字（bit 0 为最低位）

| 位 | 名称 | 含义 |
|---|---|---|
| 0 | mar_from_pc | PC 写 MAR |
| 1、2 | instruction_read、instruction_to_mdr | 取指读请求、取指数据写 MDR |
| 3、4 | ir_in、decode | MDR 写机器指令寄存器 IR、机器指令译码 |
| 5 | operands_in | 写操作数寄存器 A、B |
| 6、7 | muldiv_start、muldiv_step | 乘除开始／推进 |
| 8、9、10 | alu_in、address_in、condition_in | 写 ALUOut、MAR、条件寄存器 |
| 11、12 | data_read、data_to_mdr | 读数据、读出的数据写 MDR |
| 13、14、15 | memory_write、register_write、pc_write | 内存、寄存器堆、PC 写控制 |
| 16、17 | retire、system | 指令完成、系统状态操作 |
| 19:18 | memory_bytes | `00`=4 字节、`01`=1、`10`=2，`11` 保留 |
| 20 | load_signed | 小宽度读时符号扩展 |
| 25:21 | alu_op | ALU 操作编码，见 `alu_control.hpp` 枚举；`1`=Addu，`3`=Subu |
| 26 | operand_b | `0`=寄存器 B，`1`=立即数 |
| 29:27 | write_back | `0`=ALUOut、`1`=MDR、`2`=高位立即数、`3`=PC相对、`4`=返回地址 |
| 31:30 | pc_source | `0`=顺序、`1`=分支、`2`=立即数跳转、`3`=寄存器跳转 |
| 34:32 | compare | `0..5`=等、不等、有符号小于/大于等于、无符号小于/大于等于 |
| 37:35 | sequence | `0`=μPC+1、`1`=跳转、`2`=指令分派、`3`=按比较结果、`4`=按乘除完成标志 |
| 45:38 | next_address | 无条件跳转或条件为真时的 8 位微地址 |
| 53:46 | alternate_address | 条件为假时的 8 位微地址 |
| 63:54 | 保留 | 必须为 0 |

每个槽位宽 64 位，256 个槽位是 `256 × 64 = 16384 bit = 2 KiB` 的控制字容量。未装入程序的槽位仍属于地址空间；C++ 模型用单独的有效位表标识它们并拒绝访问。附加有效位和调试阶段元数据不计入这 64 位；C++ 构造器用 `Microinstruction` 作为汇编用的字段视图，装入后存储器及 μIR 持有的是二进制 `uint64_t`，不以 `sizeof` 作为微指令长度。

错误或保留的字段编码会在 C++ 装载 `set_raw()` 时拒绝，地址不能被悄悄截断。流水控制支持两个显式目标而非仅一个后继地址，以保留现有比较和乘除完成分支行为。`phase` 是旁路调试标签，不参与编码、控制输出或下地址的选择。

## 一条真实二进制 ADD 微程序

默认 ROM 中（十六进制显示，每行 64 位）：

| μ地址 | 控制字 | 关键字段 |
|---|---|---|
| 16 | `0000000000200020` | bit 5 `operands_in=1`，顺序执行 |
| 17 | `0000000000200100` | bit 8 `alu_in=1`，`alu_op=1`（Addu） |
| 18 | `000000080021c000` | `register_write`、`pc_write`、`retire`；sequence=Jump，next=0 |

控制字中即使不使用 ALU，`alu_op` 默认仍为 Addu，因此 bit 21 可能为 1；这不代表所有微指令都会写 ALUOut，仍由 `alu_in` 等控制。公共取指是 0..6，7 根据真实机器指令选择 ADD 入口 16。C++ 中机器指令先译码为 `Operation::Add`，RTL 子集直接由 IR 的 opcode、funct3、funct7 组合译码分派。寄存器编号来自 IR 的 rs1/rs2/rd，操作使能来自 μIR，两者协同控制寄存器堆。

一拍内，当前 μIR 的操作控制字段提供控制，组合逻辑计算数据和下一地址；有效时钟边沿将新值与下一 μPC/μIR 锁存。等待时保持状态；RTL 的 `enable` 是时钟使能而非门控时钟。分支时下地址选择使用已保存的标志。RTL `fault` 在不支持的指令或无效下址时保持，不承诺实现完整 RV32 异常机制。

## ROM 映像与验证

```bash
# 原有 C++ 硬布线/微程序对照、定长编码和二进制修改测试
bash tests/run_control_units.sh
# 可观察每拍 64 位 μIR 和操作数/ALU/写回使能
g++ -std=c++17 examples/control_unit/binary_trace.cpp -o /tmp/binary_trace
/tmp/binary_trace
# 验证磁盘 ROM 与 C++ 汇编结果一致，并运行可综合 RTL 仿真
bash tests/run_rtl_microcoded.sh
# 可选综合结构检查（需要 Yosys；未含 FPGA/ASIC 具体工艺与时序约束）
yosys -p 'read_verilog -sv rtl/microcoded/rv32_microcoded.sv; hierarchy -check -top colab_rv32_microcoded; synth -top colab_rv32_microcoded -noabc; check; stat'
```

`examples/control_unit/export_microcode.cpp` 从 C++ 控制存储器生成 `rtl/microcoded/rv32_control.hex`。每行 17 位十六进制字符：最左 1 个字符是有效位（0/1），其余 16 个是 64 位微指令；有效位是独立的检查信息，不是微指令位。测试先重新生成并逐字节比较 ROM，防止软件与 RTL 映像静默漂移。

测试覆盖 C++ 二进制字段、保留值、所有已装入字的编解码、直接修改 ALU 位后实际结果变化；RTL 测试覆盖 IR 译码、原始 μIR、ADD/ADDI/SUB/LW/SW、BEQ 成立及不成立、保持、非法指令，以及实际修改 ROM 中的 ALU 字段让 ADD 执行减法。另以 106 拍逐拍对照 C++ 与 RTL 的 μPC、μIR、IR、主要数据通路寄存器、通用寄存器和测试内存。综合不等于实际芯片流片、板级验证、时序收敛或完整处理器认证。
