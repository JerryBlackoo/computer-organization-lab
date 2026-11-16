# MMU：地址转换与访问保护

入口是 `mmu.hpp` 中的 `colab::soc::core::MMU`。这是一个同步的教学功能模型，
采用 **32 位虚拟地址、32 位物理地址、4 KiB 页、单级页表、全相联 TLB + LRU**。
不绑定 x86、ARM 或 RISC-V 的具体页表格式。
整个 `mmu/` 目录只依赖自身和标准库，可独立复制、编译和运行。

```text
虚拟地址 + 访问类型(R/W/X) + 当前特权级
                    ↓
          拆分：20 位虚页号 / 12 位偏移
                    ↓
                 查 TLB
             命中 ↙    ↘ 未命中
       缓存的页表项      页表遍历单元 → 主存中的页表项
                  ↘    ↙
             驻留状态、页框范围、权限检查
                    ↓ 通过
             更新访问位 A / 修改位 D
                    ↓ 通过
           20 位物理页框号 + 原 12 位偏移
                    ↓
              物理地址 → 独立的 Cache
```

## 组件与功能

| 组件 | 文件 | 保存的状态 / 职责 |
|---|---|---|
| MMU | `mmu.hpp` | 组织整个转换流程，控制分页开关、切换页表、TLB 失效，统计命中、遍历和异常 |
| 地址拆分与拼接 | `address_translator/address_translator.hpp` | 将虚拟地址拆成 VPN 和偏移；将 PFN 与原偏移拼成物理地址 |
| TLB | `tlb/tlb.hpp`、`tlb/entry.hpp` | 表项显式保存 V、VPN Tag、PFN、R/W/X/U、A/D；维护 LRU，执行查找、填充、替换和失效 |
| 页表遍历单元 | `page_table_walker/page_table_walker.hpp` | 保存页表基址和主存读取接口；按 `基址 + VPN × 8` 访问单级页表 |
| 权限检查单元 | `permission_checker/permission_checker.hpp` | 检查页面是否驻留、页框是否合法、用户态是否可访问、本次读/写/执行是否获准 |
| 页表项编码 | `page_table_entry/page_table_entry.hpp` | 明确的 8 字节小端格式；编解码 PFN、P/R/W/X/U/A/D、软件字段，检查保留位 |
| 物理页表端口 | `page_table_port/page_table_port.hpp` | 将物理字节读写回调适配为 PTE 读取与回写；不依赖具体 Cache 或内存对象 |
| 故障寄存器 | `fault_registers/fault_registers.hpp` | 保存最近的故障虚拟地址、访问类型、特权级、原因、发生阶段和页表基址 |
| 页表项、异常和结果 | `types.hpp` | 描述页表项、转换轨迹、跨页片段和异常 |

TLB 槽的 `valid` 表示该槽是否包含有效缓存记录；页表项的 `present` 表示页面是否驻留主存。
**TLB 未命中只意味着要查页表，不等于缺页。** 本模型只缓存驻留且本次权限检查通过的页表项。
TLB 命中取得的是 PFN 和权限信息，仍须根据本次访问类型和特权级重新检查权限。

## TLB 表项的字段与作用

字段单独定义在 [`tlb/entry.hpp`](tlb/entry.hpp)：

| 字段 | 含义 | 使用位置 |
|---|---|---|
| `valid`（V） | 这个 TLB 槽里的映射是否可用 | 必须 V=1 且 Tag 匹配才命中；初始、单页失效、全部清空时 V=0 |
| `virtual_page`（Tag） | 20 位完整虚页号 | 与请求 VPN 比较，本模型采用全相联 |
| `physical_frame`（PFN） | 20 位物理页框号 | 与原页内偏移拼接成物理地址 |
| `permissions.readable/writable/executable` | R/W/X 访问权限 | 快表命中后仍检查本次读/写/取指请求 |
| `permissions.user_accessible` | U，用户态能否访问 | 每次按当前特权级检查，不沿用上次访问的特权级 |
| `accessed`（A） | 页访问状态的缓存副本 | 获准访问时更新，并按配置回写 PTE |
| `dirty`（D） | 页修改状态的缓存副本 | 获准写访问时更新，并按配置回写 PTE |
| LRU 顺序 | 各槽最近使用次序 | 存在 `TLB` 的替换状态中；`lru_rank(slot)` 返回 0 表示 MRU |

