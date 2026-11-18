# RTL 实验：多周期 RV32IM

另有 [水平 64 位及垂直 48 位微代码的微程序 RTL 教学核](../docs/binary-microcode.md)：目前只支持 ADD、ADDI、SUB、LW、SW、BEQ 和零等待端口，独立于下述硬布线核心。

`rv32im/core.sv` 是独立的多周期 RTL 核，使用单个在途的 AXI4-Lite 读写事务。
读通道遵循 AR/R 握手；写通道分别保持 AW 和 W，待两者均被接收后等待 B。
`alu32.sv` 是 32 位加减单元，`muldiv.sv` 在 32 个迭代边沿复用该 ALU；
带符号修正可增加 1～2 拍。接口暴露退休记录、调试寄存器和故障信息。

目前它覆盖 RV32I 基本整数、访存、控制流、FENCE 与 RV32M；非对齐访问、非法指令、
ECALL/EBREAK 和 AXI 响应错误通过调试故障端口停机。**它还不是完整特权核**：没有
CSR/中断/MRET/SRET、PMP 或分页，也不能直接运行需要这些功能的整机固件。
本实验不是 SystemC 平台的替代品，也未进行架构认证或物理时序分析。

`rv32im/pipeline.sv` 是另一套五级顺序 RV32IM RTL：独立的 SRAM 风格指令和数据端口，
数据端口支持 ready 背压；RAW 相关等待写回，分支/异常冲刷，乘除占用 EX 并复用 ALU。
它可以同时容纳多条指令；`pipeline_axi.sv` 将两端口仲裁为单个在途 AXI4-Lite 事务，
带地址标签地丢弃已重定向的取指响应。`cached_core.sv` 的 `PIPELINED=1` 可再接入统一 Cache。
流水线仍不支持 CSR、外部中断或分页；故障通过调试端口停机。

`rv32im/cached_core.sv` 将该核与 `axi_cache.sv` 连接：它是阻塞式、统一的 16 项直接映射
写穿 Cache，每行一个 32 位字，写不分配，RAM 地址范围之外直通。外部写须从 `snoop_valid`
通知失效；`invalidate_all` 可整体失效。在途填充遇到失效时不会安装旧数据。它没有多核一致性
协议或独立 I/D Cache；调用方应确保失效顺序符合其总线合同。两个核均不实现 FENCE.I。

`rv32im/sv32.sv` 是单独测试的 Sv32/Svade TLB 与逐级页表遍历单元，**尚未连接到此核**。
它由外部提供有效特权级、PMP 决策和 `satp`；请求须由上游先完成对齐及跨页检查。
支持 4 项全相联 TLB、ASID/global、superpage、SFENCE 筛选和在途读取取消；A/D 位缺失按
Svade 引发页故障，不自动写回 PTE。完整的 CSR、PMP、分页 fault 接线仍需另行实现。

安装 Icarus Verilog、Clang/LLD 和 C++17 编译器后，从仓库根目录运行：

```bash
bash tests/run_rtl_muldiv.sh
bash tests/run_rtl_cache.sh
bash tests/run_rtl_sv32.sh
python3 tests/run_rtl_core.py
python3 tests/run_rtl_pipeline.py
YOSYS=yosys bash tests/run_rtl_synth.sh  # 可选：需要 Yosys
```

第一项用 Python 任意精度整数独立生成边界及随机向量，检查结果和 busy/done 的迭代拍数。
RTL Sv32 测试覆盖两级 walk、权限、TLB 刷新和取消在途请求。核心对照使用随机 AXI 背压，
比较有、无 Cache 的每条 RTL 退休事件、故障、寄存器和测试内存与现有 C++ 核，并包括 Clang 编译的
[`rtl_smoke`](../examples/rtl_smoke/) ELF、字节/半字访存及错误路径。
流水线在 SRAM、AXI 和 AXI+Cache 三种连接方式下对照，包括指令重叠、乘除 busy、
数据端口背压、精确 fault、分支冲刷和随机指令流。
Yosys 检查各核心、Cache 接线及独立 Sv32 的可综合进程转换，并确认核心网表没有乘法、除法或
取模算子；这不等于门级 STA。

若工具位于非标准位置，可通过 `IVERILOG`、`VVP`、`IVERILOG_IVL_DIR`、`YOSYS`、`CLANG`、
`LD_LLD` 指定路径。测试使用临时目录，不安装依赖。
若已有 SystemC 构建目录，也可配置 `-DCOLAB_RTL_TESTS=ON`，再运行 `ctest`；这样会将
RTL 仿真与综合测试加入常规回归，要求上述依赖均可用。
