# MC100 软件开发与实板台架进度

执行范围已按用户最新要求调整：实板已连接，只允许 COM7；SD 卡已插入，USB 供电、无电池。先完成并实测“采集→SD WAV”，用户无需在场配合的测试自动执行。完整 VAD、恢复、功耗和耐久验收仍分别保留，不把台架切片当成完整产品。已授权分阶段合入主线并推送远程。

## 当前任务

| Phase | 内容 | 状态 | 证据/下一步 |
| --- | --- | --- | --- |
| P0 | 地基整理（合并恢复、清文档、隔离台架） | PASS | 恢复已合并；21/21 host 测试（含两个恢复测试）及文档/EVT 清理门禁通过 |
| P1 | 40MHz + VAD 选型 spike | **OPEN** | COM7 已取得固定40/固定80/DFS及APB-lock对照；固定40的PDM启动卡在 `i2s_channel_enable`，DFS活跃时为80MHz。esp-sr 2.4.7 + VADNet1 medium 无 PSRAM **target build PASS**；COM7 runtime 已加载模型但 AFE 创建因 `sr_rb_create` 内部内存耗尽而 **INIT FAIL / BLOCKED**，没有帧级数据。授权语料、电池侧电流及公平 VAD 对比仍未完成，VAD 不选型。正式报告：[PM/VAD spike](2026-09-23-mc100-pm40-vad-spike.md) |
| P2 | 产品录音循环（supervisor） | **IMPLEMENTED_NOT_BOARD_VALIDATED** | supervisor/音频/存储/恢复核心与host测试、product/EVT构建已有证据；尚未在COM7跑产品 BOOT→LISTEN→RECORD→CLOSE→LISTEN、掉电恢复及卡生命周期实板 smoke，不能关闭Phase 2 |
| P3 | VAD（esp-sr/libfvad 择优 + 语料评估） | NOT_STARTED | 依赖 P1 结论 |
| P4 | 功耗与续航验证（H04/H05/H07） | NOT_STARTED | 关 35mA/24h 目标 |
| P5 | 鲁棒性（T06 扇区门禁/H06 真断电/H03 多卡） | PARTIAL | 恢复核心已合并，扇区门禁 BLOCKED |

## 执行规则

- 本地开发分支隔离主分支历史；保留硬件 PDF、现有设计文档和用户配置。`PCB1.epro2` 已删除，仓库禁止嘉立创/EasyEDA 工程文件。
- 每个功能先运行失败测试，再实现并运行通过测试；开发脚本也验证失败退出与输出隔离。
- 支持对独立检查和任务审查使用子代理；所有实际修改有明确文件所有权。
- 不自动安装/替换全局 SDK；只烧录 COM7。阶段验证/审查通过后合入并推送，不强推。
- 不格式化、不删除或覆盖SD现有文件；真实录音与Flash备份留在忽略目录，不上传远程。
- 硬件快照身份及引脚继续以 [验证矩阵](MC100-VALIDATION.md) 为准；硬件事实以 `hardware/` 下 PDF 快照为准，不依赖云工程。

## 当前已取得的软件证据

2026-09-19最新统一测试19/19通过；14套C核心测试分别运行ASan/UBSan，共28/28通过，补充收尾24场景原生及ASan通过。目标固件按固定ESP-IDF构建和分区检查通过。原8MB Flash完整备份后才写入台架固件，芯片写入哈希校验通过。实际采集、卡格式与尚未完成项目见[实板报告](../reports/2026-09-19-evt-recording.md)。

2026-09-19：WAV 编码检查 0/2/640/9,600,000 byte 与非法奇数/越界长度；CRC 已知向量；64 byte 索引记录和512 byte头的每一位损坏均拒绝，重算 CRC 后的非法版本/保留字段也被拒绝。原生 MSVC C11 `/W4 /WX` 编译无告警、执行退出0。独立 Python 标准库 wave 校验 C 产出的 WAV 格式与320个样本，1/1通过。此结果只覆盖文件格式，不代表 SD 写入/断电恢复已实现。

