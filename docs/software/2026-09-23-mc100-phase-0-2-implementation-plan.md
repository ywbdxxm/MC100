# MC100 Phase 0–2 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: 用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务执行。步骤用 `- [ ]` 复选框跟踪。

**Goal:** 合并已完成的恢复核心、清理文档与台架的"乱"、用一个 throwaway spike 回答 40 MHz + VAD 选型、并构建能自主运行且掉电不丢的产品录音循环（BOOT→LISTEN→RECORD→CLOSE→LISTEN），即"录音-存储基本功能"。

**Architecture:** 复用现有全部组件（状态机、音频、存储、恢复均已写好且 host 可测）。Phase 2 新增唯一缺的东西——一个**可移植 C11 supervisor**，通过注入的时钟/IO/PCM 接口驱动状态机发出的动作（ARM/OPEN/STOP/CLOSE/RELEASE），并在启动调用 `mc100_recover`。台架 `evt_capture.c` 是同一套编排的手动版参考实现。ESP-IDF 侧只是把真实驱动 + FreeRTOS 接到 supervisor 的薄适配层。

**Tech Stack:** C11；ESP-IDF v6.1（`esp32s3`）；FreeRTOS；已封装的 FatFs/SDMMC/I2S-PDM；host 测试用 CMake/Ninja/CTest + MSVC（或 GCC/Clang）。

**Spec:** [docs/software/2026-09-23-mc100-final-form-design.md](2026-09-23-mc100-final-form-design.md)

## Global Constraints

（每个任务的要求都隐含包含本节，值逐字取自 spec/decisions。）

- **无 PSRAM**：所有实时缓冲必须放进内部 RAM，禁止依赖 PSRAM。
- **SDK 锁定**：ESP-IDF v6.1，目标 `esp32s3`，不随环境自动升级。本机 EIM 默认是 v5.5.5，构建前必须确认激活的是 v6.1（见 [[mc100-build-environments]]）。
- **MINIMAL_BUILD ON**：Wi-Fi/BLE 由最小组件依赖图排除（`firmware/CMakeLists.txt`）；Phase 0–2 不开无线。当前 126,856 byte 空闲堆是在此前提下测得。
- **FAT only**：SD 用 FAT32 / 已批准 64 GB exFAT；不自动格式化。不得引入 littlefs 等 PC 不可直接读的文件系统。
- **可移植核心 host/target 字节一致**：核心逻辑（含 supervisor）必须能在 host 编译并产出与 target 一致的行为；平台专属代码只在适配层。
- **稳定性优先**：sync 周期保持 1s、96 帧队列、2s 预录、CRC 索引——不为省电削弱（spec §3.2 定论）。
- **不删录音**：禁止自动删除、自动格式化、循环覆盖。
- **复用优先门**：每个任务动手前先确认没有现成可用；Phase 0–2 主要是"用已封装的驱动，不重写"，新造的只有 supervisor 编排（业务专属，无库可替）和两个 no-op/占位接口。
- **TDD**：每项先写可失败测试再实现（target-only 的适配层任务除外，以构建 + 实板 smoke 验证并注明）。
- **保留台架为证据**：`evt_capture.c` 不删，隔离为 EVT 诊断工具。
- **归属**：git commit 结尾加 `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`。

---

## File Structure

**Phase 0**（改动/合并）
- 合并 `codex/mc100-recovery`：新增 `recovery.c`/`mc100_recovery.h`/两套恢复测试，轻改 `writer.c`、两个 `CMakeLists.txt`。
- `docs/software/2026-09-22-mc100-repo-cleanup.md`：删除。
- `docs/software/2026-09-18-mc100-software-architecture-design.md`、`-development.md`：加状态横幅降级。
- `docs/software/MC100-DEVELOPMENT-STATUS.md`：任务表按新路线重写。
- `firmware/main/`：台架文件加 EVT 工具横幅（不改逻辑）。

**Phase 1**（throwaway spike，独立目录，不进产品）
- `firmware/spikes/pm40_vad/`：最小 probe 固件（测完即弃，只保留结论与数据）。

**Phase 2**（新增可移植 supervisor + 适配层）
- Create `firmware/components/mc100_core/include/mc100_upload.h` + `upload_noop.c`：上传接缝（no-op）。
- Create `firmware/components/mc100_audio/include/mc100_vad.h` + `vad_fixed.c`：VAD 决策接缝 + 占位实现。
- Create `firmware/components/mc100_supervisor/`：`include/mc100_supervisor.h` + `supervisor.c`（产品编排核心，可移植）。
- Create `firmware/tests/test_supervisor_*.c`：supervisor host 测试（真状态机+音频+writer，假 io+时钟+PCM）。
- Create `firmware/components/mc100_platform_espidf/product_runtime.c`：FreeRTOS 适配层。
- Modify `firmware/main/app_main.c`：构建开关选择产品 supervisor / EVT 台架。
- Modify `firmware/host/CMakeLists.txt`：注册新 host 测试。

---

## Phase 0：地基整理

清掉"乱"，让代码与文档认知一致。此 Phase 无新功能，全部是合并、删除、文档校正——但每步都有验证门。

### Task 0.1：合并恢复分支

**Files:**
- Merge: `codex/mc100-recovery` → `main`（新增 `firmware/components/mc100_storage/recovery.c` 等 8 个文件的改动）
- Test: `firmware/tests/test_recover_prefix.c`、`test_recover_idempotent.c`（分支自带）

**Interfaces:**
- Produces: `mc100_recover(const mc100_io_t *io, void *ctx, uint64_t deadline_ms, uint64_t (*now_ms)(void *), void *clock_ctx, mc100_recovery_report_t *report)` → `mc100_result_t`；`mc100_recovery_report_t { uint64_t valid_pcm_bytes; uint32_t recovered, preserved, invalid, timed_out; }`。Phase 2 的 supervisor 启动时调用它。