填充在所有地址、权限及 A/D 字段就绪后置 V=1；同 Tag 重填不会产生两份有效映射。
组满（这里整个 TLB 为一组）才按 LRU 替换，存在 V=0 槽时优先使用无效槽。
替换装入新映射时，旧 R/W/X/U/A/D 状态全部覆盖，不得把旧页的脏位留给新页。
失效只需清 V，旧 Tag/PFN 可以留在存储阵列中，但不能继续命中。

PTE 的 P 与 TLB 的 V 是独立状态：P=1 表示页驻留主存，V=1 表示槽缓存了可用映射。
清 TLB 的 V 不会把 P 清零，TLB 中没有某页也不说明它没有驻留。
本模型只向 TLB 装入 P=1 的映射。整项回写接口所需的软件字段另作不透明副本保留，
不参与 Tag 匹配、权限或替换判断。

本模型页大小固定为 4 KiB，切换地址空间会清空 TLB，因此没有额外存储页大小、ASID 或全局映射位；
这些属于扩展相应机制时才需加入的字段。A/D 也不等同于 Cache 行的有效位/脏位。

## 两个对象的使用边界

```cpp
auto pa = mmu.translate(virtual_address, AccessType::Read, Privilege::User);
auto value = cache.read_byte(pa);
```

`translate()` 返回物理地址；`translate_with_info()` 还返回 VPN、PFN、页内偏移和 TLB 命中信息。
权限失败会抛 `TranslationFault`，调用方由 `reason()` 区分缺页、禁止读取、禁止写入、
禁止执行、用户态访问内核页和非法页框；异常带有原虚拟地址和本次访问类型。
异常发生后，调用方应终止这次数据访问，将异常交给 CPU/操作系统处理。
MMU 不负责磁盘换页，也不会在权限失败时输出一个可继续访问的物理地址。

`Permissions` 默认全部禁止，映射时显式授权。用户态访问要求 `user_accessible=true`；
内核态可访问内核页，但同样遵守 R/W/X。这是本教学模型的权限规则。
跨虚拟页访问需要调用方按页拆分，并分别转换、检查每页；相邻虚拟页不一定映射到相邻物理页。
`translate_range(va, size, access, privilege)` 已实现这一步，返回每一段的虚拟地址、物理地址和长度。
它只执行翻译和检查，不读写目标数据。任一页失败就不返回片段列表；0 长度为空操作，
超过 32 位地址上界的范围会在翻译前被拒绝。

外层的 [`simulation/memory_access.hpp`](../../../simulation/memory_access.hpp) 将整段预检查和物理读写接在一起，
提供 `read()`、`write()`、`fetch()`，分别申请 R、W、X 权限。只有所有页都通过后才发出数据请求，
因此第二页缺页或权限不足不会部分写入第一页。物理存储在后续分段传输中发生错误时不提供事务回滚。
这个接线对象不在 MMU 内部，物理端仍然只需要读写回调。

## 页表字节布局与 A/D 硬件更新

`PageTableEntryCodec` 定义下面的 64 位小端编码，不使用 `sizeof(PageTableEntry)`：

```text
63                         32 31                 12 11       7 6 5 4 3 2 1 0
| 软件自用字段 software_data | 20 位物理页框号 PFN | 保留为 0 |D|A|U|X|W|R|P|
```

P 表示驻留主存；R/W/X 表示访问权限；U 表示用户态可访问；A 是访问位，D 是页修改位。
保留位非零或非法页框会产生 `InvalidPageTableEntry`，软件字段不会被 A/D 更新覆盖。
页表遍历单元的 PTBR 为物理字节基址，每项地址为 `PTBR + VPN × 8`。

将真实物理字节端口接入页表：

```cpp
PageTablePort page_table(read_physical, write_physical);
MMU mmu;
mmu.enable_paging(ptbr, page_table.reader(), page_table.writer());
```

成功通过驻留和权限检查后，硬件在需要时将 A 置 1；允许写访问时将 D 置 1。
第一次读通常触发 A 回写，随后同页 TLB 命中不再访问页表；之后第一次写即使 TLB 命中，
也会回写 D。回写立即通过配置的物理端口提交，不等到 TLB 被替换，因此单页失效和切换 PTBR 不会丢失这些状态。
必要的回写失败时报告页表访问异常，不输出物理地址，也不将未完成的新 A/D 状态提交到 TLB。
初次缺页或权限被拒绝不会更新 A/D。

这里的 D 是**页表中的页修改状态**，Cache 的 dirty 是**缓存行是否需写回下级**，两者分别维护。
A/D 根据已获准的翻译请求保守置位；跨页预检查中，先前通过的页可能已置位，
即使后页失败而没有发出目标数据访问，也不会回滚这些标志。
软件清 A/D、修改权限或重映射后必须失效对应 TLB 项。

