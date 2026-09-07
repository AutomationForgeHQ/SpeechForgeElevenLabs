#include "ElevenLabsProvider.h"

#include "SpeechForgeElevenLabs.h"
#include "ElevenLabsVoiceChanger.h"

#include "SpeechForge.h"
#include "SpeechCredentialStore.h"
#include "SpeechForgeSettings.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"

const FName FElevenLabsProvider::ProviderId = TEXT("ElevenLabs");

namespace ElevenLabs
{
	static const TCHAR* ApiRoot = TEXT("https://api.elevenlabs.io/v1");

	/** Models this provider knows the limits of. Anything else falls back to the smallest. */
	static int32 CharacterLimitFor(const FString& ModelId)
	{
		// Read off the live /v1/models endpoint rather than the docs, which disagree with it.
		static const TMap<FString, int32> Limits =
		{
			{ TEXT("eleven_v3"),              5000  },
			{ TEXT("eleven_multilingual_v2"), 10000 },
			{ TEXT("eleven_flash_v2_5"),      40000 },
			{ TEXT("eleven_turbo_v2_5"),      40000 },
			{ TEXT("eleven_flash_v2"),        30000 },
			{ TEXT("eleven_turbo_v2"),        30000 },
		};

		if (const int32* Found = Limits.Find(ModelId))
		{
			return *Found;
		}

		// The conservative answer, so an unknown model splits a long line rather than failing it.
		return 5000;
	}
}

FSpeechProviderCaps FElevenLabsProvider::GetCaps() const
{
	FSpeechProviderCaps Caps;

	Caps.ProviderId   = ProviderId;
	Caps.DisplayName  = GetDisplayName();

	// 48000 rather than the more obvious 44100, and this is measured rather than assumed: 44.1 kHz
	// output is gated to a higher subscription tier where 48 kHz is not. Nothing documents that, and
	// it is the difference between needing the expensive plan and not.
	Caps.NativeSampleRate = 48000;

	Caps.BillingUnit  = ESpeechBillingUnit::Characters;
	Caps.bIsMetered   = true;

	Caps.bSupportsSeed = true;

	// The API takes a seed and the model does not honour it exactly - the same text and seed come
	// back subtly different. Saying so here is what stops anything downstream treating a regenerated
	// line as identical to the one it replaced.
	Caps.bSeedIsBestEffort = true;

	Caps.bSupportsAlignment      = true;

	// Speech to speech, billed by the duration of the audio rather than by character - which is why
	// this is its own flag and not something BillingUnit could have answered.
	Caps.bSupportsVoiceConversion = true;

	// Dubbing: a recording carried into another language with its voice and pacing intact. Billed
	// by the minute, at a multiple of synthesis - estimate before batching anything.
	Caps.bSupportsDubbing = true;

	Caps.bSupportsStitching      = true;
	Caps.bSupportsDirection      = true;
	Caps.bSupportsVoiceListing   = true;
	Caps.bSupportsRemoteHistory  = true;

	// A property of the subscription tier rather than of the code, and exceeding it queues rather
	// than erroring - so a batch that ignores this is simply slow with nothing in the log to say why.
	// Five is the mid-tier limit for the quality models; the fast ones allow more.
	Caps.MaxConcurrentRequests = 5;

	Caps.bNeedsCredential = true;

	if (!HasCredential())
	{
		Caps.SetupHint = FString::Printf(
			TEXT("No API key. Add one in Project Settings > Plugins > SpeechForge, or set the %s ")
			TEXT("environment variable. Signing in is a human action - there is no tool for it."),
			*FSpeechCredentialStore::GetEnvironmentVariableName(GetCredentialServiceName()));
	}

	return Caps;
}

int32 FElevenLabsProvider::GetCharacterLimit(const FString& ModelId) const
{
	return ElevenLabs::CharacterLimitFor(ModelId.IsEmpty() ? GetDefaultModelId() : ModelId);
}

bool FElevenLabsProvider::SupportsStitchingForModel(const FString& ModelId) const
{
	const FString Resolved = ModelId.IsEmpty() ? GetDefaultModelId() : ModelId;

	// eleven_v3 rejects previous_text and next_text with unsupported_model. It is the model with the
	// audio tags, so the project's two most wanted features are on opposite models - direction here,
	// prosody continuity there. Named rather than worked around, because it is a design decision per
	// bank and not a bug to hide.
	return Resolved != TEXT("eleven_v3");
}

