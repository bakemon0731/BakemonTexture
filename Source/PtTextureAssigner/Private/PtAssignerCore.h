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
	Fbx,        // FBXの参照情報（材質に接続された画像名）から自動アタッチ
	Guess,      // 名前もFBX参照も一致しないため、似た名前のテクスチャセットから推定（要確認）
	Saved,      // 前に手動で選んだ対応を記憶していて、それを使った
	Manual,     // ツール上で手動指定
	Ambiguous   // 名前検索で同名が複数 → 空のまま。手動で選んでもらう
};

/** Maps の先頭からこの数までは必須（BaseColor / Normal / ORM）。以降は任意（あれば割り当て、無くても準備OK） */
constexpr int32 PtRequiredMapCount = 3;

/** 親マテリアルのパラメータ名 と テクスチャ名の末尾 */
struct FPtMapRow
{
	FString Param;
	FString Suffix;
	FString Label;  // 画面に出す名前（保存しない）
};

struct FPtSettings
{
	FString TextureFolders = TEXT("/Game/Texture;/Game/Textures");
	FString MasterPath = TEXT("/Game/Materials/M_Master_PBR");
	FString InstanceFolder = TEXT("/Game/Materials/Instances");
	/** 0=BaseColor / 1=Normal / 2=ORM（ここまで必須）/ 3=Emissive / 4=Roughness / 5=Metallic / 6=AO / 7=Opacity（任意） */
	TArray<FPtMapRow> Maps;
	/** ツール上で手動で選んだテクスチャを覚えて、次回から同じメッシュ・スロットに自動で使う */
	bool bRememberManual = true;
	/** 一覧に追加マップ（Emissive など）の枠を表示する */
	bool bShowExtraMaps = true;
	bool bAutoFixTextureSettings = true;
	bool bSaveAfterApply = true;
	/** コンテンツブラウザの選択に合わせて、一覧を自動で読み込み直す */
	bool bFollowSelection = true;
	/** FBXの参照情報（材質に接続された画像）で、名前検索で埋まらない枠を補う */
	bool bUseFbx = true;
	/** FBXのプロパティ名にこの語（, 区切り・部分一致）が含まれていたら ORM 枠として扱う */
	FString FbxOrmKeywords = TEXT("orm,occlusion,rough,metal");
	/** 名前検索・FBX参照の両方で埋まらない枠を、スロット名/材質名/FBX画像名に似たテクスチャセットから推定して埋める（要確認） */
	bool bUseGuess = true;
	/** 指定すると、インポート元ではなくこのFBXを全メッシュに使う（保存しない） */
	FString FbxOverridePath;

	FPtSettings();
	void Load();
	void Save() const;
};

/** 「候補を見る」に出す、似たテクスチャセット1つ分 */
struct FPtCandidate
{
	FString SetName;                 // セット名（末尾の BaseColor 等を除いたもの）
	FString Folder;                  // 場所（表示用）
	float Score = 0.f;               // 名前の類似度 0〜1
	TArray<FSoftObjectPath> Paths;   // Maps と同じ並び（セットに無い枠は空）
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
	/** Maps と同じ並び。空の枠の理由（FBXの参照先が見つからない等）。重複時は重複した名前 */
	TArray<FString> Hints;
	/** 自動で決まらない時の見比べ用。似たテクスチャセットを近い順に並べたもの（空なら候補なし） */
	TArray<FPtCandidate> Candidates;
	/** 適用時に直されるテクスチャ設定（sRGB・圧縮）の予定。例：「ORM：sRGB オン→オフ / 圧縮 Default→Masks」。空なら変更なし */
	TArray<FString> SettingsChanges;
	/** このスロットに今入っているマテリアル（一覧のサムネイル表示用。適用後は作成したMIに更新） */
	FAssetData MaterialData;
	EPtStatus Status = EPtStatus::Missing;
	FString Message;
	bool bChecked = true;
};

namespace PtAssignerCore
{
	/** 手動で選んだ対応を記憶する（メッシュ + スロット名 + マップのパラメータ名 → テクスチャ）。ファイルに保存される */
	void RememberChoice(const FPtSlotEntry& Entry, const FString& Param, const FSoftObjectPath& Texture);
	/** 記憶を1件消す */
	void ForgetChoice(const FPtSlotEntry& Entry, const FString& Param);
	/** 記憶をすべて消す */
	void ClearAllChoices();
	int32 CountChoices();
	FString GetChoicesFilePath();

	/** コンテンツブラウザで選択中の StaticMesh / SkeletalMesh */
	TArray<UObject*> GetSelectedMeshes();

	/** 選択中メッシュの識別文字列（アセットは読み込まない軽量版。選択が変わったかの判定用） */
	FString GetSelectedMeshSignature();

	/** 書き込みはせず、各スロットの状態だけ調べる（プレビュー） */
	TArray<TSharedPtr<FPtSlotEntry>> Analyze(const FPtSettings& Settings, const TArray<UObject*>& Meshes, TArray<FString>* OutNotes = nullptr);

	/** テクスチャ枠の内容から Status / Message / チェックを組み立て直す（手動で差し替えた後にも呼ぶ） */
	void UpdateStatus(FPtSlotEntry& Entry, const FPtSettings& Settings);

	/** 割り当てはせず、今入っているテクスチャの sRGB・圧縮だけを用途に合わせて直す（全行が対象） */
	void FixTextureSettingsOnly(const FPtSettings& Settings,
		const TArray<TSharedPtr<FPtSlotEntry>>& Entries,
		TFunctionRef<void(const FString&)> Log);

	/** チェックされた Ready 行を適用する */
	void Apply(const FPtSettings& Settings,
		const TArray<TSharedPtr<FPtSlotEntry>>& Entries,
		TFunctionRef<void(const FString&)> Log);
}
