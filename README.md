# SpeechForge ElevenLabs

Adds ElevenLabs to [SpeechForge](https://github.com/AutomationForgeHQ/SpeechForge) as a speech provider. Add-on only:
removing it changes nothing about SpeechForge except which providers are registered.

Until 2026-09-01 this code was a folder inside the SpeechForge core, and the core's settings
defaulted to a vendor by name. It is the same provider, extracted - the same shape as
FaceForgeACE, MotionForgeKimodo and MeshForgeCloud, and the reason any of them can be swapped
without touching a core.

**Everything measured about the account, the API and the models was measured through this code
while it lived in the core** - the verification history is in SpeechForge's README and git log,
and it still applies verbatim: the extraction moved files and changed registration, not behaviour.

---

## What this provider is

Four things about this API shaped the implementation:

| | |
|---|---|
| No job | Synthesis returns audio in the response. There is nothing to poll. |
| Money at submit | Billing is per character of input, charged the moment the request is made, so a failed generation is still a paid one and the estimate has to be exact. |
| Timings free | The `/with-timestamps` endpoint costs the same as plain synthesis and returns character-level alignment, so the plain endpoint is never called. |
| History | Every generation gets a request id and can be re-fetched free, forever. On a model whose seed is only approximate, that is the only durable route back to a take. |

Measured facts no document states: 48 kHz WAV is allowed on a mid-tier account and **44.1 kHz is
not** - a gated format is refused outright with a named tier rather than silently downgraded.

## The model trade-off you cannot default correctly

Measured on a live account, not read:

| | `eleven_v3` | `eleven_multilingual_v2` |
|---|---|---|
| Inline direction (audio tags) | **yes** | no |
| Stitching (`previous_text`) | **rejected**, `unsupported_model` | yes |
| Character limit | 5,000 | 10,000 |
| Stability values | **0.0, 0.5 or 1.0 only** — anything between is a 422 | continuous |

So the two things the speech pipeline most wants — direction, and prosody that carries across a
conversation — are on **different models**. There is no correct project-wide default; it is a real
per-bank decision. `eleven_v3` is this provider's default because inline direction is what makes
SpeechForge's Direction field worth having; a bank that cares more about continuity should be moved
to `eleven_multilingual_v2`.

## Voice Changer — speech to speech

This provider converts a WAV master into a character's voice through
`POST /v1/speech-to-speech/{voice_id}`, writes the result beside the untouched master, and imports
it as a sound asset in the same call. The performance stays human - cadence, pauses, emphasis,
emotion - and only the vocal identity changes, which is the whole argument for conversion over
regenerating with TTS.

**Verified live 2026-09-01** on a real webcam take: a male performance converted into the Alexis
cast voice, **4.55 s -> 4.55 s, delta +0.001 s** - timing preservation measured at one
millisecond, and the converted audio then drove the mouth layer of the take's face solve. Needs
the **Speech to Speech** permission on the key; a key without it fails as HTTP 401 naming
`missing_permissions`, passed through verbatim.

Line-level, and already shipped: `ConvertSpeechLine` on
**[SpeechForgeToolset](https://github.com/AutomationForgeHQ/SpeechForgeToolset)** re-voices a line into its cast voice and
makes the result the line's audio, billed by the duration of the audio rather than by character.
Afterwards the line is stale only if its source or its voice changes - never its text, which a
conversion does not read - and `EstimateSpeechConversionCost` prices it before anything is sent.
A take-level twin, `ConvertSpeechTake`, does the same for a candidate on the take ledger without
promoting it to the line.

## The key

Declared in `Config/ForgeMachine.json`, stored in the OS credential vault under
`SpeechForge/ElevenLabs`, environment fallback `SPEECHFORGE_ELEVENLABS_KEY`. The entry names
predate the extraction and keep their old spelling on purpose: they are looked up rather than
displayed, and renaming them would be a migration for no gain - a key set before the extraction is
still found after it.

Set it in **Tools ▸ Automation Forge ▸ Keys**, the hub, or SpeechForge's own settings page - all
three address the same vault row.

## Related

- **[SpeechForge](https://github.com/AutomationForgeHQ/SpeechForge)** — the pipeline this registers with, and the place
  where voices, banks, staleness and graduation are explained.
