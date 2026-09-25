# MC100 固件开发指南

本页只说明如何构建、配置和验证固件。当前状态见 [验证与状态](../docs/MC100-VALIDATION.md)，架构说明见 [软件说明](../docs/MC100-SOFTWARE.md)。

## 目标和 Profile

项目目标是 ESP32-S3-MINI-1-N8、8 MB Flash、无 PSRAM，目标名为 esp32s3。dependencies.lock.json 锁定 ESP-IDF 的版本、revision 和目标；构建脚本校验当前 SDK、工具链和 Python 属于同一安装环境。

| Profile | 入口 | 用途 |
| --- | --- | --- |
| product | main/app_main.c → record_loop.c | 上电自动录音，默认 20 秒，结束后 IDLE |
| evt | main/app_main.c → evt_capture.c、evt_commands.c | USB 命令触发采集或录音，供台架诊断 |

两个 profile 共享平台、PDM、SD、WAV/IDX 和 writer。EVT 的额外音频、预录和 VAD 代码是诊断/未来资产，不会自动进入 product 图。

## Host 测试

Windows 下先进入具备 MSVC 的 x64 Developer PowerShell，再激活项目 ESP-IDF 环境。ESP-IDF 环境可能把用于固件的 Clang 放在 PATH 前面；运行 Host CMake 前显式选择本机编译器：

~~~powershell
$env:CC = (Get-Command cl.exe).Source
~~~

~~~powershell
pwsh -File firmware/tools/test-host.ps1 -Clean
~~~

需要 future 运行时测试时：

~~~powershell
pwsh -File firmware/tools/test-host.ps1 -Clean -FutureRuntimeTests
~~~

默认套件验证 V1 可移植逻辑。它不代替 PDM 时序、SD 卡、声学、电池、功耗或掉电测试。

## Target 构建

先激活与 dependencies.lock.json 匹配的 ESP-IDF 环境，再使用项目脚本：

~~~powershell
pwsh -File firmware/tools/build.ps1 -Profile product -Clean
pwsh -File firmware/tools/build.ps1 -Profile evt -OutputDirectory out/target-evt -Clean
~~~

脚本默认把输出放在 firmware/out/target/。脚本的默认 Profile 是 evt；需要产品固件时必须显式写 `-Profile product`。上面的 evt 示例使用独立输出目录，保证后续烧录步骤仍指向 product；切换同一输出目录的 profile 时必须使用 `-Clean`。不要把 Host 输出目录和 Target 输出目录混用，也不要用未校验的裸 idf.py build 代替项目脚本。

VS Code ESP-IDF 插件的 Build 使用插件配置的构建目录和 `sdkconfig`，不会自动调用 `build.ps1`。在插件输出中查看实际的构建目录和配置文件；需要可复现的 product 镜像时运行上面的 `-Profile product` 命令。`firmware/out/target/` 是项目脚本的输出目录，不是插件构建输出的通用位置。

sdkconfig 是某个输出目录的最终解析配置；sdkconfig.defaults 是公共默认值，sdkconfig.product.defaults 和 sdkconfig.evt.defaults 是 profile 覆盖，sdkconfig.ci 供 CI 使用。项目脚本通过 SDKCONFIG_DEFAULTS 组合这些文件并生成实际构建目录中的 sdkconfig。

## Product 烧录和日志

完成 `-Profile product` 构建后，从仓库根目录执行下列命令。将 `<PORT>` 换成目标板实际枚举出的串口；`@flash_args` 要在生成它的输出目录中使用。

~~~powershell
$port = '<PORT>'
Push-Location firmware/out/target
python -m esptool --chip esp32s3 --port $port write-flash '@flash_args'
Pop-Location

Push-Location firmware
idf.py -B out/target -p $port monitor
Pop-Location
~~~

观察 `RECORDER_BOOT`、`RECORDER_MOUNT`、`RECORDER_PREPARE`、`RECORDER state=RECORDING`、`RECORDER_STOP` 和 `RECORDER state=IDLE`。确认正常收尾后断电取卡，将 SD 卡插入读卡器，验证本次生成的 WAV/IDX。`mc100_serial.py` 的 status/list/record 等命令适用于 evt 固件。

## 产品录音设置

编辑 firmware/components/mc100_recorder/include/mc100_record_settings.h：

~~~c
#define MC100_RECORD_DURATION_SECONDS 20
#define MC100_RECORD_DC_BLOCK_ENABLE 1
#define MC100_RECORD_GAIN_X 8
~~~

约束如下：

- 时长 3..600 秒，按 20 ms 帧换算目标帧数，另有时长加 10 秒的安全 deadline。
- DC blocking 为 0 或 1，状态跨帧保持。
- 数字增益为 1..16 倍，输出饱和到 signed 16-bit。

改动后重新构建 product。处理器只在 product 路径使用；EVT 的 USB 录音保持原始采集路径。录音边采集边写卡，writer 约每 300 秒轮换一段。正常短录音预期生成一对 WAV/IDX；reserve .part 文件是否出现取决于 writer 的预分配和收尾路径。

## EVT 串口工具

串口必须显式传入，不假定某个操作系统端口：

~~~powershell
python firmware/tools/mc100_serial.py --port <PORT> status
python firmware/tools/mc100_serial.py --port <PORT> capture 3
python firmware/tools/mc100_serial.py --port <PORT> record 3
python firmware/tools/mc100_serial.py --port <PORT> list
python firmware/tools/mc100_serial.py --port <PORT> download <final-filename> firmware/out/<new-file>
python firmware/tools/verify_recording.py <local.wav> <local.idx>
~~~

capture 接受 3..60 秒，只统计 PCM；record 接受 3..600 秒并写入 SD。下载工具只允许把新文件写到 firmware/out/ 下。工具不会枚举端口、格式化 SD、删除文件或发送协议外命令。

目标板复位后如果进入 ROM 下载模式，先检查启动 strap、复位和供电，再重复 product smoke；不要把烧录成功当作应用已经运行。

## 录音文件

录音以 WAV 和同 stem 的 IDX 文件发布。IDX 记录每个数据块的序号、偏移和 CRC；文件末尾的 FINAL 才表示正常收尾。使用读卡器取出目标文件后运行 verify_recording.py。RECORDER_STOP 日志中的 written、discarded、overflow、fault、峰值和削波计数用于判断目标运行是否干净，但不能代替 WAV/IDX 读卡校验或声学测试。

## 安全边界

- 不自动格式化、删除或覆盖 SD 卡历史数据。
- 烧录前确认设备身份并按需保存 Flash 备份。
- 构建、串口 smoke、读卡验证和声学试听是独立证据，必须分别记录。