- [ ] **Step 1：确认合并无冲突（只读）**

```bash
cd "C:/Users/ljm75/Desktop/AI-HRADWARE/MC100"
base=$(git merge-base main codex/mc100-recovery)
git merge-tree "$base" main codex/mc100-recovery | grep -i "conflict\|<<<<\|changed in both" || echo "NO CONFLICTS"
```
Expected: 打印 `NO CONFLICTS`（已于计划编写时验证过）。

- [ ] **Step 2：在 main 上创建集成分支并合并**

```bash
cd "C:/Users/ljm75/Desktop/AI-HRADWARE/MC100"
git checkout main
git checkout -b phase0/merge-recovery
git merge --no-ff codex/mc100-recovery -m "feat: merge validated power-loss recovery core

Recovery core (recovery.c + prefix/idempotent tests) was completed and
reviewed on codex/mc100-recovery but never merged. T06 recovery logic is
done; only the exFAT sector-fault gate remains (deferred to Phase 5).

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```
Expected: 合并成功，无冲突。

- [ ] **Step 3：host 构建并跑恢复测试**

先在 `cmd.exe` 里建立 MSVC 环境（`vcvars64.bat`），不要从 Git Bash 跑（`MSYSTEM` 会干扰）。见 [[mc100-build-environments]]。
```
pwsh -File firmware/tools/test-host.ps1 -Clean
```
Expected: CTest 全绿，含新增的 `recover_prefix`、`recover_idempotent`（若脚本未自动注册，见 Task 0.1a）。

- [ ] **Step 4：确认恢复测试确实被注册并运行**

```bash
cd "C:/Users/ljm75/Desktop/AI-HRADWARE/MC100"
git show codex/mc100-recovery:firmware/host/CMakeLists.txt | grep -i "recover" || echo "NOT REGISTERED — see Task 0.1a"
```
Expected: 打印恢复测试的 `add_test` 行。若打印 `NOT REGISTERED`，先做 Task 0.1a 再回 Step 3。

- [ ] **Step 5：合并回 main**

```bash
cd "C:/Users/ljm75/Desktop/AI-HRADWARE/MC100"
git checkout main
git merge --ff-only phase0/merge-recovery
```
Expected: fast-forward 成功。

> **Task 0.1a（条件任务，仅当 Step 4 显示未注册时）：** 在 `firmware/host/CMakeLists.txt` 按现有 `foreach(name test_wav test_journal_codec ...)` 的同款模式，把 `recovery.c` 编进 `mc100_format` 库（或新建 `mc100_recovery` 库链接 `mc100_format`），并为 `test_recover_prefix`、`test_recover_idempotent` 各加 `add_executable` + `mc100_host_checks` + `add_test`。参考分支自带的 `firmware/tests/recovery_test_support.h`。加完重跑 Step 3。

### Task 0.2：删除过时清理文档、降级 09-18 文档

**Files:**
- Delete: `docs/software/2026-09-22-mc100-repo-cleanup.md`
- Modify: `docs/software/2026-09-18-mc100-software-architecture-design.md:3`
- Modify: `docs/software/2026-09-18-mc100-software-development.md`（顶部）

- [ ] **Step 1：删除一次性清理计划文档**

```bash
cd "C:/Users/ljm75/Desktop/AI-HRADWARE/MC100"
git rm docs/software/2026-09-22-mc100-repo-cleanup.md
```
Expected: 文件被暂存删除。（这解决了此前悬置的"删还是留"——它已被本计划取代。）

- [ ] **Step 2：给架构设计文档加降级横幅**

在 `docs/software/2026-09-18-mc100-software-architecture-design.md` 第 3 行的状态行后，紧接一段：
```markdown
> **状态更新（2026-09-23）**：本文档已降级为 **V1 离线核心基线**。产品最终形态（含无线回传）以 [最终形态设计](2026-09-23-mc100-final-form-design.md) 为准。本文"V1 不含 Wi-Fi/BLE"仅指离线核心阶段，不代表产品永久排除无线。离线架构内容仍然有效。
```

- [ ] **Step 3：给开发计划文档加降级横幅**

在 `docs/software/2026-09-18-mc100-software-development.md` 顶部标题下加：
```markdown
> **状态更新（2026-09-23）**：本文的 T01–T14 任务表已大半完成且认知过时，开发路线以 [最终形态设计 §7](2026-09-23-mc100-final-form-design.md) 与 [Phase 0–2 实施计划](2026-09-23-mc100-phase-0-2-implementation-plan.md) 为准。本文保留作历史语境与详细验收思路参考。
```

- [ ] **Step 4：提交**

```bash
cd "C:/Users/ljm75/Desktop/AI-HRADWARE/MC100"
git add -A docs/software/
git commit -m "docs: retire cleanup plan and downgrade 09-18 docs to V1 offline baseline

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

### Task 0.3：把台架标注为 EVT 诊断工具

不改逻辑，只加醒目横幅，明确它不是产品路径。

**Files:**
- Modify: `firmware/main/evt_capture.c:14-17`（已有一段注释，扩充）
- Modify: `firmware/main/app_main.c`（加文件级注释）

- [ ] **Step 1：扩充 evt_capture.c 顶部注释**

把 `evt_capture.c` 现有的 `/* This is a deliberately bounded USB bench path... */` 注释块替换为：
```c
/* ============================================================================
 * EVT_USB_BENCH — 手动 USB 台架诊断工具，不是产品固件。
 * 产品自主录音循环（BOOT/LISTEN/VAD/RECORD 状态机）见 mc100_supervisor 组件
 * 与 product_runtime.c；本文件保留作驱动验证证据与 EVT 诊断，经构建开关启用。
 * This is a deliberately bounded USB bench path, not LISTEN/VAD or a battery
 * safety state machine. The storage task alone owns mount, writer, and files.
 * The persistent audio task alone owns all I2S lifecycle operations.
 * ========================================================================== */
