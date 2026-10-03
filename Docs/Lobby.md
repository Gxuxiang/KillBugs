# 联机大厅（LAN Lobby）

四个玩家各自一个客户端（`bUseSplitscreen=False`），靠局域网发现凑齐，然后一起进竞技场跑图。
本文档记录**已经实现并验证过的**形态。

> 文件名用 ASCII（`Lobby.md`）而不是中文，是为了和仓库其余部分一致（`Tools/kb_setup_arena.py`、
> `Lvl_Arena` 等全是 ASCII）；内容仍用中文。

---

## 流程

```
启动 ──> Lvl_Lobby（AKBLobbyGameMode，不刷怪、不跑波次）
           │
           ├─[创建房间]  ServerTravel("Lvl_Lobby?listen") ──> 仍是大厅，但成了 listen server
           │                                                  └─ CreateSession(bIsLANMatch) 起 beacon
           ├─[刷新]      FindSessions(bIsLanQuery) ──> 列出局域网内的房间
           ├─[加入]      JoinSession(result) ──> GetResolvedConnectString ──> ClientTravel("ip:port")
           ├─[准备]      ServerRequestSetReady ──> 大厅 GameState 的 ReadyPlayerIndices
           ├─[单机开始]  ServerTravel("Lvl_Arena")
           └─[开始游戏]  (仅主机，且【全员已准备】才可点) ServerTravel("Lvl_Arena?listen")
                          所有客户端被 ProcessServerTravel 一起拉过去
```

开局后进入 `Lvl_Arena`，由原来的 `AKBGameMode` 接管，波次逻辑完全不变。

---

## 代码结构

| 文件 | 职责 |
|---|---|
| `Source/KillBugs/{Public,Private}/Net/KBSessionSubsystem.*` | **唯一**接触 `IOnlineSession` 的地方。`UGameInstanceSubsystem`，因此能跨换图存活 |
| `.../Lobby/KBLobbyGameMode.*` | 大厅的 `AGameModeBase`。派玩家序号、认定主机、开局。另含全部 `KB.Lobby.*` 调试命令 |
| `.../Lobby/KBLobbyGameState.*` | `AGameStateBase` + 一个可复制的 `HostPlayerIndex` |
| `.../Lobby/KBLobbyPlayerController.*` | 自己绑运行时点击输入（大厅不生成战斗 Pawn） |
| `.../UI/KBLobbyHud.*` | Canvas 绘制的大厅界面 |
| `Tools/kb_setup_lobby.py` | 生成 `Lvl_Lobby`（含 GameMode 覆盖） |
| `Tools/kb_verify_lobby.py` | **独立进程**回读校验，见下方「为什么要有这个脚本」 |

### 为什么大厅 GameMode 不继承 `AKBGameMode`

`AKBGameMode` 是"一局"的权威：`BeginPlay` 就生成 swarm 和弹道导演，`Tick` 驱动波次时钟。
大厅一条都不需要，继承意味着把它们全部关掉，而且以后波次机的每次改动都要再对着一个
永远用不到它的模式复查一遍。

大厅**复用** `AKBPlayerState`——需要那个稳定的玩家序号和玩家名。

### 大厅不生成 Pawn

`DefaultPawnClass = nullptr`。战斗 Pawn 带来相机、武器、属性表和**点击开火绑定**；最后一项会
把按钮需要的点击全部吃掉。没有 Pawn 就没有开火绑定，控制器的点击处理因此毫无歧义。

界面铺满全屏且不透明，所以大厅地图里没有相机、也没有需要摆的场景。

---

## 配置

`Config/DefaultEngine.ini`：

```ini
GameDefaultMap=/Game/KillBugs/Maps/Lvl_Lobby.Lvl_Lobby   ; 开游戏进大厅
EditorStartupMap=/Game/KillBugs/Maps/Lvl_Arena.Lvl_Arena  ; 编辑器仍开竞技场，PIE 单机流程不变

[OnlineSubsystem]
DefaultPlatformService=Null
```

