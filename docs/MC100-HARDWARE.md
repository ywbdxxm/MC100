# MC100 硬件说明

本页是开发所需的板级事实和限制。原理图、PCB 和详细审阅记录是证据来源；本页不把某次串口实验写成永久硬件结论。

## 1. 核心链路

~~~text
ZTS6872SE PDM 麦克风 → ESP32-S3-MINI-1-N8 → microSD
~~~

电源由 USB-C、BQ24073、1S 锂电池和 TLV75733P 组成。SW1 直接控制 LDO_EN；USB 同时用于下载和日志。

## 2. 器件和引脚

| 功能 | 器件/信号 | 连接 |
| --- | --- | --- |
| MCU | ESP32-S3-MINI-1-N8 | 8 MB Flash，无 PSRAM |
| PDM DATA | ZTS6872SE DATA | GPIO1 |
| PDM CLK | ZTS6872SE CLOCK | GPIO2，经 22 Ω |
| PDM SELECT | ZTS6872SE SELECT | GND |
| SD DAT0 | microSD DAT0 | GPIO6 |
| SD CLK | microSD CLK | GPIO7，经 R22=22 Ω |
| SD CMD | microSD CMD | GPIO8 |
| SD 检测 | microSD CD | GPIO5，上拉，极性需实板确认 |
| 电池 ADC | ADC_BAT | GPIO4，R6=1 MΩ、R7=330 kΩ |
| LED | LED1 | GPIO21，高电平亮 |
| USB | D-/D+ | GPIO19/20，原生 USB Serial/JTAG |
| 电源开关 | SW1 | 直接控制 LDO_EN |

## 3. 板级限制

- CHG# 和 PGOOD# 未连接到 MCU，固件不能直接判断充电状态。
- SW1 断开后没有关机通知，录音不能依赖软件执行最后一次同步。
- SD 与系统共用 +3V3_SYS，软件不能单独给卡断电重启。
- USB 和 SD 接口在当前 EVT 原理图中没有板载 ESD 保护，接口和制造风险仍需单独验证。
- PDM 时钟、SELECT 槽位、声孔、供电噪声和装壳影响需要实测确认。

## 4. 设计依据

- [原理图快照](../hardware/SCH_Schematic1_2026-09-18.pdf)
- [PCB 快照](../hardware/PCB_PCB1_2026-09-18.pdf)
- [原理图复核](hardware/MC100-SCHEMATIC-REVIEW.md)
- [PCB 复核](hardware/MC100-PCB-REVIEW.md)
- [烧录指南](hardware/MC100-PROGRAMMING.md)

## 5. 验证边界

历史台架记录覆盖过芯片识别、8 MB Flash、原生 USB、exFAT 卡、80 MHz PDM 连续采集以及 SD 录音读回。它们证明对应目标、配置和测试条件下的链路可行，不等于所有板卡或所有卡都已放行。

当前仍需独立验证：

- 目标板 product 自动录音和 SD 读卡闭环；
- PDM 电气裕量、槽位、声孔和声学质量；
- 电池 ADC 标定、低电阈值、边充边录温升和功耗；
- 拔卡、满卡、写入故障、真断电恢复和长时间耐久；
- USB/SD ESD 和 PCB 制造风险。