```

- [ ] **Step 2：提交**

```bash
cd "C:/Users/ljm75/Desktop/AI-HRADWARE/MC100"
git add firmware/main/
git commit -m "docs: label EVT bench as diagnostic tool, not product firmware

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

### Task 0.4：按新路线重写 DEVELOPMENT-STATUS 任务表

**Files:**
- Modify: `docs/software/MC100-DEVELOPMENT-STATUS.md`

- [ ] **Step 1：把"当前任务"表改为 Phase 视图**

保留文件其余部分，把任务表替换为反映真实状态的 Phase 表（关键更正：T06 恢复核心已合并，剩扇区门禁；产品循环 Phase 2 未开始）：
```markdown
| Phase | 内容 | 状态 | 证据/下一步 |
| --- | --- | --- | --- |
| P0 | 地基整理（合并恢复、清文档、隔离台架） | IN_PROGRESS | 恢复分支已合并，恢复测试 host 通过 |
| P1 | 40MHz + VAD 选型 spike | NOT_STARTED | throwaway，输出数据+选型结论 |
| P2 | 产品录音循环（supervisor） | NOT_STARTED | 先出基本功能；VAD 占位 |
| P3 | VAD（esp-sr/libfvad 择优 + 语料评估） | NOT_STARTED | 依赖 P1 结论 |
| P4 | 功耗与续航验证（H04/H05/H07） | NOT_STARTED | 关 35mA/24h 目标 |
| P5 | 鲁棒性（T06 扇区门禁/H06 真断电/H03 多卡） | PARTIAL | 恢复核心已合并，扇区门禁 BLOCKED |
```
并把旧的 T01–T14 表移到文件末尾"历史任务（已完成基线）"小节下保留。

- [ ] **Step 2：提交**

```bash
cd "C:/Users/ljm75/Desktop/AI-HRADWARE/MC100"
git add docs/software/MC100-DEVELOPMENT-STATUS.md
git commit -m "docs: restructure status to phase roadmap; correct T06 recovery state

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

**Phase 0 退出门：** `git status` 干净；host 测试全绿含恢复测试；文档层级一致（最终形态设计为权威，09-18 降级，STATUS 反映真实 Phase 状态）；repo-cleanup 文档已删。

---

## Phase 1：40 MHz + VAD 选型 spike（关键门禁，throwaway）

**这是 spike，不是 TDD 实现。** 输出是**数据 + 选型结论**，不是保留的产品代码。probe 固件放 `firmware/spikes/pm40_vad/`，验证完即弃（结论写回最终形态设计 §4/§8 与新 spike 报告）。**Phase 1 与 Phase 2 功能上独立**——Phase 2 用占位 VAD、不关心 CPU 频率，两者可并行；Phase 1 的产出喂给 Phase 3（选定 VAD）和 Phase 4（功耗架构）。

**复用门：** VAD 两个候选都是现成的——esp-sr VADNet（component manager: `espressif/esp-sr`）与 libfvad（git 依赖 `github.com/dpirch/libfvad`，纯定点 C 自己 wrap 成组件）。spike 的任务就是择优，不自己写 VAD。

- [ ] **Probe 1：基线降频可达性**
  最小固件：`esp_pm_configure` 设 `max_freq_mhz=40, min_freq_mhz=40`（或启用 DFS），开 I2S PDM RX 持续读，用 `esp_pm_get_configured_freq` / `rtc_clk_cpu_freq_get_config` 读实际 CPU/APB。
  **测量记录**：目标 40 MHz 是否被 PDM RX 的 PM 锁顶住降不下去（最终形态设计 §4 的头号风险）；实测 CPU/APB 频率。

- [ ] **Probe 2：libfvad 集成 + 开销**
  vendor libfvad 到 `spikes/pm40_vad/libfvad/`，包成 IDF 组件，跑 16 kHz/20 ms/mode 2；`esp_timer` 测单帧处理耗时，`heap_caps_get_free_size` 前后测 RAM。
  **测量记录**：无 PSRAM 下能否跑；每帧 CPU µs；RAM 占用。

- [ ] **Probe 3：esp-sr VADNet 可行性**
  component manager 拉 `espressif/esp-sr`，尝试无 PSRAM 下初始化 AFE VAD（`afe_config->vad_init=true`），或确认其硬性要 PSRAM。
  **测量记录**：无 PSRAM 是否可用；若可用，CPU/RAM vs libfvad。

- [ ] **Probe 4：电流实测**
  40 MHz vs 80 MHz、PDM+VAD 运行态，电池侧电流（需 H05 夹具或万用表串入）。
  **测量记录**：整机 mA，对照 spec §4 的 23–26 mA / 40–43 mA 估算。

- [ ] **Decision Gate + 报告**
  写 `docs/software/2026-09-2X-mc100-pm40-vad-spike.md`：40 MHz 可达性结论（可达/不可达 + 数据）、VAD 选型（esp-sr 或 libfvad + 理由）、对功耗架构的影响。把结论回填最终形态设计 §4 与 §8 风险表。删除 `spikes/pm40_vad/` 的 probe 代码（选定的 VAD 组件除外，留待 Phase 3 正式引入）。
  **退出门：** 有明确的"40 MHz 能否跑 PDM+VAD"答案和 VAD 选型，两者都有实测数据支撑。

---

## Phase 2：产品录音循环

从手动台架切片变为**能自主运行、掉电不丢**的产品循环。核心是新增可移植 `mc100_supervisor`，驱动已完成的状态机 + 音频 + 存储 + 恢复。

**Scope 边界（诚实声明）：** Phase 2 交付 happy path（BOOT→LISTEN→trigger→RECORD→silence→CLOSE→LISTEN）+ 启动恢复 + 基本故障收尾（.partial.wav + INCIDENT）+ no-op 上传接缝。状态机 `state.c` 已实现的更复杂路径（重触发、双会话、低电安全收尾）由 supervisor 执行其发出的动作即可覆盖基础场景；这些路径的穷尽实板验证在 Phase 4/5。VAD 用占位（固定 seq 触发），真 VAD 在 Phase 3。

**复用门：** 驱动全部复用已封装的（I2S/SDMMC/FatFs via `mc100_platform_*`）；FreeRTOS 用现成原语。新造的只有 supervisor 编排逻辑（产品专属业务，无库可替）和两个接缝接口。

### Task 2.1：上传接缝（no-op）

**Files:**
- Create: `firmware/components/mc100_core/include/mc100_upload.h`
- Create: `firmware/components/mc100_core/upload_noop.c`
- Create: `firmware/tests/test_upload_noop.c`
- Modify: `firmware/components/mc100_core/CMakeLists.txt`、`firmware/host/CMakeLists.txt`

**Interfaces:**
- Produces: `mc100_upload_sink_t { void (*notify_closed)(void *ctx, const char *final_name); void *ctx; }`；`mc100_upload_sink_t mc100_upload_noop(void)`。supervisor 在每个 segment 成功关闭后调用 `notify_closed` 恰好一次，传最终发布的文件名。

- [ ] **Step 1：写失败测试**

```c
/* firmware/tests/test_upload_noop.c */
#include "mc100_upload.h"
#include <assert.h>
#include <string.h>

