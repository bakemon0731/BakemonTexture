#include "SPtAssignerPanel.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SComboButton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Styling/CoreStyle.h"
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
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Engine/Texture2D.h"

static FText J(const TCHAR* S) { return FText::FromString(S); }

namespace
{
	constexpr int32 ThumbSize = 64;
	constexpr float LineHeight = 1.35f; // 行間（1.0 = 詰まった状態）
}

// ---------------- 詳細欄 ----------------
// メッセージを1行ずつ見て、重要度に応じて色・大きさ・左帯を変える（不足 / 推定 / 設定の変更 / OK がひと目で分かるように）。
// メッセージや状態が変わったら自動で作り直す。

class SPtDetailCell : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPtDetailCell) {}
		SLATE_ARGUMENT(TSharedPtr<FPtSlotEntry>, Entry)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Entry = InArgs._Entry;
		Rebuild();
	}

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
	{
		SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
		if (Entry.IsValid() && (Entry->Message != CachedMessage || Entry->Status != CachedStatus))
		{
			Rebuild();
		}
	}

private:
	enum class ESev : uint8 { Normal, Good, Info, Warn, Bad, Dim };

	static ESev Classify(const FString& L, EPtStatus Status)
	{
		// 足りない・決められない：最優先で目立たせる
		if (L.StartsWith(TEXT("不足")) || L.StartsWith(TEXT("同名が複数：")) || L.StartsWith(TEXT("同じテクスチャが複数")))
		{
			return ESev::Bad;
		}
		// 確認してほしいもの
		if (L.Contains(TEXT("推定です")) || L.StartsWith(TEXT("未反映")))
		{
			return ESev::Warn;
		}
		// 任意マップの補足（空でも割り当てできる）
		if (L.Contains(TEXT("使う場合は右の枠から選んでください")))
		{
			return ESev::Dim;
		}
		// テクスチャ設定を変える予定 / 変えた内容
		if (L.StartsWith(TEXT("適用時にテクスチャ設定を変更")) || L.StartsWith(TEXT("テクスチャ設定を変更")) || L.StartsWith(TEXT("・")))
		{
			return ESev::Info;
		}
		if (L.Contains(TEXT("そろっています")))
		{
			return ESev::Good;
		}
		switch (Status)
		{
		case EPtStatus::Done:    return ESev::Good;
		case EPtStatus::Skipped: return ESev::Warn;
		case EPtStatus::Failed:  return ESev::Bad;
		default:                 return ESev::Normal;
		}
	}

	static FLinearColor ColorOf(ESev Sev)
	{
		switch (Sev)
		{
		case ESev::Good: return FLinearColor(0.35f, 0.85f, 0.45f);
		case ESev::Info: return FLinearColor(0.45f, 0.75f, 1.0f);
		case ESev::Warn: return FLinearColor(1.0f, 0.65f, 0.1f);
		case ESev::Bad:  return FLinearColor(1.0f, 0.38f, 0.35f);
		default:         return FLinearColor::White;
		}
	}

	void Rebuild()
	{
		CachedMessage = Entry->Message;
		CachedStatus = Entry->Status;

		// 表示する行。設定変更の行は「見出し + 項目ごとに1行」に分ける
		TArray<TPair<FString, ESev>> Lines;
		TArray<FString> Raw;
		CachedMessage.ParseIntoArray(Raw, TEXT("\n"), /*bCullEmpty*/ true);
		for (const FString& L : Raw)
		{
			if (L.StartsWith(TEXT("適用時にテクスチャ設定を変更")) || L.StartsWith(TEXT("テクスチャ設定を変更")))
			{
				const int32 Colon = L.Find(TEXT("："));
				if (Colon != INDEX_NONE)
				{
					Lines.Add(TPair<FString, ESev>(L.Left(Colon + 1), ESev::Info));
					TArray<FString> Items;
					L.Mid(Colon + 1).ParseIntoArray(Items, TEXT(" ／ "), /*bCullEmpty*/ true);
					for (FString& Item : Items)
					{
						Lines.Add(TPair<FString, ESev>(TEXT("・") + Item.TrimStartAndEnd(), ESev::Info));
					}
					continue;
				}
			}
			Lines.Add(TPair<FString, ESev>(L, Classify(L, Entry->Status)));
		}

		TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
		for (const TPair<FString, ESev>& Line : Lines)
		{
			const ESev Sev = Line.Value;
			const bool bBar = (Sev == ESev::Bad || Sev == ESev::Warn);
			const FSlateFontInfo Font =
				Sev == ESev::Bad ? FCoreStyle::GetDefaultFontStyle("Bold", 11) :
				Sev == ESev::Warn ? FCoreStyle::GetDefaultFontStyle("Bold", 10) :
				Sev == ESev::Dim ? FAppStyle::GetFontStyle("SmallFont") :
				FCoreStyle::GetDefaultFontStyle("Regular", 10);

			Box->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, bBar ? 6.f : 0.f, 0.f)
				[
					SNew(SBox).WidthOverride(bBar ? 3.f : 0.f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
						.BorderBackgroundColor(ColorOf(Sev))
						.Visibility(bBar ? EVisibility::Visible : EVisibility::Collapsed)
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(STextBlock)
					.Font(Font)
					.ColorAndOpacity(Sev == ESev::Dim ? FSlateColor::UseSubduedForeground() : FSlateColor(ColorOf(Sev)))
					.AutoWrapText(true)
					.LineHeightPercentage(LineHeight)
					.Text(FText::FromString(Line.Key))
				]
			];
		}
		ChildSlot
		[
			Box
		];
	}

	TSharedPtr<FPtSlotEntry> Entry;
	FString CachedMessage;
	EPtStatus CachedStatus = EPtStatus::Missing;
};

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
				.Text_Lambda([E]() { return StatusText(*E); })
				.ColorAndOpacity_Lambda([E]() { return FSlateColor(StatusColor(*E)); }));
		}
		// 詳細：重要度に応じて色・大きさを変えて1行ずつ表示する
		return Cell(SNew(SPtDetailCell).Entry(E));
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

	/** テクスチャ1枠分（ラベル + アセットピッカー）。ラベルには入力元（自動 / FBX / 推定 / 手動）を添える */
	TSharedRef<SWidget> MakeTexSlot(int32 i, bool bThumb)
	{
		const TSharedPtr<FPtSlotEntry> E = Entry;
		FPtSettings* S = Settings;
		const TSharedPtr<FAssetThumbnailPool> Pool = ThumbnailPool;
		const FString Label = (S && S->Maps.IsValidIndex(i)) ? S->Maps[i].Label : FString::Printf(TEXT("Tex%d"), i);

		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 3.f)
			[
				SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("SmallFont"))
					.ColorAndOpacity_Lambda([E, i]()
					{
						const EPtTexSource Src = E->Sources.IsValidIndex(i) ? E->Sources[i] : EPtTexSource::None;
						if (Src == EPtTexSource::Guess)     { return FSlateColor(FLinearColor(1.0f, 0.65f, 0.1f)); }
						if (Src == EPtTexSource::Ambiguous) { return FSlateColor(FLinearColor(1.0f, 0.4f, 0.2f)); }
						return FSlateColor::UseSubduedForeground();
					})
					.Text_Lambda([E, i, Label]()
					{
						const EPtTexSource Src = E->Sources.IsValidIndex(i) ? E->Sources[i] : EPtTexSource::None;
						const TCHAR* Tag = TEXT("");
						switch (Src)
						{
						case EPtTexSource::Auto:      Tag = TEXT("（自動）"); break;
						case EPtTexSource::Fbx:       Tag = TEXT("（FBX）"); break;
						case EPtTexSource::Guess:     Tag = TEXT("（推定・要確認）"); break;
						case EPtTexSource::Saved:     Tag = TEXT("（記憶）"); break;
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
					.DisplayThumbnail(bThumb)
					.DisplayBrowse(true)
					.DisplayUseSelected(bThumb)
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
						if (S && S->bRememberManual && S->Maps.IsValidIndex(i))
						{
							// 手動で選んだ対応は覚えておき、次回から同じメッシュ・スロットで使う。空にしたら記憶も消す
							if (bSet) { PtAssignerCore::RememberChoice(*E, S->Maps[i].Param, E->TexturePaths[i]); }
							else      { PtAssignerCore::ForgetChoice(*E, S->Maps[i].Param); }
						}
						if (S)
						{
							PtAssignerCore::UpdateStatus(*E, *S);
						}
					})
			];
	}

	/**
	 * テクスチャ枠。上段は必須の3枚（BaseColor / Normal / ORM）をサムネイル付きで、
	 * 下段に追加マップ（Emissive / Roughness / Metallic / AO / Opacity）を折り返しで並べる。
	 * 名前検索・FBX・推定で入ったものは最初から入っていて、差し替えや空欄の補完は自由。
	 */
	TSharedRef<SWidget> MakeTextureCell()
	{
		FPtSettings* S = Settings;
		const int32 Total = Entry->TexturePaths.Num();

		TSharedRef<SHorizontalBox> Main = SNew(SHorizontalBox);
		for (int32 i = 0; i < Total && i < PtRequiredMapCount; ++i)
		{
			Main->AddSlot().FillWidth(1.f).Padding(i > 0 ? 8.f : 0.f, 0.f, 0.f, 0.f)
			[
				MakeTexSlot(i, true)
			];
		}

		TSharedRef<SVerticalBox> TextureBox = SNew(SVerticalBox);
		if (Entry->Candidates.Num() > 0)
		{
			TextureBox->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SComboButton)
					.ToolTipText(J(TEXT("似た名前のテクスチャセットを近い順に見比べて、ワンクリックで割り当てます。選んだ対応は記憶されます")))
					.OnGetMenuContent_Lambda([this]() { return MakeCandidateMenu(); })
					.ButtonContent()
					[
						SNew(STextBlock)
						.Font(FAppStyle::GetFontStyle("SmallFont"))
						.Text(FText::FromString(FString::Printf(TEXT("候補を見る（%d）"), Entry->Candidates.Num())))
					]
				]
			];
		}
		TextureBox->AddSlot().AutoHeight()
		[
			Main
		];

		if (Total > PtRequiredMapCount)
		{
			// 追加マップは1行3個ずつ。最後の行が余る時は空きで幅をそろえる
			constexpr int32 PerRow = 3;
			TSharedRef<SVerticalBox> Extra = SNew(SVerticalBox);
			for (int32 RowStart = PtRequiredMapCount; RowStart < Total; RowStart += PerRow)
			{
				TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
				for (int32 c = 0; c < PerRow; ++c)
				{
					const int32 i = RowStart + c;
					Row->AddSlot().FillWidth(1.f).Padding(c > 0 ? 8.f : 0.f, 0.f, 0.f, 0.f)
					[
						i < Total ? MakeTexSlot(i, false) : SNullWidget::NullWidget
					];
				}
				Extra->AddSlot().AutoHeight().Padding(0.f, RowStart > PtRequiredMapCount ? 6.f : 0.f, 0.f, 0.f)
				[
					Row
				];
			}
			TextureBox->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
			[
				SNew(SVerticalBox)
				.Visibility_Lambda([S]() { return (S && S->bShowExtraMaps) ? EVisibility::Visible : EVisibility::Collapsed; })
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
				[
					SNew(STextBlock)
						.Font(FAppStyle::GetFontStyle("SmallFont"))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text(J(TEXT("追加マップ（任意。空のままでも割り当てできます）")))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					Extra
				]
			];
		}
		return Cell(TextureBox);
	}

	/** 候補一覧（サムネイル付き）。クリックで、そのセットの各テクスチャを手動指定として入れる */
	TSharedRef<SWidget> MakeCandidateMenu()
	{
		const TSharedPtr<FPtSlotEntry> E = Entry;
		FPtSettings* S = Settings;
		CandidateThumbs.Reset();
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
		for (int32 c = 0; c < E->Candidates.Num(); ++c)
		{
			const FPtCandidate& Cand = E->Candidates[c];

			TSharedRef<SHorizontalBox> Thumbs = SNew(SHorizontalBox);
			for (int32 i = 0; i < PtRequiredMapCount && i < Cand.Paths.Num(); ++i)
			{
				TSharedRef<SWidget> Preview = SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(FAppStyle::GetFontStyle("SmallFont"))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text(J(TEXT("なし")))
					];
				if (!Cand.Paths[i].IsNull() && ThumbnailPool.IsValid())
				{
					const FAssetData D = AR.GetAssetByObjectPath(Cand.Paths[i]);
					if (D.IsValid())
					{
						TSharedPtr<FAssetThumbnail> T = MakeShared<FAssetThumbnail>(D, 56, 56, ThumbnailPool);
						CandidateThumbs.Add(T);
						Preview = T->MakeThumbnailWidget();
					}
				}
				const FString Label = (S && S->Maps.IsValidIndex(i)) ? S->Maps[i].Label : FString::Printf(TEXT("Tex%d"), i);
				Thumbs->AddSlot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SBox).WidthOverride(56.f).HeightOverride(56.f)
						[
							Preview
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f).HAlign(HAlign_Center)
					[
						SNew(STextBlock)
						.Font(FAppStyle::GetFontStyle("SmallFont"))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text(FText::FromString(Label))
					]
				];
			}

			TArray<FString> Extra;
			for (int32 i = PtRequiredMapCount; i < Cand.Paths.Num(); ++i)
			{
				if (!Cand.Paths[i].IsNull() && S && S->Maps.IsValidIndex(i)) { Extra.Add(S->Maps[i].Label); }
			}
			const FString Place = FPackageName::GetShortName(Cand.Folder);
			const FString Info = FString::Printf(TEXT("類似度 %.2f ／ %s"), Cand.Score, *Place);
			const FString ExtraText = Extra.Num() > 0 ? (TEXT("追加マップ：") + FString::Join(Extra, TEXT(", "))) : FString(TEXT("追加マップなし"));

			List->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.f))
				.ToolTipText(J(TEXT("このセットのテクスチャを、この行の各枠に入れます（手動指定として記憶されます）")))
				.OnClicked_Lambda([E, S, c]()
				{
					if (!E->Candidates.IsValidIndex(c)) { return FReply::Handled(); }
					const FPtCandidate Chosen = E->Candidates[c];
					for (int32 i = 0; i < Chosen.Paths.Num() && i < E->TexturePaths.Num(); ++i)
					{
						if (Chosen.Paths[i].IsNull()) { continue; }
						E->TexturePaths[i] = Chosen.Paths[i];
						E->Sources[i] = EPtTexSource::Manual;
						if (S && S->bRememberManual && S->Maps.IsValidIndex(i))
						{
							PtAssignerCore::RememberChoice(*E, S->Maps[i].Param, Chosen.Paths[i]);
						}
					}
					if (S)
					{
						PtAssignerCore::UpdateStatus(*E, *S);
					}
					FSlateApplication::Get().DismissAllMenus();
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 12.f, 0.f)
					[
						Thumbs
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock)
							.Font(FAppStyle::GetFontStyle("BoldFont"))
							.Text(FText::FromString(Cand.SetName))
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f, 0.f, 0.f)
						[
							SNew(STextBlock)
							.Font(FAppStyle::GetFontStyle("SmallFont"))
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
							.Text(FText::FromString(Info))
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock)
							.Font(FAppStyle::GetFontStyle("SmallFont"))
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
							.Text(FText::FromString(ExtraText))
						]
					]
				]
			];
		}

		return SNew(SBox).MinDesiredWidth(420.f).MaxDesiredHeight(460.f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot().Padding(6.f)
			[
				List
			]
		];
	}

	static bool HasGuess(const FPtSlotEntry& E)
	{
		return E.Sources.Contains(EPtTexSource::Guess);
	}

	static FText StatusText(const FPtSlotEntry& E)
	{
		switch (E.Status)
		{
		case EPtStatus::Ready:     return HasGuess(E) ? J(TEXT("要確認")) : J(TEXT("準備OK"));
		case EPtStatus::Missing:   return J(TEXT("不足"));
		case EPtStatus::Ambiguous: return J(TEXT("重複"));
		case EPtStatus::Done:      return J(TEXT("完了"));
		case EPtStatus::Skipped:   return J(TEXT("スキップ"));
		default:                   return J(TEXT("失敗"));
		}
	}
	static FLinearColor StatusColor(const FPtSlotEntry& E)
	{
		switch (E.Status)
		{
		case EPtStatus::Ready:     return HasGuess(E) ? FLinearColor(1.0f, 0.65f, 0.1f) : FLinearColor(0.2f, 0.8f, 0.3f);
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
	TArray<TSharedPtr<FAssetThumbnail>> CandidateThumbs; // 候補メニューを開いている間のサムネイル
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

TSharedRef<SWidget> SPtAssignerPanel::MakeSection(const FString& Title, const TSharedRef<SWidget>& Body)
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 6)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
			.Padding(FMargin(10.f, 5.f))
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("BoldFont"))
				.Text(FText::FromString(Title))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 0, 0)
		[
			Body
		];
}

