# SystemC 接入实验

保留现有 C++ 硬件模型，由 SystemC 提供模拟时间、时钟、进程调度和信号更新。
此目录是独立 CMake 项目，不要求修改原来的编译脚本。

正式的分模块硬件平台与交互控制台见 [SYSTEMC_PLATFORM.md](../../docs/SYSTEMC_PLATFORM.md)。
可直接运行 `bash examples/systemc/run.sh --interactive --trace /tmp/computer`，
再输入 `on`、`step`、`run`、`pause`、`status`、`reset` 等命令。
下面的 Timer、握手、整机桥接和独立内存示例用于分步理解接入机制。

## 构建与运行

在 `computer-organization-lab` 根目录执行。已安装 SystemC 时：

```bash
cmake -S examples/systemc -B /tmp/colab-systemc-build
cmake --build /tmp/colab-systemc-build --parallel 2
ctest --test-dir /tmp/colab-systemc-build --output-on-failure
```

设置 `COLAB_FETCH_SYSTEMC=ON` 时使用官方源码 3.0.2 的固定版本及 SHA-256，
不再查找本机安装包；下载和编译都放在构建目录，无需 sudo：

```bash
cmake -S examples/systemc -B /tmp/colab-systemc-build -DCOLAB_FETCH_SYSTEMC=ON
cmake --build /tmp/colab-systemc-build --parallel 2
ctest --test-dir /tmp/colab-systemc-build --output-on-failure
```

也可以先安装发行版的 `libsystemc-dev`，或通过 `-DCMAKE_PREFIX_PATH=/path/to/systemc`
选择自己的安装。优先使用官方 `SystemCLanguage` 包及 `SystemC::systemc` 目标；
无 CMake 包时回退到头文件、库和 Threads 查找。官方包会沿用库的 C++ 标准，
手动查找的库须使用 C++17 构建；不关闭 SystemC 的 ABI 版本检查。

```bash
/tmp/colab-systemc-build/systemc_clock_demo
/tmp/colab-systemc-build/systemc_powered_machine /tmp/colab-powered
/tmp/colab-systemc-build/systemc_soc_demo nvme /tmp/colab-nvme
/tmp/colab-systemc-build/systemc_soc_demo hdd /tmp/colab-hdd
/tmp/colab-systemc-build/systemc_tlm_memory_demo
```

第二个位置参数是 VCD 路径，不带 `.vcd` 扩展名；省略即不输出波形。
VCD 包含时钟、周期数、PC、退休指令数、中断输入/进入次数、NVMe 命令数和 HDD DMA 进度。

## 各个例子分别验证什么

| 入口 | 接入路径 | 验证 |
|---|---|---|
| `clock_demo.cpp` | 独立 Timer、CPU 输入观察器通过 `sc_signal` 连接 | 10ns 时钟，同步复位在 15ns 释放，40ns 写 IRQ，50ns 被观察 |
| `powered_machine.cpp` | Power、演示 CPU、演示 DMA、DRAM 端口独立调度，后端复用 `Memory → DIMM` | 上电、复位、CPU 独立推进、REQ/ACK 握手、单次写入、中断、VCD |
| `soc_demo.cpp nvme` | `sc_clock → ClockedSoC → SoC::tick()` | 真实 RV32 客体程序建立 NVMe 队列、提交命令、DMA 搬入 DIMM 并核对数据 |
| `soc_demo.cpp hdd` | 同上，运行已有 DMA 中断驱动 | CPU 等待期间执行其他指令，DMA 搬运 512 字节，中断处理程序处理后返回 |
| `tlm_memory_demo.cpp` | `SC_THREAD → TLM socket → MemoryTarget → Memory → DIMM` | 写/读数据、访问延迟、越界响应 |

Timer 实验中的 CPU 只是输入观察器。整机实验才运行仓库现有的 RISC-V 核心。
所有时间都是模拟时间，不调用 sleep，不要求每个硬件对应一个系统进程或线程。

上电实验的 CPU 是 `pc += 4` 的演示序列器，DMA 也只发起一次固定写请求；
不把它们当成现有 CPU 或总线 DMA 的迁移成果。其 DRAM 端口连接已有内存控制器和 DIMM，
但不包含真实 DDR 时序。预期轨迹：20ns 电源有效、40ns 释放复位并由 CPU 启动 DMA、
50ns DMA 请求、60ns 内存写入、70ns DMA 完成、80ns CPU 观察 IRQ。
DMA 与内存使用保持到确认的 REQ/ACK，避免同一请求被重复写入；
越界/非对齐请求返回错误。真实整机的 DMA 仍经过原有共享总线和 IOMMU，不走此演示直连路径。