static int spy_calls;
static char spy_last[128];
static void spy_notify(void *ctx, const char *name) {
    (void)ctx; ++spy_calls; strncpy(spy_last, name, sizeof(spy_last) - 1);
}

int main(void) {
    /* no-op sink 必须提供可调用的 notify_closed，且不崩溃 */
    mc100_upload_sink_t noop = mc100_upload_noop();
    assert(noop.notify_closed != NULL);
    noop.notify_closed(noop.ctx, "abc_1_0.wav");

    /* 自定义 sink 的接线正确性（supervisor 测试会用到 spy 模式） */
    mc100_upload_sink_t spy = { .notify_closed = spy_notify, .ctx = NULL };
    spy.notify_closed(spy.ctx, "abc_1_0.wav");
    assert(spy_calls == 1);
    assert(strcmp(spy_last, "abc_1_0.wav") == 0);
    return 0;
}
```

- [ ] **Step 2：运行确认失败**

`pwsh -File firmware/tools/test-host.ps1`
Expected: 编译失败（`mc100_upload.h` 不存在）。

- [ ] **Step 3：写接口与 no-op 实现**

```c
/* firmware/components/mc100_core/include/mc100_upload.h */
#ifndef MC100_UPLOAD_H
#define MC100_UPLOAD_H
#include "mc100_types.h"
/* Phase 6 无线回传的接缝。V1 为 no-op。产品循环在每个成功关闭的 segment 后
 * 调用 notify_closed 恰好一次，传最终发布的单个 MC100 文件名（非目录）。
 * V1 的实现不得阻塞产品循环或做 I/O。 */
typedef struct {
    void (*notify_closed)(void *ctx, const char *final_name);
    void *ctx;
} mc100_upload_sink_t;
mc100_upload_sink_t mc100_upload_noop(void);
#endif
```
```c
/* firmware/components/mc100_core/upload_noop.c */
#include "mc100_upload.h"
static void noop_notify(void *ctx, const char *final_name) {
    (void)ctx; (void)final_name;
}
mc100_upload_sink_t mc100_upload_noop(void) {
    mc100_upload_sink_t s = { noop_notify, (void *)0 };
    return s;
}
```

- [ ] **Step 4：注册 host 测试**

在 `firmware/host/CMakeLists.txt`：把 `upload_noop.c` 加入核心库源文件，按现有 `foreach` 模式加 `test_upload_noop` 的 `add_executable`/`mc100_host_checks`/`add_test`。在 `firmware/components/mc100_core/CMakeLists.txt` 的 `idf_component_register` SRCS 加 `upload_noop.c`。

- [ ] **Step 5：运行确认通过**

`pwsh -File firmware/tools/test-host.ps1`
Expected: `upload_noop` PASS。

- [ ] **Step 6：提交**

```bash
cd "C:/Users/ljm75/Desktop/AI-HRADWARE/MC100"
git add firmware/components/mc100_core firmware/tests/test_upload_noop.c firmware/host/CMakeLists.txt
git commit -m "feat: add no-op upload sink seam for future wireless offload

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

### Task 2.2：VAD 决策接缝 + 占位实现

**Files:**
- Create: `firmware/components/mc100_audio/include/mc100_vad.h`
- Create: `firmware/components/mc100_audio/vad_fixed.c`
- Create: `firmware/tests/test_vad_fixed.c`
- Modify: `firmware/components/mc100_audio/CMakeLists.txt`、`firmware/host/CMakeLists.txt`

**Interfaces:**
- Produces: `mc100_vad_t { bool (*decide)(void *ctx, const mc100_frame_t *frame); void *ctx; }`；`mc100_vad_t mc100_vad_fixed(mc100_vad_fixed_t *state, uint64_t trigger_seq)`；`mc100_vad_fixed_t { uint64_t trigger_seq; bool fired; }`。supervisor 在 LISTEN 中对每帧调 `decide`，返回值作为 `mc100_audio_push` 的 trigger 参数。占位实现在 seq==trigger_seq 时返回一次 true。

