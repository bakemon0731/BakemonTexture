#include "PtAssignerCore.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "MaterialEditingLibrary.h"
#include "FileHelpers.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/PackageName.h"
#include "Misc/ConfigCacheIni.h"

namespace
{
	enum EMapIndex { MapBase = 0, MapNormal = 1, MapOrm = 2, MapCount = 3 };
	const TCHAR* ConfigSection = TEXT("PtTextureAssigner");

	enum class EResult { Ok, Skip, Fail };

	IAssetRegistry& GetRegistry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	}

	FSoftObjectPath ObjectPathOf(const FString& PackagePath)
	{
		return FSoftObjectPath(PackagePath + TEXT(".") + FPackageName::GetShortName(PackagePath));
	}

	void GetSlotNames(UObject* Mesh, TArray<FName>& Out)
	{
		if (UStaticMesh* SM = Cast<UStaticMesh>(Mesh))
		{
			for (const FStaticMaterial& M : SM->GetStaticMaterials())
			{
				Out.Add(M.MaterialSlotName);
			}
		}
		else if (USkeletalMesh* SK = Cast<USkeletalMesh>(Mesh))
		{
			for (const FSkeletalMaterial& M : SK->GetMaterials())
			{
				Out.Add(M.MaterialSlotName);
			}
		}
	}

	UMaterialInterface* GetSlotMaterial(UObject* Mesh, int32 SlotIndex)
	{
		if (UStaticMesh* SM = Cast<UStaticMesh>(Mesh))
		{
			return SM->GetMaterial(SlotIndex);
		}
		if (USkeletalMesh* SK = Cast<USkeletalMesh>(Mesh))
		{
			const TArray<FSkeletalMaterial>& Mats = SK->GetMaterials();
			return Mats.IsValidIndex(SlotIndex) ? Mats[SlotIndex].MaterialInterface.Get() : nullptr;
		}
		return nullptr;
	}

	void FixTexture(UTexture2D* Tex, TextureCompressionSettings Compression, bool bSRGB, TArray<UPackage*>& Dirty)
	{
		if (Tex->CompressionSettings == Compression && Tex->SRGB == bSRGB)
		{
			return;
		}
		Tex->Modify();
		Tex->CompressionSettings = Compression;
		Tex->SRGB = bSRGB;
		Tex->PostEditChange();
		Tex->MarkPackageDirty();
		Dirty.AddUnique(Tex->GetOutermost());
	}

	bool ValidateMaster(const FPtSettings& S, UMaterialInterface* Master, FString& Err)
	{
		TSet<FString> Available;
		TArray<FName> TexParamNames;
		UMaterialEditingLibrary::GetTextureParameterNames(Master, TexParamNames);
		for (const FName& N : TexParamNames)
		{
			Available.Add(N.ToString().ToLower());
		}
		TArray<FString> Missing;
		for (const FPtMapRow& Row : S.Maps)
		{
			if (!Available.Contains(Row.Param.ToLower()))
			{
				Missing.Add(Row.Param);
			}
		}
		if (Missing.Num() > 0)
		{
			Err = FString::Printf(TEXT("親マテリアルにテクスチャパラメータが不足：%s"),
				*FString::Join(Missing, TEXT(", ")));
			return false;
		}
		return true;
	}

	UMaterial* CreateMaster(const FPtSettings& S, UTexture2D* Tex[MapCount], TArray<UPackage*>& Dirty, FString& Err)
	{
		IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		const FString Folder = FPackageName::GetLongPackagePath(S.MasterPath);
		const FString Name = FPackageName::GetShortName(S.MasterPath);

		UMaterial* M = Cast<UMaterial>(AT.CreateAsset(Name, Folder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!M)
		{
			Err = FString::Printf(TEXT("親マテリアルを作成できません：%s"), *S.MasterPath);
			return nullptr;
		}

		struct FOut { const TCHAR* Pin; EMaterialProperty Prop; };
		struct FNodeDef { EMaterialSamplerType Sampler; TArray<FOut> Outs; };
		const FNodeDef Defs[MapCount] =
		{
			{ SAMPLERTYPE_Color,  { { TEXT(""), MP_BaseColor } } },
			{ SAMPLERTYPE_Normal, { { TEXT(""), MP_Normal } } },
			{ SAMPLERTYPE_Masks,  { { TEXT("R"), MP_AmbientOcclusion }, { TEXT("G"), MP_Roughness }, { TEXT("B"), MP_Metallic } } },
		};

		for (int32 Row = 0; Row < MapCount; ++Row)
		{
			UMaterialExpressionTextureSampleParameter2D* Node = Cast<UMaterialExpressionTextureSampleParameter2D>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -400, Row * 250));
			if (!Node)
			{
				Err = FString::Printf(TEXT("親のノード作成失敗：%s"), *S.Maps[Row].Param);
				return nullptr;
			}
			Node->ParameterName = FName(*S.Maps[Row].Param);
			Node->SamplerType = Defs[Row].Sampler;
			Node->Texture = Tex[Row];

			for (const FOut& O : Defs[Row].Outs)
			{
				if (!UMaterialEditingLibrary::ConnectMaterialProperty(Node, O.Pin, O.Prop))
				{
					Err = FString::Printf(TEXT("親のノード接続失敗：%s %s"), *S.Maps[Row].Param, O.Pin);
					return nullptr;
				}
			}
		}

		UMaterialEditingLibrary::RecompileMaterial(M);
		if (!ValidateMaster(S, M, Err))
		{
			return nullptr;
		}
		M->MarkPackageDirty();
		Dirty.AddUnique(M->GetOutermost());
		return M;
	}

	EResult ApplyOne(const FPtSettings& S, FPtSlotEntry& E, UMaterialInterface*& Master,
		TArray<UPackage*>& Dirty, FString& Msg)
	{
		UObject* Mesh = E.Mesh.Get();
		if (!Mesh)
		{
			Msg = TEXT("メッシュが無効です");
			return EResult::Fail;
		}

		UTexture2D* Tex[MapCount] = {};
		for (int32 i = 0; i < MapCount; ++i)
		{
			Tex[i] = Cast<UTexture2D>(E.TexturePaths[i].TryLoad());
			if (!Tex[i])
			{
				Msg = FString::Printf(TEXT("Texture2D を読み込めません：%s"), *E.TexturePaths[i].ToString());
				return EResult::Skip;
			}
		}

		// 同じテクスチャが複数の枠に入っていたら、圧縮設定を書き換える前に止める（Normal / ORM の自動修正で壊れるため）
		for (int32 i = 0; i < MapCount; ++i)
		{
			for (int32 j = i + 1; j < MapCount; ++j)
			{
				if (Tex[i] == Tex[j])
				{
					Msg = FString::Printf(TEXT("%s と %s に同じテクスチャ（%s）が入っています。別々のテクスチャを指定してください"),
						*S.Maps[i].Param, *S.Maps[j].Param, *Tex[i]->GetName());
					return EResult::Skip;
				}
			}
		}

		// 既存インスタンスの確認
		UMaterialInstanceConstant* MI = nullptr;
		const FString InstancePath = S.InstanceFolder / E.InstanceName;
		const FAssetData Existing = GetRegistry().GetAssetByObjectPath(ObjectPathOf(InstancePath));
		if (Existing.IsValid())
		{
			MI = Cast<UMaterialInstanceConstant>(Existing.GetAsset());
			if (!MI)
			{
				Msg = FString::Printf(TEXT("同名の別種アセットがあります：%s"), *InstancePath);
				return EResult::Skip;
			}
			if (!Master || MI->Parent != Master)
			{
				Msg = FString::Printf(TEXT("既存インスタンスの親が異なります：%s"), *InstancePath);
				return EResult::Skip;
			}
		}

		// テクスチャ設定
		if (S.bAutoFixTextureSettings)
		{
			FixTexture(Tex[MapNormal], TC_Normalmap, false, Dirty);
			FixTexture(Tex[MapOrm], TC_Masks, false, Dirty);
			{
				// 以前の誤設定で Normalmap / Masks になった BaseColor を、通常のカラー設定（sRGB）に戻す
				const TextureCompressionSettings Cur = Tex[MapBase]->CompressionSettings;
				if (Cur == TC_Normalmap || Cur == TC_Masks)
				{
					FixTexture(Tex[MapBase], TC_Default, true, Dirty);
				}
			}
		}
		else if (!Master && Tex[MapNormal]->CompressionSettings != TC_Normalmap)
		{
			Msg = FString::Printf(TEXT("%s の Compression Settings を Normalmap にして再実行してください"),
				*Tex[MapNormal]->GetName());
			return EResult::Skip;
		}

		// 親が無ければ最初の1セットで作る
		if (!Master)
		{
			FString Err;
			Master = CreateMaster(S, Tex, Dirty, Err);
			if (!Master)
			{
				Msg = Err;
				return EResult::Fail;
			}
		}

		// インスタンス作成と設定
		bool bCreated = false;
		if (!MI)
		{
			IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
			MI = Cast<UMaterialInstanceConstant>(AT.CreateAsset(
				E.InstanceName, S.InstanceFolder,
				UMaterialInstanceConstant::StaticClass(), NewObject<UMaterialInstanceConstantFactoryNew>()));
			if (!MI)
			{
				Msg = FString::Printf(TEXT("インスタンス作成失敗：%s"), *E.InstanceName);
				return EResult::Fail;
			}
			UMaterialEditingLibrary::SetMaterialInstanceParent(MI, Master);
			bCreated = true;
		}

		const TArray<FTextureParameterValue> Previous = MI->TextureParameterValues;

		// 1) まず通常のAPIで設定
		for (int32 i = 0; i < MapCount; ++i)
		{
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MI, FName(*S.Maps[i].Param), Tex[i]);
		}
		UMaterialEditingLibrary::UpdateMaterialInstance(MI);

		// 2) インスタンス自身が「上書き」として持っているかを直接確認する。
		//    （GetMaterialInstanceTextureParameterValue は親の初期値も返すため、
		//      上書きが入っていなくても、親の初期値と同じなら一致してしまう）
		auto HasOverride = [MI](const FName Param, const UTexture* Expected) -> bool
		{
			for (const FTextureParameterValue& V : MI->TextureParameterValues)
			{
				if (V.ParameterInfo.Name == Param && V.ParameterValue == Expected)
				{
					return true;
				}
			}
			return false;
		};

		// 3) 入っていなければ直接書き込む（取りこぼし対策）
		bool bPatched = false;
		for (int32 i = 0; i < MapCount; ++i)
		{
			const FName Param(*S.Maps[i].Param);
			if (HasOverride(Param, Tex[i]))
			{
				continue;
			}
			bool bFound = false;
			for (FTextureParameterValue& V : MI->TextureParameterValues)
			{
				if (V.ParameterInfo.Name == Param)
				{
					V.ParameterValue = Tex[i];
					bFound = true;
				}
			}
			if (!bFound)
			{
				FTextureParameterValue NewValue;
				NewValue.ParameterInfo = FMaterialParameterInfo(Param);
				NewValue.ParameterValue = Tex[i];
				MI->TextureParameterValues.Add(NewValue);
			}
			bPatched = true;
		}
		if (bPatched)
		{
			UMaterialEditingLibrary::UpdateMaterialInstance(MI);
		}

		// 4) 最終確認。それでも入っていなければ元に戻して失敗にする
		FString Confirm;
		for (int32 i = 0; i < MapCount; ++i)
		{
			const FName Param(*S.Maps[i].Param);
			if (!HasOverride(Param, Tex[i]))
			{
				MI->TextureParameterValues = Previous;
				UMaterialEditingLibrary::UpdateMaterialInstance(MI);
				Msg = FString::Printf(TEXT("%s にテクスチャを設定できませんでした（親マテリアルに同名のテクスチャパラメータがあるか確認してください）"), *S.Maps[i].Param);
				return EResult::Fail;
			}
			Confirm += FString::Printf(TEXT("%s%s=%s"), i > 0 ? TEXT(", ") : TEXT(""), *S.Maps[i].Param, *Tex[i]->GetName());
		}
		MI->MarkPackageDirty();
		Dirty.AddUnique(MI->GetOutermost());

		// メッシュへ割り当て
		Mesh->Modify();
		if (UStaticMesh* SM = Cast<UStaticMesh>(Mesh))
		{
			SM->SetMaterial(E.SlotIndex, MI);
		}
		else if (USkeletalMesh* SK = Cast<USkeletalMesh>(Mesh))
		{
			TArray<FSkeletalMaterial>& Mats = SK->GetMaterials();
			if (Mats.IsValidIndex(E.SlotIndex))
			{
				Mats[E.SlotIndex].MaterialInterface = MI;
			}
			SK->MarkPackageDirty();
			SK->PostEditChange();
		}
		Dirty.AddUnique(Mesh->GetOutermost());
		E.MaterialData = FAssetData(MI);

		Msg = FString::Printf(TEXT("%s [%s]"),
			bCreated ? TEXT("インスタンスを作成して割り当て") : TEXT("既存インスタンスを更新して割り当て"), *Confirm);
		return EResult::Ok;
	}
}

