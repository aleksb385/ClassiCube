#include "Formats.h"
#include "World.h"
#include "Deflate.h"
#include "Block.h"
#include "Entity.h"
#include "Platform.h"
#include "ExtMath.h"
#include "Logger.h"
#include "Game.h"
#include "Server.h"
#include "Event.h"
#include "Funcs.h"
#include "Errors.h"
#include "Stream.h"
#include "Chat.h"
#include "Inventory.h"
#include "TexturePack.h"


/*########################################################################################################################*
*--------------------------------------------------------General----------------------------------------------------------*
*#########################################################################################################################*/
static ReturnCode Map_ReadBlocks(struct Stream* stream) {
	World.Volume = World.Width * World.Length * World.Height;
	World.Blocks = (BlockRaw*)Mem_Alloc(World.Volume, 1, "map blocks");
	return Stream_Read(stream, World.Blocks, World.Volume);
}

static ReturnCode Map_SkipGZipHeader(struct Stream* stream) {
	struct GZipHeader gzHeader;
	ReturnCode res;
	GZipHeader_Init(&gzHeader);

	while (!gzHeader.Done) {
		if ((res = GZipHeader_Read(stream, &gzHeader))) return res;
	}
	return 0;
}

IMapImporter Map_FindImporter(const String* path) {
	const static String cw  = String_FromConst(".cw"),  lvl = String_FromConst(".lvl");
	const static String fcm = String_FromConst(".fcm"), dat = String_FromConst(".dat");

	if (String_CaselessEnds(path, &cw))  return Cw_Load;
#ifndef CC_BUILD_WEB
	if (String_CaselessEnds(path, &lvl)) return Lvl_Load;
	if (String_CaselessEnds(path, &fcm)) return Fcm_Load;
	if (String_CaselessEnds(path, &dat)) return Dat_Load;
#endif

	return NULL;
}

void Map_LoadFrom(const String* path) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	struct LocationUpdate update;
	IMapImporter importer;
	struct Stream stream;
	ReturnCode res;
	Game_Reset();
	
	res = Stream_OpenFile(&stream, path);
	if (res) { Logger_Warn2(res, "opening", path); return; }

	importer = Map_FindImporter(path);
	if ((res = importer(&stream))) {
		World_Reset();
		Logger_Warn2(res, "decoding", path); stream.Close(&stream); return;
	}

	res = stream.Close(&stream);
	if (res) { Logger_Warn2(res, "closing", path); }

	World_SetNewMap(World.Blocks, World.Width, World.Height, World.Length);
	Event_RaiseVoid(&WorldEvents.MapLoaded);

	LocationUpdate_MakePosAndOri(&update, p->Spawn, p->SpawnRotY, p->SpawnHeadX, false);
	p->Base.VTABLE->SetLocation(&p->Base, &update, false);
}


/*########################################################################################################################*
*--------------------------------------------------MCSharp level Format---------------------------------------------------*
*#########################################################################################################################*/
#define LVL_CUSTOMTILE 163
#define LVL_CHUNKSIZE 16
/* MCSharp* format is a GZIP compressed binary map format. All metadata is discarded.
	U16 "Identifier" (must be 1874)
	U16 "Width",  "Length", "Height"
	U16 "SpawnX", "SpawnZ", "SpawnY"
	U8  "Yaw", "Pitch"
	U16 "Build permissions" (ignored)
	U8* "Blocks"
	
	-- this data is only in MCGalaxy maps
	U8  "Identifier" (0xBD for 'block definitions', i.e. custom blocks)
	U8* "Data"       (16x16x16 sparsely allocated chunks)
}*/

const static uint8_t Lvl_table[256] = {
	 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
	16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
	32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
	48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
	64, 65,  0,  0,  0,  0, 39, 36, 36, 10, 46, 21, 22, 22, 22, 22,
	 4,  0, 22, 21,  0, 22, 23, 24, 22, 26, 27, 28, 30, 31, 32, 33,
	34, 35, 36, 22, 20, 49, 45,  1,  4,  0,  9, 11,  4, 19,  5, 17,
	10, 49, 20,  1, 18, 12,  5, 25, 46, 44, 17, 49, 20,  1, 18, 12,
	 5, 25, 36, 34,  0,  9, 11, 46, 44,  0,  9, 11,  8, 10, 22, 27,
	22,  8, 10, 28, 17, 49, 20,  1, 18, 12,  5, 25, 46, 44, 11,  9,
	 0,  9, 11, 163, 0,  0,  9, 11,  0,  0,  0,  0,  0,  0,  0, 28,
	22, 21, 11,  0,  0,  0, 46, 46, 10, 10, 46, 20, 41, 42, 11,  9,
	 0,  8, 10, 10,  8,  0, 22, 22,  0,  0,  0,  0,  0,  0,  0,  0,
	 0,  0,  0, 21, 10,  0,  0,  0,  0,  0, 22, 22, 42,  3,  2, 29,
	47,  0,  0,  0,  0,  0, 27, 46, 48, 24, 22, 36, 34,  8, 10, 21,
	29, 22, 10, 22, 22, 41, 19, 35, 21, 29, 49, 34, 16, 41,  0, 22
};

static ReturnCode Lvl_ReadCustomBlocks(struct Stream* stream) {	
	uint8_t chunk[LVL_CHUNKSIZE * LVL_CHUNKSIZE * LVL_CHUNKSIZE];
	uint8_t hasCustom;
	int baseIndex, index, xx, yy, zz;
	ReturnCode res;
	int x, y, z, i;

	/* skip bounds checks when we know chunk is entirely inside map */
	int adjWidth  = World.Width  & ~0x0F;
	int adjHeight = World.Height & ~0x0F;
	int adjLength = World.Length & ~0x0F;

	for (y = 0; y < World.Height; y += LVL_CHUNKSIZE) {
		for (z = 0; z < World.Length; z += LVL_CHUNKSIZE) {
			for (x = 0; x < World.Width; x += LVL_CHUNKSIZE) {

				if ((res = stream->ReadU8(stream, &hasCustom))) return res;
				if (hasCustom != 1) continue;
				if ((res = Stream_Read(stream, chunk, sizeof(chunk)))) return res;
				baseIndex = World_Pack(x, y, z);

				if ((x + LVL_CHUNKSIZE) <= adjWidth && (y + LVL_CHUNKSIZE) <= adjHeight && (z + LVL_CHUNKSIZE) <= adjLength) {
					for (i = 0; i < sizeof(chunk); i++) {
						xx = i & 0xF; yy = (i >> 8) & 0xF; zz = (i >> 4) & 0xF;

						index = baseIndex + World_Pack(xx, yy, zz);
						World.Blocks[index] = World.Blocks[index] == LVL_CUSTOMTILE ? chunk[i] : World.Blocks[index];
					}
				} else {
					for (i = 0; i < sizeof(chunk); i++) {
						xx = i & 0xF; yy = (i >> 8) & 0xF; zz = (i >> 4) & 0xF;
						if ((x + xx) >= World.Width || (y + yy) >= World.Height || (z + zz) >= World.Length) continue;

						index = baseIndex + World_Pack(xx, yy, zz);
						World.Blocks[index] = World.Blocks[index] == LVL_CUSTOMTILE ? chunk[i] : World.Blocks[index];
					}
				}
			}
		}
	}
	return 0;
}

