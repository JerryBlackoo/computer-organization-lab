# DRAM 硬件层次的 C++17 功能模拟

包含关系是 `DIMM → Rank → DRAM chip → Bank → Cell`。每层单独一个头文件，统一入口为
`package.hpp`；`dimm.hpp` 为内存条（DIMM）类。

| 层次 | 实现 | 作用 |
| --- | --- | --- |
| Cell | `cell/cell.hpp` | 一个逻辑 bit；Bank 中将多个位平面打包存放 |
| Bank | `bank/bank.hpp` | 存储阵列、行缓冲、行列地址锁存、译码、数据缓冲和刷新计数器 |
| DRAM chip | `dram_chip/dram_chip.hpp` | 多个 Bank，低位多体交叉编址，各 Bank 独立保存当前激活行 |
| Rank | `rank/rank.hpp` | 多颗同规格 DRAM chip 同地址访问，按位拼接数据，chip 0 提供最低位 |
| DIMM | `dimm.hpp` | 多个同规格 Rank 占据连续地址段，扩展字数 |

术语使用 **rank** 和 **bank**。Rank 数量可配置，默认四个。

## 编译运行

在仓库根目录执行：

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror dimm/example.cpp -o /tmp/dimm-demo
/tmp/dimm-demo
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/dram_test.cpp -o /tmp/dimm-test
/tmp/dimm-test
```

示例展示 RAS/CAS 分时复用地址、OE 高阻、Bank 交叉编址、四颗 x4 芯片组成 16 位字、
Rank 地址边界、小端字节写、行命中/行冲突和刷新扫描。测试还会与独立平坦内存逐次比对，
覆盖全部小模型地址、伪随机读写、非 2 的幂配置、64 位数据、非法命令和边界。

## 对应教材图的 Bank

```text
共享地址引脚 --RAS--> 行地址锁存器 --┐
                                  MUX --> 行译码 --> 存储阵列
刷新计数器 -----------------------┘                  ↕
共享地址引脚 --CAS--> 列地址锁存器 ------> 列译码 --> 行缓冲/感应放大器
                                                     ↕
                                         输入缓冲 / 输出缓冲 --> 数据引脚
```

`BankGeometry{2048, 2048, 4}` 对应 2048 行、2048 列、4 个位平面，容量为
`4M × 4 bit = 16 Mbit = 2 MiB`，行列地址各 11 位，复用 11 根地址引脚。
默认 Chip 只有一个这样的 Bank，整颗芯片恰好对应教材图。

阵列使用 `vector<uint8_t>`，`cells[row * columns + column]` 的低 4 位同时保存
该交点上 4 个 Cell。因此默认 Bank 的宿主阵列占 4 MiB，逻辑存储容量为 2 MiB，
另有 2048 字节行缓冲及对象开销。这里优先保留易读的索引关系，未做跨列位压缩。
`Cell` 表示一个逻辑位，`inspect_cell()` 返回值快照供观察；它不会为每一位分配独立对象。

```cpp
colab::dimm::Bank bank;
bank.ras(100);                     // 行号先进入共享地址引脚，ACT 打开第 100 行。
bank.cas(7, false, true, 0xA);      // 列号后进入相同引脚，/WE=0，写入 4 位。
auto data = bank.cas(7);           // /WE=1、/OE=0，得到 optional<uint8_t>{0xA}。
auto high_z = bank.cas(7, true, true); // /OE=1，返回 nullopt，表示输出端高阻。
bank.precharge();                  // PRE 关闭行并均衡位线的功能抽象。
bank.refresh_next();               // 刷新内部计数器指向的行，然后推进计数器。
```

`activate/read/write` 是对应的直接命令接口；`ras/cas` 是教材选通接口。
一次调用表示一次选通事件，不模拟 RAS/CAS 的持续电平、边沿检测或完整引脚真值表。
CAS 读会更新内部输出缓冲，OE 只决定本次调用是否向外返回数据；`nullopt` 与数值 0 不同。

Bank 的命令约束：

- ACT 和 REF 要求 Bank 已预充电；READ/WRITE 要求已有激活行。
- ACT 读取整行到行缓冲，列译码从行缓冲中选择一个位置的所有位平面。
- WRITE 更新行缓冲和对应阵列位置。行缓冲代表感应放大器，PRE 不负责延迟写回。
- `refresh(row)` 刷新指定行；`refresh_next()` 使用并推进内部计数器，扫描完自动回绕。
  刷新走内部行号，不覆盖外部地址引脚和行地址锁存器，不清除数据。
- `state()`、`stats()`、`row_buffer()` 和 `inspect_cell()` 可观察状态，读取这些调试信息不发出访存命令。

## SoC 内存控制器接线约定

SoC 内的内存控制器已在 [`soc/controller/memory/memory.hpp`](../soc/controller/memory/memory.hpp) 实现，
负责窗口片选、基址转换和按 Rank 字宽搬运数据，可直接接到 Cache 的下级接口。
具体约定见 [控制器说明](../soc/controller/memory/README.md)，以下地址规则也保留在 `dimm.hpp` 的类定义前。

```text
系统物理地址
  -> MemoryController 判断 DIMM 地址窗口，产生片选
  -> 减去窗口基址，得到 DIMM 局部地址
  -> 选择 Rank
  -> 拆出 Bank、行、列，生成相应命令
