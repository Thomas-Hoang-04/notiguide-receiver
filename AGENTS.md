# Repository Guidelines

## Project Structure & Module Organization
This is an ESP8266 RTOS SDK firmware project. The repo root contains both [Makefile](/workspaces/receiver-esp8266/Makefile) and [CMakeLists.txt](/workspaces/receiver-esp8266/CMakeLists.txt), but the checked-in Makefile still includes `$IDF_PATH/make/project.mk`, so treat `make` as the default workflow. Application code lives in `main/`: `hello_world_main.c` is the current entry point, and `main/rf/` contains RF timing, decode, and receiver support code. `tools/hostcc` is a host compiler wrapper used during config/build. `build/` is generated output. `sdkconfig` is local-only; commit config baselines intentionally via `sdkconfig.ci.*`.

## Indexed SDK Findings
The local SDK is installed at `/opt/esp/ESP8266_RTOS_SDK`. Source `. /opt/esp/ESP8266_RTOS_SDK/export.sh` before building if tool paths are not already loaded; the script sets `IDF_PATH`, exports tool locations, and checks Python dependencies. The SDK currently exposes both legacy Make support (`make/project.mk`) and newer tooling (`tools/idf.py`), but its own `export.sh` still ends by directing users to run `make`.

The SDK contains 45 top-level components. The ones most relevant to this repo are `esp8266` for GPIO/driver APIs, `freertos` for task behavior, `spi_flash`, `esptool_py`, and `lwip`. Before changing RF interrupt or timing logic, check these upstream references first:

- `components/esp8266/include/driver/gpio.h`
- `components/esp8266/driver/ir_rx.c`
- `examples/peripherals/gpio`
- `examples/peripherals/hw_timer`
- `examples/peripherals/ir_rx`

If this project grows network or persistence features, start with `components/mqtt/esp-mqtt/mqtt_client.c`, `examples/protocols/mqtt/tcp`, and `components/nvs_flash/include/{nvs_flash.h,nvs.h}`.

## Build, Test, and Development Commands
Run commands from the repo root:

```bash
. /opt/esp/ESP8266_RTOS_SDK/export.sh
make menuconfig
make -j4
make flash
make monitor
make app-flash
```

Use `make -jN` for parallel builds. Prefer `make flash monitor` during board bring-up or RF debugging.

## Coding Style & Naming Conventions
Use 4-space indentation and same-line braces, matching `main/`. Prefer `snake_case` for functions and variables, `UPPER_SNAKE_CASE` for macros, and descriptive module names such as `rf_receiver.c`. Keep comments sparse and useful; brief Doxygen-style file headers are fine where already established.

## Testing Guidelines
There is no dedicated unit test suite. Minimum validation is a clean `make -j4` build. For hardware-facing changes, verify with `make flash monitor` and include boot logs, decoded RF output, GPIO selection, and any `menuconfig` deltas in the PR.

## Commit & Pull Request Guidelines
Current history uses short imperative summaries (`init commit`). Keep commits focused and descriptive, for example `tighten rf pulse tolerance handling`. Pull requests should state hardware used, behavior changed, validation steps run, and any serial logs needed to review the change.

<!-- gitnexus:start -->
# GitNexus — Code Intelligence

This project is indexed by GitNexus as **receiver-esp8266** (730 symbols, 1137 relationships, 40 execution flows). Use the GitNexus MCP tools to understand code, assess impact, and navigate safely.

> If any GitNexus tool warns the index is stale, run `npx gitnexus analyze` in terminal first.

## Always Do

- **MUST run impact analysis before editing any symbol.** Before modifying a function, class, or method, run `gitnexus_impact({target: "symbolName", direction: "upstream"})` and report the blast radius (direct callers, affected processes, risk level) to the user.
- **MUST run `gitnexus_detect_changes()` before committing** to verify your changes only affect expected symbols and execution flows.
- **MUST warn the user** if impact analysis returns HIGH or CRITICAL risk before proceeding with edits.
- When exploring unfamiliar code, use `gitnexus_query({query: "concept"})` to find execution flows instead of grepping. It returns process-grouped results ranked by relevance.
- When you need full context on a specific symbol — callers, callees, which execution flows it participates in — use `gitnexus_context({name: "symbolName"})`.

## Never Do

- NEVER edit a function, class, or method without first running `gitnexus_impact` on it.
- NEVER ignore HIGH or CRITICAL risk warnings from impact analysis.
- NEVER rename symbols with find-and-replace — use `gitnexus_rename` which understands the call graph.
- NEVER commit changes without running `gitnexus_detect_changes()` to check affected scope.

## Resources

| Resource | Use for |
|----------|---------|
| `gitnexus://repo/receiver-esp8266/context` | Codebase overview, check index freshness |
| `gitnexus://repo/receiver-esp8266/clusters` | All functional areas |
| `gitnexus://repo/receiver-esp8266/processes` | All execution flows |
| `gitnexus://repo/receiver-esp8266/process/{name}` | Step-by-step execution trace |

## CLI

| Task | Read this skill file |
|------|---------------------|
| Understand architecture / "How does X work?" | `.claude/skills/gitnexus/gitnexus-exploring/SKILL.md` |
| Blast radius / "What breaks if I change X?" | `.claude/skills/gitnexus/gitnexus-impact-analysis/SKILL.md` |
| Trace bugs / "Why is X failing?" | `.claude/skills/gitnexus/gitnexus-debugging/SKILL.md` |
| Rename / extract / split / refactor | `.claude/skills/gitnexus/gitnexus-refactoring/SKILL.md` |
| Tools, resources, schema reference | `.claude/skills/gitnexus/gitnexus-guide/SKILL.md` |
| Index, status, clean, wiki CLI commands | `.claude/skills/gitnexus/gitnexus-cli/SKILL.md` |

<!-- gitnexus:end -->
