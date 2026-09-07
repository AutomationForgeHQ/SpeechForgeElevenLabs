// ElevenLabs, the benchmark provider.

#pragma once

#include "CoreMinimal.h"
#include "ISpeechProvider.h"

/**
 * ElevenLabs as an ISpeechProvider.
 *
 * The first implementation, and deliberately the paid one - the same reason motion started on a paid
 * API. It establishes what good sounds like, so that everything after it can be measured rather than
 * argued about.
 *
 * Four things about this API are worth knowing before reading the code, because each one shaped it:
 *
 *   No job.       Synthesis returns audio in the response. There is nothing to poll.
 *   Money at submit. Billing is per character of input, charged the moment the request is made, so
 *                 a failed generation is still a paid one and the estimate has to be exact.
 *   Timings free. The /with-timestamps endpoint costs the same as plain synthesis and returns
 *                 character-level alignment, so the plain endpoint is never called.
 *   History.      Every generation gets a request id and can be re-fetched free, forever. On a model
 *                 whose seed is only approximate, that is the only durable route back to a take.
 *
 * Measured facts that no document states, and that the code depends on:
 *
 *   48 kHz WAV is allowed on a mid-tier account and **44.1 kHz is not**. A gated format is refused
 *   outright with a named tier rather than being silently downgraded.
 */
class SPEECHFORGEELEVENLABS_API FElevenLabsProvider : public ISpeechProvider
{
public:

	static const FName ProviderId;

	virtual FName GetProviderId() const override { return ProviderId; }
	virtual FString GetDisplayName() const override { return TEXT("ElevenLabs"); }
	virtual FSpeechProviderCaps GetCaps() const override;

	virtual FString GetDefaultModelId() const override { return TEXT("eleven_v3"); }
	virtual int32 GetCharacterLimit(const FString& ModelId) const override;
	virtual FString GetAudioFormat() const override { return TEXT("wav"); }

	/**
	 * False for the expressive model, which rejects `previous_text` outright.
	 *
	 * Measured, not read: `eleven_v3` returns `unsupported_model` when given stitching context, while
	 * `eleven_multilingual_v2` accepts it. So the two capabilities this pipeline most wants -
	 * inline direction and continuous prosody - are on different models, and choosing between them
	 * is a real per-bank decision rather than a setting anybody can default correctly.
	 */
	virtual bool SupportsStitchingForModel(const FString& ModelId) const override;

	/**
	 * Fold direction into the text as an inline audio tag.
	 *
	 * This is the whole reason Direction is a separate field: the model wants "[sighs] ...damn it."
	 * and the player must read "...damn it." Merging happens here, at the last possible moment, and
	 * only for a provider that can actually use it.
	 */
	virtual FString BuildRequestText(const FString& Text, const FString& Direction) const override;

	virtual FText GetOwningPluginName() const override
	{
		return NSLOCTEXT("SpeechForgeElevenLabs", "Owner", "SpeechForge ElevenLabs");
	}

	virtual FString GetCredentialServiceName() const override { return TEXT("ElevenLabs"); }
	virtual FString GetCredentialHelpUrl() const override { return TEXT("https://elevenlabs.io/app/settings/api-keys"); }
	virtual bool HasCredential() const override;

	// Where voices come from on this provider. The Voice Library is the community catalogue - adding
	// a voice there puts it in My Voices, which is what ListVoices returns beside the stock set.
	// Designing a voice from a description and cloning from samples live in the same app.
	virtual FString GetVoiceLibraryUrl() const override { return TEXT("https://elevenlabs.io/app/voice-library"); }
	virtual FText GetVoiceLibraryHint() const override
	{
		return NSLOCTEXT("SpeechForgeElevenLabs", "VoiceLibraryHint",
			"Want more voices? Browse the ElevenLabs Voice Library and press \"Add\" on any voice - "
			"it lands in My Voices and appears here after Fetch. Designing a voice from a "
			"description or cloning one from recordings lives in the same app, under Voices.");
	}

	virtual void Synthesize(const FSpeechSynthesisRequest& Request, FOnSpeechSynthesized OnComplete) override;

	/**
	 * Speech to speech: the performance stays, the identity changes.
	 *
	 * Needs the **Speech to Speech** permission on the key - a key without it fails as HTTP 401
	 * naming missing_permissions, which is worth knowing before blaming the audio.
	 */
	virtual void ConvertSpeech(const FSpeechConversionRequest& Request, FOnSpeechSynthesized OnComplete) override;

	/**
	 * Dubbing: the same performance in another language, voice and pacing preserved.
	 *
	 * A real job on the provider's side, unlike everything else here - submit, poll, download -
	 * hidden inside this one call as the interface promises. Needs the **Dubbing** permission on
	 * the key; without it the submit fails as HTTP 401 naming missing_permissions. The audio
	 * arrives as MP3 - that is what the dubbing endpoint renders - and imports fine.
	 */
	virtual void DubSpeech(const FSpeechDubbingRequest& Request, FOnSpeechSynthesized OnComplete) override;

	virtual void RefetchById(
		const FString& RequestId,
		const FString& AbsoluteOutputPath,
		FOnSpeechSynthesized OnComplete) override;

	virtual void TestConnection(FOnSpeechTestComplete OnComplete) override;
	virtual void ListVoices(FOnSpeechVoicesListed OnComplete) override;

private:

	/** Build a request with the api key header attached, or return null when there is no key. */
	TSharedPtr<class IHttpRequest, ESPMode::ThreadSafe> MakeRequest(const FString& Url, const FString& Verb) const;

	/** The output_format token for a sample rate, e.g. 48000 -> "wav_48000". */
	static FString MakeOutputFormat(int32 SampleRate);

	/**
	 * Coerce voice settings into what a model will actually accept.
	 *
	 * The expressive model takes stability only at discrete steps and rejects anything between them,
	 * which is the kind of thing that turns a whole batch into 422s an hour after it looked fine.
	 */
	static void ApplyModelQuirks(const FString& ModelId, TSharedRef<class FJsonObject> VoiceSettings, float Stability);

	/** Pull the character timings out of a with-timestamps response. */
	static bool ParseAlignment(
		const TSharedPtr<class FJsonObject>& Root,
		const FString& DisplayText,
		FSpeechAlignment& OutAlignment,
		FString& OutError);

	/** Decode base64 audio, write it, and read the real sample rate back off the header. */
	static bool WriteAudio(
		const FString& Base64,
		const FString& AbsolutePath,
		int32& OutSampleRate,
		FString& OutError);

	/** Sample rate from a RIFF/WAVE header, walking chunks rather than assuming 44 bytes. */
	static int32 ReadWavSampleRate(const TArray<uint8>& Bytes);

	/** Turn an ElevenLabs error body into something a reader can act on. */
	static FString DescribeError(int32 HttpCode, const FString& Body);
};
