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

private:
	TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FPtSlotEntry> Item, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<SWidget> MakeTextRow(const FString& Label, TFunction<FString()> Get, TFunction<void(const FString&)> Set);
	TSharedRef<SWidget> MakeMapRow(const FString& Label, int32 MapIndex);

	FReply OnRefreshClicked();
	FReply OnApplyClicked();
	bool CanApply() const;

	void Refresh();
	void AppendLog(const FString& Line);

	FPtSettings Settings;
	TArray<TSharedPtr<FPtSlotEntry>> Entries;
	TSharedPtr<SListView<TSharedPtr<FPtSlotEntry>>> ListView;
	TSharedPtr<SMultiLineEditableTextBox> LogBox;
	TSharedPtr<FAssetThumbnailPool> ThumbnailPool;
	FString LogText;
};