- [ ] **Step 1：写失败测试**

```c
/* firmware/tests/test_vad_fixed.c */
#include "mc100_vad.h"
#include <assert.h>

int main(void) {
    mc100_vad_fixed_t st;
    mc100_vad_t vad = mc100_vad_fixed(&st, 50);
    mc100_frame_t f = {0};
    f.seq = 49; assert(vad.decide(vad.ctx, &f) == false);
    f.seq = 50; assert(vad.decide(vad.ctx, &f) == true);   /* 触发一次 */
    f.seq = 51; assert(vad.decide(vad.ctx, &f) == false);
    f.seq = 50; assert(vad.decide(vad.ctx, &f) == false);  /* 不重复触发 */
    return 0;
}
```

- [ ] **Step 2：运行确认失败**

`pwsh -File firmware/tools/test-host.ps1`
Expected: 编译失败（`mc100_vad.h` 不存在）。

- [ ] **Step 3：写接口与占位实现**

```c
/* firmware/components/mc100_audio/include/mc100_vad.h */
#ifndef MC100_VAD_H
#define MC100_VAD_H
#include "mc100_types.h"
/* 人声决策接缝。Phase 3 用 Phase 1 spike 选定的 esp-sr VADNet 或 libfvad 替换。
 * Phase 2 用占位。decide 返回 true 表示该帧应视为触发沿。有状态实现把状态放 ctx。 */
typedef struct {
    bool (*decide)(void *ctx, const mc100_frame_t *frame);
    void *ctx;
} mc100_vad_t;
/* 占位：固定帧触发，供 bring-up。恰在 seq==trigger_seq 的帧触发一次，
 * 让 Phase 2 无需真实音频即可确定性地走完 LISTEN->RECORD->close。 */
typedef struct { uint64_t trigger_seq; bool fired; } mc100_vad_fixed_t;
mc100_vad_t mc100_vad_fixed(mc100_vad_fixed_t *state, uint64_t trigger_seq);
#endif
```
```c
/* firmware/components/mc100_audio/vad_fixed.c */
#include "mc100_vad.h"
static bool fixed_decide(void *ctx, const mc100_frame_t *frame) {
    mc100_vad_fixed_t *s = ctx;
    if (!s->fired && frame->seq == s->trigger_seq) { s->fired = true; return true; }
    return false;
}
mc100_vad_t mc100_vad_fixed(mc100_vad_fixed_t *state, uint64_t trigger_seq) {
    state->trigger_seq = trigger_seq; state->fired = false;
    mc100_vad_t v = { fixed_decide, state };
    return v;
}
```

- [ ] **Step 4：注册 host 测试**（同 2.1 模式，源文件 `vad_fixed.c` 进音频库，加 `test_vad_fixed`）

- [ ] **Step 5：运行确认通过** — Expected: `vad_fixed` PASS。

- [ ] **Step 6：提交**

```bash
cd "C:/Users/ljm75/Desktop/AI-HRADWARE/MC100"
git add firmware/components/mc100_audio firmware/tests/test_vad_fixed.c firmware/host/CMakeLists.txt
git commit -m "feat: add VAD decision seam with fixed-seq placeholder for bring-up

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

### Task 2.3：Supervisor 骨架 + 启动恢复 + 到达 LISTEN

建立可移植 supervisor 组件，注入依赖，实现 BOOT：调恢复 → 喂 READY → 执行 ACT_ARM → 到 LISTEN。

**Files:**
- Create: `firmware/components/mc100_supervisor/include/mc100_supervisor.h`
- Create: `firmware/components/mc100_supervisor/supervisor.c`
- Create: `firmware/components/mc100_supervisor/CMakeLists.txt`
- Create: `firmware/tests/test_supervisor_boot.c`
- Create: `firmware/tests/supervisor_test_support.h`（假 io/时钟/PCM 源，复用 `recovery_test_support.h` 的内存 io 若可用）
- Modify: `firmware/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `mc100_state_*`（state.h）、`mc100_audio_*`（audio.h）、`mc100_writer_*`（writer.h）、`mc100_recover`（recovery.h）、`mc100_upload_sink_t`、`mc100_vad_t`、`mc100_io_t`。
- Produces:
```c
typedef struct {
    const mc100_io_t *io;          /* 存储 io vtable */
    void *io_ctx;
    uint64_t (*now_ms)(void *);    /* 单调时钟 */
    void *clock_ctx;
    /* PCM 源：返回一帧原始 PCM 字节，count=0 表示暂无（超时）。 */
    mc100_result_t (*pcm_read)(void *ctx, uint8_t *buf, size_t cap, size_t *count, uint32_t timeout_ms);
    void *pcm_ctx;
    mc100_vad_t vad;
    mc100_upload_sink_t upload;
    const uint8_t *boot_id;        /* 16 byte */
} mc100_supervisor_deps_t;

mc100_supervisor_t *mc100_supervisor_create(const mc100_supervisor_deps_t *deps);
void mc100_supervisor_destroy(mc100_supervisor_t *s);
mc100_result_t mc100_supervisor_boot(mc100_supervisor_t *s);   /* 恢复 + 到 LISTEN */
mc100_result_t mc100_supervisor_tick(mc100_supervisor_t *s);   /* 处理一轮：读帧/推进状态/执行动作 */
mc100_state_id_t mc100_supervisor_state(const mc100_supervisor_t *s);
```
supervisor 内部持有 `mc100_state_t*`、`mc100_audio_t*`、`mc100_writer_t*`，`tick` 是主循环体（适配层反复调用）。

- [ ] **Step 1：写失败测试（启动到 LISTEN 且恢复被调用）**

