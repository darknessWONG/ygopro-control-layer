# 第三方组件与许可（THIRD-PARTY）

本仓库（`ygopro` fork，GPLv2）内含以下第三方组件。分发本仓库或其编译产物时，请保留各自的许可声明。

| 组件 | 目录 | 许可 | 说明 |
|---|---|---|---|
| ygopro 客户端（上游） | 仓库根 | **GPLv2** | `LICENSE`，派生作品整体沿用 GPLv2 |
| ocgcore（决斗内核） | `ocgcore/` | **MIT** | `ocgcore/LICENSE`（Copyright Fluorohydride） |
| 卡片脚本 | `script/` | **GPLv2** | `script/LICENSE` |
| irrlicht（渲染/GUI） | `irrlicht/` | zlib | 保留 `irrlicht/` 内许可声明 |
| zlib | `zlib/` | zlib | |
| libpng | `png/` | libpng | |
| libjpeg | `jpeg/` | IJG | |
| freetype | `freetype/` | FreeType License (FTL) / GPLv2 双许可 | |
| lzma | `lzma/` | public domain | |
| miniaudio | `miniaudio/` | public domain / MIT-0 | |
| mbedtls（若启用） | 由 premake 依赖拉取 | Apache-2.0 | |
| sqlite3 | `sqlite3/` | public domain | |
| lua | `lua/` | MIT | |
| premake | `premake/` | BSD-3-Clause | 仅构建工具 |

## 不在本仓库 / 不属于开源许可

- **卡牌图片（`pics/`）**、**`cards.cdb`**、扩展包、卡组、对局录像：属于 Konami / 社区数据，**不在本仓库**，也不随本仓库分发。
- **MyCard 客户端与商标**：本 fork 与 MyCard / Konami 官方无隶属关系，仓库名与描述均未暗示官方身份。

## 本仓库新增代码

`gframe/control*.cpp` / `control*.h`（AI 控制层）为本仓库新增，按 **GPLv2** 授权（与客户端整体一致）。

配套的桥接层 `ygopro-mcp`（MCP server、策略、工具脚本）是**独立进程**、通过 HTTP 与本客户端通信，不链接本仓库代码，按 **MIT** 单独发布。
