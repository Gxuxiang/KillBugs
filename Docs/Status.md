# KillBugs 状态与交接

> 最后更新：2026-10-04
>
> 这份文档的目的是**抗遗忘**：代码能说明"现在是什么样"，但说不清"为什么是这样、哪里验证过、
> 哪些坑踩过"。那些东西只存在于当时的对话里，所以固化在这里。
>
> 深入某个系统时看对应的专项文档：大厅见 [`Lobby.md`](Lobby.md)，
> 虫子模型与动画见 [`BugAnimation.md`](BugAnimation.md)。

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
| 虫子模型与动画 | 见 [`BugAnimation.md`](BugAnimation.md) | 真模型 + **顶点动画贴图（VAT）** 烘焙的骨骼动画；每只虫子**独立相位**，不同步摆腿 |
| 死亡爆浆与粘液 | `UKBGoreComponent` | 贴花池（上限 64、超时淡出）+ Niagara 每帧生成预算；**从复制数组的移除本地推导**，零带宽 |
| 子弹 | `AKBProjectileDirector` | 同样形态：**一个 Actor**、一个飞行数组、不是每发一个 Actor。**伤害在命中时结算**，不是开火时 |
| 武器 | `UKBWeaponInventoryComponent` + `UKBWeaponDefinition` | 数据驱动；自动/手动/范围三种投放方式；**每把枪可配后坐力**（把人往反方向推） |
| 卡牌 | `UKBCardDefinition` + `AKBGameMode` | 每波一次选卡；卡池靠**扫描资产目录**发现，不维护手工列表 |
| 波次 | `AKBGameMode` + `AKBGameState` | 服务器权威的阶段机：`Warmup → WaveActive → Explore → CardDraft → …` |
| 局末结算 | `AKBGameMode::EndRun` + `AKBGameState::RunResult` | 一局**唯一的出口**：团灭（以后撤离成功走同一条路）→ 结算界面 → 回大厅 |
| 交互原语（读条/区域） | `UKBChannelComponent` | "站在区域内 + 条件持续满足 + 读条 N 秒"。撤离、救援、开门共用；暂停/重置是每处自己选的 |
| 救援 | `AKBCharacter` 上的 `UKBChannelComponent` | 倒地者投影出救助圈，队友进圈读条；**离开则重新计时**；救起回复一半血。圈和进度由 `AKBHud::DrawRescueCircles` 画在地面上 |
| 属性 | `UKBStatSheetComponent` | 挂在 Pawn 上，不是 PlayerState |
| 音频 | `UKBAudioSubsystem` + `UKBSwarmAudioComponent` | 音乐床随复制的阶段交叉淡入；UI 音效；**虫子移动音按最近 N 只定位播放** |
| 战斗 UI | `AKBHud` | **Canvas 即时模式**（不是 UMG）：时间轴、血条、状态读数、选卡面板、左下角队伍面板 |
| 受伤反馈 | `UKBStatSheetComponent` + `UKBGameSettings` | 受伤时震屏；震动资产由设置指定，**留空则完全不震** |
| **联机大厅** | `AKBLobby*` + `UKBSessionSubsystem` | 见 [`Lobby.md`](Lobby.md)；开局时有一段延迟用于显示加载界面 |

**为什么 UI 用 Canvas 不用 UMG**：整个游戏只有大厅和战斗两块界面，搭控件树的开销大于它带来的好处。
真要换成 UMG，读的数据都在复制的 PlayerState/GameState 上，不需要动玩法代码。

---

## 本轮（2026-10-04）修掉的问题

同样都是**只有在真跑起来时才暴露**、且症状离原因很远的 bug。

### 1. 虫子完全没有移动音效 —— 两个 bug 叠在一起

**症状**：虫子接上了移动音、资源也填了、日志显示组件池子建好了、`Play()` 也调了，**但什么都听不到**。