FString FElevenLabsProvider::BuildRequestText(const FString& Text, const FString& Direction) const
{
	if (Direction.IsEmpty())
	{
		return Text;
	}

	FString Trimmed = Direction;
	Trimmed.TrimStartAndEndInline();

	// Accept both "[whispers]" and "whispers" so nobody has to remember which. Written direction is
	// prose to a human and syntax to the model, and the field should not punish either reading.
	if (!Trimmed.StartsWith(TEXT("[")))
	{
		Trimmed = FString::Printf(TEXT("[%s]"), *Trimmed);
	}

	return FString::Printf(TEXT("%s %s"), *Trimmed, *Text);
}

bool FElevenLabsProvider::HasCredential() const
{
	return FSpeechCredentialStore::Has(GetCredentialServiceName());
}

TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> FElevenLabsProvider::MakeRequest(
	const FString& Url, const FString& Verb) const
{
	FString Key;
	if (!FSpeechCredentialStore::Get(GetCredentialServiceName(), Key))
	{
		return nullptr;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(Verb);
	Request->SetHeader(TEXT("xi-api-key"), Key);
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));

	if (const USpeechForgeSettings* Settings = USpeechForgeSettings::Get())
	{
		Request->SetTimeout(static_cast<float>(Settings->RequestTimeoutSeconds));
	}

	// Not held anywhere. The key lives on the request and dies with it.
	Key.Empty();

	return Request;
}

FString FElevenLabsProvider::MakeOutputFormat(int32 SampleRate)
{
	return FString::Printf(TEXT("wav_%d"), SampleRate);
}

void FElevenLabsProvider::ApplyModelQuirks(
	const FString& ModelId, TSharedRef<FJsonObject> VoiceSettings, float Stability)
{
	if (ModelId == TEXT("eleven_v3"))
	{
		// The expressive model accepts stability only at three discrete steps and rejects anything
		// in between with a 422. Snapping is friendlier than refusing, because the value came off a
		// float slider that had no way of knowing.
		const float Steps[] = { 0.f, 0.5f, 1.f };
		float Best = Steps[0];
		float BestDistance = FMath::Abs(Stability - Steps[0]);

		for (const float Step : Steps)
		{
			const float Distance = FMath::Abs(Stability - Step);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = Step;
			}
		}

		if (!FMath::IsNearlyEqual(Best, Stability))
		{
			UE_LOG(LogSpeechForgeElevenLabs, Verbose,
				TEXT("eleven_v3 takes stability at 0.0, 0.5 or 1.0 only - snapped %.2f to %.1f."),
				Stability, Best);
		}

		VoiceSettings->SetNumberField(TEXT("stability"), Best);
		return;
	}

	VoiceSettings->SetNumberField(TEXT("stability"), Stability);
}

int32 FElevenLabsProvider::ReadWavSampleRate(const TArray<uint8>& Bytes)
{
	if (Bytes.Num() < 44)
	{
		return 0;
	}

	auto FourCC = [&Bytes](int32 Offset)
	{
		return FString::Printf(TEXT("%c%c%c%c"),
			Bytes[Offset], Bytes[Offset + 1], Bytes[Offset + 2], Bytes[Offset + 3]);
	};

	if (FourCC(0) != TEXT("RIFF"))
	{
		return 0;
	}

	// Walk the chunks rather than assuming a 44-byte header: encoders are free to insert LIST or
	// fact chunks before fmt, and a fixed offset reads whatever happens to be there.
	int32 Pos = 12;
	while (Pos + 8 < Bytes.Num())
	{
		const FString Id = FourCC(Pos);
		const uint32 Size =
			  static_cast<uint32>(Bytes[Pos + 4])
			| static_cast<uint32>(Bytes[Pos + 5]) << 8
			| static_cast<uint32>(Bytes[Pos + 6]) << 16
			| static_cast<uint32>(Bytes[Pos + 7]) << 24;

		if (Id == TEXT("fmt "))
		{
			const int32 RateOffset = Pos + 12;
			if (RateOffset + 4 > Bytes.Num())
			{
				return 0;
			}

			return static_cast<int32>(
				  static_cast<uint32>(Bytes[RateOffset])
				| static_cast<uint32>(Bytes[RateOffset + 1]) << 8
				| static_cast<uint32>(Bytes[RateOffset + 2]) << 16
				| static_cast<uint32>(Bytes[RateOffset + 3]) << 24);
		}

		Pos += 8 + static_cast<int32>(Size) + (Size % 2);
	}

	return 0;
}

