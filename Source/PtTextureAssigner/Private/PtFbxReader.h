#pragma once

// FBX（バイナリ / ASCII）から「材質 → テクスチャ画像ファイル名」の対応だけを取り出す軽量リーダー。
// FBX SDK は使わない。ジオメトリ等は読み飛ばす。

#ifdef PT_FBX_STANDALONE_TEST
#include "PtFbxShim.h"
#else
#include "CoreMinimal.h"
#endif

namespace PtFbx
{
	/** 材質につながっているテクスチャ1枚分 */
	struct FTextureRef
	{
		FString Property;   // 接続先のプロパティ名（DiffuseColor / NormalMap / Bump など）
		FString FileName;   // FBXに書かれたファイル名（絶対 or 相対パス）
	};

	struct FMaterialInfo
	{
		FString Name;       // "Material::" 等を除いた材質名（= UEのスロット名）
		TArray<FTextureRef> Textures;
	};

	struct FResult
	{
		TArray<FMaterialInfo> Materials;
		FString Format;                 // "Binary 7400" / "ASCII"
		int32 TextureNodeCount = 0;     // FBX内のテクスチャノード数
		FString Error;                  // 空なら成功
	};

	/** メモリ上の FBX を解析する。失敗時は false で Out.Error に理由 */
	bool Read(const uint8* Data, int64 Size, FResult& Out);

	/** パスの最後の要素から拡張子を除いたもの（/ と \ の両方に対応）。"C:\a\001.png" → "001" */
	FString BaseNameNoExt(const FString& Path);
}
