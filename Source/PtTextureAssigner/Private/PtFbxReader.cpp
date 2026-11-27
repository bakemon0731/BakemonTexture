#include "PtFbxReader.h"

namespace PtFbx
{
namespace
{
	// ---------------------------------------------------------------- 文字列ユーティリティ
#ifdef PT_FBX_STANDALONE_TEST
	FString FromUtf8(const uint8* P, int64 Len)
	{
		return FString(reinterpret_cast<const char*>(P), Len);
	}
#else
	FString FromUtf8(const uint8* P, int64 Len)
	{
		if (Len <= 0)
		{
			return FString();
		}
		FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(P), static_cast<int32>(Len));
		return FString(Conv.Length(), Conv.Get());
	}
#endif

	/** 最初の NUL までを文字列にする（バイナリFBXの "Name\0\1Material" 対策） */
	FString FromUtf8UntilNul(const uint8* P, int64 Len)
	{
		int64 N = 0;
		while (N < Len && P[N] != 0)
		{
			++N;
		}
		return FromUtf8(P, N);
	}

	FString Int64ToString(int64 V)
	{
		uint8 Digits[32];
		int32 N = 0;
		const bool bNeg = V < 0;
		uint64 U = bNeg ? (static_cast<uint64>(0) - static_cast<uint64>(V)) : static_cast<uint64>(V);
		if (U == 0)
		{
			Digits[N++] = '0';
		}
		while (U > 0 && N < 30)
		{
			Digits[N++] = static_cast<uint8>('0' + static_cast<int32>(U % 10));
			U /= 10;
		}
		if (bNeg)
		{
			Digits[N++] = '-';
		}
		uint8 Rev[32];
		for (int32 i = 0; i < N; ++i)
		{
			Rev[i] = Digits[N - 1 - i];
		}
		return FromUtf8(Rev, N);
	}

	/** "Material::Body" → "Body" */
	FString StripTypePrefix(const FString& In)
	{
		const int32 Pos = In.Find(TEXT("::"));
		return Pos >= 0 ? In.Mid(Pos + 2) : In;
	}

	// ---------------------------------------------------------------- 中間表現
	struct FRawObj
	{
		FString Key;      // 接続で参照される識別子（IDの数字、または旧形式の名前）
		FString AltKey;   // 予備の識別子（"Material::Body" など）
		FString Name;
		FString FileName;
		FString RelName;
	};

	struct FRawConn
	{
		FString Type;     // "OO" / "OP"
		FString Child;
		FString Parent;
		FString Prop;
	};

	struct FRaw
	{
		TArray<FRawObj> Materials;
		TArray<FRawObj> Textures;
		TArray<FRawObj> Videos;
		TArray<FRawConn> Conns;
	};

	bool IEq(const FString& A, const TCHAR* B)
	{
		return A.Equals(FString(B), ESearchCase::IgnoreCase);
	}

	// ---------------------------------------------------------------- バイナリ
	struct FBin
	{
		const uint8* D = nullptr;
		int64 N = 0;

		bool Has(int64 Pos, int64 Len) const { return Pos >= 0 && Len >= 0 && Pos <= N && Len <= N - Pos; }
		uint32 U32(int64 Pos) const
		{
			return static_cast<uint32>(D[Pos]) | (static_cast<uint32>(D[Pos + 1]) << 8)
				| (static_cast<uint32>(D[Pos + 2]) << 16) | (static_cast<uint32>(D[Pos + 3]) << 24);
		}
		uint64 U64(int64 Pos) const
		{
			return static_cast<uint64>(U32(Pos)) | (static_cast<uint64>(U32(Pos + 4)) << 32);
		}
	};

	struct FNodeHdr
	{
		uint64 EndOffset = 0;
		uint64 NumProps = 0;
		uint64 PropsLen = 0;
		uint8 NameLen = 0;
		int64 PropsPos = 0;
		int64 ChildrenPos = 0;
		FString Name;
		bool bNull = false;
	};