bool FElevenLabsProvider::WriteAudio(
	const FString& Base64, const FString& AbsolutePath, int32& OutSampleRate, FString& OutError)
{
	TArray<uint8> Audio;
	if (!FBase64::Decode(Base64, Audio) || Audio.Num() == 0)
	{
		OutError = TEXT("The response carried audio that did not decode as base64.");
		return false;
	}

	if (!FFileHelper::SaveArrayToFile(Audio, *AbsolutePath))
	{
		OutError = FString::Printf(TEXT("Could not write audio to '%s'."), *AbsolutePath);
		return false;
	}

	// Read the rate back off the bytes that arrived rather than trusting what was asked for. Cheap,
	// and it is the only thing that would catch a provider substituting a format rather than
	// refusing one.
	OutSampleRate = ReadWavSampleRate(Audio);
	return true;
}

bool FElevenLabsProvider::ParseAlignment(
	const TSharedPtr<FJsonObject>& Root,
	const FString& DisplayText,
	FSpeechAlignment& OutAlignment,
	FString& OutError)
{
	const TSharedPtr<FJsonObject>* Block = nullptr;

	// "alignment" indexes the text as written. "normalized_alignment" indexes it after the model has
	// expanded numbers and abbreviations, and the two differ in length only when that actually fired
	// - which is why reading the wrong one is correct in every test that avoided digits. We want the
	// one that matches what the player reads.
	if (!Root->TryGetObjectField(TEXT("alignment"), Block) || !Block || !Block->IsValid())
	{
		OutError = TEXT("The response carried no alignment. The plain endpoint was called by mistake.");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Characters = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Starts = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Ends = nullptr;

	if (!(*Block)->TryGetArrayField(TEXT("characters"), Characters)
		|| !(*Block)->TryGetArrayField(TEXT("character_start_times_seconds"), Starts)
		|| !(*Block)->TryGetArrayField(TEXT("character_end_times_seconds"), Ends))
	{
		OutError = TEXT("The alignment block was missing one of its three arrays.");
		return false;
	}

	FString Joined;
	Joined.Reserve(Characters->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Characters)
	{
		Joined += Value->AsString();
	}

	OutAlignment.Text = Joined;
	OutAlignment.bFromNormalizedText = false;

	OutAlignment.CharacterStartSeconds.Reset(Starts->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Starts)
	{
		OutAlignment.CharacterStartSeconds.Add(static_cast<float>(Value->AsNumber()));
	}

	OutAlignment.CharacterEndSeconds.Reset(Ends->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Ends)
	{
		OutAlignment.CharacterEndSeconds.Add(static_cast<float>(Value->AsNumber()));
	}

	if (!OutAlignment.Validate(OutError))
	{
		return false;
	}

	// ---------------------------------------------------------------------------------------------
	// Re-base the timings onto the text the player actually reads.
	//
	// The provider aligns against the string it was sent, which has the direction tag on the front -
	// so "[exhausted] Holding is..." comes back with twelve characters of timing that correspond to
	// nothing on screen. Keeping the field separate from the subtitle and then handing back timings
	// indexed to the merged string walks the same bug in through a side door: every subtitle
	// consumer would be off by the length of the tag, and only for lines that carry direction.
	//
	// Absolute times are kept. The first real word genuinely does start late, because the model
	// spends audio on the tag - and CharacterStartSeconds[0] is then the honest answer to "when does
	// the line start speaking", which a subtitle wants anyway.
	// ---------------------------------------------------------------------------------------------
	if (!DisplayText.IsEmpty() && OutAlignment.Text != DisplayText)
	{
		const int32 PrefixLength = OutAlignment.Text.Len() - DisplayText.Len();

		if (PrefixLength > 0 && OutAlignment.Text.EndsWith(DisplayText, ESearchCase::CaseSensitive))
		{
			OutAlignment.Text.RightChopInline(PrefixLength);
			OutAlignment.CharacterStartSeconds.RemoveAt(0, PrefixLength, EAllowShrinking::No);
			OutAlignment.CharacterEndSeconds.RemoveAt(0, PrefixLength, EAllowShrinking::No);

			UE_LOG(LogSpeechForgeElevenLabs, Verbose,
				TEXT("Trimmed %d characters of direction from the alignment so its timings index the ")
				TEXT("subtitle rather than the spoken string."),
				PrefixLength);
		}
		else
		{
			// Not the direction prefix - the model rewrote the text some other way. Left alone and
			// flagged, because guessing at a re-base here would be worse than saying so.
			OutAlignment.bFromNormalizedText = true;

			UE_LOG(LogSpeechForgeElevenLabs, Warning,
				TEXT("Alignment text is not the display text with a prefix - it is '%s' against '%s'. ")
				TEXT("Timings index the spoken string and must not be used to position subtitles."),
				*OutAlignment.Text.Left(60), *DisplayText.Left(60));
		}
	}

	// After the re-base, so the derived words and the duration both describe what the subtitle says.
	OutAlignment.DeriveWordsAndDuration();

	return true;
}

FString FElevenLabsProvider::DescribeError(int32 HttpCode, const FString& Body)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);

	if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
	{
		const TSharedPtr<FJsonObject>* Detail = nullptr;
		if (Root->TryGetObjectField(TEXT("detail"), Detail) && Detail && Detail->IsValid())
		{
			FString Message;
			FString Status;
			(*Detail)->TryGetStringField(TEXT("message"), Message);
			(*Detail)->TryGetStringField(TEXT("status"), Status);

			if (!Message.IsEmpty())
			{
				// The message is written for a reader who will act on it - it names the tier a format
				// needs, or the field that was rejected - so it is worth passing through rather than
				// replacing with our own summary.
				return Status.IsEmpty()
					? FString::Printf(TEXT("HTTP %d: %s"), HttpCode, *Message)
					: FString::Printf(TEXT("HTTP %d (%s): %s"), HttpCode, *Status, *Message);
			}
		}
	}

	return FString::Printf(TEXT("HTTP %d: %s"), HttpCode,
		Body.IsEmpty() ? TEXT("no response body") : *Body.Left(400));
}