`Config/DefaultGame.ini`：

```ini
[/Script/KillBugs.KBGameSettings]
MaxPlayers=4          ; 房间容量：会话的 NumPublicConnections，也是大厅界面上的 x/N

[/Script/Engine.GameSession]
MaxPlayers=4          ; 真正的准入上限
```

> **上面两个 `MaxPlayers` 不是重复。** 会话的 `NumPublicConnections` 只是**广播值**——
> OnlineSubsystemNull 从不拒绝加入。真正拒绝发生在 `AGameSession::AtCapacity`（经
> `PreLogin → ApproveLogin` 到达），它读的是 `Engine.GameSession` 那个，引擎默认 **16**。
> 只设前者的话，"4 人房间"会安静地放进第 5 个人。引擎的 GameSession 看不到 `UKBGameSettings`，
> 所以只能手工保持一致：**改房间大小要同时改这两处。**

---

## 关键实现约束（改之前请先读）

这几条都是踩过坑的地方，任何一条弄错都会以**看似无关**的方式失败。

### 1. `?listen` 是必需的，不是装饰

目的地世界的 net mode **完全由 travel URL 推导**（`UWorld::AttemptDeriveFromURL`）：既没有
`?listen` 也没有 host 的 URL 会解析成 `NM_Standalone`。所以主机切到竞技场时不带 `?listen`，
会静默地把主机降级成 standalone，**所有客户端在抵达时被踢掉**——看起来像断线 bug，不像漏了个 URL 选项。

单机开局则相反，不能带（没人要来，开个监听端口没意义）。

### 2. 连接地址必须走 `ClientTravel`，不是 `ServerTravel`

