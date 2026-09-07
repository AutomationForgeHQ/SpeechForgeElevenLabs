// The module that carries ElevenLabs into SpeechForge.

#pragma once

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

class FElevenLabsProvider;

SPEECHFORGEELEVENLABS_API DECLARE_LOG_CATEGORY_EXTERN(LogSpeechForgeElevenLabs, Log, All);

/**
 * Registers the ElevenLabs provider with SpeechForge and nothing else.
 *
 * This plugin used to be a folder inside the core, which made SpeechForge's settings default to a
 * vendor by name and its subsystem register one on startup. Extracted, the core registers nothing,
 * defaults to nothing, and a project that wants ElevenLabs enables this plugin - the same shape as
 * FaceForgeACE, MotionForgeKimodo and MeshForgeCloud, and the reason all of them can be swapped
 * without touching a core.
 */
class FSpeechForgeElevenLabsModule : public IModuleInterface
{
public:

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:

	TSharedPtr<FElevenLabsProvider> Provider;
};
