# MC100 V1 Software Development Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans or superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. 当前执行模式允许有界子任务及独立审查；共享接口变更须协调，避免并发修改同一文件。

**Goal:** 在板子到货前交付可复现构建、同源电脑测试覆盖的离线自动录音固件候选，板子到货后完成有证据的 EVT 验证。

**Architecture:** C11 可移植核心实现状态、预录、VAD 适配、文件事务和恢复；ESP-IDF 适配层负责真实外设与任务。State、Audio、Storage 分别独占设备状态、采集和文件系统，电脑仿真连接同一核心，不重写业务逻辑。

**Tech Stack:** ESP32-S3-MINI-1-N8、项目选定 ESP-IDF v6.1、C11、FreeRTOS、I2S0 PDM RX、SDMMC 1-bit/FatFs、libfvad 候选、CMake/CTest、Linux ASan/UBSan、Windows 本地开发。

**Spec:** [MC100 软件架构设计](../specs/2026-09-18-mc100-software-architecture-design.md)。执行时同时阅读[硬件依据与验收矩阵](../../MC100-VALIDATION.md)，不能只读单个任务。

**当前执行范围（用户更新）：** 暂无 MC100 板子，先执行 T01–T12 的完整无板软件开发；T13–T14 保留为到板后的验证计划，不作为本轮软件目标的完成条件。进度和实际证据见[开发进度](../../MC100-DEVELOPMENT-STATUS.md)。软件阶段仍须满足全部软件测试、目标构建与打包条件，不能仅完成可编译骨架即放行。

## Global Constraints

- MCU=ESP32-S3-MINI-1-N8，8 MB Quad Flash，无 PSRAM；目标 `esp32s3`，固定项目 ESP-IDF v6.1 修订，不自动升级 SDK。
- PCM=16,000 Hz、signed 16-bit little-endian、mono；20 ms=320 samples=640 byte；两秒预录=100 帧。
- 连续 750 个非语音帧结束录音；单文件 15,000 帧/300 秒/9,600,000 byte PCM，包含首段预录；轮换不重复预录。
- 双 100 帧预录银行，96 帧实时队列；PCM 容量 61,440 byte；正常支持卡零丢帧，异常不能静默成功。
- PDM GPIO1/2，SD DAT0/CLK/CMD GPIO6/7/8，CD GPIO5，ADC GPIO4，LED GPIO21，USB GPIO19/20。
- SDMMC 1-bit 初始 20 MHz；不使用 SDSPI/80 MHz；卡检测有效电平必须 HIL 确认。
- SW1 无关机预告/软件断电；SD 不能单独断电；CHG#/PGOOD# 未连接；不得恢复旧 GEK/按键/充电输入逻辑。
- State 单写者；Audio 独占 I2S/VAD；Storage 独占 SD/FAT；所有跨任务通道有界，ACK 带 generation，优雅停止必须排空到截止帧。
- 标准 WAV 与 CRC 索引分离；预分配不是已录长度；失败保留原件；不自动格式化、删旧录音、烧 eFuse 或推送远程。
- 正常 LISTEN/RECORD LED off、无线 off；LISTEN 持续采样，不进入停止音频的 Deep-sleep。
- 常规启动先用 80 MHz；40 MHz/35 mA/24 h 是需验证目标，不绕过驱动 PM 锁。
- 低电模拟值仅用于测试，产品阈值必须 H04 确认；无板通过只标 `SW_READY_FOR_EVT`，不得标整机通过。
- 本文全部实现项初始未完成；本文中的代码是接口/测试契约，不代表这些文件已存在。

---

## 1. 范围、里程碑和工作节奏

本计划是一套录音系统的整体计划。为避免一次性写出不可验证的大块固件，按可独立验收的状态协议、音频、存储/恢复、VAD、驱动和集成拆成 T01–T12；HIL 为 T13–T14。每项内先测试、再实现、再集成，不以“大致写完”通过关卡。

| 里程碑 | 任务 | 可见产物/门槛 | 是否需要板子 |
| --- | --- | --- | --- |
| M0 可复现基础 | T01 | 正确目标/引脚/分区、空核心与宿主测试、干净构建 | 否 |
| M1 确定性录音链 | T02–T06 | 帧边界、预录、文件轮换、CRC 与恢复，失败路径可重放 | 否 |
| M2 真实语音仿真闭环 | T07–T08 | 人声输入→WAV、标注评估、48 h 虚拟时间与故障矩阵 | 否 |
| M3 目标候选 | T09–T12 | 全驱动/任务可编译、CI 和候选包，`SW_READY_FOR_EVT` | 否；运行指标仍 NOT_RUN |
| M4 首板功能 | T13 | H01–H04，下载、采集、卡、ADC 和联合录音 | 是 |
| M5 EVT 放行 | T14 | H05–H08，真实功耗、掉电、长测、结构整机 | 是 |

依赖：T01→T02/T04，T01+T02→T03；T03+T04→T05→T06；T03→T07；T02–T07→T08；T01+T03+T05→T09，T02+T09→T10；T08+T09+T10→T11→T12→T13→T14。可独立任务可分支开发，但默认在当前任务内顺序执行，不引入未经请求的多代理或远程变更。

预计工程量（安排资源的初估，不是交付承诺）：M0 1–2 工程日；M1 6–9；M2 3–5；M3 4–6；M4 2–4；M5 至少包含连续 48 h 实测和多卡掉电循环，通常另需 5–10 工程日及故障回归。无板阶段约 14–22 工程日；语料质量、FAT 故障和驱动适配可能增加时间。板子早到可提前 H01 外设检查，但不能跳过软件完整性测试。

每个工作单元控制在一个可审查功能内；步骤中列出多个测试时逐个执行，保持红→绿证据。先记录本地修改，避免覆盖他人文件；每项通过后做范围明确的本地提交，推送/PR 按用户指示单独处理。失效修复必须新增回归测试并重跑受影响矩阵，不同时顺手升级 SDK/库或更改硬件。

## 2. 目录与产物分工

以下路径相对仓库根。`firmware/components/*/CMakeLists.txt` 随各组件同步建立；每个 public header 放在该组件 `include/` 下。现有 `hello_world_main.c` 在 T11 完整启动入口替代时移除，T01 先移除倒计时重启而保留明确的“尚未录音”诊断。

```text
firmware/
  CMakeLists.txt, sdkconfig.defaults, partitions.csv
  main/app_main.c, main/CMakeLists.txt
  components/
    mc100_core/       types, state, battery_policy, config
    mc100_board/      board_mc100_v1.h
    mc100_audio/      frame_assembler, audio, stream, vad
    mc100_storage/    io, crc32, wav, journal, writer, recovery
    mc100_platform_espidf/
                     audio_i2s, sd_fat, board_io, monitor,
                     config_nvs, diagnostics, runtime, power
  third_party/libfvad/
  host/              CMakeLists.txt, sim_main.c, file_io.c,
                     fake_io.c, fat_diskio.c, runner.c
  tests/             test_*.c, fixtures/, scenarios/, corpus_manifest.csv
  tools/             build.ps1, test-host.ps1, validate_artifacts.py,
                     evaluate_vad.py, hil_runner.py
  README.md, dependencies.lock.json
.github/workflows/firmware.yml
docs/reports/         小型测试报告与清单；大日志/音频/镜像不入 Git
```

