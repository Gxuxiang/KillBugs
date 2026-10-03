# 虫子的模型与动画

虫子模型由用户提供（`Content/KillBugs/Mesh/bug/`），动画走的是**顶点动画贴图（VAT /
Vertex Animation Textures）**：把骨骼动画烘焙成贴图，材质在顶点着色器里按贴图移动顶点。

本文记录**能走通的那条路**，以及一路上必须补的坑 —— 每一个坑都会以"看起来成功了"的
方式失败，所以值得写下来。

> 相关：`Docs/Lobby.md`、`Docs/Status.md`

---

## 为什么必须用 VAT

虫群是**实例化绘制**的：每个原型一个 `UInstancedStaticMeshComponent`，600 只虫子一次
draw call。**实例化静态网格不能播放骨骼动画** —— 这不是配置问题，是渲染路径本身的限制。

VAT 的答案是把动画烘成贴图，材质用 **WPO（World Position Offset）** 把顶点推到正确位置，
于是"有动画的虫子"仍然只是一个静态网格。

---

## 正确的烘焙流程（在编辑器里做）

**用插件自带的示例蓝图 `BP_AnimToTexture`，走骨骼模式。** 这是插件设计的工作流。

产物（都在 `Content/KillBugs/Mesh/bug/VAT/`）：

| 资产 | 作用 |
|---|---|
| `SM_bug` | 承载动画的静态网格 —— **虫群画的就是它** |
| `DA_BoneAnimation_bug` | 烘焙数据：帧数、采样率、骨骼贴图引用 |
| `TX_bonePosition` / `TX_boneRotation` / `TX_boneWeight` | 烘焙出的贴图 |
| `M_bug_bone` / `M_bug_bone_Inst` | 材质与实例 —— **实例才是虫群用的那个** |

重新烘焙：改完动画后重跑 `BP_AnimToTexture`，再 **Save All**。

---

## 三个必补的坑

### 1. 材质必须有 `UsedWithInstancedStaticMeshes`

**症状：虫子没有材质**（引擎默认灰材质），或者在带棋盘格的构建上看起来像没贴图。

实例化组件**渲染不了**这个标志为 false 的材质，引擎会**静默换成默认材质** ——
不报错、不警告。在编辑器里点开材质看是完全正常的，只有实例化组件渲染时才暴露。

**这是本工程重复踩的坑**：`M_KBEnemy` 当初就是为此单独建的，而每做一个新材质都会再踩一遍。

`Tools/kb_fix_vat_material_ism.py` 处理这个：设标志 → `recompile_material` → 保存。
（用法标志是编译期分支，不重编不生效。）**标志在基材质上，不在材质实例上。**

### 2. 材质需要打开静态开关 `AutoPlay` 和对应的 `UseUV*`

**症状：虫子完全不动**，但网格、缩放、颜色都正常。

插件的动画函数链是：

```
MF_VertexAnimation -> GetFrameSwitch -> { GetAutoPlayFrame  ← 逐实例
                                         GetFrame          ← 统一参数
```

`GetFrameSwitch` 就是用来选这两条的，所以选择权在一个**静态开关参数**上。默认走统一参数那条，
而**本项目没有任何东西去驱动那些统一参数**，结果就是完全静止。

同时：烘焙把逐帧查找写进某个 UV 通道，材质有 `UseUV0..3` 四个开关，**必须打开与烘焙一致的那个**。

基材质的开关全集（供参考）：
```
PlasticOverride, UseUV0, UseUV1, UseUV2, UseUV3, AutoPlay, UseDynamicParameters
```

### 3. 模型的正面朝向 —— `MeshYawOffset`

**症状：虫子倒着爬。**

虫群用 `FRotator(0, Yaw, 0)` 把每个实例转向它移动的方向，这**假定模型朝 +X**。
本项目的虫子模型朝 **-X**，所以原型上要设 `MeshYawOffset = 180`。