	bool ReadHdr(const FBin& B, int64 Pos, bool bWide, FNodeHdr& H)
	{
		const int64 HdrSize = bWide ? 25 : 13;
		if (!B.Has(Pos, HdrSize))
		{
			return false;
		}
		if (bWide)
		{
			H.EndOffset = B.U64(Pos);
			H.NumProps = B.U64(Pos + 8);
			H.PropsLen = B.U64(Pos + 16);
			H.NameLen = B.D[Pos + 24];
		}
		else
		{
			H.EndOffset = B.U32(Pos);
			H.NumProps = B.U32(Pos + 4);
			H.PropsLen = B.U32(Pos + 8);
			H.NameLen = B.D[Pos + 12];
		}
		if (H.EndOffset == 0 && H.NumProps == 0 && H.PropsLen == 0 && H.NameLen == 0)
		{
			H.bNull = true;
			return true;
		}
		const int64 NamePos = Pos + HdrSize;
		if (!B.Has(NamePos, H.NameLen))
		{
			return false;
		}
		H.Name = FromUtf8(B.D + NamePos, H.NameLen);
		H.PropsPos = NamePos + H.NameLen;
		if (H.PropsLen > static_cast<uint64>(B.N - H.PropsPos))
		{
			return false;
		}
		H.ChildrenPos = H.PropsPos + static_cast<int64>(H.PropsLen);
		if (H.EndOffset > static_cast<uint64>(B.N) || static_cast<int64>(H.EndOffset) < H.ChildrenPos)
		{
			return false;
		}
		return true;
	}

	struct FProp
	{
		uint8 Type = 0;
		int64 Int = 0;
		FString Str;
	};

	bool ReadProps(const FBin& B, const FNodeHdr& H, TArray<FProp>& Out, int32 MaxProps)
	{
		int64 P = H.PropsPos;
		const int64 End = H.ChildrenPos;
		const uint64 Count = H.NumProps < static_cast<uint64>(MaxProps) ? H.NumProps : static_cast<uint64>(MaxProps);

		for (uint64 k = 0; k < Count; ++k)
		{
			if (P >= End)
			{
				return false;
			}
			FProp Pr;
			Pr.Type = B.D[P];
			++P;

			int64 Need = 0;
			switch (Pr.Type)
			{
			case 'C': Need = 1; break;
			case 'Y': Need = 2; break;
			case 'I': case 'F': Need = 4; break;
			case 'D': case 'L': Need = 8; break;
			default: break;
			}

			if (Need > 0)
			{
				if (P + Need > End || !B.Has(P, Need))
				{
					return false;
				}
				if (Pr.Type == 'C')
				{
					Pr.Int = B.D[P];
				}
				else if (Pr.Type == 'Y')
				{
					Pr.Int = static_cast<int16>(static_cast<uint16>(B.D[P]) | (static_cast<uint16>(B.D[P + 1]) << 8));
				}
				else if (Pr.Type == 'I')
				{
					Pr.Int = static_cast<int32>(B.U32(P));
				}
				else if (Pr.Type == 'L')
				{
					Pr.Int = static_cast<int64>(B.U64(P));
				}
				P += Need;
			}
			else if (Pr.Type == 'S' || Pr.Type == 'R')
			{
				if (P + 4 > End || !B.Has(P, 4))
				{
					return false;
				}
				const int64 Len = B.U32(P);
				P += 4;
				if (P + Len > End || !B.Has(P, Len))
				{
					return false;
				}
				if (Pr.Type == 'S')
				{
					Pr.Str = FromUtf8UntilNul(B.D + P, Len);
				}
				P += Len;
			}
			else if (Pr.Type == 'f' || Pr.Type == 'i' || Pr.Type == 'd' || Pr.Type == 'l' || Pr.Type == 'b')
			{
				// 配列は中身を使わないので、長さ分だけ読み飛ばす（圧縮されていても展開不要）
				if (P + 12 > End || !B.Has(P, 12))
				{
					return false;
				}
				const int64 ArrLen = B.U32(P);
				const uint32 Encoding = B.U32(P + 4);
				const int64 CompLen = B.U32(P + 8);
				P += 12;
				int64 Elem = 1;
				if (Pr.Type == 'f' || Pr.Type == 'i') { Elem = 4; }
				else if (Pr.Type == 'd' || Pr.Type == 'l') { Elem = 8; }
				const int64 DataLen = (Encoding == 0) ? ArrLen * Elem : CompLen;
				if (DataLen < 0 || P + DataLen > End || !B.Has(P, DataLen))
				{
					return false;
				}
				P += DataLen;
			}
			else
			{
				return false;
			}
			Out.Add(Pr);
		}
		return true;
	}