宿主 CMake 直接编译 `components/mc100_core/*.c`、`mc100_audio/*.c`、`mc100_storage/*.c` 的可移植源，明确列源文件，不复制。第三方改动集中在适配层；确需补丁要保留 patch、原因和许可证。

## 3. 接口与测试约定

T01 建立 `mc100_types.h`，其他任务使用同一类型：

```c
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum { MC100_SAMPLE_RATE = 16000, MC100_FRAME_SAMPLES = 320,
       MC100_PREROLL_FRAMES = 100, MC100_STREAM_FRAMES = 96,
       MC100_SEGMENT_FRAMES = 15000 };
typedef uint64_t mc100_generation_t;
typedef enum { MC100_OK, MC100_INVALID, MC100_NOT_READY, MC100_FULL,
               MC100_IO, MC100_CORRUPT, MC100_TIMEOUT } mc100_result_t;
typedef struct { uint64_t seq; int16_t pcm[320]; } mc100_frame_t;
typedef struct {
    mc100_generation_t generation;
    mc100_frame_t frame;
} mc100_packet_t;
typedef struct {
    mc100_generation_t generation;
    uint64_t first_seq;
    uint16_t count;
    uint8_t bank_id;
} mc100_snapshot_t;
_Static_assert(sizeof(mc100_frame_t) == 648, "frame RAM budget");
_Static_assert(sizeof(mc100_packet_t) == 656, "stream RAM budget");
```

大缓冲只在初始化分配一次，构造失败显式返回空指针，不半启动。各组件 opaque context 的完整定义由所属任务放在私有头文件，公共 API 只传指针；下面的 create/destroy 只用于启动/完全停止，不在录音热路径调用。Host 可反复创建测试实例，固件不能在有在途消息时销毁实例。

CTest 测试用标准 C `assert`（确保测试 target 未定义 NDEBUG），每个 `test_*.c` 是一个测试 executable；fixture 从固定 seed 生成，不依赖墙上时间。文中 Run 从仓库根执行：

```powershell
cmake -S firmware/host -B firmware/out/host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build firmware/out/host
ctest --test-dir firmware/out/host --output-on-failure
```

T01 完成前这组命令预期失败，因为宿主工程尚不存在。所有命令都检查退出码；Linux 用相同命令，额外配置 sanitizer 选项。新的输出目录加入精确忽略规则，不提交本机绝对路径、私人音频或构建缓存。

## 4. 板子到货前的开发任务

### T01：工程基线、板级能力和同源测试入口

依赖：无。产出：M0；需求 R01/R20/R22。

**Files:** 修改 `firmware/CMakeLists.txt`、`firmware/main/CMakeLists.txt`、`firmware/main/hello_world_main.c`、`firmware/README.md`、根 `.gitignore`；创建 `firmware/sdkconfig.defaults`、`firmware/partitions.csv`、`firmware/dependencies.lock.json`、`firmware/components/mc100_core/include/mc100_types.h`、`firmware/components/mc100_board/include/board_mc100_v1.h`、`firmware/host/CMakeLists.txt`、`firmware/tests/test_board_contract.c`、`firmware/tools/build.ps1`、`firmware/tools/test-host.ps1`。

**Interfaces:** 输出 §3 公共类型；BSP 导出以下常量，后续组件不得再次硬编码引脚：

```c
enum { MC100_GPIO_PDM_DATA = 1, MC100_GPIO_PDM_CLK = 2,
       MC100_GPIO_BAT_ADC = 4, MC100_GPIO_SD_CD = 5,
       MC100_GPIO_SD_D0 = 6, MC100_GPIO_SD_CLK = 7,
       MC100_GPIO_SD_CMD = 8, MC100_GPIO_LED = 21,
       MC100_GPIO_USB_DM = 19, MC100_GPIO_USB_DP = 20 };
enum { MC100_HAS_PSRAM = 0, MC100_HAS_SOFT_POWER_OFF = 0,
       MC100_HAS_SD_POWER_SWITCH = 0, MC100_HAS_CHARGER_STATUS = 0 };
```

- [ ] 先创建 board_contract 测试，断言 LED=21 且不等于 SD_CLK、PDM/SD/USB/ADC 引脚无误、能力开关全为 0、帧和队列结构大小准确；运行单测看到缺失头文件/符号失败，不把环境缺编译器当红灯成功。

```c
assert(MC100_GPIO_LED == 21);
assert(MC100_GPIO_LED != MC100_GPIO_SD_CLK);
assert(MC100_HAS_PSRAM == 0 && MC100_HAS_SOFT_POWER_OFF == 0);
assert(MC100_FRAME_SAMPLES * sizeof(int16_t) == 640);
```

- [ ] 实现上面两个头文件和最小 CMake CTest 入口，构建并运行 `ctest --test-dir firmware/out/host -R board_contract --output-on-failure`，预期通过。
- [ ] 固定 target/SDK/依赖锁；保存默认配置，8 MB Flash、PSRAM off、80 MHz 启动、USB Serial/JTAG 控制台、无线 off、brownout/watchdog 保留。按项目实际 v6.1 Kconfig 校验选项，不抄其他 SDK 的未知 symbol。
- [ ] 创建并检验下面分区；停止 hello_world 倒计时重启，改为一次性打印“MC100 infrastructure only / recording not implemented”后阻塞，不能伪装成 LISTEN。

```csv
# Name, Type, SubType, Offset, Size, Flags
nvs,data,nvs,0x9000,0x6000,
phy_init,data,phy,0xf000,0x1000,
factory,app,factory,0x10000,0x300000,
```

- [ ] 在已激活的项目 SDK 终端中，构建脚本用独立输出目录/生成 sdkconfig，不覆盖用户本地配置；执行 `idf.py -C firmware -B out/target -D SDKCONFIG=out/target/sdkconfig -D SDKCONFIG_DEFAULTS=sdkconfig.defaults -D IDF_TARGET=esp32s3 build`，同时核对 build metadata 的 SDK 修订、Flash/分区和控制台。此命令必须由脚本转换为工程内绝对参数并检查路径，避免相对路径被错误解释。
- [ ] 更新 README 的环境检查、host/target 构建入口和限制；只忽略生成配置/输出，不忽略 defaults、分区和锁文件。记录 host 与 target 新鲜构建结果后本地提交 `build: establish MC100 target and host baseline`。

### T02：设备状态机、低电策略与带代次的关闭协议

依赖：T01。产出：纯逻辑状态机；需求 R05/R15/R16/R17。

