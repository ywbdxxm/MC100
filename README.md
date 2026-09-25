# MC100

MC100 是一块基于 ESP32-S3-MINI-1-N8 的本地录音板。当前产品目标只有一条链路：

~~~text
上电 → PDM 麦克风 → PCM 帧 → 可选直流阻断 → 可配置数字增益
     → 有界队列 → WAV/IDX writer → microSD → 录音结束 → IDLE
~~~

V1 默认上电自动录音 20 秒，录音过程中边采集边写入 microSD。无线、VAD、预录、电池自动化和真正掉电恢复不属于当前产品验收。

## 当前状态

当前状态以 [验证与状态](docs/MC100-VALIDATION.md) 为准：

- Host 可移植测试套件通过，覆盖帧处理、录音会话、WAV/IDX、CRC 和 writer 故障路径。
- ESP-IDF v6.1、ESP32-S3、product 和 evt 两个目标构建均通过。
- EVT 路径已有单卡录音、分段、CRC 和文件互操作的历史台架证据。
- 当前可配置 product 镜像已烧录并通过三段哈希校验，但最近一次复位停在 ROM `DOWNLOAD (boot:0x0)`，未进入应用；排查 BOOT/复位条件后，仍需完成自动录音和 WAV/IDX 读卡闭环。
- 声学质量、电池、功耗、耐久、拔卡/满卡和无线均未放行。

“构建通过”只说明源码和链接配置正确，不等于目标板录音或声学验收通过。

## 五分钟上手

### 1. 准备环境

firmware/dependencies.lock.json 锁定 ESP-IDF v6.1 的 revision 和 `esp32s3` 目标；构建脚本还会校验当前 SDK、工具链和 Python 属于同一安装环境。Host 测试需要 C11 编译器、CMake、Ninja、CTest 和 Python。

### 2. 运行 Host 测试

~~~powershell
pwsh -File firmware/tools/test-host.ps1 -Clean
~~~

默认套件是 V1 录音路径。Host 构建需要本机 C 编译器；Windows 环境中的编译器选择见[固件开发指南](firmware/README.md#host-测试)。保留的旧运行时测试可显式加入：

~~~powershell
pwsh -File firmware/tools/test-host.ps1 -Clean -FutureRuntimeTests
~~~

### 3. 构建目标固件

~~~powershell
pwsh -File firmware/tools/build.ps1 -Profile product -Clean
pwsh -File firmware/tools/build.ps1 -Profile evt -OutputDirectory out/target-evt -Clean
~~~

product 是 V1 产品路径；evt 是 USB 命令控制的诊断/验证路径。只需要产品录音时运行第一条命令。示例把 evt 放在独立输出目录，保留 `out/target` 中的 product 镜像。构建脚本未指定 Profile 时默认选择 evt，因此构建 product 必须显式写 `-Profile product`。两者共用 PDM、SD、WAV/IDX 和存储组件。构建脚本会校验 SDK、目标芯片、Flash、PSRAM、分区和被排除的无线组件。

### 4. 烧录和观察

使用目标设备实际枚举出的串口，将 `<PORT>` 替换为该端口。`flash_args` 中的镜像路径相对于构建输出目录，因此要先进入该目录：

~~~powershell
$port = '<PORT>'
Push-Location firmware/out/target
python -m esptool --chip esp32s3 --port $port write-flash '@flash_args'
Pop-Location

Push-Location firmware
idf.py -B out/target -p $port monitor
Pop-Location
~~~

不同平台的串口名称不同，仓库不假定固定端口。这里的 monitor 用于观察 product 的 `RECORDER_*` 日志；`mc100_serial.py` 的命令只用于 evt。等待 `RECORDER_STOP` 和 `RECORDER state=IDLE`，再断电取卡，用读卡器校验本次 WAV/IDX。烧录前应确认目标设备、备份必要镜像，并避免格式化或覆盖 SD 卡中的历史文件。

## Profile 和录音设置

| Profile | 入口 | 行为 |
| --- | --- | --- |
| product | main/app_main.c → main/record_loop.c | 上电自动录音，默认 20 秒，结束后 IDLE |
| evt | main/app_main.c → main/evt_capture.c | USB 命令触发采集或录音，用于诊断和存储验证 |

产品设置位于 firmware/components/mc100_recorder/include/mc100_record_settings.h：

| 宏 | 默认 | 合法范围 | 作用 |
| --- | ---: | ---: | --- |
| MC100_RECORD_DURATION_SECONDS | 20 | 3..600 | 从第一帧开始计的录音秒数 |
| MC100_RECORD_DC_BLOCK_ENABLE | 1 | 0/1 | Q16 直流阻断开关 |
| MC100_RECORD_GAIN_X | 8 | 1..16 | 线性数字增益 |

改动宏后重新构建 product。数字增益会同时放大噪声，不能当作麦克风灵敏度或声学合格证明。writer 仍按约 300 秒分段，短录音通常得到一对 WAV/IDX，长录音得到多对。

## 代码导航

下表路径均相对于 `firmware/`。

| 目录 | 职责 |
| --- | --- |
| components/mc100_platform_espidf | PDM、SD/FAT 和板级适配 |
| components/mc100_audio | PCM 帧组装、直流阻断、数字增益；VAD/预录代码保留供后续 |
| components/mc100_recorder | 录音时长、帧数上限、超时和产品设置 |
| components/mc100_storage | WAV、IDX、CRC、分段 writer 和恢复 |
| components/mc100_core | 通用状态、电池策略和上传接口 |
| components/mc100_supervisor | 保留的旧运行时编排，不在默认 product 图中 |
| main/record_loop.c | product 自动录音入口 |
| main/evt_capture.c、evt_commands.c | evt 诊断入口 |
| host/、tests/ | Host 测试和假 IO，不是固件运行功能 |
| tools/ | 构建、串口诊断和录音文件校验工具 |

仓库里测试、故障注入和 spike 文件较多，是为了验证边界和保留未来选项；V1 运行图只有上面列出的 product 路径。

## 开发路线

路线和每一项的验收门槛见 [MC100 路线图](docs/MC100-ROADMAP.md)：

1. P0：完成当前 product 的上电录音、边录边写、结束、WAV/IDX 读卡校验。
2. P1：补齐满卡、掉卡、写入故障、重启恢复和长时间耐久。
3. P2：在有真实数据和资源预算后评估 VAD、静音结束和预录。
4. P3：测量电池、功耗、温升和低电策略。
5. P4：单独设计无线回传，不把无线依赖带入 V1。

## 文档入口

- [验证与状态](docs/MC100-VALIDATION.md)：唯一的当前状态和证据矩阵。
- [软件说明](docs/MC100-SOFTWARE.md)：数据流、组件边界和运行路径。
- [硬件说明](docs/MC100-HARDWARE.md)：器件、引脚和板级限制。
- [固件开发指南](firmware/README.md)：构建、配置、烧录和文件校验。
- [路线图](docs/MC100-ROADMAP.md)：阶段目标、退出条件和下一步。
- docs/reports/：历史实验记录；不作为当前状态入口。
