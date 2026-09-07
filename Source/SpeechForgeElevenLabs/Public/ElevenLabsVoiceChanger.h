// Speech-to-speech: a performer's WAV re-voiced as a character, timing and delivery kept.

#pragma once

#include "CoreMinimal.h"

/** One conversion, fully resolved. Nothing here is a reference to be looked up later. */
struct FVoiceConversionRequest
{
	/** Absolute path of the WAV to convert. Never modified; the master is immutable. */
	FString SourceWavPath;

	/** The ElevenLabs voice to convert into. */
	FString VoiceId;

	/** The speech-to-speech model. The multilingual one is the current standard. */
	FString ModelId = TEXT("eleven_multilingual_sts_v2");

	/** Absolute path the converted WAV is written to. */
	FString OutputPath;
};

/**
 * Calls ElevenLabs' Voice Changer: POST /v1/speech-to-speech/{voice_id}, multipart, WAV out.
 *
 * This is the brief's core bet made runnable: conversion keeps the performer's cadence, pauses,
 * emphasis and emotion while changing vocal identity - the performance stays human, only the
 * voice is synthetic. The completion message carries the timing QA the capture brief asks for:
 * source duration, converted duration, delta - because conversion preserves timing approximately,
 * and "approximately" is a number somebody downstream needs, not a hope.
 *
 * Take-level on purpose, for now. The line-level integration - a SourceSound on a speech line,
 * staleness over the source hash, per-second billing in the estimator - is a designed follow-up;
 * this proves the endpoint and feeds the capture pipeline today.
 *
 * OnComplete always runs on the game thread.
 */
class SPEECHFORGEELEVENLABS_API FElevenLabsVoiceChanger
{
public:

	static void Convert(const FVoiceConversionRequest& Request, TFunction<void(bool, FString)> OnComplete);
};