**Files:** 创建 `mc100_core/include/mc100_state.h`、`mc100_core/state.c`、`mc100_core/include/mc100_battery_policy.h`、`mc100_core/battery_policy.c`（组件路径前缀均为 `firmware/components/`）；创建 `firmware/tests/test_state.c`、`test_battery_policy.c`。

**Interfaces:** `mc100_state_t` opaque；`mc100_state_create(void)` / `mc100_state_destroy(mc100_state_t *)`；`mc100_state_step(mc100_state_t *, const mc100_event_t *, mc100_action_t out[8], size_t *count)` 返回 `mc100_result_t`；`mc100_state_get(const mc100_state_t *)` 返回 `mc100_state_id_t`。

```c
typedef enum { MC100_BOOT, MC100_LISTEN, MC100_RECORD, MC100_LOW_BAT,
               MC100_LOW_BAT_HOLD, MC100_FAULT } mc100_state_id_t;
typedef enum { MC100_EV_READY, MC100_EV_TRIGGER, MC100_EV_OPENED,
    MC100_EV_SILENCE_END, MC100_EV_CAPTURE_STOPPED, MC100_EV_CLOSED,
    MC100_EV_LOW, MC100_EV_CRITICAL, MC100_EV_RECOVERED_POWER,
    MC100_EV_FAULT, MC100_EV_TICK, MC100_EV_ROTATED } mc100_event_id_t;
typedef struct {
    mc100_event_id_t id; mc100_generation_t generation;
    uint64_t seq, now_ms; uint32_t detail;
} mc100_event_t;
typedef enum { MC100_ACT_ARM, MC100_ACT_OPEN, MC100_ACT_STOP_CAPTURE,
    MC100_ACT_CLOSE_THROUGH, MC100_ACT_RELEASE, MC100_ACT_HOLD,
    MC100_ACT_BOOT, MC100_ACT_REPORT_FAULT } mc100_action_id_t;
typedef struct {
    mc100_action_id_t id; mc100_generation_t generation;
    uint64_t seq; uint32_t detail;
} mc100_action_t;
```

- [ ] 写失败测试：BOOT→READY→LISTEN，旧 generation TRIGGER/CLOSED 不改变状态，STOP 后旧有效帧仍可排空，只有 CLOSED 才宣告结束。`EV_READY.detail` 携带已准备槽数量，READY 只有在驱动/电池/恢复全通过后由运行层发出。

```c
mc100_state_t *s = mc100_state_create();
mc100_action_t actions[8]; size_t count = 0;
mc100_event_t ready = {.id = MC100_EV_READY, .detail = 2};
assert(s && mc100_state_get(s) == MC100_BOOT);
assert(mc100_state_step(s, &ready, actions, &count) == MC100_OK);
assert(mc100_state_get(s) == MC100_LISTEN);
assert(count == 1 && actions[0].id == MC100_ACT_ARM);
mc100_state_destroy(s);
```

- [ ] 为 `close_drain`、`stale_event`、`retrigger_closing`、双重 ACK、200 ms 控制超时、1,500 ms 存储超时建立确定性事件序列；先运行 `ctest --test-dir firmware/out/host -R 'state|battery_policy' --output-on-failure`，确认行为失败。
- [ ] 实现架构 §5 状态表、最多两会话上下文和最多 8 个动作输出；动作溢出返回 INTERNAL_PROTOCOL 类故障，不越界；停止/关闭截止不通过修改 generation 提前清空队列。静音策略由 ARM 预授权，Audio 自停后 State 仍发幂等 STOP 获取截止；低电超时而驱动未静止时不能错误进入 HOLD。枚举内部错误原因，并在诊断映射中保持稳定数值。
- [ ] 电池策略输入 `mc100_battery_step(policy, mv, valid, now_ms)` 返回 NORMAL/LOW/CRITICAL/RECOVERED/INVALID 枚举；公开 `mc100_battery_config_t` 含 low_mv/critical_mv/resume_mv 与持续毫秒数，`mc100_battery_create(config)`/`destroy` 分配一次。测试 3,600 mV 的 2,999/3,000 ms、单点毛刺、无效 ADC、HOLD 30 s 恢复和临界电压，不将模拟值写成量产确认值。
- [ ] 全部事件输出与状态可用固定 seed 重放，验证时钟倒退输入被拒绝、整数边界不回绕；通过后本地提交 `feat: define recorder lifecycle and battery policy`。

### T03：帧组装、双预录银行与有界音频流

依赖：T01/T02 的协议契约。产出：可逐样本验证的音频内存路径；需求 R02/R03/R07/R08。

**Files:** 创建 `mc100_audio/include/mc100_audio.h`、`frame_assembler.c`、`audio.c`、`stream.c`；测试 `firmware/tests/test_frame_assembler.c`、`test_preroll_handoff.c`、`test_queue_full.c`、`test_capture_gap.c`。

**Interfaces:** opaque `mc100_audio_t`；`create(void)`/`destroy`；所有下列名称带 `mc100_audio_` 前缀：`arm(a, generation, min_seq)`、`push(a, const mc100_frame_t *, bool trigger)`、`snapshot(a, generation, mc100_snapshot_t *)`、`snapshot_frame(a, snapshot, uint16_t index, mc100_frame_t *)`、`pop(a, mc100_packet_t *)`、`stop(a, generation, uint64_t *last_accepted_seq)`、`release(a, generation)`，除 create/destroy 外均返回 `mc100_result_t`。

组帧器 `mc100_assembler_t` 与 `mc100_assembler_init`/`mc100_assembler_feed` 在该头定义；feed 接收 PCM 字节数组、长度和帧回调 `mc100_result_t (*)(void *, const mc100_frame_t *)`，奇数字节先保留至下次输入，不丢弃半个采样；驱动确认发生真实采集缺口时另发明确错误而非补零冒充真实音频。

- [ ] 测试任意分片得到同一 PCM、触发帧 100 前快照 0–99、实时首帧 100、启动只有 17 帧时不补造 83 帧；先运行 `ctest --test-dir firmware/out/host -R 'frame_assembler|preroll_handoff|queue_full|capture_gap' --output-on-failure`，确认缺实现失败。

```c
mc100_audio_t *a = mc100_audio_create();
assert(a && mc100_audio_arm(a, 1, 0) == MC100_OK);
for (uint64_t i = 0; i <= 100; ++i) {
    mc100_frame_t f = {.seq = i};
    for (size_t k = 0; k < 320; ++k) f.pcm[k] = (int16_t)i;
    assert(mc100_audio_push(a, &f, i == 100) == MC100_OK);
}
mc100_snapshot_t snap;
mc100_packet_t live;
assert(mc100_audio_snapshot(a, 1, &snap) == MC100_OK);
assert(snap.first_seq == 0 && snap.count == 100);
assert(mc100_audio_pop(a, &live) == MC100_OK && live.frame.seq == 100);
mc100_audio_destroy(a);
```

