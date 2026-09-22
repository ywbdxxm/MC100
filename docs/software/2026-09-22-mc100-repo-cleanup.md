# MC100 仓库整理计划

日期:2026-09-22
范围:仓库卫生、证据归位、文档纠错。**不写任何功能代码**,功能完成度整理前后完全一致。

---

## 已完成的核查(不是估计,是实测)

| 项目 | 结果 |
| --- | --- |
| Host 测试 | **19/19 通过**,从零 `-Clean` 重建,4.35 秒 |
| Target 构建 | **成功**,exit 0,产出 `mc100.bin` 317456 字节,build.ps1 全部护栏通过 |
| IDF 版本锁 | 本地 v6.1 HEAD `fff9895c` 与 `dependencies.lock.json` **一致**,工作区干净 |
| 产品代码 | 3179 行(components + main);测试 2264 行;host 垫片 535 行 |
| 磁盘占用 | `firmware/out` 1777 MB + `firmware/build` 92 MB |
| 文档链接 | 本地链接 **78** 条,死链 **18** 条(全部在 `MC100-PCB-REVIEW.md`) |
| PDF 可提取性 | 纯矢量文本,**零图片**,无需 OCR;原理图 5 页,PCB **7 页** |

### 两个构建环境(仓库里没有任何文档记录)

- **Host 测试需要 MSVC**,不是 ESP-IDF。CMake 缓存用 VS 2022 BuildTools 的 `cl.exe` 配置。
  在 IDF 环境里跑会失败:IDF 的 profile 整体替换 PATH,丢掉 Windows SDK,`cl.exe` 找不到
  `kernel32.lib` 和 `stdbool.h`。正确姿势:`vcvars64.bat` + 把 IDF 的 cmake/ninja/venv 前置到 PATH。
- **Target 构建需要 IDF v6.1**,且必须 dot-source `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`。
  `esp-idf\export.ps1` 会失败(它调的 `python` 解析到 uv 的 3.14)。
- **两者都必须从 `cmd` 批处理启动,不能从 Git Bash**。Git Bash 泄漏 `MSYSTEM`/`MINGW_*`,
  IDF 会拒绝激活("MSys/Mingw is no longer supported"),`$env:IDF_PATH` 为空,
  `build.ps1` 随后死在 null 方法调用上。

---

## 我造成的一处损失(需要你知情)

为建立基线我跑了 `test-host.ps1 -Clean`,该脚本 `Remove-Item -Recurse -Force` 整个 `out/host`,
连带删掉了 T06 掉电测试的 3 个 64 MiB 证据镜像(`seed.img` / `no-fault.img` / `failpoint-6.img`)。
`-Force` 不进回收站。`out/host` 从 234 MiB 变成 32 MiB。

**可重建** —— harness 源码在 stash `9d1cf09` 里完好:`test_fat_powercut.c`、`scenarios/powercut.json`、
`fat_diskio.c`、`fatfs_compat/`,以及给 `host/CMakeLists.txt` 加 63 行构建目标的 diff。
按你的决定,**整理完成后单独重建**(MSVC 断言会弹模态框,需要人工盯)。

原三镜像 SHA256(重建后用于比对,注意镜像含时间戳,未必逐字节一致):
- seed `17C729BF18CDD250FBC92DC2C4D91BA7319B5F4FE0B2F3136293B827A44F6080`
- no-fault `D990DA7846EF83DE486CF35CD497F27797E7AC93196CB578869079CB5DC53DA8`
- failpoint-6 `01D6CBD176AB9E73A1D975AB1F7E2FB5F74240F6EC1218F3A7B8145E57298721`

---

## P0 · 证据归档(**必须最先做**)

顺序是硬的 —— 先归档,再删任何东西。

1. 建 `evidence/2026-09-19-evt-com7/`,`git mv` 不适用(未跟踪),用复制 + 校验:
   - `firmware/out/device-com7/` 全部内容 —— 原厂 8MB Flash 备份、6 条实板录音
     (含 305 秒 / 9.6 MB 那条)、`.idx`、verification JSON、串口日志、已烧录镜像 `image-412615ac/`
2. 保留 `firmware/out/corpus-cache/ms-snsd/*.json` —— 这是 MS-SNSD **选/弃文件清单的唯一记录**,
   WAV 本身可按 pin 住的 commit 重取,这份清单不能。存到 `evidence/corpus-selection/`。
