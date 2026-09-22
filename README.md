# MC100

MC100 是基于 ESP32-S3-MINI-1-N8 的便携录音设备软件与硬件记录仓库。硬件依据是 `hardware/` 中 2026-09-18 导出的原理图和 PCB PDF 快照，仓库不保存或上传任何嘉立创/EasyEDA 工程文件；`PCB1.epro2` 已删除且不得恢复。

## 当前状态

便携 C 核心、存储/录音组件和手动 USB 台架固件已实现。已在指定 COM7、USB 供电、64 GB exFAT 卡上完成采集、WAV/索引写入、轮换和 CRC 读回；这不等于声学质量、电池寿命或任意掉电恢复通过。

当前明确未完成：T06 扇区故障门禁阻塞，T07 VAD 评估未完成，产品 LISTEN/VAD 主流程尚未交付；尚无声学、电池、真实掉电和长期耐久放行。运行时目前尚未调用 `mc100_recover`。

## 构建与测试

主机测试和目标构建使用不同的 Windows 工具环境，不能混用：

- Host：使用 Visual Studio Developer PowerShell 的 MSVC C11、Windows SDK、CMake/Ninja/CTest。先建立 MSVC 环境，再运行 `pwsh -File firmware/tools/test-host.ps1 -Clean`；不要让 ESP-IDF profile 覆盖 MSVC 的 SDK 路径。
- Target：使用项目锁定的 ESP-IDF v6.1、对应 IDF Python/工具根目录、ESP32-S3 编译器、CMake 和 Ninja。激活匹配的安装专用 PowerShell profile 后运行 `pwsh -File firmware/tools/build.ps1 -Profile evt -Clean`。不要裸跑 `idf.py build`，也不要混用 uv Python 或另一套 SDK。

两类命令都从 `cmd.exe`/PowerShell 启动，不从 Git Bash 启动；`MSYSTEM`/`MINGW_*` 会使 ESP-IDF 拒绝激活。canonical 目标输出为 `firmware/out/target/`，IDE 的 clangd 编译数据库也指向该目录。

## 文档入口

- [软件架构设计](docs/software/2026-09-18-mc100-software-architecture-design.md)
- [软件开发计划](docs/software/2026-09-18-mc100-software-development.md)
- [设计裁决](docs/decisions.md)
- [硬件依据与验证矩阵](docs/software/MC100-VALIDATION.md)
- [开发状态](docs/software/MC100-DEVELOPMENT-STATUS.md)
- [硬件设计](docs/hardware/MC100-HARDWARE-DESIGN.md)、[PCB 复核](docs/hardware/MC100-PCB-REVIEW.md)、[原理图复核](docs/hardware/MC100-SCHEMATIC-REVIEW.md)、[烧录指南](docs/hardware/MC100-PROGRAMMING.md)
- [实板记录](docs/reports/2026-09-19-evt-recording.md)

## 证据与边界

不可再生的 COM7 实板证据和语料选择清单归档在 `evidence/`，由 `evidence/SHA256SUMS` 校验。构建缓存、私有录音、SD 卡内容和 EasyEDA 工程不纳入版本库。软件测试或交叉编译通过只证明对应软件层，不代表产品硬件放行；H01-H08、声学、功耗、掉电和制造风险必须以独立报告关闭。