ReturnCode Lvl_Load(struct Stream* stream) {
	uint8_t header[18];
	uint8_t* blocks;
	uint8_t section;
	ReturnCode res;
	int i;

	struct LocalPlayer* p = &LocalPlayer_Instance;
	struct Stream compStream;
	struct InflateState state;
	Inflate_MakeStream(&compStream, &state, stream);
	
	if ((res = Map_SkipGZipHeader(stream)))                       return res;
	if ((res = Stream_Read(&compStream, header, sizeof(header)))) return res;
	if (Stream_GetU16_LE(&header[0]) != 1874) return LVL_ERR_VERSION;

	World.Width  = Stream_GetU16_LE(&header[2]);
	World.Length = Stream_GetU16_LE(&header[4]);
	World.Height = Stream_GetU16_LE(&header[6]);

	p->Spawn.X = Stream_GetU16_LE(&header[8]);
	p->Spawn.Z = Stream_GetU16_LE(&header[10]);
	p->Spawn.Y = Stream_GetU16_LE(&header[12]);
	p->SpawnRotY  = Math_Packed2Deg(header[14]);
	p->SpawnHeadX = Math_Packed2Deg(header[15]);
	/* (2) pervisit, perbuild permissions */

	if ((res = Map_ReadBlocks(&compStream))) return res;
	blocks = World.Blocks;
	/* Bulk convert 4 blocks at once */
	for (i = 0; i < (World.Volume & ~3); i += 4) {
		*blocks = Lvl_table[*blocks]; blocks++;
		*blocks = Lvl_table[*blocks]; blocks++;
		*blocks = Lvl_table[*blocks]; blocks++;
		*blocks = Lvl_table[*blocks]; blocks++;
	}
	for (; i < World.Volume; i++) {
		*blocks = Lvl_table[*blocks]; blocks++;
	}

	/* 0xBD section type is not present in older .lvl files */
	res = compStream.ReadU8(&compStream, &section);
	if (res == ERR_END_OF_STREAM) return 0;

	if (res) return res;
	return section == 0xBD ? Lvl_ReadCustomBlocks(&compStream) : 0;
}


/*########################################################################################################################*
*----------------------------------------------------fCraft map format----------------------------------------------------*
*#########################################################################################################################*/
/* fCraft* format is a binary map format. All metadata is discarded.
	U32 "Identifier" (must be FC2AF40)
	U8  "Revision"   (only '13' supported)
	U16 "Width",  "Height", "Length"
	U32 "SpawnX", "SpawnY", "SpawnZ"
	U8  "Yaw", "Pitch"
	U32 "DateModified", "DateCreated" (ignored)
	U8* "UUID"
	U8  "Layers"     (only maps with 1 layer supported)
	U8* "LayersInfo" (ignored, assumes only layer is map blocks)
	U32 "MetaCount"
	METADATA { STR "Group", "Key", "Value" }
	U8* "Blocks"
}*/
static ReturnCode Fcm_ReadString(struct Stream* stream) {
	uint8_t data[2];
	int len;
	ReturnCode res;

	if ((res = Stream_Read(stream, data, sizeof(data)))) return res;
	len = Stream_GetU16_LE(data);

	return stream->Skip(stream, len);
}

ReturnCode Fcm_Load(struct Stream* stream) {
	uint8_t header[79];	
	ReturnCode res;
	int i, count;

	struct LocalPlayer* p = &LocalPlayer_Instance;
	struct Stream compStream;
	struct InflateState state;
	Inflate_MakeStream(&compStream, &state, stream);

	if ((res = Stream_Read(stream, header, sizeof(header)))) return res;
	if (Stream_GetU32_LE(&header[0]) != 0x0FC2AF40UL)        return FCM_ERR_IDENTIFIER;
	if (header[4] != 13) return FCM_ERR_REVISION;
	
	World.Width  = Stream_GetU16_LE(&header[5]);
	World.Height = Stream_GetU16_LE(&header[7]);
	World.Length = Stream_GetU16_LE(&header[9]);
	
	p->Spawn.X = ((int)Stream_GetU32_LE(&header[11])) / 32.0f;
	p->Spawn.Y = ((int)Stream_GetU32_LE(&header[15])) / 32.0f;
	p->Spawn.Z = ((int)Stream_GetU32_LE(&header[19])) / 32.0f;
	p->SpawnRotY  = Math_Packed2Deg(header[23]);
	p->SpawnHeadX = Math_Packed2Deg(header[24]);

	/* header[25] (4) date modified */
	/* header[29] (4) date created */
	Mem_Copy(&World.Uuid, &header[33], sizeof(World.Uuid));
	/* header[49] (26) layer index */
	count = (int)Stream_GetU32_LE(&header[75]);

	/* header isn't compressed, rest of data is though */
	for (i = 0; i < count; i++) {
		if ((res = Fcm_ReadString(&compStream))) return res; /* Group */
		if ((res = Fcm_ReadString(&compStream))) return res; /* Key   */
		if ((res = Fcm_ReadString(&compStream))) return res; /* Value */
	}

	return Map_ReadBlocks(&compStream);
}


/*########################################################################################################################*
*---------------------------------------------------------NBTFile---------------------------------------------------------*
*#########################################################################################################################*/
enum NbtTagType { 
	NBT_END, NBT_I8,  NBT_I16, NBT_I32,  NBT_I64,  NBT_F32, 
	NBT_R64, NBT_I8S, NBT_STR, NBT_LIST, NBT_DICT, NBT_I32S
};

#define NBT_SMALL_SIZE  STRING_SIZE
#define NBT_STRING_SIZE STRING_SIZE
#define NbtTag_IsSmall(tag) ((tag)->DataSize <= NBT_SMALL_SIZE)
struct NbtTag;

struct NbtTag {
	struct NbtTag* Parent;
	uint8_t  TagID;
	char     NameBuffer[NBT_STRING_SIZE];
	String   Name;
	uint32_t DataSize; /* size of data for arrays */

	union {
		uint8_t  U8;
		int16_t  I16;
		uint16_t U16;
		uint32_t U32;
		float    F32;
		uint8_t  Small[NBT_SMALL_SIZE];
		uint8_t* Big; /* malloc for big byte arrays */
		struct { String Text; char Buffer[NBT_STRING_SIZE]; } Str;
	} Value;
};

static uint8_t NbtTag_U8(struct NbtTag* tag) {
	if (tag->TagID != NBT_I8) Logger_Abort("Expected I8 NBT tag");
	return tag->Value.U8;
}

static int16_t NbtTag_I16(struct NbtTag* tag) {
	if (tag->TagID != NBT_I16) Logger_Abort("Expected I16 NBT tag");
	return tag->Value.I16;
}

static uint16_t NbtTag_U16(struct NbtTag* tag) {
	if (tag->TagID != NBT_I16) Logger_Abort("Expected I16 NBT tag");
	return tag->Value.U16;
}

