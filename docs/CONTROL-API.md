# 控制层 HTTP API（CONTROL-API）

> 面向"直接调 HTTP 的人/程序"：说明客户端进程内控制层暴露的每个端点、字段、动作编码与完整流程。
> 不依赖 MCP。想用 MCP（agent 工具）请看 `../AI-CONTROL.md` 与配套 bridge 仓库的 `docs/MCP-INTERFACE.md`；
> 本文件是 HTTP 层权威用法。

---

## 1. 总览

- 控制层**编译在客户端进程内**（`YGOPro.exe`），随游戏启动而启动，监听 **`127.0.0.1`** 上的
  `YGO_CONTROL_PORT`（默认 **7788**；本机双开时房主 7788、客机 7789）。
- 只绑定 loopback，**不对外网开放**；每个请求一个短连接，返回 JSON（`Content-Type: application/json; charset=utf-8`）。
- 请求方式：**按 path 路由，不区分 GET/POST**（两方法都接受）。读操作建议 `GET`，动作/写操作建议 `POST`。
- 未知路径返回 `HTTP 404` + `{"ok":false,"error":"not found"}`。

本机自检（Windows 自带 `curl.exe`）：

```bat
curl.exe -s http://127.0.0.1:7788/health
curl.exe -s http://127.0.0.1:7789/health
```

启动示例（两个客户端各开一个控制口）：

```bat
:: 房主：7911 开房，控制口 7788
set YGO_CONTROL_PORT=7788
YGOPro.exe -c -p 7911

:: 客机：控制口 7789，加入 127.0.0.1:7911
set YGO_CONTROL_PORT=7789
YGOPro.exe
:: 然后 POST /join?ip=127.0.0.1&port=7911&pass=<密码>
```

> 注意：命令行 `-j` 会忽略 `-p` 直连 7911，自动化请用 `POST /join` 而不是 `-j`。

---

## 2. 通用约定

### 2.1 返回格式

- 绝大多数端点返回**扁平 JSON 对象**；动作类返回 `{"ok":true,"queued":true}`。
- 错误统一为 `{"ok":false,"error":"<原因>"}`，常见 `error`：
  `not awaiting`（当前没有待回答 prompt）、`stale seq`（带的 seq 过期）、`bad action id`、
  `need hex or i`（`/act` 参数缺失）、`not found`。

### 2.2 `seq` / `awaiting` / stale

- 控制层每发布一个 prompt 就 `seq++`；`awaiting:true` 表示**有一个 prompt 正等着你回答**。
- 回答方式只有两种：`/act_id?id=<动作 id>`（推荐）或 `/act?seq=&i=|hex=`（底层）。
- 回答后 `awaiting=false`；**做动作时必须带对 `seq`**（`/act` 会校验，`/act_id` 自动用当前 seq）。
- 等待新 prompt 用 `/wait_prompt?since=<上次的 seq>`：返回时 `seq>since` 或 `awaiting=false`。

### 2.3 prompt 快照

`awaiting:true` 期间，`/prompt`、`/actions`、`/wait_prompt` 返回的是**该 prompt 发布瞬间冻结的快照**
（不是"最新到达的消息"），保证 `curMsg`/候选列表与你要回答的问题一致。

---

## 3. 端点

### 3.1 `GET /health`

| 项 | 内容 |
|---|---|
| query | 无 |
| 返回 | `{"ok":true,"started":true}` |
| 说明 | `started` 表示**控制层 HTTP 服务**在监听（不是"对局已开始"）；对局状态看 `/state.started`、`/state.inDuel` |

```bat
curl.exe -s http://127.0.0.1:7788/health
```

### 3.2 `GET /state`

完整局面快照（字段很多，下面按组说明）。`/state.prompt` 就是 `/prompt` 的返回。

