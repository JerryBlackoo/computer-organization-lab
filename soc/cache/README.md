# Cache：物理地址缓存

入口是 `cache.hpp` 中的 `colab::soc::Cache`。这是独立于 MMU 的对象，
只接收 **32 位物理字节地址**，通过主存读写回调装入或写回数据。
默认配置是 16 组、每组 2 路、每行 32 字节，总数据容量 1 KiB。

## 独立使用与阅读顺序

整个 `soc/cache/` 目录只依赖自身组件和 C++ 标准库，可以单独复制出去编译运行。
`Cache` 对象只管理缓存内部状态，下级存储由构造方传入两个字节读写回调：

```cpp
colab::soc::cache::Config config{128, 4, 64};
colab::soc::Cache cache(read_lower, write_lower, config);
auto value = cache.read_byte(physical_address);
cache.write_byte(physical_address, value);
cache.flush();
```

`read_lower` 和 `write_lower` 可以连接简单数组、另一层缓存或外部存储接口；
对象自己不选择下级类型。只看 Cache 原理时，从 [example.cpp](example.cpp) 开始：
示例只用一个字节数组，展示同组两路、块内命中、LRU 替换和脏行写回。

阅读代码时，先看 `cache.hpp` 的 `read()` / `write()` 主线，再看：

1. `address_decoder/`：物理地址拆成主存块号、组号、Tag 和偏移。
2. `set/` 与 `line/`：组内各路比较有效位和 Tag，选出数据所在路。
3. `replacement/`：组满时选出被替换路；`fill()` 写回旧脏块并装入新块。
4. `write_buffer/`：下级写请求排队、满时等待、逐次完成。

```text
物理地址
   ↓
拆分 Tag / 组号 / 块内偏移
   ↓
选择组 → 对该组各路执行 valid + Tag 比较
   ├─ 命中：选中该路 → 按块内偏移读取或修改数据 → 更新 LRU
   └─ 未命中：优先空行，否则选 LRU 行
                ↓
          脏行先写回原物理块
                ↓
          装入目标整块，写入 Tag/valid → 完成本次访问
```

## 组件与功能

| 组件 | 文件 | 保存的状态 / 职责 |
|---|---|---|
| Cache 控制对象 | `cache.hpp` | 组织查找、装入、替换、脏行写回；执行读写策略和跨行拆分；对接下级存储、维护统计 |
| 地址译码 | `address_decoder/address_decoder.hpp` | 拆分 Tag、组号、块内偏移，重建被替换块的原物理地址，报告各字段位宽和容量 |
| Cache 组 | `set/set.hpp` | 保存各路；对各路执行有效位与 Tag 比较，选择命中路；优先选择无效行 |
| Cache 行 | `line/line.hpp` | 保存 valid、dirty、Tag 和完整数据块 |
| 替换策略 | `replacement/replacement.hpp`、`replacement/lru.hpp` | 每组独立维护 LRU、FIFO、随机或 LFU 的替换状态；只有组满时才选择牺牲行 |
| 写缓冲 | `write_buffer/write_buffer.hpp` | 有限容量 FIFO，保存待完成的下级写请求；满时等待最早写完成，失败保留请求 |
| 配置与接口 | `types.hpp` | 配置组数、路数、行大小、替换和写策略、写缓冲；定义访问记录、容量报告和统计 |

## K 路组相联放置规则

主存按与 Cache 行同样大小的块作**逻辑划分**，映射计算是：

```text
主存块号 j = 物理字节地址 / line_bytes
Cache 组号 = j % sets
Tag        = j / sets
块内偏移   = 物理字节地址 % line_bytes
```

一个主存块只能进入算出的那一组，但可以占该组任意一路。同一块内的不同字节共享组号和 Tag。
例如 `0x1200` 和 `0x1234` 在 64 B 行配置下同属块 `0x48`，只在块内偏移上不同。
Cache 命中时按该路的数据直接返回，不调用下级接口；未命中时读取块起始地址处的**完整一行**。
组满时替换该组内的一路，脏行先写回由**旧 Tag + 原组号**重建的旧物理块地址。

组数和行字节数必须是 2 的幂。`ways=1` 表示直接映射，`sets=1` 表示全相联，
一般的多组多路配置表示组相联。默认地址字段宽度是 Tag 23 位、组号 4 位、块内偏移 5 位。
软件循环模拟各路比较器，返回的路编号对应硬件的数据选择通路；不模拟门电路的并行时延。

