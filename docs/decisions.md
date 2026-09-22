# MC100 设计裁决

本文集中记录软件开发过程中不会从源代码结构直接看出的约束。它不替代架构设计、开发计划或验证报告；当实现与这些裁决冲突时，必须先记录新的迁移决定。

## 终态与发布

- 索引终态互斥：正常完成使用 `FINAL`，异常但已界定有效前缀使用 `INCIDENT`。两者都是终态，终态后不得追加记录；`FINAL` 的 flags/detail 为零，`INCIDENT` 必须带稳定的 reason 和 incomplete 标志。
- `snapshot_release` 不等于 session release。音频银行在 Storage 复制预录后可以释放，但旧 generation 的队列帧、关闭确认和会话 drain 仍拥有 session 生命周期；重触发不能重写消费者游标。

## 文件与恢复

- 恢复临时文件使用 `.recovering_N.wav.part`。写入后执行 sync 和 close，再以不覆盖方式重命名；正式文件已存在或临时文件可疑时都不覆盖，重试选择下一个编号。
- 恢复器必须验证标准 WAV 头、连续有效长度和每个 BLOCK CRC。它保留原始损坏文件，重复运行必须幂等；一个恢复成功不能推断预分配尾部有效。
- 故障路由按安全边界区分：MIC/队列溢出和健康写入者的 STORAGE_FULL 可以有界收尾为 `INCIDENT`；存储 I/O、关键电源、ADC 无效和超时类故障禁止新写入。诊断状态进入 FAULT 不会取消既有 deadline。
- FatFs `f_close` 隐含 `f_sync`，因此违反 no-write close 契约。脏关闭路径必须丢弃句柄并毒化挂载，不能把隐式写入当成成功收尾。

## 类型、错误与配置

- 故障码域不能数值直转：core 的 `CONTROL_TIMEOUT=9` 与 storage 的 `LOW_BAT_INTERRUPTED=9` 数值冲突，跨层必须按枚举名称映射。
- Wi-Fi 通过 `MINIMAL_BUILD` 和组件依赖排除；ESP-IDF v6.1 中隐藏的 `ESP_WIFI_ENABLED` SoC 默认值使单纯 Kconfig 置 `n` 无效。
- UBSan 主机验证使用 Zig 工具链；esp-clang 21.1.3 只提供嵌入式后端，不能冒充 x86_64 Windows UBSan。
- 64 GB exFAT 是已批准的 EVT 兼容范围扩展；FAT32 仍兼容。未知文件系统不得自动格式化，实卡掉电恢复尚未通过扇区门禁，不能宣称任意断电保证。

## 硬件证据

- 硬件事实以仓库 `hardware/` 下 2026-09-18 的原理图/PCB PDF 快照及其 SHA-256 为准。PDF 不包含 EasyEDA 的实时布线统计，因此 segments/arcs/vias/pours/fills 数字不能作为可复现的权威事实。
- `PCB1.epro2` 已删除且不得迁移、复制或归档。MC100 仓库、证据目录和后续提交禁止 `.epro2`、`.eprj`、`.easyeda/` 及其他嘉立创/EasyEDA 工程或工程缓存。