**原因一：听觉半径量到了相机上。**
相机是 SpringArm（`CameraDistance 1800`、`CameraPitch -65°`），在角色上方约 **1630** 单位。
而听觉半径只有 1800，且按**到相机的三维距离**判定 —— 相机高度先吃掉 90% 的预算：

| 虫子到角色的地面距离 | 到相机的三维距离 | 结果 |
|---|---|---|
| 0（脚下） | 1631 | 衰减后音量 ≈ 5.6% |
| 500 | 1706 | ≈ 3% |
| 762 | 1800 | 刚好被排除 |

修法：听觉距离改按**地面平面（XY）**算（`KBListener::HearingDistance`）。声源仍放在虫子三维位置，
左右定位由引擎照常处理，只有音量和半径判定用水平距离 —— 俯视游戏里"听得见多远"本来就是地面距离。

**原因二：槽位从来没有被标记为"已绑定"。**
`UKBSwarmAudioComponent::BindSlot` 漏了 `BoundStableIds[Slot] = StableId;`（是清理重复赋值时删出来的，
注释还写着"这行由 BindSlot 设置"，但它从来没设过）。后果极隐蔽：`Play()` 照常调用、`SetSound` 照常执行、
池子看起来完全健康，但 `UpdateEmitters` 认为槽位空闲，**从不给它位置、也从不给它音量**，
声音以池子创建时的 0 音量播放。**什么都不报错。**

**为什么两个都没被早发现**：`-nullrhi` 听不到声音，而"没声"和"代码没跑"长得一模一样。
最后是靠新加的 `KB.Swarm.AudioLog`（cvar 门控）打出来的：修复前 `candidates=36 bound=0`，
修复后 `bound=6` 且逐槽 `playing=1`。**先加日志再下结论**，这一条在这一轮里被反复验证。

### 2. 飞行物"不会飞"，只在生成点炸一下

**症状**：换上 Niagara 特效后，子弹看着不动。

**排查**：打了一条临时诊断，同时打印子弹的**逻辑位置**和**特效组件的世界位置** —— 两者逐帧完全相同
且都在变。**组件跟着飞，画面却不动**，那么唯一的解释就是粒子本身不跟随。

**原因**：Niagara 资产的发射器是 **World Space**。世界空间的粒子在"出生"那一刻就钉死在当时的世界坐标上，
之后把组件搬到哪它们都不动。

**修在资产上**（勾 Local Space），不在代码。这条值得记住的是**排查手法**：先证明组件在动，
把问题从"代码没跟上"和"特效不跟随"里切出来，再去动资产。

### 3. 大厅"开始游戏"点了完全没反应

**原因**：`ServerTravel` 是在处理点击的**同一个函数、同一帧**里同步调用的，世界在下一帧就被拆掉，
所以谁也画不出那一帧 —— 主机看到的是大厅卡住，其他人什么都没看到、也不知道发生了什么。

**关键点：光加一个复制标志不够。** 在同一帧里设标志再 travel 一样没救：标志需要一次网络更新才能传给别人，
而世界没了之后就没有更新了。所以必须**把 travel 推迟**（现在 0.5 秒引擎时间），标志才传得到所有人。
主机自己是 listen server，标志立即生效。

### 4. 一局结束之后没有然后了

**症状**：全队倒地的瞬间游戏就静止了 —— 没有结算、没有回大厅、没有重开。`RunOver` 这个阶段
**根本不在 Tick 的 switch 里**，所以进去就再也出不来；`ReviveDownedPlayers` 只在 `StartWave` 里调，
而 `StartWave` 从 `RunOver` 到不了。团灭是一个**永久死胡同**。

**为什么必须最先修**：撤离成功和团灭是同一个收尾流程的两端 —— 不先把这条路修出来，
后面每加一个功能都得绕开它。

