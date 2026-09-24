# MC100 硬件

## 1. 板级功能

MC100 的硬件只有一条核心数据链：

```text
ZTS6872SE PDM 麦克风 → ESP32-S3-MINI-1-N8 → microSD
```

电源由 USB-C、BQ24073、1S 锂电池和 TLV75733P 组成。SW1 直接控制 3.3 V LDO 的 EN。USB 还承担原生下载和日志。

## 2. 精确器件与连接

| 功能 | 器件/信号 | 连接 |
| --- | --- | --- |
| MCU | ESP32-S3-MINI-1-N8 | 8 MB Flash，无 PSRAM |
| PDM DATA | ZTS6872SE DATA | GPIO1 |
| PDM CLK | ZTS6872SE CLOCK | GPIO2，经 22 Ω |
| PDM SELECT | ZTS6872SE SELECT | GND |
| SD DAT0 | microSD DAT0 | GPIO6 |
| SD CLK | microSD CLK | GPIO7，经 R22=22 Ω |
| SD CMD | microSD CMD | GPIO8 |
| SD 检测 | microSD CD | GPIO5，上拉，极性待实板确认 |
| 电池 ADC | ADC_BAT | GPIO4，R6=1 MΩ、R7=330 kΩ |
| LED | LED1 | GPIO21，高电平亮 |
| USB | D−/D+ | GPIO19/20，原生 USB Serial/JTAG |
| 电源开关 | SW1 | 直接控制 LDO_EN |

### 电源限制

- CHG# 和 PGOOD# 未连接到 MCU，固件不能直接判断充电状态。
- SW1 断开后没有关机通知；录音只能依靠周期性同步和启动恢复降低损失。
- SD 与系统共用 `+3V3_SYS`，固件不能单独给卡断电。
- USB、SD 接口在 EVT 原理图中没有板载 ESD 保护，外部接口和制造风险仍需单独验证。

## 3. 已归档硬件依据

- [原理图快照（2026-09-18）](../hardware/SCH_Schematic1_2026-09-18.pdf)
- [PCB 快照（2026-09-18）](../hardware/PCB_PCB1_2026-09-18.pdf)
- [原理图复核](hardware/MC100-SCHEMATIC-REVIEW.md)
- [PCB 复核](hardware/MC100-PCB-REVIEW.md)
- [烧录指南](hardware/MC100-PROGRAMMING.md)

原理图复核、PCB 复核和烧录指南保留作为证据与细节来源；本页是当前开发使用的板级摘要。

## 4. 当前硬件验证边界

已在 COM7 观察到：芯片识别、8 MB Flash、USB Serial/JTAG、64 GB exFAT 卡、80 MHz PDM 连续采集和 SD 录音读回。

尚未关闭：

- PDM 槽位、电平、声学和装壳影响；
- 电池 ADC 标定、低电阈值和边充边录温升；
- 多卡、拔卡和长时间写入；
- 真断电恢复；
- USB/SD ESD 和 PCB 制造风险。

不要把芯片、SDK 或 Host 测试通过写成整机硬件放行。