static float NbtTag_F32(struct NbtTag* tag) {
	if (tag->TagID != NBT_F32) Logger_Abort("Expected F32 NBT tag");
	return tag->Value.F32;
}

static uint8_t* NbtTag_U8_Array(struct NbtTag* tag, int minSize) {
	if (tag->TagID != NBT_I8S) Logger_Abort("Expected I8_Array NBT tag");
	if (tag->DataSize < minSize) Logger_Abort("I8_Array NBT tag too small");

	return NbtTag_IsSmall(tag) ? tag->Value.Small : tag->Value.Big;
}

static String NbtTag_String(struct NbtTag* tag) {
	if (tag->TagID != NBT_STR) Logger_Abort("Expected String NBT tag");
	return tag->Value.Str.Text;
}

static ReturnCode Nbt_ReadString(struct Stream* stream, String* str) {
	uint8_t buffer[NBT_STRING_SIZE * 4];
	int len;
	ReturnCode res;

	if ((res = Stream_Read(stream, buffer, 2)))   return res;
	len = Stream_GetU16_BE(buffer);

	if (len > Array_Elems(buffer)) return CW_ERR_STRING_LEN;
	if ((res = Stream_Read(stream, buffer, len))) return res;

	String_AppendUtf8(str, buffer, len);
	return 0;
}

typedef void (*Nbt_Callback)(struct NbtTag* tag);
static ReturnCode Nbt_ReadTag(uint8_t typeId, bool readTagName, struct Stream* stream, struct NbtTag* parent, Nbt_Callback callback) {
	struct NbtTag tag;
	uint8_t childType;
	uint8_t tmp[5];	
	ReturnCode res;
	uint32_t i, count;
	
	if (typeId == NBT_END) return 0;
	tag.TagID  = typeId; 
	tag.Parent = parent;
	tag.DataSize = 0;
	String_InitArray(tag.Name, tag.NameBuffer);

	if (readTagName) {
		res = Nbt_ReadString(stream, &tag.Name);
		if (res) return res;
	}

	switch (typeId) {
	case NBT_I8:
		res = stream->ReadU8(stream, &tag.Value.U8);
		break;
	case NBT_I16:
		res = Stream_Read(stream, tmp, 2);
		tag.Value.U16 = Stream_GetU16_BE(tmp);
		break;
	case NBT_I32:
	case NBT_F32:
		res = Stream_ReadU32_BE(stream, &tag.Value.U32);
		break;
	case NBT_I64:
	case NBT_R64:
		res = stream->Skip(stream, 8);
		break; /* (8) data */

	case NBT_I8S:
		if ((res = Stream_ReadU32_BE(stream, &tag.DataSize))) break;

		if (NbtTag_IsSmall(&tag)) {
			res = Stream_Read(stream, tag.Value.Small, tag.DataSize);
		} else {
			tag.Value.Big = (uint8_t*)Mem_Alloc(tag.DataSize, 1, "NBT data");
			res = Stream_Read(stream, tag.Value.Big, tag.DataSize);
			if (res) { Mem_Free(tag.Value.Big); }
		}
		break;
	case NBT_STR:
		String_InitArray(tag.Value.Str.Text, tag.Value.Str.Buffer);
		res = Nbt_ReadString(stream, &tag.Value.Str.Text);
		break;

	case NBT_LIST:
		if ((res = Stream_Read(stream, tmp, 5))) break;
		childType = tmp[0];
		count = Stream_GetU32_BE(&tmp[1]);

		for (i = 0; i < count; i++) {
			res = Nbt_ReadTag(childType, false, stream, &tag, callback);
			if (res) break;
		}
		break;

	case NBT_DICT:
		for (;;) {
			if ((res = stream->ReadU8(stream, &childType))) break;
			if (childType == NBT_END) break;

			res = Nbt_ReadTag(childType, true, stream, &tag, callback);
			if (res) break;
		}
		break;

	case NBT_I32S: return NBT_ERR_INT32S;
	default:       return NBT_ERR_UNKNOWN;
	}

	if (res) return res;
	callback(&tag);
	/* NOTE: callback must set DataBig to NULL, if doesn't want it to be freed */
	if (!NbtTag_IsSmall(&tag)) Mem_Free(tag.Value.Big);
	return 0;
}
#define IsTag(tag, tagName) (String_CaselessEqualsConst(&tag->Name, tagName))

/*########################################################################################################################*
*--------------------------------------------------ClassicWorld format----------------------------------------------------*
*#########################################################################################################################*/
/* ClassicWorld is a NBT tag based map format. Tags not listed below are discarded.
COMPOUND "ClassicWorld" {
	U8* "UUID"
	U16 "X", "Y", "Z"
	COMPOUND "Spawn" {
		I16 "X", "Y", "Z"
		U8  "H", "P"
	}
	U8* "BlockArray"  (lower 8 bits, required)
	U8* "BlockArray2" (upper 8 bits, optional)
	COMPOUND "Metadata" {
		COMPOUND "CPE" {
			COMPOUND "ClickDistance"  { U16 "Reach" }
			COMPOUND "EnvWeatherType" { U8 "WeatherType" }
			COMPOUND "EnvMapAppearance" {
				U8 "SideBlock", "EdgeBlock"
				I16 "SidesLevel"
				STR "TextureURL"
			}
			COMPOUND "EnvColors" {
				COMPOUND "Sky"      { U16 "R", "G", "B" }
				COMPOUND "Cloud"    { U16 "R", "G", "B" }
				COMPOUND "Fog"      { U16 "R", "G", "B" }
				COMPOUND "Sunlight" { U16 "R", "G", "B" }
				COMPOUND "Ambient"  { U16 "R", "G", "B" }
			}
			COMPOUND "BlockDefinitions" {
				COMPOUND "Block_XYZ" { (name must start with 'Block')
					U8  "ID", U16 "ID2"
					STR "Name"
					F32 "Speed"
					U8  "CollideType", "BlockDraw"					
					U8  "TransmitsLight", "FullBright"
					U8  "Shape"	, "WalkSound"	
					U8* "Textures", "Fog", "Coords"
				}
			}
		}
	}
}*/
static BlockRaw* Cw_GetBlocks(struct NbtTag* tag) {
	BlockRaw* ptr;
	if (NbtTag_IsSmall(tag)) {
		ptr = (BlockRaw*)Mem_Alloc(tag->DataSize, 1, ".cw map blocks");
		Mem_Copy(ptr, tag->Value.Small, tag->DataSize);
	} else {
		ptr = tag->Value.Big;
		tag->Value.Big = NULL; /* So Nbt_ReadTag doesn't call Mem_Free on World.Blocks */
	}
	return ptr;
}