- [ ] 实现先冻结历史、再入触发帧的顺序；银行按 FREE/ROLLING/FROZEN 明确所有权。补充 `snapshot_frame` 内容逐字节测试、重复 release 拒绝、stop 后仍滚动历史且不进旧会话。
- [ ] 实现 96 帧队列及 current/peak/drops/first_gap_seq 只读统计；第 97 帧返回 MC100_FULL、锁存错误并停止该会话继续入流。正常消费速度下测试 100 万帧零 drop；无法启动/无银行时返回 NOT_READY，不覆盖仍被 Storage 使用的银行。
- [ ] 对 closing 重触发使用 `min_seq=previous_close_seq+1`，验证两个 generation 在 FIFO 中先后连续，旧会话 CLOSED 不误释放新银行；`trigger_seq-100` 用饱和减法。ARM 往返间 VAD 保留一个待触发标记，新 ARM 时重评持续人声，测试不能漏掉短暂讲话。SPSC 的同步原语由平台适配实现，host 测内容/顺序，T11 测并发发布。
- [ ] 增加初始化后分配计数断言、缓冲尺寸静态断言；通过后本地提交 `feat: add deterministic preroll and bounded audio stream`。

### T04：标准 WAV、CRC 和索引编解码

依赖：T01。产出：版本化磁盘格式；需求 R06/R11/R13。

**Files:** 创建 `mc100_storage/include/mc100_format.h`、`crc32.c`、`wav.c`、`journal.c`；测试 `firmware/tests/test_wav.c`、`test_journal_codec.c`；创建小型黄金字节夹具 `firmware/tests/fixtures/format_v1.hex`。

**Interfaces:**

```c
uint32_t mc100_crc32(const void *data, size_t size);
mc100_result_t mc100_wav_header(uint8_t out[512], uint32_t pcm_bytes);
typedef struct {
    uint16_t type; uint64_t journal_seq, pcm_offset, first_source_sample;
    uint32_t valid_bytes, payload_crc32; mc100_generation_t generation;
    uint32_t flags, detail;
} mc100_index_record_t;
mc100_result_t mc100_index_encode(uint8_t out[64], const mc100_index_record_t *r);
mc100_result_t mc100_index_decode(const uint8_t in[64], mc100_index_record_t *r);
```

- [ ] 写失败测试：CRC 已知向量、0 byte/640 byte/9,600,000 byte WAV 头、错误奇数长度、超过段上限、索引位翻转/版本/保留字段错误；运行 `ctest --test-dir firmware/out/host -R 'wav|journal_codec' --output-on-failure`。

```c
assert(mc100_crc32("123456789", 9) == UINT32_C(0xCBF43926));
uint8_t header[512];
assert(mc100_wav_header(header, 640) == MC100_OK);
assert(memcmp(header, "RIFF", 4) == 0);
assert(memcmp(header + 504, "data", 4) == 0);
assert(mc100_wav_header(header, 641) == MC100_INVALID);
```

- [ ] 按架构 §6.1 实现显式 little-endian 序列化；RIFF size=504+pcm_bytes，data size=pcm_bytes，JUNK payload=460。禁止 `fwrite(&struct)`；魔数、版本、每个字段偏移成为黄金夹具断言。
- [ ] 定义索引 512 byte 头编解码 `mc100_index_header_encode/decode`，类型 `mc100_index_header_t` 包含 `boot_id[16]`、generation、segment_index、first_source_sample、flags（RESERVED=1/CLAIMED=2），固定 PCM 格式；精确偏移及输出头 CRC 按架构 §6.1。BLOCK/CHECKPOINT/FINAL/INCIDENT 的范围和连续性在解码及 writer/recovery 两层检查。
- [ ] 创建独立 Python 标准库 `wave` 验证（不是重写 writer）：能解析 JUNK 前缀、返回 16 kHz/16-bit/mono 和精确样本数；逐字节比对小型 PCM fixture。通过后本地提交 `feat: define WAV and CRC journal format`。

### T05：存储事务、预分配、轮换与剩余空间

依赖：T03/T04。产出：通过接口的真实 writer；需求 R06–R08/R11/R14。

**Files:** 创建 `mc100_storage/include/mc100_io.h`、`include/mc100_writer.h`、`writer.c`；`firmware/host/fake_io.c`、`file_io.c`；测试 `test_storage_writer.c`、`test_storage_short_write.c`、`test_rotate_15000.c`、`test_space_admission.c`。

**Interfaces:** I/O 以上下文与显式句柄注入，`mc100_file_t` 为 opaque handle。函数表 `mc100_io_t` 提供 `open_exclusive(path, &file)`、`open_read(path, &file)`、`read_at(file, offset, buf, len, &actual)`、`write_at(...)`、`allocate(file, bytes, &actual_size)`、`sync(file)`、`truncate(file, bytes)`、`close(file)`、`rename_no_replace(old,new)`、`stat(path,&size)`、`space(&total,&free)`、`list(visitor,ctx)`；全部带 `void *ctx` 首参数并返回 `mc100_result_t`。只有 enumerate 回调处理经验证的路径，不提供任意删除 API。

opaque `mc100_writer_t` 的公开接口：`mc100_writer_create(const mc100_io_t *, void *io_ctx, const uint8_t boot_id[16])` / `destroy`，以及均带 `mc100_writer_` 前缀、返回 `mc100_result_t` 的 `prepare(writer)`、`begin(writer, generation, first_seq)`、`append(writer, const mc100_packet_t *)`、`checkpoint(writer, now_ms)`、`close_through(writer, generation, last_seq, uint32_t reason)`。append 从调用者复制数据，不保存栈指针；仅 Storage 调用，不允许 Audio 直接调用。boot_id 由运行层注入，文件独占创建仍须处理碰撞，不能依赖“随机数不会重复”。

- [ ] fake_io 先记录每次 I/O 的次序/长度并可在第 N 次操作短写或失败；写测试断言 data sync 必须先于对应 BLOCK/CHECKPOINT 的 index sync，写短不能增长 committed_length，剩余空间门槛精确，先运行 `ctest --test-dir firmware/out/host -R 'storage_|rotate_15000|space_admission' --output-on-failure`。

```c
/* 纯容量规则也必须单独测试；函数在 mc100_writer.h 中声明。 */
bool mc100_space_can_prepare(uint64_t total, uint64_t free_bytes,
                            uint64_t reserve_bytes);
const uint64_t total = UINT64_C(1000000000);
const uint64_t slot = UINT64_C(9863168);
assert(!mc100_space_can_prepare(total, total / 10 + slot, slot));
assert(mc100_space_can_prepare(total, total / 10 + slot + 1048576, slot));
```

