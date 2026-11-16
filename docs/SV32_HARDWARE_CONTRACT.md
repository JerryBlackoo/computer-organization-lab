# Sv32 硬件阶段与后续内核启动合同

本轮交付是 **SystemC 硬件 + 实际编译程序验收**，不是微内核、ROM/磁盘引导或图形 Power On。
正式平台仍为 `simulation/systemc/platform.hpp`，复用既有 CPU/Cache/DIMM/设备和 Adapter。

## 已选的板型/工具链

- 单 hart，RV32 小端，32 位物理平台；支持 RV32I、六种 Zicsr、Zifencei，M/S/U 及本文约定的特权子集。
- 实验编译参数：`--target=riscv32 -march=rv32i_zicsr_zifencei -mabi=ilp32 -mno-relax -msmall-data-limit=0`；不提供 M/A/C/F/D。
- 默认 RAM `[0,0x40000)`，四个 64KiB Rank，共 64 个 4KiB 页；默认模拟周期 50us，CLINT timebase 为 20000Hz。
- 共享 RAM 取指的正式平台使用硬布线多周期核心。微程序多周期核心也有独立总线执行测试。
  原 ROM 流水线/独立本地 ROM 实验不支持 Sv32；显式拒绝不支持的组合，不把它描述为分页机器。
- 不提供完整 Linux/SBI/FDT/通用板级启动 ABI，不宣称架构认证或精确 DDR/PCIe 频率。

## 物理地址与外设

| 区域 | 起点 | 长度 | 合同 |
|---|---:|---:|---|
| RAM | 0 | 0x40000（默认四 Rank） | 字节/半字/字，自然对齐；教学 DIMM |
| CLINT | 0x02000000 | 0xc000 | MSIP、MTIMECMP、MTIME，RV32 对齐 32 位访问 |
| HDD | 0x10000000 | 0x100 | 现有控制器、DMA，外部 IRQ 源 1 |
| 中断控制器 | 0x10001000 | 0x100 | Pending/Mask/Claim/Complete，不是完整 PLIC |
| NVMe | 0x20000000 | 0x2000 | 已有队列/doorbell/DMA 教学模型，外部 IRQ 源 2 |
| NIC | 0x30000000 | 0x100 | 现有 NIC/DMA，外部 IRQ 源 3 |
| Timer | 0x40000000 | 0x100 | 教学计时器，外部 IRQ 源 4 |
| UART | 0x40001000 | 0x100 | FIFO，外部 IRQ 源 5 |

所有 MMIO 只接受对齐 32 位访问；空洞/非法宽度/目标失败转为原访问类型的访问故障，
并不通过扩读或读改写掩盖副作用。设备寄存器细节沿用各模型和 [平台说明](SYSTEMC_PLATFORM.md)。
外部源为电平；Mask 位 1 表示屏蔽，Claim 读具有副作用，先确认设备源再 Complete，否则可以重入。
CLINT 是单独 MTIP/MSIP 路径；NVMe/HDD/NIC 的 DMA 使用物理/IOVA，不接受 CPU 虚拟地址，保护由 IOMMU 负责。

## 装载与复位

- 宿主预装小端静态 RISC-V ELF32 ET_EXEC；PT_LOAD 的 `p_vaddr == p_paddr`，均须落在 RAM。
  拒绝动态链接、错误 ABI 标志、重叠、越界和非法入口；不应用动态重定位。
- 冷上电先重建易失控制对象、清空 RAM，再装入文件数据，因此 BSS 清零；最后保持两拍复位再释放。
  ELF 的 `e_entry` 是初始 M-mode PC；没有内置固定 ROM 启动向量。裸二进制默认从 0x8000 装入/执行。