TSharedRef<SWidget> SPtAssignerPanel::MakeCheck(const FString& Label, const FString& Tip,
	TFunction<bool()> Get, TFunction<void(bool)> Set, bool bRefresh)
{
	return SNew(SCheckBox)
		.ToolTipText(FText::FromString(Tip))
		.IsChecked_Lambda([Get]() { return Get() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([this, Set, bRefresh](ECheckBoxState S)
		{
			Set(S == ECheckBoxState::Checked);
			Settings.Save();
			if (bRefresh) { Refresh(); }
		})
		[
			SNew(STextBlock).Text(FText::FromString(Label))
		];
}

void SPtAssignerPanel::Construct(const FArguments&)
{
	Settings.Load();
	ThumbnailPool = MakeShared<FAssetThumbnailPool>(128);

	// ---------- 設定の中身（見出しごとにまとめる）----------
	TSharedRef<SVerticalBox> MapRows = SNew(SVerticalBox);
	MapRows->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
	[
		SNew(STextBlock)
		.AutoWrapText(true)
		.LineHeightPercentage(LineHeight)
		.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		.Text(J(TEXT("追加マップ（任意）は、親マテリアルに同名のテクスチャパラメータがある時だけ反映します。親マテリアルを自動作成する場合は、追加マップ用のノード（「パラメータ名＋Enable」で切り替え）も作ります。")))
	];
	for (int32 i = 0; i < Settings.Maps.Num(); ++i)
	{
		const FString RowLabel = Settings.Maps[i].Label + (i >= PtRequiredMapCount ? TEXT("（任意）") : TEXT(""));
		MapRows->AddSlot().AutoHeight()
		[
			MakeMapRow(RowLabel, i)
		];
	}

	TSharedRef<SVerticalBox> SettingsBody = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeSection(TEXT("検索先と出力先"),
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
				])
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeSection(TEXT("自動割り当ての方法"),
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
				[
					MakeCheck(TEXT("FBXの参照情報でテクスチャを補う"),
						TEXT("名前が一致しない枠を、FBXが材質に接続している画像の名前（001.png など）から探して埋めます。検索先とメッシュのフォルダ以下から探します"),
						[this]() { return Settings.bUseFbx; }, [this](bool V) { Settings.bUseFbx = V; }, true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
				[
					MakeCheck(TEXT("名前が似たテクスチャセットから推定して補う（要確認）"),
						TEXT("名前もFBX参照も一致しない枠を、スロット名・材質名・FBXの画像名に似たテクスチャセットから推定して埋めます。間違える可能性があるため、推定を含む行はチェックを外した状態で表示します"),
						[this]() { return Settings.bUseGuess; }, [this](bool V) { Settings.bUseGuess = V; }, true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
				[
					MakeCheck(TEXT("手動で選んだ対応を記憶する（次回から同じメッシュ・スロットで自動的に使う）"),
						TEXT("ピッカーや「候補を見る」で選んだテクスチャを、メッシュ + スロット名 + マップごとに保存します。次に読み込む時は、名前検索やFBX参照より優先して入ります。枠を空にすると、その記憶は消えます"),
						[this]() { return Settings.bRememberManual; }, [this](bool V) { Settings.bRememberManual = V; }, true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 8)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
					[
						SNew(SButton)
						.ToolTipText(J(TEXT("記憶した手動の対応をすべて消します（元に戻せません）")))
						.IsEnabled_Lambda([]() { return PtAssignerCore::CountChoices() > 0; })
						.OnClicked_Lambda([this]() { PtAssignerCore::ClearAllChoices(); Refresh(); return FReply::Handled(); })
						[
							SNew(STextBlock).Text(J(TEXT("記憶をすべて消す")))
						]
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text_Lambda([]()
						{
							return FText::FromString(FString::Printf(TEXT("記憶：%d 件（%s）"),
								PtAssignerCore::CountChoices(), *PtAssignerCore::GetChoicesFilePath()));
						})
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeTextRow(TEXT("ORM扱いにするFBXのプロパティ名（, 区切り・部分一致）"),
						[this]() { return Settings.FbxOrmKeywords; },
						[this](const FString& V) { Settings.FbxOrmKeywords = V; })
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.LineHeightPercentage(LineHeight)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(J(TEXT("例：「orm」だけに絞ると、FBXの Roughness / Metallic / AO のプロパティを、ORMではなく個別の追加マップ枠へ割り当てます。")))
				])
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeSection(TEXT("マップ設定（親のパラメータ名 / ファイル名の末尾）"), MapRows)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeSection(TEXT("適用時の動作"),
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
				[
					MakeCheck(TEXT("割り当て時に、用途に合う読み込み設定（sRGB・圧縮）を自動で適用する"),
						TEXT("Painter から出した画像は UE に取り込むと sRGB オンになり、ORM などの数値マップだと金属や粗さの見え方が変わります。割り当てと同時に、枠の用途に合わせて sRGB と圧縮を直します。変更内容は一覧の「詳細」に事前に表示されます"),
						[this]() { return Settings.bAutoFixTextureSettings; }, [this](bool V) { Settings.bAutoFixTextureSettings = V; }, true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.LineHeightPercentage(LineHeight)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(J(TEXT("用途ごとの設定：\n・BaseColor / Emissive：sRGB オン・圧縮 Default（BC7 と HDR 系はそのまま）\n・Normal：sRGB オフ・圧縮 Normalmap\n・ORM / Roughness / Metallic / AO / Opacity：sRGB オフ・圧縮 Masks\n同じテクスチャが用途の違う枠で使われている場合は、どちらかが壊れるため設定を変更せず、ログで知らせます。")))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
				[
					MakeCheck(TEXT("適用後にアセットを保存する"), TEXT(""),
						[this]() { return Settings.bSaveAfterApply; }, [this](bool V) { Settings.bSaveAfterApply = V; }, false)
				])
		];

	// ---------- 画面全体 ----------
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.f)
		[
			SNew(SVerticalBox)

			// ヘッダー：状況のまとめ + 主要ボタン
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
				.Padding(FMargin(16.f, 12.f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock)
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
							.Text(J(TEXT("Pt テクスチャ割り当て")))
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 5, 0, 0)
						[
							SNew(STextBlock)
							.AutoWrapText(true)
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
							.Text(this, &SPtAssignerPanel::GetSummaryText)
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12, 0, 10, 0)
					[
						SNew(SButton)
						.ContentPadding(FMargin(14.f, 8.f))
						.ToolTipText(J(TEXT("コンテンツブラウザで選択中のメッシュを読み込み直します")))
						.OnClicked(this, &SPtAssignerPanel::OnRefreshClicked)
						[
							SNew(STextBlock).Text(J(TEXT("選択を読み込む")))
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SButton)
						.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("PrimaryButton"))
						.ContentPadding(FMargin(22.f, 9.f))
						.ToolTipText(J(TEXT("チェックが入った「準備OK」の行に、親マテリアルのインスタンスを作ってメッシュへ割り当てます")))
						.IsEnabled(this, &SPtAssignerPanel::CanApply)
						.OnClicked(this, &SPtAssignerPanel::OnApplyClicked)
						[
							SNew(STextBlock)
							.TextStyle(FAppStyle::Get(), "ButtonText")
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
							.Text(this, &SPtAssignerPanel::GetApplyLabel)
						]
					]
				]
			]

			// 選択の操作 + 表示の切り替え
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.ToolTipText(J(TEXT("「準備OK」の行をすべてチェックします（推定を含む行も入ります）")))
					.OnClicked_Lambda([this]() { return OnCheckAllClicked(true); })
					[
						SNew(STextBlock).Text(J(TEXT("準備OKをすべて選択")))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton)
					.OnClicked_Lambda([this]() { return OnCheckAllClicked(false); })
					[
						SNew(STextBlock).Text(J(TEXT("選択を解除")))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(16, 0, 0, 0)
				[
					SNew(SButton)
					.ToolTipText(J(TEXT("割り当てはせず、一覧に入っているテクスチャの sRGB・圧縮だけを用途に合わせて直します（変更内容はログに出ます）")))
					.IsEnabled_Lambda([this]() { return Entries.Num() > 0; })
					.OnClicked(this, &SPtAssignerPanel::OnFixSettingsClicked)
					[
						SNew(STextBlock).Text(J(TEXT("テクスチャ設定だけ直す")))
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNullWidget::NullWidget
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 16, 0)
				[
					MakeCheck(TEXT("追加マップを表示"),
						TEXT("Emissive / Roughness / Metallic / AO / Opacity の枠を一覧に表示します"),
						[this]() { return Settings.bShowExtraMaps; }, [this](bool V) { Settings.bShowExtraMaps = V; }, false)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					MakeCheck(TEXT("選択に自動追従"),
						TEXT("コンテンツブラウザでメッシュを選ぶと、一覧を自動で読み込み直します"),
						[this]() { return Settings.bFollowSelection; }, [this](bool V) { Settings.bFollowSelection = V; }, false)
				]
			]

			// FBX の参照元
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
				[
					SNew(SButton)
					.ToolTipText(J(TEXT("インポート元のFBXが見つからない時や、別のFBXの参照情報を使いたい時に選びます（選択中の全メッシュに使います）")))
					.OnClicked(this, &SPtAssignerPanel::OnPickFbxClicked)
					[
						SNew(STextBlock).Text(J(TEXT("FBXを指定…")))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
				[
					SNew(SButton)
					.ToolTipText(J(TEXT("指定したFBXをやめて、メッシュごとのインポート元FBXを使います")))
					.IsEnabled_Lambda([this]() { return !Settings.FbxOverridePath.IsEmpty(); })
					.OnClicked(this, &SPtAssignerPanel::OnClearFbxClicked)
					[
						SNew(STextBlock).Text(J(TEXT("解除")))
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text_Lambda([this]()
					{
						return Settings.FbxOverridePath.IsEmpty()
							? FText::FromString(TEXT("FBX参照：メッシュごとのインポート元を使用"))
							: FText::FromString(FString::Printf(TEXT("FBX参照：%s（選択中の全メッシュに使用。別のメッシュを選ぶと解除されます）"), *FPaths::GetCleanFilename(Settings.FbxOverridePath)));
					})
				]
			]

			// 使い方 / 設定（普段は閉じておく）
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(SExpandableArea)
				.InitiallyCollapsed(true)
				.AreaTitle(J(TEXT("使い方")))
				.BodyContent()
				[
					SNew(SBox).Padding(FMargin(8.f, 6.f))
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.LineHeightPercentage(LineHeight)
						.Text(J(TEXT("① メッシュを選択する（右クリック →「Pt テクスチャ割り当て」。開いたままなら選択に自動追従します）\n② 名前が一致したテクスチャ、FBXが材質につないでいる画像（001.png など）、似た名前のセットから自動で入ります。違うものや空欄は各枠のピッカーから選べます（ドラッグ&ドロップも可）\n③ Emissive / Roughness / Metallic / AO / Opacity は任意です。同じセットにあれば自動で入り、無ければ空のままで割り当てできます\n④ 自動で決まらない行には「候補を見る」が出ます。似たテクスチャセットがサムネイル付きで近い順に並ぶので、クリックで割り当てられます\n⑤ 「要確認」（オレンジ）の行は推定です。サムネイルで確認してからチェックを入れてください\n⑥ ピッカーや候補で手動で選んだ対応は記憶され、次回から同じメッシュ・スロットで自動的に使われます（ラベルに「記憶」と出ます）\n⑦ 右上の「割り当てる」を押す")))
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SExpandableArea)
				.InitiallyCollapsed(true)
				.AreaTitle(J(TEXT("設定")))
				.BodyContent()
				[
					// 設定は項目が多いので、高さに上限を付けてスクロールできるようにする（右にスクロールバー分の余白）
					SNew(SBox).MaxDesiredHeight(360.f)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							SNew(SBox).Padding(FMargin(8.f, 0.f, 18.f, 8.f))
							[
								SettingsBody
							]
						]
					]
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
					+ SHeaderRow::Column("Mesh").DefaultLabel(J(TEXT("メッシュ"))).FillWidth(0.11f)
					+ SHeaderRow::Column("Slot").DefaultLabel(J(TEXT("スロット"))).FillWidth(0.10f)
					+ SHeaderRow::Column("Status").DefaultLabel(J(TEXT("状態"))).FixedWidth(80.f)
					+ SHeaderRow::Column("Textures").DefaultLabel(J(TEXT("テクスチャ（必須3種 + 追加マップ。自由に差し替え可）"))).FillWidth(0.41f)
					+ SHeaderRow::Column("Message").DefaultLabel(J(TEXT("詳細"))).FillWidth(0.38f)
				)
			]

			// ログ
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
			[
				SNew(SExpandableArea)
				.InitiallyCollapsed(false)
				.AreaTitle(J(TEXT("ログ")))
				.BodyContent()
				[
					SNew(SBox).MaxDesiredHeight(130.f)
					[
						SAssignNew(LogBox, SMultiLineEditableTextBox)
						.IsReadOnly(true)
					]
				]
			]
		]
	];

	Refresh();
}

TSharedRef<ITableRow> SPtAssignerPanel::OnGenerateRow(TSharedPtr<FPtSlotEntry> Item, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SPtEntryRow, Owner).Entry(Item).ThumbnailPool(ThumbnailPool).Settings(&Settings);
}

int32 SPtAssignerPanel::CountToApply() const
{
	int32 N = 0;
	for (const TSharedPtr<FPtSlotEntry>& E : Entries)
	{
		if (E->bChecked && E->Status == EPtStatus::Ready) { ++N; }
	}
	return N;
}

bool SPtAssignerPanel::CanApply() const
{
	return CountToApply() > 0;
}

FText SPtAssignerPanel::GetApplyLabel() const
{
	const int32 N = CountToApply();
	return N > 0 ? FText::FromString(FString::Printf(TEXT("割り当てる（%d件）"), N)) : J(TEXT("割り当てる"));
}

FText SPtAssignerPanel::GetSummaryText() const
{
	if (Entries.Num() == 0)
	{
		return J(TEXT("コンテンツブラウザでメッシュを選択すると、ここに結果が表示されます"));
	}
	int32 Ready = 0, Check = 0, Missing = 0, Amb = 0, Done = 0, Other = 0, FixSlots = 0;
	for (const TSharedPtr<FPtSlotEntry>& E : Entries)
	{
		if (E->SettingsChanges.Num() > 0) { ++FixSlots; }
		switch (E->Status)
		{
		case EPtStatus::Ready:     if (E->Sources.Contains(EPtTexSource::Guess)) { ++Check; } else { ++Ready; } break;
		case EPtStatus::Missing:   ++Missing; break;
		case EPtStatus::Ambiguous: ++Amb; break;
		case EPtStatus::Done:      ++Done; break;
		default:                   ++Other; break;
		}
	}
	TArray<FString> Parts;
	Parts.Add(FString::Printf(TEXT("スロット %d 件"), Entries.Num()));
	if (Ready > 0)   { Parts.Add(FString::Printf(TEXT("準備OK %d"), Ready)); }
	if (Check > 0)   { Parts.Add(FString::Printf(TEXT("要確認（推定）%d"), Check)); }
	if (Missing > 0) { Parts.Add(FString::Printf(TEXT("不足 %d"), Missing)); }
	if (Amb > 0)     { Parts.Add(FString::Printf(TEXT("重複 %d"), Amb)); }
	if (Done > 0)    { Parts.Add(FString::Printf(TEXT("完了 %d"), Done)); }
	if (Other > 0)   { Parts.Add(FString::Printf(TEXT("スキップ/失敗 %d"), Other)); }
	if (FixSlots > 0) { Parts.Add(FString::Printf(TEXT("テクスチャ設定を直す行 %d"), FixSlots)); }
	return FText::FromString(FString::Join(Parts, TEXT("   ／   ")));
}

FReply SPtAssignerPanel::OnFixSettingsClicked()
{
	Settings.Save();
	PtAssignerCore::FixTextureSettingsOnly(Settings, Entries, [this](const FString& Line) { AppendLog(Line); });
	Refresh(); // 直した後の状態で一覧とプレビューを作り直す
	return FReply::Handled();
}

FReply SPtAssignerPanel::OnCheckAllClicked(bool bOn)
{
	for (const TSharedPtr<FPtSlotEntry>& E : Entries)
	{
		if (E->Status == EPtStatus::Ready) { E->bChecked = bOn; }
	}
	return FReply::Handled();
}

FReply SPtAssignerPanel::OnRefreshClicked()
{
	Refresh();
	return FReply::Handled();
}

FReply SPtAssignerPanel::OnPickFbxClicked()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (DesktopPlatform)
	{
		TArray<FString> Files;
		const void* ParentWindow = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
		const bool bPicked = DesktopPlatform->OpenFileDialog(
			ParentWindow,
			TEXT("FBXファイルを選択"),
			FPaths::GetPath(Settings.FbxOverridePath),
			TEXT(""),
			TEXT("FBX (*.fbx)|*.fbx"),
			EFileDialogFlags::None,
			Files);
		if (bPicked && Files.Num() > 0)
		{
			Settings.FbxOverridePath = Files[0];
			FbxPickedSignature = PtAssignerCore::GetSelectedMeshSignature(); // このメッシュの選択に対してだけ有効
			Settings.bUseFbx = true;
			Settings.Save();
			Refresh();
		}
	}
	return FReply::Handled();
}

