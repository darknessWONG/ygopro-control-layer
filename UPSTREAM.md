# 上游信息与改动清单（UPSTREAM）

## 上游

| 项 | 值 |
|---|---|
| 上游仓库 | https://github.com/mycard/ygopro |
| 基线 commit | `0d76e26b30c5ddb0273ee746459506c5719fddb6`（"remove system.conf"） |
| 许可 | GPLv2（见 `LICENSE`，未做修改） |
| 本仓库角色 | fork，**只新增控制层与接入钩子**，未修改游戏规则/协议/资源 |

> 上游另有原始项目（Fluorohydride/ygopro、ProjectIgnis/edopro 等），本 fork 基于 MyCard 的客户端分支。README.upstream.md 保留了上游原始 README。

## 新增文件（控制层）

```
gframe/control.cpp          生命周期 / HTTP 服务 / 路由 / 动作队列 / Pump / 钩子入口
gframe/control.h            上游文件唯一可见的公共接口
gframe/control_internal.h   内部共享类型与全局声明
gframe/control_state.cpp    快照采集 + JSON 序列化
gframe/control_ui.cpp       UI 注入 / 弹窗清理 / 连锁时点 / 窗口诊断
gframe/control_sem.cpp      prompt → 语义动作列表
gframe/control_actions.cpp  动作分发与协议响应编码
gframe/control_lobby.cpp    卡组 / 准备 / 开始 / 进房 / 离开 / 聊天
```

## 修改的上游文件（只加钩子，不改逻辑）

| 文件 | 改动 |
|---|---|
| `gframe/duelclient.cpp` | 收包/分析结束时调用 `Control::OnStocPacket / OnMessageBegin / OnAnalyzeEnd`；聊天钩子 `Control::OnChat`；断线/连接/准备/卡组错误路径接入 `Control::NotifyUser / OnConnEvent / OnDeckError` |
| `gframe/duelclient.h` | 声明邮件钩子入口（含 `SendBufferToServer` 的 CTOS 计数通知） |
| `gframe/game.cpp` | 启动时 `Control::EnsureStarted()`；主循环里 `Control::Pump()` |
| `gframe/netserver.cpp` | 服务端收包时调用 `Control::OnCtosPacket`（用于 UI 动作校验） |
| `gframe/single_duel.cpp` | 单人/服务端流程接入（开始、准备、断线） |
| `premake5.lua` | 本机构建配置（可通过环境变量恢复上游默认值） |

> 直接 `msbuild` 旧工程不会编译新增的 `control*.cpp`——`gframe/premake5.lua` 的 `files { "*.cpp", "*.h" }` 只在 **premake 生成工程时**求值，因此改动后需要重新生成工程（见 `AI-CONTROL.md` 的构建步骤）。

## 与上游同步

```bash
git remote add upstream https://github.com/mycard/ygopro.git
git fetch upstream
git checkout feat/mcp-control
git rebase upstream/master      # 或 merge
```

控制层代码与上游共享文件的重叠极小（只有 6 个文件的 60 行钩子），冲突通常集中在 `duelclient.cpp` 的消息分派处。

## 未包含的内容

本仓库**不含**（也请不要提交）：`pics/`、`cards.cdb`、`deck/`、`replay/`、`expansions/`、`fonts/`、构建产物 `bin/ obj/ build/`、任何 `.exe`、运行时日志与 `resume-context-*.json`。这些由使用者自备，或属于非开源资源。