```

例如，16 位地址总线访问每块 **4K 个地址单元**的模块，若地址单位一致且窗口按 4K 对齐：

```text
 A15          A12 A11                          A0
| 高 4 位：片选 | 低 12 位：模块内地址             |
```

映射在 `0x8000..0x8FFF` 的模块，仅在高 4 位等于 `0x8` 时选中；选中后取低 12 位作为
局部地址。必须先检查片选，直接对任意系统地址取低位会让其他模块的地址发生错误别名。
一般窗口先检查 `physical_address >= base`，再检查 `physical_address - base < window_size`，
这样得到的差值才是有效局部地址。高位截取适用于大小为 2 的幂且按窗口大小对齐的情况。

**4K 是可寻址单元数，地址单位须明确：**

- 4K × 8 bit 模块接字节地址总线：容量 4 KiB，高 4 位选片，低 12 位作局部字节地址。
- 4K × 16 bit Rank 接字节地址总线：容量 8 KiB；对齐窗口用高 3 位 `A15..A13` 选片，
  `A12..A1` 是 12 位局部字地址，`A0` 选择字内字节。
- 4K × 16 bit Rank 接以 16 位字为单位的地址总线：高 4 位选片，低 12 位作局部字地址。

控制器调用 `DIMM::read/write/decode` 时传入局部**字地址**；调用 `read_byte/write_byte`
时传入局部**字节偏移**。本类再从局部地址的高位选择 Rank；同一 Rank 内的位扩展芯片
共享选择和地址，各自连接数据总线的不同位，不通过额外地址位轮流选取。

DRAM 的物理行列复用引脚数还需单独看：图中的 11 根地址引脚分两次接收 11 位行号和
11 位列号，实际提供 22 位片内逻辑地址。不能把“11 根复用引脚”解释成仅能寻址 2K 个单元。
未来接入命令级接口时，由控制器将局部地址拆成 Bank/行/列，并生成 Rank 使能和 RAS/CAS 等信号。

## 多体交叉、位扩展与字扩展

所有 `read/write/decode/encode` 使用模块内的**局部字地址**。一个字的宽度等于整个 Rank 的数据宽度。
Chip 独立使用时，一个字的宽度等于芯片 I/O 宽度。

设 `B` 为每颗 Chip 的 Bank 数，`C` 为每个 Bank 的列数，`R` 为行数：

```text
每个 Rank 的字数 = B × R × C               // 增加 Chip 数只增加位宽
Rank 编号       = DIMM字地址 / 每Rank字数
Rank 内字地址   = DIMM字地址 % 每Rank字数
Bank 编号       = Rank内字地址 % B
Bank 内字地址   = Rank内字地址 / B
列号            = Bank内字地址 % C
行号            = Bank内字地址 / C
```

当各维度为 2 的幂时，可写为 `[rank | row | column | bank]`。也支持非 2 的幂维度，
通过除法和取模实现交叉分配。`decode()` 拆地址，`encode()` 做反向映射。

Rank 中各芯片接收相同的 Bank/行/列地址，不需要额外的 Chip 地址位。例如四颗 x4：

```text
Chip 3        Chip 2        Chip 1        Chip 0
bits[15:12]   bits[11:8]    bits[7:4]     bits[3:0]
    B             E            E             F       --> 0xBEEF
