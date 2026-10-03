#include "Audio/KBAudioSubsystem.h"

#include "Components/AudioComponent.h"
#include "KBConsoleVariables.h"
#include "KBGameSettings.h"
#include "KillBugs.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"

namespace
{
	/** One-shot with no spatialisation; UI and music are both heard the same everywhere. */
	void PlayOneShot(const UObject* WorldContext, const TSoftObjectPtr<USoundBase>& Sound, float Volume)
	{
		if (!WorldContext || Sound.IsNull())
		{
			return;
		}

		if (USoundBase* Loaded = Sound.LoadSynchronous())
		{
			UGameplayStatics::PlaySound2D(WorldContext, Loaded, Volume);
		}
	}
}

void UKBAudioSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const UKBGameSettings& Settings = KBSettings();
	if (Settings.WaveMusic.IsNull() && Settings.ExploreMusic.IsNull())
	{
		UE_LOG(LogKillBugs, Warning,
			TEXT("No music configured. Set WaveMusic / ExploreMusic in Project Settings -> ")
			TEXT("Game -> KillBugs, or in Config/DefaultGame.ini."));
	}
}

void UKBAudioSubsystem::Deinitialize()
{
	if (WaveMusicComponent)
	{
		WaveMusicComponent->Stop();
	}
	if (ExploreMusicComponent)
	{
		ExploreMusicComponent->Stop();
	}

	Super::Deinitialize();
}

TStatId UKBAudioSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UKBAudioSubsystem, STATGROUP_Tickables);
}

void UKBAudioSubsystem::EnsureMusicComponents()
{
	const UKBGameSettings& Settings = KBSettings();

	// bAutoDestroy=false: these are beds that must outlive the call that started them, and
	// they are crossfaded by volume rather than stopped and restarted.
	if (!WaveMusicComponent && !Settings.WaveMusic.IsNull())
	{
		if (USoundBase* Sound = Settings.WaveMusic.LoadSynchronous())
		{
			WaveMusicComponent = UGameplayStatics::SpawnSound2D(
				this, Sound, 0.f, 1.f, 0.f, nullptr, /*bPersistAcrossLevelTransition*/ false,
				/*bAutoDestroy*/ false);
		}
	}

	if (!ExploreMusicComponent && !Settings.ExploreMusic.IsNull())
	{
		if (USoundBase* Sound = Settings.ExploreMusic.LoadSynchronous())
		{
			ExploreMusicComponent = UGameplayStatics::SpawnSound2D(
				this, Sound, 0.f, 1.f, 0.f, nullptr, false, false);
		}
	}
}

void UKBAudioSubsystem::KeepAlive(UAudioComponent* Component)
{
	if (!Component)
	{
		return;
	}

	// A one-shot source plays its envelope and stops, which would leave the run silent after
	// the first minute. Restarting it is not seamless, but silence is worse - and the log line
	// marks the asset as a poor choice for a bed so it can be swapped.
	if (!Component->IsPlaying())
	{
		Component->Play();

		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogKillBugs, Warning,
				TEXT("A music track stopped on its own and was restarted. It is probably a ")
				TEXT("one-shot rather than a sustaining bed; pick a looping source."));
		}
	}
}

void UKBAudioSubsystem::ApplyPhase(EKBWavePhase Phase)
{
	// Everything except the wave itself counts as "calm": explore, the draft, warmup and the
	// run-over screen are all non-combat.
	const bool bIntense = (Phase == EKBWavePhase::WaveActive);

	TargetWaveVolume = bIntense ? 1.f : 0.f;
	TargetExploreVolume = bIntense ? 0.f : 1.f;

	UE_LOG(LogKillBugs, Display, TEXT("Music: switching to the %s bed"),
		bIntense ? TEXT("wave") : TEXT("explore"));
}

void UKBAudioSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	const UKBGameSettings& Settings = KBSettings();

	EnsureMusicComponents();

	// Poll the phase rather than subscribing: the subsystem can outlive a GameState, and a
	// missed delegate binding would leave the music stuck on one bed forever.
	if (const UWorld* World = GetWorld())
	{
		if (const AKBGameState* RunState = World->GetGameState<AKBGameState>())
		{
			const EKBWavePhase Phase = RunState->GetWavePhase();
			if (!bPhaseInitialised || Phase != LastPhase)
			{
				bPhaseInitialised = true;
				LastPhase = Phase;
				ApplyPhase(Phase);
			}
		}
	}

	// Linear crossfade. A constant rate rather than an easing curve keeps the two beds summing
	// to a roughly constant loudness through the transition.
	const float Step = Settings.MusicCrossfadeSeconds > KINDA_SMALL_NUMBER
		? DeltaTime / Settings.MusicCrossfadeSeconds
		: 1.f;

	WaveVolume = FMath::FInterpConstantTo(WaveVolume, TargetWaveVolume, Step, 1.f);
	ExploreVolume = FMath::FInterpConstantTo(ExploreVolume, TargetExploreVolume, Step, 1.f);

	if (WaveMusicComponent)
	{
		if (WaveVolume > KINDA_SMALL_NUMBER)
		{
			KeepAlive(WaveMusicComponent);
		}
		WaveMusicComponent->SetVolumeMultiplier(WaveVolume * Settings.MusicVolume);
	}

	if (ExploreMusicComponent)
	{
		if (ExploreVolume > KINDA_SMALL_NUMBER)
		{
			KeepAlive(ExploreMusicComponent);
		}
		ExploreMusicComponent->SetVolumeMultiplier(ExploreVolume * Settings.MusicVolume);
	}
}

void UKBAudioSubsystem::PlayCardHover()
{
	PlayOneShot(this, KBSettings().CardHoverSound, KBSettings().UiVolume * 0.6f);
}

void UKBAudioSubsystem::PlayCardPick()
{
	PlayOneShot(this, KBSettings().CardPickSound, KBSettings().UiVolume);
}

void UKBAudioSubsystem::PlayCardDraftOpen()
{
	PlayOneShot(this, KBSettings().CardDraftOpenSound, KBSettings().UiVolume);
}

bool UKBAudioSubsystem::IsWaveMusicPlaying() const
{
	return WaveMusicComponent && WaveMusicComponent->IsPlaying();
}

bool UKBAudioSubsystem::IsExploreMusicPlaying() const
{
	return ExploreMusicComponent && ExploreMusicComponent->IsPlaying();
}

// ---------------------------------------------------------------------------------------
// Debug: KB.Audio.Status
//
// "Is the music actually sounding?" cannot be heard in a headless run, and it is the one
// thing that decides whether a chosen asset works as a bed at all. This reports it.
// ---------------------------------------------------------------------------------------

static void KBConsoleAudioStatus(const TArray<FString>& Args, UWorld* World)
{
	if (!World)
	{
		return;
	}

	const UKBAudioSubsystem* Audio = World->GetSubsystem<UKBAudioSubsystem>();
	if (!Audio)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("AudioStatus: no audio subsystem"));
		return;
	}

	const UKBGameSettings& Settings = KBSettings();

	UE_LOG(LogKillBugs, Display,
		TEXT("AudioStatus: wave playing=%s vol=%.2f | explore playing=%s vol=%.2f"),
		Audio->IsWaveMusicPlaying() ? TEXT("yes") : TEXT("NO"),
		Audio->GetWaveVolume(),
		Audio->IsExploreMusicPlaying() ? TEXT("yes") : TEXT("NO"),
		Audio->GetExploreVolume());

	UE_LOG(LogKillBugs, Display,
		TEXT("AudioStatus: waveAsset=%s exploreAsset=%s"),
		Settings.WaveMusic.IsNull() ? TEXT("UNSET") : *Settings.WaveMusic.ToString(),
		Settings.ExploreMusic.IsNull() ? TEXT("UNSET") : *Settings.ExploreMusic.ToString());
}

static FAutoConsoleCommandWithWorldAndArgs KBConsoleAudioStatusCommand(
	TEXT("KB.Audio.Status"),
	TEXT("KB.Audio.Status - report whether the music beds are actually playing."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBConsoleAudioStatus));