- [ ] 实现两槽准备、32 KiB staging、4 KiB CRC 数据块、1 s 检查点和精确实际写入长度检查。`allocate` 可以返回不足请求的长度；必须视为失败，不把 `f_lseek` 成功等同于申请满足。每槽索引记录区先全部清零并同步，再写有效 RESERVED 头并同步，防止分配簇的旧数据成为有效记录；不为此清零整份 PCM。
- [ ] 存储槽先用 `<boot_id>_reserve_<counter>.wav.part/.idx.part` 独占创建，索引头标 RESERVED、generation=0；首次认领时写入身份/CLAIMED、同步并改为正常会话名字后才写 PCM/返回 OPENED。重启优先复用经过校验的未认领空槽，测试连续 1,000 次无录音启动不会每次遗留两个预分配文件；不确定槽保持原件。跨任意操作断电可以识别未使用预留槽或已认领录音；具体恢复规则与架构一致。
- [ ] 按 15,000 帧切段，包括首次快照帧；新段不重新添加预录。文件名使用 boot_id+generation+segment_index；保留一个准备槽供后继，在 RECORD 中提前补齐，LISTEN 不后台持续预分配。测试 15,001/30,005 帧拼接结果与源完全一致。
- [ ] 完成短写、写入失败、索引失败、卡满、重复帧、缺帧、错误 generation、关闭截止、已知溢出 `.partial.wav` 的处理；无权把不确定文件重命名为正常完成。通过后本地提交 `feat: write bounded recordings with recovery journal`。

### T06：恢复器、幂等和扇区级故障注入

依赖：T05。产出：同源启动/电脑恢复器；需求 R11/R12/R17。

**Files:** 创建 `mc100_storage/include/mc100_recovery.h`、`recovery.c`；`firmware/host/fat_diskio.c`；测试 `test_recover_prefix.c`、`test_recover_idempotent.c`、`test_fat_powercut.c`；场景 `firmware/tests/scenarios/powercut.json`。

**Interfaces:**

```c
typedef struct {
    uint64_t valid_pcm_bytes;
    uint32_t recovered, preserved, invalid, timed_out;
} mc100_recovery_report_t;
mc100_result_t mc100_recover(const mc100_io_t *io, void *ctx,
                            uint64_t deadline_ms,
                            uint64_t (*now_ms)(void *), void *clock_ctx,
                            mc100_recovery_report_t *report);
```

- [ ] 建立预分配长于实录、预分配簇含随机垃圾/旧合法记录、索引初始化中断、索引尾部撕裂、PCM CRC 坏块、缺索引、未知版本、名字冲突、恢复再次断电的测试；每个场景先断言未实现恢复会失败，运行 `ctest --test-dir firmware/out/host -R 'recover_|fat_powercut' --output-on-failure`。
- [ ] 顺序校验 512 byte 头与 64 byte 记录，遇首个不可信块停止，不跳过坏块拼接；只复制可证明连续前缀。以流式 4 KiB 缓冲生成新 WAV，保留原始 part/idx。30 s 初始预算用注入时钟测试，不 sleep 30 s。
- [ ] 明确并测试命名中间态：wav+idx.part、wav.part+idx、已存在 recovered、未使用 RESERVED 槽、认领过程中断电；依据文件头身份与 CRC，不仅看扩展名。第二次恢复不得再次复制或覆盖第一次的正确输出；空槽复用不能扩大空间占用。对完整历史文件做头/FINAL/长度快速检查，不在每次启动重复扫描全部 PCM；完整 CRC 审计作为独立工具选项。
- [ ] FAT 镜像使用项目同修订的 FatFs 源和关键配置；实现 disk_read/write/ioctl 注入层，只操作测试内存/文件镜像。对 512 byte 扇区逐故障点切断，包括丢写、扇区撕裂、SYNC 前重排、目录和 FAT 元数据更新失败。

```text
每个 failpoint：从只读种子镜像复制 → 写既有历史文件并记哈希
→ 启动一段录音 → 在指定持久化点硬停止（不执行 close）
→ 丢弃所有 RAM 状态 → 重新挂载 → 调同源 mc100_recover
→ 校验历史哈希、连续前缀、原件保留 → 再恢复一次验证幂等
```

- [ ] 长度/offset/记录数 fuzz 至少 10,000 个固定 seed 样例，ASan/UBSan 零错误；不得把故障注入测试指向真实盘符/用户 SD。通过后本地提交 `feat: recover validated audio prefixes after interrupted writes`。

### T07：接入真实 VAD 与离线语料评估

依赖：T03。产出：可替换、受测的语音检测；需求 R04/R05。

**Files:** 创建 `firmware/third_party/libfvad/`、更新 `firmware/dependencies.lock.json`；`mc100_audio/include/mc100_vad.h`、`vad.c`；测试 `test_vad.c`、`test_silence_boundary.c`、`test_vad_corpus.c`；`firmware/tests/corpus_manifest.csv`、`firmware/tools/evaluate_vad.py`。

**Interfaces:** opaque `mc100_vad_t`；`mc100_vad_create(int mode)` / `destroy`；`mc100_vad_process(vad, const mc100_frame_t *, bool *speech)` 返回 `mc100_result_t`。分段检测器 `mc100_gate_t` 保存最近 5 帧位图和 silence_frames；`mc100_gate_init` / `mc100_gate_step(gate, bool speech, bool recording)` 返回 NONE/TRIGGER/SILENCE_END 枚举。

- [ ] 先用注入 speech 序列测试 3/5 开启和 749/750 结束边界，不用实录偶然通过替代确定性测试：

```c
mc100_gate_t gate;
mc100_gate_init(&gate);
assert(mc100_gate_step(&gate, true, false) == MC100_GATE_NONE);
assert(mc100_gate_step(&gate, true, false) == MC100_GATE_NONE);
assert(mc100_gate_step(&gate, true, false) == MC100_GATE_TRIGGER);
for (int i = 0; i < 749; ++i)
    assert(mc100_gate_step(&gate, false, true) == MC100_GATE_NONE);
assert(mc100_gate_step(&gate, false, true) == MC100_GATE_SILENCE_END);
```

- [ ] 获取候选固定修订 `532ab666c20d3cfda38bca63abbb0f152706c369` 的源码，核对 API、许可证和哈希，保存 LICENSE/PATENTS/AUTHORS；不能只拉 master。配置 16 kHz、mode 2，封装返回 -1 为错误；检查构造与处理路径分配行为。
- [ ] Host 和 ESP32-S3 编译适配层；测量每帧计算开销和持久内存（目标耗时留待 H02/H05），不启用 PSRAM。运行 `ctest --test-dir firmware/out/host -R 'vad|silence_boundary' --output-on-failure`。
- [ ] 语料清单列相对路径、SHA256、授权、场景、语音起止标签及 train/test 分组；至少 200 个事件、2 h 非语音。脚本只调用编译的 VAD/仿真 executable 并评分，不实现另一个 VAD。按验收矩阵 §4.3 报告召回/误触发/延迟以及各场景结果。
- [ ] 调参只用 train 组，test 组冻结；若不达初始门槛，报告失败并在该任务修正算法/场景定义，不将失败留给所谓“后期再优化”。通过后本地提交 `feat: integrate and evaluate voice activity detection`。

### T08：电脑端到端录音与故障场景

依赖：T02–T07。产出：M2，同源软件闭环；需求 R03–R08/R12/R14/R15。