	template<class FVisit>
	bool ForEachChild(const FBin& B, const FNodeHdr& Parent, bool bWide, FVisit Visit)
	{
		int64 Pos = Parent.ChildrenPos;
		while (Pos < static_cast<int64>(Parent.EndOffset))
		{
			FNodeHdr H;
			if (!ReadHdr(B, Pos, bWide, H))
			{
				return false;
			}
			if (H.bNull)
			{
				return true;
			}
			Visit(H);
			Pos = static_cast<int64>(H.EndOffset);
		}
		return true;
	}

	void ReadObjBinary(const FBin& B, const FNodeHdr& H, bool bWide, FRawObj& Obj)
	{
		TArray<FProp> Props;
		if (ReadProps(B, H, Props, 3) && Props.Num() >= 2 && Props[1].Type == 'S')
		{
			Obj.Key = Int64ToString(Props[0].Int);
			Obj.Name = Props[1].Str;
			Obj.AltKey = Obj.Name;
		}
		ForEachChild(B, H, bWide, [&](const FNodeHdr& C)
		{
			const bool bFile = IEq(C.Name, TEXT("FileName"));
			const bool bRel = IEq(C.Name, TEXT("RelativeFilename"));
			if (!bFile && !bRel)
			{
				return;
			}
			TArray<FProp> CP;
			if (ReadProps(B, C, CP, 1) && CP.Num() >= 1 && CP[0].Type == 'S')
			{
				(bFile ? Obj.FileName : Obj.RelName) = CP[0].Str;
			}
		});
	}

	bool ParseBinary(const uint8* Data, int64 Size, FRaw& Raw, FResult& Out)
	{
		FBin B;
		B.D = Data;
		B.N = Size;
		if (Size < 27)
		{
			Out.Error = TEXT("FBXが小さすぎます");
			return false;
		}
		const uint32 Version = B.U32(23);
		Out.Format = FString(TEXT("Binary ")) + Int64ToString(Version);
		if (Version < 7000)
		{
			Out.Error = FString(TEXT("未対応のFBXバージョンです：")) + Int64ToString(Version);
			return false;
		}
		const bool bWide = Version >= 7500;

		FNodeHdr Root;
		Root.ChildrenPos = 27;
		Root.EndOffset = static_cast<uint64>(Size);

		bool bOk = ForEachChild(B, Root, bWide, [&](const FNodeHdr& Top)
		{
			if (Top.Name == TEXT("Objects"))
			{
				ForEachChild(B, Top, bWide, [&](const FNodeHdr& O)
				{
					if (O.Name == TEXT("Material") || O.Name == TEXT("Texture") || O.Name == TEXT("Video"))
					{
						FRawObj Obj;
						ReadObjBinary(B, O, bWide, Obj);
						if (!Obj.Key.IsEmpty())
						{
							if (O.Name == TEXT("Material")) { Raw.Materials.Add(Obj); }
							else if (O.Name == TEXT("Texture")) { Raw.Textures.Add(Obj); }
							else { Raw.Videos.Add(Obj); }
						}
					}
				});
			}
			else if (Top.Name == TEXT("Connections"))
			{
				ForEachChild(B, Top, bWide, [&](const FNodeHdr& C)
				{
					if (C.Name != TEXT("C"))
					{
						return;
					}
					TArray<FProp> P;
					if (ReadProps(B, C, P, 4) && P.Num() >= 3 && P[0].Type == 'S')
					{
						FRawConn Conn;
						Conn.Type = P[0].Str;
						Conn.Child = Int64ToString(P[1].Int);
						Conn.Parent = Int64ToString(P[2].Int);
						if (P.Num() >= 4 && P[3].Type == 'S')
						{
							Conn.Prop = P[3].Str;
						}
						Raw.Conns.Add(Conn);
					}
				});
			}
		});

		if (!bOk && Raw.Materials.Num() == 0)
		{
			Out.Error = TEXT("FBXの構造を読み取れませんでした（破損している可能性があります）");
			return false;
		}
		return true;
	}

