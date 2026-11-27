# ygopro-control-layer

> 给 **YGOPro 客户端**（fork 自 [mycard/ygopro](https://github.com/mycard/ygopro)）加的一层**进程内 HTTP 控制层**：把对局决策语义化（召唤/特召/盖放/发动/攻击/连锁/选卡/先攻后攻…），让 **LLM / agent 通过 MCP** 像人一样打牌。
>
> **这层控制接口才是本项目的本体**；上游 ygopro 的游戏逻辑、协议、卡牌规则**完全没改**。

⚠️ **当前版本面向"AI 驱动"，不建议/也不方便纯手动游玩** —— 原因见 [「手动游玩的限制」](#手动游玩的限制重要)。

---

## 这个项目比上游多了什么

| 能力 | 说明 |
|---|---|
| **HTTP 控制服务（进程内）** | `127.0.0.1:7788`（房主）/ `7789`（客机），JSON 接口：`/state` `/prompt` `/wait_prompt` `/actions` `/act_id` `/net` `/ui` … |
| **语义化决策** | 把内核 prompt 翻译成 `{id, kind, label}` 动作列表（召唤/覆盖/发动/攻击/连锁/是否/选卡/位置…），LLM 只选 id，不用拼 CTOS/STOC 二进制帧 |
| **真实 UI 注入** | 动作优先通过客户端自己的按钮/鼠标事件执行（与真人点击同一 handler），点不中时自动回退直连响应，保证不卡死 |
| **连锁语义** | `chainStack` / `chainDepth` / `chainingTo` / `chainCandidates` / `canDecline`，支持多段连锁 |
| **先/后攻选择** | 开局 TP 由调用方选择（`kind:"tp"`，id 0=先攻 / 1=后攻），不再默认后手 |
| **网络存活与重连** | `/net` 状态 + 自动 `/tip` 探活；断线检测、连锁上下文 journal（重连后恢复）、`/leave` |
| **排位房流程** | 选卡组（`/deck?name=` 真正选中 `cbDeckSelect`）、准备（点 `btnHostPrepReady`）、开始、房间错误抑制 |
| **弹窗治理** | 主菜单残留、`MSG_HINT` 提示框、`MSG_CONFIRM_CARDS` 查看卡面板等阻塞窗口自动处理 |
| **结果判读** | `/state.isWin / isLoss / isDraw / lpOrder`（raw `winPlayer` 是座位号，别直接当"我赢"） |

配套的 MCP 桥接层（MCP server、策略、supervisor、工具脚本）在独立仓库：
**<https://github.com/darknessWONG/ygopro-mcp>**（MIT，零依赖 Node）。

```
LLM / agent（MCP 宿主）
      │  MCP over stdio
      ▼
ygopro-mcp（独立仓库）                薄桥：14 个 MCP 工具 → HTTP
      │  HTTP 127.0.0.1:7788 / 7789
      ▼
本仓库：YGOPro.exe 进程内控制层       协议编解码 / 动作队列 / UI 注入 / 快照 / journal
      │  CTOS_RESPONSE / STOC_*；同时向真实窗口注入事件
      ▼
ygopro 内核（ocgcore）+ 真实客户端窗口
```

---

## 手动游玩的限制（重要）

**目前这个构建是"给 AI 用"的，纯手动玩会与上游原版不一致，且部分流程人无法接管。**

| # | 现象 | 根因 | 能否手动绕过 |
|---|---|---|---|
| 1 | **猜拳不会弹给你选**，直接被随机出 | `STOC_SELECT_HAND` → `Control::AutoHandResult()` 无条件自动发送 | ❌ 当前**没有开关**（这是待补的 `YGO_AUTOPLAY`/手动模式改造） |
| 2 | **一旦控制层被任何 HTTP 请求碰过（例如 bridge 探活/接 MCP），先/后攻会转给 LLM**，人不管就会停在那里 | `g_controlDriven` 为真时 TP 不自动发，等 `/act_id` | ⚠️ 只能不接 MCP，或设 `YGO_TP=1\|2` 强制 |
| 3 | **断线/卡组错误等原生弹窗不弹**，只在 `control-<pid>.log` 里记 | `YGO_SUPPRESS_MODALS` 默认 1 | ✅ `YGO_SUPPRESS_MODALS=0` |
| 4 | **空闲时自动向对手发 `/tip` 聊天** | `YGO_NET_PROBE` 默认 1 | ✅ `YGO_NET_PROBE=0` |
| 5 | **连锁时点被强制成"可用时点"** | `YGO_CHAIN_MODE=avail` 默认，开局应用 | ✅ `YGO_CHAIN_MODE=all\|ignore` |
| 6 | `MSG_RETRY` 等提示框被自动关闭 | `YGO_AUTO_DISMISS` 默认 1 | ✅ `YGO_AUTO_DISMISS=0` |
| 7 | 目录里多出 `resume-context-<port>.json` / `control-<pid>.log` | journal 与日志 | ✅ `YGO_RESUME_JOURNAL=0` / 删文件 |

> 结论：**当前推荐用法 = 配合 [ygopro-mcp](https://github.com/darknessWONG/ygopro-mcp) 由 LLM/agent 驱动**。
> "纯手动 = 上游原版体验"的目标需要再做一个 `YGO_AUTOPLAY=0` 手动模式（默认关闭所有自动化），尚未实现。

---

## 构建

依赖与上游一致：Windows + premake5 + Visual Studio；`premake/`、`irrlicht/` 等子模块已在仓库内。

```powershell
# 1) 生成 VS2022 工程
.\premake5.exe vs2022

# 2) 编译 x64 Release
cd build
msbuild YGOPro.vcxproj /p:Configuration=Release /p:Platform=x64 /m
# 产物：bin/release/x64/YGOPro.exe
```

> `gframe/premake5.lua` 用 `files { "*.cpp", "*.h" }` 自动纳入新增的 `control*.cpp`，但只在 **premake 生成工程时**求值 —— 改了源码后要重新跑 premake，**直接 msbuild 旧工程不会编译新文件**。

已验证构建：`YGOPro.exe` **2026-11-08 03:38:41 / 6,290,944 B**（用配套仓库的 `node doctor.mjs --client <exe>` 可核对指纹）。

---

## 运行

```powershell
# 房主：监听 7911，控制端口 7788
$env:YGO_CONTROL_PORT='7788'
.\bin\release\x64\YGOPro.exe -c -h 127.0.0.1 -p 7911 -n HostA -d Blackwing -k

# 客机：控制端口 7789
$env:YGO_CONTROL_PORT='7789'
.\bin\release\x64\YGOPro.exe -j -h 127.0.0.1 -p 7911 -n GuestB -d Blackwing -k
```

确认控制层已启动：

```powershell
curl.exe http://127.0.0.1:7788/health      # {"ok":true,"started":true}
curl.exe http://127.0.0.1:7788/state       # 完整局面
```

接 MCP（详见 ygopro-mcp 仓库）：

```json
{ "command": "node", "args": ["<path>/ygopro-mcp/mcp-server.mjs"],
  "env": { "CONTROL_URL": "http://127.0.0.1:7788" } }
```

---

## HTTP 接口概要

| 类别 | 端点 |
|---|---|
| 状态 | `/health` `/state` `/prompt` `/wait_prompt?since=` `/net` `/ui` `/lobby` `/stoc` `/chat` |
| 决策 | `/actions` `/act_id?id=N` `/act` `/msg` |
| 房间 | `/join?ip=&port=&pass=` `/deck?name=` `/ready` `/start` `/leave` `/surrender` |
| 排障 | `/net/probe` `/uiclick` `/ctos` `/hs` |

每个端点的完整字段、动作编码和流程示例见 [`docs/CONTROL-API.md`](docs/CONTROL-API.md)；面向 LLM 的 MCP 接口规范在配套仓库 [ygopro-mcp](https://github.com/darknessWONG/ygopro-mcp) 的 `docs/MCP-INTERFACE.md`。

---

## 文件与目录

| 路径 | 内容 |
|---|---|
| `gframe/control*.cpp` / `control*.h` | 控制层本体（8 个文件，约 4,950 行） |
| `gframe/duelclient.cpp` 等 6 个上游文件 | 只加了入口钩子（约 60 行） |
| `docs/CONTROL-API.md` | **HTTP 层完整 API 文档**（每个端点/字段/动作编码/完整流程示例） |
| `AI-CONTROL.md` | 控制层概览（架构、能力、环境变量、状态） |
| `UPSTREAM.md` | 上游基线、改动清单、如何与上游同步 |

| `README.upstream.md` | 上游 ygopro 的原始 README（保留备查） |

---

## 状态

- ✅ 支持：局域网对局、MyCard 排位（排队→进房→选卡组→准备→开局→结算）、先/后攻选择、连锁、陷阱/手坑、掉线检测与重连、完整 UI 语义动作
- ✅ 已验证：真实排位对局可自然结算；受控掉线 13 秒重连回同一局；TP 由 agent 选择后正常开局
- ❌ 未做：手动模式（`YGO_AUTOPLAY`，见上方限制表）、策略强度（属上层策略问题）

## 许可

GPLv2 派生作品，整体按 GPLv2 发布（`LICENSE`）。分发编译产物时须同时提供完整对应源码。
**不包含**卡牌图片、`cards.cdb`、卡组、录像等资源；与 Konami / MyCard 官方无隶属关系。