// ---------------- 設定 ----------------

FPtSettings::FPtSettings()
{
	Maps = {
		{ TEXT("BasecolorTex"), TEXT("BaseColor") },
		{ TEXT("NormalTex"),    TEXT("Normal") },
		{ TEXT("ORMTex"),       TEXT("OcclusionRoughnessMetallic") },
	};
}

void FPtSettings::Load()
{
	GConfig->GetString(ConfigSection, TEXT("TextureFolders"), TextureFolders, GEditorPerProjectIni);
	GConfig->GetString(ConfigSection, TEXT("MasterPath"), MasterPath, GEditorPerProjectIni);
	GConfig->GetString(ConfigSection, TEXT("InstanceFolder"), InstanceFolder, GEditorPerProjectIni);
	GConfig->GetBool(ConfigSection, TEXT("AutoFix"), bAutoFixTextureSettings, GEditorPerProjectIni);
	GConfig->GetBool(ConfigSection, TEXT("SaveAfterApply"), bSaveAfterApply, GEditorPerProjectIni);
	GConfig->GetBool(ConfigSection, TEXT("FollowSelection"), bFollowSelection, GEditorPerProjectIni);
	for (int32 i = 0; i < Maps.Num(); ++i)
	{
		GConfig->GetString(ConfigSection, *FString::Printf(TEXT("Param%d"), i), Maps[i].Param, GEditorPerProjectIni);
		GConfig->GetString(ConfigSection, *FString::Printf(TEXT("Suffix%d"), i), Maps[i].Suffix, GEditorPerProjectIni);
	}
}

