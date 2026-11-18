# 两套 TLB 的可配置组相联组织

TLB 组织是本模拟器的实现选择，RISC-V ISA 不要求 TLB 必须采用某种路数或替换策略。
本仓库有两条独立翻译路径：正式 SystemC CPU 使用 `Sv32`，另一教学 `MMU` 使用自定义单级 8 字节 PTE。
配置共享几何定义，不共享表项、权限逻辑、统计或替换状态。

## 配置与兼容

`soc/shared/mmu/tlb/geometry.hpp` 定义 `mmu::TlbConfig{entries, ways}`：

**总项数 entries = 组数 sets × 路数 ways**，`sets = entries / ways`。
entries/ways 必须为正、整除且 ways 不大于 entries；组数必须为 2 的幂且不超过 20 位 VPN 空间的 2^20。
路数本身不必为 2 的幂，例如 `{6,3}` 是两组三路。非法几何在分配表项前抛 `std::invalid_argument`。
支持一路直接映射、二路、四路，以及 ways=entries 的一组全相联。几何在构造时确定，不能在运行中改写。

两套默认均为 `{16,16}`。旧 `TLB(n)` / `MMU(n)` 仍表示 n 项全相联，包括非 2 的幂容量。
旧构造调用仍有效；新参数追加到 Core/RV32Core 构造末尾，`SoCConfig.tlb` 追加到聚合末尾，不移动原位置字段。
`Sv32::entries()` 现返回只读 vector 而非固定 `array<Entry,16>`，长度为配置总项数；范围遍历、size、下标用法保持不变。

```cpp
namespace mmu = colab::soc::core::mmu;
const mmu::TlbConfig tlb{16, 4}; // 16 项 = 4 组 × 4 路
mmu::TLB teaching_tlb(tlb);
colab::soc::core::MMU teaching_mmu(tlb);
colab::soc::core::Sv32 architectural_tlb(tlb);

using CPU = colab::soc::RV32Core;
CPU cpu({}, CPU::Mode::Hardwired, 256, {}, CPU::Execution::MultiCycle, {},
        colab::soc::core::FaultHandling::MachineTrap, CPU::Translation::Architectural, tlb);
// 独立 RV32 教学 MMU 路径选择 Translation::Teaching；两套内部 TLB 均取得同一几何。
colab::soc::Core old_core({{colab::isa::Opcode::Halt}}, tlb); // 旧 16 位 Core 的教学 MMU

colab::SoCConfig board;
board.tlb = {32, 2}; // 32 项 = 16 组 × 2 路
colab::SoC teaching_soc({0x00000063}, board);
// 正式 SystemC 使用相同配置入口，仍固定为 Architectural 硬布线多周期 CPU：
colab::simulation::systemc::BootImage boot{0x8000, {0x0000006f}, {}};
colab::simulation::systemc::Platform machine("computer", boot,
    sc_core::sc_time(50, sc_core::SC_US), board);
```

分别包含 `soc/soc.hpp` 和 `simulation/systemc/platform.hpp`；SystemC 对象须在内核启动前构造。
`SoCConfig.cpu_execution` 继续只控制旧 ROM SoC，不将正式 Platform 切换为流水线。
配置通过 SoC→RV32Core 或 Platform→CpuModule→RV32Core 接通；reset/off 重建时保留配置。
这不是新的客体 CSR，也未增加 CLI 几何选项。

## Index、Tag 与超级页

设 s=log2(sets)，set=`key & (sets-1)`。每组在只读 entries 视图中占连续 ways 项。

| 路径/页大小 | 索引 key | 比较内容 |
|---|---|---|
| 教学 MMU，4KiB | `VA>>12`，20 位 VPN | 仅该组的 valid + 完整 VPN |
| Sv32，4KiB，level=0 | `VA>>12`，VPN[1:0] | 仅该组的 valid + level=0 + 完整 VPN + ASID/global |
| Sv32，4MiB，level=1 | `VA>>22`，VPN[1] | 仅该组的 valid + level=1 + VPN[1] + ASID/global |

逻辑 Tag 是 key 去除组索引后的高位；模型保留完整 key 进行比较，包含冗余索引位。
Sv32 的 `Entry.vpn` 对普通页保存完整 VPN，对超级页保存 `VPN[1]<<10`，低十位规范化为零。
超级页忽略全部十个 4KiB 子页位，因此同一超级页任何子页都会找到同一候选组。
即使 sets>1024 也如此；此时超级页只能使用其 10 位 key 能选中的组。