	// ---------------------------------------------------------------- ASCII
	struct FAsciiLine
	{
		FString Key;
		TArray<FString> Tokens;
		bool bOpen = false;
	};

	bool IsSpace(uint8 C) { return C == ' ' || C == '\t' || C == '\r'; }

	bool ParseAsciiLine(const uint8* D, int64 A, int64 B, FAsciiLine& L)
	{
		int64 C = A;
		while (C < B && D[C] != ':')
		{
			++C;
		}
		if (C >= B)
		{
			return false;
		}
		int64 KeyEnd = C;
		while (KeyEnd > A && IsSpace(D[KeyEnd - 1]))
		{
			--KeyEnd;
		}
		L.Key = FromUtf8(D + A, KeyEnd - A);
		L.bOpen = (D[B - 1] == '{');

		const int64 End = L.bOpen ? B - 1 : B;
		int64 P = C + 1;
		while (P < End)
		{
			while (P < End && (IsSpace(D[P]) || D[P] == ','))
			{
				++P;
			}
			if (P >= End)
			{
				break;
			}
			if (D[P] == '"')
			{
				int64 Q = P + 1;
				while (Q < End && D[Q] != '"')
				{
					++Q;
				}
				L.Tokens.Add(FromUtf8(D + P + 1, Q - (P + 1)));
				P = Q + 1;
			}
			else
			{
				int64 Q = P;
				while (Q < End && D[Q] != ',')
				{
					++Q;
				}
				int64 TokEnd = Q;
				while (TokEnd > P && IsSpace(D[TokEnd - 1]))
				{
					--TokEnd;
				}
				L.Tokens.Add(FromUtf8(D + P, TokEnd - P));
				P = Q;
			}
		}
		return true;
	}

