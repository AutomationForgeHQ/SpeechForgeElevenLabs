# SpeechForgeElevenLabs

Every released version of SpeechForgeElevenLabs, newest first. A release publishes **one** section of this
file — the one whose heading matches its tag — as its release notes; for an `open` plugin those
notes are posted to Discord `#releases` automatically. Write for someone who installs the plugin,
not for the commit log.

Headings are `## <x.y.z> — <date>`. Use `Added` / `Changed` / `Fixed` / `Compatibility` /
`Known issues`, only the ones that apply.

## 0.1.3 — unreleased

### Added
- Works with SpeechForge's new *Send Conversation Context* option: a line carrying the same speaker's earlier takes sends the newest three as `previous_request_ids`, and the text after the line as `next_text`.
- `eleven_v4` and `eleven_v4_turbo` are in the character-limit table at 10,000 characters each. Before, they fell back to 5,000. To use `eleven_v4`, set it as the model on the voice profile or as the project default; it takes audio tags, stitching context and any stability value.

### Compatibility
- Needs SpeechForge 0.2.4 or later.

## 0.1.2 — 2026-10-04

### Changed
- Copyright and licence notices now name Bojan Andrejek / MetaWorx LLC. It is still Apache 2.0, and nothing about how you may use it changed.
- Dubbing a dry voice line now drops background audio, states the line's source language, and reads back what the dub says from its transcript.

## 0.1.1 — 2026-09-08
- Packaging fix: the release now carries everything the register allows. `BuildPlugin`'s filter excludes `Config/` and every `public_extra` path, so earlier zips shipped without them.
- `Config/ForgeMachine.json` reaches an installed copy for the first time, so the hub's Keys and Runners pages are no longer empty for it.

## 0.1.0 — 2026-09-07
- Extracted out of the SpeechForge core so the core names no vendor
- Voice conversion closes the capture-brief pipeline, measured at one millisecond of timing drift
- Casting made human-readable and controllable: audition, discovered actions, a stage
- Speakers, voice profiles, and the three-page Speech Library
- `ConvertSpeechLine` shipped — re-voice a line without rewriting it
- PerformanceForge's toolset moved out; provider toolsets stay with their providers
- A provider now earns a toolset only by having something only it can do
- Localisation: a sibling bank per language, joined by line id
- Licensed and registered in `plugins.json` alongside SpeechForgeDeepL
- Repointed to kovati.dev
- Two small maintenance fixes from the SurfaceForge distribution audit
- Marked beta rather than experimental
