# PCIe目录中的总线事务/定时教学模型

共享事务互连的实现位于 [`../fabric.hpp`](../fabric.hpp)。它负责 RAM 和 MMIO 的物理地址译码、
仲裁和事务调度。这里保留原实验和 `pcie.hpp` 兼容入口，`pcie::Fabric` 与 `PCIe` 对应同一个 `fabric::Fabric`。
完整整机连线见 [I/O 系统](../../docs/io-system.md)。

## 独立总线实验的连接方式

- `adapters.hpp` 的 `map_memory` 可复用 `soc::controller::Memory` 或 `simulation::MemoryAccess` 的读写接口。
- 独立实验可按 master ID 提交 CPU 或 DMA 事务，并直接调用 `Fabric::tick()` 推进调度。

## 与图片逐项对应

### 源1：页275，总线定义与分类

地址/命令来自请求者，数据读从endpoint返回、写由请求者发出。
Trace同时给出请求者master、tag、endpoint索引、数据发送方sender。
前三种教学定时中，同一时刻仅一个事务拥有调度资源，等待期间仍占有它；
多个请求者按master ID轮转仲裁，同一master内按提交顺序**获准发请求**。
地址窗口译码选择一个从设备，不实现广播监听。

内部总线、系统总线、I/O总线、通信总线是不同层次；本目录是I/O事务抽象，
不是替代CPU内部单总线或模拟UART/USB。图片中的ISA总线标准也不是CPU指令集ISA。
PCIe实际上是高速串行、点对点、可全双工链路；这里单个调度事件互斥仅是比较教材四定时的
**共用逻辑调度器**，不意味着PCIe所有端口共享一根并行总线或不能双向同时传输。

### 源2：页284，事务和突发

`submit(Request)`仅入队并分配唯一tag，不立即读写后端，也不立即返回数据。
正常事务经过Address（地址/命令）、准备/Response、Data阶段。
`beat_bytes`是1/2/4/8字节；第i拍地址为首地址+i*beat_bytes，不把“+1”误解成一个字节。
`burst=true`只发一次首地址，连续数据拍；false每个字重发地址并重复准备阶段。
四字测试验证+4地址以及1次/4次地址阶段；在相同配置下突发总事件数更少。
这是离散事件模型，不模拟串行比特、线路频率或以拍数虚报实际GB/s。

### 源3：页285–286，四种定时

| Timing | 实现 |
|---|---|
| Synchronous | Address后固定`synchronous_wait`准备预算，再给数据拍；快设备也等到预算，慢设备报Timeout而不延长周期。无ACK/WAIT握手。 |
| Asynchronous | 仅实现**全互锁四相**：REQ↑→ACK↑→REQ↓→ACK↓。每次变化跨事件，绝不同事件将REQ/ACK同时撤掉。慢设备准备期间REQ保持。tick只推进事件，不代表双方物理公共时钟。 |
| SemiSynchronous | 共同模拟时钟边沿；未就绪时`wait_n=false`（低有效），插入等待拍；恢复高才进入Response/Data。 |
| Split（默认） | Address后释放调度资源，pending队列独立等待；就绪响应和新请求交替竞争，返回数据也轮转。每个返回结果带原tag/master，允许后发的快设备先完成。 |

异步的**不互锁、半互锁未实现**，没有用全互锁冒充三种都支持。
三种独占定时中非突发的多字逻辑操作保留所有权直到结束；分离模式则每次请求/应答事件释放。
异步全互锁支持慢从设备及失败后的Release，错误后不会永久busy。

分离模式读请求产生抽象read completion；写为posted，不生成返回completion。
写请求准备就绪时出现本地`Ready`事件（link_busy=false），不是endpoint发回ACK/Completion；
随后数据由requester发送。写成功/失败的`Result`是仿真观察器的本地结果，不是线上PCIe Completion。
`response_phases`计读应答或独占定时就绪；`read_completions`只计已路由split读的最终返回。
本地路由失败没有发出请求包，故不计read completion；不实现PCIe错误包格式。

## API与运行

