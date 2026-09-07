#include "SpeechForgeElevenLabs.h"

#include "ElevenLabsProvider.h"
#include "SpeechForge.h"


DEFINE_LOG_CATEGORY(LogSpeechForgeElevenLabs);

void FSpeechForgeElevenLabsModule::StartupModule()
{
	// GetPtr loads SpeechForge rather than looking it up. A .uplugin dependency guarantees SpeechForge
	// is enabled, not that its module started first, so a lookup here works on some runs and returns
	// null on others - and the failure mode is a provider that silently never appears.
	if (FSpeechForgeModule* SpeechForge = FSpeechForgeModule::GetPtr())
	{
		Provider = MakeShared<FElevenLabsProvider>();
		SpeechForge->RegisterProvider(Provider.ToSharedRef());
	}
	else
	{
		UE_LOG(LogSpeechForgeElevenLabs, Error,
			TEXT("SpeechForgeElevenLabs could not load the SpeechForge module, so the ElevenLabs "
			     "provider is not available. Check that the SpeechForge plugin is enabled."));
	}

}

void FSpeechForgeElevenLabsModule::ShutdownModule()
{
	// GetPtrIfLoaded, never GetPtr: loading a module during teardown to tell it something is being
	// torn down would be worse than doing nothing.
	if (FSpeechForgeModule* SpeechForge = FSpeechForgeModule::GetPtrIfLoaded())
	{
		SpeechForge->UnregisterProvider(FElevenLabsProvider::ProviderId);
	}

	Provider.Reset();
}

IMPLEMENT_MODULE(FSpeechForgeElevenLabsModule, SpeechForgeElevenLabs)