**修法**：`EndRun(EKBRunResult)` 作为唯一入口（团灭在 Tick 里查，将来的撤离点在世界的另一头触发，
两者必须汇进同一个结算和同一条回大厅的路）。结果**闩锁**在 GameState 上：同一帧里
"团灭"和"成功撤离"同时发生的话，先到的那个说了算，不会被后到的覆盖。

回大厅用 `KBTravel::ToLobbyAsListenServer()` / `ToLobby()`，靠 `GetNetMode()` 区分。
别忘了 `?listen` —— 目的地的网络模式**只由 URL 决定**，少了它主机会被静默降级成 standalone、
所有客户端在目的地被踢掉（`KBTravel::ToArenaAsListenServer` 的注释里记着这个教训）。

### 5. 两条"看着实现了、其实什么都不做"的经济代码

- **`PlayerLevel` 从来没被写过**，永远是 1 —— 而 HUD 在画面上印着「等级 1」，
  同时每张卡的 `MinPlayerLevel` 门槛都在跟一个常量比较。现在由经验推导
  （`AKBPlayerState::ComputeLevelForXP`，曲线是 `KBSettings().XpPerLevel`）。
  （另注：目前**没有任何卡设过 `min_player_level`**，所以这个门槛即使修好了也还没有内容可筛。）
- **`XpGainMult` 没有调用者** —— `kb_setup_cards.py:179` 那张 +15% 经验的卡**什么都没干**。
  现在击杀奖励处会读击杀者的 `GetXpMultiplier()`，和武器在开火时读伤害倍率是同一个道理：
  中途选的卡应该影响下一次击杀。

### 6. 自动复活被救援替换（阶段 1）

**改动**：原来每个倒地的玩家在**下一波开始时自动满血站起来**（`AKBGameMode::ReviveDownedPlayers`，
只在 `StartWave` 里调）。这已删除 —— 现在把队友扶起来是**玩家自己做的事**：
倒地者脚下出现一个救助圈，队友站进去读条，读完回复**一半**血。

**为什么要换**：这是设计里的核心机制之一，而且它和撤离咬合 —— 撤离要求"全员存活且都在区域内"，
而倒地的人走不了，所以**不救起队友就走不了**。自动复活会让这条规则完全失效。

**为什么先做救援**：撤离、救援、开房间本质是同一件事（站在区域内、条件持续满足、读条 N 秒、
条件断了就重置），所以先做一次原语（`UKBChannelComponent`）用最便宜的场合验证。救援是最便宜的：
它本来就要被替换掉，而且一个人就能触发。

**一个有意的不一致**：救援用**重置**，撤录用**暂停**。凑齐四个人站在同一个圈里很难，暂停才人道；
扶着一个人松手却是一个决定，重启才让这个决定有代价。

### 7. 救助圈在客户端不显示

**症状**（用户实测）：主机上看得到圈，客户端上看不到。读起来像"这个功能没做"，其实是 HUD 找错了地方。

**原因**：`AKBHud::DrawRescueCircles` 用 `World->GetPlayerControllerIterator()` 找玩家。
**在客户端上，这个迭代器只包含本地玩家的 controller** —— 远端玩家的 controller 在那台机器上不存在。
所以客户端只能找到自己的 pawn，永远画不出队友的圈。

**修法**：改成遍历 `AKBGameState::PlayerArray`（复制的，每台机器上都有全部玩家和他们的 pawn）。
同一个坑在别的显示队友状态的地方也会踩，`DrawPartyStatus` 一开始就用了 PlayerArray。

**怎么定位的**：给组件的 `bActive` 加了个 `ReplicatedUsing` 的 OnRep 日志。
"客户端看不到"只有两种可能 —— **标志没复制过去**，或者 **HUD 没找到组件** —— 而这两种从外面看一模一样。
日志里出现了 `channel active replicated -> ACTIVE`，于是排除了前者。**先分清是哪一种，再去改。**

### 8. 客户端开枪没有声音

**症状**（用户实测）：主机的枪有声音，客户端的枪没有。

