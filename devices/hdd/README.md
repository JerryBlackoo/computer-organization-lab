# HDD：电驱动的磁盘逻辑模型

入口为 [`hdd.hpp`](hdd.hpp) 中的 `colab::devices::HDD`，C++17、仅标准库。
默认几何与转速来自所给第 44 题。构造完成时没有供电、盘片不转，不能读写。
供电后先加速到工作转速，再执行排队的控制器命令；时间由 `advance(ms)` 或 `tick()` 推进。

```bash
# 在 computer-organization-lab 仓库根目录运行。
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror devices/hdd/example.cpp -o /tmp/hdd-demo
/tmp/hdd-demo
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror tests/hdd_test.cpp -o /tmp/hdd-test
/tmp/hdd-test
```

## 从供电到磁记录

```text
5 V 逻辑电源有效 ─→ 控制器 / 读写放大器
12 V 驱动电源有效 ─→ 主轴驱动器 ─→ 共同转轴 ─→ 全部双面盘片旋转
                 └→ 音圈驱动器 ─→ 共同磁头臂 ─→ 全部磁头沿半径一起移动

主机命令 ─→ 控制器解析 ─→ 选择盘面磁头 / 寻道 ─→ 等待扇区转到磁头下
主机内存 ←→ DMA 数据寄存器 ←→ 串并转换移位寄存器 ←→ 读写通道 ←→ 磁层极性
```

| 部件 | 实现及物理含义 |
|---|---|
| 盘片、记录面、磁道、扇区、柱面 | [`geometry.hpp`](geometry.hpp)：每盘片双面，每面一个磁头；所有面的同号磁道组成一个柱面；`track(cylinder, head)` 返回盘片、面和磁道定位 |
| 磁性介质 | [`medium/magnetic_media.hpp`](medium/magnetic_media.hpp)：稀疏保存扇区中各磁化单元的极性，断电不清除 |
| 主轴及电机 | [`spindle/spindle.hpp`](spindle/spindle.hpp)：电驱动使能、加速、恒速、失电滑行、统一角位置 |
| 音圈与磁头臂 | [`actuator/actuator.hpp`](actuator/actuator.hpp)：一个径向位置，全部磁头联动；失电中止驱动 |
| 磁头和读写通道 | [`head/read_write_head.hpp`](head/read_write_head.hpp)：磁头电子选通、读放大器使能、写电流使能；必须有转速且已定位才能访问磁层 |
| 磁盘控制器 | [`controller/controller.hpp`](controller/controller.hpp) 与 `HDD` 的事件连接：FCFS 队列、命令解析、寻道、旋转等待、扇区传输和完成状态 |
| DMA | [`dma/dma.hpp`](dma/dma.hpp)：每个缓冲字申请一次主存总线周期，搬运后释放；支持观察 BR、总线拥有状态和请求数 |

这里的电源是供电有效信号，5 V / 12 V 是教学接线命名；不求解电压、电流、反电动势或磁场。
两个供电有效时控制器才使能驱动器。仅加电不会立即得到 7200 rpm。
默认线性加速 1000 ms、失电线性减速 500 ms；这些参数是教学假设，可通过 `Config` 修改。
`Spindle` 对速度积分得到转过的圈数，因此启动、寻道、解析命令、总线等待期间盘片都会继续旋转。

音圈寻道采用与径向距离成正比的功能模型：

```text
全程寻道时间 = 2 × 配置的标称平均寻道时间
本次寻道时间 = 全程寻道时间 × |目标柱面 - 当前径向位置| / (柱面数 - 1)
```

这采用所给教材“平均寻道约为最大寻道的一半”的简化约定。
标称 5 ms 用于第 44 题的估算；实际请求的平均寻道取决于工作负载，均匀随机柱面并不必然平均 5 ms。
同柱面换磁头是电子切换，本模型不增加切换延迟，也不移动磁头臂。

磁层使用 NRZI 教学编码：数据 1 令极性翻转，数据 0 保持极性；读通道从相邻极性变化恢复数据。
每扇区有独立同步基准，字节内按低位到高位编码。未写区域定义为均匀磁化，读回全零。
`inspect_polarity()` 只是只读观察磁层；正常数据只能经过有电、已定位的磁头和控制器。
不会在构造时分配全部 40.96 GB，也不实例化 8000 万个空扇区对象；坐标仍完整区分盘片、记录面、磁道和扇区。

