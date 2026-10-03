#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "KBGameSettings.generated.h"

class USoundBase;

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
		meta = (ToolTip = "虫子中心离玩家多近算贴身（厘米）。",
			ClampMin = "10.0", UIMax = "1000.0"))
	float ContactRange = 110.f;

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
		meta = (ToolTip = "经验/掉落物的拾取半径。目前拾取系统还没接，这个值先留着。",
			ClampMin = "0.0", UIMax = "3000.0"))
	float BasePickupRadius = 300.f;

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
