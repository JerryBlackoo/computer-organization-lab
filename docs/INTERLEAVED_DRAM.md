# 低位交叉多体存储：时序教具

`soc::controller::InterleavedDramScheduler` 模拟同一 DIMM 内每个 Rank 的多个 Bank、
共享命令槽和在途访问。`InterleavedDramMemory` 将调度出的 PRE/ACT 发给真实 Memory，
数据就绪后由 Fabric 调用 Memory 读写，再通知调度器完成。它可在普通 SoC 和 SystemC 平台中
显式启用；默认仍关闭，原有 `DramTiming` 单发模式保持可用。

## 地址与体数

一个教学“体”对应 Rank 内的一组同号 Bank：Rank 的多颗 DRAM 芯片是位扩展，同号 Bank
并行提供一个 Rank 字。`n` 是 `geometry.rank.dram_chip.banks`，Rank 是字扩展，不计入
`n`。接口的 `word_address` 是 **DIMM 局部 Rank 字地址**，不是物理字节地址；调用方应先完成
内存窗口片选和字节到 Rank 字的换算。它与 `DIMM::decode` 一致：Rank 内局部字地址 `w` 的
Bank 为 `w % n`，Bank 内按列再按行排列；Rank 之间是连续地址段。不同 Rank 的 Bank 状态
独立，但共享同一命令总线。

例如一个 Rank 四个 Bank、每 Bank 四列时，局部字 0、1、2、3 分别落在 Bank 0、1、2、3
的第 0 行第 0 列；局部字 4 回到 Bank 0 的第 0 行第 1 列，字 16 是 Bank 0 的第 1 行。
四字节 Rank 字对应物理字节偏移 0、4、8、12，而不是 0、1、2、3。

## 时钟和公式

默认教具参数 `t_bus=1`、`ta=4`，构造时校验 `ta == n * t_bus`；可关闭校验以研究其他配置。
两者的单位都是平台 tick，`t_bus` 指相邻命令槽之间的最小间隔。`ta` 定义为**访问开始的时间
边界到数据就绪的时间边界**，包括此简化模型中 ACT 的首拍；它不是另加的 CAS 延迟，也不单独
添加一个输出拍。`ta` 同时定义体内部最早数据可用时间；实际 Bank 直到调用 `complete` 消费
就绪数据才释放，因此消费者晚取会延长占用。

在时间边界 0 将四个不同 Bank 的请求全部提交，每拍可用一个命令槽、各 Bank 初始关行、
消费者就绪即完成时：

| 事件 | 第 1 字 | 第 2 字 | 第 3 字 | 第 4 字 |
|---|---:|---:|---:|---:|
| ACT 所在 tick（从边界 0 开始计第 1 拍） | 1 | 2 | 3 | 4 |
| 开始边界 | 0 | 1 | 2 | 3 |
| 就绪并完成的边界 | 4 | 5 | 6 | 7 |

因此从边界 0 的提交到第 4 字完成为 `4 + 3 × 1 = 7` tick，即
`T_a + (n - 1)T`。仅比较首字到末字的两个就绪时刻，跨度是 `(n - 1)T`；
不能把这两个不同的起点混为一谈。`t_bus=2, ta=8, n=4` 时，对应末字就绪边界为 14。
本式要求请求事先可供调度、不同 Bank、无前置 PRE、消费者及时完成；排队、行冲突或消费者
延迟都会增加实际耗时。

## 调度与接口

- `prepare(tag, op, word_address)` 首次登记并返回 false；同一 tag、操作和地址的后续调用
  查询是否 Ready。改变未完成 tag 的身份会抛异常。请求按每 Bank 的到达顺序处理；共享命令槽
  从全局到达序列选最早的可执行请求，跳过不可用 Bank，避免队头阻塞其他 Bank。
- 关行访问发 `Activate`，行命中发 `Access`；二者均占一个命令槽并启动 `ta`。换行先发
  `Precharge`，再等至少 `tRP` tick 和下一个可用命令槽后发 `Activate`。这里没有独立的
  tRCD/tCAS 命令模型；PRE 额外耗时不计入 `ta`。
- `tick()` 推进一 tick；`last_tick()` 最多记录一个 PRE、ACT 或 Access。到期数据变为
  Ready 后，调用 `complete(tag)` 消费它并释放 Bank。重复使用已完成的 tag 会被视为新请求。
- `reset()` 取消在途请求并清除统计和时钟。连接 Memory 时，Bank 的实际打开行和存储数据
  由 DIMM 保留；下一次访问重新读取真实打开行。SystemC 热复位保留 RAM，冷启动重新装载。

本模型没有刷新、完整 JEDEC 时序、电气细节和独立数据总线仲裁，不能证明实际 DDR 带宽。
当前 Split Fabric 可使**多个独立请求**在多个 Bank 重叠；但单笔 burst 的下一 beat 要等
前一 beat 完成后才调用 `prepare`，因此单笔 burst 不满足上表的同时排队前提。总线地址、响应、
仲裁和消费者延迟还会增加端到端时间。上表及示例的 7 tick 是直接向已连接 DIMM 的调度器
提交四个 beat 的结果，不是 CPU 或 Fabric 测得的整笔事务耗时。

与原单发模式相同，宿主直接 `Memory::read/write` 会绕过调度器；只能在机器暂停且调度器空闲时
使用，避免改变在途访问的打开行。`Memory::inspect` 不发命令，可在运行中检查数据。

## 平台配置

```cpp
colab::SoCConfig config;
config.memory.rank.dram_chip.banks = 4;
config.dram_mode = colab::DramScheduleMode::InterleavedPipeline;
config.interleaved_dram = {1, 4, true, 0}; // t_bus, ta, 等式校验, tRP
```

`Off`（默认）、`LegacySingleIssue`、`InterleavedPipeline` 互斥；旧配置
`enable_timed_dram=true` 仍选择单发模式，与交叉模式同时设置会报错。模式启用时才校验
交叉参数；默认 SoC 的内存几何为两个 Bank，所以 C++ 调用方要显式改为四 Bank，或按实际几何
调整参数。命令行开关默认选四 Bank，可自行指定其他值：

```bash
bash examples/systemc/run.sh --demo nvme --interleaved-dram \
  --dram-banks 4 --dram-bus 1 --dram-ta 4 --dram-interleaved-trp 0
```

`--dram-no-bank-check` 可关闭 `ta == banks * t_bus` 的教具参数约束；它不改变调度规则。
现有 VCD 的 `dram_phase`、`dram_command` 和 `dram_address` 仍只表示旧单发模式。
交叉模式的命令可通过 `interleaved_dram()->scheduler().last_tick()` 和 `statistics()` 查看。

真实 DIMM 的四字周期波形与公式：

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror examples/memory_system/interleaved.cpp -o /tmp/interleaved-example
/tmp/interleaved-example
```

运行独立测试：

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror tests/interleaved_dram_test.cpp -o /tmp/interleaved-dram-test
/tmp/interleaved-dram-test
```