| 组 | 字段（真实输出） |
|---|---|
| 标识 | `valid` `inDuel` `started` `turn` `seq` `awaiting` `curMsg` |
| 阵营/座位 | `myPlayer`（本地视角恒 0）、`mySeat`（实际座位 0/1）、`turnPlayer`（raw 座位）、`turnPlayerLocal`（本地视角）、`isMyTurn`、`selftype`、`playerType` |
| 生命/阶段 | `lp:[我方,对手]` + `lpOrder:"local"`、`phase`（数字）、`phaseName`（如 `MAIN1`）、`timeLeft` |
| 胜负 | `winPlayer`（raw）、`winPlayerLocal`（0=我方赢/1=对手赢/-1=平局或未知）、`isWin` `isLoss` `isDraw`、`winReason` `winReasonName` |
| 整场（Bo3） | `match:{game,over,games:[{game,win,winnerSeat,reason,reasonName}],gamesWonLocal,gamesWonOpp,lastWinnerLocal,lastReasonName}`：`game`=当前第几局（`STOC_DUEL_START` 计数），`over`=`STOC_DUEL_END` 已收到；`win` 为我方视角，`winnerSeat` 为 raw 座位（0/1，调试用）；无结果时 `lastWinnerLocal/lastReasonName` 为 `null` |
| 计数 | `counts:{hand:[我,对],grave:[...],remove:[...],deck:[...],extra:[...]}` |
| 手牌/场面 | `hand`、`monsters`、`spells`、`grave`、`banished`：数组 `[我方, 对手]`；`extraDeck` 单数组；每张卡 `{code,atk,def,pos,faceup,hidden,ty,lv,name,loc,seq}`（`hidden=true` 时 `code/lv/name` 置空） |
| 连锁 | `chainDepth` `chainStack` `chainingTo` `chainCandidates` `canDecline` `hint` `lastChainEvent`；另有可读的 `activeEffects`（链栈文本）、`continuousEffects`（永续效果卡） |
| UI | `chainMode` `uiCleanupCount` `ui:{chainMode,chainModeName,alwaysChain,chainWhenAvail,ignoreChain,desiredChainMode,desiredChainModeName,appliedChainMode,applyPending,uiCleanupCount,popupMask,popups:{posSelect,options,cardSelect,cmdMenu,message},windows:[...],windowsVisible:[...]}` |
| 管线 | `pipeline:{analyzeCount,pumpCount,msgCount,seq,awaiting,lastStoc,lastStocName,sinceLastAnalyzeMs,sinceLastPumpMs,retryCount,lastRetryAfterMsg,lastNonRetryMsg,retryStreak,placeVariant,autoDismiss,dismissCount,recentMsgs}` + 顶层别名 `retryCount` `retryStreak` `placeVariant` `lastMsg:{id,len,hex}` |
| 提示 | `prompt`（同 `/prompt`） |
| 网络/重连 | `net`（同 `/net`）、`resume`（journal 恢复信息或 `null`）、`deckError`（见 §3.4） |
| 换备（Bo3） | `side`：`available/pending/waiting/main/extra/side/counts/pre/limits/error/note`（见 §3.5.1.1） |
| 其它 | `deckMain` `deckExtra` `errorMsg` `errorCode`（最近一条 `STOC_ERROR_MSG` 的 msg/code） |

```bat
curl.exe -s http://127.0.0.1:7788/state
:: 只看部分字段（Windows 没有 jq 时可用 findstr 粗筛）
curl.exe -s http://127.0.0.1:7788/state | findstr /C:"\"turn\":" /C:"\"lp\":" /C:"\"deckError\""
```

裁剪示例（真实字段、省略部分数组）：

```json
{"valid":true,"inDuel":true,"started":true,"turn":3,"lp":[8000,6500],"lpOrder":"local",
 "phase":4,"phaseName":"MAIN1","curMsg":11,"myPlayer":0,"mySeat":0,"turnPlayer":0,
 "turnPlayerLocal":0,"isMyTurn":true,"winPlayer":-1,"winPlayerLocal":-1,
 "isWin":false,"isLoss":false,"isDraw":false,"winReason":-1,"winReasonName":"UNKNOWN",
 "match":{"game":2,"over":false,
          "games":[{"game":1,"win":true,"winnerSeat":0,"reason":1,"reasonName":"LP_ZERO"},
                   {"game":2,"win":true,"winnerSeat":0,"reason":4,"reasonName":"DISCONNECT"}],
          "gamesWonLocal":2,"gamesWonOpp":0,
          "lastWinnerLocal":true,"lastReasonName":"DISCONNECT"},
 "chainMode":1,"seq":5,"awaiting":true,
 "counts":{"hand":[5,5],"grave":[0,1],"remove":[0,0],"deck":[35,34],"extra":[15,15]},
 "chainDepth":0,"chainStack":[],"chainingTo":null,"chainCandidates":[],"canDecline":true,
 "pipeline":{"analyzeCount":42,"pumpCount":900,"msgCount":40,"seq":5,"awaiting":true,
             "retryCount":0,"retryStreak":0,"recentMsgs":[40,41,11]},
 "net":{"connected":true,"alive":true,"state":"healthy"},
 "resume":null,
 "deckError":{"local":{"set":false,"reason":"","type":-1,"code":0},
              "server":{"set":false,"type":-1,"code":0,"text":""}}}
```

### 3.3 `GET /prompt` 与 `GET /wait_prompt?since=&timeout_ms=`

`/prompt` 返回当前 prompt（字段与 `/state.prompt` 相同）；`/wait_prompt` 长轮询到下一个 prompt。

主要字段：

