using UnrealBuildTool;

public class PtTextureAssigner : ModuleRules
{
	public PtTextureAssigner(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "DeveloperSettings"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate", "SlateCore", "InputCore",
			"UnrealEd", "ToolMenus",
			"AssetRegistry", "AssetTools", "ContentBrowser",
			"MaterialEditor", "PropertyEditor"
		});
	}
}