	bool ParseAscii(const uint8* D, int64 N, FRaw& Raw)
	{
		TArray<FString> Stack;
		FRawObj Cur;
		int32 CurKind = 0;   // 0=なし 1=Material 2=Texture 3=Video
		bool bHaveCur = false;

		int64 Pos = 0;
		while (Pos < N)
		{
			int64 LineEnd = Pos;
			while (LineEnd < N && D[LineEnd] != '\n')
			{
				++LineEnd;
			}
			int64 A = Pos;
			int64 B = LineEnd;
			Pos = LineEnd + 1;

			while (A < B && IsSpace(D[A])) { ++A; }
			while (B > A && IsSpace(D[B - 1])) { --B; }
			if (A >= B || D[A] == ';')
			{
				continue;
			}

			if (D[A] == '}')
			{
				if (Stack.Num() == 2 && Stack[0] == TEXT("Objects") && bHaveCur)
				{
					if (CurKind == 1) { Raw.Materials.Add(Cur); }
					else if (CurKind == 2) { Raw.Textures.Add(Cur); }
					else if (CurKind == 3) { Raw.Videos.Add(Cur); }
					bHaveCur = false;
					CurKind = 0;
				}
				if (Stack.Num() > 0)
				{
					Stack.Pop();
				}
				continue;
			}

			const bool bOpen = (D[B - 1] == '{');
			const int32 Depth = Stack.Num();

			// 関係のある階層だけ中身を解析する。ジオメトリ等の巨大ブロックは括弧の対応だけ追う
			const bool bInObjects = Depth >= 1 && Stack[0] == TEXT("Objects");
			const bool bRelevant =
				(Depth == 1 && (bInObjects || Stack[0] == TEXT("Connections"))) ||
				(Depth == 2 && bInObjects && bHaveCur);

			if (!bRelevant)
			{
				if (bOpen)
				{
					int64 C = A;
					while (C < B && D[C] != ':') { ++C; }
					int64 KeyEnd = C;
					while (KeyEnd > A && IsSpace(D[KeyEnd - 1])) { --KeyEnd; }
					Stack.Add(FromUtf8(D + A, KeyEnd - A));
				}
				continue;
			}

			FAsciiLine L;
			if (!ParseAsciiLine(D, A, B, L))
			{
				continue;
			}

			if (Depth == 1 && bInObjects)
			{
				int32 Kind = 0;
				if (L.Key == TEXT("Material")) { Kind = 1; }
				else if (L.Key == TEXT("Texture")) { Kind = 2; }
				else if (L.Key == TEXT("Video")) { Kind = 3; }

				if (Kind != 0 && L.Tokens.Num() >= 1)
				{
					FRawObj Obj;
					const FString& First = L.Tokens[0];
					const bool bNumeric = First.Len() > 0 && ((First[0] >= '0' && First[0] <= '9') || First[0] == '-');
					if (bNumeric && L.Tokens.Num() >= 2)
					{
						Obj.Key = First;
						Obj.AltKey = L.Tokens[1];
						Obj.Name = StripTypePrefix(L.Tokens[1]);
					}
					else
					{
						Obj.Key = First;          // FBX 6.x 形式: 名前がそのままキー
						Obj.AltKey = First;
						Obj.Name = StripTypePrefix(First);
					}
					if (bOpen)
					{
						Cur = Obj;
						CurKind = Kind;
						bHaveCur = true;
						Stack.Add(L.Key);
					}
					else if (Kind == 1) { Raw.Materials.Add(Obj); }
					else if (Kind == 2) { Raw.Textures.Add(Obj); }
					else { Raw.Videos.Add(Obj); }
				}
				else if (bOpen)
				{
					Stack.Add(L.Key);
				}
			}
			else if (Depth == 1)   // Connections
			{
				if ((L.Key == TEXT("C") || L.Key == TEXT("Connect")) && L.Tokens.Num() >= 3)
				{
					FRawConn Conn;
					Conn.Type = L.Tokens[0];
					Conn.Child = L.Tokens[1];
					Conn.Parent = L.Tokens[2];
					if (L.Tokens.Num() >= 4)
					{
						Conn.Prop = L.Tokens[3];
					}
					Raw.Conns.Add(Conn);
				}
				else if (bOpen)
				{
					Stack.Add(L.Key);
				}
			}
			else   // Depth == 2: Material / Texture / Video の直下
			{
				if (L.Tokens.Num() >= 1)
				{
					if (IEq(L.Key, TEXT("FileName"))) { Cur.FileName = L.Tokens[0]; }
					else if (IEq(L.Key, TEXT("RelativeFilename")) || IEq(L.Key, TEXT("RelPath"))) { Cur.RelName = L.Tokens[0]; }
				}
				if (bOpen)
				{
					Stack.Add(L.Key);
				}
			}
		}
		return true;
	}

	// ---------------------------------------------------------------- 接続の解決
	FString PickFile(const FRawObj& O)
	{
		return O.FileName.IsEmpty() ? O.RelName : O.FileName;
	}

