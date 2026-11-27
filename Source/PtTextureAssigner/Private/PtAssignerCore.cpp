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
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "MaterialEditingLibrary.h"
#include "FileHelpers.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/PackageName.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/FileManager.h"
#include "UObject/UnrealType.h"
#include "EditorFramework/AssetImportData.h"
#include "PtFbxReader.h"

namespace
{
	enum EMapIndex
	{
		MapBase = 0, MapNormal = 1, MapOrm = 2,                 // 必須
		MapEmissive = 3, MapRoughness = 4, MapMetallic = 5,     // 任意
		MapAO = 6, MapOpacity = 7,
		MapCount = 8
	};
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

	// ---------------- 用途別のテクスチャ設定（sRGB・圧縮）----------------
	// Painter から出した画像は、UE に取り込むと既定で sRGB オン・圧縮 Default になる。
	// ORM や Roughness などの「数値を持つ」マップが sRGB オンのままだと、金属や粗さの見え方が変わってしまう。
	enum class EPtUsage : uint8 { Color, Normal, Mask };

	EPtUsage UsageOf(int32 MapIdx)
	{
		switch (MapIdx)
		{
		case MapBase:
		case MapEmissive: return EPtUsage::Color;   // 見た目の色：sRGB オン
		case MapNormal:   return EPtUsage::Normal;  // 法線：Normalmap・sRGB オフ
		default:          return EPtUsage::Mask;    // ORM / Roughness / Metallic / AO / Opacity：Masks・sRGB オフ
		}
	}

	FString CompressionName(TextureCompressionSettings C)
	{
		const UEnum* Enum = StaticEnum<TextureCompressionSettings>();
		FString Name = Enum ? Enum->GetNameStringByValue(static_cast<int64>(C)) : FString::Printf(TEXT("%d"), static_cast<int32>(C));
		Name.RemoveFromStart(TEXT("TC_"));
		return Name;
	}

	/** その枠の用途に合う設定を決める。今の設定から直す必要があれば true */
	bool DesiredTextureSettings(const UTexture2D* Tex, int32 MapIdx, TextureCompressionSettings& OutComp, bool& OutSRGB)
	{
		const TextureCompressionSettings Cur = Tex->CompressionSettings;
		switch (UsageOf(MapIdx))
		{
		case EPtUsage::Normal:
			OutComp = TC_Normalmap;
			OutSRGB = false;
			break;
		case EPtUsage::Mask:
			OutComp = TC_Masks;
			OutSRGB = false;
			break;
		default:
		{
			// 色：Default / BC7 / HDR 系はそのまま使い、それ以外（Normalmap・Masks など誤設定）は Default に戻す
			const bool bHdr = (Cur == TC_HDR || Cur == TC_HDR_Compressed);
			const bool bKeep = bHdr || Cur == TC_Default || Cur == TC_BC7;
			OutComp = bKeep ? Cur : TC_Default;
			OutSRGB = bHdr ? Tex->SRGB : true; // HDR は sRGB を触らない
			break;
		}
		}
		return OutComp != Cur || OutSRGB != Tex->SRGB;
	}

