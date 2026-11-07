# AI 控制层（AI Control Layer）

> 给 [mycard/ygopro](https://github.com/mycard/ygopro) 客户端加的一层**进程内 HTTP 控制接口**，让 LLM / agent（通过 MCP）可以像人一样打牌：**不拼二进制帧、不做屏幕像素点击**，而是调用语义化的 HTTP/JSON 接口，由客户端自己把动作编码成协议响应并尽量走真实 UI 注入。

本仓库是 `mycard/ygopro` 的 **fork**（GPLv2，未做任何协议/规则改动），改动只集中在新增控制层与少量接入钩子。

---

## 它解决什么问题

原版 ygopro 客户端是给人类玩的：所有决策靠鼠标点窗口，协议是 CTOS/STOC 二进制帧。要让 LLM 玩，必须有人把它翻译成"语义动作"，并处理弹窗、连锁、时点、重连等细节。

本控制层就是这个翻译层，位于客户端进程内：

```
LLM / agent（MCP 宿主）
      │  MCP over stdio
      ▼
 ygopro-mcp（独立仓库，MIT）        桥接层：MCP 工具 → HTTP
      │  HTTP 127.0.0.1:7788（房主）/ 7789（客机）
      ▼
 本仓库：YGOPro.exe 进程内控制层      协议编解码 / 动作队列 / UI 注入 / 状态快照
      │  CTOS_RESPONSE / STOC_*；同时向真实窗口注入鼠标/按钮事件
      ▼
 ygopro 内核（ocgcore）+ 真实客户端窗口
```

**为什么用 HTTP 而不是 CLI 或管道**：客户端是 GUI 程序、需要持续双向交互；HTTP+JSON 跨语言、可用 `curl` 排障、支持同机双客户端（7788/7789），且桥接层无需链接客户端代码。

---

## HTTP 接口一览

控制层监听 `127.0.0.1:<YGO_CONTROL_PORT>`（默认 7788）。

| 类别 | 端点 |
|---|---|
| 状态 | `GET /health`、`GET /state`、`GET /prompt`、`GET /wait_prompt?since=<seq>`、`GET /net`、`GET /ui`、`GET /lobby`、`GET /stoc`、`GET /chat` |
| 决策 | `GET /actions`（当前语义动作列表）、`GET /act_id?id=N`、`GET /act?...`、`GET /msg` |
| 房间 | `GET /join?ip=&port=&pass=`、`GET /deck?name=`、`GET /ready`、`GET /start`、`GET /leave`、`GET /surrender` |
| 排障 | `GET/POST /net/probe`（主动探活）、`GET /uiclick?...`、`GET /ctos`、`GET /hs` |

语义动作（`/actions` 的 `kind`）覆盖：召唤/特召/盖放/发动/攻击/战阶切换/位置/放卡/选卡/选连锁/是否/选项/结束阶段等。LLM 只需要「读状态 → 选动作 id」，编码与 UI 执行都在客户端内完成。

详细字段与行为见配套仓库 **ygopro-mcp** 的 `docs/MCP-INTERFACE.md`。

---

## 新增/修改的文件

**新增（8 个，约 4,950 行）**

| 文件 | 职责 |
|---|---|
| `gframe/control.cpp` | 生命周期、HTTP 服务、请求路由、动作队列、`Pump()` 执行、钩子入口 |
| `gframe/control.h` | 上游文件唯一可见的公共接口（`Control::*` 静态入口） |
| `gframe/control_internal.h` | 内部共享类型（快照/动作/状态）与全局声明 |
| `gframe/control_state.cpp` | 局面快照采集 + JSON 序列化（含 prompt/net/resume） |
| `gframe/control_ui.cpp` | UI 注入（按钮/鼠标事件）、弹窗清理、连锁时点、窗口诊断 |
| `gframe/control_sem.cpp` | 把 prompt 翻译成语义动作列表 |
| `gframe/control_actions.cpp` | 动作分发与原始响应编码 |
| `gframe/control_lobby.cpp` | 卡组/准备/开始/进房/离开/聊天 |

**修改的上游文件（6 个，约 60 行钩子）**

`gframe/duelclient.cpp`、`duelclient.h`、`gframe/game.cpp`、`gframe/netserver.cpp`、`gframe/single_duel.cpp`、`premake5.lua`。

上游文件里只加了**入口调用**（收包/分析结束/连接建立/准备/聊天等时点调用 `Control::*`），没有改任何游戏逻辑。

---

## 构建

依赖与上游一致（Windows + premake5 + Visual Studio；`premake/`、`irrlicht/` 等子模块在仓库内）。

```powershell
# 1) 生成工程（VS2022）
cd <repo>
.\premake5.exe vs2022

# 2) 编译 x64 Release
cd build
msbuild YGOPro.vcxproj /p:Configuration=Release /p:Platform=x64 /m

# 产物：bin/release/x64/YGOPro.exe
```

> 本环境验证过：`gframe/premake5.lua` 用 `files { "*.cpp", "*.h" }` 自动纳入新增的 `control*.cpp`；**必须用 premake 重新生成工程**（直接 msbuild 旧工程不会编译新文件）。

---

## 运行

```powershell
# 房主（监听 7911，控制端口 7788）
$env:YGO_CONTROL_PORT='7788'
.\bin\release\x64\YGOPro.exe -c -h 127.0.0.1 -p 7911 -n HostA -d Blackwing -k

# 客机（控制端口 7789）
$env:YGO_CONTROL_PORT='7789'
.\bin\release\x64\YGOPro.exe -j -h 127.0.0.1 -p 7911 -n GuestB -d Blackwing -k
```

验证控制层已启动：

```powershell
curl.exe http://127.0.0.1:7788/health
# {"ok":true,"started":true}
```

然后用配套的 **ygopro-mcp**（MIT）接上 MCP：

```json
{ "command": "node", "args": ["<path>/ygopro-mcp/mcp-server.mjs"],
  "env": { "CONTROL_URL": "http://127.0.0.1:7788" } }
```

---

## 环境变量

| 变量 | 默认 | 作用 |
|---|---|---|
| `YGO_CONTROL_PORT` | 7788 | 控制层监听端口（客机常用 7789） |
| `YGO_HOOKS` | — | 调试用钩子开关（`msganalyze` / `all`） |
| `YGO_UI_ACTIONS` | 1 | 动作优先走 UI 注入；0 = 全部直连协议（对照用） |
| `YGO_SUPPRESS_MODALS` | 1 | 断线/错误类原生弹框只记日志不阻塞 |
| `YGO_NET_PROBE` | 1 | `/tip` 主动探活（掉线检测） |
| `YGO_NET_IDLE_MS` / `YGO_NET_PROBE_GAP_MS` / `YGO_NET_PROBE_WINDOW_MS` / `YGO_NET_DEAD_SILENCE_MS` | 2500 / 3000 / 1500 / 10000 | 探活节奏 |
| `YGO_RESUME_JOURNAL` / `YGO_RESUME_PATH` | 1 / `resume-context-<port>.json` | 上下文 journal（重连后恢复连锁上下文） |
| `YGO_CHAIN_MODE` | `avail` | 连锁时点策略（`all` / `avail` / `ignore`） |

---

## 状态

- 支持：局域网对局、MyCard 排位对局（排队→进房→选卡组→准备→开局→结算）、掉线检测与重连、连锁语义、陷阱/手坑激活、完整 UI 语义动作
- 已验证：完整排位对局可自然结算；受控掉线 13 秒重连回同一局
- 未做：策略强度（属上层策略问题）、部分信息类动作仍走直连协议（见 ygopro-mcp 的 `docs/direct-actions.md`）

---

## 许可

本仓库是 GPLv2 的派生作品，**整体按 GPLv2 发布**（见 `LICENSE`）。分发编译后的可执行文件时，必须同时提供完整对应源码。

**本仓库不包含**：卡牌图片、`cards.cdb`、卡组、对局录像等资源（这些不属于上游开源部分，也请勿自行打包分发）。依赖库许可见 `THIRD-PARTY.md`，上游信息见 `UPSTREAM.md`。
