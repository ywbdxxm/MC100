# MC100 软件说明

本页说明当前 V1 的数据流、代码边界和设计取舍。它不记录某次个人设备操作；当前状态和证据统一见 [MC100-VALIDATION.md](MC100-VALIDATION.md)。

## 1. V1 数据流

~~~text
product:
PDM RX → PCM 320 samples / 20 ms → DC blocking → digital gain
       → 96-frame bounded queue → storage owner → WAV/IDX writer → microSD
       → FINAL publication → IDLE

evt:
USB command → PDM RX/PCM → optional capture or writer → WAV/IDX → response
~~~

产品路径中，捕获任务拥有音频输入和 PCM 处理状态；存储任务拥有 SD、writer 和文件句柄。捕获和存储通过固定容量队列通信，队列满、超时、写入失败和收尾失败都会锁存故障，不能把未完成文件报告为 clean close。

录音按 20 ms 帧计数。默认时长 20 秒，宏允许 3..600 秒。writer 约 300 秒轮换一个 WAV/IDX 对，因此长会话会产生多个已发布分段；录音并不是等到结束才一次性写卡。

## 2. 固件 Profile 与 Host 测试

| 入口 | 责任 | 不包含 |
| --- | --- | --- |
| product | 上电、挂载 SD、创建 writer、持续录音、收尾、IDLE | USB 命令控制、VAD、无线、电池自动化 |
| evt | USB 命令触发采集/录音、下载和 CRC 读回 | 产品自动录音策略 |
| Host 可选 future 测试 | 旧 supervisor、预录、VAD 和状态测试 | 默认固件运行路径 |

product 和 evt 是两个目标固件 profile，使用同一套 WAV/IDX 格式、CRC 和 writer。Host 的 future 开关只增加本机测试，不生成第三种目标固件；EVT 的测试入口不能被误认为产品启动流程。

## 3. 组件职责

下表路径均相对于 `firmware/`。

| 路径 | 责任 | V1 地位 |
| --- | --- | --- |
| components/mc100_platform_espidf | I2S PDM、SD/FAT、板级 IO 和时间 | 必需 |
| components/mc100_audio | 帧组装、跨帧 PCM filter | 必需；VAD/预录文件保留 |
| components/mc100_recorder | 时长、帧数上限、deadline、产品设置 | 必需 |
| components/mc100_storage | WAV、IDX、CRC、journal、writer、恢复 | 必需 |
| components/mc100_core | 通用类型、状态和电池/上传接口 | 类型依赖；未来功能 |
| components/mc100_supervisor | 旧生命周期编排 | future；默认 product 不链接 |
| main/record_loop.c | 自动录音入口、队列和任务 owner | product 入口 |
| main/evt_capture.c、evt_commands.c | USB 诊断入口 | evt 入口 |
| host/、tests/ | 假 IO、格式测试、故障注入和协议测试 | 不进入固件 |

仓库文件数量大于 V1 运行图，是因为存储可靠性、错误注入、旧 runtime 和 VAD spike 都需要独立测试。读者只需先看 product 入口及上表。

## 4. 产品设置

设置头文件为 firmware/components/mc100_recorder/include/mc100_record_settings.h：

| 宏 | 默认 | 范围 | 含义 |
| --- | ---: | ---: | --- |
| MC100_RECORD_DURATION_SECONDS | 20 | 3..600 | 第一帧开始计的秒数 |
| MC100_RECORD_DC_BLOCK_ENABLE | 1 | 0/1 | Q16 直流阻断 |
| MC100_RECORD_GAIN_X | 8 | 1..16 | 线性数字增益 |

PCM filter 使用跨帧状态、64 位中间值和 signed 16-bit 饱和。增益可用于听感实验，会同时放大噪声；DC blocking 消除的是慢变化偏置，不会增加麦克风实际灵敏度。RECORDER_BOOT 报告解析后的设置，RECORDER_STOP 报告输入峰值、输出峰值和削波计数。

## 5. 文件和故障语义

- WAV 保存 16 kHz、16-bit、单声道 PCM。
- IDX 保存数据块的序号、偏移和 CRC；FINAL 表示正常收尾。
- .part 表示尚未发布的临时文件；INCIDENT 或故障状态不能当作完整录音。
- writer 通过 generation、预分配、CRC 和 publication 防止旧会话或未完成尾部被误读。
- 恢复逻辑只在 Host 和明确的目标测试中启用；真实掉电恢复尚未放行。

## 6. 当前明确不做的事情

V1 不引入新的无线协议、AI 模型、自动 VAD、复杂后台任务或电池状态机。VAD/预录只有在最小录音闭环、内存预算、功耗和真实音频数据齐备后再评估。

## 7. 构建和验证入口

~~~powershell
pwsh -File firmware/tools/test-host.ps1 -Clean
pwsh -File firmware/tools/build.ps1 -Profile product -Clean
pwsh -File firmware/tools/build.ps1 -Profile evt -OutputDirectory out/target-evt -Clean
~~~

构建结果、Host 结果、目标板结果和未完成门禁只在 [MC100-VALIDATION.md](MC100-VALIDATION.md) 维护。