`AGameModeBase::CanServerTravel` 只接受长包名（以 `/` 开头），含 `:` 或 `\` 直接拒绝。
加入用的 `"ip:port"` 会撞上它并报 `CanServerTravel: FURL 192.168.x.y:7777 blocked`。

**而且不能按 net mode 分支**：客户端在加入那一刻还是 standalone（它自己刚从大厅启动、还没有连接），
所以"我是不是客户端"的判断会选中 `ServerTravel`，加入必挂。**由 URL 的性质决定**：

- 地图路径 → `ServerTravel`（`UKBSessionSubsystem::TravelToMap`）
- `host:port` → `ClientTravel`（`UKBSessionSubsystem::ConnectTo`）

### 3. 广告要同时满足 `bShouldAdvertise` 和 `bIsLANMatch`

`FOnlineSessionNull::NeedsToAdvertise` 是 `bShouldAdvertise && IsHost && (bIsLANMatch && ...)`
——**与**关系，不是或。少任何一个都会得到一个"本地存在但从不打开 beacon socket"的会话，
局域网搜索永远返回空，且日志里一个字都不说。

### 4. 客户端不需要知道地图

加入握手时，服务器发现客户端不在自己的地图上，会主动发 `ClientTravel` 把它拉过去。所以
连接串里硬编码地图名会变成第二个真相来源，主机一进竞技场就过期。

### 5. 会话委托都是多播委托

`OnlineSessionDelegates.h` 里全是 `DECLARE_MULTICAST_DELEGATE_*`。绑定时存下 `FDelegateHandle`，
并在 `Deinitialize()` 里成对 `ClearOn*Delegate`——会话接口比子系统活得久，留着的 handle
会在关闭时回调进已释放的内存。

### 6. Unity build 下别用匿名 namespace 放通用名

一个 unity blob 会把多个 `.cpp` 拼进同一个 TU，于是里面**每个匿名 namespace 都是同一个**。
`KBLobbyHud.cpp` 的 `Ink`/`Dim`/`PanelFill` 和 `KBHud.cpp` 撞名就是这个原因。
解法是具名 namespace + 别名（`namespace Style = KBLobbyHudPrivate;`），**不要**用
`using namespace`——那会让每一处使用都变成歧义。

### 7. 具名会话会赖在机器上——建/加入之前必须先销毁

**这条最容易重犯，也是最难从症状反推的。**

OnlineSubsystemNull 在**加入成功时也会在本机留下一个具名会话**（不只是主机端）。于是
`CreateSession` / `JoinSession` 在一个已存在同名会话的机器上会直接失败：

```
OSS: Session (KillBugsGame) already exists, can't join twice
OSS: Cannot create session 'KillBugsGame': session already exists.
```

表现是：**这台机器成功加入过一次之后，之后的创建和加入就永久失败**。而且引擎的拒绝理由
一个字都不会到界面上，玩家看到的就是"这个房间一直拒绝我"——症状离原因非常远。

所以 `HostSession` / `JoinSession` 都要先 `GetNamedSession()` 检查，有就先销毁、再从销毁回调里
接着做原来的动作（`UKBSessionSubsystem::DestroySessionThen` + `EPendingSessionAction`）。
销毁是异步的，所以"玩家到底想干什么"必须存下来。**不要**把这个前置清理去掉。

### 8. 完成委托的句柄必须在注册新的一句之前清掉

`CancelFindSessions` **不会**触发 `OnFindSessionsComplete`。所以如果只在完成回调里清句柄，
那么"搜索进行中再点一次刷新"会把旧回调永久留在多播委托上。四次快速点击 = 四个活回调，
下一次完成时四个一起跑——日志里表现为 `found N room(s)` 在**同一毫秒**打印四次。

凡是要注册 `AddOn*Delegate` 的地方，注册前先 `if (Handle.IsValid()) ClearOn*Delegate_Handle(Handle);`。

### 9. 从编辑器测试时的两个陷阱

- **PIE 玩的是编辑器里当前打开的关卡**，不是 `GameDefaultMap`。所以 `EditorStartupMap` 必须也是
  `Lvl_Lobby`，否则按 Play 直接进竞技场：`AKBGameMode` 跑起来、波次时钟开始，**大厅根本不出现**——
  看起来像"整个功能没做"。想直接测战斗，手动打开 `Lvl_Arena` 再 Play。
- **PIE 的多人模式（2 个客户端）里两个实例已经被连起来了**，再走大厅的"加入房间"是**重复连接**，
  正好会触发第 7 条。
- **PIE 的房主广播出去的端口是 0，谁也别想连上。** 主机日志里网络驱动明明写着
  `IpNetDriver listening on port 17777`，但 beacon 里带的是 `192.168.31.153:0` —— PIE 的网络驱动
  不把自己的端口报给会话。客户端于是照着连 0 号端口，瞬间失败，而屏幕上**什么都不显示**。
  `HandleJoinSessionComplete` 现在会明确拒绝 `:0` 地址并说明原因，但根子上要解决只有一条路：
  **局域网测试必须用两个独立游戏进程**，别用编辑器的 Play 菜单。

```bash
# 两个终端各跑一个，这才是真实进程（会开窗口）
"D:/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe" \
  "<项目>.uproject" -game -windowed -port=7777    # 主机
"D:/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe" \
  "<项目>.uproject" -game -windowed               # 客户端