## 第 44 题的可计算和实测指标

| 指标 | 默认值 / 结果 | 接口 |
|---|---|---|
| 盘片数、记录面/磁头数 | 4、8 | `geometry().platters / heads()` |
| 柱面数、全部磁道数 | 20000、160000 | `geometry().cylinders / tracks()` |
| 每道扇区、每扇区数据 | 500、512 B | `geometry()` |
| 总扇区数 | 80000000 | `geometry().sectors()` |
| 格式化容量 | 40960000000 B = 40.96 GB ≈ 38.147 GiB | `geometry().capacity_bytes()` |
| 柱面号、磁头号、扇区号的最小位数 | **15、3、9 bit** | `geometry().address_widths()` |
| 一圈时间 | 8.333333 ms | `config().revolution_ms()` |
| 平均旋转等待 | 4.166667 ms | `config().average_rotation_ms()`；实测 `measurement.rotation_ms` |
| 一个扇区的传输时间 | 0.016667 ms ≈ 16.667 μs | `config().sector_ms()`；实测 `measurement.transfer_ms` |
| 平均访问时间 | **5 + 4.166667 + 0.016667 = 9.183333 ms** | `config().average_access_ms()`；实测 `measurement.access_ms()` |
| 标称有效数据率 | 30720000 B/s = 30.72 MB/s | `config().payload_bytes_per_second()` |
| 64 bit 缓冲区一扇区的 DMA 请求 | **512 × 8 / 64 = 64 次** | 完成记录 `measurement.dma_requests` |
| 理想无格式开销时 64 bit 字的到达间隔 | 16.667 μs / 64 ≈ 260.417 ns | `config().sector_ms() / config().dma_requests_per_sector()` |

公式结果之外，测试使用 500 组实际机械事件采样寻道和旋转角度，均值核对为 9.183333 ms；
示例用 64 组事件打印相同的测量。采样中一半零距离、一半全程寻道，角位置均匀取中点。
这验证了分阶段计时，不把某一次随机读的访问时间硬编码成平均值。

每个 `Completion` 的 `Measurement` 分别记录：

```text
access_ms   = seek_ms + rotation_ms + transfer_ms
response_ms = queue_ms + controller_ms + prefetch_ms + access_ms + drain_ms
```

`queue_ms` 包含前一请求和电机启动造成的等待。写入先经 DMA 预取第一个字，记入 `prefetch_ms`；
读完最后一字后等待其 DMA 周期完成，记入 `drain_ms`。因此端到端响应时间可能大于题目的磁盘访问时间。
`Statistics` 还累计成功/失败命令数、实际磁层读写字节和成功请求的平均访问时间。
`Measurement::media_bytes_per_second()` 按磁层传输阶段测量吞吐量，
`host_bytes_per_second()` 按含等待的完整响应时间测量有效主机吞吐量。
参数提供公式估算，完成记录提供此次事件运行的测量，两个数值的意义分开。

第 44 题没有给磁化单元总数、格式开销或盘面半径，因此默认不编造非格式化容量或记录密度。
可在构造前补充：

```cpp
colab::devices::hdd::Config config;
config.geometry.raw_bytes_per_track = 272000; // 示例假设，非第 44 题条件。
config.geometry.dimensions = colab::devices::hdd::Dimensions{20, 50}; // 半径，mm。
colab::devices::HDD disk(config);
auto raw_capacity = disk.geometry().unformatted_capacity_bytes();
auto density = disk.geometry().density(19999);
```

未给 `raw_bytes_per_track` 时非格式化容量为 `nullopt`，计时按无额外格式开销的理想扇区处理。
给定后，整个扇区槽仍占一圈的 1/500，其中额外空间作为开头的格式区占用时间，剩余时间传数据；
传输区的瞬时字到达间隔相应缩短。几何密度使用均匀磁道间距，磁道中心处计算圆周长度，
返回道密度、有效数据位密度/面密度，并在给定原始容量时另外返回原始记录密度。
固定每道扇区数时，内圈的位密度更高，符合图中的定角扇区划分。

## 使用、总线竞争和断电

