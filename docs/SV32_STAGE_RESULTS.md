# Sv32 硬件阶段实测记录

记录时间：2026-11-15T12:14+08:00（Asia/Shanghai）。这是本轮本机实现与验证，不沿用修改前的测试结果。

## 结果

| 验证 | 结果 |
|---|---|
| SystemC 3.0.2 全新项目构建目录 + 最终回归 | 59/59 CTest 通过 |
| SystemC 2.3.4 全新项目构建目录 + 最终回归 | 59/59 CTest 通过 |
| tests 下所有非 SystemC `*_test.cpp` 独立源码重编译/执行 | 43/43 通过 |
| 原 run_pipeline/control_units/cpu_cache/io 四脚本 | 均退出 0，包含各自示例程序 |
| 实际分页 ELF + 自动 VCD 检查 | 通过；冷上电和热复位均签名 0x53563332 |
| NVMe 整机示例 | 1815 周期、117 条退休、3 命令；核对 512 字节 |
| HDD 整机示例 | 1164 周期、96 条退休、512 DMA 字节、1 次 IRQ/ISR/MRET |

CTest 和独立测试有重叠，**不能相加当作不重复用例数**。编译器 GNU C++ 13.3/C++17、Clang 18.1.3；
Clang/LLD 构建实际 RV32 ELF，不是宿主解释 C。分页实验使用默认 64KiB。

首轮对最新未完成初稿重新构建的基线为 55/56，旧监督态单测仍要求 SUM/MXR=0、satp Bare-only；
本轮更新为已选 Sv32 语义并新增定向矩阵，没有删除/跳过该测试。初始 CMake 复用构建树 package 缺 export 文件，
随后改用已有头文件和动态库，未安装/重新下载 SystemC。

## 可复现构建/运行

在仓库根目录，使用本机已有库（新建的项目构建目录，不把旧二进制作为证据）：

```bash
L=/home/jerry/agent-tools/sv32-stage
SC=/tmp/colab-systemc.Xs5Z1R
cmake -S examples/systemc -B "$L/build3" \
 -DCOLAB_FETCH_SYSTEMC=OFF -DSystemCLanguage_DIR=SystemCLanguage_DIR-NOTFOUND \
 -DCOLAB_SYSTEMC_INCLUDE_DIR="$SC/systemc-3.0.2/src" \
 -DCOLAB_SYSTEMC_LIBRARY="$SC/build/_deps/systemc-build/src/libsystemc.so" \
 -DCOLAB_COMPILER_SMOKE_TEST=ON
cmake --build "$L/build3" -j4
ctest --test-dir "$L/build3" --output-on-failure -j4

cmake -S examples/systemc -B "$L/build2" \
 -DCOLAB_FETCH_SYSTEMC=OFF -DSystemCLanguage_DIR=SystemCLanguage_DIR-NOTFOUND \
 -DCOLAB_SYSTEMC_INCLUDE_DIR="$SC/installed-2.3.4/include" \
 -DCOLAB_SYSTEMC_LIBRARY="$SC/installed-2.3.4/lib/libsystemc.so" \
 -DCOLAB_COMPILER_SMOKE_TEST=ON
cmake --build "$L/build2" -j4
ctest --test-dir "$L/build2" --output-on-failure -j4

"$L/build3/systemc_machine" --demo nvme
"$L/build3/systemc_machine" --demo hdd
bash examples/compiler_smoke/check_paged.sh "$L/build3/systemc_machine" /tmp/paged-smoke
python3 examples/compiler_smoke/verify_paged.py --machine "$L/build3/systemc_machine" \
 --elf "$L/build3/paged-smoke/program.elf" --trace "$L/paged-verified"
bash tests/run_pipeline.sh
bash tests/run_control_units.sh
bash tests/run_cpu_cache.sh
bash tests/run_io.sh
```

其他机器可用现有 `COLAB_FETCH_SYSTEMC=ON` 固定源码构建方式，或正确安装的 SystemCLanguage package；
上述 `/tmp` 库路径是本机当前环境，不是项目新依赖。完整的独立重编译驱动与逐项结果保存在本机日志目录，
不把它作为项目运行所需依赖。

## 分页执行证据

`paged_start.S` 自己清理/建立 RAM 页表，MRET 到 S、SRET 到 U 的 C 函数，客体处理十次同步异常。
验证器逐项按 ELF 符号核对 cause/stval/PC；A/D 缺位修复后重试，内核数据未损坏，用户 RX 页不可写。
TLB 重映射后同一 VA 从旧物理页读 42 变为从新物理页读 99。

```text
signature=0x53563332  UART=V!
cycles=160000 retired=6725 exceptions=10 mode=S waiting=wfi
Sv32 walks=20 PTE reads=36 page faults=9
WFI cycles=83267
```

这里 PTE reads 是页表端口事务数，不声称等于真实 DRAM 命令数。十次异常中九次页故障、一次 ECALL。
VCD 自动解析验证 satp=0x80000001、20 次 walk、36 次 PTE 事务、9 次 page fault。
ELF SHA256：`b028c2a3979aad0b661f57ab2c6432485d97e45e457be6358ac971f8a774d019`。

## 本机日志与修改隔离

日志目录：`/home/jerry/agent-tools/sv32-stage/`：

- `baseline-configure.log`、`baseline-build.log`、`baseline-ctest.log`：本轮开始时实际 55/56 基线。
- `fresh3-configure.log`、`fresh3-build.log`、`fresh3-ctest.log`；`fresh2-*`：全新项目构建记录。
- `final3-build.log`、`final3-ctest.log`；`final2-*`：最后增加错误恢复断言后的回归。
- `standalone-summary.log`、`standalone/results.json` 和各测试日志：43 项全重编译结果。
- `regression-pipeline/control/cache/io.log`：原脚本回归。
- `nvme-demo.log`、`hdd-demo.log`、`paged-verified.log`、`paged-verified.vcd`：整机和波形证据。
- `before/`、`expected-hashes.json`：保留本轮触碰文件的原始内容与逐次哈希；只针对本轮改动，未覆盖他人未提交工作。

未 commit/push、未续 Codex、未改 OS 仓或 soc/soc.hpp，未安装依赖。
GUI 电源、实际微内核、全部复位/中断竞争排列、多核与真实 PCIe/DDR 协议均不在本次已验收范围；
后续内核须匹配 [板级合同](SV32_HARDWARE_CONTRACT.md)，不把 smoke ELF 叫作微内核。
