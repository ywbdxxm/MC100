# MC100 验证与状态

更新时间：2026-09-26

本页是当前状态的唯一入口。状态含义：

- PASS：对应门禁有完整证据。
- PARTIAL：部分证据通过，仍有明确缺口。
- BLOCKED：已有具体阻塞，待排查。
- NOT RUN：尚未执行。
- DEFERRED：明确延期到后续阶段。
- OPEN：需要数据或决策，尚未放行。

## 1. 当前结论

| 项目 | 状态 | 证据和限制 |
| --- | --- | --- |
| 板级身份和原理图连接 | PARTIAL | ESP32-S3-MINI-1-N8、8 MB Flash、无 PSRAM 和引脚契约已由源码/原理图固定；制造和电气裕量未完成 |
| Host 默认 V1 套件 | PASS | 2026-09-26 复跑 17/17（历史报告的 16/16 后新增 PCM filter）；保留运行时套件 39/39；覆盖 PCM filter、record session、WAV/IDX、CRC、writer 和故障路径 |
| product 目标构建 | PASS | ESP-IDF v6.1、esp32s3、8 MB Flash、无 PSRAM、无线排除和自定义分区检查通过 |
| evt 目标构建 | PASS | 同一 SDK/目标下构建和配置检查通过 |
| EVT PDM 连续采集 | PASS（历史台架） | 80 MHz 路径约 10 秒、501 帧、无 timeout/error；只适用于记录中的硬件和配置 |
| EVT SD/WAV/IDX/CRC | PASS（历史台架） | 单卡 3 秒及约 305 秒分段读回通过；不等于 product 当前版本已验收 |
| product 自动录音至 IDLE | BLOCKED（当前可配置版本） | 镜像已烧录且 bootloader、分区、app 三段哈希校验通过；最近一次复位停在 ROM `DOWNLOAD (boot:0x0)`，未输出 `RECORDER_BOOT`。历史版本的 600 秒串口录音至 IDLE 另有记录；先检查 GPIO0/BOOT 和复位时序，再重测当前版本 |
| product WAV/IDX 读卡校验 | NOT RUN | 当前可配置版本尚未把目标文件交给读卡器并运行 verifier |
| PCM 处理器 | PASS（Host）；声学 OPEN | DC blocking、增益、跨帧、饱和和非法参数有 Host 证据；真实声压、噪声和听感未测 |
| 队列满和写入故障 | PASS（Host）；实板 NOT RUN | Host 验证 96 帧容量、故障锁存和禁止 clean close；真实 FreeRTOS/I2S/SD 调度仍待注入 |
| 40 MHz PDM | BLOCKED | 当前 ESP-IDF v6.1/ESP32-S3 tuple 的固定 40 MHz 启动实验卡在 I2S enable；活动 PDM 需要 80 MHz |
| VAD 选型 | OPEN | libfvad 有资源探针；无 PSRAM 的 esp-sr VADNet 配置初始化因内存不足失败，尚无公平语料比较 |
| 真断电恢复 | DEFERRED | V1 先完成正常收尾和读卡闭环 |
| 拔卡、满卡、扇区故障、多卡 | NOT RUN | 尚无目标板故障注入证据 |
| 电池、功耗、温升、耐久 | NOT RUN | 尚无校准仪器和长期记录 |
| 无线回传 | DEFERRED | 不进入 V1 |

## 2. 当前产品验收门槛

product V1 必须同时满足：

1. 目标设备上电后自动进入 RECORDING，不依赖 USB 命令。
2. 录音达到配置时长后停止，capture producer quiesce，writer clean close，进入 IDLE。
3. 录音过程中能观察到边采集边写入，不能只在结束时生成内存中的整段数据。
4. 通过读卡器取得同 stem 的 WAV/IDX，验证 WAV 参数、连续序号、每块 CRC 和 FINAL。
5. 固定声源条件下记录输入峰值、输出峰值、削波计数和主观听感；文件完整性不能替代声学结论。

当前可配置 product 最近一次复位尚未进入应用，上述五项实板验收都需要重新记录，因此 V1 尚未标记为完整 PASS。

## 3. 验证记录

- [2026-09-19 EVT 录音报告](reports/2026-09-19-evt-recording.md)：历史台架录音、exFAT、分段和 CRC 证据。
- [2026-09-25 最小录音记录](reports/2026-09-25-mc100-minimal-recorder.md)：历史 product 构建和串口观察；读卡器门禁保持未完成。
- [2026-09-19 exFAT 决策](reports/2026-09-19-exfat-decision.md)：文件系统选择背景。
- [2026-09-23 PM/VAD spike](software/2026-09-23-mc100-pm40-vad-spike.md)：功耗/VAD 探针，不是产品路径。
- [硬件复核](hardware/MC100-SCHEMATIC-REVIEW.md) 和 [PCB 复核](hardware/MC100-PCB-REVIEW.md)：板级依据。

历史记录中的串口、路径、卡容量和个人环境只描述当次实验，不能作为新开发者的固定前提。

## 4. 可复现命令

~~~powershell
pwsh -File firmware/tools/test-host.ps1 -Clean
pwsh -File firmware/tools/build.ps1 -Profile product -Clean
pwsh -File firmware/tools/build.ps1 -Profile evt -OutputDirectory out/target-evt -Clean
~~~

Host/Target 构建通过只证明对应软件层级通过；目标板、SD 卡、声学、电池和耐久证据必须单独记录。
