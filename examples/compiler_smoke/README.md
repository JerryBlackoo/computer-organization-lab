# 编译器生成的 RV32 C 程序验收

这个实验验证真实的 C 编译结果能沿着 ELF → 指令 Cache → CPU → 数据 Cache/TLM → DIMM/UART 运行。
它是硬件与工具链联调程序，尚不是微内核。

在项目根目录执行，需 Clang、LLVM LLD（或 Rust 附带的 rust-lld）和 Python 3：

```bash
bash examples/compiler_smoke/check.sh /tmp/colab-systemc-build/systemc_machine
```

通过 `CLANG`、`LD_LLD` 指定工具的可执行文件路径。未指定 LLD 时先检查 PATH，再检查现有
Rust 工具链中的 `rust-lld`；脚本不安装任何工具。输出放在 `/tmp/colab-compiler-smoke`，
也可通过 `check.sh` 的第二个参数指定目录。

`start.S` 设置 SP=0x10000 并调用 C 函数。C 代码从 RAM 读取四个 volatile 输入，循环调用
不可内联的 `mix()`，执行移位、掩码、异或、比较和累加；保存结果到物理地址 0x60 并向 UART 写 `C`。
保存前将结果的低 24 位经 volatile 字节与半字数组往返，并检查有符号 byte/short 读取，
覆盖 SB/SH/LBU/LHU/LB/LH；UART 保持对齐的 32 位写入。
编译器负责生成调用、返回、callee-saved 寄存器和栈读写，验证器检查返回后的 SP 恢复到 0x10000。
Python 使用独立的整数计算得到预期签名 **0x9935510a**，并核对实际 DIMM 内容与 UART 输出。

启动汇编还会安装机器态异常入口。C 执行一次 ECALL，处理程序保存工作寄存器，
将 mcause/mepc/mtval 写入 0x70/0x74/0x78，将 mepc 加 4 后 MRET 返回。
C 核对 cause=11、精确的 ECALL 地址和 mtval=0；验证器同时要求异常次数为 1、SP 恢复且签名正确。
执行 ECALL 前，C 通过高/低/高读取 CLINT mtime，用 RV32 安全分步写法设置 mtimecmp，
开启 MTIE/MIE 后执行 WFI。定时中断处理程序将比较值移到未来、记录计数及 mepc，
不修改中断返回 PC，然后 MRET。C 要求中断次数为 1、保存的 mepc 等于 WFI 后的标签地址。
验证器要求实际 WFI 等待周期大于 0，最终仍得到原签名 `0x9935510a`。
这组默认实验验证机器态异常硬件及保存/恢复路径；下面另有用户态/PMP 验收，二者均不是微内核。

脚本使用 RV32IM+Zicsr+Zifencei/ILP32、无压缩指令、无链接松弛、无小数据 GP 依赖的配置。
机器态 C 程序还用 volatile 操作数执行有符号/无符号除法、余数和乘积高低位；这些运算必须正确才提交原签名。
链接脚本将正文和数据放在 0x8000 附近，留出顶部栈空间；ELF 可在控制台中单独加载：

```bash
/tmp/colab-systemc-build/systemc_machine --interactive --elf /tmp/colab-compiler-smoke/program.elf
```

也可将实验加入 CTest：

```bash
cmake -S examples/systemc -B /tmp/colab-systemc-build \
  -DCOLAB_FETCH_SYSTEMC=ON -DCOLAB_COMPILER_SMOKE_TEST=ON
cmake --build /tmp/colab-systemc-build --parallel 2
ctest --test-dir /tmp/colab-systemc-build -R systemc_compiled_c --output-on-failure
```

编译器可能选择不同的等价指令序列；最终签名和恢复后的状态才是验收条件。
机器态实验还用真实汇编执行六种 CSR 读改写，逐一核对旧值及最终 mscratch，并执行 FENCE/FENCE.I。

## 用户态 C 程序与 PMP 隔离

```bash
bash examples/compiler_smoke/check_user.sh /tmp/colab-systemc-build/systemc_machine
```

同一 Clang/LLD 构建脚本选择 `user` 变体，使用 `user_start.S`、`user_main.c` 和 `user.ld`。
默认输出 `/tmp/colab-user-smoke/program.elf`；也可传第二个输出目录参数。
启动汇编设置机器态栈/mtvec/PMP，将 mepc 指向 C 入口，再通过 MRET 进入 U-mode。

| 范围 | 用途 | U-mode 权限 |
|---|---|---|
| 0x8000～0x8fff | 机器态启动、异常处理、保护数据 | 禁止 |
| 0x9000～0x9fff | C 用户代码 | R/X |
| 0xa000～0xafff | C 用户数据和栈 | R/W |
| 其他地址及设备 MMIO | 内核/设备保留区域 | 禁止 |

