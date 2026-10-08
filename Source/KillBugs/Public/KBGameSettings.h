#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "KBGameSettings.generated.h"

class USoundBase;
class UNiagaraSystem;
class UCameraShakeBase;

/**
 * Every global tuning value in one place, editable in Project Settings -> Game -> KillBugs.
 *
 * WHY THIS EXISTS. These values used to be EditDefaultsOnly properties on plain C++ classes
 * (AKBGameMode, AKBEnemyDirector, AKBCharacter). Those classes have no Blueprint subclass, so
 * there was nowhere in the editor to edit them at all - changing the spawn rate meant editing
 * C++ and recompiling the module.
 *
 * DeveloperSettings puts them in Project Settings AND writes them to Config/DefaultGame.ini as
 * plain text, so they can be changed by a designer in the editor, or by editing one line of
 * the ini - no compile, no asset regeneration, no editor session needed.
 *
 * TOOLTIPS ARE WRITTEN IN CHINESE ON PURPOSE. UE shows a property's doc comment as its
 * tooltip, but these are user-facing controls and the people using them read Chinese. Rather
 * than switch the whole file's comments over, each property carries an explicit
 * meta = (ToolTip = "..."); the surrounding // comments stay English for code maintenance.
 * Use // for anything that must NOT become a tooltip.
 *
 * PER-ITEM content (enemy archetypes, weapons, cards) stays as data assets: there are many of
 * them, they are authored individually, and one asset per thing is the right shape for that.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "KillBugs"))
class KILLBUGS_API UKBGameSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UKBGameSettings();

	virtual FName GetCategoryName() const override { return FName(TEXT("Game")); }

	/** Convenience: the CDO of the settings, for reading values. */
	static const UKBGameSettings& Get();

	// =====================================================================================
	// 波次时序
	// =====================================================================================

	UPROPERTY(Config, EditAnywhere, Category = "波次|时序",
		meta = (ToolTip = "开局准备时间（秒）。\n\n不只是给玩家反应时间：客户端的独立进程启动并加载关卡本身就要好几秒，这段时间太短的话，第一个选卡会在新加入玩家的窗口还没就绪时就弹出来，他会看不到卡片。\n每当有玩家加入，这个计时会重新开始。",
			ClampMin = "0.0", UIMin = "0.0", UIMax = "60.0"))
	float WarmupSeconds = 15.f;

	UPROPERTY(Config, EditAnywhere, Category = "波次|时序",
		meta = (ToolTip = "虫潮持续时间（秒）。这期间虫子按速率持续不断地刷，不是刷完一批就停。",
			ClampMin = "5.0", UIMin = "10.0", UIMax = "180.0"))
	float WaveSeconds = 45.f;

	UPROPERTY(Config, EditAnywhere, Category = "波次|时序",
		meta = (ToolTip = "清剿与搜刮时间（秒）。\n\n刻意设得长：虫潮结束【不会】清场，残余虫子会留下来，所以这段时间的前半段是打仗，清干净之后剩下的才是真正的搜刮时间。",
			ClampMin = "10.0", UIMin = "30.0", UIMax = "300.0"))
	float ExploreSeconds = 90.f;

	// =====================================================================================
	// 刷怪压力
	// =====================================================================================

	UPROPERTY(Config, EditAnywhere, Category = "波次|刷怪压力",
		meta = (ToolTip = "第 1 波每秒刷多少只虫。\n\n这是【速率】不是配额——整个虫潮期间持续按这个速度刷。",
			ClampMin = "0.0", UIMin = "0.0", UIMax = "40.0"))
	float BaseSpawnRate = 4.f;

	UPROPERTY(Config, EditAnywhere, Category = "波次|刷怪压力",
		meta = (ToolTip = "每往后一波，每秒多刷多少只。\n\n实际速率 = BaseSpawnRate + 这个值 × 波次序号。",
			ClampMin = "0.0", UIMin = "0.0", UIMax = "20.0"))
	float SpawnRatePerWave = 1.5f;

	UPROPERTY(Config, EditAnywhere, Category = "波次|刷怪压力",
		meta = (ToolTip = "探索期每秒刷多少只。数值很小，只是让地图不至于绝对安全。填 0 可以完全关闭。",
			ClampMin = "0.0", UIMin = "0.0", UIMax = "10.0"))
	float ExploreAmbientSpawnRate = 0.4f;

	UPROPERTY(Config, EditAnywhere, Category = "波次|刷怪压力",
		meta = (ToolTip = "同屏虫子超过这个数量就暂停刷新。防止一次打崩之后局面无限恶化。",
			ClampMin = "10", UIMax = "600"))
	int32 MaxAliveDuringWave = 450;

	UPROPERTY(Config, EditAnywhere, Category = "波次|刷怪压力",
		meta = (ToolTip = "单帧最多刷多少只。防止某一帧突然涌入一大群。",
			ClampMin = "1", UIMax = "64"))
	int32 SpawnBatchSize = 12;

	// =====================================================================================
	// 生成位置
	// =====================================================================================

	UPROPERTY(Config, EditAnywhere, Category = "波次|生成位置",
		meta = (ToolTip = "虫子距离玩家多远生成（厘米）。\n\n【必须大于相机的可视半径】，否则虫子会直接出现在你屏幕里。\n当前相机（FOV 70°、俯角 -65°、距离 1800）大约能看到玩家前方 2000 单位，所以这里取 2400 是安全的。\n如果你调大了 CameraFieldOfView 或 CameraDistance，这个值必须跟着调大。",
			ClampMin = "500.0", UIMax = "8000.0"))
	float MinSpawnDistance = 2400.f;

	UPROPERTY(Config, EditAnywhere, Category = "波次|生成位置",
		meta = (ToolTip = "生成距离的上限。虫子在 Min 和 Max 之间随机取距离。",
			ClampMin = "600.0", UIMax = "10000.0"))
	float MaxSpawnDistance = 3300.0f;

	// =====================================================================================
	// 相位属性缩放
	// =====================================================================================

	UPROPERTY(Config, EditAnywhere, Category = "波次|属性缩放",
		meta = (ToolTip = "虫潮期间虫子【承受伤害】的倍率。1.0 = 用 archetype 资产里配的血量。\n\n调高 = 虫子更脆。",
			ClampMin = "0.05", UIMax = "5.0"))
	float WaveEnemyDamageTakenScale = 1.f;

	UPROPERTY(Config, EditAnywhere, Category = "波次|属性缩放",
		meta = (ToolTip = "虫潮期间虫子的移速倍率。1.0 = 用 archetype 资产里配的速度。",
			ClampMin = "0.05", UIMax = "3.0"))
	float WaveEnemySpeedScale = 1.f;

	UPROPERTY(Config, EditAnywhere, Category = "波次|属性缩放",
		meta = (ToolTip = "探索期间虫子【承受伤害】的倍率，相当于给它们减血。\n\n注意它买不到什么：Grunt 只有 10 血而步枪一枪 12 伤，本来就一枪死，翻倍纯属过杀浪费，只有 Brute 会因此变快清。\n清剿速度真正取决于【每秒击杀数】（武器冷却 × 自动武器数量），不是目标有多硬。如果清剿太慢，改 BaseSpawnRate 或 WaveSeconds 更有效。",
			ClampMin = "0.05", UIMax = "10.0"))
	float ExploreEnemyDamageTakenScale = 3.f;

	UPROPERTY(Config, EditAnywhere, Category = "波次|属性缩放",
		meta = (ToolTip = "探索期间虫子的移速倍率。小于 1 = 更慢，玩家可以甩开它们去搜刮。\n\n这是个手感取舍：虫子慢下来会让清剿拖得更久（从 3300 单位外走回来要十几秒），但换来一个安静的地图。",
			ClampMin = "0.05", UIMax = "3.0"))
	float ExploreEnemySpeedScale = 0.6f;

	// =====================================================================================
	// 接触伤害
	// =====================================================================================

	UPROPERTY(Config, EditAnywhere, Category = "战斗|接触伤害",
		meta = (ToolTip = "虫子贴身多久咬一次（秒）。伤害按这个间隔结算，不按帧。\n\n参考：玩家 100 血、3 只虫同时咬、间隔 0.6 秒时，约能扛 4 秒接触。",
			ClampMin = "0.05", UIMax = "3.0"))
	float ContactDamageInterval = 0.6f;

	UPROPERTY(Config, EditAnywhere, Category = "战斗|接触伤害",
		meta = (ToolTip = "咬到之前，两个身体之间最多允许空多少（厘米）。\n\n判定是「玩家胶囊半径 + 这只虫的 BodyRadius + 这一项」，所以每只虫的接触距离跟着它自己的模型大小走。\n以前这里是一个固定的「中心到中心 110」，和模型无关 —— 结果小虫子在肉眼还空着近半米时就能咬你，\n而 Brute 那种大块头得跟你重叠上才算。\n\n调小 = 要贴得更近才掉血；0 = 必须真的碰上。",
			ClampMin = "0.0", UIMax = "200.0"))
	float ContactGap = 15.f;

	UPROPERTY(Config, EditAnywhere, Category = "战斗|接触伤害",
		meta = (ToolTip = "同一时间最多几只虫能咬同一个玩家。\n\n不设上限的话伤害会随局部密度暴涨——被 40 只虫围住会瞬间秒杀，玩家既来不及反应，也看不出伤害是哪来的。",
			ClampMin = "1", UIMax = "20"))
	int32 MaxSimultaneousContacts = 3;

	// =====================================================================================
	// 子弹表现
	// =====================================================================================

	UPROPERTY(Config, EditAnywhere, Category = "战斗|子弹",
		meta = (ToolTip = "所有子弹的颜色。\n\n目前全局统一，不分武器——弹体共用一个材质实例。要做到每把武器不同颜色，需要给子弹材质加逐实例颜色支持。"))
	FLinearColor ProjectileTint = FLinearColor(1.f, 0.82f, 0.25f, 1.f);

	UPROPERTY(Config, EditAnywhere, Category = "战斗|子弹",
		meta = (ToolTip = "飞行物的特效（Niagara）。\n\n每颗在飞的子弹挂一个该系统并跟着它移动，命中或超程时销毁。\n【留空则退回默认球体】——球体只是占位，不参与碰撞，换成特效不影响命中判定。\n\n注意霰弹一次打 6 颗，会同时有 6 个系统在跑。\n\n组件必须挂在 AKBProjectileDirector 上——因为那个类没有蓝图子类，属性放它身上在编辑器里根本改不了，所以和 ProjectileTint 一样放在这里。"))
	TSoftObjectPtr<UNiagaraSystem> ProjectileEffect;

	// =====================================================================================
	// 救援
	// =====================================================================================

	UPROPERTY(Config, EditAnywhere, Category = "战斗|救援",
		meta = (ToolTip = "救助圈的半径（厘米）。队友进入这个圈才开始读条。\n\n也是倒地玩家在画面上看到的那个圈的半径——代码和显示用的是同一个值，不会对不上。", ClampMin = "50.0", UIMax = "1000.0"))
	float RescueRadius = 220.f;

	UPROPERTY(Config, EditAnywhere, Category = "战斗|救援",
		meta = (ToolTip = "救助需要站多久（秒）。\n\n【中途离开圈要重新计时】——这是有意的，和撤离的“暂停”相反：撤离去凑齐四个人本来就难，\n救一个人却松手就该付代价。", ClampMin = "0.5", UIMax = "30.0"))
	float RescueSeconds = 4.f;

	UPROPERTY(Config, EditAnywhere, Category = "战斗|救援",
		meta = (ToolTip = "被救起来时回复到最大血量的百分之多少。\n\n0.5 = 一半。调高会让倒地的代价变轻。", ClampMin = "0.05", ClampMax = "1.0"))
	float ReviveHealthFraction = 0.5f;

	// =====================================================================================
	// 局末结算
	// =====================================================================================

	UPROPERTY(Config, EditAnywhere, Category = "波次|局末",
		meta = (ToolTip = "一局结束（团灭或撤离成功）后，结算界面停留多久才回大厅。\n\n不能太短：这段时间玩家在读自己这局赚了多少，而且这是全队唯一的“战绩”展示。\n到点后所有人一起回大厅，不能跳过——联机下让某个人按继续会把其他人留在原地。", ClampMin = "3.0", UIMax = "60.0"))
	float RunSummarySeconds = 12.f;

	UPROPERTY(Config, EditAnywhere, Category = "波次|局末",
		meta = (ToolTip = "升级所需的经验基准值。第 N 级升到 N+1 级需要 XpPerLevel × N 点经验，\n所以前几级来得很密、后面越来越慢（总经验 = XpPerLevel × N(N-1)/2）。\n\n调大 = 升级变慢。这会影响卡牌的等级门槛能多早解锁。", ClampMin = "1", UIMax = "2000"))
	int32 XpPerLevel = 100;

	// =====================================================================================
	// 撤离
	// =====================================================================================

	UPROPERTY(Config, EditAnywhere, Category = "撤离|区域",
		meta = (ToolTip = "撤离圈的半径（厘米）。全员存活且都在这个圈里，撤离计时才开始走。\n\n和救助圈一样，代码判定和画面上的圈用的是同一个值。\n实测 350 太挤：四人加虫潮，一被推就把谁挤出圈，进度反复归零。现在 700（直径 14 米）。", ClampMin = "50.0", UIMax = "3000.0"))
	float ExtractionRadius = 700.f;

	UPROPERTY(Config, EditAnywhere, Category = "撤离|时序",
		meta = (ToolTip = "【撤离计时】全员都在圈里时，要站多久才算撤离成功（秒）。\n\n这段时间里虫子会大规模涌来，所以它是一个守卫战的长度，不是走路的长度。", ClampMin = "1.0", UIMax = "180.0"))
	float ExtractionSeconds = 30.f;

	UPROPERTY(Config, EditAnywhere, Category = "撤离|时序",
		meta = (ToolTip = "【开启时间】撤离点从出现到关闭的总时长（秒）。\n\n它只在“不是全员都在圈里”的时候走；全员进圈时它会冻结，把时间让给撤离计时。\n走完 = 这次撤离失败，撤离点关闭，这一局继续，过 N 波再出现。\n\n【这两个钟互斥，所以整段撤离最多占用 开启时间 + 撤离计时 秒。】", ClampMin = "10.0", UIMax = "600.0"))
	float ExtractionOpenWindowSeconds = 60.f;

	UPROPERTY(Config, EditAnywhere, Category = "撤离|时序",
		meta = (ToolTip = "撤离波次的 Explore 阶段在时序上多留的余量（秒）。\n\n撤离点只在 Explore 阶段出现，而这一阶段的时长会被撑到至少“开启时间 + 撤离计时 + 这个余量”，\n保证整段撤离不会跨到下一个阶段去。", ClampMin = "0.0", UIMax = "60.0"))
	float ExtractionPhaseTailSeconds = 5.f;

	UPROPERTY(Config, EditAnywhere, Category = "撤离|排期",
		meta = (ToolTip = "撤离点第一次出现在第几波（玩家看到的波次编号，从 1 数）。\n\n3 = 第 3 波的 Explore 阶段出现。", ClampMin = "1", UIMax = "50"))
	int32 ExtractionFirstWave = 3;

	UPROPERTY(Config, EditAnywhere, Category = "撤离|排期",
		meta = (ToolTip = "多久出现一次（以波计）。撤离失败之后也按这个间隔重新排期。\n\n调小 = 机会更多、整局压力更大。", ClampMin = "1", UIMax = "50"))
	int32 ExtractionWaveInterval = 3;

	UPROPERTY(Config, EditAnywhere, Category = "撤离|压力",
		meta = (ToolTip = "【全员进圈读条】期间，刷怪速率取这个值（而不是 Explore 的涓涓细流）。\n\n只在读条时生效：跑过去的那段路、以及有人出圈暂停时，都回到 Explore 的平静。\n\n【这个值要对着玩家的清怪速度定，不是对着波次强度】——步枪 0.55 秒一发，\n清怪上限约 1.8 只/秒；速率定成 12 时场上每秒净增约 10 只，30 秒的读条必然守不住，\n实测就是这么输的。现在 6/秒，净增约 4 只/秒，靠走位和地形才打得动。", ClampMin = "0.0", UIMax = "60.0"))
	float ExtractionSpawnRate = 6.f;

	UPROPERTY(Config, EditAnywhere, Category = "撤离|压力",
		meta = (ToolTip = "读条期间场上虫子上限。\n\n【不能超过 虫群|MaxEnemies（默认 600）】—— 超了会被静默夹回来，因为增虫的地方会按那一个上限再夹一次。", ClampMin = "10", ClampMax = "2000", UIMax = "600"))
	int32 ExtractionAliveCap = 520;

	UPROPERTY(Config, EditAnywhere, Category = "撤离|压力",
		meta = (ToolTip = "【玩家开启撤离的那一刻】在视野外撒这么多虫子，作为回应。\n\n每个撤离点只给一次（第一次全员进圈时），不是每次进出圈都给——\n否则反复进出就能无限刷虫。", ClampMin = "0", UIMax = "300"))
	int32 ExtractionOpeningBurst = 30;

	UPROPERTY(Config, EditAnywhere, Category = "撤离|区域",
		meta = (ToolTip = "候选点离竞技场边缘至少留出的距离（厘米）。\n\n保证整个撤离圈落在墙内，不然会出现“圈有一半在墙外面”的情况。\n改成生成式迷宫之后，候选点会换成房间中心，这个值就只在找落点时还有意义。", ClampMin = "0.0", UIMax = "3000.0"))
	float ExtractionEdgeMargin = 600.f;

	// =====================================================================================
	// 经济
	// =====================================================================================

	UPROPERTY(Config, EditAnywhere, Category = "经济",
		meta = (ToolTip = "一局最多能带几把武器。\n\n【两个地方要它】：局内的武器栏上限，和商店里“最多能配装几把”。\n以前它在武器组件上（EditDefaultsOnly），但代码创建的组件在编辑器里改不到，\n而商店也要用同一个数——所以挪到这里，只留一份。", ClampMin = "1", ClampMax = "12"))
	int32 MaxWeaponSlots = 6;

	UPROPERTY(Config, EditAnywhere, Category = "经济",
		meta = (ToolTip = "卖出一个材料换多少金币。\n\n这是【全局】汇率：它和具体哪把武器无关，所以放在这里而不是武器资产上。\n\n注意目前只有“材料→金币”这一个方向（没有用金币买材料），所以不存在套利循环。\n以后真要加“买材料”，买入价必须高于这个卖出价。", ClampMin = "1", UIMax = "1000"))
	int32 GoldPerMaterial = 5;

	// =====================================================================================
	// 搜刮
	// =====================================================================================

	UPROPERTY(Config, EditAnywhere, Category = "搜刮|拾取",
		meta = (ToolTip = "走到离掉落物多近才算捡到（厘米）。\n\n这是【从玩家中心到掉落物中心】的距离，不是净空——玩家胶囊半径约 34，掉落物约 30，\n所以 100 大约是“踩上去”，而 300 是“从旁边路过就吸走”。\n实际半径还要乘玩家自己的拾取倍率（卡牌「远见」加的就是那个倍率）。\n拾取是【走过去自动发生】的，没有按键。", ClampMin = "0.0", UIMax = "3000.0"))
	float BasePickupRadius = 90.f;

	UPROPERTY(Config, EditAnywhere, Category = "搜刮|拾取",
		meta = (ToolTip = "药包用一次回多少血。\n\n回血上限是玩家的最大血量，超出的部分不会浪费在溢出上（Heal 会自己钳）。\n\n注意：药包【不再是踩到就回血】，它进了背包，由按键使用；满血时使用会被拒绝（不消耗）。", ClampMin = "1.0", UIMax = "500.0"))
	float MedkitHealAmount = 40.f;

	// =====================================================================================
	// 背包
	// =====================================================================================

	UPROPERTY(Config, EditAnywhere, Category = "搜刮|背包",
		meta = (ToolTip = "背包的重量上限。\n\n材料按 MaterialUnitWeight 计重、药包按 MedkitWeight 计重，加起来不能超过这个数。\n\n标尺参照：实测一局能搜出约 225 个材料，所以默认值留了余量；而场上掉落上限是 MaxDrops，\n一场饱和的战场光地上的东西就接近上限——所以它也不是永远够用的摆设。", ClampMin = "0", UIMax = "5000"))
	int32 BackpackCapacity = 300;

	UPROPERTY(Config, EditAnywhere, Category = "搜刮|背包",
		meta = (ToolTip = "每个材料占多少重量。", ClampMin = "0", UIMax = "100"))
	int32 MaterialUnitWeight = 1;

	UPROPERTY(Config, EditAnywhere, Category = "搜刮|背包",
		meta = (ToolTip = "每个药包占多少重量。\n\n默认 25 = “一个药包约等于 25 个材料”，也就是 225 材料 + 3 药包正好装满 300。\n这就是搜刮的取舍：多带一个药包，就得少带一把材料。", ClampMin = "0", UIMax = "500"))
	int32 MedkitWeight = 25;

	UPROPERTY(Config, EditAnywhere, Category = "搜刮|背包",
		meta = (ToolTip = "从背包里丢出去的东西落在离玩家多远的地方（厘米）。\n\n【必须大于玩家能加成的最大拾取半径】，否则丢在脚下、下一帧就被自己捡回来，\n玩家看到的是“丢弃没生效”。拾取半径 BasePickupRadius=90，卡牌「远见」能乘到 135。",
			ClampMin = "0.0", UIMax = "1000.0"))
	float DroppedItemDistance = 160.f;

	UPROPERTY(Config, EditAnywhere, Category = "搜刮|掉落",
		meta = (ToolTip = "场上同时最多存在多少个掉落物。到顶之后不再掉新的。\n\n【不做“回收最旧的”】：在玩家眼皮底下、还够得着的地方把战利品删掉，比不掉新的更糟。", ClampMin = "8", UIMax = "1024"))
	int32 MaxDrops = 128;

	UPROPERTY(Config, EditAnywhere, Category = "搜刮|掉落",
		meta = (ToolTip = "掉落物离地面多高（厘米）。\n\n抬起来一点，免得和地面共面、从俯视角度看不出来。", ClampMin = "0.0", UIMax = "300.0"))
	float DropHeightOffset = 35.f;

	UPROPERTY(Config, EditAnywhere, Category = "搜刮|掉落",
		meta = (ToolTip = "掉落物的显示缩放。\n\n先用引擎基础体当占位，真正的美术资源还没做。", ClampMin = "0.05", UIMax = "10.0"))
	float DropScale = 0.6f;

	UPROPERTY(Config, EditAnywhere, Category = "搜刮|掉落",
		meta = (ToolTip = "材料在地面上长什么样。\n\n【留空会回落到引擎自带的方块】并打一条警告——功能是好的但看不见才是最难查的那种 bug。"))
	TSoftObjectPtr<UStaticMesh> MaterialDropMesh;

	UPROPERTY(Config, EditAnywhere, Category = "搜刮|掉落",
		meta = (ToolTip = "药包在地面上长什么样。留空的处理同上。"))
	TSoftObjectPtr<UStaticMesh> MedkitDropMesh;

	/**
	 * The two colours, applied as a dynamic material instance on the drop mesh.
	 *
	 * The engine's placeholder cube uses BasicShapeMaterial, which exposes a `Color` parameter,
	 * so this works with no art at all. A real drop mesh will need a material with the same
	 * parameter name - if it does not have one, the tint is silently skipped and both drop types
	 * look identical, which is why the code checks for the parameter and says so in the log
	 * rather than assuming.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "搜刮|掉落",
		meta = (ToolTip = "掉落物的材质。\n\n【留空时有回退链】：先用网格自带的材质；如果那个材质没有名为 Color 的向量参数\n（引擎那个方块自带的 WorldGridMaterial 就没有），就回退到引擎的 BasicShapeMaterial 并打一条日志。\n\n真美术资源来了之后，要么把材质留空、让它的材质带一个 Color 参数，要么在这里直接指定。"))
	TSoftObjectPtr<UMaterialInterface> DropMaterial;

	UPROPERTY(Config, EditAnywhere, Category = "搜刮|掉落",
		meta = (ToolTip = "材料的颜色（琥珀色：看着像“资源”）。"))
	FLinearColor MaterialDropTint = FLinearColor(0.86f, 0.62f, 0.20f, 1.f);

	UPROPERTY(Config, EditAnywhere, Category = "搜刮|掉落",
		meta = (ToolTip = "药包的颜色（绿色：一眼认出是“回血”）。"))
	FLinearColor MedkitDropTint = FLinearColor(0.28f, 0.82f, 0.44f, 1.f);

	// =====================================================================================
	// 受伤反馈
	// =====================================================================================

	/**
	 * Camera shake played on the machine of the player who was hurt.
	 *
	 * UNSET by default, and this is the hook for one: create a UCameraShakeBase asset and drop it
	 * in here (Project Settings -> Game -> KillBugs, or the matching key in
	 * Config/DefaultGame.ini). Nothing else needs changing - UKBStatSheetComponent already looks
	 * this up on every hit.
	 *
	 * A class rather than an instance, because that is what ClientStartCameraShake takes and the
	 * shake is spawned per hit rather than held.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "战斗|受伤反馈",
		meta = (ToolTip = "玩家受伤时的摄像机震动（UCameraShakeBase 类）。\n\n【留空则完全不震】—— 本工程目前没有任何震屏资产，这一项就是给你挂资产的位置。\n在 Project Settings › Game › KillBugs › 战斗|受伤反馈 里设置，或直接写进 DefaultGame.ini。\n\n挂上之后不需要改任何代码：受伤的收口在 UKBStatSheetComponent::ApplyDamage。"))
	TSubclassOf<UCameraShakeBase> PlayerDamageCameraShake;

	/**
	 * Multiplier on the shake above, so intensity can be dialled without editing the asset.
	 *
	 * Separate from the asset's own scale on purpose: "a bit less" is the single most common
	 * tweak to a hit shake, and doing it here is an ini edit rather than opening the editor.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "战斗|受伤反馈",
		meta = (ToolTip = "上面那个震动的强度倍率。\n\n存在意义是「太强了，调小一点」不用去改资产——改这个数字就行，不用重编译。\n0 = 关闭（等同于留空）。", ClampMin = "0.0", UIMax = "3.0"))
	float PlayerDamageShakeScale = 1.f;

	// =====================================================================================
	// 死亡表现
	// =====================================================================================

	UPROPERTY(Config, EditAnywhere, Category = "战斗|死亡",
		meta = (ToolTip = "虫子死后留在地上的粘液贴花材质。\n\n【必须是 Deferred Decal 域的材质】，否则贴上去什么都不显示。材质需要暴露两个参数：\n  Opacity（标量）—— 池子靠它做淡出，没有这个参数贴花就永远不消失\n  Base Color（向量）—— 按原型染色（见 KBEnemyArchetype::SlimeColor）\n\n全原型共用这一份材质，靠颜色区分，理由和虫子身体共用 M_KBEnemy 一样。"))
	TSoftObjectPtr<UMaterialInterface> SlimeDecalMaterial;

	// =====================================================================================
	// 虫群容量
	// =====================================================================================

	UPROPERTY(Config, EditAnywhere, Category = "虫群",
		meta = (ToolTip = "模拟虫子的硬上限，同时也是网络复制数组的容量。调大会显著增加带宽和 CPU。",
			ClampMin = "10", UIMax = "2000"))
	int32 MaxEnemies = 600;

	UPROPERTY(Config, EditAnywhere, Category = "虫群",
		meta = (ToolTip = "竞技场半边长（厘米）。\n\n必须和 Tools/kb_setup_arena.py 生成的地图一致，改这里要同时改脚本里的 FLOOR_HALF_SIZE 并重新生成地图。",
			ClampMin = "500.0", UIMax = "20000.0"))
	float ArenaHalfExtent = 5500.f;

	UPROPERTY(Config, EditAnywhere, Category = "虫群",
		meta = (ToolTip = "流场网格的格边长（厘米）。\n\n要和迷宫的走廊宽度对齐：格子比走廊宽，虫子就找不到路。\n代价是平方关系——5500 的半边长配 100 的格子是 1.1 万个，配 50 就是 4.8 万个。",
			ClampMin = "20.0", UIMax = "500.0"))
	float FlowCellSize = 100.f;

	UPROPERTY(Config, EditAnywhere, Category = "虫群",
		meta = (ToolTip = "流场重建的间隔（秒）。\n\n玩家以 700/秒移动时 0.1 秒走 70 厘米，不到一格，所以起点格永远新鲜。\n调大省 CPU，但玩家绕到墙另一边之后，虫子会晚一点才改道。",
			ClampMin = "0.02", UIMax = "2.0"))
	float FlowRebuildInterval = 0.1f;

	// =====================================================================================
	// 玩家
	// =====================================================================================

	UPROPERTY(Config, EditAnywhere, Category = "玩家",
		meta = (ToolTip = "玩家初始生命上限。卡片可以在其上叠加。",
			ClampMin = "1.0", UIMax = "2000.0"))
	float BaseMaxHealth = 100.f;

	UPROPERTY(Config, EditAnywhere, Category = "玩家",
		meta = (ToolTip = "玩家基础移动速度。卡片可以按倍率放大。",
			ClampMin = "100.0", UIMax = "3000.0"))
	float BaseMoveSpeed = 700.f;

	UPROPERTY(Config, EditAnywhere, Category = "玩家",
		meta = (ToolTip = "联机大厅的房间容量。同时是会话的 NumPublicConnections 和大厅界面上显示的 x/N。\n\n设计上是 4 人各自一个客户端（见 DefaultEngine.ini 的 bUseSplitscreen 说明），所以这个值不是分屏人数。",
			ClampMin = "1", ClampMax = "16"))
	int32 MaxPlayers = 4;

	// =====================================================================================
	// 选卡
	// =====================================================================================

	UPROPERTY(Config, EditAnywhere, Category = "选卡",
		meta = (ToolTip = "每次弹出几张卡供选择。",
			ClampMin = "1", ClampMax = "5"))
	int32 CardsPerDraft = 3;

	UPROPERTY(Config, EditAnywhere, Category = "选卡",
		meta = (ToolTip = "选卡时限（秒）。超时后服务端会【随机】给玩家一张，不会给第一张——否则第一张会变成隐性默认选项，玩家会摸到这个规律。",
			ClampMin = "5.0", UIMax = "300.0"))
	float DraftSeconds = 60.f;

	UPROPERTY(Config, EditAnywhere, Category = "选卡",
		meta = (ToolTip = "卡片弹出后的输入保护时间（秒）。\n\n这段时间内卡片不显示、点击无效。因为卡片很可能在玩家正在连点开火时弹出，不加保护就会替他做出选择，而他根本没看到选项。",
			ClampMin = "0.0", UIMax = "5.0"))
	float DraftInputLockSeconds = 1.f;

	// =====================================================================================
	// 音频
	//
	// Asset references live here rather than on a class property so they can be swapped by
	// editing Config/DefaultGame.ini - no compile, no editor session. Unset means silent,
	// never a crash.
	// =====================================================================================

	/** Sustained bed played during a wave. */
	UPROPERTY(Config, EditAnywhere, Category = "音频|背景音乐",
		meta = (ToolTip = "虫潮期间循环播放的紧张底噪。\n\n推荐用 SoundMorph 包里的 Weather/Presets/Thunder_Rain_Wind_*（暴风雨声，和虫潮很搭）。\n留空则该阶段无音乐。"))
	TSoftObjectPtr<USoundBase> WaveMusic;

	/** Sustained bed played while exploring. */
	UPROPERTY(Config, EditAnywhere, Category = "音频|背景音乐",
		meta = (ToolTip = "探索期间循环播放的平和底噪。\n\n同样推荐 Weather/Presets/ 下的音源，选一个比 WaveMusic 平缓的。\n留空则该阶段无音乐。"))
	TSoftObjectPtr<USoundBase> ExploreMusic;

	UPROPERTY(Config, EditAnywhere, Category = "音频|背景音乐",
		meta = (ToolTip = "背景音乐音量（0~1）。",
			ClampMin = "0.0", ClampMax = "1.0"))
	float MusicVolume = 0.45f;

	UPROPERTY(Config, EditAnywhere, Category = "音频|背景音乐",
		meta = (ToolTip = "切换阶段时音乐交叉淡入淡出的时长（秒）。0 = 瞬间切换。",
			ClampMin = "0.0", UIMax = "10.0"))
	float MusicCrossfadeSeconds = 2.5f;

	UPROPERTY(Config, EditAnywhere, Category = "音频|界面",
		meta = (ToolTip = "鼠标悬停到卡片上时的音效。"))
	TSoftObjectPtr<USoundBase> CardHoverSound;

	UPROPERTY(Config, EditAnywhere, Category = "音频|界面",
		meta = (ToolTip = "选中卡片时的音效。"))
	TSoftObjectPtr<USoundBase> CardPickSound;

	UPROPERTY(Config, EditAnywhere, Category = "音频|界面",
		meta = (ToolTip = "卡牌弹出时的音效。\n\n注意：它和输入保护同时开始，所以玩家听到声音时还不能点，这是刻意的。"))
	TSoftObjectPtr<USoundBase> CardDraftOpenSound;

	UPROPERTY(Config, EditAnywhere, Category = "音频|界面",
		meta = (ToolTip = "界面通用音量（0~1）。",
			ClampMin = "0.0", ClampMax = "1.0"))
	float UiVolume = 0.7f;

	// =====================================================================================
	// 相机
	// =====================================================================================

	UPROPERTY(Config, EditAnywhere, Category = "相机",
		meta = (ToolTip = "俯视角度。度数越接近 -90 越接近垂直俯视，-45 是较平的三分之四视角。",
			ClampMin = "-89.0", ClampMax = "-20.0"))
	float CameraPitch = -65.f;

	UPROPERTY(Config, EditAnywhere, Category = "相机",
		meta = (ToolTip = "相机沿摇臂后退的距离。调大 = 视野更远、虫子更小；调大后必须同时调大 MinSpawnDistance。",
			ClampMin = "100.0", UIMax = "6000.0"))
	float CameraDistance = 1800.f;

	UPROPERTY(Config, EditAnywhere, Category = "相机",
		meta = (ToolTip = "相机视野角度（FOV）。默认值 70 比引擎默认的 90 窄，是刻意收窄的。\n\n90 度时相机能看到玩家前方约 3700 单位的地面——几乎整个场地——那样就没有任何地方可以让虫子在屏幕外生成。70 度把它压到约 2000。\n调大 FOV 必须同时调大 MinSpawnDistance，否则虫子会重新出现在屏幕里。",
			ClampMin = "40.0", ClampMax = "120.0"))
	float CameraFieldOfView = 70.f;
};

/**
 * Shorthand for the project's tuning values, used at the point of use rather than cached.
 *
 * Declared inline in the header rather than as a helper in each .cpp: Unreal builds with unity
 * by default, which concatenates several .cpp files into one translation unit, so an identical
 * anonymous-namespace helper in two files is a redefinition error.
 */
FORCEINLINE const UKBGameSettings& KBSettings()
{
	return UKBGameSettings::Get();
}