仓库根目录：

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror interconnect/pcie/example.cpp -o /tmp/pcie-demo
/tmp/pcie-demo
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/pcie_test.cpp -o /tmp/pcie-test
/tmp/pcie-test
```

```cpp
#include "interconnect/pcie/adapters.hpp"
namespace p = colab::interconnect::pcie;
p::Fabric fabric({p::Timing::Split, 1, 64});
// 已有controller::Memory实例，内部已将DIMM映射在0x8000，容量至少128字节。
p::map_memory(fabric, 0x8000, 128, memory_controller, 3, "DIMM");
auto tag = fabric.submit({0, p::Operation::Write, 0x8000, 4, 1, true, {1,2,3,4}});
fabric.run();
auto result = fabric.take(tag); // status、data、transferred_bytes、error、read_completion
```

映射地址和传给后端的地址均为**绝对字节地址**，不自动减base；Memory自己进行窗口基址换算。
`map_memory`也可接现有MemoryAccess，其底层物理回调接Memory/Cache/DIMM；此时映射窗口表示输入
虚拟地址，MMU照常检查权限。后端及捕获回调的对象必须比fabric长寿。
也可直接`map(Endpoint)`接设备读写回调；只读/只写端口允许缺另一回调，访问不支持方向报BackendError。

`tick()`每次推进一个事件；空闲tick仍记时。`run(limit)`运行至无未完成事务，
超限抛出但保留请求，可继续tick/run。`ready(tag)`只读就绪状态；`take(tag)`取出并移除结果，
未知/未就绪tag抛错。已完成但未take的结果不占outstanding限额，请调用者及时取走，避免结果缓存增长。
`idle()`只表示无未完成事务，不表示结果已取走。`last_tick()`只保留最近事件，历史由调用者收集。

配置/请求错误在提交前抛出，不改变队列：正的固定准备预算、正的队列上限、合法Timing、
1/2/4/8字节拍宽、1..1024拍、首地址按拍宽对齐、完整payload长度、32位地址范围。
端点窗口不能重叠、不能越出32位空间；事务整段必须落在一个窗口内，禁止跨窗口，避免部分路由写入。
有未完成事务时不能改地址映射。

## 结果、错误与计数约定

- 地址未映射/跨窗口：Unmapped；同步预算不足：Timeout；后端异常、权限拒绝、短读、方向不支持：BackendError。
  所有运行期错误变为结果并释放事务，不妨碍后面的请求。
- `transferred_bytes`只计已成功的数据拍。事务不是原子的：后续拍失败时此前拍的副作用保留；
  后端若在一个拍内部部分写入后抛错，模型无法回滚它。回调必须同步、有限时间、不能重入tick。
- `link_busy`是当前调度事件有无占用，不是PCIe线利用率；独占等待时为true但无数据。
  `idle_ticks`数无调度事件的tick（不含posted本地Ready事件），不能直接当链路吞吐率。
- 分离模式只保证同master请求获准次序，不保证不同tag/端点的执行或完成顺序；
  需要先写后读的调用者应等待写本地结果再提交读。本模型**不实现PCIe完整ordering/fence规则**。
- non-burst多字操作用一个逻辑tag聚合多次地址阶段；它不是一个真实PCIe TLP/tag编码。
  任一时刻可有多tag在等待，读写callback只在选中的Data事件调用。

## 测试证据与未实现范围

定向测试覆盖：真实DIMM写入和读取，MemoryAccess/MMU接线及只读权限拒绝；四种定时的
burst/non-burst和1/2/4/8字节步长；非法配置、对齐、溢出、窗口跨界、短读、后端异常、
部分突发失败计数、队列满、run超限恢复、结果取走；慢设备与全互锁撤线顺序；多master仲裁、
分离等待时其他请求进展及乱序tag返回。

固定周期例：设备准备5、同步预算2，在事件3报错且后端未读；预算4/准备1的读在6事件完成；
半同步准备5出现4次WAIT低，7事件完成。全互锁REQ/ACK变化严格为00→10→11→01→00。
这些只是本模型事件约定，非任何PCIe代际时延或带宽数据。

未实现：Root Complex配置空间、BAR枚举、MSI/MSI-X/中断响应事务、实际设备DMA/IOMMU，
TLP线格式、LTSSM/PHY、lane编码、CRC、credit、replay、真实全双工并行、真实协议一致性验证。