查找先探测普通页候选组，再探测超级页候选组，各只比较该页级别的 k 路，不全表扫描。
同一个组可同时保存两种页大小，它们共享 k 路及替换状态；**没有额外一份超级页容量**。
一次查找至多探测两个候选组，不代表最多两项，也不模拟并行比较器或额外查找周期。
若软件没有正确 SFENCE 而保留了重叠的新旧映射，不应依赖优先级；本模型先检查 4KiB 项。

## 替换、失效与生命周期

- 教学 TLB 保留 LRU：命中及同 Tag 更新触碰该组 MRU；填充优先选该组无效项，否则替换该组 LRU。
  `lru_rank(slot)` 返回所属组内的排名，0 为 MRU。`invalidate_page(vpn)` 只检查对应组，flush 清全部 valid。
- Sv32 保留轮转：每组有独立 cursor，每次成功走表填充推进一格，命中不推进；其他组不受影响。
  保留旧逻辑，不优先搜索无效槽；选择失效后可能先替换该组有效项。这会影响命中率，不改变映射正确性。
  普通页/超级页和不同 ASID 竞争同组的同一 cursor。TLB 命中仍重检权限、SUM/MXR、A/D 和最终 PMP。
- `satp` 写入不隐式失效，保留 ASID/global 合同；同 ASID 的根/映射复用需要软件 SFENCE。
- `Sv32::flush(optional<VA>, optional<ASID>)` 保持无参数全刷入口，支持以下选择。存在的 ASID 参数取低九位。

| SFENCE 寄存器 | 失效范围 |
|---|---|
| rs1=x0，rs2=x0 | 全部有效项，包括 global |
| rs1 非 x0，rs2=x0 | 覆盖该 VA 的所有叶，包含所有 ASID 和 global |
| rs1=x0，rs2 非 x0 | 指定 ASID 的非 global 项 |
| rs1、rs2 均非 x0 | 指定 ASID、覆盖指定 VA 的非 global 叶 |

按**寄存器编号**判断是否使用选择条件；非 x0 寄存器内容为零代表指定 VA=0 / ASID=0。
地址不必页对齐，超级页内部任何地址均使整个匹配超级页失效；global 包含从非叶继承的 G。
失效保留替换次序/cursor，不重置统计；每次 flush 调用令 flushes 加一。
所有选择都取消并排空在途走表，避免旧响应在 fence 后重新填充（对无关在途走表可保守取消）。
SFENCE 的权限检查、退休时序与取指串行化保持不变；教学格式无 ASID，CPU 中仍保守全刷教学 TLB。

reset/off 重建会清除两套表项、替换状态、走表及统计；再次上电仍采用已配置的 entries/ways。
Pause 不推进有效硬件阶段，step 仍是一拍而非一条指令。
未增加多核 shootdown、运行时几何切换或 TLB 查找周期/面积模型；I/D 仍共享一套 Sv32 TLB。

## 针对性验证

`tlb_associativity_test` 覆盖非法/默认几何、1/2/4/全相联、三路、组内 LRU、组间隔离、MMU 实际走表、
旧 Core 取指和三种 SoC CPU 执行配置的分页访存。
`sv32_associativity_test` 覆盖组内轮转、普通页/超级页共享组及交错命中、跨子页、ASID/global、权限与选择失效。
它还验证重叠的普通页/超级页一起失效，以及硬布线和微程序 CPU 执行选择性 SFENCE 的实际结果。
`systemc_tlb_associativity_test` 通过实际 CPU 指令、Cache/TLM/DIMM 验证分页访问、超级页跨子页、
四种 SFENCE 选择（含零值寄存器）、satp ASID 切换、global 保留/失效、暂停/热复位/断电冷启动。
CTest 注册 16 项的 1/2/4/16 路及 8 项二路正式 Platform 场景；原有 Sv32/Svade/PMP/设备回归继续保留。
选择性 SFENCE 可减少合法命中的走表次数；统计字段含义不变，不能把旧 over-fence 的次数作为固定硬件合同。
分页 smoke 的可选 VCD 验证对照已验证的冷/热启动状态快照，继续独立核对客体签名、精确异常与九次页故障。
固定分页固件在默认 TLB 下仍有严格的次数断言：19 次 walk、35 次 PTE 请求、9 次页故障。
其末尾按地址 SFENCE 保留了不覆盖目标 VA 的监督态 identity 超级页，故比原全刷少一次 walk 和一次根 PTE 读取。