用户程序从自己的数据/栈计算数值，执行一次 ECALL，机器态处理程序通过串口输出 `U` 并返回 0x55。
用户程序也会执行 FENCE 和 FENCE.I，验证这两条屏障在 U-mode 可用。
随后用户程序尝试读取 mtvec、读取 UART、覆盖已预热缓存的机器态数据、跳入机器态代码，
硬件依次产生 IllegalInstruction、LoadAccessFault、StoreAccessFault 和 InstructionAccessFault。
处理程序通过 mepc 跳过或修复故障返回点；用户程序继续运行，并再次 ECALL 提交最终签名。
处理程序通过 mscratch 切换到独立机器态栈并保存工作寄存器，最终输出 `!` 并停留在机器态循环。

验收要求六次异常的 cause 序列为 `8,2,5,7,1,8`，PC 对应 ELF 中的故障指令/被拒绝取指目标，
MPP 都记录原 U-mode；签名为 `0x50524f54`，受保护数据仍为 `0xcafebabe`，
非法 UART 读取不消费预先注入的 `Q`，复位清空 PMP 配置。
该实验加入同一 `COLAB_COMPILER_SMOKE_TEST` 选项下的 `systemc_compiled_user` 测试。

这里采用无分页的 M/U + PMP。它验证用户态隔离和特权异常硬件，不包含调度、地址空间管理或微内核服务。

## 监督态 C 程序与 M/S/U 嵌套陷入

```bash
bash examples/compiler_smoke/check_supervisor.sh /tmp/colab-systemc-build/systemc_machine
```

构建脚本的 `supervisor` 变体使用 `supervisor_start.S`、`supervisor_main.c` 和 `supervisor.ld`，
默认输出 `/tmp/colab-supervisor-smoke/program.elf`，同样接受第二个输出目录参数。
它使用 M-mode 固件、S-mode 处理程序和真正编译的 S/U C 函数验证硬件：

1. MRET 进入 S-mode，S 通过 ECALL 请求 M 固件设置 CLINT 比较值，然后执行 WFI。
2. CLINT 产生 MTIP，M 处理程序确认计时器并写 mip.STIP；MRET 后进入已委托的 S 定时处理程序。
   S 再请求 M 清除 STIP，经 SRET 恢复 WFI 后的指令。验收核对 M/S 保存的两个 PC 都等于 ELF 中的返回标签。
3. SRET 进入 U C 程序；用户 ECALL 交给 S，S C 函数再以 ECALL 调用 M 服务，结果依次返回 S/U。
   三个特权级分别使用顶部为 0x10000、0xf000、0xd000 的栈。
4. 用户读取机器 CSR 被委托给 S 的非法指令处理程序；处理程序保存实际 PC/机器字并跳过，用户继续计算并提交签名。

预期签名 `0x53555052`，六次同步异常（S → M 三次、U → S 三次）、两次中断（MTIP、STIP），
UART 输出 `S!`。验证器独立计算服务结果/循环校验值，核对 ELF 故障地址、SPP/MPP、
WFI 确实等待、两套陷入寄存器以及复位后的委托/监督态状态。
此实验加入 `COLAB_COMPILER_SMOKE_TEST` 下的 `systemc_compiled_supervisor`。

```bash
python3 examples/compiler_smoke/verify_supervisor.py \
  --machine /tmp/colab-systemc-build/systemc_machine \
  --elf /tmp/colab-supervisor-smoke/program.elf --trace /tmp/supervisor
```

波形中 `privilege=3/1/0` 分别表示 M/S/U，`scause/sepc/stval` 显示 S-mode 陷入。
此实验的 satp 固定为 Bare，PMP 有意允许全部物理空间以测试特权控制流；
它不提供 S/U 之间的页级隔离，也不是通用 SBI 或微内核实现。物理保护拒绝另由上述用户态实验及 S-mode 单测验证。


## Sv32 分页 C 程序（使用低 64KiB，不是微内核）

```bash
bash examples/compiler_smoke/check_paged.sh /path/to/systemc_machine /tmp/colab-paged-smoke
python3 examples/compiler_smoke/verify_paged.py --machine /path/to/systemc_machine \
  --elf /tmp/colab-paged-smoke/program.elf --trace /tmp/paged
```

`paged_start.S` 在客体 CPU 上清理并建立两级页表，MRET 进入 S，再 SRET 进入虚拟地址 `0x4000a000` 的真实 C 程序。
用户 C 使用映射的数据/栈，返回结果 8。十次精确异常覆盖 SUM、MXR、U 访问 S 页、写 RX 代码、
未映射页、A/D 缺位修复后重试、禁止取指与 ECALL；Python 按 ELF 符号独立核对每次 cause/stval/PC。
S 通过修改 PTE + SFENCE.VMA 将同一 VA 从物理 `0xc000` 切到 `0x3000`，结果从 42 变为 99。
验证器核对物理内存、串口 `V!`、签名 `0x53563332`、走表统计与冷/热两次启动；每次预算 160000 个系统周期。
默认整机现为四 Rank、256KiB；本示例的表格、代码、栈仍全部位于低 64KiB 内，保持原链接布局和签名。详见 [启动合同](../../docs/SV32_HARDWARE_CONTRACT.md)。