3. 对归档内容算 SHA256 存成 `evidence/SHA256SUMS`,P7 逐字节回验。
4. 对 `hardware/*.pdf` 算 SHA256 一并记录(已测:
   PCB `71af4557...`,SCH `cb88f1f4...`)。

`image-412615ac/` 处理:其中 `bootloader/` 有 13 MB 构建中间物(CMakeCache/CMakeFiles/build.ninja/
compile_commands)。建议只留可发布产物集 —— `mc100.bin`/`.elf`/`.map`、`sdkconfig`、
`project_description.json`、`flash_args`、`flasher_args.json`、`bootloader/bootloader.bin`、
`partition_table/partition-table.bin`,约 9 MB。**若你想整包冻结,我原样保留。**

---

## P1 · Git 收口

| 对象 | 动作 |
| --- | --- |
| HEAD | 当前在 `codex/mc100-vad`,切回 `main` |
| `codex/mc100-vad` | 删 —— 与 main 同 SHA `039df0f`,T07A 没有任何提交 |
| `codex/mc100-software` | 删 —— 已 ff 合并进 main |
| `codex/mc100-recovery` | **保留**,打注释 tag `t06-recovery-blocked-20260919`,tag 信息写明扇区门禁 |
| `stash@{0}` | 转成注释 tag `t06-powercut-harness-fail`,保住 1770 行工作 + harness 源码 |
| `PCB1.epro2` | `git rm` 提交(工作区已无此文件)。**按你的决定保留 git 历史,不改写** |
| `.gitignore:8` | 删掉 EasyEDA 那条规则及其"the design source is PCB1.epro2"注释 |

---

## P2 · 磁盘清理(约 1.74 GB)

删除(全部已确认可再生或无价值):

| 目标 | 大小 | 依据 |
| --- | --- | --- |
| `firmware/out/corpus-cache/*.tar.gz` | 482 MB | LibriSpeech **部分下载**,小于官方尺寸,校验不过,留着无意义 |
| `firmware/out/vendor-cache/` | 457 MB | zig + libfvad,**两者哈希已验证匹配**,可按哈希重取 |
| `firmware/out/host` `t06-host-core` `sanitizer-probe` | 404 MB | 可重建(host 已实测 19/19) |
| `firmware/out/{target,t06-target,exfat-target,t09-target}` | 380 MB | 可重建(target 已实测成功) |
| `firmware/build/` | 92 MB | 9/15 的死构建,裸跑 `idf.py build` 的产物 |
| 其余 `out/` 零散 .obj 目录 | ~40 MB | format / storage / audio / state 等试验产物 |
| `firmware/.cache/`、`tools/__pycache__/`、`sdkconfig`、`sdkconfig.old` | — | 生成物,已在 .gitignore |
| `firmware/pytest_hello_world.py` | — | IDF 模板自带,唯一需要 `git rm` 的 |
| `firmware/out/baseline-probe/` | 95 MB | 我这次验证建的,用完即删 |

**删除前必须完成 P0 归档。**

vendor 复取信息(已实测哈希匹配,写进正式文档):
- libfvad `codeload.github.com/dpirch/libfvad/zip/532ab666c20d3cfda38bca63abbb0f152706c369`
  962588 字节 SHA256 `A72A440F1A4E6ABE0204027EC3484F23B75395808DF3FDF1274E78E1D4623A97`
- zig 0.15.2 windows-x86_64 92614574 字节
  SHA256 `3A0ED1E8799A2F8CE2A6E6290A9FF22E6906F8227865911FB7DDEDC3CC14CB0C`
  (**注意:URL 在原始记录里缺失**,需按官方 index.json 重新确认后再写入文档)

---

## P3 · 构建输出与 IDE 索引

- 唯一 target 输出定为 `firmware/out/target/`。**不动 `build.ps1` 的安全护栏** ——
  它禁止输出到 `firmware/out` 之外、禁止路径含 junction/软链、强制校验 IDF commit 与分区表。
- **修一个真实的 IDE 故障**:`firmware/.vscode/settings.json:24` 的
  `--compile-commands-dir` 指向 `firmware/build`(9/15 的死构建),改指 `firmware/out/target`。
  *(注:根目录 `.clangd` 里并无此项,只有 `CompileFlags: Remove`。)*
- `firmware/README.md` 补两个构建环境的完整说明(上文那两段),以及"不要裸跑 `idf.py build`"。

---

## P4 · 文档归位与决策抽取

只搬目录、不改文件名,全部 `git mv` 保住历史:

