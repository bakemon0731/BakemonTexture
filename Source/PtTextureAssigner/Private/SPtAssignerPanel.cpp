#include "SPtAssignerPanel.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"
#include "Styling/AppStyle.h"
#include "AssetThumbnail.h"
#include "PropertyCustomizationHelpers.h"
#include "Engine/Texture2D.h"

static FText J(const TCHAR* S) { return FText::FromString(S); }

namespace
{
	constexpr int32 ThumbSize = 64;
	constexpr float LineHeight = 1.35f; // 行間（1.0 = 詰まった状態）
}

// ---------------- 1行分の表示 ----------------

class SPtEntryRow : public SMultiColumnTableRow<TSharedPtr<FPtSlotEntry>>
{
public:
	SLATE_BEGIN_ARGS(SPtEntryRow) {}
		SLATE_ARGUMENT(TSharedPtr<FPtSlotEntry>, Entry)
		SLATE_ARGUMENT(TSharedPtr<FAssetThumbnailPool>, ThumbnailPool)
		SLATE_ARGUMENT(FPtSettings*, Settings)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable)
	{
		Entry = InArgs._Entry;
		ThumbnailPool = InArgs._ThumbnailPool;
		Settings = InArgs._Settings;
		SMultiColumnTableRow<TSharedPtr<FPtSlotEntry>>::Construct(
			FSuperRowType::FArguments()
				.Style(&FAppStyle::Get().GetWidgetStyle<FTableRowStyle>("TableView.AlternatingRow")),
			OwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
	{
		const TSharedPtr<FPtSlotEntry> E = Entry;

		if (Column == "Check")
		{
			return Cell(SNew(SCheckBox)
				.IsEnabled_Lambda([E]() { return E->Status == EPtStatus::Ready; })
				.IsChecked_Lambda([E]() { return E->bChecked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([E](ECheckBoxState S) { E->bChecked = (S == ECheckBoxState::Checked); }));
		}
		if (Column == "Material")
		{
			return MakeMaterialCell();
		}
		if (Column == "Textures")
		{
			return MakeTextureCell();
		}
		if (Column == "Mesh")
		{
			return Cell(SNew(STextBlock)
				.Text(FText::FromString(E->MeshName))
				.AutoWrapText(true)
				.LineHeightPercentage(LineHeight));
		}
		if (Column == "Slot")
		{
			return Cell(SNew(STextBlock)
				.Text(FText::FromString(E->SlotName))
				.AutoWrapText(true)
				.LineHeightPercentage(LineHeight));
		}
		if (Column == "Status")
		{
			return Cell(SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("BoldFont"))
				.Text_Lambda([E]() { return StatusText(E->Status); })
				.ColorAndOpacity_Lambda([E]() { return FSlateColor(StatusColor(E->Status)); }));
		}
		// 詳細：1件ずつ改行され、行間を広めにとる
		return Cell(SNew(STextBlock)
			.Text_Lambda([E]() { return FText::FromString(E->Message); })
			.AutoWrapText(true)
			.LineHeightPercentage(LineHeight));
	}

private:
	/** どの列も 縦中央 + 上下左右に余白 */
	static TSharedRef<SWidget> Cell(const TSharedRef<SWidget>& Content)
	{
		return SNew(SBox)
			.VAlign(VAlign_Center)
			.Padding(FMargin(6.f, 8.f))
			[
				Content
			];
	}

	/** スロットに入っているマテリアルをサムネイルで表示（なければ空枠） */
	TSharedRef<SWidget> MakeMaterialCell()
	{
		const FAssetData& Data = Entry->MaterialData;

		TSharedRef<SWidget> Thumb = SNullWidget::NullWidget;
		FText Tip = J(TEXT("マテリアル未設定"));

		if (Data.IsValid() && ThumbnailPool.IsValid())
		{
			Thumbnail = MakeShared<FAssetThumbnail>(Data, ThumbSize, ThumbSize, ThumbnailPool);
			FAssetThumbnailConfig Config;
			Config.bAllowFadeIn = false;
			Thumb = Thumbnail->MakeThumbnailWidget(Config);
			Tip = FText::FromString(Data.GetSoftObjectPath().ToString());
		}
		else
		{
			Thumb = SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
						.Text(J(TEXT("なし")))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				];
		}

		return SNew(SBox)
			.VAlign(VAlign_Center)
			.Padding(FMargin(6.f, 6.f))
			.ToolTipText(Tip)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SNew(SBox).WidthOverride(ThumbSize).HeightOverride(ThumbSize)
					[
						Thumb
					]
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 4.f, 0.f, 0.f)
				[
					SNew(SBox).WidthOverride(ThumbSize + 8)
					[
						SNew(STextBlock)
							.Font(FAppStyle::GetFontStyle("SmallFont"))
							.Text(FText::FromString(Data.IsValid() ? Data.AssetName.ToString() : FString()))
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
							.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					]
				]
			];
	}

	/**
	 * テクスチャ3枠（BaseColor / Normal / ORM）。コンテンツブラウザと同じアセットピッカー付き。
	 * 名前検索で見つかったものは最初から入っている。差し替え・空欄の補完はここで自由に行う。
	 */
	TSharedRef<SWidget> MakeTextureCell()
	{
		static const TCHAR* Labels[] = { TEXT("BaseColor"), TEXT("Normal"), TEXT("ORM") };

		const TSharedPtr<FPtSlotEntry> E = Entry;
		FPtSettings* S = Settings;
		const TSharedPtr<FAssetThumbnailPool> Pool = ThumbnailPool;

		TSharedRef<SHorizontalBox> TextureBox = SNew(SHorizontalBox);
		for (int32 i = 0; i < E->TexturePaths.Num(); ++i)
		{
			const FString Label = (i < (int32)UE_ARRAY_COUNT(Labels)) ? FString(Labels[i]) : FString::Printf(TEXT("Tex%d"), i);

			TextureBox->AddSlot()
				.FillWidth(1.f)
				.Padding(i > 0 ? 6.f : 0.f, 0.f, 0.f, 0.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 3.f)
				[
					SNew(STextBlock)
						.Font(FAppStyle::GetFontStyle("SmallFont"))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text_Lambda([E, i, Label]()
						{
							const EPtTexSource Src = E->Sources.IsValidIndex(i) ? E->Sources[i] : EPtTexSource::None;
							const TCHAR* Tag = TEXT("");
							switch (Src)
							{
							case EPtTexSource::Auto:      Tag = TEXT("（自動）"); break;
							case EPtTexSource::Manual:    Tag = TEXT("（手動）"); break;
							case EPtTexSource::Ambiguous: Tag = TEXT("（重複・要選択）"); break;
							default: break;
							}
							return FText::FromString(Label + Tag);
						})
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SObjectPropertyEntryBox)
						.AllowedClass(UTexture2D::StaticClass())
						.ThumbnailPool(Pool)
						.DisplayThumbnail(true)
						.DisplayBrowse(true)
						.DisplayUseSelected(true)
						.AllowClear(true)
						.ObjectPath_Lambda([E, i]()
						{
							return (E->TexturePaths.IsValidIndex(i) && !E->TexturePaths[i].IsNull())
								? E->TexturePaths[i].ToString() : FString();
						})
						.OnObjectChanged_Lambda([E, S, i](const FAssetData& Data)
						{
							if (!E->TexturePaths.IsValidIndex(i) || !E->Sources.IsValidIndex(i))
							{
								return;
							}
							const bool bSet = Data.IsValid();
							E->TexturePaths[i] = bSet ? Data.GetSoftObjectPath() : FSoftObjectPath();
							E->Sources[i] = bSet ? EPtTexSource::Manual : EPtTexSource::None;
							if (S)
							{
								PtAssignerCore::UpdateStatus(*E, *S);
							}
						})
				]
			];
		}
		return Cell(TextureBox);
	}

	static FText StatusText(EPtStatus S)
	{
		switch (S)
		{
		case EPtStatus::Ready:     return J(TEXT("準備OK"));
		case EPtStatus::Missing:   return J(TEXT("不足"));
		case EPtStatus::Ambiguous: return J(TEXT("重複"));
		case EPtStatus::Done:      return J(TEXT("完了"));
		case EPtStatus::Skipped:   return J(TEXT("スキップ"));
		default:                   return J(TEXT("失敗"));
		}
	}
	static FLinearColor StatusColor(EPtStatus S)
	{
		switch (S)
		{
		case EPtStatus::Ready:
		case EPtStatus::Done:      return FLinearColor(0.2f, 0.8f, 0.3f);
		case EPtStatus::Missing:
		case EPtStatus::Ambiguous:
		case EPtStatus::Skipped:   return FLinearColor(1.0f, 0.65f, 0.1f);
		default:                   return FLinearColor(1.0f, 0.25f, 0.25f);
		}
	}

	TSharedPtr<FPtSlotEntry> Entry;
	TSharedPtr<FAssetThumbnailPool> ThumbnailPool;
	FPtSettings* Settings = nullptr;
	TSharedPtr<FAssetThumbnail> Thumbnail; // 行が生きている間はサムネイルも保持する
};