void FElevenLabsProvider::Synthesize(const FSpeechSynthesisRequest& Request, FOnSpeechSynthesized OnComplete)
{
	FSpeechSynthesisResult Failure;

	if (Request.Voice.ProviderVoiceId.IsEmpty())
	{
		Failure.Error = TEXT("No provider voice id. Pair the Speech Voice asset with a voice first.");
		OnComplete(Failure);
		return;
	}

	const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();
	const int32 SampleRate = Settings ? Settings->RequestedSampleRate : 48000;

	const FString Url = FString::Printf(TEXT("%s/text-to-speech/%s/with-timestamps?output_format=%s"),
		ElevenLabs::ApiRoot,
		*Request.Voice.ProviderVoiceId,
		*MakeOutputFormat(SampleRate));

	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Http = MakeRequest(Url, TEXT("POST"));
	if (!Http.IsValid())
	{
		Failure.Error = TEXT("No API key. Add one in Project Settings > Plugins > SpeechForge.");
		OnComplete(Failure);
		return;
	}

	const FString ModelId = Request.Voice.ModelId.IsEmpty() ? GetDefaultModelId() : Request.Voice.ModelId;

	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("text"), Request.RequestText);
	Body->SetStringField(TEXT("model_id"), ModelId);

	TSharedRef<FJsonObject> VoiceSettings = MakeShared<FJsonObject>();
	ApplyModelQuirks(ModelId, VoiceSettings, Request.Voice.Settings.Stability);
	VoiceSettings->SetNumberField(TEXT("similarity_boost"), Request.Voice.Settings.Similarity);
	VoiceSettings->SetNumberField(TEXT("style"), Request.Voice.Settings.StyleIntensity);
	VoiceSettings->SetNumberField(TEXT("speed"), Request.Voice.Settings.Speed);
	Body->SetObjectField(TEXT("voice_settings"), VoiceSettings);

	if (Request.Seed >= 0)
	{
		Body->SetNumberField(TEXT("seed"), Request.Seed);
	}

	// Stitching. Prosody carries across a conversation instead of every line sounding recorded in
	// its own session, and it costs nothing but sending what we already know.
	if (!Request.PreviousText.IsEmpty())
	{
		Body->SetStringField(TEXT("previous_text"), Request.PreviousText);
	}
	if (!Request.NextText.IsEmpty())
	{
		Body->SetStringField(TEXT("next_text"), Request.NextText);
	}
	if (!Request.PreviousRequestId.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> Ids;
		Ids.Add(MakeShared<FJsonValueString>(Request.PreviousRequestId));
		Body->SetArrayField(TEXT("previous_request_ids"), Ids);
	}

	// Never false. Zero-retention mode would keep this generation out of history, and history is the
	// only durable route back to a take that the seed cannot reproduce.
	Body->SetBoolField(TEXT("enable_logging"), true);

	// Provider overrides, passed through untouched. Anything already set above wins, so an override
	// cannot quietly break stitching or logging.
	for (const TPair<FName, FString>& Override : Request.Voice.Settings.ProviderOverrides)
	{
		const FString Key = Override.Key.ToString();
		if (!Body->HasField(Key))
		{
			Body->SetStringField(Key, Override.Value);
		}
	}

	FString Payload;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Payload);
	FJsonSerializer::Serialize(Body, Writer);

	Http->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Http->SetContentAsString(Payload);

	const FString OutputPath = Request.AbsoluteOutputPath;
	const FString DisplayText = Request.DisplayText;
	const int32 BilledCharacters = Request.RequestText.Len();
	const int32 ExpectedRate = SampleRate;
	const bool bFailOnRateMismatch = Settings ? Settings->bFailOnSampleRateMismatch : true;

	Http->OnProcessRequestComplete().BindLambda(
		[OnComplete, OutputPath, DisplayText, BilledCharacters, ExpectedRate, bFailOnRateMismatch]
		(FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		FSpeechSynthesisResult Result;
		Result.BilledCharacters = BilledCharacters;

		if (!bConnected || !Response.IsValid())
		{
			Result.Error = TEXT("The request never reached ElevenLabs. Check the network.");
			OnComplete(Result);
			return;
		}

		const int32 Code = Response->GetResponseCode();
		if (Code < 200 || Code >= 300)
		{
			Result.Error = DescribeError(Code, Response->GetContentAsString());
			OnComplete(Result);
			return;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			Result.Error = TEXT("The response was not JSON.");
			OnComplete(Result);
			return;
		}

		FString Base64;
		if (!Root->TryGetStringField(TEXT("audio_base64"), Base64) || Base64.IsEmpty())
		{
			Result.Error = TEXT("The response carried no audio.");
			OnComplete(Result);
			return;
		}

		FString Error;
		int32 ActualRate = 0;
		if (!WriteAudio(Base64, OutputPath, ActualRate, Error))
		{
			Result.Error = Error;
			OnComplete(Result);
			return;
		}

		if (ActualRate != ExpectedRate)
		{
			const FString Message = FString::Printf(
				TEXT("Asked for %d Hz and got %d Hz. The provider substituted a format rather than ")
				TEXT("refusing one, which is not how this API is supposed to behave."),
				ExpectedRate, ActualRate);

			if (bFailOnRateMismatch)
			{
				Result.Error = Message;
				OnComplete(Result);
				return;
			}

			UE_LOG(LogSpeechForgeElevenLabs, Warning, TEXT("%s"), *Message);
		}

		if (!ParseAlignment(Root, DisplayText, Result.Alignment, Error))
		{
			Result.Error = Error;
			OnComplete(Result);
			return;
		}

		// The request id lives in a header, not the body. It is the only durable handle to this exact
		// generation, so losing it loses the take on a model whose seed only approximates.
		Result.RequestId = Response->GetHeader(TEXT("request-id"));
		if (Result.RequestId.IsEmpty())
		{
			Result.RequestId = Response->GetHeader(TEXT("x-request-id"));
		}

		Result.bSuccess = true;
		Result.AbsoluteAudioPath = OutputPath;
		Result.AudioFormat = TEXT("wav");
		Result.SampleRate = ActualRate;

		OnComplete(Result);
	});

	Http->ProcessRequest();
}

