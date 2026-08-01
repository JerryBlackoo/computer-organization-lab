# Computer Organization Lab — 完整规格

> 本规格定义两个无脚手架项目的全部功能契约。测试即 oracle：你的实现必须通过 `tests/` 中的全部验收测试。
> 参考实现位于 `reference` 分支（闭卷完成后对照用）。

---

## 项目 1: RV32I 模拟器 (`rv32emu`)

### 1.1 目标
从零实现 RV32I 基础整数指令集的模拟器。不依赖任何 RISC-V 库。

### 1.2 CLI 契约
```
rv32emu [options] <image>
  --trace            逐指令打印 PC + 寄存器写（默认关闭）
  --max-cycles N     执行上限（默认 100000），防止死循环
  --start-addr A     起始地址（默认 0x00000000）
  --hex              输入为十六进制文本格式（每行一条 32 位指令）
  --dump-regs        结束时打印寄存器文件
```

### 1.3 输入格式
两种输入，二选一：
1. **二进制镜像**：原始指令字节（小端）。从 `--start-addr` 开始取指。
2. **Hex 文本**：每行一条 32 位指令，`00000513`（addi x10, x0, 0）格式。起始地址从 0 开始顺序分配。

### 1.4 指令集要求（全部必须实现）
- **U 型**：LUI, AUIPC
- **J 型**：JAL, JALR
- **B 型**：BEQ, BNE, BLT, BGE, BLTU, BGEU
- **I 型**：LB, LH, LW, LBU, LHU, ADDI, SLTI, SLTIU, XORI, ORI, ANDI, SLLI, SRLI, SRAI
- **S 型**：SB, SH, SW
- **R 型**：ADD, SUB, SLL, SLT, SLTU, XOR, SRL, SRA, OR, AND
- **杂项**：FENCE（NOP 处理）、ECALL（见 1.6）、EBREAK（终止执行，退出码 0）

### 1.5 不变量
1. `x0` 永远是 0，任何写入被忽略
2. 寄存器文件 32 个 32 位寄存器
3. 内存模型：32 位地址空间，可配置大小（默认 1 MiB）
4. 非对齐访问：定义为**允许**（模拟器不做异常）——但测试不覆盖
5. 取指从 PC 处读取 4 字节（小端）

### 1.6 ECALL 约定
仅支持一个环境调用号：
- `a7 = 1`：退出模拟器，退出码 = `a0`
- 其他 `a7`：打印 `ECALL unimplemented`，退出码 2

### 1.7 输出格式
- `--trace`：每指令一行：`pc=0x%08x reg=%s=%08x`（无寄存器写则不打印 reg 段）
- `--dump-regs`：`x0=00000000 x1=00000000 ... x31=00000000`（每行 8 个）
- 退出码：0（正常/EBREAK）、1（非法指令）、2（ECALL 未实现）、3（max-cycles 超限）

### 1.8 验收测试（tests/ 中的测试）
| 测试 | 输入 | 期望 |
|---|---|---|
| `t1_add.bin` | 3 条指令：addi x1,x0,5; addi x2,x0,7; add x3,x1,x2 | 退出后 x3 == 12 |
| `t2_branch.bin` | 循环 5 次计数器 | x5 == 5，cycle 数正确 |
| `t3_mem.bin` | sw x1→[0x100]; lw x2←[0x100] | x2 == x1 |
| `t4_fib.bin` | 递归斐波那契 fib(10) | x10 == 55 |
| `t5_illegal.bin` | 含非法指令码 0xffffffff | 退出码 1 |

---

## 项目 2: MESI 缓存一致性模拟器 (`mesi_sim`)

### 2.1 目标
从零实现 4 核 MESI 协议模拟器。不依赖 gem5 等框架。

### 2.2 CLI 契约
```
mesi_sim [options] <trace-file>
  --cores N          核数（默认 4）
  --line-size B      缓存行大小（默认 64）
  --sets S           集合数（默认 8）
  --ways W           路数（默认 2）
  --verbose          打印每次状态转换
```

### 2.3 Trace 格式
每行一条操作（`#` 开头为注释）：
```
R <core> <addr>      读 32 位
W <core> <addr>      写 32 位
```

### 2.4 协议要求
- 状态：**M**（Modified）、**E**（Exclusive）、**S**（Shared）、**I**（Invalid）
- 4 核通过共享总线通信，总线行为：
  - 读缺失：广播 BusRd
  - 写命中 E/M：本地写，不发总线
  - 写命中 S：广播 BusRdX（无效化其他副本）
  - 写缺失：广播 BusRdX
- 监听规则：持有 M 的核看到 BusRd → 降级为 S 并刷新；看到 BusRdX → 置 I
- 替换：被逐出的 M 行写回内存

### 2.5 输出格式
默认输出统计：
```
cycles=N hits=M misses=K hit_rate=0.xxx
per-core: core0 hits=.. misses=.. core1 ...
```
`--verbose` 额外打印每步：`cycle=N core=C op=W addr=0x... state=M->I (BusRdX from core2)`

### 2.6 不变量（模拟器必须检测违规并报告）
1. 不存在两个核同时持有同一行的 M 状态（M 唯一性）
2. 任一时刻，同地址行至多一个 E，或任意多个 S，或恰好一个 M
3. 状态转换必须符合 MESI 转换表（非法转换 → 打印 `VIOLATION` + 退出码 4）

### 2.7 验收测试（tests/ 中的测试）
| 测试 | 场景 | 期望 |
|---|---|---|
| `t1_single.bin` | 单核顺序读写 | 无违规，hit_rate 与手工计算一致 |
| `t2_shared.bin` | 两核交替读同一地址 | 第二核读缺失，之后两核 S，无违规 |
| `t3_invalidate.bin` | 核0写、核1读、核0再写 | 核1副本被无效化，无违规 |
| `t4_false_sharing.bin` | 两核写同一行不同字 | 交替 BusRdX，无违规，但显示低效 |
| `t5_violation.bin` | 陷阱场景：M 行被读降级后再次被其他核写（常见 bug：忘记把 M 降为 S/I） | 正确实现：无违规、退出码 0；错误实现：不变量检查器捕获 VIOLATION、退出码 4 |

---

## 测试运行器

`tests/run_all.py`（Python 3，无第三方依赖）：
```
python3 tests/run_all.py --emu ./rv32emu --mesi ./mesi_sim
```
对每个测试：编译生成输入 → 运行 → 比对期望输出 → 输出 `PASS`/`FAIL` 汇总，全部通过退出码 0。

---

## 交付物清单（提交到 main 分支）
1. `src/` — 两个模拟器的源码
2. `tests/run_all.py` — 测试运行器（或等价物）
3. `docs/ARCH.md` — 架构设计：取指/译码/执行数据通路、MESI 状态机图
4. `docs/INVARIANTS.md` — 你识别的不变量清单
5. 测试日志（`docs/test_log.md`）