// ---------------- パネル ----------------

TSharedRef<SWidget> SPtAssignerPanel::MakeTextRow(const FString& Label,
	TFunction<FString()> Get, TFunction<void(const FString&)> Set)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 5, 12, 5)
		[
			SNew(SBox).WidthOverride(190.f)
			[
				SNew(STextBlock).Text(FText::FromString(Label)).AutoWrapText(true).LineHeightPercentage(LineHeight)
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0, 5)
		[
			SNew(SEditableTextBox)
			.Text_Lambda([Get]() { return FText::FromString(Get()); })
			.OnTextCommitted_Lambda([this, Set](const FText& T, ETextCommit::Type)
			{
				Set(T.ToString());
				Settings.Save();
				Refresh();
			})
		];
}

TSharedRef<SWidget> SPtAssignerPanel::MakeMapRow(const FString& Label, int32 MapIndex)
{
	auto Commit = [this](const FText&, ETextCommit::Type) { Settings.Save(); Refresh(); };
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 5, 12, 5)
		[
			SNew(SBox).WidthOverride(190.f)
			[
				SNew(STextBlock).Text(FText::FromString(Label)).AutoWrapText(true).LineHeightPercentage(LineHeight)
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0, 5, 6, 5)
		[
			SNew(SEditableTextBox)
			.HintText(J(TEXT("パラメータ名")))
			.Text_Lambda([this, MapIndex]() { return FText::FromString(Settings.Maps[MapIndex].Param); })
			.OnTextChanged_Lambda([this, MapIndex](const FText& T) { Settings.Maps[MapIndex].Param = T.ToString(); })
			.OnTextCommitted_Lambda(Commit)
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(6, 5, 0, 5)
		[
			SNew(SEditableTextBox)
			.HintText(J(TEXT("ファイル名の末尾")))
			.Text_Lambda([this, MapIndex]() { return FText::FromString(Settings.Maps[MapIndex].Suffix); })
			.OnTextChanged_Lambda([this, MapIndex](const FText& T) { Settings.Maps[MapIndex].Suffix = T.ToString(); })
			.OnTextCommitted_Lambda(Commit)
		];
}

void SPtAssignerPanel::Construct(const FArguments&)
{
	Settings.Load();
	ThumbnailPool = MakeShared<FAssetThumbnailPool>(128);

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.f)
		[
			SNew(SVerticalBox)

			// 使い方（3行に分けて読みやすく）
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.LineHeightPercentage(LineHeight)
				.Text(J(TEXT("① コンテンツブラウザでメッシュを選択\n② 名前が一致したテクスチャは自動で入ります。違うものや空欄は各枠のピッカーから選ぶ（ドラッグ&ドロップも可）\n③ 「準備OK」になった行で「割り当てる」を押す")))
			]

			// 設定（普段は閉じておく）
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				SNew(SExpandableArea)
				.InitiallyCollapsed(true)
				.AreaTitle(J(TEXT("設定")))
				.BodyContent()
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						MakeTextRow(TEXT("テクスチャ検索先（; 区切り）"),
							[this]() { return Settings.TextureFolders; },
							[this](const FString& V) { Settings.TextureFolders = V; })
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						MakeTextRow(TEXT("親マテリアル"),
							[this]() { return Settings.MasterPath; },
							[this](const FString& V) { Settings.MasterPath = V; })
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						MakeTextRow(TEXT("インスタンス出力先"),
							[this]() { return Settings.InstanceFolder; },
							[this](const FString& V) { Settings.InstanceFolder = V; })
					]
					+ SVerticalBox::Slot().AutoHeight() [ MakeMapRow(TEXT("BaseColor（名前 / 末尾）"), 0) ]
					+ SVerticalBox::Slot().AutoHeight() [ MakeMapRow(TEXT("Normal（名前 / 末尾）"), 1) ]
					+ SVerticalBox::Slot().AutoHeight() [ MakeMapRow(TEXT("ORM（名前 / 末尾）"), 2) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([this]() { return Settings.bAutoFixTextureSettings ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { Settings.bAutoFixTextureSettings = (S == ECheckBoxState::Checked); Settings.Save(); })
						[
							SNew(STextBlock).Text(J(TEXT("Normal / ORM の圧縮設定を自動で直す")))
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([this]() { return Settings.bSaveAfterApply ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { Settings.bSaveAfterApply = (S == ECheckBoxState::Checked); Settings.Save(); })
						[
							SNew(STextBlock).Text(J(TEXT("適用後にアセットを保存する")))
						]
					]
				]
			]

			// ボタン
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
				[
					SNew(SButton)
					.Text(J(TEXT("選択を読み込む")))
					.ToolTipText(J(TEXT("コンテンツブラウザの選択メッシュを再解析します")))
					.OnClicked(this, &SPtAssignerPanel::OnRefreshClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(J(TEXT("割り当てる")))
					.IsEnabled(this, &SPtAssignerPanel::CanApply)
					.OnClicked(this, &SPtAssignerPanel::OnApplyClicked)
				]
			]

			// 一覧
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SAssignNew(ListView, SListView<TSharedPtr<FPtSlotEntry>>)
				.ListItemsSource(&Entries)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow(this, &SPtAssignerPanel::OnGenerateRow)
				.HeaderRow
				(
					SNew(SHeaderRow)
					+ SHeaderRow::Column("Check").DefaultLabel(FText::GetEmpty()).FixedWidth(34.f)
					+ SHeaderRow::Column("Material").DefaultLabel(J(TEXT("マテリアル"))).FixedWidth(ThumbSize + 24.f)
					+ SHeaderRow::Column("Mesh").DefaultLabel(J(TEXT("メッシュ"))).FillWidth(0.15f)
					+ SHeaderRow::Column("Slot").DefaultLabel(J(TEXT("スロット"))).FillWidth(0.13f)
					+ SHeaderRow::Column("Status").DefaultLabel(J(TEXT("状態"))).FixedWidth(80.f)
					+ SHeaderRow::Column("Textures").DefaultLabel(J(TEXT("テクスチャ（名前検索で自動入力 / 自由に差し替え可）"))).FillWidth(0.52f)
					+ SHeaderRow::Column("Message").DefaultLabel(J(TEXT("詳細"))).FillWidth(0.2f)
				)
			]

			// ログ
			+ SVerticalBox::Slot().AutoHeight().MaxHeight(140.f).Padding(0, 12, 0, 0)
			[
				SAssignNew(LogBox, SMultiLineEditableTextBox)
				.IsReadOnly(true)
			]
		]
	];

	Refresh();
}