FReply SPtAssignerPanel::OnClearFbxClicked()
{
	Settings.FbxOverridePath.Reset();
	FbxPickedSignature.Reset();
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
	LastSelectionSignature = PtAssignerCore::GetSelectedMeshSignature();

	// FBXを指定した後に別のメッシュを選んだら、指定を解除する（前のメッシュ用のFBXを新しいメッシュに使い回さないため）。
	// 何も選ばれていない時は変えない。メッシュ未選択のまま指定した場合は、最初に選んだメッシュを対象にする
	if (!Settings.FbxOverridePath.IsEmpty() && FbxPickedSignature.IsEmpty())
	{
		FbxPickedSignature = LastSelectionSignature;
	}
	if (!Settings.FbxOverridePath.IsEmpty() && !LastSelectionSignature.IsEmpty()
		&& LastSelectionSignature != FbxPickedSignature)
	{
		AppendLog(FString::Printf(TEXT("メッシュの選択が変わったため、FBXの指定（%s）を解除しました"),
			*FPaths::GetCleanFilename(Settings.FbxOverridePath)));
		Settings.FbxOverridePath.Reset();
		FbxPickedSignature.Reset();
	}

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
	TArray<FString> Notes;
	Entries = PtAssignerCore::Analyze(Settings, Meshes, &Notes);

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
	for (const FString& Note : Notes)
	{
		AppendLog(Note);
	}
}

void SPtAssignerPanel::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	if (!Settings.bFollowSelection || InCurrentTime < NextPollTime)
	{
		return;
	}
	NextPollTime = InCurrentTime + 0.3;

	// メッシュが選ばれていて、前回と変わった時だけ読み込み直す（空選択では一覧を消さない）
	const FString Sig = PtAssignerCore::GetSelectedMeshSignature();
	if (!Sig.IsEmpty() && Sig != LastSelectionSignature)
	{
		Refresh();
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
