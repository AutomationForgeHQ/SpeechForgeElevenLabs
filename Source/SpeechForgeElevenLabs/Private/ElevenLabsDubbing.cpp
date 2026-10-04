// Dubbing: the one ElevenLabs operation that is a real job - submit, poll, download.
//
// Measured shape of the API, since the docs are thin on the mechanics:
//
//   POST /v1/dubbing                       multipart: file, target_lang, [source_lang], num_speakers,
//                                          drop_background_audio
//     -> { "dubbing_id": "...", "expected_duration_sec": 42.0 }
//   GET  /v1/dubbing/{id}                  -> { "status": "dubbing" | "dubbed" | "failed", "error": ... }
//   GET  /v1/dubbing/{id}/audio/{lang}     -> the rendered audio, MP3 for audio-only input
//   GET  /v1/dubbing/{id}/transcript/{lang} -> what the dub says, as SRT
//
// Billed by the minute of source audio, at a multiple of synthesis, charged at submission.

#include "ElevenLabsProvider.h"

#include "SpeechForgeElevenLabs.h"
#include "SpeechCredentialStore.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace ElevenLabsDubbingPrivate
{
	constexpr float PollIntervalSeconds = 5.f;
	constexpr double PollTimeoutSeconds = 20.0 * 60.0;

	TSharedPtr<FJsonObject> ParseJson(const FString& Body)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
		FJsonSerializer::Deserialize(Reader, Root);
		return Root;
	}

	/** The spoken lines of an SRT, joined; indices and timestamps dropped. */
	FString TextOfSrt(const FString& Srt)
	{
		TArray<FString> Lines;
		Srt.ParseIntoArrayLines(Lines);
		TArray<FString> Spoken;
		for (FString& Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (Line.IsEmpty() || Line.Contains(TEXT("-->")) || Line.IsNumeric())
			{
				continue;
			}
			Spoken.Add(Line);
		}
		return FString::Join(Spoken, TEXT(" "));
	}

	/**
	 * What the dub actually says, so nobody has to guess from listening whether the service
	 * understood the source. The transcript is the service's own, in the target language; it lands
	 * on the result as the alignment text and in the log. A failure here is not a failed dub - the
	 * audio is already on disk - so it degrades to "unknown" rather than to an error.
	 */
	void FetchTranscript(
		const FString& ApiKey,
		const FString& DubbingId,
		const FString& Language,
		FSpeechSynthesisResult Result,
		FOnSpeechSynthesized OnComplete)
	{
		const FString Url = FString::Printf(
			TEXT("https://api.elevenlabs.io/v1/dubbing/%s/transcript/%s?format_type=srt"),
			*DubbingId, *Language);

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
		Http->SetURL(Url);
		Http->SetVerb(TEXT("GET"));
		Http->SetHeader(TEXT("xi-api-key"), ApiKey);
		Http->SetTimeout(60.f);

		Http->OnProcessRequestComplete().BindLambda(
			[DubbingId, Language, Result, OnComplete](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected) mutable
			{
				if (bConnected && Response.IsValid() && Response->GetResponseCode() == 200)
				{
					Result.Alignment.Text = TextOfSrt(Response->GetContentAsString());
					UE_LOG(LogSpeechForgeElevenLabs, Log, TEXT("Dub '%s' says (%s): %s"),
						*DubbingId, *Language, *Result.Alignment.Text);
				}
				else
				{
					UE_LOG(LogSpeechForgeElevenLabs, Warning,
						TEXT("Dub '%s' rendered, but its transcript could not be read (HTTP %d). Listen to it."),
						*DubbingId, Response.IsValid() ? Response->GetResponseCode() : 0);
				}
				OnComplete(Result);
			});

		Http->ProcessRequest();
	}

	void Download(
		const FString& ApiKey,
		const FString& DubbingId,
		const FSpeechDubbingRequest Request,
		FOnSpeechSynthesized OnComplete)
	{
		const FString Url = FString::Printf(
			TEXT("https://api.elevenlabs.io/v1/dubbing/%s/audio/%s"),
			*DubbingId, *Request.TargetLanguage);

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
		Http->SetURL(Url);
		Http->SetVerb(TEXT("GET"));
		Http->SetHeader(TEXT("xi-api-key"), ApiKey);
		Http->SetTimeout(180.f);

		Http->OnProcessRequestComplete().BindLambda(
			[ApiKey, DubbingId, Request, OnComplete](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
			{
				FSpeechSynthesisResult Result;
				Result.RequestId = DubbingId;

				if (!bConnected || !Response.IsValid())
				{
					Result.Error = TEXT("The dubbed audio download never completed. Network, or the service is down.");
					OnComplete(Result);
					return;
				}

				if (Response->GetResponseCode() != 200)
				{
					Result.Error = FString::Printf(TEXT("Downloading the dub failed. HTTP %d: %s"),
						Response->GetResponseCode(), *Response->GetContentAsString().Left(400));
					OnComplete(Result);
					return;
				}

				if (!FFileHelper::SaveArrayToFile(Response->GetContent(), *Request.AbsoluteOutputPath))
				{
					Result.Error = FString::Printf(TEXT("Could not write '%s'."), *Request.AbsoluteOutputPath);
					OnComplete(Result);
					return;
				}

				Result.bSuccess = true;
				Result.AbsoluteAudioPath = Request.AbsoluteOutputPath;
				Result.AudioFormat = FPaths::GetExtension(Request.AbsoluteOutputPath);
				FetchTranscript(ApiKey, DubbingId, Request.TargetLanguage, MoveTemp(Result), OnComplete);
			});

		Http->ProcessRequest();
	}

	void Poll(
		const FString& ApiKey,
		const FString& DubbingId,
		const FSpeechDubbingRequest Request,
		const double StartedAt,
		FOnSpeechSynthesized OnComplete)
	{
		if (FPlatformTime::Seconds() - StartedAt > PollTimeoutSeconds)
		{
			FSpeechSynthesisResult Result;
			Result.RequestId = DubbingId;
			Result.Error = FString::Printf(
				TEXT("Dub '%s' was still rendering after %.0f minutes. It may yet finish on the "
					 "service; this call gave up waiting."),
				*DubbingId, PollTimeoutSeconds / 60.0);
			OnComplete(Result);
			return;
		}

		const FString Url = FString::Printf(TEXT("https://api.elevenlabs.io/v1/dubbing/%s"), *DubbingId);

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
		Http->SetURL(Url);
		Http->SetVerb(TEXT("GET"));
		Http->SetHeader(TEXT("xi-api-key"), ApiKey);
		Http->SetTimeout(30.f);

		Http->OnProcessRequestComplete().BindLambda(
			[ApiKey, DubbingId, Request, StartedAt, OnComplete]
			(FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
			{
				const auto TryAgainLater = [ApiKey, DubbingId, Request, StartedAt, OnComplete]()
				{
					// A ticker rather than a wait: the editor stays interactive for the minutes a
					// dub takes, which is the async contract every long operation here honours.
					FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
						[ApiKey, DubbingId, Request, StartedAt, OnComplete](float) -> bool
						{
							Poll(ApiKey, DubbingId, Request, StartedAt, OnComplete);
							return false; // One shot; the next poll schedules its own.
						}),
						PollIntervalSeconds);
				};

				if (!bConnected || !Response.IsValid())
				{
					// One dropped status check is not a failed dub; the job is still running remotely.
					TryAgainLater();
					return;
				}

				if (Response->GetResponseCode() != 200)
				{
					FSpeechSynthesisResult Result;
					Result.RequestId = DubbingId;
					Result.Error = FString::Printf(TEXT("Dub status check failed. HTTP %d: %s"),
						Response->GetResponseCode(), *Response->GetContentAsString().Left(400));
					OnComplete(Result);
					return;
				}

				const TSharedPtr<FJsonObject> Root = ParseJson(Response->GetContentAsString());
				FString Status;
				if (Root.IsValid())
				{
					Root->TryGetStringField(TEXT("status"), Status);
				}

				if (Status == TEXT("dubbed"))
				{
					Download(ApiKey, DubbingId, Request, OnComplete);
					return;
				}

				if (Status == TEXT("failed"))
				{
					FSpeechSynthesisResult Result;
					Result.RequestId = DubbingId;
					FString ServiceError;
					if (Root.IsValid())
					{
						Root->TryGetStringField(TEXT("error"), ServiceError);
					}
					Result.Error = FString::Printf(TEXT("The service could not dub this audio: %s"),
						ServiceError.IsEmpty() ? TEXT("no reason given") : *ServiceError);
					OnComplete(Result);
					return;
				}

				TryAgainLater();
			});

		Http->ProcessRequest();
	}
}