TSharedRef<ITableRow> SPtAssignerPanel::OnGenerateRow(TSharedPtr<FPtSlotEntry> Item, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SPtEntryRow, Owner).Entry(Item).ThumbnailPool(ThumbnailPool).Settings(&Settings);
}

bool SPtAssignerPanel::CanApply() const
{
	return Entries.ContainsByPredicate([](const TSharedPtr<FPtSlotEntry>& E)
	{
		return E->bChecked && E->Status == EPtStatus::Ready;
	});
}

FReply SPtAssignerPanel::OnRefreshClicked()
{
	Refresh();
	return FReply::Handled();
}

FReply SPtAssignerPanel::OnApplyClicked()
{
	Settings.Save();
	PtAssignerCore::Apply(Settings, Entries, [this](const FString& Line) { AppendLog(Line); });
	if (ListView.IsValid())
	{
		// 適用後のマテリアル(サムネイル)を反映するため、行を作り直す
		ListView->RebuildList();
	}
	return FReply::Handled();
}

void SPtAssignerPanel::Refresh()
{
	auto KeyOf = [](const FPtSlotEntry& E) -> FString
	{
		const UObject* M = E.Mesh.Get();
		return M ? FString::Printf(TEXT("%s#%d"), *M->GetPathName(), E.SlotIndex) : FString();
	};

	// 手動で差し込んだテクスチャは、再読み込みしても保持する
	TMap<FString, TMap<int32, FSoftObjectPath>> KeptManual;
	for (const TSharedPtr<FPtSlotEntry>& Old : Entries)
	{
		const FString Key = KeyOf(*Old);
		if (Key.IsEmpty()) { continue; }
		for (int32 i = 0; i < Old->Sources.Num(); ++i)
		{
			if (Old->Sources[i] == EPtTexSource::Manual && Old->TexturePaths.IsValidIndex(i))
			{
				KeptManual.FindOrAdd(Key).Add(i, Old->TexturePaths[i]);
			}
		}
	}

	const TArray<UObject*> Meshes = PtAssignerCore::GetSelectedMeshes();
	Entries = PtAssignerCore::Analyze(Settings, Meshes);

	for (const TSharedPtr<FPtSlotEntry>& E : Entries)
	{
		const TMap<int32, FSoftObjectPath>* Kept = KeptManual.Find(KeyOf(*E));
		if (!Kept) { continue; }
		for (const TPair<int32, FSoftObjectPath>& Pair : *Kept)
		{
			if (E->TexturePaths.IsValidIndex(Pair.Key))
			{
				E->TexturePaths[Pair.Key] = Pair.Value;
				E->Sources[Pair.Key] = EPtTexSource::Manual;
			}
		}
		PtAssignerCore::UpdateStatus(*E, Settings);
	}
	if (ListView.IsValid())
	{
		ListView->RebuildList();
	}
	if (Meshes.Num() == 0)
	{
		AppendLog(TEXT("スタティックメッシュ / スケルタルメッシュを選択して「選択を読み込む」を押してください。"));
	}
	else
	{
		AppendLog(FString::Printf(TEXT("読み込み：メッシュ %d 件 / スロット %d 件"), Meshes.Num(), Entries.Num()));
	}
}

void SPtAssignerPanel::AppendLog(const FString& Line)
{
	UE_LOG(LogTemp, Log, TEXT("[PtTextureAssigner] %s"), *Line);
	LogText += Line + TEXT("\n");
	if (LogBox.IsValid())
	{
		LogBox->SetText(FText::FromString(LogText));
	}
}