	void Resolve(const FRaw& Raw, FResult& Out)
	{
		TMap<FString, int32> MatByKey;
		TMap<FString, int32> TexByKey;
		TMap<FString, int32> VidByKey;

		for (int32 i = 0; i < Raw.Materials.Num(); ++i)
		{
			FMaterialInfo Info;
			Info.Name = Raw.Materials[i].Name;
			Out.Materials.Add(Info);
			MatByKey.Add(Raw.Materials[i].Key, i);
			if (!Raw.Materials[i].AltKey.IsEmpty()) { MatByKey.Add(Raw.Materials[i].AltKey, i); }
		}
		for (int32 i = 0; i < Raw.Textures.Num(); ++i)
		{
			TexByKey.Add(Raw.Textures[i].Key, i);
			if (!Raw.Textures[i].AltKey.IsEmpty()) { TexByKey.Add(Raw.Textures[i].AltKey, i); }
		}
		for (int32 i = 0; i < Raw.Videos.Num(); ++i)
		{
			VidByKey.Add(Raw.Videos[i].Key, i);
			if (!Raw.Videos[i].AltKey.IsEmpty()) { VidByKey.Add(Raw.Videos[i].AltKey, i); }
		}
		Out.TextureNodeCount = Raw.Textures.Num();

		// Video → Texture（テクスチャ自身にファイル名が無い場合の予備）
		TArray<int32> TexVideo;
		for (int32 i = 0; i < Raw.Textures.Num(); ++i)
		{
			TexVideo.Add(-1);
		}
		for (const FRawConn& C : Raw.Conns)
		{
			const int32* V = VidByKey.Find(C.Child);
			const int32* T = TexByKey.Find(C.Parent);
			if (V && T)
			{
				TexVideo[*T] = *V;
			}
		}

		// Texture → Material
		for (const FRawConn& C : Raw.Conns)
		{
			const int32* T = TexByKey.Find(C.Child);
			const int32* M = MatByKey.Find(C.Parent);
			if (!T || !M)
			{
				continue;
			}
			FString File = PickFile(Raw.Textures[*T]);
			if (File.IsEmpty() && TexVideo[*T] >= 0)
			{
				File = PickFile(Raw.Videos[TexVideo[*T]]);
			}
			if (File.IsEmpty())
			{
				continue;
			}
			FTextureRef Ref;
			Ref.Property = C.Prop;
			Ref.FileName = File;
			Out.Materials[*M].Textures.Add(Ref);
		}
	}
}

bool Read(const uint8* Data, int64 Size, FResult& Out)
{
	if (!Data || Size <= 0)
	{
		Out.Error = TEXT("FBXが空です");
		return false;
	}

	FRaw Raw;
	static const uint8 Magic[18] = { 'K','a','y','d','a','r','a',' ','F','B','X',' ','B','i','n','a','r','y' };
	bool bBinary = Size >= 18;
	for (int32 i = 0; bBinary && i < 18; ++i)
	{
		bBinary = (Data[i] == Magic[i]);
	}

	if (bBinary)
	{
		if (!ParseBinary(Data, Size, Raw, Out))
		{
			return false;
		}
	}
	else
	{
		Out.Format = TEXT("ASCII");
		ParseAscii(Data, Size, Raw);
		if (Raw.Materials.Num() == 0 && Raw.Textures.Num() == 0 && Raw.Conns.Num() == 0)
		{
			Out.Error = TEXT("FBXとして読み取れませんでした（形式が未対応か、材質情報がありません）");
			return false;
		}
	}

	Resolve(Raw, Out);
	return true;
}

FString BaseNameNoExt(const FString& Path)
{
	int32 Start = 0;
	for (int32 i = 0; i < Path.Len(); ++i)
	{
		if (Path[i] == TEXT('/') || Path[i] == TEXT('\\'))
		{
			Start = i + 1;
		}
	}
	FString Name = Path.Mid(Start);
	int32 Dot = -1;
	for (int32 i = 0; i < Name.Len(); ++i)
	{
		if (Name[i] == TEXT('.'))
		{
			Dot = i;
		}
	}
	if (Dot > 0)
	{
		Name = Name.Left(Dot);
	}
	return Name;
}
}