这些入口是命令行实验。正式 `systemc_machine` 已接入独立硬件模块和运行时平台复位；
图形界面及操作系统启动尚未实现，用户当前确认的范围是先完成硬件平台。

## 整机桥接与时间单位

适配器在 `simulation/systemc/adapters/clocked_soc.hpp`，只在上升沿调用一次 `SoC::tick()`。
其内部保持原有顺序：总线响应 → 数据 Cache → 设备及其 DMA → 中断采样 → CPU。
DMA 仍由所属控制器推进，不能额外注册一次 DMA tick。

外部每拍结束时，适配器将观察值写入 `sc_signal`；监视器在这些信号更新后检查完成状态，
因此不依赖同一上升沿上多个进程的执行顺序。此处的信号是观测快照，
整机内部事务仍经过原有总线，并未改成 TLM。

整机例子使用 50μs 的缩放教学时钟，首次上升沿在 50μs，故 `时间 = 周期数 × 50μs`。
`config.disk.tick_ms = period.to_seconds() * 1000`，确保 HDD 的时间增量与 SystemC 相符。
这不是现实 CPU、PCIe 或 NVMe 的工作频率。改变频率时应重新校准总线等待拍数和设备延迟。
磁盘原件写入是 `sc_start()` 之前的测试夹具初始化，不属于客体 I/O 路径或本次测量时间。

原有 `SoC` 没有统一的运行时复位/电源域接口，因此过渡用的 `ClockedSoC` 不提供 reset 端口；
Timer 例子单独演示同步复位，不能把跳过 `tick()` 当作整机复位。
正式 `Platform` 则保留介质/内存、取消在途事务并重建控制状态，不依赖 `SoC` 的复位接口。

## TLM 内存适配器的语义

`simulation/systemc/adapters/memory_target.hpp` 复用已有内存控制器完成字节地址到 DIMM 的转换，
不绕过它直接操作存储单元。目标在 `b_transport()` 内等待“传入 delay + 固定访问延迟”，
然后执行读写并将返回 delay 清零。调用者不得再等待同一份延迟。
它必须从 `SC_THREAD` 等允许等待的进程调用，不能由 `SC_METHOD` 调用。

这里的 20ns 是演示参数，不是 DDR 时序校准结果。这个简单目标允许读写连续字节，
拒绝 byte enable、流式回绕和不支持的命令；不提供 DMI、仲裁或非阻塞四阶段协议。
多个请求方的排序、并发冲突和控制器排队应由后续互连/内存控制器模型明确实现。

## 后续如何拆成独立模块

| 部分 | 保留的现有模型 | SystemC / TLM 接入职责 |
|---|---|---|
| CPU | 核心、译码器、控制器、流水线 | 时钟驱动；保留非阻塞的访存提交/等待接口 |
| Cache / 总线 | `CpuPort`、`Interconnect` 的队列和仲裁 | 为请求和响应定义显式通道，再连接 TLM sockets |
| 内存 | `Memory`、`DIMM` | 由已演示的 target 扩展排队、Bank 时序和刷新事件 |
| NVMe / NIC | 控制器、DMA 引擎 | MMIO target、DMA initiator、完成事件、IRQ 信号 |
| 中断 | 中断控制器、CPU Trap / CSR | 以信号传递 IRQ，明确采样与生效时刻 |

不能直接把 CPU、总线和设备各自的现有 `tick()` 注册成同沿 `SC_METHOD`：
它们仍共享并立即修改普通 C++ 对象，SystemC 不会自动隔离这些修改。
需先定义旧状态/下一状态、请求何时入队、响应何时可见，再拆分调度。

CPU 现有 `DmaBus::submit/ready/take` 是非阻塞接口；后续可在适配层映射到队列和
TLM 非阻塞传输，也可用后台 `SC_THREAD` 执行阻塞事务再发布响应。
不要在 CPU 的时钟回调中直接调用会 wait 的 `b_transport()`。
TLM 定义通信接口，不自动补全 PCIe 包、链路、信用控制、DDR 时序或缓存一致性。

官方资料：[SystemC 安装及 CMake 接入](https://github.com/accellera-official/systemc/blob/3.0.2/INSTALL.md)、
[SystemC / TLM 规范入口](https://systemc.org/resources/standards/)。