```

配置示例（四个 Rank、四颗 x4 芯片/Rank、四个 Bank/Chip）：

```cpp
#include "dimm/package.hpp" // 相对仓库根目录

colab::dimm::DIMMGeometry g;
g.ranks = 4;
g.rank.dram_chips = 4;
g.rank.dram_chip.banks = 4;
g.rank.dram_chip.bank = {512, 2048, 4}; // 四个 Bank 合计仍为每芯片 4M × 4 bit。
colab::dimm::DIMM memory(g);
memory.write(0, 0xBEEF);
auto word = memory.read(0);
auto address = memory.decode(0); // rank=0, bank=0, row=0, column=0
```

默认 DIMM 使用四个 Rank、每 Rank 四颗 x4 Chip、每 Chip 一个 `2048×2048×4` Bank，
逻辑容量为 `16M × 16 bit = 32 MiB`。仅修改默认 Rank 数，保留独立教学 DIMM 的原有每 Rank 几何。
`dimm/example.cpp` 使用四 Rank 的小阵列，教材缩比例题 `verify_textbook_snippets.cpp` 仍显式选两 Rank。

整机 `SoCConfig.memory` / SystemC 使用另一套显式小几何：四 Rank，每 Rank 四颗 x8 Chip、每 Chip
两个 Bank、每 Bank 64 行 × 128 列；每 Rank 64 KiB，总计 256 KiB，RAM 窗口 `[0, 0x40000)`。
DIMM 相对字节地址从高到低为 `[Rank 17:16 | 行 15:10 | 列 9:3 | Bank 2 | 字内字节 1:0]`。
四 Rank 连续占用 `[0,0x10000)`、`[0x10000,0x20000)`、`[0x20000,0x30000)`、`[0x30000,0x40000)`；
选 Rank 后，片内低位 Bank 交叉规则不变。地址 1045、66581、132117、197653 分别选 Rank 0～3，
其余都是行 1、列 2、Bank 1、字内偏移 1。这里是功能级选择，不新增逐根 CS 信号或 DDR 时序。
`DIMMGeometry.ranks` 仍可显式配置其他数量。
Bank 支持 1～8 位 I/O，Rank 总位宽支持 1～64 位。超出位宽的数据会报错，不静默截断。
行列越界、非法配置和容量计算溢出也会明确报错。

`read_byte/write_byte` 使用小端字节地址，要求 Rank 位宽是 8 的倍数。16 位 Rank 中，
字地址 5 对应字节地址 10、11。字节写是软件读改写，会产生一次字读和一次字写。

## 模拟范围

这是功能级模型，命令调用立即完成。Chip 的便捷读写保留打开的行，行命中时直接访问，
行冲突时执行 PRE + ACT；可通过 Bank 的统计观察不同访问路径。
各 Bank 可以同时保持不同的激活行。Chip/Rank/DIMM 的 `refresh_next()` 给所属每个 Bank
各刷新一行，要求它们事先全部预充电；不会悄悄关闭正在使用的行。

刷新在此模型中维护选择路径、扫描计数器和统计；未模拟电容漏电、丢失数据、保持时间或自动刷新调度。
未加入 tRCD/tRP/tRAS/CAS 延迟、共享命令/数据总线、突发传输、ECC 或 DRAM 请求调度队列。
所以低位交叉目前能验证地址分布和独立行状态，尚不能用于计算并行带宽提升。
现可运行 [`Cache → 控制器 → DIMM 示例`](../examples/memory_system/cache_dram.cpp)：

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror examples/memory_system/cache_dram.cpp -o /tmp/cache-dram-demo
/tmp/cache-dram-demo
```

该链路不改变 DIMM 的存储规则；Cache Line 与 DRAM 行缓冲分别管理不同层级的数据块。
现有 CPU 的五拍取指演示尚未接入这条链路的可变延迟行为。