- 热 Reset 保留 RAM（包括此前的 BSS/表项/写入）和存储介质，不重载 ELF；启动软件须自行重建易失软件状态。
  CPU/GPR/CSR/TLB/走表、Cache、总线/TLM 队列、DMA、设备命令/IRQ 重置，已提交的写不回滚。
  DIMM 行/bank/统计和 HDD 主轴/机械位置不是上述易失控制重建范围；不承诺电气/机械全新上电，见平台说明。
  断电取消在途事务；再次冷上电清 RAM、装回镜像，NAND/磁介质保留。
- GPR 本实现复位为 0；启动汇编仍必须设置 sp/gp。无约定的 boot-info 指针；不能假设 a0/a1 是 Linux 入口参数。
- Pause 冻结整个有效硬件域；裸 `sc_start` 仍可让自由运行 clock 波形变化，但组件不推进。
  Step 是 **一个系统周期**，不是一条退休指令；TLM 等待/陷入周期可以零退休。控制命令只在 sc_start 挂起时执行。

## M/S 交接与时钟中断

M-mode 启动软件负责 mtvec、栈、PMP、medeleg/mideleg/mie、mepc/mstatus，然后 MRET 进入 S；
S 不能直接访问 M CSR。页表不能替代 PMP；走表隐式读使用 S-mode PMP，最终读写执行仍重新检查 PMP。
示例为测试特权控制流而开放全部 PMP，并非生产级的 M 固件隔离策略；内核固件须按自身内存预算收紧。

MTIME 每有效周期加 1，mtime>=mtimecmp 时 MTIP 电平有效。RV32 读高/低/高并重试；
更新 compare 用低=-1、高、低的顺序。将 compare 移到未来清 MTIP；没有定时器 Claim/Complete。
现有 supervisor smoke 以 M 固件处理 MTIP、置 mip.STIP，再由 S 请求 M 清 STIP；这是实验固件服务，**不是标准 SBI**。
S 内核需保留/约定这套服务或提供自己的 M stub，不能直接把 CLINT MTIP 当 STIP。

## Sv32/Svade

- SystemC CPU 显式选 `RV32Core::Translation::Architectural`：Bare 及正常 M-mode 为 PA=VA，
  即使宿主启用了 `cpu.mmu()` 教学页表也不调用它；MPRV 数据访问按有效特权处理。
  独立教学 CPU 构造默认仍为 `Translation::Teaching`，保留旧 8 字节 PTE callback API；
  这不是架构 Sv32/Bare，二者不可混称。不支持把 Architectural 选择用于旧 ROM 流水线模式。
- `satp.MODE=0` Bare（其余字段归零）；MODE=1 Sv32，9 位 ASID、22 位 PPN。
  `satp` 写入本身不隐式刷新；同 ASID 根/映射复用必须由软件执行合适的 SFENCE。
- 两级、每级 1024 个 4 字节 PTE；4KiB 和 4MiB 叶。超页必须 PPN[0]=0；VA 的页内/低级索引不丢失。
  V=0、R=0/W=1、底层仍指针、非叶 U/A/D 保留位触发页故障；RSW 忽略，非叶 G 向下继承。
- U/S、R/W/X 检查在 TLB 命中也生效。SUM 仅允许 S 对 U 页的数据访问，S 永不能执行 U 页；MXR 只放宽读取 X 页。
  MPRV/MPP 只改变 M 的数据有效特权，取指仍按 M；离开 M 的 xRET 清 MPRV。
- 采用 **Svade 式软件维护 A/D**：A=0 的 I/L/S 或 D=0 的 store 产生页故障，不修改 PTE；load/fetch 不要求 D。
  本轮 firmware 实际修复 PTE、执行 SFENCE 后重试原指令；不实现 Svadu/ADUE 可选切换。
- 原访问类型的页故障 cause 为 12/13/15；走表 RAM/PMP 失败或最终 PA 被拒则为访问故障 1/5/7。
  `xepc` 是故障指令/取指目标的 VA，`xtval` 始终原始 VA，不泄漏成 PTE 的物理地址。