void FPtSettings::Save() const
{
	GConfig->SetString(ConfigSection, TEXT("TextureFolders"), *TextureFolders, GEditorPerProjectIni);
	GConfig->SetString(ConfigSection, TEXT("MasterPath"), *MasterPath, GEditorPerProjectIni);
	GConfig->SetString(ConfigSection, TEXT("InstanceFolder"), *InstanceFolder, GEditorPerProjectIni);
	GConfig->SetBool(ConfigSection, TEXT("AutoFix"), bAutoFixTextureSettings, GEditorPerProjectIni);
	GConfig->SetBool(ConfigSection, TEXT("SaveAfterApply"), bSaveAfterApply, GEditorPerProjectIni);
	GConfig->SetBool(ConfigSection, TEXT("FollowSelection"), bFollowSelection, GEditorPerProjectIni);
	for (int32 i = 0; i < Maps.Num(); ++i)
	{
		GConfig->SetString(ConfigSection, *FString::Printf(TEXT("Param%d"), i), *Maps[i].Param, GEditorPerProjectIni);
		GConfig->SetString(ConfigSection, *FString::Printf(TEXT("Suffix%d"), i), *Maps[i].Suffix, GEditorPerProjectIni);
	}
	GConfig->Flush(false, GEditorPerProjectIni);
}

