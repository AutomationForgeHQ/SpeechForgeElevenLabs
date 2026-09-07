using UnrealBuildTool;

public class SpeechForgeElevenLabs : ModuleRules
{
	public SpeechForgeElevenLabs(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"SpeechForge",  // the capability this provider plugs into
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"HTTP",         // the whole provider is REST calls
				"Json",

				// The one tool this module publishes. See ElevenLabsToolset.h for why it lives
				// here rather than in a sidecar plugin, for now.

				// Importing the converted WAV as a sound asset, the moment it lands.
				"UnrealEd",
				"AssetTools",
				"AudioEditor",
			}
			);
	}
}