**原因**：`PlayFireFeedback`（枪口特效 + 开火音）只在 `FireProjectileSpread` 里调用，
而那条路径**只在服务端跑** —— 所以问题比"队友听不到"更严重：**射手自己的客户端也听不到**。
代码注释里早就写着"联机时其他玩家听不到"，但实际情况是连自己都听不到。

**修法**：`UKBWeaponInventoryComponent::MulticastPlayFireFeedback`（Unreliable 多播），
并把调用点从弹道路径**上移到两处"扣扳机"的地方**（`FireAtAcquiredTarget` / `TryFireManualWeapon`），
所以三种投放方式现在都有反馈 —— 冲击波（Radial）以前连枪口特效都没有。

**一个必须记住的取舍：跨网络的用"槽位下标"，不是武器资产指针。**
`UKBWeaponDefinition` 是 DataAsset，**不参与复制**，而 UE 的 RPC 不能传不支持网络的对象 ——
引擎会警告并**发 null**，客户端拿到空指针就静默不播（又是一个"看着接好了但什么都不发生"）。
每个客户端本来就有射手的**复制过的 `Weapons` 数组**，所以传下标就够，各机器自己解出同一份定义。

### 9. 看不到队友的血量

**不是 bug，是没做。** `UKBStatSheetComponent::CurrentHealth` 一直是复制的，数据早就在每台机器上，
只是从来没有任何地方画过它 —— 读数区画的全是"你自己"（等级、金币、你的血条）。

补了 `AKBHud::DrawPartyStatus`：左下角每个队友一行，名字 + 血条。
- **底部对齐**而不是接在读数区下面：读数区的高度是变的（阶段倒计时有无），接在后面东西会跟着跳
- 倒地的队友血条**强制画成空的**，并加一个「倒地」文字标记 —— 血条本身还留着血，但"躺在地上"才是要看的状态
- 用文字而不仅仅是颜色：颜色是色觉差异的人唯一读不到的信号，而"队友倒了"值得比一个色相更多的表达

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
| **虫子真模型 + 腿会动 + 朝向正确** | ✅ 实测（2026-10-04，编辑器烘焙后肉眼看） |
| 每实例动画相位（600 只不同步） | ✅ 实测（日志里各槽位 TimeOffset 互不相同） |
| **死亡贴花的观感** | ⚠️ `M_KBSlime` 已经有 `Opacity` / `BaseColor` 参数，但**淡出与按虫染色是否真的生效没人确认过**（见「未决问题」） |
| 大厅 UI 的**视觉质量**（排版/字体/悬停手感） | ⚠️ **只看过一次，没有细看** |
| 竞技场在联机下的完整玩法 | ⚠️ 只跑到"进图 + swarm 起来"，没打完整局 |
| `AKBGameMode::Logout` 的注释改动 | 无行为变化，仅澄清（原 TODO 是过期的） |
| **虫子移动音**（听得到、有方位感） | ✅ 用户已确认。绑定链路另有日志可证：`bound=6/6`、逐槽 `playing=1` |
| **死亡音效** | ✅ 用户已确认 |
| **子弹换成 Niagara 特效** | ✅ 用户已确认（勾上 Local Space 后正常飞行） |
| **霰弹枪后坐力的力度** | ⚠️ **贴地距离从未实测**。唯一一次测量是在角色**滞空中**开火（见「会浪费时间的坑」），那种情况下没有行走刹车，300 的设定滑出了 627 |
| **受伤震屏** | ✅ 用户已确认（资产 `CS_Damage`，已写进 `DefaultGame.ini`） |
| **大厅加载界面** | ✅ 用户已确认。延迟本身有日志可证（起意 → travel 之间隔了 765 帧） |
| `FX_BugBlast` 的**按虫染色/缩放** | ❌ 该系统**没有暴露任何 User 参数**，C++ 写的 `SplatColor` / `SplatScale` 是空操作，三种虫子的爆炸长得一样 |
| **团灭 → 结算 → 回大厅** | ✅ 实测（无头跑通全链路：倒地 → `Run over (wiped out)` → 12 秒 → `travelling back to the lobby` → 大厅起来并重新认主机） |
| **等级由经验推导** | ✅ 实测（临时把 `XpPerLevel` 调成 5，`xp 15` 得到 `level 3`，与曲线 `5×1 + 5×2 = 15` 完全吻合） |
| 结算界面的**视觉** | ❌ headless 画不出来（`DrawHUD` 在 `-nullrhi` 下不执行），排版和数字对不对需要人看 |
| 结算数字的**数据链路** | ✅ 实测（`EndRun` 会把每个玩家的 `level/xp/gold` 写进日志 —— 加这条日志正是因为界面在无头下看不见） |
| **多人下退回大厅后两边仍在同一房间** | ✅ 实测（双进程窗口测试：回大厅后日志出现 `joined (index 1, 2 player(s))`，客户端重连成功） |
| **无头复现不了的问题：`ServerTravel` 重入** | ⚠️ 双进程窗口测试里 `ReturnToLobby` **同一秒被调了 5 次**（`ServerTravel` 要到帧末才生效，所以 `RunOver` 分支每帧都触发一次）。已加闩锁，并在后来的双进程测试里确认只出现 **1 次** |
| **救援整条链**（倒地 → 圈开 → 队友在圈内 → 读条 → 复活一半血） | ✅ 实测（双进程无头，靠临时把半径/时长调成 800/0.5 才够得着，见下）。日志：`gate OPEN` → `channel complete` → `Character revived by a teammate` |
| **两个人都倒地时救援正确拒绝** | ✅ 实测（日志里 `player 1 at 468 units (IN, downed yes)`，门不开，随即正常团灭） |
| 救助圈的**视觉**（地上的圈、进度） | ✅ 用户已确认（并修掉了"客户端看不到"—— 见本轮问题 7） |
| **客户端能听到自己的枪声** | ✅ 用户已确认。日志另有证据：客户端收到 `FireFeedback: CLIENT for slot 1 (DA_Weapon_Shotgun)`，且**每发一条**、间隔与霰弹枪 0.9s 冷却吻合（没有播两遍） |
| **队友的血条** | ✅ 用户已确认（左下角 `AKBHud::DrawPartyStatus`） |

