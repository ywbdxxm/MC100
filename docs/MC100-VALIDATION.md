# MC100 验证与状态

更新时间：2026-09-25

## 1. 当前结论

| 项目 | 状态 | 证据/说明 |
| --- | --- | --- |
| 硬件身份 | PARTIAL PASS | COM7 识别为 ESP32-S3、8 MB Flash、USB Serial/JTAG；原理图/PCB 快照已归档 |
| PDM 采集 | EVT PASS @80 MHz | 约 10 秒、501 帧、0 timeout/error |
| WAV/索引/CRC | HOST PASS | 格式边界、损坏拒绝、CRC 和 Python WAV 互操作已测 |
| SD 录音写入 | EVT PASS（单卡） | 64 GB exFAT，3 秒及约 305 秒记录读回通过 |
| Host 默认 V1 套件 | PASS (17/17) | PCM 处理器加入后，MSVC 干净构建与独立 CTest 为 17/17；可控存储故障/会话策略 focused 4/4；不代表实板 |
| product / EVT 目标构建 | product PASS；EVT 未重建 | 锁定 v6.1 的当前 product 图已通过，`mc100.bin` 与 bootloader 已生成并通过分区大小检查；EVT 本轮未重建 |
| 产品 boot → configured record → idle | 旧 600 s 串口 PASS；当前可调音频版本待实板 | 旧版本 COM7 660.139 s 观察：30,000 帧写入、2 次 publication、clean close、IDLE；queue 峰值 17/96，discard/overflow/fault 均 0。当前版本默认 20 s、DC blocking 开启、8 倍增益，需重新刷写并用读卡器校验 WAV/IDX；输入/输出峰值和削波计数也待记录 |
| 产品 PCM 处理器 | HOST PASS；声学 NOT RUN | DC blocking、增益、饱和、跨帧连续性、非法参数和小信号算术均已由 `test_pcm_filter` 覆盖。Host 结果不能证明麦克风灵敏度、声孔、PDM 电气裕量或播放听感 |
| V1 队列塞满故障注入 | 实板 NOT RUN；HOST PASS | 默认 Host 用 96 项 `mc100_frame_t` 队列模型验证第 97 帧拒绝、首个 `MC100_FULL` 锁存及 quiescence 前后禁止 clean close；只覆盖可移植会话策略，真实 FreeRTOS/I2S 调度及生产者停止仍未注入验证 |
| VAD 选型 | OPEN | libfvad 有 80 MHz 工程探针；esp-sr 当前 runtime 内存失败 |
| 40 MHz PDM | BLOCKED | 固定 40 MHz 在 PDM 启动阶段触发 Task WDT；DFS 活跃为 80 MHz |
| 真断电恢复 | DEFERRED | 不在当前 V1 产品验收范围；恢复核心仅有 Host 测试 |
| 扇区级故障 | NOT RUN | 真实卡故障注入和门禁尚未完成 |
| 电池、功耗、声学、耐久 | NOT RUN | 尚无发布结论 |
| 无线回传 | DEFERRED | 当前不启用，不进入本阶段验收 |

## 2. 已有证据

- [2026-09-19 EVT 录音报告](reports/2026-09-19-evt-recording.md)
- [2026-09-25 最小录音产品验证](reports/2026-09-25-mc100-minimal-recorder.md)
- [2026-09-19 exFAT 决策记录](reports/2026-09-19-exfat-decision.md)
- [2026-09-23 PM/VAD 实验](software/2026-09-23-mc100-pm40-vad-spike.md)
- [原理图复核](hardware/MC100-SCHEMATIC-REVIEW.md)
- [PCB 复核](hardware/MC100-PCB-REVIEW.md)

## 3. 当前优先级

### P0：整理和最小录音闭环

- 只保留一个项目入口、一个硬件摘要、一个软件摘要和一个状态页。
- 旧版本曾在 COM7 观察到自动录音至 IDLE；当前可调音频版本仍需完成 COM7 串口 smoke，并通过 PC 读卡器校验本次 WAV/IDX。
- 重新验证产品 PCM 处理版本：先用默认 20 秒/8 倍配置，再按需要调整 `MC100_RECORD_DURATION_SECONDS`、`MC100_RECORD_GAIN_X` 和 `MC100_RECORD_DC_BLOCK_ENABLE`。记录 `RECORDER_BOOT` 配置、`RECORDER_STOP` 峰值/削波计数，并用固定声源、距离和方向对比未处理基线。
- 补齐本次卡型号/CID、文件 CRC/FINAL，以及真实 FreeRTOS/I2S 队列饱和与生产者停止注入；Host 队列模型已覆盖策略边界。

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
- 产品完整验收需要 COM7 实板日志和读卡器校验共同完成；当前仅 `SERIAL PASS`，文件校验保持 `NOT RUN`，不能写作完整产品 PASS。
- 声学、电池、真断电和长期耐久必须有独立实板记录。
- 不把估算、电流预算或文档中的目标写成实测 PASS。

## 5. 音量实验边界

`MC100_RECORD_GAIN_X` 是产品路径上的整数数字增益，8 倍约为 +18 dB；它会同时放大噪声，输出削波由 `RECORDER_STOP` 的 clip 计数暴露。`MC100_RECORD_DC_BLOCK_ENABLE` 只去除慢变化直流估计，不会提升真实交流声压或麦克风灵敏度。WAV/IDX CRC、峰值变化和主观试听只能说明当前录音链路的处理结果，不能单独判定硬件声学合格。

放行声学结论前，必须在同一声源、距离、声孔方向和播放增益下比较原始路径与处理路径；若仍过低或噪声明显，应测量麦克风 VDD、PDM 时钟/数据和实际声孔路径，再决定是否修改硬件或 PDM 配置。

## 6. 可复现命令

```powershell
pwsh -File firmware/tools/test-host.ps1 -Clean
pwsh -File firmware/tools/test-host.ps1 -Clean -FutureRuntimeTests
pwsh -File firmware/tools/build.ps1 -Profile product -Clean
pwsh -File firmware/tools/build.ps1 -Profile evt -Clean
```

默认 Host 套件是 V1 录音路径；`-FutureRuntimeTests` 会在 V1 测试上增加保留的旧运行时测试。上述构建结果不替代 COM7 和 SD 卡物理证据。