- 本平台不执行非对齐 load/store：自然对齐检查先于数据地址翻译，故同时不对齐和缺页时先报
  load/store-address-misaligned（4/6）。这是规范允许的平台优先级，不是所有 RISC-V 的统一要求。
  RV32I 无 C，JAL/JALR/已采纳分支目标须四字节对齐；错误在跳转指令处产生 instruction-address-misaligned（0），
  不提交 link/PC，也不对错误目标开始新一轮走表。
- Sv32 允许 34 位 PA，本平台只寻址 32 位；高根/叶 PPN不可达时明确访问故障，绝不截断别名。
  页表必须位于 RAM，不能以有副作用的 MMIO 充当 PTE 存储。
- 16 项合并 I/D TLB，ASID/global；所有 SFENCE.VMA 地址/ASID 变体保守全刷（含 global），是允许的 over-fence。
  U 执行 SFENCE 非法；TVM=1 拦截 S 的 satp 读写和 SFENCE。SUM/MXR 改变立即重检，不依赖 fence。
- 走表通过数据 Cache → 请求队列 → TLM → 原 Fabric/内存控制器 → DIMM，不用宿主页表 callback 替代。
  缓存命中的 PTE 不必每次访问 DIMM；统计区分页表事务提交数与 TLB 命中数，不虚报 DRAM 访问。

依据：[Supervisor 20260120](https://docs.riscv.org/reference/isa/v20260120/priv/supervisor.html)、
[Machine 20260120](https://docs.riscv.org/reference/isa/v20260120/priv/machine.html)。

## 分页 ELF 实验预算（保留低 64KiB 布局）

`paged.ld` 固定 M/S/U 代码分别在 0x8000/0x9000/0xa000，各有链接时页边界断言。
根表 0x1000、二级表 0x2000；0x3000 为替换数据页，0x5000 为 A/D 测试页；
用户数据 0xc000、S 栈 0xd000～0xdfff、U 栈 0xe000～0xefff、M 初始栈顶 0x10000。
用户 VA 为 0x4000a000（代码）、0x4000c000（数据）、0x4000e000（栈）；该实验仍只使用低 64KiB，四 Rank 扩容不移动原有代码、页表或栈。
此前 `SV32_STAGE_RESULTS.md` / `HARDWARE_CLOSEOUT_RESULTS.md` 的 64KiB 是当时验收快照，不代表当前默认容量。
S 的 identity 超页仅表示页表权限，超出实际 RAM/MMIO 图的最终请求仍失败。

运行 `bash examples/compiler_smoke/check_paged.sh /path/to/systemc_machine`：
客体自己创建表、M→S→U 执行编译 C、十次精确陷入、A/D 修复重试、SFENCE 重映射、UART `V!`、
冷/热启动均得签名 `0x53563332`。验证器独立比较物理内存与 ELF 符号，而不是仅看 banner。
`systemc_sv32_lifecycle` 另验证在途走表 Pause、Reset、断电跨长模拟时间、重新上电，无旧响应污染。
`status` 和 VCD 增加 satp、走表次数、PTE 事务读数、页面故障数，便于复核真实路径。

## 验收边界

1. **本阶段硬件**：以本轮自动回归实际结果为准，见 [阶段记录](SV32_STAGE_RESULTS.md)。
2. **编译程序 smoke**：证明取指/访存、特权、隔离、计时器和外设链，不是 OS 调度或 IPC。
3. **实际微内核**：尚未验收。需 OS 给出确定的 ELF、内存/页表预算、启动 ABI、M 固件服务与 IRQ 确认约定，
   再验证真实内核入口、隔离任务/调度/IPC；本轮不代写整个 OS。

GUI 点击 Power On 仍未实现；当前控制入口为 CLI。PCIe 是原有事务/仲裁教学模型，不是完整 PHY/TLP/credit/replay，
DIMM 亦非真实 DDR 电气时序模型。本轮未做所有读写提交边界 × 复位、全部 IRQ 同 delta 竞争或多核一致性的穷举认证。