| 字段 | 说明 |
|---|---|
| `seq` `curMsg` `turn` `awaiting` | 当前 prompt 的序号/消息类型/回合/是否待答 |
| `pendingKind` `realCurMsg` | **仅当 prompt 由"仍在等待的 UI 查询"合成时出现**：`realCurMsg` 是被后续消息覆盖的真实 `dInfo.curMsg`（通常 3=WAITING），`pendingKind` 是实际等待的查询类型（13=YESNO/EFFECTYN、15=SELECT_CARD、14=OPTION、19=POSITION）；正常 prompt 无这两个字段 |
| `tpPending` `tpOptions` `tpChooseBy` | **仅先/后攻选择时出现**（见 §3.5）；`tpOptions:[1,2]`，1=先攻、2=后攻 |
| `summonable` `spsummonable` `msetable` `ssetable` `reposable` `activatable` `attackable` | 各类可动作数量 |
| `summonableCards` `spsummonableCards` `msetableCards` `ssetableCards` `activatableCards` `attackableCards` `selectableCards` `chains` | 候选卡数组，元素 `{i,code,atk,def,loc,seq,seqNo,op,lv,rk,ty,desc,ctrl,forced}` |
| `chains` / `chainDepth` / `chainStack` / `chainingTo` / `chainCandidates` / `canDecline` / `hint` / `lastChainEvent` | 连锁相关（`chainCandidates` 为空表示"只能不连锁"） |
| `selectMin` `selectMax` `selectCancelable` `selectableCount` | 选卡类约束 |
| `canBp` `canEp` | 是否可进战阶/结束阶段 |
| `places` `placesMask` | 放卡候选 `{i,ctrl,loc,seq,bit}` |
| `sumTarget` `sumMode` `options` | 选素材/选项 |
| `isFirst` `placeCoords` | 先/后手与放卡坐标约定（`server`/`local`） |

```bat
curl.exe -s "http://127.0.0.1:7788/prompt"
curl.exe -s "http://127.0.0.1:7788/wait_prompt?since=12&timeout_ms=30000"
```

### 3.4 `GET /actions` 与 `GET /act_id?id=N`

`/actions` 返回**当前 prompt 的语义候选**，LLM/脚本只选 `id`：

```json
{"seq":42,"curMsg":11,"awaiting":true,"actions":[
  {"id":0,"kind":"summon","label":"summon #0 code=22835145 lv=2 tuner"},
  {"id":1,"kind":"set_monster","label":"set monster #0 code=22835145"},
  {"id":2,"kind":"activate","label":"activate #0 code=5318639"},
  {"id":3,"kind":"to_bp","label":"go to battle phase"},
  {"id":4,"kind":"to_ep","label":"end phase"}
]}
```

- `awaiting:false` 时返回 `{"actions":[],"awaiting":false,...,"reason":"not awaiting"}`；
- `curMsg/seq` 与 `/prompt` 一致（prompt 快照）。

`/act_id?id=<id>`：提交上表中的一个动作；成功 `{"ok":true,"queued":true}`。
`id` 无效 → `{"ok":false,"error":"bad action id"}`；无 prompt → `not awaiting`。

```bat
curl.exe -s -X POST "http://127.0.0.1:7788/act_id?id=2"
```

### 3.5 先/后攻（`kind:"tp"`）

由 MCP/HTTP 驱动时（**控制层收到过任意 HTTP 请求**），客户端不再自动发先/后攻，而是：

`/prompt` 出现：

```json
{"seq":1,"curMsg":0,"turn":0,"awaiting":true,
 "tpPending":true,"tpOptions":[1,2],"tpChooseBy":"act_id kind=tp index=1|2"}
```

`/actions` 返回：

```json
{"actions":[
  {"id":0,"kind":"tp","label":"tp first (index=1)","index":1},
  {"id":1,"kind":"tp","label":"tp second (index=2)","index":2}],
 "awaiting":true,"tpPending":true}
```

选择：`/act_id?id=0` → 先攻（`res=1`）；`/act_id?id=1` → 后攻（`res=2`）。
日志：`TP choice res=N (from control)`。

- 纯手动（从未有 HTTP 请求）：保持旧行为自动选后攻（`res=2`），不出现 tp prompt；
- `YGO_TP=1|2` 强制覆盖（`TP choice res=N (forced by YGO_TP)`）。

### 3.5.1 换备牌（Bo3，`kind:"side_ok"`）

Bo3 的 `Game1 → Game2` 之间服务端会发 `STOC_CHANGE_SIDE`（进 siding 界面，显示 `btnSideOK` 等）。
控制层被 HTTP 驱动时：

- `/state.sidePending:true`；`/prompt` 出现 `sidePending:true`（`seq++`，与普通 prompt 一样冻结）；
- `/actions` 返回一条：
  ```json
  {"actions":[{"id":0,"kind":"side_ok","label":"keep side deck (no changes) and continue"}],
   "awaiting":true,"sidePending":true}
  ```
- `/act_id?id=0` → 在 UI 线程点击真实 `btnSideOK`（客户端自己的 `BUTTON_SIDE_OK` handler：校验
  main/extra/side 数量不变 → `SendUpdateDeck`），日志 `SIDE ok clicked (from control)`；
  随后服务端 `STOC_WAITING_SIDE`（等对手）会清掉 pending，下一局重新 `/ready` 即可。