这件事**代码无法自动判断** —— 一个虫子的包围盒看不出哪边是头。换成新模型时如果发现横着走或
倒着走，改这个值。

---

## 绘制侧（C++）

`UKBEnemyVisualizerComponent` 做三件与 VAT 相关的事：

1. **每实例动画相位**。`FAnimToTextureAutoPlayData`（`TimeOffset, PlayRate, StartFrame,
   EndFrame`）正好 4 个 float，写进 HISM 的 `NumCustomDataFloats = 4`。
   **`TimeOffset` 随机化是这个机制存在的全部理由** —— 没有它，600 只虫子整齐划一地同步摆腿，
   看起来就是一台机器而不是一群虫子。

2. **撑开包围盒**。动画在**顶点着色器**里移动顶点，网格自身的包围盒完全不知道这件事，
   不撑开的话虫子会在屏幕边缘被视锥剔除、**凭空消失**。用 `SetBoundsScale`。
   （插件自己的 `SetBoundsExtensions` 没有 `UFUNCTION` 宏，Python 够不着。）

3. **朝向偏移**，见上面第 3 条。

原型侧：`AnimData` 字段指向烘焙数据资产 —— **绘制时就要它**，帧数和采样率是算相位的依据。

---

## 为什么没有用脚本烘焙

`Tools/kb_setup_bug_vat.py` 那条路线**走不通**，结论记在这里免得重试：

- **骨骼模式会让编辑器直接崩。** 它要写一张骨骼权重贴图，而那张贴图**只有编辑器的属性变更
  路径才会创建**，脚本驱动时是空的，插件 `check(Texture)` 断言 →
  `Assertion failed: Texture [AnimToTextureUtils.h:161]`。所以脚本被迫改用顶点模式。
- **顶点模式要求你预先准备两张贴图。** `VertexPositionTexture` / `VertexNormalTexture` 在
  整个插件里**只被读取、没有任何创建代码** —— 烘焙是往贴图里**写像素**的。没有贴图时写入被
  静默跳过，而函数**照样返回 true**。
- **`NumDriverTriangles` 默认是 10**（插件的示例值，给人形模型用的）。烘焙据此建一个极简代理
  网格并**用它的顶点数决定贴图尺寸** —— 2992 个顶点的虫子只烘出 32×32 的贴图，
  ≈1% 的顶点在动，肉眼看就是"没动"。要设成和网格自身三角面数相当。
- **无头无法验证。** 每一步的失败都是"返回值正常但什么都没发生"，而我不能渲染，
  所以只能靠往返问你 —— 这条路线的效率极低。

**结论：用编辑器的 `BP_AnimToTexture`。** 它绕开了第一条，其余几条在编辑器里都有可见的反馈。

---

## 怎么判断动画对不对

无头跑不了画面，但**数据侧可以测**：`KBEnemyVisualizerComponent::LogAnimationData()`
会把每个实例的 `TimeOffset` 打进日志（挂在 `KB.Swarm.PerfLog` 的间隔上）。

```
Swarm anim | mesh=SM_bug | customFloats=4 | instances=15 | animData=yes | boundsScale=2.00
    slot 0 TimeOffset = 0.2679
    slot 1 TimeOffset = 0.2903      ← 各不相同 = 相位生效
    slot 2 TimeOffset = 0.3370
```

这份日志**把问题一分为二**：数据对了但画面不动 → 材质侧；数据是零或全都一样 → C++ 侧。
这条线当初就是靠它定位的。

---

## 已知未做

- **死亡贴花还没有材质**（`M_KBSlime`）。贴花池已经建好、位置也对，但没有材质时引擎给的是
  默认贴花材质 —— 看起来是**一个发光的大方块**。需要按 `UKBGameSettings::SlimeDecalMaterial`
  的说明做一个 Deferred Decal 材质。
- **贴花的数量上限与存活时间写在 C++ 里且无处可改**（gore 组件是代码创建的，
  `EditDefaultsOnly` 不生效）。应该挪到 `UKBGameSettings`。