static void Cw_Callback_1(struct NbtTag* tag) {
	if (IsTag(tag, "X")) { World.Width  = NbtTag_U16(tag); return; }
	if (IsTag(tag, "Y")) { World.Height = NbtTag_U16(tag); return; }
	if (IsTag(tag, "Z")) { World.Length = NbtTag_U16(tag); return; }

	if (IsTag(tag, "UUID")) {
		if (tag->DataSize != sizeof(World.Uuid)) Logger_Abort("Map UUID must be 16 bytes");
		Mem_Copy(World.Uuid, tag->Value.Small, sizeof(World.Uuid));
		return;
	}

	if (IsTag(tag, "BlockArray")) {
		World.Volume = tag->DataSize;
		World.Blocks = Cw_GetBlocks(tag);
	}
#ifdef EXTENDED_BLOCKS
	if (IsTag(tag, "BlockArray2")) World_SetMapUpper(Cw_GetBlocks(tag));
#endif
}

static void Cw_Callback_2(struct NbtTag* tag) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	if (!IsTag(tag->Parent, "Spawn")) return;
	
	if (IsTag(tag, "X")) { p->Spawn.X = NbtTag_I16(tag); return; }
	if (IsTag(tag, "Y")) { p->Spawn.Y = NbtTag_I16(tag); return; }
	if (IsTag(tag, "Z")) { p->Spawn.Z = NbtTag_I16(tag); return; }
	if (IsTag(tag, "H")) { p->SpawnRotY  = Math_Packed2Deg(NbtTag_U8(tag)); return; }
	if (IsTag(tag, "P")) { p->SpawnHeadX = Math_Packed2Deg(NbtTag_U8(tag)); return; }
}

static BlockID cw_curID;
static int cw_colR, cw_colG, cw_colB;
static PackedCol Cw_ParseCol(PackedCol defValue) {
	int r = cw_colR, g = cw_colG, b = cw_colB;
	PackedCol c;
	if (r > 255 || g > 255 || b > 255) return defValue;

	c.R = r; c.G = g; c.B = b; c.A = 255;
	return c;		
}

static void Cw_Callback_4(struct NbtTag* tag) {
	BlockID id = cw_curID;
	struct LocalPlayer* p = &LocalPlayer_Instance;

	if (!IsTag(tag->Parent->Parent, "CPE")) return;
	if (!IsTag(tag->Parent->Parent->Parent, "Metadata")) return;

	if (IsTag(tag->Parent, "ClickDistance")) {
		if (IsTag(tag, "Distance")) { p->ReachDistance = NbtTag_U16(tag) / 32.0f; return; }
	}
	if (IsTag(tag->Parent, "EnvWeatherType")) {
		if (IsTag(tag, "WeatherType")) { Env.Weather = NbtTag_U8(tag); return; }
	}

	if (IsTag(tag->Parent, "EnvMapAppearance")) {
		if (IsTag(tag, "SideBlock")) { Env.SidesBlock = NbtTag_U8(tag);  return; }
		if (IsTag(tag, "EdgeBlock")) { Env.EdgeBlock  = NbtTag_U8(tag);  return; }
		if (IsTag(tag, "SideLevel")) { Env.EdgeHeight = NbtTag_I16(tag); return; }

		if (IsTag(tag, "TextureURL")) {
			String url = NbtTag_String(tag);
			if (Game_AllowServerTextures && url.length) {
				Server_RetrieveTexturePack(&url);
			}
			return;
		}
	}

	/* Callback for compound tag is called after all its children have been processed */
	if (IsTag(tag->Parent, "EnvColors")) {
		if (IsTag(tag, "Sky")) {
			Env.SkyCol = Cw_ParseCol(Env_DefaultSkyCol); return;
		} else if (IsTag(tag, "Cloud")) {
			Env.CloudsCol = Cw_ParseCol(Env_DefaultCloudsCol); return;
		} else if (IsTag(tag, "Fog")) {
			Env.FogCol = Cw_ParseCol(Env_DefaultFogCol); return;
		} else if (IsTag(tag, "Sunlight")) {
			Env_SetSunCol(Cw_ParseCol(Env_DefaultSunCol)); return;
		} else if (IsTag(tag, "Ambient")) {
			Env_SetShadowCol(Cw_ParseCol(Env_DefaultShadowCol)); return;
		}
	}

	if (IsTag(tag->Parent, "BlockDefinitions")) {
		const static String blockStr = String_FromConst("Block");
		if (!String_CaselessStarts(&tag->Name, &blockStr)) return;	

		/* hack for sprite draw (can't rely on order of tags when reading) */
		if (Blocks.SpriteOffset[id] == 0) {
			Blocks.SpriteOffset[id] = Blocks.Draw[id];
			Blocks.Draw[id] = DRAW_SPRITE;
		} else {
			Blocks.SpriteOffset[id] = 0;
		}

		Block_DefineCustom(id);
		Blocks.CanPlace[id]  = true;
		Blocks.CanDelete[id] = true;
		Event_RaiseVoid(&BlockEvents.PermissionsChanged);

		cw_curID = 0;
	}
}

static void Cw_Callback_5(struct NbtTag* tag) {
	BlockID id = cw_curID;
	uint8_t* arr;
	uint8_t sound;

	if (!IsTag(tag->Parent->Parent->Parent, "CPE")) return;
	if (!IsTag(tag->Parent->Parent->Parent->Parent, "Metadata")) return;

	if (IsTag(tag->Parent->Parent, "EnvColors")) {
		if (IsTag(tag, "R")) { cw_colR = NbtTag_U16(tag); return; }
		if (IsTag(tag, "G")) { cw_colG = NbtTag_U16(tag); return; }
		if (IsTag(tag, "B")) { cw_colB = NbtTag_U16(tag); return; }
	}

	if (IsTag(tag->Parent->Parent, "BlockDefinitions") && Game_AllowCustomBlocks) {
		if (IsTag(tag, "ID"))             { cw_curID = NbtTag_U8(tag);  return; }
		if (IsTag(tag, "ID2"))            { cw_curID = NbtTag_U16(tag); return; }
		if (IsTag(tag, "CollideType"))    { Block_SetCollide(id, NbtTag_U8(tag)); return; }
		if (IsTag(tag, "Speed"))          { Blocks.SpeedMultiplier[id] = NbtTag_F32(tag); return; }
		if (IsTag(tag, "TransmitsLight")) { Blocks.BlocksLight[id] = NbtTag_U8(tag) == 0; return; }
		if (IsTag(tag, "FullBright"))     { Blocks.FullBright[id] = NbtTag_U8(tag) != 0; return; }
		if (IsTag(tag, "BlockDraw"))      { Blocks.Draw[id] = NbtTag_U8(tag); return; }
		if (IsTag(tag, "Shape"))          { Blocks.SpriteOffset[id] = NbtTag_U8(tag); return; }

		if (IsTag(tag, "Name")) {
			String name = NbtTag_String(tag);
			Block_SetName(id, &name);
			return;
		}

		if (IsTag(tag, "Textures")) {
			arr = NbtTag_U8_Array(tag, 6);
			Block_Tex(id, FACE_YMAX) = arr[0]; Block_Tex(id, FACE_YMIN) = arr[1];
			Block_Tex(id, FACE_XMIN) = arr[2]; Block_Tex(id, FACE_XMAX) = arr[3];
			Block_Tex(id, FACE_ZMIN) = arr[4]; Block_Tex(id, FACE_ZMAX) = arr[5];

			/* hacky way of storing upper 8 bits */
			if (tag->DataSize >= 12) {
				Block_Tex(id, FACE_YMAX) |= arr[6]  << 8; Block_Tex(id, FACE_YMIN) |= arr[7]  << 8;
				Block_Tex(id, FACE_XMIN) |= arr[8]  << 8; Block_Tex(id, FACE_XMAX) |= arr[9]  << 8;
				Block_Tex(id, FACE_ZMIN) |= arr[10] << 8; Block_Tex(id, FACE_ZMAX) |= arr[11] << 8;
			}
			return;
		}
		
		if (IsTag(tag, "WalkSound")) {
			sound = NbtTag_U8(tag);
			Blocks.DigSounds[id]  = sound;
			Blocks.StepSounds[id] = sound;
			if (sound == SOUND_GLASS) Blocks.StepSounds[id] = SOUND_STONE;
			return;
		}

		if (IsTag(tag, "Fog")) {
			arr = NbtTag_U8_Array(tag, 4);
			Blocks.FogDensity[id] = (arr[0] + 1) / 128.0f;
			/* Fix for older ClassicalSharp versions which saved wrong fog density value */
			if (arr[0] == 0xFF) Blocks.FogDensity[id] = 0.0f;
 
			Blocks.FogCol[id].R = arr[1];
			Blocks.FogCol[id].G = arr[2];
			Blocks.FogCol[id].B = arr[3];
			Blocks.FogCol[id].A = 255;
			return;
		}

		if (IsTag(tag, "Coords")) {
			arr = NbtTag_U8_Array(tag, 6);
			Blocks.MinBB[id].X = (int8_t)arr[0] / 16.0f; Blocks.MaxBB[id].X = (int8_t)arr[3] / 16.0f;
			Blocks.MinBB[id].Y = (int8_t)arr[1] / 16.0f; Blocks.MaxBB[id].Y = (int8_t)arr[4] / 16.0f;
			Blocks.MinBB[id].Z = (int8_t)arr[2] / 16.0f; Blocks.MaxBB[id].Z = (int8_t)arr[5] / 16.0f;
			return;
		}
	}
}