- 纯手动（从未有 HTTP 请求）不发布该 prompt，原 siding UI 行为不变。
- **重要**：每局结束服务端可能发 `STOC_REPLAY`（回放），若"自动保存回放"未勾选，原版会弹保存框并把
  网络线程阻塞在 `replaySignal.Wait()`，导致其后的 `STOC_CHANGE_SIDE` 收不到。**控制驱动模式下控制层直接
  跳过该对话框、不阻塞网络线程**（日志 `replay: dialog skipped (control-driven, no wait)`；纯手动仍弹框）；
  另有兜底 `replay: signalled after skip-save` / `replay: retry skip (analyze stalled)`。

#### 3.5.1.1 `/state.side`（换备信息）

`STOC_CHANGE_SIDE` 期间 `/state.side` 给出当前正在编辑的卡组（数据源 `deckManager.current_deck`）：

```json
"side": {
  "available": true,
  "pending": true,
  "waiting": false,
  "waitingSeen": false,
  "lastWaitMs": 0,
  "elapsedMs": 0,
  "main":  [{"code":14558127,"name":"灰流丽","count":2}],
  "extra": [{"code":52687916,"name":"...","count":1}],
  "side":  [{"code":23434538,"name":"增殖的G","count":1}],
  "counts": {"main":40,"extra":15,"side":15},
  "pre":    {"main":40,"extra":15,"side":15},
  "limits": {"mainMin":40,"mainMax":60,"extraMax":15,"sideMax":15},
  "error": null,
  "note": "swap must keep main/extra/side counts unchanged (client validates on btnSideOK)"
}
```

- `main/extra/side` 是**按卡号聚合**的列表（`code`+`name`+`count`），不是逐张顺序；
- `counts` 是当前各区张数，`pre` 是 `STOC_CHANGE_SIDE` 到达时的张数；两者不一致时 `error` 为
  `"counts must match"`（此时 `side_ok` 会被拒绝，见下）；
- `waiting` 是**瞬时值**：自己已确认、还没收到下一局 `DUEL_START` 时为 true；LAN 内置服务端可能只持续
  十几毫秒（双方都快时），所以**断言优先看 sticky 字段**：
  `waitingSeen`（本次换备是否进入过等待）、`lastWaitMs`（上次等待时长，下一局开始时写入，
  保留到下一次 `STOC_CHANGE_SIDE`）、`elapsedMs`（正在等待时的已等待毫秒，`waiting=true` 时递增）；
- `waiting:true` 期间日志 `SIDE waiting for opponent (elapsed=..ms)` 每 10s 打一次，**不会退出**；
  对手掉线用 `/net`（`alive`/`state`/`sinceLastStoc`）判断，不要靠换备状态猜。

#### 3.5.1.2 `/side_swap`（换备操作）

`GET|POST /side_swap?code=<card>&from=main|extra|side&to=main|extra|side[&n=1]`

- 把 `code` 的 `n` 张从 `from` 区移到 `to` 区（只改 `deckManager.current_deck`，UI 线程执行并刷新视图）；
- 校验：`from` 区必须真有这么多张；`from != to`；区名合法；不合法返回
  `{"ok":false,"error":"..."}`（如 `not enough copies in side (have 1, need 2)`）；
- 返回当前（预测）张数：`{"ok":true,"queued":true,"main":40,"extra":15,"side":15}`；
- 日志：`SIDE swap code=.. from=.. to=.. n=.. -> counts m/e/s=..`。

```bat
curl.exe -s -X POST "http://127.0.0.1:7788/side_swap?code=23434538&from=side&to=main&n=1"
curl.exe -s -X POST "http://127.0.0.1:7788/side_swap?code=23434538&from=main&to=side&n=1"
curl.exe -s http://127.0.0.1:7788/state          :: 看 state.side.counts 是否变化
```

> 换备必须保持 **main/extra/side 张数不变**（main 40 张、extra 15、side 15 起步；只允许在区之间交换）。
> 若张数变了，`/actions.sideError` / `/state.side.error` 会显示 `"counts must match"`，`/act_id?id=0`
> 返回 `{"ok":false,"error":"counts must match",...}` 并记 `SIDE ok rejected reason=counts must match`。
> 点击 `btnSideOK` 之前客户端会在 UI 线程**再校验一次**（exec-time），不一致则不点击并记
> `SIDE ok rejected reason=counts must match (exec-time)`；刚调用 `/side_swap` 还没执行完时
> `side_ok` 会先返回 `{"ok":false,"error":"side swap pending, retry"}`，稍后重试即可。

### 3.6 `GET /act`（底层原始响应）

直接发协议响应，用于排障/特殊场景；正常请用 `/act_id`。

| 参数 | 说明 |
|---|---|
| `seq` | 当前 prompt 的 seq；`0` 表示跳过校验（不推荐） |
| `i` | 4 字节有符号整数响应（如 `-1` 表示放弃连锁） |
| `hex` | 原始字节（如 `00040102`），与 `i` 二选一 |

```bat
:: 连锁放弃（等价于 /act_id 里的 decline）
curl.exe -s -X POST "http://127.0.0.1:7788/act?seq=42&i=-1"
```

错误：`{"ok":false,"error":"stale seq","seq":<当前>}` / `not awaiting` / `need hex or i`。