```

**同一台机器就够**，不需要两台电脑：beacon 走的是广播地址，`FLanBeacon` 绑端口前调了
`SetReuseAddr()`，两个进程都能收到（实测通过）。单机测试唯一的不便是两个窗口抢鼠标焦点。

### 10. `GetNetMode()` 会在真正连上之前就翻成 `NM_Client`

`ClientTravel("ip:port")` 一被调用，`UWorld::AttemptDeriveFromURL` 就从**待处理的 URL** 推导出
net mode 并立刻返回 `NM_Client` —— 此时服务器还没连上、当前世界还是旧的。等这个值变化再动作，
会落在**旧世界**里，而紧接着的换图会建一个全新的 GameState，把刚才写进去的东西丢掉。

要判断"真的进了别人的房间"，客户端必须看**有没有真正的网络连接**：
`PlayerController->GetNetConnection() != nullptr`。注意这里要按角色分开 ——
listen server 自己的控制器**永远没有** NetConnection，所以主机侧只能看 `NM_ListenServer`。

这条是踩出来的：`KB.Lobby.Ready` 原本只看 net mode，结果客户端在换图前就在自己的旧世界里按了准备，
换图后准备状态清零，主机一直等到超时。

**准备状态本身不跨换图**，这是对的（新世界新 GameState）。真人点不到那一毫秒的窗口，
所以这是调试命令的判据问题；但任何"等连接就绪再做事"的新代码都会踩同一个坑。

---

## 调试命令

仿 `KB.Weapon.FireManual` 的既有模式注册：

| 命令 | 作用 |
|---|---|
| `KB.Lobby.Host` | 创建房间（等价于点"创建房间"） |
| `KB.Lobby.Find` | 搜一次 |
| `KB.Lobby.Join <ip:port>` | 直连，**绕过 beacon 发现**（发现环节出问题时的兜底） |
| `KB.Lobby.JoinFound [index]` | 搜索 → 等结果 → 加入该结果。一条命令跑完整个发现链路 |
| `KB.Lobby.Ready [0]` | 按准备（给 0 则取消准备）。会先等本机真的连上主机再按 |
| `KB.Lobby.Start [minPlayers]` | 以主机身份开局；会等到 minPlayers 人齐**且全员已准备**，否则服务端会拒绝 |
| `KB.Lobby.Solo` | 直接进竞技场，不涉及任何会话 |

`Join` / `JoinFound` / `Start` 内部会**等待**（`FTSTicker`）而不是立即执行：`-ExecCmds` 只在
启动时跑一次，而搜索要 ~5 秒、别人加入要更久。等待器每帧重新解析当前 game world，所以能跨过换图。

---

## 无头测试方法

编辑器关闭。两个进程（`-ExecCmds` 用逗号串联多条命令）：

```bash
# 主机：开房 -> 自己准备 -> 等 2 人齐且全员准备后再开局
UnrealEditor.exe <uproject> -game -nullrhi -unattended -nosplash -NoHotReload \
  -port=7777 -ExecCmds="KB.Lobby.Host,KB.Lobby.Ready,KB.Lobby.Start 2" -LOG=LobbyHost.log

# 客户端（约 15 秒后起）：搜索 -> 加入 -> 准备
UnrealEditor.exe <uproject> -game -nullrhi -unattended -nosplash -NoHotReload \
  -ExecCmds="KB.Lobby.JoinFound,KB.Lobby.Ready" -LOG=LobbyClient.log
```

**逐段判据：**

| 阶段 | 期望在日志里看到 |
|---|---|
| 大厅地图 + 大厅模式 | `Bringing World .../Lvl_Lobby` 且 `Lobby: <name> is the host` |
| 点击绑定存在 | `Lobby: click input bound`（否则界面能画但完全点不动，无头看不出来） |
| 建会话 | `Session: hosting on LAN, 4 slot(s)` |
| 变 listen server | `ProcessServerTravel: ...Lvl_Lobby?listen` |
| **发现** | 客户端 `Session: found N room(s) on the LAN`（N ≥ 1） |
| 加入 | 客户端 `Session: joining <ip>:7777`；主机 `Join succeeded: <name>` + `joined (index 1, 2 player(s))` |
| **准备（门禁）** | 主机先 `<host> is ready (1/1 ready)`；客户端加入后主机出现 `<client> is ready (2/2 ready)`。**主机在 `2 player(s)` 之后不应立刻开局**，必须等到 `2/2 ready` |
| 开局 | 主机 `ready - 2 player(s) in the room, all ready` → `starting the run with 2 player(s)` → `ProcessServerTravel: ...Lvl_Arena?listen` |
| 都到了 | **两个**日志都出现 `Bringing World .../Lvl_Arena`；主机 `Run started: warmup 15s` |
| 回归 | 两边 `Native NetSerialize` 计数必须为 **0** |

**同机双进程的 LAN 广播是实测可用的。** `FLanBeacon` 在 `Bind` 前调用 `SetReuseAddr()`，
两个进程能同时绑 beacon 端口，Windows 也确实把广播投递给了两者。如果哪天发现搜不到了，
**先查防火墙（UDP 14001）**，不要怀疑设计。

### 回归测试：先开房再加入（覆盖第 7 条约束）

上面的正常路径**不会**触发"清理旧会话"那条分支（客户端加入前本机没有会话）。要专门覆盖它，
让客户端先自己开一个房、再去加入别人的房：

```bash
# 主机（待在房间里别开局）
UnrealEditor.exe <uproject> -game -nullrhi -unattended -nosplash -NoHotReload \
  -port=7777 -ExecCmds="KB.Lobby.Host" -LOG=DupHost.log