## 替换策略

`config.replacement_policy` 可选：

| 策略 | 选择规则 |
|---|---|
| `LRU`（默认） | 淘汰最久未访问的行；`lru_rank(set, way)` 中 0 表示最近使用 |
| `FIFO` | 淘汰最早装入的行；命中不改变装入先后 |
| `Random` | 用每组可复现的 xorshift32 状态选一路；`random_seed` 控制仿真复现 |
| `LFU` | 淘汰累计访问次数最少的行，同频时按 LRU；`frequency(set, way)` 可观察次数 |

所有策略都优先用无效行。装入时 LFU 次数清零，紧接着完成的本次访问计数一次；
64 位计数器饱和后保持最大值。LRU 和 FIFO 通过有限顺序表维护相对次序，没有时间戳回绕问题。
例如四路 LRU 中五个同组块反复按 `1,2,3,4,5` 访问，会持续缺失，即图片中的冲突抖动。

## 写命中与写缺失

| 选项 | 行为 |
|---|---|
| `WriteBack`（默认） | 写命中更新行数据并置脏，替换或显式 `flush()` 时整行写回 |
| `WriteThrough` | 写入时同步更新下级存储和缓存，不产生脏行 |
| `WriteAllocate`（默认） | 写缺失时先装入整行，再按写策略修改目标字节 |
| `NoWriteAllocate` | 写缺失时直接写下级，不装入缓存；写命中仍遵守当前写策略 |

四种组合都支持。读缺失始终装入整行。装入和写回使用块对齐的地址；
写穿或不按写分配的直接写入只传递本次修改的字节，保留周围数据。
写分配采用图片脚注所述的“先装入，再在 Cache 中更新”的方式；是否马上写下级 由写回/写穿策略决定。

## 写缓冲

默认 `write_buffer_entries=0`，下级写同步完成。设置成正数后，写穿、非写分配的写、
脏行写回会先放入有限 FIFO。请求拥有字节副本，不会因 Cache 行后来被覆盖而丢失。

- `drain_one_write()`：宿主推进最早的一次下级写，空队列返回 false。
- `drain_writes()`：完成全部待写请求。
- `pending_writes()`：观察当前队列长度。
- 缓冲满时先完成最早请求，再接收新请求，统计一次 `write_buffer_stalls`。
- 读缺失装入行前保守地排空写缓冲，确保读到本核之前写出的最新数据；没有实现缓冲转发。
- `flush()` 和 `invalidate()` 返回前确保所有相关写请求已落入下级。下级写失败时队首保留，可重试。

启用缓冲后，写 API 成功表示缓存已更新或请求已接收，下级稍后更新。
这是可由模拟器逐步推进的功能队列，不会启动后台线程，也不把一次推进解释为一个真实存储周期。
有写缓冲的写穿行仍不置脏；最新数据由有效行及待写队列共同保有，结束时必须排空。

## 对外接口

- `Cache(read_memory, write_memory, config)`：构造并连接下级存储。
- `read(pa, size)` / `write(pa, bytes)`：按字节序列读写，自动拆分跨缓存行访问。
- `read_byte(pa)` / `write_byte(pa, value)`：单字节快捷接口。
- `flush()`：将所有有效脏行写回，保留缓存内容。
- `invalidate()`：先写回所有脏行，再清除有效位。
- `clean_range(pa, size)`：写回与物理范围相交的脏行并排空写缓冲，保留有效行。
- `invalidate_range(pa, size)`：先完成上述写回，再使相交行失效；供外层在页框复用或 DMA 前维护缓存。
- `line(set, way)`：只读观察有效位、脏位、Tag 和数据。
- `statistics()`：读写命中/未命中、装入、替换、脏行写回次数。
- `last_access()`：最近一个成功块访问的物理地址、主存块号、组、Tag、偏移、命中路、被替换块与脏行写回情况。
- `address_decoder()`：检查地址字段及容量。

读回调为 `Bytes(physical_address, byte_count)`，应准确返回请求长度；写回调为
`void(physical_address, bytes)`，正常返回表示写入成功。Cache 会拒绝越过 32 位地址上界的访问。
回调引用的下级存储必须在使用 Cache 期间保持存活，外部直接修改下级存储后须维护缓存一致性。

