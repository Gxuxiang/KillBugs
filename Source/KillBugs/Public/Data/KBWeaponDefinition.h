#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "KBWeaponDefinition.generated.h"

class UNiagaraSystem;
class USoundBase;

/**
 * Who pulls the trigger.
 *
 *  Auto   - the weapon fires itself on cooldown at whatever it acquires. The player only
 *           positions. This is the survivors-like default and is fully server-driven.
 *  Manual - the player aims and holds fire. Client sends an aim point, server validates it.
 */
UENUM(BlueprintType)
enum class EKBWeaponBehavior : uint8
{
	Auto   UMETA(DisplayName = "Auto (fires itself)"),
	Manual UMETA(DisplayName = "Manual (player aims)")
};

/** How a shot resolves once it is fired. */
UENUM(BlueprintType)
enum class EKBWeaponDelivery : uint8
{
	/** Instant, single target - damages the nearest acquired bug the moment it fires. */
	Hitscan UMETA(DisplayName = "Hitscan (instant)"),

	/**
	 * A bullet that travels and damages what it actually reaches.
	 *
	 * Preferred over Hitscan for anything the player aims: a target that moves out of the way
	 * is missed, which is the point of a projectile being visible at all.
	 */
	Projectile UMETA(DisplayName = "Projectile (travelling bullet)"),

	/** Instant, area - damages every bug within RadialRadius of the aim point. */
	Radial  UMETA(DisplayName = "Radial burst (area)")
};

/**
 * A weapon. Everything a designer tunes lives here; adding a weapon is a new asset, not code.
 *
 * Damage and cooldown grow linearly with level so cards can level a weapon up without a
 * separate asset per level.
 */
