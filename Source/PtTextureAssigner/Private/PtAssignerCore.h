#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "AssetRegistry/AssetData.h"

enum class EPtStatus : uint8
{
	Ready,      // 3枚そろっていて適用可能
	Missing,    // テクスチャ不足
	Ambiguous,  // 同名候補が複数
	Done,       // 適用済み
	Skipped,    // スキップ
	Failed      // 失敗
};

/** そのテクスチャ枠が どうやって埋まったか */
enum class EPtTexSource : uint8
{
	None,       // 空
	Auto,       // 名前検索で自動アタッチ
	Manual,     // ツール上で手動指定
	Ambiguous   // 名前検索で同名が複数 → 空のまま。手動で選んでもらう
};

/** 親マテリアルのパラメータ名 と テクスチャ名の末尾 */
struct FPtMapRow
{
	FString Param;
	FString Suffix;
};

struct FPtSettings
{
	FString TextureFolders = TEXT("/Game/Texture;/Game/Textures");
	FString MasterPath = TEXT("/Game/Materials/M_Master_PBR");
	FString InstanceFolder = TEXT("/Game/Materials/Instances");
	/** 0=BaseColor / 1=Normal / 2=ORM */
	TArray<FPtMapRow> Maps;
	bool bAutoFixTextureSettings = true;
	bool bSaveAfterApply = true;

	FPtSettings();
	void Load();
	void Save() const;
};

/** メッシュの1スロット = 1行 */
struct FPtSlotEntry
{
	TWeakObjectPtr<UObject> Mesh;
	int32 SlotIndex = 0;
	FString MeshName;
	FString SlotName;
	FString Prefix;
	FString InstanceName;
	TArray<FSoftObjectPath> TexturePaths; // Maps と同じ並び（自動検索でも手動でも、ここに入る）
	TArray<EPtTexSource> Sources;         // Maps と同じ並び
	/** このスロットに今入っているマテリアル（一覧のサムネイル表示用。適用後は作成したMIに更新） */
	FAssetData MaterialData;
	EPtStatus Status = EPtStatus::Missing;
	FString Message;
	bool bChecked = true;
};

namespace PtAssignerCore
{
	/** コンテンツブラウザで選択中の StaticMesh / SkeletalMesh */
	TArray<UObject*> GetSelectedMeshes();

	/** 書き込みはせず、各スロットの状態だけ調べる（プレビュー） */
	TArray<TSharedPtr<FPtSlotEntry>> Analyze(const FPtSettings& Settings, const TArray<UObject*>& Meshes);

	/** テクスチャ枠の内容から Status / Message / チェックを組み立て直す（手動で差し替えた後にも呼ぶ） */
	void UpdateStatus(FPtSlotEntry& Entry, const FPtSettings& Settings);

	/** チェックされた Ready 行を適用する */
	void Apply(const FPtSettings& Settings,
		const TArray<TSharedPtr<FPtSlotEntry>>& Entries,
		TFunctionRef<void(const FString&)> Log);
}