// ---------------- 公開API ----------------

TArray<UObject*> PtAssignerCore::GetSelectedMeshes()
{
	TArray<UObject*> Result;
	FContentBrowserModule& CB = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	TArray<FAssetData> Selected;
	CB.Get().GetSelectedAssets(Selected);
	for (const FAssetData& A : Selected)
	{
		UObject* Obj = A.GetAsset();
		if (Cast<UStaticMesh>(Obj) || Cast<USkeletalMesh>(Obj))
		{
			Result.Add(Obj);
		}
	}
	return Result;
}

FString PtAssignerCore::GetSelectedMeshSignature()
{
	FContentBrowserModule& CB = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	TArray<FAssetData> Selected;
	CB.Get().GetSelectedAssets(Selected);

	const FTopLevelAssetPath StaticClass = UStaticMesh::StaticClass()->GetClassPathName();
	const FTopLevelAssetPath SkeletalClass = USkeletalMesh::StaticClass()->GetClassPathName();

	TArray<FString> Paths;
	for (const FAssetData& A : Selected)
	{
		if (A.AssetClassPath == StaticClass || A.AssetClassPath == SkeletalClass)
		{
			Paths.Add(A.GetSoftObjectPath().ToString());
		}
	}
	Paths.Sort();
	return FString::Join(Paths, TEXT(";"));
}