# 客户端：先开房，再搜并加入别人的房
UnrealEditor.exe <uproject> -game -nullrhi -unattended -nosplash -NoHotReload \
  -ExecCmds="KB.Lobby.Host,KB.Lobby.JoinFound" -LOG=DupClient.log
```

客户端日志里必须出现这两行，而且**不能**有 `already exists`：

```
Session: clearing a leftover session before joining
Session: joining <ip>:7777
```

主机侧则应出现 `Join succeeded: <name>`。

---

## 已知限制与未决

- **UI 未经肉眼验证。** `-nullrhi` 下 `DrawHUD` 不执行，布局、中文字体、悬停反馈、按钮手感
  都需要有人在编辑器 PIE 里看一眼。无头能证明的只有"点击绑定存在"和"数据链路正确"。
- **大厅没有音乐。** 大厅用普通 `AGameStateBase`，`UKBAudioSubsystem` 找不到 `AKBGameState`
  会静默不播。不崩，只是空。
- **主机退出 = 房间结束。** 主机就是 listen server，他退了进程就没了，所以没有做主机交接
  （见 `AKBLobbyGameMode::Logout` 的注释）。
- **玩家序号不复用。** `NextPlayerIndex` 单调递增，退出再加入会拿到更大的序号。只用于排序和
  身份显示，有空洞不影响正确性。
- **准备规则**：房里**每个人（含主机）都要按准备**，"开始游戏"才会亮。这是有意的 ——
  主机不按就等于"我还没准备好"，规则对所有人生效才不会有例外。想让主机免按直接开，
  改 `AKBLobbyHud::DrawButtons` 里 Start 的启用条件并去掉 `AKBLobbyGameMode::RequestStartGame`
  里的 `AreAllPlayersReady()` 检查（两边要一起改，否则按钮和规则会不一致）。
- **准备状态不跨换图**：`ReadyPlayerIndices` 在新世界是空的。加入者换图进来后要重新按准备。
- **产品默认值**（要改告诉我）：4 人；允许中途加入（迟到玩家进竞技场并重开 Warmup）。

---

## 为什么要有 `kb_verify_lobby.py`

本项目最常见的失败模式是**"报告成功但没落盘"**：一个脚本改了资产又在自己进程里读回来，
读到的是自己的内存对象。所以生成脚本和校验脚本必须**分成两个进程**跑，校验才作数。

```bash
UnrealEditor-Cmd.exe <uproject> -ExecutePythonScript=".../kb_setup_lobby.py"  -unattended -nosplash -nullrhi
UnrealEditor-Cmd.exe <uproject> -ExecutePythonScript=".../kb_verify_lobby.py" -unattended -nosplash -nullrhi
```

另注：设置 World Settings 的 GameMode 覆盖**不能**靠遍历 `get_all_level_actors()` 找
`WorldSettings`——它不在那个列表里，静默找不到。要走
`UnrealEditorSubsystem.get_editor_world().get_world_settings()`。这两点都写在脚本注释里了。
