#include "ElevenLabsVoiceChanger.h"

#include "SpeechForgeElevenLabs.h"

#include "SpeechCredentialStore.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace VoiceChangerPrivate
{
	/**
	 * Seconds of audio in a RIFF/WAVE file, walking chunks rather than assuming a 44-byte header.
	 * Zero when the file is not a WAV this can read. Local on purpose: the provider has its own
	 * header reader, but a private static is not an API and this file should not reach into it.
	 */
	double ReadWavDurationSeconds(const TArray<uint8>& Bytes)
	{
		if (Bytes.Num() < 44 || FMemory::Memcmp(Bytes.GetData(), "RIFF", 4) != 0 ||
			FMemory::Memcmp(Bytes.GetData() + 8, "WAVE", 4) != 0)
		{
			return 0.0;
		}

		uint32 ByteRate = 0;
		int64 Offset = 12;
		while (Offset + 8 <= Bytes.Num())
		{
			const uint8* Chunk = Bytes.GetData() + Offset;
			uint32 ChunkSize = 0;
			FMemory::Memcpy(&ChunkSize, Chunk + 4, 4);

			if (FMemory::Memcmp(Chunk, "fmt ", 4) == 0 && Offset + 16 <= Bytes.Num())
			{
				FMemory::Memcpy(&ByteRate, Chunk + 8 + 8, 4);
			}
			else if (FMemory::Memcmp(Chunk, "data", 4) == 0 && ByteRate > 0)
			{
				return static_cast<double>(ChunkSize) / ByteRate;
			}

			Offset += 8 + ChunkSize + (ChunkSize & 1);
		}

		return 0.0;
	}
}

void FElevenLabsVoiceChanger::Convert(
	const FVoiceConversionRequest& Request, TFunction<void(bool, FString)> OnComplete)
{
	using namespace VoiceChangerPrivate;

	FString ApiKey;
	if (!FSpeechCredentialStore::Get(TEXT("ElevenLabs"), ApiKey))
	{
		OnComplete(false, TEXT("No ElevenLabs key is available. Set one in Tools > Automation Forge > Keys."));
		return;
	}

	TArray<uint8> SourceBytes;
	if (!FFileHelper::LoadFileToArray(SourceBytes, *Request.SourceWavPath))
	{
		OnComplete(false, FString::Printf(TEXT("Could not read '%s'."), *Request.SourceWavPath));
		return;
	}

	const double SourceSeconds = ReadWavDurationSeconds(SourceBytes);

	// Multipart by hand: the HTTP module has no form builder, and the shape is three fixed parts.
	const FString Boundary = FString::Printf(TEXT("----SpeechForgeSTS%08x"), FMath::Rand());

	TArray<uint8> Body;
	auto AppendString = [&Body](const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	};

	AppendString(FString::Printf(
		TEXT("--%s\r\nContent-Disposition: form-data; name=\"model_id\"\r\n\r\n%s\r\n"),
		*Boundary, *Request.ModelId));
	AppendString(FString::Printf(
		TEXT("--%s\r\nContent-Disposition: form-data; name=\"audio\"; filename=\"%s\"\r\n")
		TEXT("Content-Type: audio/wav\r\n\r\n"),
		*Boundary, *FPaths::GetCleanFilename(Request.SourceWavPath)));
	Body.Append(SourceBytes);
	AppendString(FString::Printf(TEXT("\r\n--%s--\r\n"), *Boundary));

	// wav_48000 rather than a default: the same tier gate as text-to-speech, where 44.1 kHz is
	// refused on this account and 48 kHz is not - and 48 kHz is what everything downstream wants.
	const FString Url = FString::Printf(
		TEXT("https://api.elevenlabs.io/v1/speech-to-speech/%s?output_format=wav_48000"),
		*Request.VoiceId);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
	Http->SetURL(Url);
	Http->SetVerb(TEXT("POST"));
	Http->SetHeader(TEXT("xi-api-key"), ApiKey);
	Http->SetHeader(TEXT("Content-Type"),
		FString::Printf(TEXT("multipart/form-data; boundary=%s"), *Boundary));
	Http->SetContent(MoveTemp(Body));
	Http->SetTimeout(180.f);

	UE_LOG(LogSpeechForgeElevenLabs, Log,
		TEXT("Converting %.2fs of '%s' into voice '%s'."),
		SourceSeconds, *FPaths::GetCleanFilename(Request.SourceWavPath), *Request.VoiceId);

	Http->OnProcessRequestComplete().BindLambda(
		[Request, SourceSeconds, OnComplete = MoveTemp(OnComplete)]
		(FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected) mutable
		{
			if (!bConnected || !Response.IsValid())
			{
				OnComplete(false, TEXT("The request never reached ElevenLabs. Network, or the service is down."));
				return;
			}

			if (Response->GetResponseCode() != 200)
			{
				// The error body is JSON and names the actual problem - a missing permission on the
				// key reads as 401 with "missing_permissions", which is exactly what a reader needs.
				OnComplete(false, FString::Printf(TEXT("HTTP %d: %s"),
					Response->GetResponseCode(), *Response->GetContentAsString().Left(400)));
				return;
			}

			const TArray<uint8>& Audio = Response->GetContent();
			const double ConvertedSeconds = VoiceChangerPrivate::ReadWavDurationSeconds(Audio);

			if (ConvertedSeconds <= 0.0)
			{
				OnComplete(false, FString::Printf(
					TEXT("The response was not a readable WAV (%d bytes, content-type '%s')."),
					Audio.Num(), *Response->GetContentType()));
				return;
			}

			if (!FFileHelper::SaveArrayToFile(Audio, *Request.OutputPath))
			{
				OnComplete(false, FString::Printf(TEXT("Could not write '%s'."), *Request.OutputPath));
				return;
			}

			// The timing QA from the capture brief: conversion is designed to preserve timing and
			// never guaranteed to. The delta is data the alignment stage needs, so it is in the
			// message rather than in a log somebody has to know to look for.
			OnComplete(true, FString::Printf(
				TEXT("Converted %.2fs -> %.2fs (delta %+.3fs) into '%s'."),
				SourceSeconds, ConvertedSeconds, ConvertedSeconds - SourceSeconds,
				*Request.OutputPath));
		});

	Http->ProcessRequest();
}