兼容旧示例的 `enable_paging(base, reader)` 只提供解码后的只读页表：这种模式 A/D 只保留在快表，
没有主存回写保证。完整模式应同时提供 writer，`access_bits_writeback_enabled()` 可检查配置。

## 页表与 TLB 的维护

- `enable_paging(base, reader, writer)`：设置物理页表基址与读写接口，清除旧 TLB 并启用分页。
- `disable_paging()`：关闭分页、清除 TLB，之后使用地址直通模式。
- `invalidate_page(vpn)`：页表映射或权限更新后，失效该虚页的旧缓存；参数是**页号**，不是字节地址。
- `flush_tlb()`：清空全部缓存；本模型没有 ASID，切换地址空间必须清空。
- `statistics()`、`tlb().entries()`：只读观察转换次数和快表内容。
- `fault()`、`clear_fault()`：读取和清除故障寄存器；成功访问不会隐式清掉先前的故障。
- `last_translation()`：观察最近一次成功/失败请求的 TLB 命中、页表访问、A/D 更新、故障阶段和物理结果。

`reader` 接收页表项的**物理字节地址**，返回已解码的 `PageTableEntry`。
`PageTablePort` 负责上述真实字节编码；也可直接提供已解码的 reader/writer。
不存在的映射应返回 `present=false`。物理页表读取、短读、写回失败会转换成 `PageTableAccess`，
与页面不驻留的 `NotPresent` 分开统计。页表访问可以走物理 Cache，但不能再次经过 MMU。
读写须使用一致的物理存储视图；若页表经 Cache 访问，外层直接改主存前后也要维护该缓存的一致性。
回调引用的存储对象必须在使用 MMU 期间保持存活。

## 缺页暂停、修复和重试

硬件检测到 P=0 后抛出 `TranslationFault(NotPresent)`，保存故障地址、R/W/X 请求和特权级。
故障发生前不会输出该虚页的物理地址、填充它的 TLB 项或发起目标数据访问。
页表自身的访问请求与目标数据请求分别看待：即使缺页，之前也已读取了页表项。

以下操作由外层处理器/OS 驱动，硬件提供所需机制：

1. 根据故障寄存器查找未驻留页；选择空闲页框或决定替换哪一页。
2. 换出旧页前撤销其映射并失效 TLB；通过 Cache 的 `invalidate_range(pa, size)` 写回并失效旧物理范围。
3. 将脏页保存到后备存储，调入新页；更新主存页表，失效旧快表项。
4. 清除已处理故障，重新发出原虚拟地址访问；MMU 重新查页表、装入 TLB 并翻译。

MMU 不调用磁盘、不选择牺牲页，也不实现操作系统页框分配。
`software_data` 可以保存上层解释的后备存储标识，硬件不会读取该标识指向的磁盘。
现有 Core 在取指异常时保持该取指拍的寄存器和阶段，修复后可重新调用 `tick()`；
本项目尚未实现某种 ISA 的异常入口/返回指令和处理程序 PC 跳转。

## 独立示例与验证

在仓库根目录执行：

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror soc/core/mmu/example.cpp -o /tmp/mmu-demo
/tmp/mmu-demo
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/virtual_memory_test.cpp -o /tmp/vm-test
/tmp/vm-test
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/tlb_test.cpp -o /tmp/tlb-test
/tmp/tlb-test
```

示例只使用主存字节数组，演示缺页、外层安装映射、同一地址重试，以及 TLB 命中时更新 D。
独立测试验证编码、权限、A/D、故障寄存器、页表物理端口失败和非连续物理页的跨页翻译。
TLB 字段测试还验证 V=0 时旧 Tag 不能命中、控制位重填、空槽复用、LRU 和非法表项拒绝。
全链路和图中五种组合见 [外层接线示例](../../../examples/memory_system/README.md)。

当前没有多级页表、大页、分段、ASID 或多核 TLB shootdown。
这是功能调用模型，不模拟页表访问周期或缺页异常入口的逐拍执行。

现有 `Core` 持有一个 MMU，默认分页关闭；取指显式申请 `Execute` 权限。
宿主可以通过 `core.mmu()` 配置它。核心的 PC/MAR 仍是 16 位，MMU 本身可独立处理 32 位地址。
可运行的 MMU → Cache 示例见 [`../../../examples/memory_system/mmu_cache.cpp`](../../../examples/memory_system/mmu_cache.cpp)。