```c
/* firmware/tests/test_supervisor_boot.c 摘要 */
#include "mc100_supervisor.h"
#include "supervisor_test_support.h"
#include <assert.h>

int main(void) {
    sup_fake_t fake;                    /* 内存 io + 虚拟时钟 + 罐装 PCM 源 */
    sup_fake_init(&fake);
    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    mc100_supervisor_t *s = mc100_supervisor_create(&deps);
    assert(s != NULL);

    assert(mc100_supervisor_boot(s) == MC100_OK);
    /* 启动必须调用恢复（空卡：recovered=0），并到达 LISTEN */
    assert(fake.recovery_called == 1);
    assert(mc100_supervisor_state(s) == MC100_LISTEN);
    mc100_supervisor_destroy(s);
    return 0;
}
```

- [ ] **Step 2：运行确认失败** — Expected: 编译失败（组件不存在）。

- [ ] **Step 3：实现 create/destroy + boot**

`supervisor.c` 的 boot 逻辑（用真 state/writer，注入 io/clock）：
```c
/* 伪代码骨架——用真实 API，实现时按 state.h 契约填全 */
mc100_result_t mc100_supervisor_boot(mc100_supervisor_t *s) {
    /* 1. 恢复先于音频启动运行，常量 RAM，30s 预算 */
    mc100_recovery_report_t rep = {0};
    uint64_t deadline = s->deps.now_ms(s->deps.clock_ctx) + 30000;
    mc100_result_t r = mc100_recover(s->deps.io, s->deps.io_ctx, deadline,
                                     s->deps.now_ms, s->deps.clock_ctx, &rep);
    if (r != MC100_OK && r != MC100_TIMEOUT) return r; /* RECOVERY_REQUIRED 时进 FAULT */
    /* 2. 喂 READY（detail>=2 表电池/恢复/驱动就绪），驱动状态机出 BOOT/ARM 动作 */
    mc100_event_t ev = { .id = MC100_EV_READY, .now_ms = s->deps.now_ms(s->deps.clock_ctx),
                         .detail = 2, .global = true };
    return sup_pump_event(s, &ev);  /* 见下：喂事件 + 执行动作 */
}
```
动作执行器 `sup_pump_event`（核心分发，本 task 先实现 ARM/BOOT，其余 task 补全）：
```c
static mc100_result_t sup_pump_event(mc100_supervisor_t *s, const mc100_event_t *ev) {
    mc100_action_t acts[8]; size_t n = 0;
    mc100_result_t r = mc100_state_step(s->state, ev, acts, &n);
    if (r != MC100_OK) return r;
    for (size_t i = 0; i < n; ++i) {
        switch (acts[i].id) {
        case MC100_ACT_ARM:
            r = mc100_audio_arm(s->audio, acts[i].generation, acts[i].seq_valid ? acts[i].seq : 0);
            if (r == MC100_OK) { mc100_event_t a = { .id = MC100_EV_ARMED, .generation = acts[i].generation,
                                 .now_ms = ev->now_ms }; r = sup_pump_event(s, &a); }
            break;
        /* ACT_OPEN/STOP_CAPTURE/CLOSE_THROUGH/RELEASE/HOLD/REPORT_FAULT：后续 task */
        default: break;
        }
        if (r != MC100_OK) return r;
    }
    return MC100_OK;
}
```
`supervisor_test_support.h` 提供：内存 io（可复用/参考 `recovery_test_support.h`）、虚拟时钟（`now_ms` 返回可控计数）、罐装 PCM 源、`recovery_called` 计数（通过包装 io 或探针）。

- [ ] **Step 4：运行确认通过** — Expected: `supervisor_boot` PASS（恢复被调用，状态到 LISTEN）。

- [ ] **Step 5：提交**

```bash
cd "C:/Users/ljm75/Desktop/AI-HRADWARE/MC100"
git add firmware/components/mc100_supervisor firmware/tests/test_supervisor_boot.c firmware/tests/supervisor_test_support.h firmware/host/CMakeLists.txt
git commit -m "feat: add portable supervisor skeleton with boot recovery to LISTEN

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

### Task 2.4：LISTEN→RECORD 开启路径（TRIGGER→OPEN→预录落盘）

**Files:**
- Modify: `firmware/components/mc100_supervisor/supervisor.c`
- Create: `firmware/tests/test_supervisor_record_open.c`
- Modify: `firmware/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `mc100_audio_push`（trigger 来自 vad.decide）、`mc100_audio_snapshot`/`_snapshot_frame`/`_snapshot_release`、`mc100_writer_prepare`/`_begin`/`_append`、`mc100_state_step`（TRIGGER→ACT_OPEN）。参考 `evt_capture.c:176-200` 的 snapshot→begin→append preroll 序列。
- Produces: `tick` 在 LISTEN 中读帧→vad→push；观察 audio.status.triggered→喂 TRIGGER(snapshot.first_seq)→执行 ACT_OPEN（prepare+begin+copy 100 preroll frames）→喂 OPENED→RECORD。

- [ ] **Step 1：写失败测试**

```c
/* 摘要：罐装 PCM 源产生连续帧，占位 VAD 在 seq=120 触发（预留 >=100 preroll）。
 * 反复 tick 直到状态==RECORD。断言：writer 有 open segment，已写入 100 帧预录，
 * segment first_seq==snapshot.first_seq。 */
int main(void) {
    sup_fake_t fake; sup_fake_init(&fake);
    sup_fake_set_vad_trigger(&fake, 120);
    /* ... create + boot ... */
    for (int i = 0; i < 300 && mc100_supervisor_state(s) != MC100_RECORD; ++i)
        assert(mc100_supervisor_tick(s) == MC100_OK);
    assert(mc100_supervisor_state(s) == MC100_RECORD);
    mc100_writer_status_t ws; /* 经测试钩子读 */ 
    assert(sup_fake_written_frames(&fake) == MC100_PREROLL_FRAMES);
    return 0;
}
```

