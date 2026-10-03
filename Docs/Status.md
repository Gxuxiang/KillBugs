# KillBugs 状态与交接

> 最后更新：2026-10-03
>
> 这份文档的目的是**抗遗忘**：代码能说明"现在是什么样"，但说不清"为什么是这样、哪里验证过、
> 哪些坑踩过"。那些东西只存在于当时的对话里，所以固化在这里。
>
> 深入某个系统时看对应的专项文档：大厅见 [`Lobby.md`](Lobby.md)。

---

## 这是什么

四人合作的俯视角虫潮射击。四个玩家**各自一个客户端**（`bUseSplitscreen=False`），
局域网发现凑齐 → 大厅准备 → 一起进竞技场跑图。

所有面向玩家的文字是中文；注释和标识符是英文。

- 仓库：https://github.com/Gxuxiang/KillBugs （分支 `main`）
- 引擎：UE 5.8，路径 `D:/Epic Games/UE_5.8`
- 工程：`D:/Projects/UE/5.8/KillBugs`

---

## 已经实现的系统

| 系统 | 核心类 | 形态 |
|---|---|---|
| 虫潮 | `AKBEnemyDirector` | 最多 600 只，**一个 Actor** 里的扁平数组 + 空间哈希；复制的 `FFastArraySerializer`；HISM + 池化绘制 |
| 子弹 | `AKBProjectileDirector` | 同样形态：**一个 Actor**、一个飞行数组、不是每发一个 Actor。**伤害在命中时结算**，不是开火时 |
| 武器 | `UKBWeaponInventoryComponent` + `UKBWeaponDefinition` | 数据驱动；自动/手动/范围三种投放方式 |
| 卡牌 | `UKBCardDefinition` + `AKBGameMode` | 每波一次选卡；卡池靠**扫描资产目录**发现，不维护手工列表 |
| 波次 | `AKBGameMode` + `AKBGameState` | 服务器权威的阶段机：`Warmup → WaveActive → Explore → CardDraft → …` |
| 属性 | `UKBStatSheetComponent` | 挂在 Pawn 上，不是 PlayerState |
| 音频 | `UKBAudioSubsystem` | 音乐床随复制的阶段交叉淡入；UI 音效 |
| 战斗 UI | `AKBHud` | **Canvas 即时模式**（不是 UMG）：时间轴、血条、状态读数、选卡面板 |
| **联机大厅** | `AKBLobby*` + `UKBSessionSubsystem` | 见 [`Lobby.md`](Lobby.md) |

**为什么 UI 用 Canvas 不用 UMG**：整个游戏只有大厅和战斗两块界面，搭控件树的开销大于它带来的好处。
真要换成 UMG，读的数据都在复制的 PlayerState/GameState 上，不需要动玩法代码。

---

## 本轮（2026-10-03）修掉的问题

这些都是**真 bug**，且大多只有在真跑起来时才暴露。留档是因为症状离原因都很远。

### 1. 客户端子弹数量/方向错误

`FireAtAcquiredTarget` 把**未归一化的位移向量**传进了 `FVector_NetQuantizeNormal`。
该类型每个分量只承载 -1..1，引擎的 `WriteFixedCompressedFloat<1,16>` 会把超出的分量
**clamp 成 ±1 并返回失败** —— 一次霰弹的所有弹丸于是指向同一个对角线，日志刷满
`Native NetSerialize ... failed`。

修在两处：调用方补归一化（和手动武器路径一致），并在 `FireShot` 这个**网络边界**上强制归一化。

### 2. 会话泄漏 —— "房间一直拒绝我"

OnlineSubsystemNull **在客户端加入成功时也会在本机留下一个具名会话**。没清理它，
这台机器之后的 `CreateSession` / `JoinSession` **就永久失败**，而引擎的拒绝理由一个字都到不了界面：

```
OSS: Session (KillBugsGame) already exists, can't join twice
```

修法：建/加入前先 `GetNamedSession()` 检查，有就先销毁、再从销毁回调里**继续原来想做的事**
（销毁是异步的，所以"玩家到底想干什么"必须存下来 → `EPendingSessionAction`）。

