#include "PtTextureAssignerModule.h"
#include "SPtAssignerPanel.h"
#include "ToolMenus.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"

static const FName PtTabName(TEXT("PtTextureAssignerTab"));

static TWeakPtr<SPtAssignerPanel> GPtPanel;

static TSharedRef<SDockTab> SpawnPtTab(const FSpawnTabArgs&)
{
	const TSharedRef<SPtAssignerPanel> Panel = SNew(SPtAssignerPanel);
	GPtPanel = Panel;
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			Panel
		];
}

/** タブを開く。すでに開いていた場合は、今の選択を読み込み直す（新規なら Construct で読み込み済み） */
static void OpenPtTab()
{
	const bool bAlreadyOpen = GPtPanel.IsValid();
	FGlobalTabmanager::Get()->TryInvokeTab(PtTabName);
	if (bAlreadyOpen)
	{
		if (const TSharedPtr<SPtAssignerPanel> Panel = GPtPanel.Pin())
		{
			Panel->Refresh();
		}
	}
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
		FUIAction(FExecuteAction::CreateStatic(&OpenPtTab)));

	// コンテンツブラウザでメッシュを右クリック → そのまま開く
	for (const TCHAR* MenuName : { TEXT("ContentBrowser.AssetContextMenu.StaticMesh"), TEXT("ContentBrowser.AssetContextMenu.SkeletalMesh") })
	{
		UToolMenu* AssetMenu = UToolMenus::Get()->ExtendMenu(MenuName);
		FToolMenuSection& AssetSection = AssetMenu->FindOrAddSection(
			"PtTextureAssigner", FText::FromString(TEXT("Pt Texture Assigner")));
		AssetSection.AddMenuEntry(
			"OpenPtTextureAssignerFromAsset",
			FText::FromString(TEXT("Pt テクスチャ割り当て")),
			FText::FromString(TEXT("選択したメッシュを読み込んで割り当てツールを開きます")),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateStatic(&OpenPtTab)));
	}
}

void FPtTextureAssignerModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(PtTabName);
}

IMPLEMENT_MODULE(FPtTextureAssignerModule, PtTextureAssigner)