### 3.7 `GET /msg`

最近一条游戏消息：`{"id":<MSG_*>, "len":<字节数>, "hex":"<原始字节>"}`。排障用。

### 3.8 房间流程端点

| 端点 | 作用 | 返回/说明 |
|---|---|---|
| `GET|POST /join?ip=&port=&pass=` | 连接服务器；缺省 `ip=127.0.0.1`、`port=7911` | `{"ok":true,"queued":true}`；成功后 `/lobby`、`/net.joined` 变化 |
| `GET|POST /deck?name=<deck>` | 加载 `<游戏目录>/deck/<name>.ydk` 并**在准备界面 `cbDeckSelect` 选中该卡组**（缺省 `Blackwing`） | `{"ok":true,"queued":true}`；日志 `deck: select combo name=... -> confirmed` |
| `GET|POST /ready` | 准备：准备窗口存在时先同步卡组下拉框，再点真实 `btnHostPrepReady`（=真人流程 `LoadCurrentDeck`+`UpdateDeck`+`CTOS_HS_READY`）；**可安全重试**（每次请求都会重新尝试，点击或直发），日志 `ready: attempt #N (clicked|sent direct)` | `{"ok":true,"queued":true}` |
| `GET|POST /start` | 房主开始对局（`CTOS_HS_START`）；**可安全重试**：每次请求都重发，**只有真正收到 `STOC_DUEL_START` 后才跳过**（不再用 `dInfo.isStarted`，避免点击未生效时被 latch），日志 `start: HS_START sent #N` / `start: skipped (duel already started)` | `{"ok":true,"queued":true}` |
| `GET|POST /leave` | 离开房间回主菜单（重连/换房用） | `{"ok":true,"queued":true}` |
| `GET|POST /surrender` | 投降（`CTOS_SURRENDER`） | `{"ok":true,"queued":true}` |
| `GET|POST /hostopts?match=1` | LAN Bo3 辅助：把建房 `cbMatchMode` 选到指定模式（0=Single、1=Match、2=Tag） | `{"ok":true,"queued":true,"matchMode":1}`；日志 `hostopts: matchMode=...` |
| `GET|POST /host?match=1&port=7911` | 同上并**直接建房**（等价 `-c -p`：设 `serverport` + 点 `HOST_CONFIRM`） | `{"ok":true,"queued":true,"matchMode":1,"port":7911}` |

```bat
curl.exe -s -X POST "http://127.0.0.1:7788/deck?name=Blackwing"
curl.exe -s -X POST "http://127.0.0.1:7788/ready"
curl.exe -s -X POST "http://127.0.0.1:7788/start"
```

### 3.9 `GET /net` 与 `GET|POST /net/probe`

`/net` 返回网络/探活状态：

| 字段 | 说明 |
|---|---|
| `connected` `joined` `inDuel` `turn` | 连接/入房/对局状态 |
| `alive` `state` | 是否存活 + 状态机：`healthy` / `probe_failed` / `confirmed_dead` / `disconnected` / `unsupported` / `idle` |
| `closeReason` | `none` / `eof` / `stop` / … |
| `lastStocAt` `lastStocName` `sinceLastStoc` | 最近一条 STOC 包时间/类型/距今毫秒 |
| `probe:{enabled,unsupported,lastSentAt,lastReplyAt,rttMs,fails,lastReplyPlayerType,lastReplyText}` | `/tip` 探活细节 |
| `lastNetMsg:{id,text,ageMs}` | 最近一条服务器消息 |

`/net/probe` 主动探一次（默认自动探活已开启）：`{"ok":<有回包>,"sent":..,"rttMs":..,"alive":..,"fails":..,"lastReplyPlayerType":..}`。

> **被动判死与"对手长考"**：`probe.enabled=false`（例如排位里关掉主动 `/tip`）时，客户端靠
> `sinceLastStoc` 判死。默认阈值 `YGO_NET_DEAD_SILENCE_MS=10000`，但**当不是我的回合且没有
> awaiting/queued action 时**有效阈值自动放宽到 **60s**（对手长考/AFK 不再 10s 误报）；静默 >30s 时
> 每 15s 打日志 `NET silence diag since=.. oppTurn=.. awaiting=.. lastStoc=.. timeLeft=.. effSilence=..`，
> 判死日志 `NET DEAD reason=stoc-timeout since=.. eff=.. oppTurn=..`。真断线（我方回合或 >60s）仍会判
> `confirmed_dead`，供 supervisor 重启/重进。

```bat
curl.exe -s http://127.0.0.1:7788/net
curl.exe -s -X POST http://127.0.0.1:7788/net/probe
```

### 3.10 `GET /ui`

返回 UI 状态（连锁时点 + 弹窗 + 窗口诊断）：