- [ ] **Step 2：运行确认失败** — Expected: 卡在 LISTEN，断言失败或超时。

- [ ] **Step 3：实现 LISTEN 采集 + ACT_OPEN 执行器**

在 `tick` 的 LISTEN 分支：`pcm_read`→`mc100_assembler_feed`（回调里对每帧 `vad.decide` 得 trigger，`mc100_audio_push(frame, trigger)`）；`mc100_audio_status` 若 `triggered`，取 `mc100_audio_snapshot`，喂 `MC100_EV_TRIGGER{ generation, seq=snapshot.first_seq, seq_valid=true }`。在 `sup_pump_event` 补 `MC100_ACT_OPEN`：`mc100_writer_prepare`→`mc100_writer_begin(gen, snapshot.first_seq)`→循环 `mc100_audio_snapshot_frame`+`mc100_writer_append` 落 100 预录→`mc100_audio_snapshot_release`→喂 `MC100_EV_OPENED`。（严格按 state.h：snapshot 低于 ARM 最小值是 INTERNAL_PROTOCOL，不 clamp。）

- [ ] **Step 4：运行确认通过** — Expected: `supervisor_record_open` PASS。

- [ ] **Step 5：提交**（`feat: supervisor opens recording on trigger with preroll`）

### Task 2.5：RECORD 活跃（pop/append/checkpoint + 轮换）

**Files:**
- Modify: `firmware/components/mc100_supervisor/supervisor.c`
- Create: `firmware/tests/test_supervisor_record_active.c`

**Interfaces:**
- Consumes: `mc100_audio_pop`、`mc100_writer_append`、`mc100_writer_checkpoint(now_ms)`、轮换在 15000 帧边界（`MC100_SEGMENT_FRAMES`）经 close_through 当前 + begin 下一 segment（segment_index+1，同 generation）→喂 `MC100_EV_ROTATED`。参考 `evt_capture.c:201-217`。
- Produces: RECORD 活跃态每 tick：pop 实时帧→append→按 1000ms 周期 checkpoint。

- [ ] **Step 1：写失败测试**（罐装源产生 200 实时帧，断言全部 append，checkpoint 至少触发一次；单独一个测试推到 15001 帧断言 segment_index 从 0→1）
- [ ] **Step 2：确认失败**
- [ ] **Step 3：实现 pop/append/checkpoint + 轮换**（checkpoint 用 `now_ms - record_start`；轮换在精确采样边界切下一预分配槽，下一文件第一帧接前一文件最后一帧，不重附预录）
- [ ] **Step 4：确认通过**
- [ ] **Step 5：提交**（`feat: supervisor streams live frames with checkpoint and rotation`）

### Task 2.6：正常关闭（silence→STOP→CLOSE→RELEASE→回 LISTEN）

**Files:**
- Modify: `firmware/components/mc100_supervisor/supervisor.c`
- Create: `firmware/tests/test_supervisor_close.c`

**Interfaces:**
- Consumes: `mc100_audio_stop`/`_status`（cutoff_valid/last_accepted_seq）、`mc100_writer_close_through(gen, last_seq, reason=0)`、`mc100_audio_release`、`upload.notify_closed`。状态事件链：audio 在 750 静音帧后（ARM.detail 预授权）自停→supervisor 观察 status.stopped→喂 `MC100_EV_SILENCE_END`/`CAPTURE_STOPPED{seq=cutoff}`→ACT_CLOSE_THROUGH→喂 CLOSED→ACT_RELEASE→回 LISTEN 并重新 ARM 下一 generation。
- Produces: 完整关闭序列 + 关闭后 `upload.notify_closed(final_name)` 恰一次。

- [ ] **Step 1：写失败测试**（罐装源：120 触发后给 750 静音帧；断言：segment 关成 `.wav`，spy upload 收到该文件名一次，状态回 LISTEN，且已为下一次重新 ARM——`mc100_supervisor_state`==LISTEN 且内部有有效 armed generation）
- [ ] **Step 2：确认失败**
- [ ] **Step 3：实现 STOP/CLOSE_THROUGH/RELEASE 执行器 + 关闭后通知 upload + 回 LISTEN 重新 ARM**（按 state.h：CAPTURE_STOPPED.seq_valid=false 表无接纳数据；CLOSE_THROUGH 保留该 flag；直到 CLOSED 才回收会话）
- [ ] **Step 4：确认通过**
- [ ] **Step 5：提交**（`feat: supervisor closes on silence, notifies upload, returns to LISTEN`）

### Task 2.7：故障收尾 + 掉电恢复往返

**Files:**
- Modify: `firmware/components/mc100_supervisor/supervisor.c`
- Create: `firmware/tests/test_supervisor_fault.c`、`test_supervisor_recover_roundtrip.c`

**Interfaces:**
- Consumes: `mc100_writer_close_through(..., reason!=0)`（→`.partial.wav`+INCIDENT）、`mc100_writer_abandon`、`MC100_ACT_REPORT_FAULT`、`mc100_recover`。
- Produces: 注入写故障→锁存 FAULT→在存储健康时收尾已接纳前缀为 `.partial.wav`；掉电往返：录到一半用假 io 模拟断电（不 close）→新 supervisor boot→恢复产出 `.recovered.wav`、原件保留。

- [ ] **Step 1：写失败测试**（两个：(a) 注入 `write_at` 在第 N 帧返回 MC100_IO→断言 partial + INCIDENT + 状态 FAULT；(b) 写 50 帧后丢弃 writer 不 close，新 supervisor boot→断言恢复报告 recovered>=1 且原 part 文件仍在）
- [ ] **Step 2：确认失败**
- [ ] **Step 3：实现故障路由（按 decisions.md：MIC/队列溢出/健康写入者的 STORAGE_FULL 可收尾为 INCIDENT；STORAGE_IO/超时禁止新写入）+ REPORT_FAULT**
- [ ] **Step 4：确认通过**
- [ ] **Step 5：提交**（`feat: supervisor fault finalize and power-loss recovery roundtrip`）