**无头测不到 UI**：`-nullrhi` 下 `DrawHUD` 不执行。能无头证明的只有数据链路和
`Lobby: click input bound`（绑定存在），画面对不对必须人看。

---

## 环境与构建约定

编辑器**必须关闭**才能编译（Live Coding 互斥锁）。**每次都要先问用户，不要默认直接关** ——
用户从未给过"以后直接关"的长期授权，而编辑器里可能有没保存的东西。关闭前确认进程号，
用 `taskkill /PID`，**不要**用 `/IM`（会连别的 Unreal 进程一起杀掉）。

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
5. **`get_editor_property` 的属性名拼写按类而变。** `UKBEnemyArchetype` / `UKBWeaponDefinition`
   这类 DataAsset 用**蛇形**（`recoil_distance`）；`UKBGameSettings`（UDeveloperSettings）用
   **驼峰**（`ProjectileEffect`）。用错名字报的是
   `Failed to find property '...'`，**看起来像"这个属性根本没加"**，很容易跑去反复检查头文件。
   `Tools/kb_probe_settings_props.py` 能一次列出某个类实际暴露的属性和两种拼法。
   另：某个类的反射生成在 `Intermediate/.../UHT/<Class>.gen.cpp`，**不在** `.generated.h` ——
   去 `.generated.h` 里 grep 新属性什么都找不到，也证明不了任何事。
