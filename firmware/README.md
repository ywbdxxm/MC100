# MC100 固件

目标板为 ESP32-S3-MINI-1-N8（8 MB Flash、无 PSRAM），锁定 ESP-IDF v6.1；精确目标与 SDK 修订见 `dependencies.lock.json`。当前有两种构建入口：

| Profile | 入口 | 用途 | 实板状态 |
| --- | --- | --- | --- |
| `evt` | `main/evt_capture.c` | USB 命令控制的采集和 SD 录音台架 | COM7 单卡通过 |
| `product` | `main/app_main.c` → `record_loop.c` → PDM/PCM 处理/有界队列/writer | 上电自动录音，默认 20 秒，通常生成一对 WAV/IDX 后进入 IDLE | 旧 600 秒版本曾通过 COM7 串口；新可调时长与增益版本待实板验证 |

无线组件在当前目标构建中排除。软件现状、硬件约束和验证记录分别见[软件说明](../docs/MC100-SOFTWARE.md)、[硬件说明](../docs/MC100-HARDWARE.md)和[验证与状态](../docs/MC100-VALIDATION.md)。

## Host 测试

使用 MSVC Developer PowerShell 的 C11、Windows SDK、CMake、Ninja 和 CTest：

```powershell
pwsh -File firmware/tools/test-host.ps1 -Clean
pwsh -File firmware/tools/test-host.ps1 -Clean -FutureRuntimeTests
```

第一个命令是 V1 默认套件（含可移植 PCM 处理器）；第二个命令在 V1 套件上增加保留的 future-runtime 测试。Host 测试验证可移植逻辑，不能代替板上 PDM、SD、电池、声学或掉电测试。

## Target 构建

先激活与 `dependencies.lock.json` 一致的 ESP-IDF 安装、Python、工具根目录和 ESP32-S3 编译器，再运行：

```powershell
pwsh -File firmware/tools/build.ps1 -Profile evt -Clean
pwsh -File firmware/tools/build.ps1 -Profile product -Clean
```

两个 profile 默认复用 `firmware/out/target/`；切换 profile 时使用 `-Clean` 或另设 `-OutputDirectory out/<name>`。脚本验证 SDK 修订、目标、配置、分区及无线/PSRAM 排除条件；不自动安装工具。不要混用 Host 与 Target 工具环境，也不要用裸 `idf.py build` 绕过项目脚本。`release` profile 尚不可用。

## 产品录音时长与音量

编译前编辑 [`components/mc100_recorder/include/mc100_record_settings.h`](components/mc100_recorder/include/mc100_record_settings.h) 中的三个 `#define`，然后运行 `pwsh -File firmware/tools/build.ps1 -Profile product -Clean`。默认如下：

```c
#define MC100_RECORD_DURATION_SECONDS 20  // 3..600 秒
#define MC100_RECORD_DC_BLOCK_ENABLE 1    // 0 或 1
#define MC100_RECORD_GAIN_X 8             // 1..16 倍
```

例如把时长改成 `10`、增益改成 `4` 即可调试 10 秒、4 倍录音。时长从第一帧开始计，目标帧数为秒数乘以 50，安全超时为设置时长加 10 秒。录音过程中边采集边写卡；writer 仍以最多约 300 秒为一段轮换。默认 20 秒正常结束时预期有一对完整 WAV/IDX，可能仍有预留的 `.part` 文件。关闭直流阻断并设置 1 倍增益可得到原始 PCM 路径；处理仅用于 `product`，`evt` 的 USB 录音不受影响。

8 倍增益约为 +18 dB，输出限制在 16-bit 范围内。它能提高文件播放音量，也会放大噪声并可能削波；查看 `RECORDER_STOP` 的输入峰值、输出峰值和削波计数，并在固定声源、距离和声孔方向下试听。文件格式或 CRC 校验通过不能证明声学质量合格。

## COM7 EVT 台架

只在 MC100 确认占用 COM7 且 Flash 已备份后使用。台架为 USB 供电、无电池的受限诊断入口，录音由命令触发，不自动格式化或删除卡上文件：

```text
python firmware/tools/mc100_com7.py status
python firmware/tools/mc100_com7.py capture 3
python firmware/tools/mc100_com7.py record 3
python firmware/tools/mc100_com7.py list
python firmware/tools/mc100_com7.py download <final-filename> firmware/out/<new-local-file>
python firmware/tools/verify_recording.py <local.wav> <local.idx>
```

`capture` 接受 3–60 秒，只采集 PCM；`record` 接受 3–600 秒并写 SD。下载路径必须是 `firmware/out/` 下的新文件。使用匹配 IDF 环境的 Python/pyserial。产品 V1 不等待电池 ADC；EVT USB 路径仍由命令触发并与产品路径分开，电池自动化延期。

原始实板证据见[2026-09-19 EVT 报告](../docs/reports/2026-09-19-evt-recording.md)。