	/** 「ORM：sRGB オン→オフ / 圧縮 Default→Masks」のような説明 */
	FString DescribeSettingsChange(const UTexture2D* Tex, const FPtMapRow& Row, TextureCompressionSettings Comp, bool bSRGB)
	{
		TArray<FString> Parts;
		if (Tex->SRGB != bSRGB)
		{
			Parts.Add(FString::Printf(TEXT("sRGB %s→%s"), Tex->SRGB ? TEXT("オン") : TEXT("オフ"), bSRGB ? TEXT("オン") : TEXT("オフ")));
		}
		if (Tex->CompressionSettings != Comp)
		{
			Parts.Add(FString::Printf(TEXT("圧縮 %s→%s"), *CompressionName(Tex->CompressionSettings), *CompressionName(Comp)));
		}
		return FString::Printf(TEXT("%s（%s）：%s"), *Row.Label, *Tex->GetName(), *FString::Join(Parts, TEXT(" / ")));
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
		for (int32 i = 0; i < S.Maps.Num() && i < PtRequiredMapCount; ++i) // 追加マップは親に無くても動かす（無いものは反映しない）
		{
			if (!Available.Contains(S.Maps[i].Param.ToLower()))
			{
				Missing.Add(S.Maps[i].Param);
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

	/** 追加マップを有効にするスカラーパラメータ名（親が持っていれば、インスタンスで 1 にする） */
	FString EnableParamName(const FPtSettings& S, int32 MapIdx)
	{
		return S.Maps[MapIdx].Param + TEXT("Enable");
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

		// テクスチャノード（必須の3枚には最初のセットを初期値として入れる。追加マップは空のまま）
		UMaterialExpressionTextureSampleParameter2D* Node[MapCount] = {};
		auto MakeTexNode = [&](int32 Row, EMaterialSamplerType Sampler) -> bool
		{
			Node[Row] = Cast<UMaterialExpressionTextureSampleParameter2D>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -700, Row * 250));
			if (!Node[Row])
			{
				Err = FString::Printf(TEXT("親のノード作成失敗：%s"), *S.Maps[Row].Param);
				return false;
			}
			Node[Row]->ParameterName = FName(*S.Maps[Row].Param);
			Node[Row]->SamplerType = Sampler;
			// テクスチャが空のままだと「テクスチャが設定されていない」エラーでコンパイルが通らない。
			// 追加マップは Enable=0 の間は使われないので、同じサンプラー種別で有効な必須マップのテクスチャを仮に入れておく
			// （Emissive は Color なので BaseColor、それ以外は Masks なので ORM。インスタンス側で本物に差し替わる）
			UTexture2D* Placeholder = Tex[Row];
			if (Row >= PtRequiredMapCount)
			{
				Placeholder = (Row == MapEmissive) ? Tex[MapBase] : Tex[MapOrm];
			}
			Node[Row]->Texture = Placeholder;
			return true;
		};
		auto MakeEnable = [&](int32 Row) -> UMaterialExpressionScalarParameter*
		{
			UMaterialExpressionScalarParameter* P = Cast<UMaterialExpressionScalarParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionScalarParameter::StaticClass(), -700, Row * 250 + 130));
			if (!P)
			{
				Err = FString::Printf(TEXT("親のノード作成失敗：%s"), *EnableParamName(S, Row));
				return nullptr;
			}
			P->ParameterName = FName(*EnableParamName(S, Row));
			P->DefaultValue = 0.f;
			return P;
		};
		// 追加マップ未使用の時は元の値（ORMの各チャンネルなど）をそのまま使い、Enable=1 の時だけ追加マップに切り替える
		auto MakeLerp = [&](int32 Row, UMaterialExpression* A, const TCHAR* APin, UMaterialExpression* B,
			const TCHAR* BPin, UMaterialExpression* Alpha) -> UMaterialExpressionLinearInterpolate*
		{
			UMaterialExpressionLinearInterpolate* L = Cast<UMaterialExpressionLinearInterpolate>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionLinearInterpolate::StaticClass(), -330, Row * 250));
			if (!L
				|| !UMaterialEditingLibrary::ConnectMaterialExpressions(A, APin, L, TEXT("A"))
				|| !UMaterialEditingLibrary::ConnectMaterialExpressions(B, BPin, L, TEXT("B"))
				|| !UMaterialEditingLibrary::ConnectMaterialExpressions(Alpha, TEXT(""), L, TEXT("Alpha")))
			{
				Err = FString::Printf(TEXT("親のノード接続失敗：%s"), *S.Maps[Row].Param);
				return nullptr;
			}
			return L;
		};

		// --- 必須3枚 ---
		if (!MakeTexNode(MapBase, SAMPLERTYPE_Color) || !MakeTexNode(MapNormal, SAMPLERTYPE_Normal) || !MakeTexNode(MapOrm, SAMPLERTYPE_Masks))
		{
			return nullptr;
		}
		if (!UMaterialEditingLibrary::ConnectMaterialProperty(Node[MapBase], TEXT(""), MP_BaseColor)
			|| !UMaterialEditingLibrary::ConnectMaterialProperty(Node[MapNormal], TEXT(""), MP_Normal))
		{
			Err = TEXT("親のノード接続失敗：BaseColor / Normal");
			return nullptr;
		}

		// --- 追加マップ（S.Maps に行がある分だけ作る）---
		const bool bHasRow[MapCount] = {
			true, true, true,
			S.Maps.IsValidIndex(MapEmissive), S.Maps.IsValidIndex(MapRoughness), S.Maps.IsValidIndex(MapMetallic),
			S.Maps.IsValidIndex(MapAO), S.Maps.IsValidIndex(MapOpacity) };

		// Roughness / Metallic / AO：ORMの G / B / R と切り替え
		struct FChan { int32 Map; const TCHAR* OrmPin; EMaterialProperty Prop; };
		const FChan Chans[] = {
			{ MapAO,        TEXT("R"), MP_AmbientOcclusion },
			{ MapRoughness, TEXT("G"), MP_Roughness },
			{ MapMetallic,  TEXT("B"), MP_Metallic } };
		for (const FChan& C : Chans)
		{
			if (!bHasRow[C.Map])
			{
				if (!UMaterialEditingLibrary::ConnectMaterialProperty(Node[MapOrm], C.OrmPin, C.Prop))
				{
					Err = TEXT("親のノード接続失敗：ORM");
					return nullptr;
				}
				continue;
			}
			UMaterialExpressionScalarParameter* En = nullptr;
			UMaterialExpressionLinearInterpolate* L = nullptr;
			if (!MakeTexNode(C.Map, SAMPLERTYPE_Masks) || !(En = MakeEnable(C.Map))
				|| !(L = MakeLerp(C.Map, Node[MapOrm], C.OrmPin, Node[C.Map], TEXT("R"), En))
				|| !UMaterialEditingLibrary::ConnectMaterialProperty(L, TEXT(""), C.Prop))
			{
				if (Err.IsEmpty()) { Err = FString::Printf(TEXT("親のノード接続失敗：%s"), *S.Maps[C.Map].Param); }
				return nullptr;
			}
		}

		// Emissive：色 × Enable（0 なら発光なし）
		if (bHasRow[MapEmissive])
		{
			UMaterialExpressionScalarParameter* En = nullptr;
			UMaterialExpressionMultiply* Mul = nullptr;
			if (!MakeTexNode(MapEmissive, SAMPLERTYPE_Color) || !(En = MakeEnable(MapEmissive)))
			{
				return nullptr;
			}
			Mul = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(
				M, UMaterialExpressionMultiply::StaticClass(), -330, MapEmissive * 250));
			if (!Mul
				|| !UMaterialEditingLibrary::ConnectMaterialExpressions(Node[MapEmissive], TEXT(""), Mul, TEXT("A"))
				|| !UMaterialEditingLibrary::ConnectMaterialExpressions(En, TEXT(""), Mul, TEXT("B"))
				|| !UMaterialEditingLibrary::ConnectMaterialProperty(Mul, TEXT(""), MP_EmissiveColor))
			{
				Err = FString::Printf(TEXT("親のノード接続失敗：%s"), *S.Maps[MapEmissive].Param);
				return nullptr;
			}
		}

		// Opacity：Enable=1 の時だけ Opacity Mask にする（ブレンドモードは Masked）
		if (bHasRow[MapOpacity])
		{
			UMaterialExpressionScalarParameter* En = nullptr;
			UMaterialExpressionLinearInterpolate* L = nullptr;
			UMaterialExpressionConstant* One = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(
				M, UMaterialExpressionConstant::StaticClass(), -700, MapOpacity * 250 + 260));
			if (One) { One->R = 1.f; }
			if (!One || !MakeTexNode(MapOpacity, SAMPLERTYPE_Masks) || !(En = MakeEnable(MapOpacity))
				|| !(L = MakeLerp(MapOpacity, One, TEXT(""), Node[MapOpacity], TEXT("R"), En))
				|| !UMaterialEditingLibrary::ConnectMaterialProperty(L, TEXT(""), MP_OpacityMask))
			{
				if (Err.IsEmpty()) { Err = FString::Printf(TEXT("親のノード接続失敗：%s"), *S.Maps[MapOpacity].Param); }
				return nullptr;
			}
			M->BlendMode = BLEND_Masked;
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
		TArray<UPackage*>& Dirty, const TSet<FSoftObjectPath>& NoFix, FString& Msg)
	{
		UObject* Mesh = E.Mesh.Get();
		if (!Mesh)
		{
			Msg = TEXT("メッシュが無効です");
			return EResult::Fail;
		}

		UTexture2D* Tex[MapCount] = {};
		for (int32 i = 0; i < MapCount && i < S.Maps.Num(); ++i)
		{
			const bool bRequired = i < PtRequiredMapCount;
			if (!E.TexturePaths.IsValidIndex(i) || E.TexturePaths[i].IsNull())
			{
				if (bRequired)
				{
					Msg = FString::Printf(TEXT("%s のテクスチャが空です"), *S.Maps[i].Param);
					return EResult::Skip;
				}
				continue; // 追加マップは空でよい
			}
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
				if (Tex[i] && Tex[i] == Tex[j])
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

		// テクスチャ設定：割り当てと同時に、用途（色 / 法線 / マスク）に合う sRGB・圧縮へそろえる
		TArray<FString> FixLog;
		if (S.bAutoFixTextureSettings)
		{
			for (int32 i = 0; i < MapCount && i < S.Maps.Num(); ++i)
			{
				if (!Tex[i] || !E.TexturePaths.IsValidIndex(i) || NoFix.Contains(E.TexturePaths[i])) { continue; }
				TextureCompressionSettings Comp;
				bool bSRGB;
				if (DesiredTextureSettings(Tex[i], i, Comp, bSRGB))
				{
					FixLog.Add(DescribeSettingsChange(Tex[i], S.Maps[i], Comp, bSRGB)); // 変更前の値で説明を作ってから直す
					FixTexture(Tex[i], Comp, bSRGB, Dirty);
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

		// 追加マップは、親マテリアルに同名のテクスチャパラメータがある時だけ反映する
		TSet<FString> TexParams, ScalarParams;
		{
			TArray<FName> Names;
			UMaterialEditingLibrary::GetTextureParameterNames(Master, Names);
			for (const FName& N : Names) { TexParams.Add(N.ToString().ToLower()); }
			Names.Reset();
			UMaterialEditingLibrary::GetScalarParameterNames(Master, Names);
			for (const FName& N : Names) { ScalarParams.Add(N.ToString().ToLower()); }
		}
		bool bUse[MapCount] = {};
		TArray<FString> Unapplied;
		for (int32 i = 0; i < MapCount && i < S.Maps.Num(); ++i)
		{
			bUse[i] = Tex[i] && (i < PtRequiredMapCount || TexParams.Contains(S.Maps[i].Param.ToLower()));
			if (Tex[i] && !bUse[i])
			{
				Unapplied.Add(S.Maps[i].Param);
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
		for (int32 i = 0; i < MapCount && i < S.Maps.Num(); ++i)
		{
			if (!bUse[i]) { continue; }
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MI, FName(*S.Maps[i].Param), Tex[i]);
			if (i >= PtRequiredMapCount)
			{
				const FString En = EnableParamName(S, i); // 親が「◯◯Enable」を持っていれば 1 にして有効化
				if (ScalarParams.Contains(En.ToLower()))
				{
					UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MI, FName(*En), 1.f);
				}
			}
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
		for (int32 i = 0; i < MapCount && i < S.Maps.Num(); ++i)
		{
			if (!bUse[i]) { continue; }
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
		for (int32 i = 0; i < MapCount && i < S.Maps.Num(); ++i)
		{
			if (!bUse[i]) { continue; }
			const FName Param(*S.Maps[i].Param);
			if (!HasOverride(Param, Tex[i]))
			{
				MI->TextureParameterValues = Previous;
				UMaterialEditingLibrary::UpdateMaterialInstance(MI);
				Msg = FString::Printf(TEXT("%s にテクスチャを設定できませんでした（親マテリアルに同名のテクスチャパラメータがあるか確認してください）"), *S.Maps[i].Param);
				return EResult::Fail;
			}
			Confirm += FString::Printf(TEXT("%s%s=%s"), Confirm.IsEmpty() ? TEXT("") : TEXT(", "), *S.Maps[i].Param, *Tex[i]->GetName());
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
		if (Unapplied.Num() > 0)
		{
			Msg += FString::Printf(TEXT("\n未反映（親マテリアルにパラメータが無い）：%s"), *FString::Join(Unapplied, TEXT(", ")));
		}
		if (FixLog.Num() > 0)
		{
			Msg += TEXT("\nテクスチャ設定を変更：") + FString::Join(FixLog, TEXT(" ／ "));
		}
		return EResult::Ok;
	}
}

// ---------------- 設定 ----------------

FPtSettings::FPtSettings()
{
	Maps = {
		{ TEXT("BasecolorTex"),  TEXT("BaseColor"),                  TEXT("BaseColor") },
		{ TEXT("NormalTex"),     TEXT("Normal"),                     TEXT("Normal") },
		{ TEXT("ORMTex"),        TEXT("OcclusionRoughnessMetallic"), TEXT("ORM") },
		// ここから任意（あれば割り当て、無くても準備OK）
		{ TEXT("EmissiveTex"),   TEXT("Emissive"),                   TEXT("Emissive（発光）") },
		{ TEXT("RoughnessTex"),  TEXT("Roughness"),                  TEXT("Roughness") },
		{ TEXT("MetallicTex"),   TEXT("Metallic"),                   TEXT("Metallic") },
		{ TEXT("AOTex"),         TEXT("AmbientOcclusion"),           TEXT("AO") },
		{ TEXT("OpacityTex"),    TEXT("Opacity"),                    TEXT("Opacity（透過）") },
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
	GConfig->GetBool(ConfigSection, TEXT("UseFbx"), bUseFbx, GEditorPerProjectIni);
	GConfig->GetString(ConfigSection, TEXT("FbxOrmKeywords"), FbxOrmKeywords, GEditorPerProjectIni);
	GConfig->GetBool(ConfigSection, TEXT("UseGuess"), bUseGuess, GEditorPerProjectIni);
	GConfig->GetBool(ConfigSection, TEXT("ShowExtraMaps"), bShowExtraMaps, GEditorPerProjectIni);
	GConfig->GetBool(ConfigSection, TEXT("RememberManual"), bRememberManual, GEditorPerProjectIni);
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
	GConfig->SetBool(ConfigSection, TEXT("UseFbx"), bUseFbx, GEditorPerProjectIni);
	GConfig->SetString(ConfigSection, TEXT("FbxOrmKeywords"), *FbxOrmKeywords, GEditorPerProjectIni);
	GConfig->SetBool(ConfigSection, TEXT("UseGuess"), bUseGuess, GEditorPerProjectIni);
	GConfig->SetBool(ConfigSection, TEXT("ShowExtraMaps"), bShowExtraMaps, GEditorPerProjectIni);
	GConfig->SetBool(ConfigSection, TEXT("RememberManual"), bRememberManual, GEditorPerProjectIni);
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
	TArray<FString> Optional; // 任意マップについての注意（準備OKは妨げない）
	bool bAmbiguous = false;
	int32 ManualCount = 0;
	int32 FbxCount = 0;
	int32 GuessCount = 0;
	int32 SavedCount = 0;
	int32 AutoCount = 0;
	int32 Filled = 0;
	TSet<FSoftObjectPath> SeenPaths;
	TArray<FString> DuplicateNames;

	for (int32 i = 0; i < S.Maps.Num(); ++i)
	{
		const bool bRequired = i < PtRequiredMapCount;
		const bool bHas = E.TexturePaths.IsValidIndex(i) && !E.TexturePaths[i].IsNull();
		const EPtTexSource Src = E.Sources.IsValidIndex(i) ? E.Sources[i] : EPtTexSource::None;
		if (bHas)
		{
			++Filled;
			if (SeenPaths.Contains(E.TexturePaths[i]))
			{
				DuplicateNames.AddUnique(E.TexturePaths[i].GetAssetName());
			}
			else
			{
				SeenPaths.Add(E.TexturePaths[i]);
			}
			if (Src == EPtTexSource::Manual) { ++ManualCount; }
			else if (Src == EPtTexSource::Fbx) { ++FbxCount; }
			else if (Src == EPtTexSource::Guess) { ++GuessCount; }
			else if (Src == EPtTexSource::Saved) { ++SavedCount; }
			else { ++AutoCount; }
			continue;
		}
		if (!bRequired)
		{
			if (Src == EPtTexSource::Ambiguous)
			{
				Optional.Add(FString::Printf(TEXT("%s：同名が複数あります（使う場合は左の枠から選んでください）"), *S.Maps[i].Label));
			}
			continue; // 空でも問題なし
		}
		const FString TexName = E.Prefix + TEXT("_") + S.Maps[i].Suffix;
		if (Src == EPtTexSource::Ambiguous)
		{
			bAmbiguous = true;
			const FString AmbiguousName = (E.Hints.IsValidIndex(i) && !E.Hints[i].IsEmpty()) ? E.Hints[i] : TexName;
			Problems.Add(FString::Printf(TEXT("同名が複数：%s（左の枠から選んでください）"), *AmbiguousName));
		}
		else
		{
			if (E.Hints.IsValidIndex(i) && !E.Hints[i].IsEmpty())
			{
				Problems.Add(FString::Printf(TEXT("不足：%s（%s）"), *TexName, *E.Hints[i]));
			}
			else
			{
				Problems.Add(FString::Printf(TEXT("不足：%s（左の枠から選べます）"), *TexName));
			}
		}
	}

	if (DuplicateNames.Num() > 0)
	{
		Problems.Add(FString::Printf(TEXT("同じテクスチャが複数の枠に入っています：%s（枠ごとに別々のテクスチャにしてください）"),
			*FString::Join(DuplicateNames, TEXT(", "))));
	}
	if (Problems.Num() == 0)
	{
		E.Status = EPtStatus::Ready;
		E.bChecked = (GuessCount == 0); // 推定を含む行は、確認してもらうためチェックを外しておく
		TArray<FString> Parts;
		if (AutoCount > 0) { Parts.Add(FString::Printf(TEXT("名前検索 %d"), AutoCount)); }
		if (FbxCount > 0) { Parts.Add(FString::Printf(TEXT("FBX参照 %d"), FbxCount)); }
		if (GuessCount > 0) { Parts.Add(FString::Printf(TEXT("推定 %d"), GuessCount)); }
		if (SavedCount > 0) { Parts.Add(FString::Printf(TEXT("記憶 %d"), SavedCount)); }
		if (ManualCount > 0) { Parts.Add(FString::Printf(TEXT("手動 %d"), ManualCount)); }
		E.Message = FString::Printf(TEXT("%d枚そろっています（%s）"), Filled, *FString::Join(Parts, TEXT(" / ")));
		if (GuessCount > 0)
		{
			E.Message += TEXT("\n推定です。サムネイルで合っているか確認し、問題なければチェックを入れてください");
		}
	}
	else
	{
		E.Status = bAmbiguous ? EPtStatus::Ambiguous : EPtStatus::Missing;
		E.bChecked = false;
		E.Message = FString::Join(Problems, TEXT("\n"));
	}
	if (Optional.Num() > 0)
	{
		E.Message += TEXT("\n") + FString::Join(Optional, TEXT("\n"));
	}

	// 適用時に直されるテクスチャ設定（sRGB・圧縮）の予定。割り当てる前に、何が変わるかを見られるようにする
	E.SettingsChanges.Reset();
	if (S.bAutoFixTextureSettings)
	{
		for (int32 i = 0; i < S.Maps.Num() && i < MapCount; ++i)
		{
			if (!E.TexturePaths.IsValidIndex(i) || E.TexturePaths[i].IsNull()) { continue; }
			const UTexture2D* Tex = Cast<UTexture2D>(E.TexturePaths[i].TryLoad());
			if (!Tex) { continue; }
			TextureCompressionSettings Comp;
			bool bSRGB;
			if (DesiredTextureSettings(Tex, i, Comp, bSRGB))
			{
				E.SettingsChanges.Add(DescribeSettingsChange(Tex, S.Maps[i], Comp, bSRGB));
			}
		}
		if (E.SettingsChanges.Num() > 0)
		{
			E.Message += TEXT("\n適用時にテクスチャ設定を変更：") + FString::Join(E.SettingsChanges, TEXT(" ／ "));
		}
	}
}

// ---------------------------------------------------------------- 手動修正の記憶
// Saved/PtTextureAssigner/ManualChoices.tsv に「メッシュ \t スロット名 \t パラメータ名 \t テクスチャ」を1行ずつ保存する。
namespace
{
	TMap<FString, FString>& Choices()
	{
		static TMap<FString, FString> Map;
		static bool bLoaded = false;
		if (!bLoaded)
		{
			bLoaded = true;
			TArray<FString> Lines;
			if (FFileHelper::LoadFileToStringArray(Lines, *PtAssignerCore::GetChoicesFilePath()))
			{
				for (const FString& Line : Lines)
				{
					TArray<FString> P;
					Line.ParseIntoArray(P, TEXT("\t"), /*bCullEmpty*/ false);
					if (P.Num() == 4 && !P[3].IsEmpty())
					{
						Map.Add(P[0] + TEXT("\t") + P[1] + TEXT("\t") + P[2], P[3]);
					}
				}
			}
		}
		return Map;
	}

	void SaveChoices()
	{
		TArray<FString> Lines;
		for (const TPair<FString, FString>& Pair : Choices())
		{
			Lines.Add(Pair.Key + TEXT("\t") + Pair.Value);
		}
		Lines.Sort();
		const FString File = PtAssignerCore::GetChoicesFilePath();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree*/ true);
		FFileHelper::SaveStringArrayToFile(Lines, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	FString ChoiceKey(const FPtSlotEntry& E, const FString& Param)
	{
		const UObject* M = E.Mesh.Get();
		return (M ? M->GetPathName() : FString()) + TEXT("\t") + E.SlotName + TEXT("\t") + Param;
	}
}

FString PtAssignerCore::GetChoicesFilePath()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PtTextureAssigner") / TEXT("ManualChoices.tsv"));
}

void PtAssignerCore::RememberChoice(const FPtSlotEntry& Entry, const FString& Param, const FSoftObjectPath& Texture)
{
	if (!Entry.Mesh.IsValid() || Param.IsEmpty() || Texture.IsNull()) { return; }
	Choices().Add(ChoiceKey(Entry, Param), Texture.ToString());
	SaveChoices();
}

void PtAssignerCore::ForgetChoice(const FPtSlotEntry& Entry, const FString& Param)
{
	if (Choices().Remove(ChoiceKey(Entry, Param)) > 0)
	{
		SaveChoices();
	}
}

void PtAssignerCore::ClearAllChoices()
{
	Choices().Empty();
	SaveChoices();
}

int32 PtAssignerCore::CountChoices()
{
	return Choices().Num();
}

// ---------------------------------------------------------------- FBX 参照まわり
namespace
{
	void GetImportedSlotNames(UObject* Mesh, TArray<FName>& Out)
	{
		if (UStaticMesh* SM = Cast<UStaticMesh>(Mesh))
		{
			for (const FStaticMaterial& M : SM->GetStaticMaterials())
			{
				Out.Add(M.ImportedMaterialSlotName);
			}
		}
		else if (USkeletalMesh* SK = Cast<USkeletalMesh>(Mesh))
		{
			for (const FSkeletalMaterial& M : SK->GetMaterials())
			{
				Out.Add(M.ImportedMaterialSlotName);
			}
		}
	}

	/** メッシュ種別に依らず、リフレクションで AssetImportData を取る */
	UAssetImportData* GetImportData(UObject* Mesh)
	{
		if (!Mesh)
		{
			return nullptr;
		}
		const FObjectProperty* Prop = CastField<FObjectProperty>(Mesh->GetClass()->FindPropertyByName(TEXT("AssetImportData")));
		return Prop ? Cast<UAssetImportData>(Prop->GetObjectPropertyValue_InContainer(Mesh)) : nullptr;
	}

	/** 使うFBXのパス。見つからなければ空を返し、理由を OutWhy に入れる */
	FString ResolveFbxPath(UObject* Mesh, const FPtSettings& S, FString& OutWhy)
	{
		if (!S.FbxOverridePath.IsEmpty())
		{
			if (FPaths::FileExists(S.FbxOverridePath))
			{
				return S.FbxOverridePath;
			}
			OutWhy = FString::Printf(TEXT("指定したFBXが見つかりません：%s"), *S.FbxOverridePath);
			return FString();
		}

		const UAssetImportData* Data = GetImportData(Mesh);
		if (!Data)
		{
			OutWhy = TEXT("インポート元の情報がありません（「FBXを指定」で選べます）");
			return FString();
		}
		const FString Path = Data->GetFirstFilename();
		if (Path.IsEmpty())
		{
			OutWhy = TEXT("インポート元のFBXが記録されていません（「FBXを指定」で選べます）");
			return FString();
		}
		if (!FPaths::GetExtension(Path).Equals(TEXT("fbx"), ESearchCase::IgnoreCase))
		{
			OutWhy = FString::Printf(TEXT("インポート元がFBXではありません：%s"), *Path);
			return FString();
		}
		if (!FPaths::FileExists(Path))
		{
			OutWhy = FString::Printf(TEXT("元のFBXが見つかりません：%s（「FBXを指定」で選べます）"), *Path);
			return FString();
		}
		return Path;
	}

	struct FFbxCacheEntry
	{
		FDateTime Stamp;
		int64 Size = 0;
		TSharedPtr<PtFbx::FResult> Result;
		FString Error;
	};

	TMap<FString, FFbxCacheEntry>& FbxCache()
	{
		static TMap<FString, FFbxCacheEntry> Cache;
		return Cache;
	}

	/** FBXを読んで解析する。ファイルが変わらない限り結果を使い回す（選択に追従して何度も呼ばれるため） */
	TSharedPtr<PtFbx::FResult> LoadFbxCached(const FString& Path, FString& OutError)
	{
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Path);
		const int64 Size = IFileManager::Get().FileSize(*Path);

		if (const FFbxCacheEntry* Hit = FbxCache().Find(Path))
		{
			if (Hit->Stamp == Stamp && Hit->Size == Size)
			{
				OutError = Hit->Error;
				return Hit->Result;
			}
		}

		FFbxCacheEntry Entry;
		Entry.Stamp = Stamp;
		Entry.Size = Size;

		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			Entry.Error = FString::Printf(TEXT("FBXを読み込めません：%s"), *Path);
		}
		else
		{
			TSharedPtr<PtFbx::FResult> Parsed = MakeShared<PtFbx::FResult>();
			if (PtFbx::Read(Bytes.GetData(), Bytes.Num(), *Parsed))
			{
				Entry.Result = Parsed;
			}
			else
			{
				Entry.Error = FString::Printf(TEXT("FBXを解析できません：%s"), *Parsed->Error);
			}
		}
		FbxCache().Add(Path, Entry);
		OutError = Entry.Error;
		return Entry.Result;
	}

	/** 記号を除いた小文字（材質名の表記ゆれを吸収するため） */
	FString NormalizeKey(const FString& In)
	{
		FString Out;
		for (int32 i = 0; i < In.Len(); ++i)
		{
			const TCHAR C = In[i];
			if (FChar::IsAlnum(C))
			{
				Out.AppendChar(FChar::ToLower(C));
			}
		}
		return Out;
	}

	/** UE がアセット名にするときの置換（空白や記号を _ に）を真似る */
	FString SanitizeAssetName(const FString& In)
	{
		static const FString Bad = TEXT(" ,|&!~@#(){}[]=;^%$`'\"\\/:*?<>.");
		FString Out;
		for (int32 i = 0; i < In.Len(); ++i)
		{
			const TCHAR C = In[i];
			Out.AppendChar(Bad.Find(FString::Chr(C)) != INDEX_NONE ? TEXT('_') : C);
		}
		return Out;
	}

	/** スロットに対応する FBX の材質を探す（名前一致 → 表記ゆれ → 1材質1スロットなら） */
	const PtFbx::FMaterialInfo* FindFbxMaterial(const PtFbx::FResult& R, const FString& SlotName, const FString& ImportedName, int32 SlotCount)
	{
		const FString Names[2] = { ImportedName, SlotName };
		for (const FString& N : Names)
		{
			if (N.IsEmpty()) { continue; }
			for (const PtFbx::FMaterialInfo& M : R.Materials)
			{
				if (M.Name.Equals(N, ESearchCase::IgnoreCase)) { return &M; }
			}
		}
		for (const FString& N : Names)
		{
			const FString Key = NormalizeKey(N);
			if (Key.IsEmpty()) { continue; }
			for (const PtFbx::FMaterialInfo& M : R.Materials)
			{
				if (NormalizeKey(M.Name) == Key) { return &M; }
			}
		}
		if (R.Materials.Num() == 1 && SlotCount == 1)
		{
			return &R.Materials[0];
		}
		return nullptr;
	}

	/** FBX の接続先プロパティ名 → 枠番号（該当なしは INDEX_NONE）。
	 *  ORM 扱いのキーワード（設定）に当たるものは従来どおり ORM。キーワードを orm だけ等に絞ると、
	 *  Roughness / Metallic / AO の個別プロパティも別々の枠として拾う。 */
	int32 ClassifyFbxProperty(const FString& Property, const TArray<FString>& OrmKeys)
	{
		const FString P = Property.ToLower();
		if (P.Contains(TEXT("normal")) || P.Contains(TEXT("bump")))
		{
			return MapNormal;
		}
		if (P.Contains(TEXT("diffuse")) || P.Contains(TEXT("basecolor")) || P.Contains(TEXT("base_color")) || P.Contains(TEXT("albedo")))
		{
			return MapBase;
		}
		if (P.Contains(TEXT("emissive")) || P.Contains(TEXT("emission")))
		{
			return MapEmissive;
		}
		if (P.Contains(TEXT("opacity")) || P.Contains(TEXT("transparen")))
		{
			return MapOpacity;
		}
		for (const FString& K : OrmKeys)
		{
			if (!K.IsEmpty() && P.Contains(K))
			{
				return MapOrm;
			}
		}
		if (P.Contains(TEXT("rough")))
		{
			return MapRoughness;
		}
		if (P.Contains(TEXT("metal")))
		{
			return MapMetallic;
		}
		if (P.Contains(TEXT("occlusion")) || P.Contains(TEXT("ao_map")) || P.Contains(TEXT("aomap")) || P == TEXT("ao"))
		{
			return MapAO;
		}
		return INDEX_NONE;
	}

	int32 CommonPrefixLength(const FString& A, const FString& B)
	{
		const int32 Max = FMath::Min(A.Len(), B.Len());
		int32 N = 0;
		while (N < Max && FChar::ToLower(A[N]) == FChar::ToLower(B[N]))
		{
			++N;
		}
		return N;
	}

	/** 同名の候補が複数あるとき、メッシュに一番近い（フォルダが近い）ものを選ぶ。決められなければ曖昧 */
	FSoftObjectPath PickBestPath(const TArray<FSoftObjectPath>& Cands, const FString& MeshPackage, bool& bOutAmbiguous)
	{
		bOutAmbiguous = false;
		if (Cands.Num() == 1)
		{
			return Cands[0];
		}
		int32 BestIdx = INDEX_NONE;
		int32 BestScore = -1;
		bool bTie = false;
		for (int32 i = 0; i < Cands.Num(); ++i)
		{
			const int32 Score = CommonPrefixLength(Cands[i].GetLongPackageName(), MeshPackage);
			if (Score > BestScore)
			{
				BestScore = Score;
				BestIdx = i;
				bTie = false;
			}
			else if (Score == BestScore)
			{
				bTie = true;
			}
		}
		if (bTie || BestIdx == INDEX_NONE)
		{
			bOutAmbiguous = true;
			return FSoftObjectPath();
		}
		return Cands[BestIdx];
	}

	typedef TMap<FString, TArray<FSoftObjectPath>> FTexIndex;

	/** 指定フォルダ以下の Texture2D を「小文字の名前 → パス」にする */
	void BuildFolderIndex(IAssetRegistry& AR, const FString& Folder, FTexIndex& Out)
	{
		TArray<FAssetData> Found;
		AR.GetAssetsByPath(FName(*Folder), Found, /*bRecursive*/ true);
		const FTopLevelAssetPath TexClass = UTexture2D::StaticClass()->GetClassPathName();
		for (const FAssetData& Asset : Found)
		{
			if (Asset.AssetClassPath == TexClass)
			{
				Out.FindOrAdd(Asset.AssetName.ToString().ToLower()).AddUnique(Asset.GetSoftObjectPath());
			}
		}
	}

	/** FBX に書かれた画像名から、プロジェクト内のテクスチャを探す。メッシュ周辺 → 検索先の順 */
	bool FindTextureByFbxName(const FString& BaseName, const FTexIndex* Local, const FTexIndex& Global,
		const FString& MeshPackage, FSoftObjectPath& OutPath, bool& bOutAmbiguous)
	{
		bOutAmbiguous = false;
		const FString KeySanitized = SanitizeAssetName(BaseName).ToLower();
		const FString KeyRaw = BaseName.ToLower();
		const FTexIndex* Sources[2] = { Local, &Global };
		for (const FTexIndex* Map : Sources)
		{
			if (!Map) { continue; }
			const TArray<FSoftObjectPath>* Cands = Map->Find(KeySanitized);
			if (!Cands) { Cands = Map->Find(KeyRaw); }
			if (!Cands || Cands->Num() == 0) { continue; }
			OutPath = PickBestPath(*Cands, MeshPackage, bOutAmbiguous);
			return !bOutAmbiguous;
		}
		return false;
	}

	// ---------------------------------------------------------------- 推定マッチ
	// 名前検索もFBX参照も当たらないとき用。テクスチャを「セット名 + 種類」でグループ化し、
	// スロット名 / FBX材質名 / FBX画像名 / メッシュ名に似ているセットを選ぶ。
	// 間違えやすいので、結果は「推定」として確認用にチェックを外した状態で出す。
	/** このスロットの枠のどれかが、すでにそのテクスチャを使っているか */
	bool EntryUsesPath(const FPtSlotEntry& E, const FSoftObjectPath& P)
	{
		for (const FSoftObjectPath& Q : E.TexturePaths)
		{
			if (!Q.IsNull() && Q == P) { return true; }
		}
		return false;
	}

	struct FTexGroup
	{
		FString Key;     // セット名（末尾の BaseColor 等を除いたもの。大文字小文字は元のまま）
		FString Folder;  // テクスチャのあるフォルダ（同名セットを区別する）
		bool bLocal = false; // メッシュと同じフォルダ以下にあるか
		FSoftObjectPath Paths[MapCount];
		int32 Counts[MapCount] = {};
	};

	/** テクスチャ名の末尾が各枠の種類を表す語なら、その枠番号とセット名を返す（該当なしは INDEX_NONE） */
	int32 ClassifyTextureName(const FString& Name, const FPtSettings& S, FString& OutSetKey)
	{
		static const TArray<FString> Defaults[MapCount] = {
			{ TEXT("basecolor"), TEXT("base_color"), TEXT("albedo"), TEXT("diffuse"), TEXT("diff"), TEXT("color"), TEXT("col"), TEXT("bc") },
			{ TEXT("normal_opengl"), TEXT("normal_directx"), TEXT("normalopengl"), TEXT("normaldirectx"), TEXT("normalgl"), TEXT("normaldx"), TEXT("normal"), TEXT("nrm"), TEXT("nor") },
			{ TEXT("occlusionroughnessmetallic"), TEXT("orm"), TEXT("packed"), TEXT("rma") },
			{ TEXT("emissive"), TEXT("emission"), TEXT("emit"), TEXT("glow") },
			{ TEXT("roughness"), TEXT("rough") },
			{ TEXT("metallic"), TEXT("metalness"), TEXT("metal") },
			{ TEXT("ambientocclusion"), TEXT("ambient_occlusion"), TEXT("occlusion"), TEXT("ao") },
			{ TEXT("opacity"), TEXT("opacitymask"), TEXT("transparency"), TEXT("alpha") }
		};
		const FString Low = Name.ToLower();
		int32 BestMap = INDEX_NONE;
		int32 BestLen = 0;
		for (int32 i = 0; i < MapCount; ++i)
		{
			TArray<FString> Sufs = Defaults[i];
			if (S.Maps.IsValidIndex(i) && !S.Maps[i].Suffix.IsEmpty())
			{
				Sufs.Add(S.Maps[i].Suffix.ToLower());
			}
			for (const FString& Suf : Sufs)
			{
				if (Suf.Len() <= BestLen || Low.Len() <= Suf.Len() || !Low.EndsWith(Suf))
				{
					continue;
				}
				const int32 Before = Low.Len() - Suf.Len() - 1;
				if (FChar::IsAlnum(Low[Before]))
				{
					continue; // 語の途中（例：xxcolor の color）は種類として扱わない
				}
				BestMap = i;
				BestLen = Suf.Len();
			}
		}
		if (BestMap == INDEX_NONE)
		{
			return INDEX_NONE;
		}
		OutSetKey = Name.Left(Name.Len() - BestLen);
		while (OutSetKey.Len() > 0 && !FChar::IsAlnum(OutSetKey[OutSetKey.Len() - 1]))
		{
			OutSetKey.LeftChopInline(1);
		}
		return OutSetKey.IsEmpty() ? INDEX_NONE : BestMap;
	}

	/** 区切り文字・camelCase・英字と数字の境目で語に分ける（T_ や SM_ など意味の薄い語は捨てる） */
	void TokenizeName(const FString& In, TSet<FString>& Out)
	{
		static const TSet<FString> Stop = {
			TEXT("t"), TEXT("tex"), TEXT("texture"), TEXT("sm"), TEXT("mi"), TEXT("m"), TEXT("mat"),
			TEXT("material"), TEXT("set"), TEXT("ue"), TEXT("fbx"), TEXT("lambert"), TEXT("default"), TEXT("standard")
		};
		FString Cur;
		auto Flush = [&]()
		{
			if (!Cur.IsEmpty())
			{
				const FString L = Cur.ToLower();
				if (!Stop.Contains(L)) { Out.Add(L); }
				Cur.Reset();
			}
		};
		for (int32 i = 0; i < In.Len(); ++i)
		{
			const TCHAR C = In[i];
			if (!FChar::IsAlnum(C))
			{
				Flush();
				continue;
			}
			if (Cur.Len() > 0)
			{
				const TCHAR P = Cur[Cur.Len() - 1];
				if ((FChar::IsLower(P) && FChar::IsUpper(C)) || (FChar::IsAlpha(P) != FChar::IsAlpha(C)))
				{
					Flush();
				}
			}
			Cur.AppendChar(C);
		}
		Flush();
	}

	/** 0〜1。語の一致率（Dice係数）か、4文字以上の部分一致のどちらか高い方 */
	float NameSimilarity(const FString& A, const FString& B)
	{
		TSet<FString> TA, TB;
		TokenizeName(A, TA);
		TokenizeName(B, TB);
		float Score = 0.f;
		if (TA.Num() > 0 && TB.Num() > 0)
		{
			Score = 2.f * static_cast<float>(TA.Intersect(TB).Num()) / static_cast<float>(TA.Num() + TB.Num());
		}
		const FString NA = NormalizeKey(A);
		const FString NB = NormalizeKey(B);
		const FString& Short = NA.Len() <= NB.Len() ? NA : NB;
		const FString& Long = NA.Len() <= NB.Len() ? NB : NA;
		if (Short.Len() >= 4 && Long.Contains(Short))
		{
			Score = FMath::Max(Score, 0.6f + 0.4f * static_cast<float>(Short.Len()) / static_cast<float>(Long.Len()));
		}
		return Score;
	}

	void BuildTexGroups(const FTexIndex& Global, const FTexIndex* Local, const FString& MeshFolder,
		const FPtSettings& S, TArray<FTexGroup>& Out)
	{
		TSet<FSoftObjectPath> Seen;
		TMap<FString, int32> GroupOf; // フォルダ + セット名 → Out の番号
		const FTexIndex* Sources[2] = { Local, &Global };
		for (const FTexIndex* Map : Sources)
		{
			if (!Map) { continue; }
			for (const TPair<FString, TArray<FSoftObjectPath>>& Pair : *Map)
			{
				for (const FSoftObjectPath& Path : Pair.Value)
				{
					bool bAlready = false;
					Seen.Add(Path, &bAlready);
					if (bAlready) { continue; }

					FString SetKey;
					const int32 MapIdx = ClassifyTextureName(Path.GetAssetName(), S, SetKey);
					if (MapIdx == INDEX_NONE) { continue; }

					const FString Folder = FPackageName::GetLongPackagePath(Path.GetLongPackageName());
					const FString Id = Folder + TEXT("/") + SetKey.ToLower();
					int32* Found = GroupOf.Find(Id);
					if (!Found)
					{
						FTexGroup G;
						G.Key = SetKey;
						G.Folder = Folder;
						G.bLocal = !MeshFolder.IsEmpty() && Folder.StartsWith(MeshFolder);
						Found = &GroupOf.Add(Id, Out.Add(G));
					}
					FTexGroup& G = Out[*Found];
					G.Paths[MapIdx] = Path;
					++G.Counts[MapIdx];
				}
			}
		}
	}

	/** 1つのメッシュの、まだ空の枠を推定で埋める。Names = スロットごとの（手がかりの名前, 重み） */
	void GuessForMesh(const FPtSettings& S, const TArray<FTexGroup>& Groups,
		const TArray<TSharedPtr<FPtSlotEntry>>& Entries, const TArray<TArray<TPair<FString, float>>>& Names,
		TFunctionRef<void(const FString&)> AddNote)
	{
		constexpr float MinScore = 0.45f;   // これ未満は似ているとみなさない
		constexpr float MinMargin = 0.15f;  // 2位との差がこれ未満なら決めない

		auto NeedsMap = [](const FPtSlotEntry& E, int32 i)
		{
			return E.Sources.IsValidIndex(i) && E.Sources[i] == EPtTexSource::None
				&& E.TexturePaths.IsValidIndex(i) && E.TexturePaths[i].IsNull();
		};
		// 推定を始めるかどうかは必須の3枠だけで決める（追加マップが空なのは普通のこと）。選んだセットに追加マップがあれば一緒に入れる
		auto NeedsAny = [&](const FPtSlotEntry& E)
		{
			for (int32 i = 0; i < S.Maps.Num() && i < PtRequiredMapCount; ++i) { if (NeedsMap(E, i)) { return true; } }
			return false;
		};
		auto CanFill = [&](const FPtSlotEntry& E, const FTexGroup& G)
		{
			for (int32 i = 0; i < S.Maps.Num() && i < PtRequiredMapCount; ++i) { if (NeedsMap(E, i) && G.Counts[i] == 1) { return true; } }
			return false;
		};
		auto Fill = [&](FPtSlotEntry& E, const FTexGroup& G, const TCHAR* Why, float Score)
		{
			for (int32 i = 0; i < S.Maps.Num() && i < MapCount; ++i)
			{
				if (NeedsMap(E, i) && G.Counts[i] == 1 && !(i >= PtRequiredMapCount && EntryUsesPath(E, G.Paths[i])))
				{
					E.TexturePaths[i] = G.Paths[i];
					E.Sources[i] = EPtTexSource::Guess;
					E.Hints[i] = FString();
				}
			}
			AddNote(FString::Printf(TEXT("  [%s] 推定：テクスチャセット「%s」（%s / 類似度 %.2f）"), *E.SlotName, *G.Key, Why, Score));
		};

		bool bAnyNeeds = false;
		for (const TSharedPtr<FPtSlotEntry>& E : Entries) { bAnyNeeds |= NeedsAny(*E); }
		if (!bAnyNeeds) { return; }

		if (Groups.Num() == 0) { return; }

		struct FCand { int32 Slot; int32 Group; float Score; };
		TArray<FCand> Cands;
		for (int32 s = 0; s < Entries.Num(); ++s)
		{
			const FPtSlotEntry& E = *Entries[s];
			if (!NeedsAny(E)) { continue; }

			int32 BestG = INDEX_NONE;
			float Best = 0.f, Second = 0.f;
			for (int32 g = 0; g < Groups.Num(); ++g)
			{
				if (!CanFill(E, Groups[g])) { continue; }
				float Score = 0.f;
				for (const TPair<FString, float>& N : Names[s])
				{
					Score = FMath::Max(Score, NameSimilarity(N.Key, Groups[g].Key) * N.Value);
				}
				Score += (Groups[g].bLocal && Score > 0.f) ? 0.05f : 0.f; // 似ているもの同士なら、メッシュの近くを優先
				if (Score > Best) { Second = Best; Best = Score; BestG = g; }
				else if (Score > Second) { Second = Score; }
			}
			if (BestG != INDEX_NONE && Best >= MinScore && (Best - Second) >= MinMargin)
			{
				Cands.Add({ s, BestG, Best });
			}
		}

		Cands.Sort([](const FCand& A, const FCand& B) { return A.Score > B.Score; });
		TSet<int32> UsedGroups;
		TSet<int32> DoneSlots;
		for (const FCand& C : Cands)
		{
			if (UsedGroups.Contains(C.Group)) { continue; } // 同じセットを別スロットに二重に使わない
			Fill(*Entries[C.Slot], Groups[C.Group], TEXT("名前の類似"), C.Score);
			UsedGroups.Add(C.Group);
			DoneSlots.Add(C.Slot);
		}

		// 消去法：メッシュと同じフォルダに未使用のセットが1つだけ、空のスロットも1つだけなら、それだと推定する
		TArray<int32> StillNeed;
		for (int32 s = 0; s < Entries.Num(); ++s)
		{
			if (!DoneSlots.Contains(s) && NeedsAny(*Entries[s])) { StillNeed.Add(s); }
		}
		if (StillNeed.Num() == 1)
		{
			int32 OnlyG = INDEX_NONE;
			int32 LocalUnused = 0;
			for (int32 g = 0; g < Groups.Num(); ++g)
			{
				if (Groups[g].bLocal && !UsedGroups.Contains(g) && CanFill(*Entries[StillNeed[0]], Groups[g]))
				{
					OnlyG = g;
					++LocalUnused;
				}
			}
			if (LocalUnused == 1)
			{
				Fill(*Entries[StillNeed[0]], Groups[OnlyG], TEXT("消去法：メッシュのフォルダに残った唯一のセット"), 0.f);
			}
		}
	}

	/**
	 * 必須の3枠に入ったテクスチャと同じセット（同じフォルダ・同じセット名）に、Emissive / Roughness などの
	 * 追加マップがあれば自動で入れる。FBXの参照や名前検索には出てこない追加マップを拾うための補完。
	 * セット名が同じなので間違いにくく、「推定」ではなく通常の自動入力として扱う。
	 */
	void FillExtrasFromSet(const FPtSettings& S, const TArray<FTexGroup>& Groups,
		const TArray<TSharedPtr<FPtSlotEntry>>& Entries, TFunctionRef<void(const FString&)> AddNote)
	{
		if (S.Maps.Num() <= PtRequiredMapCount) { return; }
		for (const TSharedPtr<FPtSlotEntry>& EPtr : Entries)
		{
			FPtSlotEntry& E = *EPtr;
			const FTexGroup* Anchor = nullptr;
			for (int32 i = 0; i < PtRequiredMapCount && !Anchor; ++i)
			{
				if (!E.TexturePaths.IsValidIndex(i) || E.TexturePaths[i].IsNull()) { continue; }
				FString SetKey;
				if (ClassifyTextureName(E.TexturePaths[i].GetAssetName(), S, SetKey) == INDEX_NONE) { continue; }
				const FString Folder = FPackageName::GetLongPackagePath(E.TexturePaths[i].GetLongPackageName());
				for (const FTexGroup& G : Groups)
				{
					if (G.Folder == Folder && G.Key.Equals(SetKey, ESearchCase::IgnoreCase)) { Anchor = &G; break; }
				}
			}
			if (!Anchor) { continue; }

			TArray<FString> Added;
			for (int32 i = PtRequiredMapCount; i < S.Maps.Num() && i < MapCount; ++i)
			{
				if (E.Sources.IsValidIndex(i) && E.Sources[i] == EPtTexSource::None
					&& E.TexturePaths.IsValidIndex(i) && E.TexturePaths[i].IsNull() && Anchor->Counts[i] == 1
					&& !EntryUsesPath(E, Anchor->Paths[i]))
				{
					E.TexturePaths[i] = Anchor->Paths[i];
					E.Sources[i] = EPtTexSource::Auto;
					Added.Add(Anchor->Paths[i].GetAssetName());
				}
			}
			if (Added.Num() > 0)
			{
				AddNote(FString::Printf(TEXT("  [%s] 追加マップ：同じセット「%s」から %s"),
					*E.SlotName, *Anchor->Key, *FString::Join(Added, TEXT(", "))));
			}
		}
	}

	/**
	 * 「候補を見る」用。自動で決まっていない（必須枠が空 / 推定 / 重複）スロットに、
	 * 似た名前のテクスチャセットを近い順に並べて持たせる。メッシュと同じフォルダのセットは類似度が低くても入れる。
	 */
	void BuildCandidates(const FPtSettings& S, const TArray<FTexGroup>& Groups,
		const TArray<TSharedPtr<FPtSlotEntry>>& Entries, const TArray<TArray<TPair<FString, float>>>& Names)
	{
		constexpr int32 MaxCandidates = 6;
		constexpr float MinScore = 0.15f;

		struct FScored { int32 Group; float Score; };
		for (int32 s = 0; s < Entries.Num(); ++s)
		{
			FPtSlotEntry& E = *Entries[s];
			E.Candidates.Reset();

			bool bUseful = E.Sources.Contains(EPtTexSource::Guess) || E.Sources.Contains(EPtTexSource::Ambiguous);
			for (int32 i = 0; i < S.Maps.Num() && i < PtRequiredMapCount && !bUseful; ++i)
			{
				bUseful = !E.TexturePaths.IsValidIndex(i) || E.TexturePaths[i].IsNull();
			}
			if (!bUseful) { continue; }

			TArray<FScored> List;
			for (int32 g = 0; g < Groups.Num(); ++g)
			{
				bool bHasRequired = false;
				for (int32 i = 0; i < PtRequiredMapCount; ++i) { bHasRequired |= (Groups[g].Counts[i] == 1); }
				if (!bHasRequired) { continue; }

				float Score = 0.f;
				for (const TPair<FString, float>& N : Names[s])
				{
					Score = FMath::Max(Score, NameSimilarity(N.Key, Groups[g].Key) * N.Value);
				}
				if (Groups[g].bLocal && Score > 0.f) { Score += 0.05f; }
				if (Score >= MinScore || Groups[g].bLocal)
				{
					List.Add({ g, Score });
				}
			}
			List.Sort([&Groups](const FScored& A, const FScored& B)
			{
				if (!FMath::IsNearlyEqual(A.Score, B.Score)) { return A.Score > B.Score; }
				return Groups[A.Group].bLocal && !Groups[B.Group].bLocal;
			});

			for (int32 k = 0; k < List.Num() && k < MaxCandidates; ++k)
			{
				const FTexGroup& G = Groups[List[k].Group];
				FPtCandidate C;
				C.SetName = G.Key;
				C.Folder = G.Folder;
				C.Score = FMath::Min(List[k].Score, 1.f);
				C.Paths.SetNum(S.Maps.Num());
				for (int32 i = 0; i < S.Maps.Num() && i < MapCount; ++i)
				{
					if (G.Counts[i] == 1) { C.Paths[i] = G.Paths[i]; }
				}
				E.Candidates.Add(MoveTemp(C));
			}
		}
	}
}

TArray<TSharedPtr<FPtSlotEntry>> PtAssignerCore::Analyze(const FPtSettings& S, const TArray<UObject*>& Meshes, TArray<FString>* OutNotes)
{
	TArray<TSharedPtr<FPtSlotEntry>> Result;
	IAssetRegistry& AR = GetRegistry();

	auto AddNote = [OutNotes](const FString& Line)
	{
		if (OutNotes) { OutNotes->Add(Line); }
	};

	// FBX のプロパティ名を ORM 枠として扱うキーワード
	TArray<FString> OrmKeys;
	S.FbxOrmKeywords.ParseIntoArray(OrmKeys, TEXT(","), true);
	for (FString& Key : OrmKeys)
	{
		Key.TrimStartAndEndInline();
		Key = Key.ToLower();
	}

	// メッシュのフォルダ以下のテクスチャ索引（フォルダごとに1回だけ作る）
	TMap<FString, FTexIndex> FolderIndexCache;

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
		// ---- FBX の参照情報（材質 → 画像ファイル名）を用意する ----
		TSharedPtr<PtFbx::FResult> Fbx;
		const FTexIndex* LocalIndex = nullptr;
		const FString MeshPackage = Mesh->GetOutermost()->GetName();
		const FString MeshFolder = FPackageName::GetLongPackagePath(MeshPackage);
		{
			if (!FolderIndexCache.Contains(MeshFolder))
			{
				FTexIndex NewIndex;
				BuildFolderIndex(AR, MeshFolder, NewIndex);
				FolderIndexCache.Add(MeshFolder, NewIndex);
			}
			LocalIndex = FolderIndexCache.Find(MeshFolder);
		}
		TArray<TSharedPtr<FPtSlotEntry>> MeshEntries;
		TArray<TArray<TPair<FString, float>>> MeshNames;
		if (S.bUseFbx)
		{
			FString Why;
			const FString FbxPath = ResolveFbxPath(Mesh, S, Why);
			if (FbxPath.IsEmpty())
			{
				AddNote(FString::Printf(TEXT("[%s] FBX参照なし：%s"), *Mesh->GetName(), *Why));
			}
			else
			{
				FString LoadError;
				Fbx = LoadFbxCached(FbxPath, LoadError);
				if (!Fbx.IsValid())
				{
					AddNote(FString::Printf(TEXT("[%s] %s"), *Mesh->GetName(), *LoadError));
				}
				else
				{
					AddNote(FString::Printf(TEXT("[%s] FBX：%s（%s / 材質 %d 件・テクスチャノード %d 件）"),
						*Mesh->GetName(), *FbxPath, *Fbx->Format, Fbx->Materials.Num(), Fbx->TextureNodeCount));
				}
			}
		}

		TArray<FName> Slots;
		GetSlotNames(Mesh, Slots);
		TArray<FName> ImportedSlots;
		GetImportedSlotNames(Mesh, ImportedSlots);
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
			E->Hints.Init(FString(), S.Maps.Num());

			// 前に手動で選んだ対応は、名前検索やFBXより優先して使う（自動判定が間違っていた箇所の修正を残すため）
			if (S.bRememberManual)
			{
				for (int32 i = 0; i < S.Maps.Num(); ++i)
				{
					const FString* Saved = Choices().Find(ChoiceKey(*E, S.Maps[i].Param));
					if (!Saved) { continue; }
					const FSoftObjectPath Path(*Saved);
					if (!AR.GetAssetByObjectPath(Path).IsValid())
					{
						AddNote(FString::Printf(TEXT("  [%s] %s：記憶していたテクスチャが見つかりません（%s）"),
							*E->SlotName, *S.Maps[i].Param, *Path.GetAssetName()));
						continue;
					}
					E->TexturePaths[i] = Path;
					E->Sources[i] = EPtTexSource::Saved;
					AddNote(FString::Printf(TEXT("  [%s] %s ← 記憶した選択「%s」"), *E->SlotName, *S.Maps[i].Param, *Path.GetAssetName()));
				}
			}

			// 名前検索（従来どおりのハード参照）。見つかったものは自動でアタッチする
			for (int32 i = 0; i < S.Maps.Num(); ++i)
			{
				if (E->Sources[i] == EPtTexSource::Saved) { continue; }
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
				if (i >= PtRequiredMapCount && EntryUsesPath(*E, (*Paths)[0]))
				{
					continue; // 追加マップに、すでに別の枠で使っているテクスチャは入れない
				}
				E->TexturePaths[i] = (*Paths)[0];
				E->Sources[i] = EPtTexSource::Auto;
			}

			// 推定マッチ用の手がかり（名前, 重み）。FBX材質名・FBX画像名は下で追加する
			TArray<TPair<FString, float>> Names;
			Names.Add(TPair<FString, float>(E->SlotName, 1.0f));
			if (ImportedSlots.IsValidIndex(SlotIdx) && ImportedSlots[SlotIdx].ToString() != E->SlotName)
			{
				Names.Add(TPair<FString, float>(ImportedSlots[SlotIdx].ToString(), 1.0f));
			}
			Names.Add(TPair<FString, float>(E->MeshName, 0.7f));

			// FBX の参照情報で、名前検索で埋まらなかった枠を補う（名前検索で見つかった枠は優先）
			if (Fbx.IsValid())
			{
				const FString ImportedName = ImportedSlots.IsValidIndex(SlotIdx) ? ImportedSlots[SlotIdx].ToString() : FString();
				const PtFbx::FMaterialInfo* FbxMat = FindFbxMaterial(*Fbx, E->SlotName, ImportedName, Slots.Num());
				if (!FbxMat)
				{
					AddNote(FString::Printf(TEXT("  [%s] FBXに対応する材質がありません"), *E->SlotName));
				}
				else
				{
					Names.Add(TPair<FString, float>(FbxMat->Name, 1.0f));
					for (const PtFbx::FTextureRef& Ref : FbxMat->Textures)
					{
						const FString Base = PtFbx::BaseNameNoExt(Ref.FileName);
						FString SetKey;
						Names.Add(TPair<FString, float>(
							ClassifyTextureName(Base, S, SetKey) != INDEX_NONE ? SetKey : Base, 0.9f));
					}
					for (int32 i = 0; i < S.Maps.Num(); ++i)
					{
						if (E->Sources[i] == EPtTexSource::Auto || E->Sources[i] == EPtTexSource::Saved)
						{
							continue;
						}
						const PtFbx::FTextureRef* Ref = nullptr;
						for (const PtFbx::FTextureRef& Candidate : FbxMat->Textures)
						{
							if (ClassifyFbxProperty(Candidate.Property, OrmKeys) == i)
							{
								Ref = &Candidate;
								break;
							}
						}
						if (!Ref)
						{
							continue;
						}

						const FString RefBase = PtFbx::BaseNameNoExt(Ref->FileName);
						const FString RefFile = FPaths::GetCleanFilename(Ref->FileName.Replace(TEXT("\\"), TEXT("/")));
						FSoftObjectPath Picked;
						bool bAmbiguousRef = false;
						if (FindTextureByFbxName(RefBase, LocalIndex, Index, MeshPackage, Picked, bAmbiguousRef))
						{
							// FBXでは複数のプロパティが同じ画像（ORM など）を指すことがある。追加マップは同じテクスチャを重ねて入れない
							if (i >= PtRequiredMapCount && EntryUsesPath(*E, Picked))
							{
								AddNote(FString::Printf(TEXT("  [%s] %s ← FBX「%s」は別の枠で使用中のため、追加マップには入れません"),
									*E->SlotName, *S.Maps[i].Param, *RefFile));
								continue;
							}
							E->TexturePaths[i] = Picked;
							E->Sources[i] = EPtTexSource::Fbx;
							AddNote(FString::Printf(TEXT("  [%s] %s ← FBX「%s」= %s"),
								*E->SlotName, *S.Maps[i].Param, *RefFile, *Picked.GetAssetName()));
						}
						else if (bAmbiguousRef)
						{
							E->Sources[i] = EPtTexSource::Ambiguous;
							E->Hints[i] = RefBase;
							AddNote(FString::Printf(TEXT("  [%s] %s ← FBX「%s」は同名のテクスチャが複数あり決められません"),
								*E->SlotName, *S.Maps[i].Param, *RefFile));
						}
						else
						{
							E->Hints[i] = FString::Printf(TEXT("FBXの参照「%s」に一致するテクスチャが見つかりません"), *RefFile);
							AddNote(FString::Printf(TEXT("  [%s] %s ← FBX「%s」に一致するテクスチャがありません"),
								*E->SlotName, *S.Maps[i].Param, *RefFile));
						}
					}

					// どの枠にも当てはまらなかったFBXのプロパティ（ORMの設定の手がかり）
					for (const PtFbx::FTextureRef& Other : FbxMat->Textures)
					{
						if (ClassifyFbxProperty(Other.Property, OrmKeys) == INDEX_NONE)
						{
							AddNote(FString::Printf(TEXT("  [%s] 未使用のFBXプロパティ：%s = %s"),
								*E->SlotName, *Other.Property, *FPaths::GetCleanFilename(Other.FileName.Replace(TEXT("\\"), TEXT("/")))));
						}
					}
				}
			}

			MeshEntries.Add(E);
			MeshNames.Add(MoveTemp(Names));
		}

		// 名前検索・FBX参照のどちらでも埋まらなかった枠を、似たテクスチャセットから推定する
		TArray<FTexGroup> Groups;
		BuildTexGroups(Index, LocalIndex, MeshFolder, S, Groups); // 推定・追加マップの補完・候補プレビューで共通に使う
		if (S.bUseGuess)
		{
			GuessForMesh(S, Groups, MeshEntries, MeshNames, AddNote);
		}
		// 追加マップ（Emissive など）は、決まったセットの兄弟テクスチャから補う
		FillExtrasFromSet(S, Groups, MeshEntries, AddNote);
		BuildCandidates(S, Groups, MeshEntries, MeshNames);
		for (const TSharedPtr<FPtSlotEntry>& E : MeshEntries)
		{
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

	// 同じテクスチャが、用途の違う枠（例：ある行では BaseColor、別の行では ORM）で使われていると、
	// 設定をどちらに合わせても片方が壊れる。そのテクスチャの設定は触らずに知らせる
	TSet<FSoftObjectPath> NoFix;
	{
		TMap<FSoftObjectPath, int32> UseOf;
		for (const TSharedPtr<FPtSlotEntry>& E : Entries)
		{
			if (!E->bChecked || E->Status != EPtStatus::Ready) { continue; }
			for (int32 i = 0; i < E->TexturePaths.Num() && i < S.Maps.Num(); ++i)
			{
				if (E->TexturePaths[i].IsNull()) { continue; }
				if (const int32* Prev = UseOf.Find(E->TexturePaths[i]))
				{
					if (UsageOf(*Prev) != UsageOf(i)) { NoFix.Add(E->TexturePaths[i]); }
				}
				else
				{
					UseOf.Add(E->TexturePaths[i], i);
				}
			}
		}
		for (const FSoftObjectPath& P : NoFix)
		{
			Log(FString::Printf(TEXT("[注意] %s は用途の違う枠で使われているため、sRGB・圧縮は変更しません"), *P.GetAssetName()));
		}
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
		switch (ApplyOne(S, *E, Master, Dirty, NoFix, Msg))
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

void PtAssignerCore::FixTextureSettingsOnly(const FPtSettings& S, const TArray<TSharedPtr<FPtSlotEntry>>& Entries,
	TFunctionRef<void(const FString&)> Log)
{
	// 画像ごとに「どの枠で使われているか」を集める。用途の違う枠で使われていたら触らない
	TMap<FSoftObjectPath, int32> UseOf;
	TSet<FSoftObjectPath> Conflict;
	for (const TSharedPtr<FPtSlotEntry>& E : Entries)
	{
		for (int32 i = 0; i < E->TexturePaths.Num() && i < S.Maps.Num(); ++i)
		{
			if (E->TexturePaths[i].IsNull()) { continue; }
			if (const int32* Prev = UseOf.Find(E->TexturePaths[i]))
			{
				if (UsageOf(*Prev) != UsageOf(i)) { Conflict.Add(E->TexturePaths[i]); }
			}
			else
			{
				UseOf.Add(E->TexturePaths[i], i);
			}
		}
	}
	if (UseOf.Num() == 0)
	{
		Log(TEXT("設定を直すテクスチャがありません（一覧の各枠にテクスチャが入っている行が対象です）"));
		return;
	}

	TArray<UPackage*> Dirty;
	int32 Checked = 0, Changed = 0, Failed = 0;
	for (const TPair<FSoftObjectPath, int32>& Pair : UseOf)
	{
		if (Conflict.Contains(Pair.Key))
		{
			Log(FString::Printf(TEXT("[注意] %s は用途の違う枠で使われているため、変更しません"), *Pair.Key.GetAssetName()));
			continue;
		}
		UTexture2D* Tex = Cast<UTexture2D>(Pair.Key.TryLoad());
		if (!Tex)
		{
			Log(FString::Printf(TEXT("[失敗] 読み込めません：%s"), *Pair.Key.ToString()));
			++Failed;
			continue;
		}
		++Checked;
		TextureCompressionSettings Comp;
		bool bSRGB;
		if (DesiredTextureSettings(Tex, Pair.Value, Comp, bSRGB))
		{
			Log(TEXT("[変更] ") + DescribeSettingsChange(Tex, S.Maps[Pair.Value], Comp, bSRGB));
			FixTexture(Tex, Comp, bSRGB, Dirty);
			++Changed;
		}
	}
	if (S.bSaveAfterApply && Dirty.Num() > 0)
	{
		UEditorLoadingAndSavingUtils::SavePackages(Dirty, /*bPromptUserToSave*/ false);
	}
	Log(FString::Printf(TEXT("テクスチャ設定の確認が完了：確認 %d 枚 / 変更 %d 枚 / 失敗 %d 枚"), Checked, Changed, Failed));
}
