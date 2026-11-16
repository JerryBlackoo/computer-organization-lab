# Cache 与 DIMM 之间的内存控制器

`colab::soc::controller::Memory` 接收系统的 32 位物理**字节地址**。
它检查 DIMM 地址窗口，减去基址转换为局部地址，再调用现有 DIMM 的字接口。
本工作区的 DRAM 硬件层次现位于 `dimm/`。

```text
CPU/MMU 的物理地址
       ↓
Cache：主存块号 → 固定组 → 组内 K 路 Tag 比较
  命中：直接返回
  缺失/写回：整块请求
       ↓
Memory：窗口片选 → 局部字节偏移 → 字地址/字内字节
       ↓
DIMM → Rank → 各位扩展 DRAM chip → Bank → 行缓冲/存储阵列
```

`Memory(module, base)` 映射第一条 DIMM；`map(base, module)` 可增加不重叠的 DIMM 窗口。
同一 DIMM 不允许重复映射，以免产生未维护一致性的物理地址别名。
物理窗口宽度等于模块的字节容量，窗口不能超出 32 位地址空间，Rank 位宽须为 8 的倍数。
必须先片选再减基址，不会把任意地址直接截成 DIMM 的低位地址。

`read(pa, size)` 返回字节数组；`write(pa, bytes)` 写入字节数组，均使用小端字节顺序。
整个请求必须位于一条 DIMM 窗口内，越界在发出任何命令前报错；可以跨该 DIMM 的 Rank/Bank/行，
一次调用暂不跨多个独立 DIMM 窗口。长度为 0 的访问为空操作。
Cache 行填充也须整行位于有效窗口，常规接线宜将窗口基址和大小对齐到 Cache 行。

按 Rank 字宽合并传输，完整字写不先读，部分字写通过读改写保留其他字节。
`decode(pa)` 只读显示局部字节地址、字地址、字内偏移、Rank/Bank/行/列；
`statistics()` 显示上层请求数、字节数和实际 DIMM 字读写调用数。
`reader()`、`writer()` 生成可直接传给 Cache 的回调。

DIMM 下层已实现行命中时直接 READ/WRITE、换行时 PRE + ACT，以及多 Bank 的独立打开行。
Cache 组号决定主存块在缓存中的放置范围；DRAM Bank/行/列决定数据在模块中的物理位置，
两种映射互不代替。控制器不保存 Cache 的 Tag/Way，也不让操作系统参与逐次替换。

这是同步功能控制器，通过直接回调连接 Cache 和 DIMM，尚未引入 NoC 仲裁、tRCD/tRP/tRAS、
突发传输时序或共享总线带宽模型。现有 Bank 命令计数可验证命令路径，不能当作真实耗时。
DIMM 与控制器必须在所有 Cache 访问、写缓冲排空和 `flush()` 期间保持存活。
示例见 [`../../../examples/memory_system/cache_dram.cpp`](../../../examples/memory_system/cache_dram.cpp)。