**Files:** 创建 `firmware/host/sim_main.c`、`runner.c`、`include/mc100_sim.h`；`firmware/tests/test_sim.c`、`tests/scenarios/preroll.json`、`rotation.json`、`slow_card.json`、`retrigger.json`、`low_battery.json`、`soak.json`。

**Interfaces:** CLI `mc100_sim --scenario <json> --output <task-owned-directory> --seed <u64>`；输出 `report.json`、WAV/idx、事件轨迹。`mc100_sim_run(const char *scenario, const char *out_dir, uint64_t seed)` 返回 `mc100_result_t`，main 把非 OK 映射为非零退出码。

- [ ] 先定义 report schema，包含 expected/actual 样本范围、正常/异常关闭数、drops、queue_peak、recovery、各断言 PASS/FAIL；脚本不能只看 executable 未崩溃。CTest 注册 `preroll_handoff_e2e`、`rotate_15000_e2e`、`overflow_partial`、`retrigger_closing`、`boot_low_battery`、`virtual_48h`。
- [ ] 场景使用虚拟单调时钟，每次音频 tick=20 ms；驱动事件和存储延迟交错调度，但始终调用同一个 State/Audio/Writer/Recovery。正常会话输入 100 帧历史+触发+后续语音+750 静音，逐样本比对导出 WAV。

```json
{"name":"rotation","sample_rate":16000,"frame_samples":320,
 "frames":30005,"vad":"forced_speech","trigger_frame":100,
 "storage_delay_ms":0,"expect_drops":0,"seed":100}
```

- [ ] 扫描写入/sync/预分配/关闭的 0、10、100、500、1,000、1,500、2,000、3,000 ms 延迟，分别覆盖空队列和已有积压；低于缓冲容量不能自动断言“必定安全”，必须按事件轨迹验算。溢出场景预期明确 `.partial.wav`/FAULT，正常场景预期零 drop。
- [ ] 注入旧 generation 完成、重复消息、关闭时重新讲话、低电期间触发、插拔和卡满；全生命周期循环至少 10,000 次并运行至少 48 h 虚拟时间，不生成无必要的几十 GB 音频，用流式哈希和采样窗口比对。
- [ ] 同时运行真实 VAD 语料场景；将 forced_speech 与 real_vad 报告分开。全套 `ctest --test-dir firmware/out/host --output-on-failure` 通过并审查不变量后本地提交 `test: exercise end-to-end recorder and failure scenarios`。

### T09：ESP-IDF 外设适配与可独立诊断的驱动

依赖：T01/T03/T05。产出：无需业务重写的目标驱动；需求 R01/R02/R09/R10/R18/R21。

**Files:** 创建 `mc100_platform_espidf/include/mc100_platform.h`、`audio_i2s.c`、`sd_fat.c`、`board_io.c`、组件 `CMakeLists.txt`、`Kconfig`；`firmware/tests/test_driver_contract.c`、`firmware/tools/probe_idf_storage.c`；更新 defaults。

**Interfaces:**

```c
mc100_result_t mc100_platform_audio_start(void);
mc100_result_t mc100_platform_audio_read(uint8_t *out, size_t capacity,
                                       size_t *actual, uint32_t timeout_ms);
mc100_result_t mc100_platform_audio_stop(void);
mc100_result_t mc100_platform_storage_mount(void);
mc100_result_t mc100_platform_storage_unmount(void);
const mc100_io_t *mc100_platform_storage_io(void);
void *mc100_platform_storage_context(void);
void mc100_platform_led(bool on);
bool mc100_platform_card_level(void);
mc100_result_t mc100_platform_adc_mv(int *pin_mv);
uint64_t mc100_platform_now_ms(void);
```

- [ ] 先建立假 IDF 调用记录器，用 test_driver_contract 断言 board 引脚、I2S0/PDM RX、单声道 16 kHz、SD 1-bit/20 MHz、USB 控制台、LED 默认关闭。记录器只证明调用参数，不记为外设运行成功。
- [ ] 根据 v6.1 的 `driver/i2s_pdm.h` 实现 channel init/enable/read/disable，启动丢弃 40 ms，部分读取交 T03 组帧器；核对 DMA 数量/实际每描述符大小，不能把一帧等于一次读取当成驱动保证。I2S 所有调用只出现在 Audio 所有者上下文。

```c
/* 实现时依该修订完整填写 pin/slot/clock 结构并检查每个返回值。 */
i2s_pdm_rx_clk_config_t clock = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000);
i2s_pdm_rx_slot_config_t slot =
    I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
/* SELECT=GND 的 slot_mask 是 H02 试验参数，不从能编译推断采样正确。 */
```

- [ ] SDMMC host 配置 20 MHz、slot.width=1、正确 CLK/CMD/D0；V1 支持 FAT32，启用至少 128 字符的长文件名并核对其工作区预算，挂载失败不得 format。实现 T05 io 适配：FatFs 的实际读写长度、`f_sync`、`f_truncate`、独占创建、不覆盖重命名、目录枚举和空间统计。不得混用独立文件描述符绕过 Storage 所有权。
- [ ] 对已选预分配路径做单独 target 编译/链接 probe；若 FF_USE_EXPAND=0，使用经验证的 f_lseek 扩展和长度检查，不能使用未链接的 helper。宿主碎片镜像先测试预分配不足；真实卡延迟明确留给 H03。
- [ ] 配置 ADC 校准、GPIO21、CD 输入和 USB 日志。卡有效电平作为明确 EVT 配置候选、初始 low-active，但在 H01 前不得标为确认；缺少实测时构建元数据标 `board_io_verified=false`，发布门禁检查此字段。
- [ ] Run：`powershell -File firmware/tools/build.ps1 -Profile evt`；脚本 profile 列表限定 host/evt/release，未知 profile 失败。检查 SDK API/链接/静态内存结果，通过后本地提交 `feat: add MC100 ESP-IDF peripheral adapters`，硬件运行项仍 NOT_RUN。

### T10：电池监控、卡检测、NVS、USB 与低功耗策略

依赖：T02/T09。产出：可诊断的外围状态策略；需求 R10/R16–R19/R21/R22。

**Files:** 创建 `mc100_core/include/mc100_config.h`、`config.c`；`mc100_platform_espidf/monitor.c`、`config_nvs.c`、`diagnostics.c`、`power.c`；测试 `test_config_migration.c`、`test_card_debounce.c`、`test_led_policy.c`、`test_diagnostic_commands.c`。

**Interfaces:** `mc100_config_t` 含 schema_version、vad_mode、battery_cfg、EVT/产品参数来源标识（字段名 `calibration_profile`）；`mc100_config_validate(const mc100_config_t *)` 返回 `mc100_result_t`。Monitor 只通过 `mc100_platform_post_event(const mc100_event_t *)` 发布状态，不调用存储或音频驱动。