void FElevenLabsProvider::RefetchById(
	const FString& RequestId, const FString& AbsoluteOutputPath, FOnSpeechSynthesized OnComplete)
{
	FSpeechSynthesisResult Failure;

	if (RequestId.IsEmpty())
	{
		Failure.Error = TEXT("No request id to re-fetch.");
		OnComplete(Failure);
		return;
	}

	// History is keyed by its own item id rather than by the request id the generation returned, so
	// this is a search followed by a fetch. Worth the extra call: it is free, where regenerating both
	// costs money and produces a subtly different performance.
	const FString ListUrl = FString::Printf(TEXT("%s/history?page_size=100"), ElevenLabs::ApiRoot);

	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Http = MakeRequest(ListUrl, TEXT("GET"));
	if (!Http.IsValid())
	{
		Failure.Error = TEXT("No API key.");
		OnComplete(Failure);
		return;
	}

	TWeakPtr<FElevenLabsProvider> WeakThis;
	FElevenLabsProvider* Self = this;

	Http->OnProcessRequestComplete().BindLambda(
		[Self, RequestId, AbsoluteOutputPath, OnComplete]
		(FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		FSpeechSynthesisResult Result;

		if (!bConnected || !Response.IsValid() || Response->GetResponseCode() != 200)
		{
			Result.Error = Response.IsValid()
				? DescribeError(Response->GetResponseCode(), Response->GetContentAsString())
				: TEXT("Could not reach the history endpoint.");
			OnComplete(Result);
			return;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			Result.Error = TEXT("History response was not JSON.");
			OnComplete(Result);
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Root->TryGetArrayField(TEXT("history"), Items))
		{
			Result.Error = TEXT("History response carried no items.");
			OnComplete(Result);
			return;
		}

		FString ItemId;
		for (const TSharedPtr<FJsonValue>& Value : *Items)
		{
			const TSharedPtr<FJsonObject> Item = Value->AsObject();
			if (!Item.IsValid())
			{
				continue;
			}

			FString Candidate;
			if (Item->TryGetStringField(TEXT("request_id"), Candidate) && Candidate == RequestId)
			{
				Item->TryGetStringField(TEXT("history_item_id"), ItemId);
				break;
			}
		}

		if (ItemId.IsEmpty())
		{
			Result.Error = FString::Printf(
				TEXT("Request id '%s' is not in the most recent 100 history items. It may have aged ")
				TEXT("out, or the generation was made with logging disabled."),
				*RequestId);
			OnComplete(Result);
			return;
		}

		const FString AudioUrl = FString::Printf(TEXT("%s/history/%s/audio"), ElevenLabs::ApiRoot, *ItemId);
		TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Fetch = Self->MakeRequest(AudioUrl, TEXT("GET"));
		if (!Fetch.IsValid())
		{
			Result.Error = TEXT("No API key.");
			OnComplete(Result);
			return;
		}

		Fetch->OnProcessRequestComplete().BindLambda(
			[OnComplete, AbsoluteOutputPath, RequestId]
			(FHttpRequestPtr, FHttpResponsePtr AudioResponse, bool bAudioConnected)
		{
			FSpeechSynthesisResult AudioResult;

			if (!bAudioConnected || !AudioResponse.IsValid() || AudioResponse->GetResponseCode() != 200)
			{
				AudioResult.Error = AudioResponse.IsValid()
					? DescribeError(AudioResponse->GetResponseCode(), AudioResponse->GetContentAsString())
					: TEXT("Could not fetch the history audio.");
				OnComplete(AudioResult);
				return;
			}

			const TArray<uint8>& Bytes = AudioResponse->GetContent();
			if (!FFileHelper::SaveArrayToFile(Bytes, *AbsoluteOutputPath))
			{
				AudioResult.Error = FString::Printf(TEXT("Could not write '%s'."), *AbsoluteOutputPath);
				OnComplete(AudioResult);
				return;
			}

			// History returns the audio in the format it was generated in, and carries no alignment -
			// so a re-fetch restores the sound and not the timings. The line keeps the alignment it
			// already stored, which is why that is stored rather than recomputed.
			AudioResult.bSuccess = true;
			AudioResult.AbsoluteAudioPath = AbsoluteOutputPath;
			AudioResult.SampleRate = ReadWavSampleRate(Bytes);
			AudioResult.AudioFormat = AudioResult.SampleRate > 0 ? TEXT("wav") : TEXT("mp3");
			AudioResult.RequestId = RequestId;
			AudioResult.BilledCharacters = 0;

			OnComplete(AudioResult);
		});

		Fetch->ProcessRequest();
	});

	Http->ProcessRequest();
}

