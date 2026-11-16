# 内存系统组件接线示例

这里负责组装独立硬件对象。Cache 本身的组件、独立示例和读写主线全部在
[`soc/cache/`](../../soc/cache/README.md)，不依赖本目录、MMU 或 DIMM。

另有 [`cpu_cache.cpp`](cpu_cache.cpp) 使用真正的 RV32 `SoC` 执行 LW/SW，展示
`CPU → MMU → Cache → 总线 → Memory → DIMM`。运行 `bash tests/run_cpu_cache.sh`，
可比较冷读与命中读取的总线事务、主存访问和模拟拍数，详情见 [CPU 数据 Cache](../../docs/cpu-cache.md)。

| 对象 | 自己负责的行为 | 与外界连接的边界 |
|---|---|---|
| MMU | 查 TLB、查页表、权限检查、A/D 更新、故障寄存器、拼接物理地址 | 输入虚拟地址，输出物理地址；页表物理读写端口由外层提供 |
| Cache | 选组、组内 Tag 比较、替换、装入、写策略、写缓冲 | 输入物理地址；下级只需提供读/写字节回调 |
| 内存控制器 | 系统物理窗口片选、减基址、按字宽搬运字节 | 将读写请求转成 DIMM 的局部字访问 |
| DIMM | Rank/Bank/行/列译码、存储阵列、行缓冲与命令状态 | 接收模块局部地址，不保存 Cache 的组或路 |

## MMU 与 Cache

[`mmu_cache.cpp`](mmu_cache.cpp) 通过最简单的字节数组提供下级存储，展示：

```text
虚拟地址 → MMU 转换并检查权限 → Cache 接收物理地址 → 下级字节数组
```

外层先调用 `mmu.translate()`，成功后才调用 Cache；权限失败不会继续访问 Cache。
MMU 与 Cache 对象不互相持有，Cache 无需知道虚拟地址或页表。

## Cache 与 DIMM

[`cache_dram.cpp`](cache_dram.cpp) 的接线仅发生在本示例中：

```cpp
colab::dimm::DIMM dram({2, {4, {4, {64, 128, 4}}}});
colab::soc::controller::Memory controller(dram, 0x10000000);
colab::soc::Cache cache(controller.reader(), controller.writer(), {128, 4, 64});
```

Cache 是 32 KiB、4 路、128 组、64 B 行。示例把五个主存块映射到第 20 组，
打印前四块占据四路、同块其他字节命中、第五块替换脏行并写回 DIMM 的过程。

```text
物理地址 → Cache [Tag / 组号 / 块内偏移]
                  ↓ 未命中或下级写
        内存控制器 [DIMM 窗口片选、减基址]
                  ↓ 局部字节偏移 → 字地址/字内字节
                DIMM [Rank / Bank / 行 / 列]
```

这两套映射各自独立：Cache 组相联决定主存块可以占据哪些缓存行；
DRAM 译码决定数据位于哪个 Rank、Bank、行和列。DRAM 行缓冲与 Cache Line 的大小不必相等。
下级换成另一层 Cache 或测试数组，只需在外层替换回调，Cache 内部的放置和替换逻辑不变。

对象按 `DIMM → 控制器 → Cache` 的顺序构造，使用期间保持存活。
在销毁它们前调用 `cache.flush()`，完成脏行和写缓冲中的请求。

## 虚拟存储器硬件访问流程

[`virtual_memory.cpp`](virtual_memory.cpp) 将独立对象接成：

```text
虚拟访问 → MemoryAccess 逐页预检查 → MMU/TLB/权限/A-D
                                          ↓ 成功后返回物理片段
                            Cache → 内存控制器 → DIMM
MMU 页表端口 ──独立的物理读写回调──→ 内存控制器 → DIMM
```

`simulation/memory_access.hpp` 是外层接线助手，负责跨虚拟页拆分和目标数据请求。
MMU 不知道 Cache，Cache 也不知道页表；物理页表中保存真正的 8 字节 PTE，
没有用另一个软件 map 代替主存中的页表。示例页表端口直接接控制器，目标数据走 Cache。
也可在外层将页表端口接到物理 Cache；MMU 的组件无需改变。

对应图片中五种可能组合：

| 情况 | TLB | Page | 目标数据 Cache | 示例中的主存读请求数 |
|---|---|---|---|---|
| 1 | 命中 | 驻留 | 命中 | 0 |
| 2 | 命中 | 驻留 | 未命中 | 1，装入数据块 |
| 3 | 未命中 | 驻留 | 命中 | 1，读页表 |
| 4 | 未命中 | 驻留 | 未命中 | 2，页表与数据块 |
| 5 | 未命中 | 不驻留 | 暂停、未发出目标数据请求 | 1，读页表后报告缺页 |

表中预设 A=1，并且不含页表状态回写、脏行写回、写缓冲等待等额外操作；
一次控制器的块读请求在 DRAM 下层会拆成多个字读，并不等于一个真实 DRAM 周期。
这里把缺页时的数据 Cache 状态记为“未访问”；教材常将该格简写为缺失。
即使某个物理页框恰有旧缓存副本，也不能绕过缺页和权限检查把它当成这个虚页的有效数据。
TLB 命中蕴含 Page 驻留的前提是外层修改 PTE 后正确失效旧 TLB 项。

情况 5 展示故障 VA、外层修复映射及对同一 VA 的重试。完整换出/页框复用过程由
`tests/virtual_memory_system_test.cpp` 验证：先清理旧物理页的缓存，再保存旧页、写入新内容、
修改 PTE/失效 TLB，最后重试原访问。测试中以字节数组模拟后备页，库中没有加入操作系统换页策略。

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror examples/memory_system/virtual_memory.cpp -o /tmp/vm-demo
/tmp/vm-demo
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/virtual_memory_system_test.cpp -o /tmp/vm-system-test
/tmp/vm-system-test
```

## 运行与验证

在仓库根目录执行：

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror examples/memory_system/mmu_cache.cpp -o /tmp/mmu-cache-demo
/tmp/mmu-cache-demo
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror examples/memory_system/cache_dram.cpp -o /tmp/cache-dram-demo
/tmp/cache-dram-demo
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/mmu_cache_test.cpp -o /tmp/mmu-cache-test
/tmp/mmu-cache-test
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/cache_dram_test.cpp -o /tmp/cache-dram-test
/tmp/cache-dram-test
```

上述 `mmu_cache`、`cache_dram`、`virtual_memory` 组合仍是同步功能模型。
`cpu_cache` 整机示例则通过现有共享总线按拍等待 Cache 缺失与写穿响应；
这些示例都未模拟真实 DRAM 电气延迟，RV32 取指仍使用独立 ROM。