诊断只读快照 `mc100_status_t` 由 State/各 owner 的只读统计组合，公开 `mc100_status_snapshot(mc100_status_t *)`，`mc100_status_json(const mc100_status_t *, char *out, size_t capacity, size_t *used)`；容量不足返回 MC100_FULL，不越界或输出伪完整 JSON。

- [ ] 先写 ADC 无效、短时抖动、低电启动不执行写卡、50 ms CD 去抖/500 ms 恢复、正常 LED off、NVS 未知版本不擦除、过长/未知 USB 命令拒绝等测试；运行 `ctest --test-dir firmware/out/host -R 'battery_policy|config_migration|card_debounce|led_policy|diagnostic_commands' --output-on-failure`。
- [ ] 实现校准后 ADC mV→VBAT 换算，用 64 位整数防溢出并可追踪 rounding。输入不在测量可信范围/校准失败发 ADC_INVALID，不显示虚构电量和充电状态；需要明确定义纯 USB 无电池 EVT 模式才允许台架诊断越过产品低电保护。

```c
/* 放入可移植策略模块供单测，单位为 mV；不是 ADC 校准替代。 */
static int mc100_vbat_from_pin_mv(int pin_mv) {
    if (pin_mv < 0 || pin_mv > 2000) return -1;
    return (int)(((int64_t)pin_mv * 1330 + 165) / 330);
}
/* 例：ADC 引脚 1000 mV 对应四舍五入后的 4030 mV。 */
```

- [ ] 实现 150 ms 初始稳定等待、100 ms 有界采样、3 s 低电持续/30 s 恢复滞回的 EVT 试验配置；低电关停由 State 下发，Storage 自己关闭/卸载。HOLD 采用停止外设+定时检测的独立策略，保留是否成功卸载/是否低于禁止写阈值的诊断。
- [ ] NVS 有版本和范围校验，仅显式配置变更时写；初始化失败返回 CONFIG_INVALID 并保留现场，不使用示例中常见的无条件 erase。日志限速、正常无音频内容；USB 无主机/背压不会阻塞 State 或 Audio。
- [ ] `status` JSON 满足架构 §8；EVT 命令只接收固定长度命令、显式参数，改变状态的操作统一入 State；产品 profile 关闭破坏性诊断。看门狗基于进展而不是定时假喂狗。
- [ ] 全 host 回归 + evt 交叉构建通过，检查没有 CHG/PGOOD/GEK/soft-power-off 虚构接口；本地提交 `feat: add battery supervision and bounded diagnostics`。

### T11：任务集成、竞态和资源预算门禁

依赖：T08/T09/T10。产出：能交给实板测试的完整目标软件；需求 R07/R15/R19/R20。

**Files:** 创建 `mc100_platform_espidf/runtime.c`、`include/mc100_runtime.h`、`firmware/main/app_main.c`；更新组件/主入口 CMake，移除已替代的 `hello_world_main.c`；创建 `firmware/tests/test_runtime_protocol.c`、`test_memory_budget.c`、`firmware/tools/validate_artifacts.py`。

**Interfaces:** `mc100_runtime_init(void)`、`mc100_runtime_start(void)` 返回 `mc100_result_t`；启动失败保持 FAULT/可诊断，不退回示例重启循环。实现 T10 `post_event` 与状态快照；各任务通过内部固定容量 channel 交互，不把 FreeRTOS 头泄漏到纯核心。

- [ ] 先以假调度器测试控制队列满、完成通知满、Audio 未启动/Storage 卡住、关闭后晚到消息、snapshot 双释放、stop 唤醒和超时；每个 owner 每次处理有界工作，不使 Audio 负责补卡文件。Run：`ctest --test-dir firmware/out/host -R 'runtime_protocol|memory_budget' --output-on-failure`。
- [ ] 建立 Audio/State/Storage/Monitor/Diagnostics 五任务，优先级/核/初始栈按架构 §3.2；32 项事件队列、8 项控制队列、关键 ACK sticky mailbox。SPSC 使用已证明的 acquire/release 或短临界区；SMP 压力测试覆盖实际队列实现，不仅模拟业务时间。
- [ ] 装配 BOOT 顺序：版本/配置与资源→生成 16 byte boot_id→电池有效且安全→Storage 挂载/恢复/准备→Audio 初始化稳定→READY→LISTEN。记录 boot_id 只为区分会话，不宣称密码学安全随机；名字碰撞有界重试。State 未收到完整 READY 前不能对外显示“正在监听录音”。

```c
void app_main(void) {
    mc100_result_t r = mc100_runtime_init();
    if (r == MC100_OK) r = mc100_runtime_start();
    if (r != MC100_OK) {
        /* 实现函数属于 runtime：仅初始化必要诊断，不再次启动录音。 */
        mc100_runtime_report_start_failure(r);
    }
}
```

`mc100_runtime_report_start_failure(mc100_result_t)` 在 `mc100_runtime.h` 声明并由该任务实现；不得假设失败前任何对象都已初始化。

- [ ] map/size 脚本核对 229,184 byte 大缓冲预算、34,816 byte 初始任务栈、分区余量和无 PSRAM 引用。对五任务、队列、VAD、FAT/USB 记录静态与动态预计分配，目标实际剩余堆/栈门槛留给 H07，不能从 map 推出实际高水位已通过。
- [ ] PM 默认正确使用驱动锁；导出实际 CPU/APB/锁信息，支持受控比较 80 MHz 与 DFS 候选，不强设不可用 40 MHz 启动项。禁无线、空闲阻塞、正常 LED off 的配置自动检查。
- [ ] 全 host/sanitizer 回归、干净 evt 构建通过，删除仅属于 hello_world 的测试预期（如存在），不删用户其他测试；本地提交 `feat: integrate MC100 recorder tasks and resource checks`。

### T12：CI、候选包与无板阶段放行

依赖：T11。产出：M3 / `SW_READY_FOR_EVT`；需求 R20/R22/R25。

**Files:** 创建 `.github/workflows/firmware.yml`、`firmware/tools/package-release.ps1`、`docs/reports/software-readiness.md`；更新 `firmware/README.md`、依赖锁和 `validate_artifacts.py`。

**Interfaces:** CI 两条主链：host 测试/ASan/UBSan/语料评估；固定 SDK 的 target clean build/分区/size 检查。`package-release.ps1 -Profile evt -Output <new-directory>` 只写明确的新产物目录，存在同名关键产物时拒绝覆盖。