2026-09-23：Phase 1 首轮只操作 COM7。芯片识别为 ESP32-S3 QFN56 revision v0.2、8 MB Flash、USB Serial/JTAG；实验前已读出 8 MB Flash 备份。固定 80 MHz PDM+libfvad 连续约10秒：501帧、320,640 bytes、0 timeout/error，libfvad P95 402 µs；固定 40 MHz 及显式 APB lock 对照均在 PDM 启动阶段触发 Task WDT，DFS（max80/min40）成功但 PDM 活跃时实际 CPU/APB=80/80 MHz。随后完成独立 Component Manager esp-sr 2.4.7 / VADNet1 medium 无 PSRAM target build，模型打包和 ESP32-S3 镜像生成 PASS；再用 COM7 做 runtime probe：`SR_MODEL_INIT result=OK`、发现 `vadnet1_medium`，但 AFE 创建在 `sr_rb_create` 报 `Memory exhausted` 后触发 `StoreProhibited` panic（`INIT FAIL / BLOCKED`），没有帧处理数据。完整 build/runtime 日志为 [`esp-sr-probe-c3-rerun-after-spiram.log`](../../firmware/out/phase1-evidence/esp-sr-probe-c3-rerun-after-spiram.log) 与 [`esp-sr-runtime-com7-20260923.log`](../../firmware/out/phase1-evidence/esp-sr-runtime-com7-20260923.log)。这不是电流、语料或公平 VAD 选择证据，P1 gate 仍 OPEN；该失败只约束当前模型/内存策略/无 PSRAM tuple，不代表所有 esp-sr 方案均不可行。详见 [Phase 1 PM/VAD spike 报告](2026-09-23-mc100-pm40-vad-spike.md)。

## 历史任务（已完成基线）

| 任务 | 状态 | 证据/下一步 |
| --- | --- | --- |
| T01 工程/BSP/测试入口 | PASS | 原生 CTest 1/1、脚本安全负例、ESP32-S3 干净构建通过；独立审查通过，提交 a93de0e |
| T02 状态/电池策略 | PASS_CORE | 审查问题已修复并复审通过；不等于电池实测/产品运行集成完成 |
| T03 组帧/预录/队列 | PASS_CORE | 含100万帧逐样本检查，统一测试及独立审查通过；实板采集零丢帧见报告 |
| T04 WAV/索引/CRC | PASS | 统一 CTest 4/4、目标干净构建、独立审查通过，提交8eb35dc；不代表存储事务/恢复已完成 |
| T05 写入/轮换/空间 | PASS_CORE | 四项电脑测试、真实目录适配、独立审查通过；实卡验证推进中 |
| T06 恢复/扇区故障 | BLOCKED | 恢复核心复审通过；精确 SDK 的 exFAT 扇区门禁失败，不能宣称任意掉电恢复 |
| T07 VAD/语料评估 | NOT_STARTED | 真实语料评估不可由强制语音标记替代 |
| T08 同源端到端仿真 | DEFERRED | 用户优先实测；不能将延期项写为通过 |
| T09 IDF 外设适配 | EVT_SLICE_PASS | 3秒与60秒采集通过；64GB exFAT的3秒及305秒录音完整CRC读回、300+5秒精确分段通过；声学/电气仍待测 |
| T10 监控/配置/诊断 | NOT_STARTED | 低电参数保留 EVT 标识 |
| T11 任务集成/资源 | EVT_SLICE | 单独Audio/Storage所有者；实测后Storage栈调至16KiB，余量40.4%，最终内部空闲堆126,856byte；完整自动State/VAD运行未完成 |
| T12 CI/候选包 | NOT_STARTED | 所有软件门禁通过才标 SW_READY_FOR_EVT |
| T13–T14 实板验证 | PARTIAL | 已有板，按安全顺序推进；电池、仪器、拔卡/断电和长期测试未执行 |
