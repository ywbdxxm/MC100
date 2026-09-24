# MC100 固件

目标板为 ESP32-S3-MINI-1-N8（8 MB Flash、无 PSRAM），锁定 ESP-IDF v6.1；精确目标与 SDK 修订见 `dependencies.lock.json`。当前有两种构建入口：

| Profile | 入口 | 用途 | 实板状态 |
| --- | --- | --- | --- |
| `evt` | `main/evt_capture.c` | USB 命令控制的采集和 SD 录音台架 | COM7 单卡通过 |
| `product` | `main/app_main.c` → `product_runtime.c` → supervisor | 自主录音循环，当前使用固定序号 VAD 占位 | 尚未完成 COM7 全流程 smoke |

无线组件在当前目标构建中排除。软件现状、硬件约束和验证记录分别见[软件说明](../docs/MC100-SOFTWARE.md)、[硬件说明](../docs/MC100-HARDWARE.md)和[验证与状态](../docs/MC100-VALIDATION.md)。

## Host 测试

使用 MSVC Developer PowerShell 的 C11、Windows SDK、CMake、Ninja 和 CTest：

```powershell
pwsh -File firmware/tools/test-host.ps1 -Clean
```

Host 测试验证可移植逻辑；它不能代替板上 PDM、SD、电池或掉电测试。

## Target 构建

先激活与 `dependencies.lock.json` 一致的 ESP-IDF 安装、Python、工具根目录和 ESP32-S3 编译器，再运行：

```powershell
pwsh -File firmware/tools/build.ps1 -Profile evt -Clean
pwsh -File firmware/tools/build.ps1 -Profile product -Clean
```

两个 profile 默认复用 `firmware/out/target/`；切换 profile 时使用 `-Clean` 或另设 `-OutputDirectory out/<name>`。脚本验证 SDK 修订、目标、配置、分区及无线/PSRAM 排除条件；不自动安装工具。不要混用 Host 与 Target 工具环境，也不要用裸 `idf.py build` 绕过项目脚本。`release` profile 尚不可用。

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

`capture` 接受 3–60 秒，只采集 PCM；`record` 接受 3–600 秒并写 SD。下载路径必须是 `firmware/out/` 下的新文件。使用匹配 IDF 环境的 Python/pyserial。产品 profile 需要稳定有效的电池 ADC；USB 无电池台架不会绕过产品电池门禁。

原始实板证据见[2026-09-19 EVT 报告](../docs/reports/2026-09-19-evt-recording.md)。