void FElevenLabsProvider::DubSpeech(const FSpeechDubbingRequest& Request, FOnSpeechSynthesized OnComplete)
{
	using namespace ElevenLabsDubbingPrivate;

	FSpeechSynthesisResult Early;

	FString ApiKey;
	if (!FSpeechCredentialStore::Get(TEXT("ElevenLabs"), ApiKey))
	{
		Early.Error = TEXT("No ElevenLabs key is available. Set one in Tools > Automation Forge > Keys.");
		OnComplete(Early);
		return;
	}

	if (Request.TargetLanguage.IsEmpty())
	{
		Early.Error = TEXT("A dub needs a target language.");
		OnComplete(Early);
		return;
	}

	TArray<uint8> SourceBytes;
	if (!FFileHelper::LoadFileToArray(SourceBytes, *Request.AbsoluteSourcePath))
	{
		Early.Error = FString::Printf(TEXT("Could not read '%s'."), *Request.AbsoluteSourcePath);
		OnComplete(Early);
		return;
	}

	// Multipart by hand, same as speech-to-speech: the HTTP module has no form builder.
	const FString Boundary = FString::Printf(TEXT("----SpeechForgeDub%08x"), FMath::Rand());

	TArray<uint8> Body;
	const auto AppendString = [&Body](const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	};
	const auto AppendField = [&AppendString, &Boundary](const FString& Name, const FString& Value)
	{
		AppendString(FString::Printf(
			TEXT("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n"),
			*Boundary, *Name, *Value));
	};

	AppendField(TEXT("target_lang"), Request.TargetLanguage);
	if (!Request.SourceLanguage.IsEmpty())
	{
		AppendField(TEXT("source_lang"), Request.SourceLanguage);
	}

	// One line, one speaker. Naming it skips the diarisation pass and its chances to be wrong.
	AppendField(TEXT("num_speakers"), TEXT("1"));
	// A voice line is a monologue with no soundtrack. Left on, the service separates "background"
	// from the speech and lays it under the dub - and on a dry recording that background is a
	// smeared copy of the original words, so two languages come out at once and it reads as
	// gibberish. Dropping it is what the API offers for exactly this input.
	AppendField(TEXT("drop_background_audio"), TEXT("true"));

	AppendString(FString::Printf(
		TEXT("--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n")
		TEXT("Content-Type: audio/wav\r\n\r\n"),
		*Boundary, *FPaths::GetCleanFilename(Request.AbsoluteSourcePath)));
	Body.Append(SourceBytes);
	AppendString(FString::Printf(TEXT("\r\n--%s--\r\n"), *Boundary));

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
	Http->SetURL(TEXT("https://api.elevenlabs.io/v1/dubbing"));
	Http->SetVerb(TEXT("POST"));
	Http->SetHeader(TEXT("xi-api-key"), ApiKey);
	Http->SetHeader(TEXT("Content-Type"),
		FString::Printf(TEXT("multipart/form-data; boundary=%s"), *Boundary));
	Http->SetContent(MoveTemp(Body));
	Http->SetTimeout(180.f);

	UE_LOG(LogSpeechForgeElevenLabs, Log,
		TEXT("Submitting '%s' for dubbing into '%s'. Billed by the minute at submission."),
		*FPaths::GetCleanFilename(Request.AbsoluteSourcePath), *Request.TargetLanguage);

	const FSpeechDubbingRequest RequestCopy = Request;

	Http->OnProcessRequestComplete().BindLambda(
		[ApiKey, RequestCopy, OnComplete = MoveTemp(OnComplete)]
		(FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected) mutable
		{
			FSpeechSynthesisResult Result;

			if (!bConnected || !Response.IsValid())
			{
				Result.Error = TEXT("The dub request never reached ElevenLabs. Network, or the service is down.");
				OnComplete(Result);
				return;
			}

			if (Response->GetResponseCode() != 200)
			{
				Result.Error = FString::Printf(TEXT("Submitting the dub failed. HTTP %d: %s"),
					Response->GetResponseCode(), *Response->GetContentAsString().Left(400));
				OnComplete(Result);
				return;
			}

			const TSharedPtr<FJsonObject> Root = ParseJson(Response->GetContentAsString());
			FString DubbingId;
			if (!Root.IsValid() || !Root->TryGetStringField(TEXT("dubbing_id"), DubbingId) || DubbingId.IsEmpty())
			{
				Result.Error = FString::Printf(TEXT("The dub was accepted but no dubbing_id came back: %s"),
					*Response->GetContentAsString().Left(400));
				OnComplete(Result);
				return;
			}

			double ExpectedSeconds = 0.0;
			if (Root.IsValid())
			{
				Root->TryGetNumberField(TEXT("expected_duration_sec"), ExpectedSeconds);
			}

			UE_LOG(LogSpeechForgeElevenLabs, Log,
				TEXT("Dub '%s' is rendering (service expects ~%.0fs). Polling every %.0fs."),
				*DubbingId, ExpectedSeconds, ElevenLabsDubbingPrivate::PollIntervalSeconds);

			ElevenLabsDubbingPrivate::Poll(
				ApiKey, DubbingId, RequestCopy, FPlatformTime::Seconds(), MoveTemp(OnComplete));
		});

	Http->ProcessRequest();
}
