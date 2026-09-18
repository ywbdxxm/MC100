# MC100

The authoritative MC100 schematic and PCB are stored in the EasyEDA Pro cloud project named `MC100`.

## Software development baseline — 2026-09-18

The user-provided [schematic PDF](hardware/SCH_Schematic1_2026-09-18.pdf) and [PCB PDF](hardware/PCB_PCB1_2026-09-18.pdf) are the hardware snapshots used for the new software plan. All five schematic pages and seven PCB pages were inspected. This is not a fresh native EDA DRC or manufacturing release; snapshot hashes, source locators, and stale schematic annotations are recorded in the validation document below.

- [软件架构设计](docs/superpowers/specs/2026-09-18-mc100-software-architecture-design.md): portable C core, State/Audio/Storage ownership, two-second prerecord, VAD, bounded buffers, WAV/CRC journal, recovery, and hardware limits.
- [完整软件开发计划](docs/superpowers/plans/2026-09-18-mc100-software-development.md): T01–T12 develop and test without a board; T13–T14 cover board bring-up and qualification.
- [硬件依据、需求追踪与验收矩阵](docs/MC100-VALIDATION.md): confirmed connections, software tests, and H01–H08 hardware gates.

Status: portable recording/storage core and a manual USB bench firmware are implemented. Board bring-up is underway on the explicitly authorized COM7, USB powered without a battery. See [development status](docs/MC100-DEVELOPMENT-STATUS.md) and [actual bench evidence](docs/reports/2026-09-19-evt-recording.md). The software baseline uses the project's ESP-IDF v6.1 and ESP32-S3-MINI-1-N8 (8 MB Flash, no PSRAM). A successful host test or cross-build is not proof of acoustic quality, SD power-loss tolerance, or battery life. Automatic VAD/product lifecycle and full qualification are not yet complete.

## Existing hardware reviews

Current hardware documentation: [MC100-HARDWARE-DESIGN.md](MC100-HARDWARE-DESIGN.md), reconciled with the live schematic and PCB on 2026-09-08. The board uses a slide switch to control the LDO enable pin; the earlier GEK100 push-button control is no longer present. Firmware behavior and endurance figures in the document remain targets until validated on hardware.

Current schematic review: [MC100-SCHEMATIC-REVIEW.md](MC100-SCHEMATIC-REVIEW.md). Open items cover recording integrity at switch-off, charge-timer tolerance, USB input inrush, and LDO operating margins. No definite microphone wiring fault was identified; its combined input/output level specification is tracked as a documentation clarification and prototype measurement item, without requiring a circuit change on that evidence alone. The selected battery is confirmed to have three wires and a 10 kOhm NTC. All five schematic DRC runs reported zero violations; P4 still has a title-block warning in the strict check.

Current PCB review: [MC100-PCB-REVIEW.md](MC100-PCB-REVIEW.md), refreshed from the live design on 2026-09-09. All five schematic pages and 234 PCB pad records match across 56 placements, and native DRC passed. The new 01:03 Gerber confirms the corrected four-sector microphone paste opening and 0.60 mm NPTH. USB now runs on Inner2 over Inner1 ground; the previous mid-route USB/SD reference-plane split crossings are resolved. Remaining layout recommendations cover local USB via clearances and nearby ground vias, R22 at the card end, C20 far from the SD supply pin, and C8/C9 near the LDO. The previous six silkscreen-to-pad bounding-box warnings did not reproduce in actual Gerber geometry. Fabricator stackup and final assembly/stencil matching remain pending; this is not a manufacturing release. The current effective routing has 255 line segments, 4 arcs, 78 standalone vias, 4 enumerable pours, and 6 static copper fills. End-of-review reads confirmed the inspected geometry and rules were unchanged.

Programming guide: [MC100-PROGRAMMING.md](MC100-PROGRAMMING.md). The present connections support native USB ROM download with SW1 on and factory-default download eFuses. TP3 is GPIO0/BOOT, TP4 is EN/RESET, TP1 is ground, and TP2 is 3V3 for measurement only. All four bottom testpoints have exported solder-mask openings. UART0 TX/RX are not brought out; application logging needs USB Serial/JTAG console configuration. No physical flashing or MC100 firmware build was tested in this review.

SMT service compatibility was checked against the domestic JLC SMT catalog on 2026-09-08 for all 25 distinct BOM part numbers. U3 / C2913206 and U4 / C42372687 are explicitly marked standard-SMT-only; the remaining 23 part numbers (50 placements) have no such restriction label. Full assembly with the current U3/U4 therefore needs the standard service. See hardware design section 12.4 for the complete mapping and its order-validation limits. Basic/extended library class is separate from economic/standard assembly service.

When documentation conflicts with the live EasyEDA schematic or PCB, inspect the current EDA design and update the documentation. Do not use an older document snapshot to infer the current components, pin assignments, or routing status.

This directory is intentionally clean. Keep current project documentation, firmware sources/tests, hardware snapshots, and released manufacturing exports in their designated directories. Do not retain EasyEDA screenshots, intermediate PCB dumps, routing trials, rollback journals, build caches, private audio, or temporary inspection logs.