void FElevenLabsProvider::TestConnection(FOnSpeechTestComplete OnComplete)
{
	const FString Url = FString::Printf(TEXT("%s/user/subscription"), ElevenLabs::ApiRoot);

	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Http = MakeRequest(Url, TEXT("GET"));
	if (!Http.IsValid())
	{
		OnComplete(false, TEXT("No API key. Add one in Project Settings > Plugins > SpeechForge."));
		return;
	}

	Http->OnProcessRequestComplete().BindLambda(
		[OnComplete](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		if (!bConnected || !Response.IsValid())
		{
			OnComplete(false, TEXT("Could not reach ElevenLabs."));
			return;
		}

		if (Response->GetResponseCode() != 200)
		{
			OnComplete(false, DescribeError(Response->GetResponseCode(), Response->GetContentAsString()));
			return;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OnComplete(false, TEXT("The response was not JSON."));
			return;
		}

		FString Tier;
		int32 Used = 0;
		int32 Limit = 0;
		Root->TryGetStringField(TEXT("tier"), Tier);
		Root->TryGetNumberField(TEXT("character_count"), Used);
		Root->TryGetNumberField(TEXT("character_limit"), Limit);

		OnComplete(true, FString::Printf(
			TEXT("Connected. Tier '%s', %d of %d credits used, %d remaining."),
			*Tier, Used, Limit, FMath::Max(0, Limit - Used)));
	});

	Http->ProcessRequest();
}