- [ ] 先给 artifact validator 写测试：缺 sdk revision、缺 firmware hash、HIL 未跑却标 PASS、应用越分区、依赖许可证缺失、测试失败的包均必须返回非零；新包只允许 `SW_READY_FOR_EVT` 且 H01–H08=NOT_RUN。
- [ ] 工作流固定 SDK 修订、Python/CMake/编译器/第三方校验；CI action/container 版本应使用当时已核验的固定 digest/SHA 并写锁文件，不在计划里伪造不存在的锁值。禁止在线构建每次拉 latest 或依赖本机生成 sdkconfig。
- [ ] 启用 host deterministic tests、sanitizer、短故障矩阵；完整扇区/48 h 虚拟长测作为本地候选放行或 CI 专项 job。真实语料若不能上传公开运行器，使用受控存储/本地报告并保持许可证/哈希可验证，不公开私人录音。
- [ ] 在新输出目录执行 `powershell -File firmware/tools/test-host.ps1` 和 `powershell -File firmware/tools/build.ps1 -Profile evt -Clean`，Clean 只处理脚本明确且验证在 project output 根内的生成目录，不能删除工作区/用户 sdkconfig/音频。
- [ ] 包含 bootloader/partition/app bin、ELF、map、实际 flasher_args、SDK/依赖/配置/板/格式版本、SHA256、测试报告、README；烧录使用构建产物地址，不能复用旧 bin 或手写猜测偏移。
- [ ] readiness 报告逐条填写 R01–R25 的软件证据，目标动态/HIL 标 NOT_RUN；软件门槛未全部通过不打候选。通过后本地提交 `ci: gate MC100 software readiness and package EVT artifacts`，不自动推送或创建 PR。

## 5. 板子回来后的验证任务

### T13：首板 bring-up 与功能联合验证

依赖：T12；板子、限流电源、可测电池、至少三型号专用测试卡与 USB 数据线就绪。产出：M4；H01–H04。

**Files:** 创建 `firmware/tools/hil_runner.py`、`docs/reports/evt-bringup.md`、`docs/reports/card-matrix.csv`、`docs/reports/battery-calibration.csv`；必要修改 `mc100_board` 与 EVT 配置，任何修复都新增 host 回归。

**Interfaces:** HIL runner 接受明确设备端口/板号/固件哈希/测试 ID/输出目录；只对指定设备发送受限诊断命令。首次烧录由操作者按[烧录指南](../../../MC100-PROGRAMMING.md)确认 TP/供电，自动工具不得猜测端口或 GPIO 测试点。

- [ ] 记录板快照哈希、板号、装配/短路检查，按 H01 测电源、EN、ROM 下载、芯片/Flash、校验和应用日志；失败立即停在该关卡，不启动耗电/写卡压力。
- [ ] H02 用已知音调/脉冲检查 PDM CLK/DATA、有效槽、40 ms 起始丢弃与采样率；把测得 slot/polarity 配置写入 BSP，并对所有已跑 host/target 回归。保存原始授权音频和仪器设置，不只写“声音正常”。
- [ ] H03 确认 CD 实际电平、20 MHz 三卡测试、插拔/卡满/预分配长延迟与 5 分钟轮换。记录每卡峰值写延迟与队列高水位；无证据不得调到 40 MHz。
- [ ] H04 校准 ADC、测供电跌落与收尾时间，确定停止/禁止写/恢复阈值及容差依据；更新 calibration_profile 与软件测试 golden 值。纯 USB 台架覆盖与产品电池策略分别记录。
- [ ] 从 READY→语音→静音关闭→连续轮换→低电/拔卡错误跑完整实板链路，逐样本/文件哈希检查而非仅播放一次。通过后本地提交 `test: record MC100 first-board functional validation`；未通过保留 FAIL 和复现证据，修复后重跑对应关卡。

### T14：功耗、掉电、耐久与产品放行

依赖：T13；专用断电/采样夹具准备好，破坏性测试目标明确。产出：M5；H05–H08 与产品风险关闭。

**Files:** 扩展 `firmware/tools/hil_runner.py` 的 powercut/soak 模式；创建 `docs/reports/evt-power.md`、`evt-powercut.md`、`evt-soak.md`、`release-checklist.md`；更新支持卡型清单和已验证配置。

**Interfaces:** 测试记录格式按验收文档 §6；断电夹具必须实际切断系统供电，不能用软件 reset 冒充 SW1 OFF。runner 必须记录切断窗口、随机 seed、供电/温度、卡型、前后哈希和恢复结果；对卡文件/夹具破坏操作设置明确目标校验与人工开始确认。

- [ ] H05 测 actual CPU/APB/PM 锁和电池侧电流，音频质量不变前提下优化；比较 LISTEN/RECORD/HOLD/USB 情况，说明 35 mA 和 24 h 目标实测是否达到。若受持续 PDM 限制不能达 40 MHz，应提交功耗预算/产品决策，不把时钟配置值当实际运行值。
- [ ] H06 每卡每窗口至少 100 次：PCM 写入、sync、轮换，三个卡型号至少 900 次；另覆盖修头/命名/恢复时掉电。完整历史文件哈希必须不变；本次录音只允许正确前缀或明确失败且保留原件，统计尾部损失。
- [ ] H07 执行 24 h 监听触发与 48 h 连续录音，≥3 台最终结构整机各 ≥24 h；持续记录 reset reason、堆/栈、queue peak/drop、CPU、温升、电池侧能耗；零非预期复位、零正常丢帧、无持续内存增长。
- [ ] H08 对指定样机完成 ≥1000 次开关循环，检查低电反复启动风险与最终结构/装配/充电/ESD 独立清单。未覆盖的电气/制造项目仍然阻止产品放行，不能在固件报告里自动勾选。
- [ ] 将每个 FAIL 修复为具体回归用例并重跑，核对实际发包镜像与最后通过的哈希一致；完整满足验证矩阵才将 release-checklist 标 EVT_PASSED。通过后本地提交 `test: qualify MC100 EVT reliability and power`；正式发布、推远程及不可逆安全配置另按用户指示执行。

## 6. 开发中的统一完成标准

一个任务完成必须同时满足：

- [ ] 与该任务的 R 编号逐条对应，不漏正常/错误/恢复路径。
- [ ] 新测试曾因缺失行为而失败，实施后新鲜运行通过；环境缺失不是功能红绿证据。
- [ ] 同源 host 测试通过；影响目标部分的改动还要干净 ESP32-S3 构建通过。
- [ ] 关键边界（所有权、序号、文件长度、CRC、超时、内存容量）有明确断言，不只检查函数返回 OK。
- [ ] 无无界队列、音频热路径堆分配、跨任务文件/驱动调用、自动格式化/擦除或敏感音频日志。
- [ ] 文档/配置/锁文件同步，报告准确区分 PASS/FAIL/NOT_RUN；未做实测不填测量数值。
- [ ] 检查最终 diff、`git diff --check`、提交范围；保留用户硬件 PDF 和其他未提交变更，不自动推送。

## 7. 当前执行入口

当前状态：**DEVELOPING；T01 开始，后续完成状态以开发进度中的实测证据为准；T13–T14 等板子回来后执行。**

下一项是 T01：固定 ESP32-S3-N8/ESP-IDF v6.1 的工程配置和板级能力，建立能够在电脑运行的同源测试入口。T01 的结束条件是 host board_contract 和新鲜 target 构建均通过，不是“搭了几个空文件”。之后继续 T02/T03/T04，优先形成可测试的录音完整性链路，不等待实板。