6. **无头运行的角色出生时还在下落。** `-ExecCmds` 在引擎初始化时执行（地图加载后约 0.3 秒），
   那时 Pawn 已经生成但**还在从出生点往下掉**（实测 Z 183 → 159 → 126 → 90，约 0.4 秒落地）。
   `KB.Debug.PlacePlayer` 帮不上：它把 Z 硬编码成 200，等于是重新放回空中。
   **后果**：任何在 t=0 测量角色位移的测试，量到的都是**滞空滑行**。空中不应用
   `BrakingDecelerationWalking`，所以横向推力会一点不减地滑到落地为止 ——
   霰弹枪后坐力 300 的设定就是这么量出 627 的，而"推力重复施加/刹车常数不对/撞到几何体"
   这些顺理成章的猜测**全是错的**。加一列 Z 一看就明白了。
7. **无头测试进程会占住 DLL。** 跑着 `UnrealEditor-Cmd` 时编译会 `LNK1104`。
   杀进程要按**自己启动的那个**的 PID（后台任务用 `TaskStop`），不要 `/IM UnrealEditor-Cmd.exe`。
8. **Niagara 挂到会动的组件上，粒子不一定跟着走。** 发射器是 World Space 的话，粒子在生成那一刻
   就钉死在世界坐标里，之后移动组件它们不动。症状是"特效只在生成点炸一下"，
   而组件本身是逐帧正确跟随的（用逻辑位置 vs 组件位置对比可以证明）。
   修法在资产（Emitter Properties → Local Space），不在代码。
   Python 读不到这个标志：5.8 的绑定里 `NiagaraSystem` 没有 `emitter_handles`，
   和 `get_exposed_parameters` 一样不可用。记录见 `Tools/kb_probe_niagara_local_space.py`。

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

按"值不值得先做"排，不按发现顺序。

> **接下来做什么见 [`GameDesign.md`](GameDesign.md) 里的分阶段路线图。** 本节只记"哪里还有洞"。

### 会挡住新设计的一件

0. **换图会清空 PlayerState，所以金币目前跨不了局。**
   `bUseSeamlessTravel = false`，每次 travel 之后 Gold/XP/等级全部重建归零。
   而设计里"团灭时金币不掉、金币买局外商店"要求金币**跨局持久化** ——
   所以它必须在回大厅 travel **之前**写进存档。
   `AKBGameMode::ReturnToLobby()` 里已经留了注释标出这个位置。

### 内容缺口（功能已接好，只差资源或一个勾）

1. **武器音效还没填完。** 反馈链路已经修好（多播，三种投放方式都有），
   但只有**步枪和霰弹枪**配了 `MS_Fire`；**冲击波还没有声音**（`DA_Weapon_Shockwave` 里没有声音引用）。
   命中音效（`ImpactSound`）三个武器也都还没配。
2. **`FX_BugBlast` 没有暴露任何 User 参数。** `UKBGoreComponent` 生成后会写 `SplatColor`
   和 `SplatScale`，但该系统里一个用户参数都没有，两次写入都是**静默空操作** ——
   三种虫子的爆炸颜色和大小完全一样。修法是在 Niagara 编辑器里加两个用户参数并接进图。
   判断依据：`grep -a -o -E "User\.[A-Za-z0-9_]+" Content/KillBugs/VFX/FX_BugBlast.uasset` 返回空。
3. **粘液贴花的两个材质参数名对不上（线索，未验证）。**
   `M_KBSlime` 和 `M_KBEnemy` 里存的都是 `BaseColor`（**无空格**），
   而 `KBGoreComponent.cpp:24` 写的是 `Base Color`（**有空格**）。
   如果确实对不上，那 `SlimeColor` 一直是空操作 —— 这正好能解释"三种虫子粘液颜色相同"。
   用 `unreal.MaterialEditingLibrary.get_vector_parameter_names()` 一问便知。
   注意：身体材质的 tint 诊断日志显示读回值匹配，**但那条诊断可能有假阳性**
   （参数不存在时读回缓冲里可能还留着刚写进去的值），所以它不能作为"拼写没问题"的证据。

### 已知但没解释的现象

