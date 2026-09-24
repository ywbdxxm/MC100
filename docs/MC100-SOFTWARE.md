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
| `components/mc100_platform_espidf` | I2S PDM、SD/FAT、板级 IO、产品运行时 | 目标适配层 |
| `components/mc100_audio` | PCM 帧组装、预录银行、有界队列、VAD 接口 | 可复用核心；VAD 仍是占位 |
| `components/mc100_storage` | WAV、索引、CRC、写入和恢复 | 已有较完整实现，实板仍需验证 |
| `components/mc100_core` | 状态机、电池策略、上传 no-op 接口 | 状态机偏产品化，上传接口暂不使用 |
| `components/mc100_supervisor` | 录音生命周期编排 | 已有 Host 测试，尚未完成产品实板 smoke |
| `main/evt_capture.c` | 手动 USB 台架工具 | 保留作 EVT 诊断，不是最终产品流程 |
| `host/`、`tests/` | Host 假 IO、格式/故障/生命周期测试 | 测试资产，不是固件功能 |

## 3. 当前运行路径

EVT 路径用于确定性验证：由 USB 命令启动采集或录音，写 WAV 和索引，再由工具下载、校验和读回。

产品路径已经有 `app_main`、`product_runtime` 和 supervisor，但尚未在 COM7 完成以下完整链路：

```text
BOOT → LISTEN → RECORD → CLOSE → LISTEN
```

当前产品运行时的 VAD 是固定序号占位实现，不是最终语音 VAD。

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
- 产品运行时尚未通过 COM7 完整 BOOT 到 LISTEN/RECORD/CLOSE smoke。
- 真断电、扇区级故障、多卡、声学、电池和长期耐久均未放行。

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
```

Target：在项目锁定的 ESP-IDF v6.1 环境中运行：

```powershell
pwsh -File firmware/tools/build.ps1 -Profile evt -Clean
```

只在明确需要产品路径时构建 `product` profile。不要混用 SDK、Python、CMake 或 Ninja 环境。