统计以访问的**块**为单位；一次跨三块的读 API 调用会产生三次命中/未命中判断。
空读写不计数。主存回调失败会向上传递异常；脏行写回失败时不会被新行覆盖，
跨行操作已完成的早期块不会因后续块失败而回滚。构造时分配各行的字节存储，
析构时不调用下级存储，结束模拟前需要显式 `flush()`。
范围维护按整行处理，部分相交也覆盖该整行，其他行保持有效；0 长度不执行维护。
这些接口只解释物理地址，页大小、虚拟地址和页替换策略由外层决定。

## 命中率、耗时估计与容量

`statistics().hit_rate()` 按读写合计计算 `hits / (hits + misses)`；`miss_rate()` 返回未命中比例。
没有访问时两者均返回 0。命中/未命中计数记录尝试的块访问；异常不会回滚已经记录的次数。
下级读写次数、字节数以及写缓冲满导致的等待次数也可观察。`writebacks` 统计脏行已交给下级接口/缓冲的次数，
启用缓冲时不能据此断言 下级已完成写入；`memory_writes` 统计实际成功完成的下级写。

`average_access_time(hit_time, miss_penalty)` 使用串行查找的理论式
`命中时间 + 缺失率 × 额外缺失代价`，两项时间须使用相同单位。它不从 Bank 命令计数推导周期，
也不自动加入脏块写回、写缓冲等待或总线竞争的实际延迟。

`address_decoder().capacity_bytes()` 返回纯数据容量；`capacity()` 另外返回逻辑 bit 报告：

```text
行数       = sets × ways
数据位     = 行数 × line_bytes × 8
标记位     = 行数 × Tag 位数
有效位     = 行数
修改位     = 写回策略下每行 1 位；写穿策略下为 0
array_bits = 数据位 + 标记位 + 有效位 + 修改位
total_bits = array_bits + 替换状态位
```

报告采用一种明确的状态编码：LRU 每行 `ceil(log2 K)` 位相对次序计数，FIFO 每行同宽装入次序，
随机每组 32 位状态，LFU 每行 64 位频次再加 LRU 平局状态；单路无需替换状态。
这不是某款 CPU 的最小位数，也不是宿主 C++ 对象大小。写缓冲容量、总线控制逻辑不计入此缓存阵列报告。
例如 32 KiB、4 路、64 B 行、32 位物理地址，字段宽度为 `Tag 19 / Index 7 / Offset 6`，
写回 + LRU 的纯数据为 262144 bit，阵列加标记/有效/修改为 272896 bit，
再加本模型的 1024 bit 替换状态，共 273920 bit。四路的 2 位 LRU 计数是**每行**的位数。

## 独立编译与测试

在仓库根目录执行，以下示例和测试均只引用 Cache 组件和标准库：

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror soc/cache/example.cpp -o /tmp/cache-demo
/tmp/cache-demo
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/cache_test.cpp -o /tmp/cache-test
/tmp/cache-test
```

也可仅复制 `soc/cache/` 到任意位置，直接编译其中的 `example.cpp`，无需其他硬件目录。
整机适配器 `cpu_port.hpp` 单独依赖外部共享总线，不由这个独立示例引入。
纯 Cache 测试用平坦字节数组验证组相联、四种替换、写策略、写缓冲、跨行和地址边界。

MMU/Cache 和 Cache/控制器/DIMM 的接线示例、运行命令位于外层的
[examples/memory_system/](../../examples/memory_system/README.md)。组合测试分别在
`tests/mmu_cache_test.cpp` 和 `tests/cache_dram_test.cpp`，不会被独立 Cache 测试引入。

本对象采用物理索引、物理标记（PIPT），模拟同步功能行为；地址转换、访问权限、
外部设备地址译码和物理存储命令由各自的模块处理。16 位 Core 的取指时序保持原教学模型。

## RV32 整机逐拍接口

`cpu_port.hpp` 的 `cache::CpuPort` 接在 RV32 MMU 的物理地址输出后，复用本对象的行、
Tag 比较、替换与统计。它用无副作用的 `contains()` 决定是否发整行读事务，收到完整响应后
才调用 `read()` 装入并返回数据。写穿同样先等主存确认，再调用 `write()` 更新 Cache。
下级回调只消费已确认的数据，不自行推进总线。

整机采用写穿、不按写分配、无写缓冲；读命中不发总线事务，MMIO 不进 Cache。
`snoop_write()` 只更新已驻留字节，不分配、不触及替换/CPU 统计；它要求写穿且无写缓冲，
不能用于写回所有权协议。组合方式、DMA 交错处理与运行入口见 [CPU Cache 说明](../../docs/cpu-cache.md)。