```
README.md                    ← 重写(中文)
docs/hardware/               ← 根目录 4 个 MD 搬入
docs/software/               ← spec + plan 从 docs/superpowers/ 搬出
docs/decisions.md            ← 新建,见下
docs/reports/                ← 不动
evidence/                    ← P0 产物,gitignore
```

**用户追加硬性约束（2026-09-22）**：`PCB1.epro2` 保持删除，不迁移、不复制到
`hardware/`、`evidence/` 或其他目录。MC100 仓库及本次整理产生的提交不得包含任何
嘉立创/EasyEDA 工程或工程缓存（包括 `.epro2`、`.eprj`、`.easyeda/`）；硬件依据仅保留
仓库内 PDF 快照和可审计的文字记录。

**`docs/decisions.md`(新建)** —— `.superpowers/sdd/` 未被 git 跟踪,里面埋着 13 条
只存在于该处的设计裁决,丢了就没了。至少包含:

- 索引 FINAL/INCIDENT 终态语义(为何互斥、为何共用一个校验器)
- `snapshot_release` ≠ session release(双 bank / 重触发 / drain 的归属)
- 恢复文件命名:`.recovering_N.wav.part` → sync+close → 不覆盖重命名(绝不覆盖存疑数据)
- 故障路由:哪些故障可有界收尾成 INCIDENT,哪些禁止新写入
- **故障码域冲突(地雷)**:core `CONTROL_TIMEOUT=9` vs storage `LOW_BAT_INTERRUPTED=9`,
  **禁止数值直转,必须按名映射**
- FatFs `f_close` 隐含 `f_sync`,违反 no-write close 契约 → 脏关闭丢弃句柄并毒化挂载
- Wi-Fi 必须用 `MINIMAL_BUILD` 排除,Kconfig 置 `n` 无效(`ESP_WIFI_ENABLED` 是隐藏 SoC 默认 y)
- UBSan 走 Zig 而非 esp-clang(esp-clang 21.1.3 只有嵌入式后端)

然后删 8 个 `review-*.diff`(git 里有对应提交,可再生),保留 task 简报/报告。

---

## P5 · 硬件文档以 PDF 为准(按你的决定)

**这一步比原计划重,因为原计划的前提是错的** —— 它说"文档与 PDF 一致,只需修表述",
但实测两份文档**互相矛盾**,所以至少有一份与 PDF 不符。

### 5.1 可核验的:用 PDF 逐项核对

PDF 是纯矢量文本,BOM 表在 PCB PDF 第 3-4 页,含位号 / 封装 / 值 / 厂商料号 / LCSC 编号。
用 `esp-hardware-knowledge` 的 venv(pymupdf)提取,逐项比对物料号、位号数、GPIO 映射、页名器件。
不一致的一律改成 PDF 的值。

### 5.2 不可核验的:布线几何必须降级

`MC100-HARDWARE-DESIGN.md:794` 写 268 segments / 7 arcs / 80 vias / 6 pours / 5 fills,
`MC100-PCB-REVIEW.md:95-97` 写 255 / 4 / 78 / 4 / 6。**我已实测:
`segment`/`arc`/`via`/`pour`/`fill` 在 PCB PDF 中出现 0 次** —— 这些是 EasyEDA 的布线统计量,
不印在图纸上。按"以 PDF 为准",两组数字**都无法被权威源证实**。
处理:删除这两处数字,或改写为"源自 2026-09-09 的 EasyEDA 实时读取,PDF 快照无此数据,不可复现"。
**不做二选一** —— 挑一个等于凭空给未经证实的数字背书。

### 5.3 权威声明改口径(实测 10 处,不是 6 处)

7 处声称立创 EDA 云工程为准:`README.md:3`、`README.md:27`、
`MC100-HARDWARE-DESIGN.md:10`、`:815`、`MC100-PCB-REVIEW.md:3`、
`MC100-SCHEMATIC-REVIEW.md:3`、`MC100-PROGRAMMING.md:3`。
另有 3 处已声称以快照 PDF 为准(`docs/MC100-VALIDATION.md:9`、
`docs/MC100-DEVELOPMENT-STATUS.md:30`、spec:7)—— 与前 7 处直接冲突。
全部统一为:**以 `hardware/` 下 2026-09-18 PDF 快照为准**。

### 5.4 日期澄清与过期注释

- PDF 导出于 09-18,但设计内容日期为 P1/P2 = 09-08、P3–P5 = 09-02,需写清以免误读为差 10 天。
- 原理图 P2 页仍印着 `GEK100_35` side key 和 `D4 BAS116`,实际器件是 SW1 滑动开关、板上无 D4。
  标注"图纸注释过期,以实际器件清单为准"。