```json
{"chainMode":1,"chainModeName":"avail","alwaysChain":false,"chainWhenAvail":true,
 "ignoreChain":false,"desiredChainMode":1,"appliedChainMode":1,"applyPending":false,
 "popupMask":0,"popups":{"posSelect":false,"options":false,"cardSelect":false,
 "cmdMenu":false,"message":false},
 "windowsVisible":["wChat"],
 "windows":[{"name":"wMessage","visible":false,"rect":{"l":821,"t":335,"r":1171,"b":475},
             "x":821,"y":335,"w":350,"h":140,"textLen":0}]}
```

切换连锁时点：`GET /ui?chain=all|avail|ignore`（返回同一结构，下一帧生效；`applyPending` 为 true 表示还没落地）。

### 3.11 `GET /lobby`

```json
{"self":0,"isHost":true,"launchSeat":0,"ready":[1,1],"bothReady":true,
 "deckMain":40,"statuses":[9,25]}
```

- `self` 本地座位（0/1）；`isHost`；`launchSeat` 由启动参数 `-c/-j` 推断；
- `ready:[房主,客人]`（1=已准备）；`bothReady`；`deckMain` 最近一次发送的主卡组数；
- `statuses` 原始 HS 状态字节数组（`pos<<4 | state`；state 9=准备、10=取消准备）；
  例如 `[9,25]` = 房主 ready（`0<<4|9`）、客人 ready（`1<<4|9`）。

### 3.12 `GET /stoc`、`GET /chat`、`GET /hs`、`GET /ctos`

| 端点 | 返回 | 说明 |
|---|---|---|
| `/stoc` | `{"packets":[2,4,2002,...]}` | 最近 40 条收到的 STOC 包类型号（含客户端标记值 2000+res 等），排障用 |
| `/chat` | `{"chats":["<utf8>",...]}` | 最近 ≤50 条聊天/系统消息原文 |
| `/hs` | `{"statuses":[9,25,...]}` | 原始 HS 状态字节数组（同 `/lobby.statuses`） |
| `/ctos` | `{"packets":[...]}` | 最近发出的 CTOS 包类型号 |
| `/uiclick?x=&y=` | `{"ok":true,"queued":true,"x":..,"y":..}` | 注入一次屏幕像素点击（校准/排障） |

---

## 4. 动作编码表

`/act_id` 会自动按 `kind` 编码；下表同时给出底层 `i`/字节含义（`/act` 手动调试时用）。

| prompt 类型 | kind | 编码 |
|---|---|---|
| 主阶段（`MSG_SELECT_IDLECMD`=11） | `summon` | `i = (index<<16) | 0` |
| | `spsummon` | `(index<<16) | 1` |
| | `set_monster` | `(index<<16) | 3` |
| | `set_spell` | `(index<<16) | 4` |
| | `activate` | `(index<<16) | 5` |
| | `to_bp` / `to_ep` | `(0<<16) | 6` / `(0<<16) | 7` |
| 战斗阶段（`MSG_SELECT_BATTLECMD`=10） | `attack` | `(index<<16) | 1` |
| | `to_m2` / `end_battle` | `2` / `3` |
| 连锁（`MSG_SELECT_CHAIN`=16） | `chain` | `i = index`（消息顺序） |
| | `decline` | `i = -1` |
| 是否（`MSG_SELECT_YESNO`=13 / `EFFECTYN`=12） | `yes` / `no` | `1` / `0` |
| 选项（`MSG_SELECT_OPTION`=14） | `option` | `i = index` |
| 表示形式（`MSG_SELECT_POSITION`=19） | `position` | `i & 0xff`：`1`=攻击表示，`2`=守备表示 |
| 放卡（`MSG_SELECT_PLACE`=18 / `DISFIELD`=24） | `place` | 原始字节 `[p, loc, seq]`；客户端按 `placeVariant` 轮换 4 种坐标约定（收到 `MSG_RETRY` 自动换下一种） |
| 选卡（`MSG_SELECT_CARD`=15 / `TRIBUTE`=20 / `UNSELECT_CARD`=26） | `select` | 字节 `[n, seqNo...]`（默认选前 `selectMin` 张） |
| 选素材（`MSG_SELECT_SUM`=23） | `sum` | 字节 `[n, seqNo...]`，按 `opParam` 贪心凑 `sumTarget` |
| 指示物/排序/宣言 | `counter` / `sort` / `announce` | 安全默认（多为 `[n, seqNo...]` 或 `i=0`） |
| 先/后攻（`STOC_SELECT_TP`） | `tp` | 仅 `/act_id`：`id=0`→`res=1`（先攻），`id=1`→`res=2`（后攻） |
| 换备牌（`STOC_CHANGE_SIDE`，Bo3） | `side_ok` | 仅 `/act_id`：`id=0`（保持备牌、继续）；客户端点击真实 `btnSideOK` |
| 其它未建模 prompt | `auto` | 客户端安全默认（不应依赖它做关键决策） |

> `/actions` 里的 `label` 只是给人/LLM 看的提示（`summon #0 code=…`），**决策请用 `id`**，不要解析 label。

---

## 5. 完整流程示例

### 5.1 局域网 1v1（可直接照抄）

