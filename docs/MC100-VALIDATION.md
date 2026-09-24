# MC100 验证与状态

更新时间：2026-09-25

## 1. 当前结论

| 项目 | 状态 | 证据/说明 |
| --- | --- | --- |
| 硬件身份 | PARTIAL PASS | COM7 识别为 ESP32-S3、8 MB Flash、USB Serial/JTAG；原理图/PCB 快照已归档 |
| PDM 采集 | EVT PASS @80 MHz | 约 10 秒、501 帧、0 timeout/error |
| WAV/索引/CRC | HOST PASS | 格式边界、损坏拒绝、CRC 和 Python WAV 互操作已测 |
| SD 录音写入 | EVT PASS（单卡） | 64 GB exFAT，3 秒及约 305 秒记录读回通过 |
| Host 生命周期测试 | HISTORICAL PASS | supervisor、存储、恢复测试已注册；本次文档整理未重跑测试 |
| 产品 BOOT→LISTEN→RECORD→CLOSE | NOT RUN | 产品固件尚未在 COM7 完整 smoke |
| VAD 选型 | OPEN | libfvad 有 80 MHz 工程探针；esp-sr 当前 runtime 内存失败 |
| 40 MHz PDM | BLOCKED | 固定 40 MHz 在 PDM 启动阶段触发 Task WDT；DFS 活跃为 80 MHz |
| 真断电/扇区故障 | NOT RUN/BLOCKED | 恢复核心有 Host 测试，真实卡门禁未关闭 |
| 电池、功耗、声学、耐久 | NOT RUN | 尚无发布结论 |
| 无线回传 | DEFERRED | 当前不启用，不进入本阶段验收 |

## 2. 已有证据

- [2026-09-19 EVT 录音报告](reports/2026-09-19-evt-recording.md)
- [2026-09-19 exFAT 决策记录](reports/2026-09-19-exfat-decision.md)
- [2026-09-23 PM/VAD 实验](software/2026-09-23-mc100-pm40-vad-spike.md)
- [原理图复核](hardware/MC100-SCHEMATIC-REVIEW.md)
- [PCB 复核](hardware/MC100-PCB-REVIEW.md)

## 3. 当前优先级

### P0：整理和最小录音闭环

- 只保留一个项目入口、一个硬件摘要、一个软件摘要和一个状态页。
- 在 COM7 验证产品固件最小录音闭环。
- 记录实际文件、卡型号、串口日志和失败原因。

### P1：存储可靠性

- 多卡和卡检测极性。
- 写入延迟、轮换、拔卡和空间不足。
- 启动恢复和真实断电；在证据不足前不宣称“任意掉电不丢”。

### P2：自动触发

- 选择并集成 VAD。
- 决定是否保留 2 秒预录和 15 秒静音结束。
- 用授权语料和板上声学场景验证召回、延迟和误触发。

### P3：电池与耐久

- ADC 标定、低电策略、电流和温升。
- 24/48 小时测试。
- 多板和装壳验证。

### P4：无线

- 另立需求和设计；当前只保留硬件能力，不实现回传。

## 4. 证据规则

- Host 测试通过只说明可移植逻辑通过。
- Target 构建通过只说明固件可构建。
- EVT 台架录音通过只说明台架路径在指定卡上工作。
- 声学、电池、真断电和长期耐久必须有独立实板记录。
- 不把估算、电流预算或文档中的目标写成实测 PASS。