static void Cw_Callback(struct NbtTag* tag) {
	struct NbtTag* tmp = tag->Parent;
	int depth = 0;
	while (tmp) { depth++; tmp = tmp->Parent; }

	switch (depth) {
	case 1: Cw_Callback_1(tag); return;
	case 2: Cw_Callback_2(tag); return;
	case 4: Cw_Callback_4(tag); return;
	case 5: Cw_Callback_5(tag); return;
	}
	/* ClassicWorld -> Metadata -> CPE -> ExtName -> [values]
	        0             1         2        3          4   */
}

ReturnCode Cw_Load(struct Stream* stream) {
	uint8_t tag;
	struct Stream compStream;
	struct InflateState state;
	Vector3* spawn; Vector3I pos;
	ReturnCode res;

	Inflate_MakeStream(&compStream, &state, stream);
	if ((res = Map_SkipGZipHeader(stream))) return res;
	if ((res = compStream.ReadU8(&compStream, &tag))) return res;

	if (tag != NBT_DICT) return CW_ERR_ROOT_TAG;
	res = Nbt_ReadTag(NBT_DICT, true, &compStream, NULL, Cw_Callback);
	if (res) return res;

	/* Older versions incorrectly multiplied spawn coords by * 32, so we check for that */
	spawn = &LocalPlayer_Instance.Spawn; 
	Vector3I_Floor(&pos, spawn);

	if (!World_Contains(pos.X, pos.Y, pos.Z)) { 
		spawn->X /= 32.0f; spawn->Y /= 32.0f; spawn->Z /= 32.0f; 
	}
	return 0;
}


/*########################################################################################################################*
*-------------------------------------------------Minecraft .dat format---------------------------------------------------*
*#########################################################################################################################*/
/* .dat is a java serialised map format. Rather than bothering following this, I skip a lot of it.
     Stream              BlockData        BlockDataTiny      BlockDataLong
|--------------|     |---------------|  |---------------|  |---------------| 
| U16 Magic    |     |>BlockDataTiny |  | TC_BLOCKDATA  |  | TC_BLOCKLONG  |
| U16 Version  |     |>BlockDataLong |  | U8 Size       |  | U32 Size      |
| Content[var] |     |_______________|  | U8 Data[size] |  | U8 Data[size] |
|______________|                        |_______________|  |_______________|

    Content
|--------------| |--------------| 
| >BlockData   | | >NewString   |
| >Object      | | >TC_RESET    |
|______________| | >TC_NULL     |
| >PrevObject     |
| >NewClass     |
| >NewEnum     |

}*/
enum JTypeCode {
	TC_NULL = 0x70, TC_REFERENCE = 0x71, TC_CLASSDESC = 0x72, TC_OBJECT = 0x73, 
	TC_STRING = 0x74, TC_ARRAY = 0x75, TC_ENDBLOCKDATA = 0x78
};
enum JFieldType {
	JFIELD_I8 = 'B', JFIELD_F32 = 'F', JFIELD_I32 = 'I', JFIELD_I64 = 'J',
	JFIELD_BOOL = 'Z', JFIELD_ARRAY = '[', JFIELD_OBJECT = 'L'
};

#define JNAME_SIZE 48
struct JFieldDesc {
	uint8_t Type;
	uint8_t FieldName[JNAME_SIZE];
	union {
		uint8_t  U8;
		int32_t  I32;
		uint32_t U32;
		float    F32;
		struct { uint8_t* Ptr; uint32_t Size; } Array;
	} Value;
};

struct JClassDesc {
	uint8_t ClassName[JNAME_SIZE];
	int FieldsCount;
	struct JFieldDesc Fields[22];
};

static ReturnCode Dat_ReadString(struct Stream* stream, uint8_t* buffer) {
	int len;
	ReturnCode res;

	if ((res = Stream_Read(stream, buffer, 2))) return res;
	len = Stream_GetU16_BE(buffer);

	Mem_Set(buffer, 0, JNAME_SIZE);
	if (len > JNAME_SIZE) return DAT_ERR_JSTRING_LEN;
	return Stream_Read(stream, buffer, len);
}

static ReturnCode Dat_ReadFieldDesc(struct Stream* stream, struct JFieldDesc* desc) {
	uint8_t typeCode;
	uint8_t className1[JNAME_SIZE];
	ReturnCode res;

	if ((res = stream->ReadU8(stream, &desc->Type)))     return res;
	if ((res = Dat_ReadString(stream, desc->FieldName))) return res;

	if (desc->Type == JFIELD_ARRAY || desc->Type == JFIELD_OBJECT) {		
		if ((res = stream->ReadU8(stream, &typeCode))) return res;

		if (typeCode == TC_STRING) {
			return Dat_ReadString(stream, className1);
		} else if (typeCode == TC_REFERENCE) {
			return stream->Skip(stream, 4); /* (4) handle */
		} else {
			return DAT_ERR_JFIELD_CLASS_NAME;
		}
	}
	return 0;
}