### 5.5 死链与跨仓链接

- **18 条死链**全部在 `MC100-PCB-REVIEW.md`,全指向不存在的 `../.easyeda/reviews/...`。
  在文档顶部加显著声明:"本评审基于 2026-09-09 的 EasyEDA 实时读取,原始证据文件已不存在,
  结论无法从本仓库内复现";死链改成纯文本引用,不再伪装成可点击。
- **另有 15 条链接指向仓库之外**(`AI-HRADWARE\docs\`),clone 或移动即失效:
  `MC100-PCB-REVIEW.md:168-172`、`MC100-PROGRAMMING.md:77-80`、`MC100-SCHEMATIC-REVIEW.md:137-142`。
  改为明确标注的外部引用。
- `MC100-HARDWARE-DESIGN.md:823-829` 有 7 条裸证据路径,作为仓库相对路径是死的。

### 5.6 状态类矛盾(修文档,不碰代码)

- `docs/MC100-DEVELOPMENT-STATUS.md:14` 写 **T06 = NOT_STARTED**,但 `codex/mc100-recovery`
  上有 4 个提交、核心复审通过、且存在已知可复现的耐久性 FAIL。**这是最危险的一条** ——
  只读已提交文档的人会重做 T06 并完全错过扇区门禁。
- spec:13 称 firmware 仍是会倒计时重启的 hello_world —— 假的,5 个组件都在。
- plan:540 称 "T01 开始"、plan:10 称 "暂无 MC100 板子" —— 与实板已连接、T01-T05/T09/T11 完成矛盾。
- `MC100-SCHEMATIC-REVIEW.md:103`、`MC100-PROGRAMMING.md:5,7` 称无固件/未烧录 —— 与 COM7 实测报告矛盾。
- 64GB exFAT 变更未传播:`VALIDATION:128` 仍要求 32GB 卡,spec:213 / plan:420 仍写 FAT32。
- `docs/reports/2026-09-19-evt-recording.md` 自相矛盾:`:15` 写 17/17,`:53` 写 19/19(实测 19)。
- IDF 版本不一:`HARDWARE-DESIGN:830`、`SCHEMATIC-REVIEW:103,143` 写 v6.0.2,其余写 v6.1(实际 v6.1)。
- `DEV-STATUS` 用了 `VALIDATION:143-147` 未定义的状态词(PASS_CORE / EVT_SLICE_PASS / DEFERRED 等)。

---

## P6 · README 重写(中文)

现有 29 行英文,且有三处明确失实:L3/L27 指向已不存在的 EDA 权威与已删除的 PCB1.epro2;
L29 称"目录刻意保持干净、不保留构建缓存",而实际有 1.7 GB;L23 称本次未做实板烧录,
与同文件 L13 及 COM7 报告矛盾。且全文未提 `build.ps1`、未链接 `firmware/README.md`。

重写为入口文档:这是什么 → 现在什么状态 → **怎么构建/测试(两个环境写清楚)** →
文档在哪 → 已知限制与未完成项(T06 扇区门禁阻塞、T07 VAD 未做、产品主流程不存在、
无声学/电池/掉电验收、固件运行时尚未调用 `mc100_recover`)。

---

## P7 · 验证

1. 重跑 host 测试(MSVC 环境)→ 必须仍是 **19/19**
2. 重跑 target 构建(IDF v6.1)→ 必须仍能产出 `mc100.bin`
   *注:镜像含构建时间戳,**功能等价但非逐字节一致**,不能拿哈希当验收标准*
3. 死链脚本回归 → 78 条本地链接,死链必须仍是那 18 条,不能多一条
4. `evidence/SHA256SUMS` 逐字节回验 → **这些必须逐字节一致**
5. `git status` 干净,`git log` 未被改写(SHA 与整理前一致)

---

## 两个必须提前说清的

**一、这个计划不写任何功能代码。** T06 恢复、T07 VAD、产品主流程都只是被准确记录,不会被实现。
整完你得到一个干净、可信、能重建的基线,但功能完成度和现在一模一样。

**二、PCB 评审的证据链已断,且无法在本次补上。** 18 条死链意味着那份文档的结论现在不可验证 ——
可能都对,但仓库里没有任何东西能证明。我的处理是诚实标注,不是删掉或假装有效。
如果要拿它去投板,需要重新做一次 PCB 评审,而那需要 EasyEDA(你说这次不碰)。
这是已知缺口,不是整理能补的。