UCLASS(BlueprintType)
class KILLBUGS_API UKBWeaponDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FText DisplayName;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FText Description;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Behaviour")
	EKBWeaponBehavior Behavior = EKBWeaponBehavior::Auto;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Behaviour")
	EKBWeaponDelivery Delivery = EKBWeaponDelivery::Hitscan;

	// ---- Stats --------------------------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats")
	float BaseDamage = 12.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats")
	float DamagePerLevel = 6.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats", meta = (ClampMin = "0.05"))
	float BaseCooldown = 0.9f;

	/** Negative: weapons speed up as they level. Floored in GetCooldown. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats")
	float CooldownPerLevel = -0.06f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats", meta = (ClampMin = "1"))
	int32 MaxLevel = 8;

	/** Acquisition range. Also the length of the manual-fire ray. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats", meta = (ClampMin = "1"))
	float Range = 1400.f;

	// ---- Delivery -----------------------------------------------------------------------

	/** Radial delivery only. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats", meta = (ClampMin = "1"))
	float RadialRadius = 250.f;

	/** How close a line must pass to a bug to count as a hit, for hitscan and manual fire. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats", meta = (ClampMin = "1"))
	float HitRadius = 60.f;

	/**
	 * Projectile delivery only.
	 *
	 * Slow enough to be dodgeable is the point - at 2600 a bullet crosses its whole range in
	 * half a second and the travel is barely visible, which defeats having one at all.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats", meta = (ClampMin = "100"))
	float ProjectileSpeed = 2600.f;

	/**
	 * Projectiles launched per trigger pull. Above 1 this becomes a spread weapon.
	 *
	 * GetDamage() is the PER-PELLET damage, so a six-pellet shotgun at 13 damage hits for 78
	 * if every pellet connects - the cone is what makes that uncertain.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats", meta = (ClampMin = "1", ClampMax = "32"))
	int32 ProjectilesPerShot = 1;

	/** Total cone width in degrees. 0 fires every projectile straight ahead. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float SpreadDegrees = 0.f;

	// ---- Presentation -------------------------------------------------------------------
	//
	// All of these are actually played - see UKBWeaponInventoryComponent (fire) and
	// AKBProjectileDirector (impact). A field here that nothing reads is worse than no field
	// at all: it looks configured and does nothing, a mistake this project has made twice
	// already (first Tint, then TracerColor).

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "表现|弹道",
		meta = (ToolTip = "这把武器的主题色。\n\n目前实际用途只有冲击波范围圈的绘制（调试用）。子弹本身的颜色由 KBGameSettings 里的 ProjectileTint 统一控制——要让每把武器有不同弹色，需要给子弹材质加逐实例颜色支持。"))
	FLinearColor TracerColor = FLinearColor(1.f, 0.85f, 0.3f);

	// ---- Feel --------------------------------------------------------------------------

	/**
	 * How far firing pushes the SHOOTER backwards, in cm. 0 disables recoil entirely.
	 *
	 * The pawn, not the camera: this game's camera is a fixed top-down boom with
	 * absolute rotation, so a "camera kick" would mean sliding the whole view, which reads as
	 * the world moving rather than as the gun pushing back. Moving the character is the honest
	 * version, and it is the shooter who should feel it.
	 *
	 * Per weapon on purpose. Recoil is what makes a heavy weapon read as heavy, and a value
	 * shared across every gun would make the shotgun and the rifle feel identical.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "手感|后坐力",
		meta = (ToolTip = "后坐力：开火时把玩家自己朝瞄准的反方向推多远（厘米）。0 = 关闭。\n\n推的是角色本体（服务端施力，自动复制），不是相机——本作相机是锁死的顶视，晃相机等于整个画面在动。\n\n这是【贴地时】的距离，由角色的 BrakingDecelerationWalking 刹停算出。\n按住移动键时推不到这么远，那个“推不动”是对的，没有补偿。\n【腾空开火会滑得远得多】：空中没有行走刹车，实测 300 的设定在跳跃中会滑出 627，落地才停。", ClampMin = "0.0", UIMax = "600.0"))
	float RecoilDistance = 0.f;

	/** Played at the muzzle on every shot. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "表现|开火",
		meta = (ToolTip = "开火音效。每次扣扳机时在枪口位置播放（一次扣扳机播一次，不是每颗弹丸各播一次）。\n\n目前只在开火的那台机器上播放（单机没问题）。联机时其他玩家听不到，这个要等 Phase 5 用复制的开火事件补齐。"))
	TSoftObjectPtr<USoundBase> FireSound;

	/**
	 * Pitch multiplier applied to FireSound.
	 *
	 * Exists so one sound asset can serve several weapons: a shotgun playing the same shot at
	 * 0.7 reads as a heavier gun rather than as a different one. Cheaper than needing a
	 * separate asset per weapon, and the fastest way to A/B a sound that is close but not
	 * quite right.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "表现|开火",
		meta = (ToolTip = "【开火音效】的音高倍率（命中音效不受影响）。\n\n1.0 = 原音；小于 1 更低沉厚重（适合霰弹），大于 1 更尖锐轻快（适合速射武器）。\n用途是让同一份素材在几把武器上听起来不一样，省一套素材。",
			ClampMin = "0.25", ClampMax = "4.0"))
	float FireSoundPitch = 1.f;

	/** Niagara system spawned at the muzzle on every shot. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "表现|开火",
		meta = (ToolTip = "枪口特效（Niagara）。每次开火在枪口位置生成一次。\n\n留空则只有子弹实体、没有枪口火光。"))
	TSoftObjectPtr<UNiagaraSystem> MuzzleEffect;

	/** Played where a bullet actually lands. Never fires on a miss. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "表现|命中",
		meta = (ToolTip = "命中音效。子弹打到虫子时在命中点播放。打空不播。"))
	TSoftObjectPtr<USoundBase> ImpactSound;

	/** Niagara system spawned where a bullet lands. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "表现|命中",
		meta = (ToolTip = "命中特效（Niagara）。子弹打到虫子时在命中点生成。\n\n注意霰弹一次打 6 颗弹丸，近距离全中的话会同时炸开 6 个特效。嫌乱可以留空，只靠子弹消失来表达命中。"))
	TSoftObjectPtr<UNiagaraSystem> ImpactEffect;

	// ---- Classification -----------------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "分类",
		meta = (ToolTip = "武器标签。供卡片的前置条件筛选使用（比如「仅限手动武器」的卡）。"))
	FGameplayTagContainer WeaponTags;

	float GetDamage(int32 Level) const
	{
		return BaseDamage + DamagePerLevel * FMath::Max(0, Level - 1);
	}

	float GetCooldown(int32 Level) const
	{
		return FMath::Max(0.05f, BaseCooldown + CooldownPerLevel * FMath::Max(0, Level - 1));
	}

	// ---- Economy --------------------------------------------------------------------------
	//
	// Per-weapon numbers live on the weapon, the way the stat curves above already do. The one
	// value that is NOT per-weapon - materials to gold - is a global setting instead.

	/**
	 * Gold to own this weapon.
	 *
	 * ZERO MEANS NOT FOR SALE, not free. A free default is indistinguishable from a designer who
	 * forgot to fill it in, and a weapon that costs nothing is a hole rather than a feature. An
	 * unpriced weapon simply does not appear in the shop.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "经济",
		meta = (ClampMin = "0", UIMax = "100000",
			ToolTip = "买下这把武器要多少金币。\n\n【0 表示不出售】——不是免费。免费和“忘了填”从外面看一模一样，\n而未标价的武器干脆不出现在商店里。"))
	int32 BuyPriceGold = 0;

	/** Materials for the first upgrade (level 1 -> 2). Zero means the weapon cannot be crafted. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "经济",
		meta = (ClampMin = "0", UIMax = "10000",
			ToolTip = "从 1 级升到 2 级要多少材料。【0 表示不可合成】。"))
	int32 BaseUpgradeCost = 0;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "经济",
		meta = (ClampMin = "0", UIMax = "10000",
			ToolTip = "每升一级，材料成本增加多少。\n\n和伤害曲线同一个道理：越往后越贵，所以升级是个要选的投入，不是刷材料就能填满的东西。"))
	int32 UpgradeCostPerLevel = 0;

	/** Materials to take this weapon from CurrentLevel to CurrentLevel + 1. */
	int32 GetUpgradeCost(int32 CurrentLevel) const
	{
		return BaseUpgradeCost + UpgradeCostPerLevel * FMath::Max(0, CurrentLevel - 1);
	}

	bool IsForSale() const { return BuyPriceGold > 0; }

	bool IsCraftableAt(int32 CurrentLevel) const
	{
		return CurrentLevel < MaxLevel && GetUpgradeCost(CurrentLevel) > 0;
	}
};