static ReturnCode Dat_ReadClassDesc(struct Stream* stream, struct JClassDesc* desc) {
	uint8_t typeCode;
	uint8_t count[2];
	struct JClassDesc superClassDesc;
	ReturnCode res;
	int i;

	if ((res = stream->ReadU8(stream, &typeCode))) return res;
	if (typeCode == TC_NULL) { desc->ClassName[0] = '\0'; desc->FieldsCount = 0; return 0; }
	if (typeCode != TC_CLASSDESC) return DAT_ERR_JCLASS_TYPE;

	if ((res = Dat_ReadString(stream, desc->ClassName))) return res;
	if ((res = stream->Skip(stream, 9))) return res; /* (8) serial version UID, (1) flags */

	if ((res = Stream_Read(stream, count, 2))) return res;
	desc->FieldsCount = Stream_GetU16_BE(count);
	if (desc->FieldsCount > Array_Elems(desc->Fields)) return DAT_ERR_JCLASS_FIELDS;
	
	for (i = 0; i < desc->FieldsCount; i++) {
		if ((res = Dat_ReadFieldDesc(stream, &desc->Fields[i]))) return res;
	}

	if ((res = stream->ReadU8(stream, &typeCode))) return res;
	if (typeCode != TC_ENDBLOCKDATA) return DAT_ERR_JCLASS_ANNOTATION;

	return Dat_ReadClassDesc(stream, &superClassDesc);
}

static ReturnCode Dat_ReadFieldData(struct Stream* stream, struct JFieldDesc* field) {
	uint8_t typeCode;
	String fieldName;
	uint32_t count;
	struct JClassDesc arrayClassDesc;
	ReturnCode res;

	switch (field->Type) {
	case JFIELD_I8:
	case JFIELD_BOOL:
		return stream->ReadU8(stream, &field->Value.U8);
	case JFIELD_F32:
	case JFIELD_I32:
		return Stream_ReadU32_BE(stream, &field->Value.U32);
	case JFIELD_I64:
		return stream->Skip(stream, 8); /* (8) data */

	case JFIELD_OBJECT: {
		/* Luckily for us, we only have to account for blockMap object */
		/* Other objects (e.g. player) are stored after the fields we actually care about, so ignore them */
		fieldName = String_FromRaw((char*)field->FieldName, JNAME_SIZE);
		if (!String_CaselessEqualsConst(&fieldName, "blockMap")) return 0;
		if ((res = stream->ReadU8(stream, &typeCode))) return res;

		/* Skip all blockMap data with awful hacks */
		/* These offsets were based on server_level.dat map from original minecraft classic server */
		if (typeCode == TC_OBJECT) {
			if ((res = stream->Skip(stream, 315)))         return res;
			if ((res = Stream_ReadU32_BE(stream, &count))) return res;

			if ((res = stream->Skip(stream, 17 * count)))  return res;
			if ((res = stream->Skip(stream, 152)))         return res;
		} else if (typeCode != TC_NULL) {
			/* WoM maps have this field as null, which makes things easier for us */
			return DAT_ERR_JOBJECT_TYPE;
		}
	} break;

	case JFIELD_ARRAY: {
		if ((res = stream->ReadU8(stream, &typeCode))) return res;
		if (typeCode == TC_NULL) break;
		if (typeCode != TC_ARRAY) return DAT_ERR_JARRAY_TYPE;

		if ((res = Dat_ReadClassDesc(stream, &arrayClassDesc))) return res;
		if (arrayClassDesc.ClassName[1] != JFIELD_I8) return DAT_ERR_JARRAY_CONTENT;

		if ((res = Stream_ReadU32_BE(stream, &count))) return res;
		field->Value.Array.Size = count;

		field->Value.Array.Ptr = (uint8_t*)Mem_Alloc(count, 1, ".dat map blocks");
		res = Stream_Read(stream, field->Value.Array.Ptr, count);
		if (res) { Mem_Free(field->Value.Array.Ptr); return res; }
	} break;
	}
	return 0;
}

static int32_t Dat_I32(struct JFieldDesc* field) {
	if (field->Type != JFIELD_I32) Logger_Abort("Field type must be Int32");
	return field->Value.I32;
}

ReturnCode Dat_Load(struct Stream* stream) {
	uint8_t header[10];
	struct JClassDesc obj;
	struct JFieldDesc* field;
	String fieldName;
	ReturnCode res;
	int i;

	struct LocalPlayer* p = &LocalPlayer_Instance;
	struct Stream compStream;
	struct InflateState state;
	Inflate_MakeStream(&compStream, &state, stream);

	if ((res = Map_SkipGZipHeader(stream)))                       return res;
	if ((res = Stream_Read(&compStream, header, sizeof(header)))) return res;
	/* .dat header */
	if (Stream_GetU32_BE(&header[0]) != 0x271BB788) return DAT_ERR_IDENTIFIER;
	if (header[4] != 0x02) return DAT_ERR_VERSION;

	/* Java seralisation headers */
	if (Stream_GetU16_BE(&header[5]) != 0xACED) return DAT_ERR_JIDENTIFIER;
	if (Stream_GetU16_BE(&header[7]) != 0x0005) return DAT_ERR_JVERSION;
	if (header[9] != TC_OBJECT)                 return DAT_ERR_ROOT_TYPE;
	if ((res = Dat_ReadClassDesc(&compStream, &obj))) return res;

	for (i = 0; i < obj.FieldsCount; i++) {
		field = &obj.Fields[i];
		if ((res = Dat_ReadFieldData(&compStream, field))) return res;
		fieldName = String_FromRaw((char*)field->FieldName, JNAME_SIZE);

		if (String_CaselessEqualsConst(&fieldName, "width")) {
			World.Width  = Dat_I32(field);
		} else if (String_CaselessEqualsConst(&fieldName, "height")) {
			World.Length = Dat_I32(field);
		} else if (String_CaselessEqualsConst(&fieldName, "depth")) {
			World.Height = Dat_I32(field);
		} else if (String_CaselessEqualsConst(&fieldName, "blocks")) {
			if (field->Type != JFIELD_ARRAY) Logger_Abort("Blocks field must be Array");
			World.Blocks = field->Value.Array.Ptr;
			World.Volume = field->Value.Array.Size;
		} else if (String_CaselessEqualsConst(&fieldName, "xSpawn")) {
			p->Spawn.X = (float)Dat_I32(field);
		} else if (String_CaselessEqualsConst(&fieldName, "ySpawn")) {
			p->Spawn.Y = (float)Dat_I32(field);
		} else if (String_CaselessEqualsConst(&fieldName, "zSpawn")) {
			p->Spawn.Z = (float)Dat_I32(field);
		}
	}
	return 0;
}


/*########################################################################################################################*
*--------------------------------------------------ClassicWorld export----------------------------------------------------*
*#########################################################################################################################*/
#define CW_META_RGB NBT_I16,0,1,'R',0,0,  NBT_I16,0,1,'G',0,0,  NBT_I16,0,1,'B',0,0,

static int Cw_WriteEndString(uint8_t* data, const String* text) {
	Codepoint cp;
	uint8_t* cur = data + 2;
	int i, wrote, len = 0;

	for (i = 0; i < text->length; i++) {
		cp    = Convert_CP437ToUnicode(text->buffer[i]);
		wrote = Convert_UnicodeToUtf8(cp, cur);
		len += wrote; cur += wrote;
	}

	Stream_SetU16_BE(data, len);
	*cur = NBT_END;
	return len + 1;
}