### 3. 完成委托泄漏

`CancelFindSessions` **不触发** `OnFindSessionsComplete`。只在完成回调里清句柄的话，
"搜索中重复点刷新"会把旧回调永久留在多播委托上 —— 四次点击 = 四个活回调，下一次完成时全跑。
日志里的症状是 `found N room(s)` 在**同一毫秒打印四次**。

修法：注册新句柄之前先清旧的。

### 4. "开始游戏"按钮对所有人永远是灰的

```cpp
{ EKBLobbyButton::Start, TEXT("开始游戏"), bHosting && bOffline }   // 错的
```

`bOffline` 是"State == Offline"，可主机建完房状态就是 `Hosting`。**门禁谓词写错了。**

**为什么无头测试没抓到**：命令行直接调 `KB.Lobby.Start`，不经过 UI 的启用判断。
**UI 的启用逻辑是无头测不到的** —— 这是本次最值得记住的教训。

### 5. PIE 广播端口是 0

主机在编辑器里 PIE 时，日志写着 `IpNetDriver listening on port 17777`，但 beacon 里带的是
`192.168.31.153:0`。客户端照着连 0 号端口，瞬间失败，**屏幕上什么都不显示**。
现在代码会明确拒绝 `:0` 地址并说明原因，但根子上只有一条路：**局域网测试必须用独立游戏进程**。

### 6. 准备（Ready）系统

原本的设计是"只有主机能开始、其他人没有任何表达方式"——加入者除了看着什么都做不了。
现在：每人一个准备按钮 → 主机看到谁准备好了 → **全员准备后"开始游戏"才亮**，
且**服务端也真的会拒绝**（不只是界面变灰）。准备状态放在大厅专属的 `AKBLobbyGameState`。

---

## 验证到什么程度（重要）

诚实划清边界，避免把"没测过"当成"没问题"。

| 项 | 状态 |
|---|---|
| 局域网发现（同机双进程） | ✅ 实测 |
| 加入 → 主机接受 → 双方同房 | ✅ 实测 |
| **准备门禁**（人到齐但仍需等全员准备） | ✅ 实测，日志可证 |
| 全员准备 → 开局 → 双方进竞技场 | ✅ 实测 |
| **真人在两个窗口点完整流程** | ✅ 已做（2026-10-03） |
| 子弹复制无 NetSerialize 警告 | ✅ 实测（两边 0 条） |
| 大厅 UI 的**视觉质量**（排版/字体/悬停手感） | ⚠️ **只看过一次，没有细看** |
| 竞技场在联机下的完整玩法 | ⚠️ 只跑到"进图 + swarm 起来"，没打完整局 |
| `AKBGameMode::Logout` 的注释改动 | 无行为变化，仅澄清（原 TODO 是过期的） |

**无头测不到 UI**：`-nullrhi` 下 `DrawHUD` 不执行。能无头证明的只有数据链路和
`Lobby: click input bound`（绑定存在），画面对不对必须人看。

---

## 环境与构建约定

编辑器**必须关闭**才能编译（Live Coding 互斥锁）。**用户已授权直接关闭，不必再问。**

```bash
"D:/Epic Games/UE_5.8/Engine/Build/BatchFiles/Build.bat" KillBugsEditor Win64 Development \
  -project="D:/Projects/UE/5.8/KillBugs/KillBugs.uproject" -waitmutex
```

**改了构建配置**：编辑器 target 关掉了 unity build（`KillBugsEditor.Target.cs` 里的
`bUseUnityBuild = false`）。原因：unity 把 22 个源文件塞进**一个**翻译单元，
改一行要全编且单核 —— 实测 **~2 分钟/次**；关掉后 **~6 秒**。打包 target 保持开启。

> 这是**目标级**设置。`ModuleRules` 上没有 `bUseUnityBuild`，写进 `Build.cs` 会直接
> `CS0103` 编译失败。

### 会浪费时间的坑（都踩过）