### Task 2.8：ESP-IDF 适配层（target-only）

**⚠ 非 TDD：** FreeRTOS/驱动无法 host 测；以目标构建 + 实板 smoke 验证。

**Files:**
- Create: `firmware/components/mc100_platform_espidf/product_runtime.c`
- Modify: `firmware/components/mc100_platform_espidf/CMakeLists.txt`

**Interfaces:**
- Consumes: `mc100_supervisor_*`、`mc100_platform_*`（audio_start/read/stop、storage_mount、adc_mv、card_present、now_ms、led）。
- Produces: `void mc100_product_run(void)` — 装配 deps（`pcm_read`=`mc100_platform_audio_read`，`now_ms`=`mc100_platform_now_ms`，io=`mc100_platform_storage_io`，vad=`mc100_vad_fixed` 占位，upload=`mc100_upload_noop`）→ create → boot → 循环 tick。监控（ADC/卡检测）另起低优先级任务喂 LOW/CRITICAL/TICK 事件。

- [ ] **Step 1：实现 product_runtime.c**（参考 `evt_capture.c:307-324` 的 board_init/mount/boot_id 装配 + `app_main.c` 的任务创建；音频任务所有权、存储任务所有权按现有台架的分工，SD 写不持音频锁）
- [ ] **Step 2：目标干净构建**

先激活 IDF v6.1 profile（EIM 默认 v5.5.5，必须确认 v6.1，见 [[mc100-build-environments]] 与 [[machine-esp-idf-is-eim-managed]]），从 `cmd.exe` 不从 Git Bash：
```
pwsh -File firmware/tools/build.ps1 -Profile evt -Clean
```
Expected: 干净构建通过，分区/链接检查通过。

- [ ] **Step 3：实板 smoke（COM7）**：上电自主进 LISTEN；占位 VAD 在预设 seq 触发→录一段→静音关闭→回 LISTEN；掉电重启→恢复运行、原件保留。记录 heap/栈高水位。
- [ ] **Step 4：提交**（`feat: add ESP-IDF product runtime wiring supervisor to drivers`）

### Task 2.9：app_main 构建开关（产品 / 台架）

**Files:**
- Modify: `firmware/main/app_main.c`
- Modify: `firmware/main/CMakeLists.txt`（加 Kconfig 或编译宏 `MC100_APP_MODE`）

- [ ] **Step 1：加构建开关**

```c
/* app_main.c */
#include "sdkconfig.h"
#if defined(CONFIG_MC100_APP_PRODUCT)
extern void mc100_product_run(void);   /* product_runtime.c */
#else
#include "evt_capture.h"               /* 台架保留 */
#endif
void app_main(void) {
#if defined(CONFIG_MC100_APP_PRODUCT)
    /* 产品自主录音循环 */
    xTaskCreatePinnedToCore(/* wrap */ mc100_product_task, "mc100_product", 16384, NULL, 8, NULL, 0);
#else
    xTaskCreatePinnedToCore(evt_task, "evt_storage", 16384, NULL, 8, NULL, 0);
#endif
}
```
加 `firmware/main/Kconfig.projbuild` 定义 `MC100_APP_PRODUCT`（默认 y=产品，台架经 menuconfig/`sdkconfig.ci` 显式选）。

- [ ] **Step 2：两种模式各干净构建通过**（产品默认 + 台架 profile）
- [ ] **Step 3：提交**（`feat: select product loop or EVT bench via build config`）

**Phase 2 退出门：** host 测试全绿（2.1–2.7）；目标产品构建通过；实板 smoke 走通 boot→listen→trigger→record→silence→close→listen + 掉电恢复；台架经开关仍可用；uploader no-op 接缝就位；VAD 为占位（真 VAD 待 Phase 3）。

---

## Self-Review（对照 spec）

- **Spec §1（两发现）**：Task 0.1 合并恢复；Task 2.3–2.9 建产品循环。✓
- **Spec §2（离线优先）**：SD 主存储、恢复优先、no-op upload 接缝（2.1/2.6）。✓
- **Spec §3（稳定性优先/trade-off）**：Global Constraints 固定 1s sync/96 队列/2s 预录；不为省电削弱。✓
- **Spec §4（40MHz 命题）**：Phase 1 spike 全覆盖。✓
- **Spec §5（无线预留）**：2.1 upload 接缝 no-op；Phase 1 复用门评估 esp-sr/libfvad。✓
- **Spec §6（删/留/整合）**：Phase 0 全覆盖（合并/删 cleanup/降级/隔离台架/重写 STATUS）。✓
- **Spec §6.3（复用优先）**：每 Phase 复用门；Phase 1 VAD 择优；Phase 2 复用驱动。✓
- **Spec §7（路线）**：Phase 0/1/2 逐任务展开；Phase 3+ 暂不展开（按用户要求）。✓
- **占位符扫描**：无 TBD/TODO；git 与 target 任务给了真实命令；host 任务给了真实测试码。Phase 2.4–2.7 的测试用"摘要"形式给出断言与 API 序列，实现时按 state.h 契约与 evt_capture.c 参考填全——这是有意的右尺寸（完整 700 行 supervisor 不宜逐行入计划），非占位。
- **类型一致性**：`mc100_upload_sink_t.notify_closed`、`mc100_vad_t.decide`、`mc100_supervisor_deps_t` 字段、`mc100_recover` 签名跨任务一致。✓