static uint8_t cw_begin[131] = {
NBT_DICT, 0,12, 'C','l','a','s','s','i','c','W','o','r','l','d',
	NBT_I8,   0,13, 'F','o','r','m','a','t','V','e','r','s','i','o','n', 1,
	NBT_I8S,  0,4,  'U','U','I','D', 0,0,0,16, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	NBT_I16,  0,1,  'X', 0,0,
	NBT_I16,  0,1,  'Y', 0,0,
	NBT_I16,  0,1,  'Z', 0,0,
	NBT_DICT, 0,5,  'S','p','a','w','n',
		NBT_I16,  0,1, 'X', 0,0,
		NBT_I16,  0,1, 'Y', 0,0,
		NBT_I16,  0,1, 'Z', 0,0,
		NBT_I8,   0,1, 'H', 0,
		NBT_I8,   0,1, 'P', 0,
	NBT_END,
	NBT_I8S,  0,10, 'B','l','o','c','k','A','r','r','a','y', 0,0,0,0,
};
static uint8_t cw_map2[18] = {
	NBT_I8S,  0,11, 'B','l','o','c','k','A','r','r','a','y','2', 0,0,0,0,
};
static uint8_t cw_meta_cpe[303] = {
	NBT_DICT, 0,8,  'M','e','t','a','d','a','t','a',
		NBT_DICT, 0,3, 'C','P','E',
			NBT_DICT, 0,13, 'C','l','i','c','k','D','i','s','t','a','n','c','e',
				NBT_I16,  0,8,  'D','i','s','t','a','n','c','e', 0,0,
			NBT_END,
			NBT_DICT, 0,14, 'E','n','v','W','e','a','t','h','e','r','T','y','p','e',
				NBT_I8,   0,11, 'W','e','a','t','h','e','r','T','y','p','e', 0,
			NBT_END,
			NBT_DICT, 0,9,  'E','n','v','C','o','l','o','r','s',
				NBT_DICT, 0,3, 'S','k','y',                     CW_META_RGB
				NBT_END,
				NBT_DICT, 0,5, 'C','l','o','u','d',             CW_META_RGB
				NBT_END,
				NBT_DICT, 0,3, 'F','o','g',                     CW_META_RGB
				NBT_END,
				NBT_DICT, 0,7, 'A','m','b','i','e','n','t',     CW_META_RGB
				NBT_END,
				NBT_DICT, 0,8, 'S','u','n','l','i','g','h','t', CW_META_RGB
				NBT_END,
			NBT_END,
			NBT_DICT, 0,16, 'E','n','v','M','a','p','A','p','p','e','a','r','a','n','c','e',
				NBT_I8,   0,9,  'S','i','d','e','B','l','o','c','k',     0,
				NBT_I8,   0,9,  'E','d','g','e','B','l','o','c','k',     0,
				NBT_I16,  0,9,  'S','i','d','e','L','e','v','e','l',     0,0,
				NBT_STR,  0,10, 'T','e','x','t','u','r','e','U','R','L', 0,0,
};
static uint8_t cw_meta_defs[19] = {
			NBT_DICT, 0,16, 'B','l','o','c','k','D','e','f','i','n','i','t','i','o','n','s',
};
static uint8_t cw_meta_def[189] = {
				NBT_DICT, 0,9,  'B','l','o','c','k','\0','\0','\0','\0',
					NBT_I8,  0,2,  'I','D',                              0,
					/* It would be have been better to just change ID to be a I16 */
					/* Unfortunately this isn't backwards compatible with ClassicalSharp */
					NBT_I16, 0,3,  'I','D','2',                          0,0,
					NBT_I8,  0,11, 'C','o','l','l','i','d','e','T','y','p','e', 0,
					NBT_F32, 0,5,  'S','p','e','e','d',                  0,0,0,0,
					/* Ugly hack for supporting texture IDs over 255 */
					/* First 6 elements are lower 8 bits, next 6 are upper 8 bits */
					NBT_I8S, 0,8,  'T','e','x','t','u','r','e','s',      0,0,0,12, 0,0,0,0,0,0, 0,0,0,0,0,0,
					NBT_I8,  0,14, 'T','r','a','n','s','m','i','t','s','L','i','g','h','t', 0,
					NBT_I8,  0,9,  'W','a','l','k','S','o','u','n','d',  0,
					NBT_I8,  0,10, 'F','u','l','l','B','r','i','g','h','t', 0,
					NBT_I8,  0,5,  'S','h','a','p','e',                  0,
					NBT_I8,  0,9,  'B','l','o','c','k','D','r','a','w',  0,
					NBT_I8S, 0,3,  'F','o','g',                          0,0,0,4, 0,0,0,0,
					NBT_I8S, 0,6,  'C','o','o','r','d','s',              0,0,0,6, 0,0,0,0,0,0,
					NBT_STR, 0,4,  'N','a','m','e',                      0,0,
};
static uint8_t cw_end[4] = {
			NBT_END,
		NBT_END,
	NBT_END,
NBT_END,
};


static ReturnCode Cw_WriteBockDef(struct Stream* stream, int b) {
	uint8_t tmp[512];
	String name;
	int len;

	bool sprite = Blocks.Draw[b] == DRAW_SPRITE;
	union IntAndFloat speed;
	TextureLoc tex;
	uint8_t fog;
	PackedCol col;
	Vector3 minBB, maxBB;	

	Mem_Copy(tmp, cw_meta_def, sizeof(cw_meta_def));
	{
		/* Hacky unique tag name for each by using hex of block */
		name = String_Init((char*)&tmp[8], 0, 4);
		String_AppendHex(&name, b >> 8);
		String_AppendHex(&name, b);

		tmp[17] = b;
		Stream_SetU16_BE(&tmp[24], b);

		tmp[40] = Blocks.Collide[b];
		speed.f = Blocks.SpeedMultiplier[b];
		Stream_SetU32_BE(&tmp[49], speed.u);

		tex = Block_Tex(b, FACE_YMAX); tmp[68] = tex; tmp[74] = tex >> 8;
		tex = Block_Tex(b, FACE_YMIN); tmp[69] = tex; tmp[75] = tex >> 8;
		tex = Block_Tex(b, FACE_XMIN); tmp[70] = tex; tmp[76] = tex >> 8;
		tex = Block_Tex(b, FACE_XMAX); tmp[71] = tex; tmp[77] = tex >> 8;
		tex = Block_Tex(b, FACE_ZMIN); tmp[72] = tex; tmp[78] = tex >> 8;
		tex = Block_Tex(b, FACE_ZMAX); tmp[73] = tex; tmp[79] = tex >> 8;

		tmp[97]  = Blocks.BlocksLight[b] ? 0 : 1;
		tmp[110] = Blocks.DigSounds[b];
		tmp[124] = Blocks.FullBright[b] ? 1 : 0;
		tmp[133] = sprite ? 0 : (uint8_t)(Blocks.MaxBB[b].Y * 16);
		tmp[146] = sprite ? Blocks.SpriteOffset[b] : Blocks.Draw[b];

		fog = (uint8_t)(128 * Blocks.FogDensity[b] - 1);
		col = Blocks.FogCol[b];
		tmp[157] = Blocks.FogDensity[b] ? fog : 0;
		tmp[158] = col.R; tmp[159] = col.G; tmp[160] = col.B;

		minBB = Blocks.MinBB[b]; maxBB = Blocks.MaxBB[b];
		tmp[174] = (uint8_t)(minBB.X * 16); tmp[175] = (uint8_t)(minBB.Y * 16); tmp[176] = (uint8_t)(minBB.Z * 16);
		tmp[177] = (uint8_t)(maxBB.X * 16); tmp[178] = (uint8_t)(maxBB.Y * 16); tmp[179] = (uint8_t)(maxBB.Z * 16);
	}

	name = Block_UNSAFE_GetName(b);
	len  = Cw_WriteEndString(&tmp[187], &name);
	return Stream_Write(stream, tmp, sizeof(cw_meta_def) + len);
}

