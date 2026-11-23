# 13SentinelsRecomp

《十三机兵防卫圈 / 13 Sentinels: Aegis Rim》（Nintendo Switch 版）PC 原生静态重编译运行器。  
A native Windows PC static recompilation runtime for *13 Sentinels: Aegis Rim* (Nintendo Switch), built upon [peiikai/LASRecomp](https://github.com/peiikai/LASRecomp).

---

## 版权与合规声明 / Legal & Copyright Notice

> [!IMPORTANT]
> - **零内置版权资产 / Zero Bundled Copyrighted Assets**：本仓库与所有发行包**不包含**任何《十三机兵防卫圈》游戏镜像、NSO 代码、RomFS 数据、字体、主机固件或解密密钥（`prod.keys`）。  
> - **Not an Emulator**：本项目通过本机离线工具链将用户自己合法提取的 Switch 版游戏代码直接静态重编译为原生 x86-64 DLL（`lasgame_<build16>.dll`），配合 Vulkan 1.3 渲染与 HLE 系统服务在 Windows PC 上原生运行。  
> - 本项目**不提供**任何获取游戏 ROM 或密钥的途径或指导。

---

## 游戏版本支持矩阵 / Supported Game Versions

| 地区与版本 / Region & Version | Title ID | Main Build ID（前 16 位） | 支持状态 / Status | 内置游戏文本语言 / Game Languages |
|---|---|---|---|---|
| **亚洲版 / Asia v1.00** | `01008D7016438000` | `311947020F64F574` | **✅ 已实测支持 / Verified** | 简体中文、繁體中文、한국어 |
| **欧美版 / US & EU v1.10** | `01003FC01670C000` | `4B632573CA5C160E` | **✅ 已实测支持 / Verified** | English, Français, Deutsch, Italiano, Español |
| **欧美版 / US & EU v1.00** | `01003FC01670C000` | `57212E16CB83BB55` | **⚠️ 已识别（建议更新至 v1.10）/ Detected** | English, Français, Deutsch, Italiano, Español |
| **日版 / Japan (JP)** | — | — | **⏳ 暂未适配验证 / Not Yet Adapted** | — |

---

## 主要特性 / Features

- **原生 60 FPS 与 Vulkan 1.3 渲染**：实时稳定 60 FPS，支持无边框/独占全屏、多档垂直同步（`mailbox` / `fifo` / `immediate`）与着色器预热缓存。
- **超采样分辨率与画质实验选项**：支持内部渲染倍率（`match` / `1x` / `1.5x` / `2x` 及缩放档位）、输出分辨率选择（`auto` / `720p` / `1080p` / `1440p` / `4k`）、FSR 1 锐化放大与 SMAA 抗锯齿。
- **五国语言前端与游戏内实时语言切换**：启动前端与游戏内菜单（`F1` 或手柄 `Minus + Plus`）均支持 **简体中文 / 繁體中文 / English / 日本語 / 한국어** 高 DPI 原生无缺字切换，并可在设置中切换游戏语言（写入 `13sentinels.ini`）。
- **全流程手柄操控与完整改键**：原生支持 Xbox / Switch 手柄双按键布局预设，启动器与设置页可全程使用手柄浏览操作，同时支持键鼠与手柄全按键自定义映射。
- **零版权本机自构建与便携/存档管理**：首次运行自动并发生成并编译 `main` / `sdk` / `subsdk0` 三模块为 `lasgame_<build16>.dll`（支持增量缓存续编与旧 `larecomp.ini` 兼容迁移），并提供存档快捷定位与一键备份（`save_backups/`）。

---

## 系统要求 / System Requirements

- **OS**：Windows 10 / 11（64-bit）
- **CPU**：支持 AVX2 与 FMA 指令集的 x86-64 处理器
- **GPU**：支持 **Vulkan 1.3** 并安装最新驱动的独立显卡或核显
- **内存 / 磁盘**：建议 16 GB RAM（最低 8 GB），首次本机重编译需要约 4~8 GB 可用磁盘空间

---

## 三步快速上手 / Quick Start

1. **准备合法解包的游戏目录 / Prepare Extracted Game Files**  
   使用您自备的正版 Switch 版游戏文件解包出包含 **`exefs`**（内含 `main`、`sdk`、`subsdk0`、`main.npdm`）与 **`romfs`** 目录的文件夹。（注：标准发行包为避免依赖玩家本机安装 .NET 9.0 Runtime，不内置 `.nsp/.xci` 解包器，请直接选择已解包的文件夹。）
2. **启动前端并执行本机编译 / Launch & Local Self-Build**  
   运行 **`13SentinelsRecomp.exe`**，在首页选择刚才准备好的游戏目录，确认区域与版本号校验通过后点击 **「本地构建游戏模块 / Build Game Module」** 或 **「开始游戏 / Start Game」**；首次全量生成与编译三模块通常需要约 **6~7 分钟**，后续启动秒进（有缓存复用）。
3. **开始游玩 / Play**  
   随时按 **`F1`** 或手柄 **`Minus + Plus`** 打开游戏内设置菜单调节画质、音频、语言与按键映射；所有配置保存于 `13sentinels.ini`。

---

## 命令行与启动器集成参数 / CLI Options (`RecompLauncher` Compatible)

支持以下标准 CLI 参数用于批处理脚本或第三方启动器集成（详见 [recomplauncher.json](recomplauncher.json)）：

| 参数 / Flag | 说明 / Description |
|---|---|
| `--game-dir <path>` | 指定包含 `exefs` 与 `romfs` 的已解包游戏根目录 / Specify the extracted game folder containing `exefs` and `romfs`. |
| `--build-only` | 仅执行本机重编译生成 `lasgame_<build16>.dll` 并在完成后退出；向 `stdout` 输出去重后的 `progress <0.000~1.000> <stage>` 进度行 / Build `lasgame_<build16>.dll` headlessly and exit, emitting deduplicated `progress <0.0~1.0> <stage>` lines on `stdout`. |
| `--play` | 跳过启动器界面直接以当前已配置/传入目录启动游戏 / Skip the launcher UI and boot directly into the game. |

---

## 从源码构建 / Building from Source (Windows MSYS2 UCRT64)

请先安装 MSYS2 UCRT64 环境依赖（参考 [docs/BUILD-windows.md](docs/BUILD-windows.md)），再准备 LGPL FFmpeg、Zig 与代码生成器：

```bash
bash tools/build_deps_windows.sh
bash tools/fetch_zig.sh
make -j8 GEN_PY=/path/to/python.exe gen-helper
make -j8 dist
```

生成的可分发压缩包位于 `build/dist/13SentinelsRecomp-v1.0.0-beta1-windows.zip`。

---

## 致谢与开发声明 / Credits, Attribution & Disclosure

- **上游致谢 / Upstream Attribution**：本项目核心静态重编译运行器、AArch64 指令提升/转译框架、HLE 系统服务与 NVN-to-Vulkan 后端源自 [**peiikai/LASRecomp**](https://github.com/peiikai/LASRecomp)（MIT License）。  
  **请勿向 `peiikai/LASRecomp` 上游仓库反馈本项目的任何问题或提交 Issue！** 如遇《十三机兵防卫圈》适配、编译或运行问题，请仅在本项目仓库提交反馈。
- **开发方式透明声明 / Development Transparency**：本项目面向《十三机兵防卫圈》的适配分析、多语言 UI 改造、三模块自构建闭环、跳转表加固及全流程验证由**人类主导架构决策、测试设计与验收**，并**结合 AI 辅助编码开发完成**。
- **第三方许可 / Third-Party Licenses**：详见 [LICENSE](LICENSE) 与 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。
