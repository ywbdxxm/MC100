# MC100 软件

## 1. 当前范围

第一目标是让板子稳定完成：

```text
PDM → PCM → WAV → microSD
```

无线回传不在当前软件范围内。VAD、预录和掉电恢复已有部分实现，但先作为独立验证项处理，不能把 Host 通过写成最小录音链路的实板完成。

## 2. 当前代码分区

| 目录 | 作用 | 当前判断 |
| --- | --- | --- |
| `components/mc100_platform_espidf` | I2S PDM、SD/FAT、板级 IO | 目标适配层 |
| `components/mc100_audio` | PCM 帧组装、预录银行、有界队列、VAD 接口 | 可复用核心；VAD 仍是占位 |
| `components/mc100_storage` | WAV、索引、CRC、写入和恢复 | 已有较完整实现，实板仍需验证 |
| `components/mc100_core` | 状态机、电池策略、上传 no-op 接口 | 状态机偏产品化，上传接口暂不使用 |
| `components/mc100_supervisor` | 旧录音生命周期编排 | 保留供 future profile，不在默认产品图中 |
| `components/mc100_recorder` | 可配置录音 session 策略和产品设置 | 默认产品路径；设置见 [`mc100_record_settings.h`](../firmware/components/mc100_recorder/include/mc100_record_settings.h)，策略见 [`mc100_record_session.h`](../firmware/components/mc100_recorder/include/mc100_record_session.h) |
| `main/evt_capture.c` | 手动 USB 台架工具 | 保留作 EVT 诊断，不是最终产品流程 |
| `host/`、`tests/` | Host 假 IO、格式/故障/生命周期测试 | 测试资产，不是固件功能 |

## 3. 当前运行路径

EVT 路径用于确定性验证：由 USB 命令启动采集或录音，写 WAV 和索引，再由工具下载、校验和读回。

默认产品路径由 `app_main` 启动 `record_loop`，上电后自动录音。捕获任务在产品路径中复制每个 320 样本帧，使用一个跨帧保持状态的 PCM 处理器完成可选直流阻断和数字增益，然后再入现有有界队列：

```text
BOOT → PDM/PCM → DC blocking → gain → WAV/IDX writer → configured duration → CLOSE → IDLE
```

每帧 20 ms，录音时长由 `MC100_RECORD_DURATION_SECONDS` 设置，合法范围为 3–600 秒，默认 20 秒；目标帧数为时长乘 50，安全 deadline 为时长加 10 秒。现有 writer 约 5 分钟轮换，因此超过 300 秒的 session 会产生多个 WAV/IDX 对；短 session 通常产生一对，可能保留一个 reserve `.part` 对。EVT USB 命令入口独立保留作诊断，不是产品流程。VAD、预录、电池自动化、无线和真实掉电恢复均延期。

### 产品音频设置

设置头文件是 `firmware/components/mc100_recorder/include/mc100_record_settings.h`：

| 宏 | 默认 | 范围 | 含义 |
| --- | ---: | ---: | --- |
| `MC100_RECORD_DURATION_SECONDS` | `20` | `3..600` | 第一帧开始计的录音秒数 |
| `MC100_RECORD_DC_BLOCK_ENABLE` | `1` | `0/1` | Q16 直流阻断开关 |
| `MC100_RECORD_GAIN_X` | `8` | `1..16` | 线性数字增益，8 倍约 +18 dB |

宏值在编译期校验。修改后使用 `pwsh -File firmware/tools/build.ps1 -Profile product -Clean`，不能用裸 `idf.py build` 替代项目脚本。处理器使用 64 位中间值并在输出端饱和到有符号 16-bit；`RECORDER_BOOT` 打印 `duration_seconds`、`dc_block` 和 `gain_x`，`RECORDER_STOP` 在捕获任务退出后打印 `input_peak`、`output_peak` 和 `clipped_samples`。EVT 路径、writer、WAV/IDX 格式和 SD 文件协议保持不变。

增益只用于当前听感实验。它也会放大噪声，削波计数非零时说明输入或增益已超出 16-bit 动态范围；直流阻断不会增加真实交流信号幅度。文件完整性、峰值变化和试听结果都不能单独证明麦克风灵敏度、声孔或整机声学链路合格，声学结论必须在固定声源、距离、方向下另行验证。

## 4. 代码复杂度边界

当前实现包含双预录银行、96 帧队列、状态机、generation、CRC 索引、预分配和恢复器。这些机制服务于“自动触发且尽量不丢录音”的产品目标，但不应被误认为硬件本身必须如此复杂。

整理后的开发顺序是：

1. 先验证最小连续录音和 WAV 写卡；
2. 再验证卡延迟、轮换和掉电恢复；
3. 再决定是否启用 VAD、预录和静音结束；
4. 最后单独规划无线回传。

在第 1 步完成前，不新增无线、AI 模型、复杂配置或新的后台任务。

## 5. 当前已知问题

- 40 MHz PDM 启动实验未通过；DFS 活跃点为 80 MHz。
- esp-sr VADNet1 medium 当前无 PSRAM runtime 初始化失败；没有完成 VAD 选型。
- 旧产品版本已通过 COM7 boot → 600 s continuous record → IDLE 串口 smoke；当前默认 20 秒、带 PCM 处理版本需重新执行目标构建、串口 smoke 和 SD 文件 CRC/FINAL 读回。
- 真断电恢复仍延期；扇区级故障、多卡、声学、电池和长期耐久均未放行。

## 6. 保留的安全规则

- 固件不自动格式化、删除或覆盖 SD 卡上的历史录音。
- 音频采集和存储各有明确 owner；音频路径不能等待 SD 写入，监控任务不能直接操作 I2S 或文件句柄。
- 录音文件正常结束写 `FINAL`；有界故障前缀写 `INCIDENT` 或保留 `.part`，不能把未验证尾部当作完整录音。
- 启动恢复只生成新副本并保留原件；恢复算法通过 CRC 和连续前缀判断有效数据，不能凭预分配长度猜测录音。
- 任何 Host、Target 或 EVT 结果只对对应层级成立，未运行的实板项目保持 `NOT RUN`。

## 7. 构建

Host：在 MSVC Developer PowerShell 中运行：

```powershell
pwsh -File firmware/tools/test-host.ps1 -Clean
pwsh -File firmware/tools/test-host.ps1 -Clean -FutureRuntimeTests
```

Target：在项目锁定的 ESP-IDF v6.1 环境中运行：

```powershell
pwsh -File firmware/tools/build.ps1 -Profile evt -Clean
pwsh -File firmware/tools/build.ps1 -Profile product -Clean
```

只在明确需要产品路径时构建 `product` profile。不要混用 SDK、Python、CMake 或 Ninja 环境。
