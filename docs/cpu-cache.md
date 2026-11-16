# RV32 整机 CPU 数据 Cache

运行 `bash tests/run_cpu_cache.sh`。示例执行四条真实 RV32 指令：冷读、重复读取、写穿、读取新值，
分别打印每条指令的模拟拍数、总线事务数、DIMM 读请求数和累计 Cache 命中/缺失。

## 接线与逐拍行为

```text
RV32 LB/LBU/LH/LHU/LW 与 SB/SH/SW
  → Core MMU：翻译并检查整个访问范围的权限
  → CPU PMP：按当前特权级或 MPRV 检查物理访问范围
  → cache::CpuPort：物理地址，单个在途请求
       RAM 读命中 → Cache 数据直接返回，无总线事务
       RAM 读缺失 → 总线整行读 → 等待 → 装入 Cache → 返回目标字
       RAM 写     → 总线写穿 → 等待确认 → 更新已有行 → CPU 继续
       MMIO       → 原始总线设备事务，不缓存、不预取
  → Interconnect / Fabric：仲裁、延迟、数据拍与响应
  → Memory → DIMM / Rank / Chip / Bank
```

`SoC::tick()` 依次推进总线、CPU Cache 端口、设备，最后采样中断并推进 CPU。
整机默认采用 RV32 流水线；Cache 命中的查找/返回在 MEM 当前访问拍内完成，下一拍 WB 退休。
多周期实验仍保留原控制器的后续锁存/写回拍。
未命中时一次排队一个整行读取，CPU 保持访存状态，流水线冻结 MEM 及上游、允许 WB 排空；只有完整响应成功后才置行 valid 并继续。
写穿也必须得到总线确认才退休。失败不装入残缺行；故障由 Core 所选策略交给宿主或机器态处理程序。
请求端从不递归运行总线，队列满时保留 CPU 指令状态并在下一拍重试。

CPU 原有 MMU 在提交数据请求之前执行，因此缓存命中不能绕过页面权限或缺页检查。
PMP 同样先于缓存查找；用户态即使访问机器态预热过的缓存行，也必须重新获得物理访问权限。
Cache 使用物理 Tag；两个虚拟地址映射同一物理行时共享缓存副本。
页表仍由外层配置的同步物理回调读写，不把页表访问的耗时伪装成总线模拟拍数。

## 默认配置

`SoCConfig.enable_data_cache=true`；`SoCConfig.data_cache` 默认 16 组、2 路、32B 行，
共 1 KiB 数据，LRU，写穿、不按写分配、无写缓冲。组数、路数、行大小和替换策略可以配置；
本整机端口要求行大小为 4～1024B 的二次幂，仅支持上述写策略。独立 `Cache` 的写回、
写分配和写缓冲实验继续保留。

```cpp
colab::SoCConfig config;
config.data_cache.sets = 32;
config.data_cache.ways = 4;
config.data_cache.line_bytes = 64;
colab::SoC system(program, config);
const auto& stats = system.data_cache().statistics();
```

可设置 `enable_data_cache=false` 对比原来的 CPU → MMU → 总线直连路径。
自然对齐的 1/2/4 字节 CPU 普通 RAM 数据访问进入 Cache；访问须位于单行内，
整行必须完整落在一个已映射 RAM 窗口内。窄 Store 写穿保持原宽度，不扩大为字写入。
窗口边缘、其他宽度、MMIO 和未映射地址交给原始总线处理。

`system.data_cache()` 是只读观察接口，提供统计、地址拆分、各路 valid/tag/data 与最近一次访问。
`system.data_port().statistics()` 另有绕过次数、物理写监听次数、填充期间转发的字节数。
Cache 统计按成功交给功能阵列的 CPU 访问计数；MMIO、监听更新和失败填充不计为 CPU 命中/缺失。
`system.cpu().bus_waits()` 包含缓存缺失、写穿、MMIO 与总线背压的等待。

## DMA 与物理写监听

CPU 写穿保证 DIMM 始终持有已提交的新值，DMA 读直接访问 DIMM。
`Memory::write()` 在物理数据提交后调用外层注册的写监听，CPU、DMA、宿主修复/初始化均走同一路径。
`SoC` 把监听接到 `CpuPort::snoop_write()`：命中的已有缓存行同步更新对应字节；不分配行，
不把 DMA 访问计为 CPU 访问，也不改变替换次序。

整行填充需要多个总线数据拍。若 DMA 写了已经返回的字节，控制器记录该物理行的最新写入，
完整响应到达后合并到填充缓冲，再装入 Cache，避免旧字节覆盖 DMA 新值。
这是单核写穿的教学写更新监听协议；没有实现多核 MESI、写回所有权或真实互连的监听消息时序。

运行期间外层物理修改应经 `system.memory().write()`；直接操作 `system.dimm()` 的存储阵列会绕过监听。
`Memory` 当前保留一个写监听，由 SoC 安装；替换它会断开此接线，监听不得抛异常或递归写主存。
程序取指仍使用独立只读程序数组，本次接入的是数据 Cache。

## 验证范围

`tests/cpu_cache_test.cpp` 检查 RV32 流水线和两种多周期控制器下的真实 LW/SW 命中/缺失、命中不占总线、
写穿确认、写缺失不分配、虚拟别名、暖缓存上的页面权限/缺页/PMP 拒绝、MMIO 状态更新、
IOMMU 后 DMA 写更新、CPU 缓存轮询 DMA 标志、DMA 与行填充交错、满队列重试、
替换、窗口边界、部分行失败和超时。

另用固定种子生成 300 条包含字节/半字/字读写和运算的指令，在流水线/两种多周期控制器、四种总线时序下分别运行缓存与
直连系统，对比每次退休后的 PC、32 个寄存器与最终 DIMM；比较的是架构结果，不要求模拟拍数相同。
`rv32_subword_test.cpp` 另以独立数值参照和延迟字节后端验证扩展、数据相关、退休宽度及故障；
SystemC 平台用例覆盖实际 TLM/DIMM 上的窄访问、半字原子可见性、Cache 监听和 MMIO 拒绝。
原 I/O 六项测试继续验证 HDD、NVMe、NIC、中断和 DMA 共用唯一 DIMM 的路径。

流水线的较年轻 MEM 请求可以与较老 WB 退休发生在同一拍。
因此 `bus_waits()`、Cache 统计和外部事务数是整个核心的累计活动，
不能把两次退休之间发生的所有访存活动都归给刚退休的那一条指令。