void FElevenLabsProvider::ListVoices(FOnSpeechVoicesListed OnComplete)
{
	const FString Url = FString::Printf(TEXT("%s/voices"), ElevenLabs::ApiRoot);

	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Http = MakeRequest(Url, TEXT("GET"));
	if (!Http.IsValid())
	{
		OnComplete(false, {}, TEXT("No API key."));
		return;
	}

	Http->OnProcessRequestComplete().BindLambda(
		[OnComplete](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		if (!bConnected || !Response.IsValid() || Response->GetResponseCode() != 200)
		{
			const FString Error = Response.IsValid()
				? DescribeError(Response->GetResponseCode(), Response->GetContentAsString())
				: TEXT("Could not reach ElevenLabs.");
			OnComplete(false, {}, Error);
			return;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OnComplete(false, {}, TEXT("The response was not JSON."));
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Root->TryGetArrayField(TEXT("voices"), Items))
		{
			OnComplete(false, {}, TEXT("The response carried no voices."));
			return;
		}

		TArray<FSpeechRemoteVoice> Voices;
		for (const TSharedPtr<FJsonValue>& Value : *Items)
		{
			const TSharedPtr<FJsonObject> Item = Value->AsObject();
			if (!Item.IsValid())
			{
				continue;
			}

			FSpeechRemoteVoice Voice;
			Item->TryGetStringField(TEXT("voice_id"), Voice.Id);
			Item->TryGetStringField(TEXT("name"), Voice.Name);
			Item->TryGetStringField(TEXT("description"), Voice.Description);
			Item->TryGetStringField(TEXT("preview_url"), Voice.PreviewUrl);

			FString Category;
			Item->TryGetStringField(TEXT("category"), Category);

			// Worth flagging rather than ignoring: a stock voice can be retired by the provider, and
			// a library built on one goes with it.
			Voice.bIsPremade = (Category == TEXT("premade"));

			Voices.Add(MoveTemp(Voice));
		}

		OnComplete(true, Voices, FString());
	});

	Http->ProcessRequest();
}

void FElevenLabsProvider::ConvertSpeech(
	const FSpeechConversionRequest& Request, FOnSpeechSynthesized OnComplete)
{
	FVoiceConversionRequest Conversion;
	Conversion.SourceWavPath = Request.AbsoluteSourcePath;
	Conversion.VoiceId       = Request.Voice.ProviderVoiceId;
	Conversion.OutputPath    = Request.AbsoluteOutputPath;

	// The conversion model is not the synthesis model, so an empty request keeps the changer's own
	// default rather than inheriting eleven_v3 and being refused.
	if (!Request.ModelId.IsEmpty())
	{
		Conversion.ModelId = Request.ModelId;
	}

	const FString OutputPath = Request.AbsoluteOutputPath;

	FElevenLabsVoiceChanger::Convert(Conversion,
		[OutputPath, OnComplete](bool bSuccess, FString Message)
	{
		FSpeechSynthesisResult Result;
		Result.bSuccess = bSuccess;
		Result.AudioFormat = TEXT("wav");
		Result.SampleRate = 48000;

		if (bSuccess)
		{
			Result.AbsoluteAudioPath = OutputPath;
		}
		else
		{
			Result.Error = MoveTemp(Message);
		}

		OnComplete(Result);
	});
}
