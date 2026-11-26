#include "PtTextureAssignerModule.h"
#include "SPtAssignerPanel.h"
#include "ToolMenus.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"

static const FName PtTabName(TEXT("PtTextureAssignerTab"));

static TSharedRef<SDockTab> SpawnPtTab(const FSpawnTabArgs&)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SPtAssignerPanel)
		];
}

void FPtTextureAssignerModule::StartupModule()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(PtTabName, FOnSpawnTab::CreateStatic(&SpawnPtTab))
		.SetDisplayName(FText::FromString(TEXT("Pt テクスチャ割り当て")))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FPtTextureAssignerModule::RegisterMenus));
}

void FPtTextureAssignerModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
	FToolMenuSection& Section = Menu->FindOrAddSection(
		"PtTextureAssigner", FText::FromString(TEXT("Pt Texture Assigner")));

	Section.AddMenuEntry(
		"OpenPtTextureAssigner",
		FText::FromString(TEXT("Pt テクスチャ割り当て")),
		FText::FromString(TEXT("選択したメッシュにPainterのテクスチャを自動で割り当てます")),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			FGlobalTabmanager::Get()->TryInvokeTab(PtTabName);
		})));
}

void FPtTextureAssignerModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(PtTabName);
}

IMPLEMENT_MODULE(FPtTextureAssignerModule, PtTextureAssigner)
