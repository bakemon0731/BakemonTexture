#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "PtAssignerCore.h"

class SMultiLineEditableTextBox;
class FAssetThumbnailPool;

class SPtAssignerPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPtAssignerPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** コンテンツブラウザの選択を読み込み直す（メニューから開いた時にも外部から呼ぶ） */
	void Refresh();

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

private:
	TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FPtSlotEntry> Item, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<SWidget> MakeTextRow(const FString& Label, TFunction<FString()> Get, TFunction<void(const FString&)> Set);
	TSharedRef<SWidget> MakeMapRow(const FString& Label, int32 MapIndex);
	/** 見出し付きのまとまり（設定画面用） */
	TSharedRef<SWidget> MakeSection(const FString& Title, const TSharedRef<SWidget>& Body);
	TSharedRef<SWidget> MakeCheck(const FString& Label, const FString& Tip, TFunction<bool()> Get, TFunction<void(bool)> Set, bool bRefresh);

	FReply OnRefreshClicked();
	FReply OnApplyClicked();
	FReply OnPickFbxClicked();
	FReply OnClearFbxClicked();
	FReply OnCheckAllClicked(bool bOn);
	FReply OnFixSettingsClicked();
	bool CanApply() const;
	int32 CountToApply() const;
	FText GetApplyLabel() const;
	FText GetSummaryText() const;

	void AppendLog(const FString& Line);

	FPtSettings Settings;
	TArray<TSharedPtr<FPtSlotEntry>> Entries;
	TSharedPtr<SListView<TSharedPtr<FPtSlotEntry>>> ListView;
	TSharedPtr<SMultiLineEditableTextBox> LogBox;
	TSharedPtr<FAssetThumbnailPool> ThumbnailPool;
	FString LogText;
	FString LastSelectionSignature;
	/** 「FBXを指定」した時に選択していたメッシュ。選択が変わったら指定を解除する */
	FString FbxPickedSignature;
	double NextPollTime = 0.0;
};