```bat
:: 1) 起两个客户端（控制口 7788/7789）
set YGO_CONTROL_PORT=7788
start YGOPro.exe -c -p 7911
set YGO_CONTROL_PORT=7789
start YGOPro.exe

:: 2) 客机连房主
curl.exe -s -X POST "http://127.0.0.1:7789/join?ip=127.0.0.1&port=7911&pass=test"

:: 3) 双方加载卡组（会同时选中准备界面的卡组下拉框）
curl.exe -s -X POST "http://127.0.0.1:7788/deck?name=Blackwing"
curl.exe -s -X POST "http://127.0.0.1:7789/deck?name=Blackwing"

:: 4) 双方准备
curl.exe -s -X POST "http://127.0.0.1:7788/ready"
curl.exe -s -X POST "http://127.0.0.1:7789/ready"
curl.exe -s http://127.0.0.1:7788/lobby          :: 等到 "bothReady":true

:: 5) 房主开始
curl.exe -s -X POST "http://127.0.0.1:7788/start"

:: 6) 决策循环（两个客户端各自执行；下面以 7788 为例）
curl.exe -s "http://127.0.0.1:7788/wait_prompt?since=0&timeout_ms=30000"
curl.exe -s "http://127.0.0.1:7788/actions"
curl.exe -s -X POST "http://127.0.0.1:7788/act_id?id=0"
:: 把返回的 seq 记下来，下一轮 since=<该 seq>
```

### 5.2 线上排位（MC 服）

```bat
:: 1) 通过房间信息拿到 host/port/pass（例：tiramisu.moenext.com:8911）
:: 2) 进房（用锁定账号启动的客户端）
curl.exe -s -X POST "http://127.0.0.1:7788/join?ip=tiramisu.moenext.com&port=8911&pass=<pass>"

:: 3) 加载合法卡组 + 准备（必须让 cbDeckSelect 真正选中；/deck 已处理）
curl.exe -s -X POST "http://127.0.0.1:7788/deck?name=BlackwingHT2"
curl.exe -s -X POST "http://127.0.0.1:7788/ready"

:: 4) 先/后攻由这里决定（控制层收到过 HTTP 请求就不会自动选）
curl.exe -s "http://127.0.0.1:7788/prompt"
::   看到 "tpPending":true 后：
curl.exe -s "http://127.0.0.1:7788/actions"
curl.exe -s -X POST "http://127.0.0.1:7788/act_id?id=0"    :: 0=先攻；1=后攻
::   客户端日志应出现：TP choice res=1 (from control)

:: 5) 若服务端未自动开始，房主侧 /start
curl.exe -s -X POST "http://127.0.0.1:7788/start"

:: 6) 决策循环同 5.1 第 6 步
```

> 排位注意：
> - 卡组必须符合服务器 lflist（超限会在准备/开始阶段被拒）；用 `/state.deckError` 判断原因。
> - 只看 `isWin/isLoss/isDraw` 判胜负；`winPlayer` 是 raw 座位号（客机 raw 1 时 raw 0 = 对手赢）。
> - **Bo3 换备（Game1→Game2）**：Game1 结束后 `/prompt` 会出现 `sidePending:true`，`/actions` 给
>   `kind:"side_ok"`，`/act_id?id=0` 后继续（保持备牌不变）；随后下一局重新走 `/ready`。
>   Bo3 是 Match 模式，排位房间由服务器设定；若 LAN 复现可用 `/host?match=1&port=7911` 建房。

### 5.3 连锁示例

```bat
:: 对手发动效果，轮到我方决定是否连锁
curl.exe -s "http://127.0.0.1:7788/prompt"
:: 关键字段：curMsg=16、chainDepth>=1、chainCandidates=[{index,code,name,forced,...}]、canDecline
curl.exe -s "http://127.0.0.1:7788/actions"
:: 典型返回：{"id":0,"kind":"chain","label":"chain #0 code=84749824"},{"id":1,"kind":"decline","label":"do not chain"}

:: 连锁：用候选的 index 对应动作 id（/actions 已按顺序给出）
curl.exe -s -X POST "http://127.0.0.1:7788/act_id?id=0"
:: 不连锁：选 decline 动作
curl.exe -s -X POST "http://127.0.0.1:7788/act_id?id=1"
```

`chainCandidates` 为空是**正常**的：表示这条 chain prompt 没有任何可连锁项（客户端会自动放弃，不会发布该 prompt；
`/prompt` 里看到空候选时只需 decline/等待即可）。

---

## 6. 环境变量