4. **粘液贴花的深浅不一** —— 战场上有明显偏深和偏浅的两类（浅的偏黄绿、深的偏墨绿）。
   猜测是"贴花重叠叠加"（延迟贴花是混合的，同处两滩深一倍），**但用户认为不是，我也没验证过**。
   能一次定性的检查：找**只有一滩**的地方和另一块**也只有一滩**的地方比颜色 ——
   若两者也不同，则与重叠无关。
   代码侧随机化的只有大小（1.6–2.4×）和朝向，不产生颜色差异。

### 配置无处可调（EditDefaultsOnly 在代码创建的类上不生效）

5. **贴花的数量上限与存活时间**写在 `UKBGoreComponent` 里且无处可改 —— 该组件是代码创建的，
   `EditDefaultsOnly` 不生效。应该像其他设置一样挪进 `UKBGameSettings`。
   同样的坑 `AKBProjectileDirector` 和 `AKBLobbyHud` 也踩过（后者已在注释里记了结论）。
6. **大厅开局的延迟**（`AKBLobbyGameMode::StartTravelDelaySeconds = 0.5f`）是常量，
   想调得重编译。要的话挪进 `UKBGameSettings`。

### 设计取舍，可能想改

7. **飞行物特效是全局的，不是每把枪一份。** 放在 `KBGameSettings::ProjectileEffect`，
   因为放武器上就得跟着 `MulticastPlayShot` 走，那是 Unreliable 的多播、还要多带一个对象引用；
   而放设置里客户端本来就有，零复制开销。要让步枪和霰弹枪长得不一样，需要改多播。
8. **霰弹枪后坐力的贴地距离从未实测过** —— 唯一一次测量是在滞空中开火（见「会浪费时间的坑」6），
   量到 627 而设定是 300。贴地时应该是 300（由 `BrakingDecelerationWalking` 刹停算出），
   但**这是推导，不是测量**。要实测需要在落地之后再开火，目前的无头工具做不到。
9. **主机要不要也按准备？** 现在是"所有人包括主机都要按"。想让主机免按直接开，
   要**同时**改两处，否则按钮和规则会不一致：
   `AKBLobbyHud::DrawButtons` 里 Start 的启用条件 + `AKBLobbyGameMode::RequestStartGame` 的校验。
10. **大厅没有音乐**（用普通 `AGameStateBase`，音频子系统找不到 `AKBGameState` 会静默不播）。
11. **`NextPlayerIndex` 不复用**：退出再加入会拿到更大的序号。只影响排序显示，无正确性问题。
12. **准备状态不跨换图**（新世界新 GameState）。加入者换图进来要重新按准备 —— 这是对的，
    但如果你希望"沿用上一局的准备"，那需要额外做持久化。

---

## 下一步候选

**主线在 [`GameDesign.md`](GameDesign.md) 的路线图里**，当前进度：**阶段 0 已完成**，
下一个是**阶段 1 —— 交互原语（读条/区域），用救援来验证**。

阶段 1 之所以排在搜和撤前面：撤离、救援、开房间是**同一个东西**（站在区域内、条件持续满足、
读条 N 秒、条件断了就重置），做一次三处复用。救援是验证它最便宜的场景 ——
现在的"等下一波自动复活"本来就该换掉。

以下是不在主线上的零散项，有空再收：

- **补完音频**：武器开火/命中音（含把 `PlayFireFeedback` 提到共用位置，顺手修掉冲击波没反馈的问题）
- **给 `FX_BugBlast` 加 User 参数**：两种虫子的爆炸区分开，两分钟的编辑器活儿
- **后坐力贴地距离实测**一次（顺便给无头工具补上"落地后开火"的能力）
- **把 C++ 常量挪进 `UKBGameSettings`**：贴花上限/存活时间、大厅开局延迟
- **联机**：把 `AKBPlayerController::SetAimYaw` 的速率限制补上（代码里标着 Phase 5 TODO）