```cpp
#include "devices/hdd/hdd.hpp" // 相对仓库根目录。

colab::devices::HDD disk;
disk.power_on();
disk.advance(1000); // 到达额定转速；也可以在启动期间先排队命令。
auto id = disk.submit_write({10000, 6, 123}, colab::devices::hdd::Bytes(512, 0xA5));
if (disk.run_until_idle(100)) {
    auto completion = disk.take_completion();
    // 检查 completion->id、status、measurement；失败也会生成完成记录。
}
disk.submit_read({10000, 6, 123});
disk.run_until_idle(100);
auto result = disk.take_completion(); // result->data 是 DMA 搬入主存的字节。
```

`submit_read_lba()` / `submit_write_lba()` 提供线性扇区地址，映射为
`LBA = (cylinder × heads + head) × sectors_per_track + sector`。
所有 CHS 字段从 **0** 编号，越界编码即使能放进 15/3/9 bit 字段也拒绝。
一次命令操作一个完整扇区；连续扇区可连续提交，控制器按 FCFS 执行。
`tick()` 默认推进 1 μs，可直接用于现有 `simulation::Simulator<HDD>`。
内部事件精确处理期间的 DMA/磁记录，所以大步长不会凭空漏掉中间传输。

总线输入如下：

```cpp
disk.set_bus_signals({true, false}); // CPU 有新请求，但尚未占用总线。
// DMA 优先获得下一个周期；每个字仍分别发一次 BR。
disk.set_bus_signals({true, true});  // CPU 已有主存周期在进行，DMA 等待。
disk.set_bus_signals({false, false}); // 外部释放总线，待处理 DMA 可获准。
```

本地仲裁策略是 DMA 高于 CPU 的新请求；已经开始的 CPU 周期不能被抢占，已经获准的 DMA 周期也不会被撤销。
因此，第 44 题中 CPU **同时需要**主存时，DMA 能在下一个可用总线周期优先获得使用权。
CPU 的在途周期要由外部调用方在完成后释放；此处没有实现完整 CPU 调度器。
`dma().requested()` 是待处理 BR 的逻辑电平，`dma().owns_bus()` 表示 DMA 总线周期进行中。
请求计数按每个新 BR 计，等待期间不重复增加。

默认数据寄存器为 64 bit，另有磁通道的串并转换移位寄存器。
读通道每形成一个字就送到数据寄存器并发 DMA 请求，DMA 清空后才能接下一个字。
写通道先取一个字到移位寄存器，同时请求下一个字。
这是周期挪用，整个扇区的 `host_memory` 是主存端口的模拟，不能作为隐藏的整扇区控制器缓存救场。
若总线延迟过长，读取产生 `ReadOverrun`，写入产生 `WriteUnderrun`，不会让磁盘倒转或停在目标扇区等数据。
非整倍数长度使用最后一个短字；总线请求数为向上取整的扇区字数。

失去任一供电会关闭磁头电路、取消在途 DMA、将进行中和排队命令报告为 `PowerLost`，重启不会重放旧命令。
磁层内容保留；已完成的磁通道字保留，尚未写完的当前字不提交。
连续 NRZI 编码在部分写入边界调整后续磁单元的极性，使未提交的逻辑数据保持不变；该功能模型不模拟边界处的物理磁写瞬态。
所以断电可能留下部分更新的扇区，写入不是整个扇区原子提交。
读失败只返回已经搬入主存的前缀，`media_bytes` 与 `dma_bytes` 分别报告磁层和主存端已完成的数量。

## 验证与模型边界

`tests/hdd_test.cpp` 检查第 44 题的公式及独立事件采样、两个供电的组合、
启动/滑行角位移、磁极性编码、断电重读、盘面/磁道/扇区隔离、FCFS 排队、
CPU 竞争、DMA 周期不可抢占、读溢出/写欠载、部分扇区写入保留、
非 2 的幂配置及末尾短字、与独立平坦磁盘数据对照、步长一致性和参数边界。

这是带机械时序和供电约束的功能级 HDD。磁层按一个缓冲字提交，不模拟字内每一位的失电瞬态；
音圈采用线性寻道，不模拟惯性、伺服、停泊斜坡或失电自动归位。
格式区按占用空间/时间建模，未生成真实地址标记、ECC 波形或做缺陷管理。
暂未实现写回缓存、预取、磁头切换延迟、分区带记录、SATA 协议或文件镜像持久化；
断电保留指同一 `HDD` 对象中的磁层，不表示销毁对象或退出进程后自动保存到文件。
该模块可独立运行，尚未接入现有 Core、MMIO、中断控制器或真实主存总线；
SoC 接入时需要将 CPU 在途主存周期反馈给上述仲裁输入。