| 变量 | 默认 | 作用 |
|---|---|---|
| `YGO_CONTROL_PORT` | `7788` | HTTP 监听端口（本机双开时 7789） |
| `YGO_HOOKS` | `all`（=7） | 钩子集合：`msg`/`analyze`/`stoc`，或 `msganalyze` 之类组合 |
| `YGO_CHAIN_MODE` | `avail` | 连锁时点：`all` / `avail` / `ignore` |
| `YGO_TP` | 未设 | 强制先/后攻：`1`=先攻、`2`=后攻（覆盖 LLM 选择与手动默认） |
| `YGO_RPS` | 随机 | 猜拳结果：`1/2/3`（石头/剪刀/布） |
| `YGO_UI_ACTIONS` | `1` | 是否用真实 UI 注入执行动作（`0`=全部直发协议） |
| `YGO_UI_EVENT_DIRECT` | `0` | `1`=直接调用控件 `OnEvent`（调试） |
| `YGO_UI_HIDE_EXTRA` | `1` | 动作后隐藏多余弹窗/卡片预览 |
| `YGO_UI_CLOSE_EMPTY` | `1` | 自动关闭空白残留框 |
| `YGO_AUTO_DISMISS` | `1` | 自动确认阻塞对话框 |
| `YGO_SUPPRESS_MODALS` | `1` | 断线/卡组错误等原生弹窗改为只记日志 |
| `YGO_PLACE_COORDS` | `server` | 放卡坐标约定：`server`（LocalPlayer 规则）/ `local`（raw half） |
| `YGO_RESUME_JOURNAL` | `1` | 写/读 `resume-context-<port>.json`（重连 journal） |
| `YGO_RESUME_PATH` | 自动 | 覆盖 journal 路径 |
| `YGO_TP` | 未设 | 强制先/后攻：`1`=先攻、`2`=后攻（直接发 `CTOS_TP_RESULT`，日志 `TP choice res=..`；未设时控制驱动模式发布 `tpPending` prompt 等 `/act_id`） |
| `YGO_SIDE_AUTO` | 未设 | `1`=收到 `STOC_CHANGE_SIDE` 时自动点 `SIDE OK`（日志 `SIDE auto-ok queued`），用于 LAN/脚本验证；排位不设，仍走 `sidePending`+`/act_id kind=side_ok` |
| `YGO_NET_PROBE` | `1` | `/tip` 探活开关 |
| `YGO_NET_PROBE_REQUIRE_SYSTEM` | `1` | 只把 `type==8` 的系统消息算探活回包 |
| `YGO_NET_IDLE_MS` | `2500` | 多久无 STOC 触发探活 |
| `YGO_NET_PROBE_GAP_MS` | `3000` | 两次探活最小间隔 |
| `YGO_NET_PROBE_WINDOW_MS` | `1500` | 探活回包等待窗口 |
| `YGO_NET_DEAD_SILENCE_MS` | `10000` | 静默判死阈值（被动探测） |
| `YGO_CTRL_DEBUG` | 未设 | 设任意值输出逐帧调试日志（`CDBG`/`DBG`） |

---

## 7. 排障

| 症状 | 检查 | 处理 |
|---|---|---|
| 连不上 `127.0.0.1:7788` | 客户端是否在跑；端口是否被占 | 看客户端 `control.log` 的 `listening on 127.0.0.1:<port>`；双开时两端必须不同端口 |
| `/actions` 空且 `awaiting:false` | 是否轮到你/是否有 prompt | 正常（对手回合/等待中）；用 `/wait_prompt` 等下一个 prompt |
| `{"ok":false,"error":"stale seq"}` | 你带的 `seq` 与 `/state.seq` 不一致 | 重新读 `/prompt`（或 `/state.seq`）后用返回值重试 |
| 动作被拒 `bad action id` | `id` 是否来自当前 `/actions` | 每次 prompt 都重新读 `/actions`，不要缓存旧 id |
| 卡组非法 / 无法开局 | `/state.deckError`：`server.set=true` 时看 `type/code/text`；`local.set=true` 看 `reason` | 换合规卡组（对照 `lflist.conf`）；`/deck` 后确认日志 `deck: select combo name=... -> confirmed` |
| 开局前卡在准备界面 | `/lobby.bothReady`、`/state.prompt`（`tpPending`） | 先/后攻未选：`/act_id?id=0/1`；未准备：`/ready` |
| 掉线/假死 | `/net`：`alive`、`state`、`sinceLastStoc`、`probe.fails` | `state=confirmed_dead/disconnected` 时重启客户端并用 `/join`+`/ready` 重进（journal 会给出 `resume`）；LAN 服务端不支持续局 |
| 弹窗卡住分析 | `/ui.popups`、`/ui.windowsVisible`、日志 `UI auto-confirm`/`MSG_RETRY` | 控制层默认自动确认 HINT/确认框；仍有残留可看 `YGO_AUTO_DISMISS`/`YGO_UI_HIDE_EXTRA` |
| 想验证"客户端到底发没发包" | `/ctos`、`/stoc`、`/msg` | 观察 CTOS/STOC 序列与最后一条消息的 `hex` |

---

## 8. 与 MCP 的关系（一句话）

MCP 层（bridge 仓库）只是把本文件的部分端点封装成 agent 工具（`control_wait_prompt`/`control_actions`/`control_act_id` 等）。
HTTP 直连能力是全集：MCP 没暴露的端点（`/net`、`/ui`、`/lobby`、`/stoc`、`/net/probe` 等）可直接用 `curl` 调。
