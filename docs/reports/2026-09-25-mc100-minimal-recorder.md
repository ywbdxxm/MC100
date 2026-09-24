# MC100 最小录音产品验证记录

日期：2026-09-25（Asia/Shanghai）。代码基线：`23b4c4c`。

本轮只验证上电自动录音、持续写入现有 WAV/IDX writer、30,000 帧结束和 IDLE。
COM7 串口观察与 PC 读卡器文件校验是两个独立门禁。本记录不提供 VAD、电池保护、
声学质量、真断电、耐久或无线的通过结论。

## 验证状态

| 门禁 | 状态 | 本轮证据 |
| --- | --- | --- |
| 默认 Host V1 套件 | PASS | 干净构建 16/16，独立 CTest 重跑 16/16 |
| Host 可控存储故障/会话策略 | PASS（限定范围） | focused 4/4；短写、空间不足、关闭失败、无帧 clean-close 拒绝 |
| product 构建及依赖图 | PASS | 项目脚本验证目标、SDK、配置、分区；无默认 Supervisor/VAD/旧 runtime |
| EVT 构建及诊断依赖图 | PASS | EVT 源入口及旧诊断帮助代码保留；不包含 record_loop.c |
| COM7 写入与哈希校验 | PASS | bootloader、partition-table、app 三个区域校验成功 |
| product 自动录音至 IDLE | SERIAL PASS | 660.139 s 观察，30,000 帧、2 次 publication、clean close、IDLE，无重启或第三次 publication |
| 两对 WAV/IDX 的 CRC、FINAL 和时长 | NOT RUN | PC 读卡器 E: 无介质；未取得本次文件 |
| V1 96 帧队列实际塞满故障注入 | NOT RUN | 默认测试只验证已锁存故障后的策略；未驱动真实 V1 队列塞满 |
| 实板缺卡/挂载失败的有界退出 | NOT RUN | 本轮使用已插卡，未操作卡或切电 |
| VAD、无线、自动电池策略、真断电恢复 | DEFERRED | 不属于 V1 本轮验收 |
| 声学与耐久 | NOT RUN | 未开展相应物理测试 |

## Host 测试

在 MSVC Developer PowerShell 中选择 MSVC `19.44.35228.0`，CMake `4.0.3`、
Ninja `1.12.1`；C11 构建使用仓库 `/W4 /WX /UNDEBUG` 检查。没有安装工具。

```powershell
pwsh -File firmware/tools/test-host.ps1 -Clean
ctest --test-dir firmware/out/host --output-on-failure
ctest --test-dir firmware/out/host --output-on-failure -V -R '^(record_session|storage_short_write|space_admission|finalize_faults)$'
```

结果依次为 `100% tests passed, 0 tests failed out of 16`（4.05 s）、相同 16/16
（3.35 s）、`100% tests passed, 0 tests failed out of 4`（0.29 s）。
16 个默认测试是 board_contract、wav、journal_codec、wave_interop、record_session、
frame_assembler、record_assembler_gap、storage_writer、storage_short_write、rotate_15000、
space_admission、finalize_faults、writer_publication、driver_contract、com7_client 和
recording_verifier。

`finalize_faults` 输出为 `15 hard failures, 5 short writes, 2 collisions, release retry
and normal bytes PASS`。`storage_short_write` 包含短写和空间不足的 INCIDENT 路径；
`record_session` 包含 30,000 帧上限、610 s deadline、producer quiescence、consumer
drain 及无帧不 clean-close 的策略断言。它的 queue fault 用例直接锁存 `MC100_IO`，
没有实际填满 V1 的 96 项 FreeRTOS 队列；future profile 的 `queue_full` 测试属于旧
audio queue，不能替代这个门禁。本轮未运行独立静态分析器或 sanitizer。

## Target 身份、构建和内存

目标为项目指定的 MC100 v1 / ESP32-S3-MINI-1-N8，8 MB Flash、无 PSRAM；配置为
80 MHz CPU、USB Serial/JTAG console。串口/ROM 本轮识别 ESP32-S3 QFN56 revision v0.2，
8 MB XMC Flash，制造商 `20`、器件 `4017`。器件配置值不等于供电实测。

锁定 SDK 是 ESP-IDF `v6.1`，commit
`fff9895c82d744c7237be8847347bdd1b07c6643`，SDK tracked files 保持干净。
本机激活元组：