void PtAssignerCore::UpdateStatus(FPtSlotEntry& E, const FPtSettings& S)
{
	TArray<FString> Problems;
	bool bAmbiguous = false;
	int32 ManualCount = 0;
	TSet<FSoftObjectPath> SeenPaths;
	TArray<FString> DuplicateNames;

	for (int32 i = 0; i < S.Maps.Num(); ++i)
	{
		const bool bHas = E.TexturePaths.IsValidIndex(i) && !E.TexturePaths[i].IsNull();
		const EPtTexSource Src = E.Sources.IsValidIndex(i) ? E.Sources[i] : EPtTexSource::None;
		if (bHas)
		{
			if (SeenPaths.Contains(E.TexturePaths[i]))
			{
				DuplicateNames.AddUnique(E.TexturePaths[i].GetAssetName());
			}
			else
			{
				SeenPaths.Add(E.TexturePaths[i]);
			}
			if (Src == EPtTexSource::Manual) { ++ManualCount; }
			continue;
		}
		const FString TexName = E.Prefix + TEXT("_") + S.Maps[i].Suffix;
		if (Src == EPtTexSource::Ambiguous)
		{
			bAmbiguous = true;
			Problems.Add(FString::Printf(TEXT("同名が複数：%s（右の枠から選んでください）"), *TexName));
		}
		else
		{
			Problems.Add(FString::Printf(TEXT("不足：%s（右の枠から選べます）"), *TexName));
		}
	}

	if (DuplicateNames.Num() > 0)
	{
		Problems.Add(FString::Printf(TEXT("同じテクスチャが複数の枠に入っています：%s（BaseColor / Normal / ORM は別々のテクスチャにしてください）"),
			*FString::Join(DuplicateNames, TEXT(", "))));
	}
	if (Problems.Num() == 0)
	{
		E.Status = EPtStatus::Ready;
		E.bChecked = true;
		E.Message = ManualCount > 0
			? FString::Printf(TEXT("%d枚そろっています（手動指定 %d 枚）"), S.Maps.Num(), ManualCount)
			: FString::Printf(TEXT("%d枚そろっています（名前検索）"), S.Maps.Num());
	}
	else
	{
		E.Status = bAmbiguous ? EPtStatus::Ambiguous : EPtStatus::Missing;
		E.bChecked = false;
		E.Message = FString::Join(Problems, TEXT("\n"));
	}
}

TArray<TSharedPtr<FPtSlotEntry>> PtAssignerCore::Analyze(const FPtSettings& S, const TArray<UObject*>& Meshes)
{
	TArray<TSharedPtr<FPtSlotEntry>> Result;
	IAssetRegistry& AR = GetRegistry();

	// 検索先を再帰走査して「小文字の名前 → パス候補」の索引を作る
	TMap<FString, TArray<FSoftObjectPath>> Index;
	TArray<FString> Roots;
	S.TextureFolders.ParseIntoArray(Roots, TEXT(";"), true);
	for (FString& Root : Roots)
	{
		Root.TrimStartAndEndInline();
		if (Root.IsEmpty() || !AR.PathExists(FName(*Root)))
		{
			continue;
		}
		TArray<FAssetData> Found;
		AR.GetAssetsByPath(FName(*Root), Found, /*bRecursive*/ true);
		for (const FAssetData& A : Found)
		{
			if (A.AssetClassPath == UTexture2D::StaticClass()->GetClassPathName())
			{
				Index.FindOrAdd(A.AssetName.ToString().ToLower()).AddUnique(A.GetSoftObjectPath());
			}
		}
	}

	for (UObject* Mesh : Meshes)
	{
		TArray<FName> Slots;
		GetSlotNames(Mesh, Slots);
		for (int32 SlotIdx = 0; SlotIdx < Slots.Num(); ++SlotIdx)
		{
			TSharedPtr<FPtSlotEntry> E = MakeShared<FPtSlotEntry>();
			E->Mesh = Mesh;
			E->SlotIndex = SlotIdx;
			E->MeshName = Mesh->GetName();
			E->SlotName = Slots[SlotIdx].ToString();
			E->Prefix = FString::Printf(TEXT("%s_%s"), *E->MeshName, *E->SlotName);
			E->InstanceName = TEXT("MI_") + E->Prefix;
			if (UMaterialInterface* CurMat = GetSlotMaterial(Mesh, SlotIdx))
			{
				E->MaterialData = FAssetData(CurMat);
			}
			E->TexturePaths.SetNum(S.Maps.Num());
			E->Sources.Init(EPtTexSource::None, S.Maps.Num());

			// 名前検索（従来どおりのハード参照）。見つかったものは自動でアタッチする
			for (int32 i = 0; i < S.Maps.Num(); ++i)
			{
				const FString TexName = E->Prefix + TEXT("_") + S.Maps[i].Suffix;
				const TArray<FSoftObjectPath>* Paths = Index.Find(TexName.ToLower());
				if (!Paths || Paths->Num() == 0)
				{
					continue; // 空のまま（手動で差し込める）
				}
				if (Paths->Num() > 1)
				{
					E->Sources[i] = EPtTexSource::Ambiguous; // 空のまま。手動で選ぶ
					continue;
				}
				E->TexturePaths[i] = (*Paths)[0];
				E->Sources[i] = EPtTexSource::Auto;
			}

			UpdateStatus(*E, S);
			Result.Add(E);
		}
	}
	return Result;
}

