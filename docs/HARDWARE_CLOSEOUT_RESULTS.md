# 硬件收尾复核结果（2026-11-15，Asia/Shanghai）

本阶段在既有本机 checkout 上复核 CLI 审查线索；不是 GUI 或微内核交付。
原阶段记录 `SV32_STAGE_RESULTS.md` 与 `/home/jerry/agent-tools/sv32-stage/` 保留。
新证据根目录：`/home/jerry/agent-tools/hardware-closeout-stage/`。

## 风险判定与改动

1. **确认：宿主旧 MMU 与 Bare/M 语义存在可触发混用。** 原路径只要未启用 Sv32，
   即可退回宿主配置的单级 8 字节 PTE 教学 MMU；默认未启用该教学分页时并不发生错误。
   新增 `RV32Core::Translation`，正式 SystemC CPU 显式用 `Architectural`：Bare/普通 M 为 PA=VA，
   MPRV 按有效数据特权处理，不访问教学 callback。独立 CPU 默认仍是 `Teaching`，保持历史 API。
   不支持的 Architectural + 旧流水线组合明确拒绝，未把旧实验偷偷解释成架构分页。
2. **排除“非对齐一定应后报/先报”的单一规范结论。** 机器态规范允许 load/store misaligned 与
   page/access fault 的平台优先级选择。保留当前自然对齐先检查合同，不改已合法的执行路径。
   10 个两种控制器 LH/LW/SH/SW/JALR 反例锁定异常现场、无错误目标走表/无 link 写回。
3. **本轮未复现 I/L/S 故障委托、动态根/Bare 或 MPRV/PMP 错误。** 新增硬布线/微程序共 60 个
   精确异常例：无效 PTE、PTE PMP、PTE 总线错误、最终 PA PMP、最终 PA 总线错误，均覆盖 M/S 委托。
   检查 cause/xepc/xtval/陷入 PC、特权、退休、GPR/内存副作用和最终总线请求边界。
   新增两种控制器的 Bare→根1→根2→Bare 切换，satp 不隐式刷新，教学 callback 不被调用；另证旧教学重映射仍正常。
   新增 24 组 MPP×PTE.U×SUM×PMP 情景，各检查 M fetch 与 L/S；真实 SystemC Cache/TLM/DIMM 再验证
   12 个 I/L/S × 无效页/最终物理空洞 × 委托情景，以及恶意教学 callback 隔离。
4. **设备取消：补测试，未发现需改设备实现的问题。** 新增三个 CTest，共 20 个实际 Platform 切断窗口：
   NVMe 8、HDD 6、NIC 6。每设备覆盖早期在途、目标缓冲区部分 DMA 已提交、完成且 IRQ 已挂起，
   分别 warm reset 与 off/on；NVMe 另含写 DMA 完成但 NAND 提交前 MediaWait 两例。
   核对已提交 RAM 写不回滚、关机跨 20ms 不冒出旧 DMA、复位后运行 2000 周期无晚到 RAM/CQE/IRQ、
   旧 NVMe 写不追加落盘；相同队列/目标重新请求不同 LBA/载荷后数据及新 IRQ 正确，并可确认清除，邻接字节不损坏。
   这是有界采样，不是所有传输/机械/总线 delta-cycle 边界的穷举。
5. **确认文档过度承诺，不改机械/DRAM 模型来凑结论。** HDD 切电取消命令但不瞬间清零惯性转速/位置；
   平台 off 不推进机械域，单独增加 SystemC 时间不等于机械滑行。DIMM 不重建，不保证清行/bank/统计。
   已删除“每次重新经历转速爬升”的错误陈述，区分 RAM/命令控制复位与真实 DDR/机械冷复位。
6. **GUI 跨线程为将来边界。** 当前 CLI 同仿真线程调用；文档要求未来 GUI 消息投递到该线程，
   不直接跨线程调用 Platform/sc_start。本轮没有新增 GUI 或线程安全承诺。

## 最终重新编译与结果

复用已存在的两个构建目录，不安装、不 clone、不重开 CLI 审查：

| 验证 | 结果 | 新日志 |
|---|---|---|
| SystemC 3.0.2 最终源码重建 + 全量 CTest | **62/62** | `build3-final.log`, `ctest3-final.log` |
| SystemC 2.3.4 最终源码重建 + 全量 CTest | **62/62** | `build2-final.log`, `ctest2-final.log` |
| 全部独立测试重新 g++ 编译/执行 | **43/43** | `standalone-summary.log`, `standalone/results.json` |
| 原 pipeline/control_units/cpu_cache/io 四脚本 | **均通过** | `script-*.log` |
| 两版本编译分页 ELF + 冷/热启动 + VCD 断言 | **均通过** | `paged-explicit.log`, `paged-explicit2.log`, `paged*.vcd` |
| 两版本 NVMe / HDD 真实客体 demo | **均通过** | `nvme-demo*.log`, `hdd-demo*.log` |

分页 ELF SHA256：`b028c2a3979aad0b661f57ab2c6432485d97e45e457be6358ac971f8a774d019`。
默认 64KiB RAM，签名 `0x53563332`，160000 周期 / 6725 退休 / 10 异常（9 页故障），20 walks / 36 PTE 请求。
NVMe demo：1815 周期、117 退休、3 命令；HDD demo：1164 周期、96 退休、512 DMA 字节、1 IRQ。
这些套件有重叠，不能相加称为全新独立测试总量。中间一次新增测试夹具使用了不存在的 write32 API，
改用已有字节 write 后重新编译，最终两版本全部通过；初始失败日志保留，不把旧通过冒充新结果。

复现（目录前缀 `/home/jerry/agent-tools/sv32-stage`）：

```sh
cmake --build /home/jerry/agent-tools/sv32-stage/build3 -j4
ctest --test-dir /home/jerry/agent-tools/sv32-stage/build3 --output-on-failure -j4
# 将 build3 换成 build2 对第二版本执行同样命令。
python3 /home/jerry/agent-tools/hardware-closeout-stage/run_standalone.py
python3 examples/compiler_smoke/verify_paged.py --machine /home/jerry/agent-tools/sv32-stage/build3/systemc_machine --elf /home/jerry/agent-tools/sv32-stage/build3/paged-smoke/program.elf --trace /home/jerry/agent-tools/hardware-closeout-stage/recheck-paged
/home/jerry/agent-tools/sv32-stage/build3/systemc_machine --demo nvme
/home/jerry/agent-tools/sv32-stage/build3/systemc_machine --demo hdd
```

## 范围与未覆盖

没有提交/推送、清理未提交改动、改 OS 仓库、安装、读凭据或删除 `.codeartsdoer/`。
本阶段所有原始源码先与上轮 manifest 比较，再按阶段基线/每次写入哈希检查，原文件备份 `before/`，
阶段独立补丁 `this-stage.patch`、`modified-files.json` 和 `integrity.json` 位于新证据目录。

仍未做：全部 HDD/NVMe 写提交点与部分介质写取消排列、DIMM bank 电气重置、所有 IRQ 清除/完成同 delta 竞争、
所有背压及多核一致性排列；原有测试只对其实际覆盖范围作证。
**最终用户目标尚未全部完成**：没有 GUI 点击 Power On，也没有启动并验收用户自写微内核的隔离任务/抢占调度/IPC。
现有入口仍是 CLI + ELF 预装载，不是完整 ROM/NVMe 磁盘引导，不是实际 PCIe/DDR 电气协议实现。