- 激活脚本：`C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`
- IDF：`C:\esp\v6.1\esp-idf`
- tools：`C:\Espressif\tools`
- Python：`C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe`，3.14.7
- ESP32-S3 编译器：`xtensa-esp32s3-elf-gcc` 15.2.0，`esp-15.2.0_20251204`
- CMake 4.0.3，Ninja 1.12.1，esptool 5.4.0，pyserial 3.5

```powershell
pwsh -File firmware/tools/build.ps1 -Profile product -Clean
pwsh -File firmware/tools/build.ps1 -Profile evt -OutputDirectory out/target-evt -Clean
```

两次都返回 exit 0、`Project build complete.` 和
`MC100 target build verified; physical validation is recorded separately from compilation.`
EVT 使用独立输出目录，以保留烧录的 product 产物。SDK FatFs Kconfig 的既有 bool
`default 0` 提示仍存在，解析为 `n`；没有本轮编译错误。

| 项目 | product | EVT |
| --- | ---: | ---: |
| 实际 app bin 字节 | 305,520 | 317,744 |
| `esp_idf_size --format json2` image 统计 | 305,409 | 317,625 |
| DIRAM used / free | 119,963 / 221,797 | 57,731 / 284,029 |
| DIRAM `.bss` | 70,288 | 8,024 |
| main 源文件 | app_main.c, record_loop.c | app_main.c, evt_capture.c, evt_commands.c |
| mc100_audio 源文件 | frame_assembler.c | frame_assembler.c, audio.c, preroll.c, stream.c, vad_fixed.c |
| mc100_core | header-only | state.c, battery_policy.c, upload_noop.c |
| mc100_platform_espidf | audio_i2s.c, sd_fat.c, board_io.c | 相同 |

两种图均有 `mc100_recorder/record_session.c`，源于 IDF 要求依赖声明不依赖 CONFIG；
EVT 不引用产品 record loop，`nm` 核对最终 EVT ELF 不含 `mc100_record_run` 或
`mc100_record_session_*` 符号。product 中 `product_runtime.c`、`supervisor.c`、
`vad_fixed.c`、`preroll.c`、`state.c`、`battery_policy.c`、`upload_noop.c` 均不存在；
`mc100_supervisor`、`esp_wifi` 和 `bt` 不在组件图。`esp_psram` 仅为 SDK 的单个 MSPI shim，
`CONFIG_SPIRAM=n`。完整 72 项组件列表存于本轮 JSON。

产品静态 queue 为 96 × 648 = 62,208 byte；runtime `.bss` symbol 为 62,432 byte。
capture/storage 栈预算分别为 8,192 / 16,384 byte。以上链接期数字不是运行期剩余堆。
分区保持 nvs `0x9000/0x6000`、phy_init `0xf000/0x1000`、factory
`0x10000/0x300000`。

| 产物 | SHA256 |
| --- | --- |
| product mc100.bin | `1bc1531a697c606be49ec129446cbf9b66a316011174092573c96710748164e6` |
| product mc100.elf | `78f1f4c9c9bad516ee9b7aa9791694c31a69ac5646bd0f42232fe4c7c03fdd1c` |
| product bootloader.bin | `b5b5bcc0a43bd245fbb7e0023f103e52fa1ce2e169bbcb90c5a0ec6096ad2955` |
| partition-table.bin | `73c0b5c3e5fcba3a151cc70c453c93dd5f4798899e7f2f8cca76da1f32ffc501` |
| EVT mc100.bin | `ff3fc2ffa3d97c7e369c1e3bf2531ad024f470f3ff6408009f338b754dbba035` |
| 烧录前 8,388,608 byte Flash 备份 | `6c428112f1fd3ba858acbeeb3264ff0c323bcbe70921938c1691e4a07aedbe37` |

## COM7 操作与观察

只使用 COM7。先读取 ROM 芯片和 Flash 信息，再完整备份当前 8 MiB Flash，然后在
`firmware/out/target` 中使用该构建生成的 `@flash_args`：

```powershell
python -m esptool --chip esp32s3 --port COM7 --after no-reset flash-id
python -m esptool --chip esp32s3 --port COM7 --baud 460800 --before no-reset --after no-reset read-flash 0 0x800000 <new-local-backup>
python -m esptool --chip esp32s3 --port COM7 --baud 460800 --before no-reset --after no-reset write-flash '@flash_args'
```