void PtAssignerCore::Apply(const FPtSettings& S, const TArray<TSharedPtr<FPtSlotEntry>>& Entries,
	TFunctionRef<void(const FString&)> Log)
{
	IAssetRegistry& AR = GetRegistry();
	UMaterialInterface* Master = nullptr;

	const FAssetData MasterData = AR.GetAssetByObjectPath(ObjectPathOf(S.MasterPath));
	if (MasterData.IsValid())
	{
		Master = Cast<UMaterialInterface>(MasterData.GetAsset());
		FString Err;
		if (!Master)
		{
			Log(FString::Printf(TEXT("[中止] 親マテリアルとして使用できません：%s"), *S.MasterPath));
			return;
		}
		if (!ValidateMaster(S, Master, Err))
		{
			Log(TEXT("[中止] ") + Err);
			return;
		}
	}

	int32 Targets = 0;
	for (const TSharedPtr<FPtSlotEntry>& E : Entries)
	{
		if (E->bChecked && E->Status == EPtStatus::Ready) { ++Targets; }
	}

	TArray<UPackage*> Dirty;
	int32 Saved = 0, Skipped = 0, Failed = 0;

	FScopedSlowTask Task(Targets, FText::FromString(TEXT("テクスチャを割り当て中...")));
	Task.MakeDialog(true);

	for (const TSharedPtr<FPtSlotEntry>& E : Entries)
	{
		if (!E->bChecked || E->Status != EPtStatus::Ready)
		{
			continue;
		}
		if (Task.ShouldCancel())
		{
			Log(TEXT("[キャンセル] 残りは処理しません"));
			break;
		}
		Task.EnterProgressFrame(1, FText::FromString(E->Prefix));

		FString Msg;
		switch (ApplyOne(S, *E, Master, Dirty, Msg))
		{
		case EResult::Ok:
			E->Status = EPtStatus::Done; E->Message = Msg; ++Saved;
			Log(FString::Printf(TEXT("[成功] %s"), *E->Prefix));
			break;
		case EResult::Skip:
			E->Status = EPtStatus::Skipped; E->Message = Msg; ++Skipped;
			Log(FString::Printf(TEXT("[スキップ] %s / %s"), *E->Prefix, *Msg));
			break;
		case EResult::Fail:
			E->Status = EPtStatus::Failed; E->Message = Msg; ++Failed;
			Log(FString::Printf(TEXT("[失敗] %s / %s"), *E->Prefix, *Msg));
			break;
		}
	}

	if (S.bSaveAfterApply && Dirty.Num() > 0)
	{
		UEditorLoadingAndSavingUtils::SavePackages(Dirty, /*bPromptUserToSave*/ false);
	}
	Log(FString::Printf(TEXT("処理完了：成功 %d / スキップ %d / 失敗 %d"), Saved, Skipped, Failed));
}