1. **C1001 内部编译器错误**：报的位置会乱跳（引擎头、你没碰过的项目 .cpp、生成的 `.gen.cpp`）。
   这是编译器/中间产物的间歇问题，**不是代码 bug**。顺序：**先重试一次**，不行再
   `rm -rf Intermediate/Build/Win64/{,x64/}UnrealEditor/{Inc,Development}/KillBugs` 重编。
   报错位置本身没有意义。
2. **unity build 下的匿名 namespace 冲突**：一个 unity blob 里**所有匿名 namespace 是同一个**。
   `KBLobbyHud.cpp` 的 `Ink`/`Dim`/`PanelFill` 和 `KBHud.cpp` 撞名就是这个。
   解法是具名 namespace + 别名，**不要**用 `using namespace`（会让每处使用都变歧义）。
3. **UBA 有全局编译缓存**（`C:\ProgramData\Epic\UnrealBuildAccelerator`），
   所以删掉 `Intermediate` **不等于**真冷编译。`touch` 一个内容没变的文件是缓存命中（~9 秒）；
   只有**真改内容**才测得出真实的"改一行要等多久"。
4. **UE Python 脚本**：本项目的"报告成功但没落盘"是高频失败模式。
   改资产的脚本和校验脚本必须**分成两个进程**跑。
   还有：`get_all_level_actors()` **不包含** WorldSettings，要拿它得走
   `UnrealEditorSubsystem.get_editor_world().get_world_settings()`。

---

## 怎么复现验证

### 局域网联机（同机双进程，**一台机器就够**）

```bash
# 两个终端各跑一个：这才是真实进程（会开窗口）
".../UnrealEditor.exe" "<项目>.uproject" -game -windowed -port=7777   # 主机
".../UnrealEditor.exe" "<项目>.uproject" -game -windowed              # 客户端
```

主机点「创建房间」→ 客户端点「刷新」→ 点列表那行 →两边各按「准备」→ 主机「开始游戏」。

**判据**：客户端地址应是 `192.168.31.153:7777`，出现 `:0` 说明房主是 PIE 开的。

### 无头端到端（不需要人）

```bash
".../UnrealEditor.exe" "<项目>.uproject" -game -nullrhi -unattended -nosplash -NoHotReload \
  -port=7777 -ExecCmds="KB.Lobby.Host,KB.Lobby.Ready,KB.Lobby.Start 2" -LOG=LobbyHost.log
".../UnrealEditor.exe" "<项目>.uproject" -game -nullrhi -unattended -nosplash -NoHotReload \
  -ExecCmds="KB.Lobby.JoinFound,KB.Lobby.Ready" -LOG=LobbyClient.log
```

逐段判据见 [`Lobby.md`](Lobby.md)。调试命令：`KB.Lobby.{Host,Find,Join,JoinFound,Ready,Start,Solo}`。

### 别用编辑器 Play 菜单测局域网

两个原因，都会以"看起来像 bug"的方式失败：
**① PIE 广播端口是 0；② PIE 多人模式里两个实例已经被连好了**，再点「加入房间」是重复连接，
正好触发会话泄漏。PIE 用来调 UI 和单机流程没问题。

---

## 未决问题

1. **主机要不要也按准备？** 现在是"所有人包括主机都要按"。想让主机免按直接开，
   要**同时**改两处，否则按钮和规则会不一致：
   `AKBLobbyHud::DrawButtons` 里 Start 的启用条件 + `AKBLobbyGameMode::RequestStartGame` 的校验。
2. **大厅没有音乐**（用普通 `AGameStateBase`，音频子系统找不到 `AKBGameState` 会静默不播）。
3. **`NextPlayerIndex` 不复用**：退出再加入会拿到更大的序号。只影响排序显示，无正确性问题。
4. **准备状态不跨换图**（新世界新 GameState）。加入者换图进来要重新按准备——这是对的，
   但如果你希望"沿用上一局的准备"，那需要额外做持久化。

---

## 下一步候选

- 大厅打磨：房间名显示、准备/取消的准备音效、更清楚的房间列表信息
- 战斗：手感调优、更多武器与卡牌
- 联机：把 `AKBPlayerController::SetAimYaw` 的速率限制补上（代码里标着 Phase 5 TODO），
  防止客户端瞬转瞄准