flash 保持 bootloader，打开只读串口记录后调用 SDK 的 `esptool.reset.HardReset`
一次启动应用。记录从 `2026-09-24T21:14:03.509248Z`（本地 9 月 25 日 05:14:03）开始，
于 `2026-09-24T21:25:03.647505Z` 结束，共 660.139 秒。主机为每个串口行增加相对秒数，`HOST_*` 行是采集器标记；它们不属于设备
日志。先前的 EVT status/sync 请求在 30 秒后无回复超时，随后 ROM 身份、备份、烧录
均成功；没有把 status 超时当成产品失败或成功证据。

启动实测：`RECORDER_BOARD result=0`；exFAT 首次挂载成功，卷总容量
63,831,015,424 byte、准备前空闲 63,798,640,640 byte；`RECORDER_PREPARE result=0
elapsed_ms=469`，随后无需录音命令进入 `RECORDING`。本轮卡品牌/型号与 CID 未重新
确认；历史 2026-09-19 报告中的“用户确认闪迪 64 GB”仅作为历史背景。

600.970 s 时记录 `RECORDER_CLOSE result=0 clean=1 last_seq=29999`，并发布第二段；
600.972 s 进入 IDLE。两个串口报告的 publication 为：

| 相对主机时间 | generation / segment | WAV 文件名 |
| --- | --- | --- |
| 301.110 s | 1 / 0 | `adb119cfac90c3e5ac8ac4fd906f7104_1_0.wav` |
| 600.970 s | 1 / 1 | `adb119cfac90c3e5ac8ac4fd906f7104_1_1.wav` |

两者预期有同 stem 的 `.idx`，但本轮尚未读卡验证文件实际内容。终止日志为：

```text
RECORDER_STOP enqueued=30000 consumed=30000 written=30000 discarded=0 queue_peak=17 overflow=0 fault=0 reason=0 publications=2 quiesced=1 mounted=1 capture_stack_free=5048 storage_stack_free=7320 heap=260692 reset_required=0
RECORDER state=IDLE
```

队列峰值 17/96；日志中丢弃、DMA 溢出、故障与 incident reason 均为 0。
capture/storage 最低剩余栈为 5,048 / 7,320 byte（预算的 61.6% / 44.7%，IDF 的
`uxTaskGetStackHighWaterMark` 单位为 byte）；结束时内部空闲 heap 为 260,692 byte。
capture 已确认 quiesced，存储保持 mounted。串口状态表示本次会话 clean close，
不能单凭这一行证明卡上每个 BLOCK 的 CRC 或音频质量。
IDLE 后继续观察约 59.167 s，没有第三次 publication、自动重录或重启日志。
串口文件 SHA256：`17f4d2a0cb939152e3e9cb2472594729c44cafdfb6a08195e79001dd35870dfb`。
本轮串口门禁通过，包含读卡校验的完整产品验收仍为 PARTIAL PASS。

## PC 读卡验证和剩余门禁

电脑枚举到 E: 为 removable，但容量 0、无文件系统和介质。本轮无法从软件完成板子
物理断电、取卡并移到读卡器，因此未复制 WAV/IDX 或 reserve `.part`，也未对本次
硬件输出调用 `verify_recording.py`。`recording-verification.json` 中对应字段是
`NOT RUN`、`valid=null`、`complete=null`，不能将串口 publication 代替 CRC/FINAL 读回。
物理缺卡启动测试也未运行。

下一次操作应先确认 IDLE，再断开板子电源，将卡放入 PC 读卡器；只复制本轮命名的
两个 clean WAV/IDX 对和存在的 reserve pair 到证据目录，然后运行仓库 verifier。
期望每段 9,600,000 PCM byte、16 kHz/16-bit/mono、300 秒，合计 600 秒；这些数字在
取得真实文件前仍是期望值。PDM 启动丢弃的 1,280 byte / 40 ms 不计入有效 30,000 帧。

## 证据定位

- [带主机时间戳的 COM7 串口原始日志](../../evidence/2026-09-25-mc100-minimal-recorder/serial-com7.log)
- [验证状态、构建组件、Host 输出及哈希](../../evidence/2026-09-25-mc100-minimal-recorder/recording-verification.json)
- 本机完整构建/Host/flash 日志、Flash 备份和产品镜像归档位于本次 SDD 工作目录。

没有格式化、删除或手工覆盖卡数据，没有故障切电、修改 eFuse 或安装工具。正常
录音所需的 writer 创建新会话文件属于用户授权的验证行为。最终状态依赖本轮实际
日志和读回文件，其他历史验证行保持原结论。
