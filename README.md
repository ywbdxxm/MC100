# MC100

MC100 是一块基于 ESP32-S3-MINI-1-N8 的便携录音板：PDM 麦克风采集声音，ESP32-S3 转换为 PCM，microSD 保存 WAV 文件。无线能力只保留给后续规划，本阶段不启用、不验证。

## 当前目标

当前只整理并验证 V1 的本地录音链路。产品模式上电自动录音，默认 20 秒，随后停录并保持 IDLE。时长、直流阻断和数字增益都是编译期设置：

```text
上电 → PDM 采集 →（产品路径）直流阻断 → 数字增益 → 有界 PCM 队列 → 现有 writer → microSD → WAV/IDX → IDLE
```

录音策略和由时长推导的帧数上限见 [recorder-session policy](firmware/components/mc100_recorder/include/mc100_record_session.h)。自动 VAD、2 秒预录、低电自动化、无线回传和真实掉电恢复保留供后续规划，当前不在产品入口中。

### 产品录音设置

编辑 [`mc100_record_settings.h`](firmware/components/mc100_recorder/include/mc100_record_settings.h) 后重新构建 `product` 固件：

| 宏 | 默认值 | 合法范围 | 作用 |
| --- | ---: | ---: | --- |
| `MC100_RECORD_DURATION_SECONDS` | `20` | `3..600` | 首个有效 PCM 帧起算的录音秒数 |
| `MC100_RECORD_DC_BLOCK_ENABLE` | `1` | `0` 或 `1` | 启用 Q16 直流阻断 |
| `MC100_RECORD_GAIN_X` | `8` | `1..16` | 线性数字增益；8 倍约为 +18 dB |

例如设置为 10 秒、4 倍增益：

```c
#define MC100_RECORD_DURATION_SECONDS 10
#define MC100_RECORD_DC_BLOCK_ENABLE 1
#define MC100_RECORD_GAIN_X 4
```

然后在锁定的 ESP-IDF v6.1 环境中构建：

```powershell
pwsh -File firmware/tools/build.ps1 -Profile product -Clean
```

增益用于听感和削波实验，可能同时放大噪声；它不是麦克风灵敏度或声学质量已经合格的证明。WAV/IDX 仍保持 16 kHz、16-bit、单声道格式，EVT USB 采集路径不使用这些产品处理设置。

## 硬件事实

- 主控：ESP32-S3-MINI-1-N8，8 MB Flash，无 PSRAM。
- 麦克风：ZTS6872SE PDM，DATA=GPIO1，CLK=GPIO2，SELECT 接地。
- SD：microSD，SDMMC 1-bit，DAT0=GPIO6，CLK=GPIO7，CMD=GPIO8，卡检测=GPIO5。
- 电池 ADC：GPIO4，来自 1 MΩ / 330 kΩ 分压。
- LED：GPIO21，高电平点亮。
- USB：GPIO19/20，原生 USB Serial/JTAG。
- SW1：直接控制 LDO_EN，断电没有软件预告。
- CHG#/PGOOD# 没有接到 MCU；SD 与系统共用 3.3 V，不能由软件单独断电重启。

硬件依据只认 [原理图 PDF](hardware/SCH_Schematic1_2026-09-18.pdf) 和 [PCB PDF](hardware/PCB_PCB1_2026-09-18.pdf)。

## 当前软件状态

- EVT 台架固件已经在 COM7、USB 供电、64 GB exFAT 卡上完成 PDM 采集、WAV/索引写入、轮换和 CRC 读回。
- Host 默认测试覆盖 V1 录音策略、帧组装、PCM 处理器、WAV、存储和 CRC；supervisor 等旧运行时测试仅在 future profile 中保留。
- 旧产品 profile 已在 COM7 完成 boot → PDM → writer → 600 秒 → IDLE 串口 smoke。当前版本默认 20 秒并增加可配置 DC blocking/数字增益，锁定 ESP-IDF v6.1 的新目标构建、COM7 录音和 SD 文件 CRC/FINAL 读回仍需重新验证。
- 40 MHz PDM 启动实验失败；DFS 下 PDM 活跃时实际为 80 MHz。
- esp-sr VADNet1 medium 在无 PSRAM 板上的当前 runtime 初始化因内存耗尽失败；VAD 尚未定型。
- 真实声学、电池电流、长期耐久、真断电和多卡验证尚未放行。

## 文档入口

- [项目状态与验证](docs/MC100-VALIDATION.md)：唯一的软件/实板状态表。
- [硬件规格](docs/MC100-HARDWARE.md)：板级连接、限制和硬件验证边界。
- [软件说明](docs/MC100-SOFTWARE.md)：当前录音路径、代码结构和后续边界。
- [烧录指南](docs/hardware/MC100-PROGRAMMING.md)：USB 下载和测试点操作。

## 开发约束

- 只使用 COM7 做实板操作。
- 不格式化、删除或覆盖现有 SD 卡数据。
- 不恢复 EasyEDA/嘉立创工程文件。
- Target 使用项目锁定的 ESP-IDF v6.1；Host 测试使用 MSVC C11 环境。
- 构建通过不等于硬件放行；每项实板结论必须有记录。
