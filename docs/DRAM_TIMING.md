# 可选的逐拍 DRAM 命令模型

`soc::controller::DramTiming` 将单个总线数据拍排入命令状态机，按目标 Rank/Bank 的
真实打开行选择 `PRE → ACT → READ/WRITE → 数据就绪`，行命中跳过 PRE/ACT。
`tRP`、`tRCD`、`tCAS` 以平台有效时钟拍计数，均须为正；同一时刻只有一个 DRAM 数据拍在处理。
PRE/ACT 实际广播到 DIMM 中所选 Rank 的各芯片，数据拍就绪后才由原内存端口完成读写和监听通知。
另有可选的多 Bank 交叉教学模型，参数、流水条件和 Fabric 限制见 [INTERLEAVED_DRAM.md](INTERLEAVED_DRAM.md)。

在 `SoCConfig` 中启用：

```cpp
colab::SoCConfig config;
config.enable_timed_dram = true;
config.dram_timing = {2, 3, 4}; // tRCD, tRP, tCAS；单位：系统周期
config.instruction_cache_hit_cycles = 2;
config.data_cache_hit_cycles = 2;
```

SystemC 平台和旧 `SoC` 都支持该配置。默认仍为 `enable_timed_dram=false`、Cache 命中一拍，
以保留原有教学实验；两拍命中先接受查找、下一有效拍读取数据，期间监听写入仍可更新该行。
SystemC 的 CPU、I/D Cache、DMA 和调试总线请求共享同一时序控制器；直接调用宿主
`Memory::read/write` 是功能调试旁路，不经过该时序状态机，应在机器暂停且控制器空闲时使用。
`Memory::inspect` 则直接观察存储单元，不改变打开行和访问统计，可在运行中用于调试或检查完成标志。
窄于 32 位的 Rank 会将总线事务拆成不跨 Rank 字的数据拍；例如 8 位 Rank 的字读取需要四个数据拍。
拆分后的多拍访问不是单个不可分割的数据拍，其他总线请求可能在拍间观察到部分写入。
同步总线将 DRAM 准备计入固定预算，超时会取消对应请求；已发命令的剩余时序仍受约束。
命令行示例（也可配合 `--interactive`、`--elf`）：

```bash
bash examples/systemc/run.sh --timed-dram --dram-trcd 2 --dram-trp 3 --dram-tcas 4 \
    --icache-hit-cycles 2 --dcache-hit-cycles 2 --trace /tmp/dram
```

总线端点在 DRAM 尚未就绪时保持请求；对 split 总线，等待的端点不占链路。
`DramTiming::last_tick()` 和 `statistics()` 可查看命令、地址和计数，SystemC VCD 包含
`dram_phase`、`dram_command`、`dram_address`。复位取消尚未提交的数据拍；DIMM 数据保留规则
仍由平台冷启动/热复位决定。

这是一种**教学命令时序模型**：尚未建模 tRAS/tRC/tWR/tRFC、刷新仲裁、bank 并发、突发协议、
电气引脚或完整 JEDEC 时序；写入复用 tCAS 作为简化数据延迟。不能据此宣称真实 DDR 周期精度。

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror tests/dram_timing_test.cpp -o /tmp/dram-timing-test
/tmp/dram-timing-test
bash tests/run_cpu_cache.sh
```