ReturnCode Cw_Save(struct Stream* stream) {
	uint8_t tmp[768];
	PackedCol col;
	struct LocalPlayer* p = &LocalPlayer_Instance;
	ReturnCode res;
	int b, len;

	Mem_Copy(tmp, cw_begin, sizeof(cw_begin));
	{
		Mem_Copy(&tmp[43], World.Uuid, sizeof(World.Uuid));
		Stream_SetU16_BE(&tmp[63], World.Width);
		Stream_SetU16_BE(&tmp[69], World.Height);
		Stream_SetU16_BE(&tmp[75], World.Length);
		Stream_SetU32_BE(&tmp[127], World.Volume);
		
		/* TODO: Maybe keep real spawn too? */
		Stream_SetU16_BE(&tmp[89],  (uint16_t)p->Base.Position.X);
		Stream_SetU16_BE(&tmp[95],  (uint16_t)p->Base.Position.Y);
		Stream_SetU16_BE(&tmp[101], (uint16_t)p->Base.Position.Z);
		tmp[107] = Math_Deg2Packed(p->SpawnRotY);
		tmp[112] = Math_Deg2Packed(p->SpawnHeadX);
	}
	if ((res = Stream_Write(stream, tmp,      sizeof(cw_begin)))) return res;
	if ((res = Stream_Write(stream, World.Blocks, World.Volume))) return res;

	if (World.Blocks != World.Blocks2) {
		Mem_Copy(tmp, cw_map2, sizeof(cw_map2));
		Stream_SetU32_BE(&tmp[14], World.Volume);

		if ((res = Stream_Write(stream, tmp,        sizeof(cw_map2)))) return res;
		if ((res = Stream_Write(stream, World.Blocks2, World.Volume))) return res;
	}

	Mem_Copy(tmp, cw_meta_cpe, sizeof(cw_meta_cpe));
	{
		Stream_SetU16_BE(&tmp[44], (uint16_t)(LocalPlayer_Instance.ReachDistance * 32));
		tmp[78] = Env.Weather;

		col = Env.SkyCol;    tmp[103] = col.R; tmp[109] = col.G; tmp[115] = col.B;
		col = Env.CloudsCol; tmp[130] = col.R; tmp[136] = col.G; tmp[142] = col.B;
		col = Env.FogCol;    tmp[155] = col.R; tmp[161] = col.G; tmp[167] = col.B;
		col = Env.ShadowCol; tmp[184] = col.R; tmp[190] = col.G; tmp[196] = col.B;
		col = Env.SunCol;    tmp[214] = col.R; tmp[220] = col.G; tmp[226] = col.B;

		tmp[260] = (BlockRaw)Env.SidesBlock;
		tmp[273] = (BlockRaw)Env.EdgeBlock;
		Stream_SetU16_BE(&tmp[286], Env.EdgeHeight);
	}
	len = Cw_WriteEndString(&tmp[301], &World_TextureUrl);
	if ((res = Stream_Write(stream, tmp, sizeof(cw_meta_cpe) + len))) return res;

	if ((res = Stream_Write(stream, cw_meta_defs, sizeof(cw_meta_defs)))) return res;
	/* Write block definitions in reverse order so that software that only reads byte 'ID' */
	/* still loads correct first 256 block defs when saving a map with over 256 block defs */
	for (b = BLOCK_MAX_DEFINED; b >= 1; b--) {
		if (!Block_IsCustomDefined(b)) continue;
		if ((res = Cw_WriteBockDef(stream, b))) return res;
	}
	return Stream_Write(stream, cw_end, sizeof(cw_end));
}


/*########################################################################################################################*
*---------------------------------------------------Schematic export------------------------------------------------------*
*#########################################################################################################################*/

static uint8_t sc_begin[78] = {
NBT_DICT, 0,9, 'S','c','h','e','m','a','t','i','c',
	NBT_STR,  0,9,  'M','a','t','e','r','i','a','l','s', 0,7, 'C','l','a','s','s','i','c',
	NBT_I16,  0,5,  'W','i','d','t','h',                 0,0,
	NBT_I16,  0,6,  'H','e','i','g','h','t',             0,0,
	NBT_I16,  0,6,  'L','e','n','g','t','h',             0,0,
	NBT_I8S,  0,6,  'B','l','o','c','k','s',             0,0,0,0,
};
static uint8_t sc_data[11] = {
	NBT_I8S,  0,4,  'D','a','t','a',                     0,0,0,0,
};
static uint8_t sc_end[37] = {
	NBT_LIST, 0,8,  'E','n','t','i','t','i','e','s',                 NBT_DICT, 0,0,0,0,
	NBT_LIST, 0,12, 'T','i','l','e','E','n','t','i','t','i','e','s', NBT_DICT, 0,0,0,0,
NBT_END,
};

ReturnCode Schematic_Save(struct Stream* stream) {
	uint8_t tmp[256], chunk[8192] = { 0 };
	ReturnCode res;
	int i;

	Mem_Copy(tmp, sc_begin, sizeof(sc_begin));
	{
		Stream_SetU16_BE(&tmp[41], World.Width);
		Stream_SetU16_BE(&tmp[52], World.Height);
		Stream_SetU16_BE(&tmp[63], World.Length);
		Stream_SetU32_BE(&tmp[74], World.Volume);
	}
	if ((res = Stream_Write(stream, tmp, sizeof(sc_begin)))) return res;
	if ((res = Stream_Write(stream, World.Blocks, World.Volume))) return res;

	Mem_Copy(tmp, sc_data, sizeof(sc_data));
	{
		Stream_SetU32_BE(&tmp[7], World.Volume);
	}
	if ((res = Stream_Write(stream, tmp, sizeof(sc_data)))) return res;

	for (i = 0; i < World.Volume; i += sizeof(chunk)) {
		int count = World.Volume - i; count = min(count, sizeof(chunk));
		if ((res = Stream_Write(stream, chunk, count))) return res;
	}
	return Stream_Write(stream, sc_end, sizeof(sc_end));
}
