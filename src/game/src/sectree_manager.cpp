#include "stdafx.h"
#include <sstream>
#include "../../libgame/include/targa.h"
#include "../../libgame/include/attribute.h"
#include "config.h"
#include "utils.h"
#include "sectree_manager.h"
#include "regen.h"
#include "lzo_manager.h"
#include "desc.h"
#include "desc_manager.h"
#include "char.h"
#include "char_manager.h"
#include "item.h"
#include "item_manager.h"
#include "buffer_manager.h"
#include "packet.h"
#include "start_position.h"

WORD SECTREE_MANAGER::current_sectree_version = MAKEWORD(0, 3);

SECTREE_MAP::SECTREE_MAP()
{
	memset( &m_setting, 0, sizeof(m_setting) );
}

SECTREE_MAP::~SECTREE_MAP()
{
	MapType::iterator it = map_.begin();

	while (it != map_.end()) {
		LPSECTREE sectree = (it++)->second;
		M2_DELETE(sectree);
	}

	map_.clear();
}

SECTREE_MAP::SECTREE_MAP(SECTREE_MAP & r)
{
	m_setting = r.m_setting;

	MapType::iterator it = r.map_.begin();

	while (it != r.map_.end())
	{
		LPSECTREE tree = M2_NEW SECTREE;

		tree->m_id.coord = it->second->m_id.coord;
		tree->CloneAttribute(it->second);

		map_.insert(MapType::value_type(it->first, tree));
		++it;
	}

	Build();
}

LPSECTREE SECTREE_MAP::Find(DWORD dwPackage)
{
	MapType::iterator it = map_.find(dwPackage);

	if (it == map_.end())
		return NULL;

	return it->second;
}

LPSECTREE SECTREE_MAP::Find(DWORD x, DWORD y)
{
	SECTREEID id;
	id.coord.x = x / SECTREE_SIZE;
	id.coord.y = y / SECTREE_SIZE;
	return Find(id.package);
}

void SECTREE_MAP::Build()
{
    // Ŭ���̾�Ʈ���� �ݰ� 150m ĳ������ ������ �ֱ�����
    // 3x3ĭ -> 5x5 ĭ���� �ֺ�sectree Ȯ��(�ѱ�)
    if (LC_IsYMIR() || LC_IsKorea())
    {
#define NEIGHBOR_LENGTH		5
#define NUM_OF_NEIGHBORS	(NEIGHBOR_LENGTH * NEIGHBOR_LENGTH - 1)
	int	width = NEIGHBOR_LENGTH / 2;
	struct neighbor_coord_s
	{
		int x;
		int y;
	} neighbor_coord[NUM_OF_NEIGHBORS];

	{
	    int i = 0;
	    for (int x = -width; x <= width; ++x)
	    {
		for (int y = -width; y <= width; ++y)
		{
		    if (x == 0 && y == 0)
			continue;

		    neighbor_coord[i].x = x * SECTREE_SIZE;
		    neighbor_coord[i].y = y * SECTREE_SIZE;
		    ++i;
		}
	    }
	}

	//
	// ��� sectree�� ���� ���� sectree�� ����Ʈ�� �����.
	//
	MapType::iterator it = map_.begin();

	while (it != map_.end())
	{
		LPSECTREE tree = it->second;

		tree->m_neighbor_list.push_back(tree); // �ڽ��� �ִ´�.

		SPDLOG_TRACE("{}x{}", (int) tree->m_id.coord.x, (int) tree->m_id.coord.y);

		int x = tree->m_id.coord.x * SECTREE_SIZE;
		int y = tree->m_id.coord.y * SECTREE_SIZE;

		for (DWORD i = 0; i < NUM_OF_NEIGHBORS; ++i)
		{
			LPSECTREE tree2 = Find(x + neighbor_coord[i].x, y + neighbor_coord[i].y);

			if (tree2)
			{
				SPDLOG_TRACE("   {} {}x{}", i, (int) tree2->m_id.coord.x, (int) tree2->m_id.coord.y);
				tree->m_neighbor_list.push_back(tree2);
			}
		}

		++it;
	}
    }
    else
    {
	struct neighbor_coord_s
	{
		int x;
		int y;
	} neighbor_coord[8] = {
		{ -SECTREE_SIZE,	0		},
		{  SECTREE_SIZE,	0		},
		{ 0	       ,	-SECTREE_SIZE	},
		{ 0	       ,	 SECTREE_SIZE	},
		{ -SECTREE_SIZE,	 SECTREE_SIZE	},
		{  SECTREE_SIZE,	-SECTREE_SIZE	},
		{ -SECTREE_SIZE,	-SECTREE_SIZE	},
		{  SECTREE_SIZE,	 SECTREE_SIZE	},
	};

	//
	// ��� sectree�� ���� ���� sectree�� ����Ʈ�� �����.
	//
	MapType::iterator it = map_.begin();

	while (it != map_.end())
	{
		LPSECTREE tree = it->second;

		tree->m_neighbor_list.push_back(tree); // �ڽ��� �ִ´�.

		SPDLOG_TRACE("{}x{}", (int) tree->m_id.coord.x, (int) tree->m_id.coord.y);

		int x = tree->m_id.coord.x * SECTREE_SIZE;
		int y = tree->m_id.coord.y * SECTREE_SIZE;

		for (DWORD i = 0; i < 8; ++i)
		{
			LPSECTREE tree2 = Find(x + neighbor_coord[i].x, y + neighbor_coord[i].y);

			if (tree2)
			{
				SPDLOG_TRACE("   {} {}x{}", i, (int) tree2->m_id.coord.x, (int) tree2->m_id.coord.y);
				tree->m_neighbor_list.push_back(tree2);
			}
		}

		++it;
	}
    }
}

SECTREE_MANAGER::SECTREE_MANAGER()
{
}

SECTREE_MANAGER::~SECTREE_MANAGER()
{
	/*
	   std::map<DWORD, LPSECTREE_MAP>::iterator it = m_map_pkSectree.begin();

	   while (it != m_map_pkSectree.end())
	   {
	   M2_DELETE(it->second);
	   ++it;
	   }
	 */
}

LPSECTREE_MAP SECTREE_MANAGER::GetMap(int lMapIndex)
{
	std::map<DWORD, LPSECTREE_MAP>::iterator it = m_map_pkSectree.find(lMapIndex);

	if (it == m_map_pkSectree.end())
		return NULL;

	return it->second;
}

LPSECTREE SECTREE_MANAGER::Get(DWORD dwIndex, DWORD package)
{
	LPSECTREE_MAP pkSectreeMap = GetMap(dwIndex);

	if (!pkSectreeMap)
		return NULL;

	return pkSectreeMap->Find(package);
}

LPSECTREE SECTREE_MANAGER::Get(DWORD dwIndex, DWORD x, DWORD y)
{
	SECTREEID id;
	id.coord.x = x / SECTREE_SIZE;
	id.coord.y = y / SECTREE_SIZE;
	return Get(dwIndex, id.package);
}

// -----------------------------------------------------------------------------
// Setting.txt �� ���� SECTREE �����
// -----------------------------------------------------------------------------
int SECTREE_MANAGER::LoadSettingFile(int lMapIndex, const char * c_pszSettingFileName, TMapSetting & r_setting)
{
	memset(&r_setting, 0, sizeof(TMapSetting));

	FILE * fp = fopen(c_pszSettingFileName, "r");

	if (!fp)
	{
		SPDLOG_ERROR("cannot open file: {}", c_pszSettingFileName);
		return 0;
	}

	char buf[256], cmd[256];
	int iWidth = 0, iHeight = 0;

	while (fgets(buf, 256, fp))
	{
		sscanf(buf, " %s ", cmd);

		if (!strcasecmp(cmd, "MapSize"))
		{
			sscanf(buf, " %s %d %d ", cmd, &iWidth, &iHeight);
		}
		else if (!strcasecmp(cmd, "BasePosition"))
		{
			sscanf(buf, " %s %d %d", cmd, &r_setting.iBaseX, &r_setting.iBaseY);
		}
		else if (!strcasecmp(cmd, "CellScale"))
		{
			sscanf(buf, " %s %d ", cmd, &r_setting.iCellScale);
		}
	}

	fclose(fp);

	if ((iWidth == 0 && iHeight == 0) || r_setting.iCellScale == 0)
	{
		SPDLOG_ERROR("Invalid Settings file: {}", c_pszSettingFileName);
		return 0;
	}

	r_setting.iIndex = lMapIndex;
	r_setting.iWidth = (r_setting.iCellScale * 128 * iWidth);
	r_setting.iHeight = (r_setting.iCellScale * 128 * iHeight);
	return 1;
}

LPSECTREE_MAP SECTREE_MANAGER::BuildSectreeFromSetting(TMapSetting & r_setting)
{
	LPSECTREE_MAP pkMapSectree = M2_NEW SECTREE_MAP;

	pkMapSectree->m_setting = r_setting;

	int x, y;
	LPSECTREE tree;

	for (x = r_setting.iBaseX; x < r_setting.iBaseX + r_setting.iWidth; x += SECTREE_SIZE)
	{
		for (y = r_setting.iBaseY; y < r_setting.iBaseY + r_setting.iHeight; y += SECTREE_SIZE)
		{
			tree = M2_NEW SECTREE;
			tree->m_id.coord.x = x / SECTREE_SIZE;
			tree->m_id.coord.y = y / SECTREE_SIZE;
			pkMapSectree->Add(tree->m_id.package, tree);
			SPDLOG_TRACE("new sectree {} x {}", (int) tree->m_id.coord.x, (int) tree->m_id.coord.y);
		}
	}

	if ((r_setting.iBaseX + r_setting.iWidth) % SECTREE_SIZE)
	{
		tree = M2_NEW SECTREE;
		tree->m_id.coord.x = ((r_setting.iBaseX + r_setting.iWidth) / SECTREE_SIZE) + 1;
		tree->m_id.coord.y = ((r_setting.iBaseY + r_setting.iHeight) / SECTREE_SIZE);
		pkMapSectree->Add(tree->m_id.package, tree);
	}

	if ((r_setting.iBaseY + r_setting.iHeight) % SECTREE_SIZE)
	{
		tree = M2_NEW SECTREE;
		tree->m_id.coord.x = ((r_setting.iBaseX + r_setting.iWidth) / SECTREE_SIZE);
		tree->m_id.coord.y = ((r_setting.iBaseX + r_setting.iHeight) / SECTREE_SIZE) + 1;
		pkMapSectree->Add(tree->m_id.package, tree);
	}

	return pkMapSectree;
}

void SECTREE_MANAGER::LoadDungeon(int iIndex, const char * c_pszFileName)
{
	FILE* fp = fopen(c_pszFileName, "r");

	if (!fp)
		return;

	int count = 0; // for debug

	while (!feof(fp))
	{
		char buf[1024];

		if (NULL == fgets(buf, 1024, fp))
			break;

		if (buf[0] == '#' || buf[0] == '/' && buf[1] == '/')
			continue;

		std::istringstream ins(buf, std::ios_base::in);
		std::string position_name;
		int x, y, sx, sy, dir;

		ins >> position_name >> x >> y >> sx >> sy >> dir;

		if (ins.fail())
			continue;

		x -= sx;
		y -= sy;
		sx *= 2;
		sy *= 2;
		sx += x;
		sy += y;

		m_map_pkArea[iIndex].insert(std::make_pair(position_name, TAreaInfo(x, y, sx, sy, dir)));

		count++;
	}

	fclose(fp);

	SPDLOG_DEBUG("Dungeon Position Load [{:3}]{} count {}", iIndex, c_pszFileName, count);
}

// Fix me
// ���� Town.txt���� x, y�� �׳� �ް�, �װ� �� �ڵ� ������ base ��ǥ�� �����ֱ� ������
// �ٸ� �ʿ� �ִ� Ÿ������ ���� �̵��� �� ���� �Ǿ��ִ�.
// �տ� map�̶�ų�, ��Ÿ �ٸ� �ĺ��ڰ� ������,
// �ٸ� ���� Ÿ�����ε� �̵��� �� �ְ� ����.
// by rtsummit
bool SECTREE_MANAGER::LoadMapRegion(const char * c_pszFileName, TMapSetting & r_setting, const char * c_pszMapName)
{
	FILE * fp = fopen(c_pszFileName, "r");

    SPDLOG_TRACE("[LoadMapRegion] file({})", c_pszFileName );

	if (!fp)
		return false;

	int iX=0, iY=0;
	PIXEL_POSITION pos[3] = { {0,0,0}, {0,0,0}, {0,0,0} };

	fscanf(fp, " %d %d ", &iX, &iY);

	int iEmpirePositionCount = fscanf(fp, " %d %d %d %d %d %d ", 
			&pos[0].x, &pos[0].y,
			&pos[1].x, &pos[1].y,
			&pos[2].x, &pos[2].y);

	fclose(fp);

	if( iEmpirePositionCount == 6 )
	{
		for ( int n = 0; n < 3; ++n )
			SPDLOG_DEBUG("LoadMapRegion {} {} ", pos[n].x, pos[n].y );
	}
	else
	{
		SPDLOG_DEBUG("LoadMapRegion no empire specific start point" );
	}

	TMapRegion region;

	region.index = r_setting.iIndex;
	region.sx = r_setting.iBaseX;
	region.sy = r_setting.iBaseY;
	region.ex = r_setting.iBaseX + r_setting.iWidth;
	region.ey = r_setting.iBaseY + r_setting.iHeight;

	region.strMapName = c_pszMapName;

	region.posSpawn.x = r_setting.iBaseX + (iX * 100);
	region.posSpawn.y = r_setting.iBaseY + (iY * 100); 

	r_setting.posSpawn = region.posSpawn;

	SPDLOG_DEBUG("LoadMapRegion {} x {} ~ {} y {} ~ {}, town {} {}",
			region.index,
			region.sx,
			region.ex,
			region.sy,
			region.ey,
			region.posSpawn.x,
			region.posSpawn.y);

	if (iEmpirePositionCount == 6)
	{
		region.bEmpireSpawnDifferent = true;

		for (int i = 0; i < 3; i++)
		{
			region.posEmpire[i].x = r_setting.iBaseX + (pos[i].x * 100);
			region.posEmpire[i].y = r_setting.iBaseY + (pos[i].y * 100);
		}
	}
	else
	{
		region.bEmpireSpawnDifferent = false;
	}

	m_vec_mapRegion.push_back(region);

	SPDLOG_DEBUG("LoadMapRegion {} End", region.index);
	return true;
}

bool SECTREE_MANAGER::LoadAttribute(LPSECTREE_MAP pkMapSectree, const char * c_pszFileName, TMapSetting & r_setting)
{
	FILE * fp = fopen(c_pszFileName, "rb");

	if (!fp)
	{
		SPDLOG_ERROR("SECTREE_MANAGER::LoadAttribute : cannot open {}", c_pszFileName);
		return false;
	}

	int32_t iWidth, iHeight;

	fread(&iWidth, sizeof(int32_t), 1, fp);
	fread(&iHeight, sizeof(int32_t), 1, fp);

	size_t maxMemSize = LZOManager::instance().GetMaxCompressedSize(sizeof(DWORD) * (SECTREE_SIZE / CELL_SIZE) * (SECTREE_SIZE / CELL_SIZE));

    int32_t uiSize;
	lzo_uint uiDestSize;

	auto * abComp = new BYTE[maxMemSize];
	auto * attr = new DWORD[maxMemSize];

	for (int y = 0; y < iHeight; ++y)
		for (int x = 0; x < iWidth; ++x)
		{
			// UNION ���� ��ǥ�� ���ĸ��� DWORD���� ���̵�� ����Ѵ�.
			SECTREEID id;
			id.coord.x = (r_setting.iBaseX / SECTREE_SIZE) + x;
			id.coord.y = (r_setting.iBaseY / SECTREE_SIZE) + y;

			LPSECTREE tree = pkMapSectree->Find(id.package);

			// SERVER_ATTR_LOAD_ERROR
			if (tree == nullptr)
			{
				SPDLOG_ERROR("FATAL ERROR! LoadAttribute({}) - cannot find sectree(package={}, coord=({}, {}), map_index={}, map_base=({}, {}))",
						c_pszFileName, id.package, (int) id.coord.x, (int) id.coord.y, r_setting.iIndex, r_setting.iBaseX, r_setting.iBaseY);
				SPDLOG_ERROR("ERROR_ATTR_POS({}, {}) attr_size({}, {})", x, y, iWidth, iHeight);
				SPDLOG_ERROR("CHECK! 'Setting.txt' and 'server_attr' MAP_SIZE!!");

				pkMapSectree->DumpAllToSysErr();
				abort();

				M2_DELETE_ARRAY(attr);
				M2_DELETE_ARRAY(abComp);

				return false;
			}
			// END_OF_SERVER_ATTR_LOAD_ERROR

			if (tree->m_id.package != id.package)
			{
				SPDLOG_ERROR("returned tree id mismatch! return {}, request {}",
						tree->m_id.package, id.package);
				fclose(fp);

				M2_DELETE_ARRAY(attr);
				M2_DELETE_ARRAY(abComp);

				return false;
			}

			fread(&uiSize, sizeof(int32_t), 1, fp);
			fread(abComp, sizeof(uint8_t), uiSize, fp);

			//LZOManager::instance().Decompress(abComp, uiSize, (BYTE *) tree->GetAttributePointer(), &uiDestSize);
			uiDestSize = sizeof(DWORD) * maxMemSize;
			LZOManager::instance().Decompress(abComp, uiSize, (BYTE *) attr, &uiDestSize);

			if (uiDestSize != sizeof(DWORD) * (SECTREE_SIZE / CELL_SIZE) * (SECTREE_SIZE / CELL_SIZE))
			{
				SPDLOG_ERROR("SECTREE_MANAGER::LoadAttribute : {} : {} {} size mismatch! {}",
						c_pszFileName, (int) tree->m_id.coord.x, (int) tree->m_id.coord.y, uiDestSize);
				fclose(fp);

				M2_DELETE_ARRAY(attr);
				M2_DELETE_ARRAY(abComp);

				return false;
			}

			tree->BindAttribute(M2_NEW CAttribute(attr, SECTREE_SIZE / CELL_SIZE, SECTREE_SIZE / CELL_SIZE));
		}

	fclose(fp);

	M2_DELETE_ARRAY(attr);
	M2_DELETE_ARRAY(abComp);

	return true;
}

bool SECTREE_MANAGER::GetRecallPositionByEmpire(int iMapIndex, BYTE bEmpire, PIXEL_POSITION & r_pos)
{
	std::vector<TMapRegion>::iterator it = m_vec_mapRegion.begin();

	// 10000�� �Ѵ� ���� �ν��Ͻ� �������� �����Ǿ��ִ�.
	if (iMapIndex >= 10000)
	{
		iMapIndex /= 10000;
	}

	while (it != m_vec_mapRegion.end())
	{
		TMapRegion & rRegion = *(it++);

		if (rRegion.index == iMapIndex)
		{
			if (rRegion.bEmpireSpawnDifferent && bEmpire >= 1 && bEmpire <= 3)
				r_pos = rRegion.posEmpire[bEmpire - 1];
			else
				r_pos = rRegion.posSpawn;

			return true;
		}
	}

	return false;
}

bool SECTREE_MANAGER::GetCenterPositionOfMap(int lMapIndex, PIXEL_POSITION & r_pos)
{
	std::vector<TMapRegion>::iterator it = m_vec_mapRegion.begin();

	while (it != m_vec_mapRegion.end())
	{
		TMapRegion & rRegion = *(it++);

		if (rRegion.index == lMapIndex)
		{
			r_pos.x = rRegion.sx + (rRegion.ex - rRegion.sx) / 2;
			r_pos.y = rRegion.sy + (rRegion.ey - rRegion.sy) / 2;
			r_pos.z = 0;
			return true;
		}
	}

	return false;
}

bool SECTREE_MANAGER::GetSpawnPositionByMapIndex(int lMapIndex, PIXEL_POSITION& r_pos)
{
	if (lMapIndex> 10000) lMapIndex /= 10000;
	std::vector<TMapRegion>::iterator it = m_vec_mapRegion.begin();

	while (it != m_vec_mapRegion.end())
	{
		TMapRegion & rRegion = *(it++);

		if (lMapIndex == rRegion.index)
		{
			r_pos = rRegion.posSpawn;
			return true;
		}
	}

	return false;
}

bool SECTREE_MANAGER::GetSpawnPosition(int x, int y, PIXEL_POSITION & r_pos)
{
	std::vector<TMapRegion>::iterator it = m_vec_mapRegion.begin();

	while (it != m_vec_mapRegion.end())
	{
		TMapRegion & rRegion = *(it++);

		if (x >= rRegion.sx && y >= rRegion.sy && x < rRegion.ex && y < rRegion.ey)
		{
			r_pos = rRegion.posSpawn;
			return true;
		}
	}

	return false;
}

bool SECTREE_MANAGER::GetMapBasePositionByMapIndex(int lMapIndex, PIXEL_POSITION & r_pos)
{
	if (lMapIndex> 10000) lMapIndex /= 10000;
	std::vector<TMapRegion>::iterator it = m_vec_mapRegion.begin();

	while (it != m_vec_mapRegion.end())
	{
		TMapRegion & rRegion = *(it++);

		//if (x >= rRegion.sx && y >= rRegion.sy && x < rRegion.ex && y < rRegion.ey)
		if (lMapIndex == rRegion.index)
		{
			r_pos.x = rRegion.sx;
			r_pos.y = rRegion.sy;
			r_pos.z = 0;
			return true;
		}
	}

	return false;
}

bool SECTREE_MANAGER::GetMapBasePosition(int x, int y, PIXEL_POSITION & r_pos)
{
	std::vector<TMapRegion>::iterator it = m_vec_mapRegion.begin();

	while (it != m_vec_mapRegion.end())
	{
		TMapRegion & rRegion = *(it++);

		if (x >= rRegion.sx && y >= rRegion.sy && x < rRegion.ex && y < rRegion.ey)
		{
			r_pos.x = rRegion.sx;
			r_pos.y = rRegion.sy;
			r_pos.z = 0;
			return true;
		}
	}

	return false;
}

const TMapRegion * SECTREE_MANAGER::FindRegionByPartialName(const char* szMapName)
{
	std::vector<TMapRegion>::iterator it = m_vec_mapRegion.begin();

	while (it != m_vec_mapRegion.end())
	{
		TMapRegion & rRegion = *(it++);

		//if (rRegion.index == lMapIndex)
		//return &rRegion;
		if (rRegion.strMapName.find(szMapName))
			return &rRegion; // ĳ�� �ؼ� ������ ����
	}

	return NULL;
}

const TMapRegion * SECTREE_MANAGER::GetMapRegion(int lMapIndex)
{
	std::vector<TMapRegion>::iterator it = m_vec_mapRegion.begin();

	while (it != m_vec_mapRegion.end())
	{
		TMapRegion & rRegion = *(it++);

		if (rRegion.index == lMapIndex)
			return &rRegion;
	}

	return NULL;
}

int SECTREE_MANAGER::GetMapIndex(int x, int y)
{
	std::vector<TMapRegion>::iterator it = m_vec_mapRegion.begin();

	while (it != m_vec_mapRegion.end())
	{
		TMapRegion & rRegion = *(it++);

		if (x >= rRegion.sx && y >= rRegion.sy && x < rRegion.ex && y < rRegion.ey)
			return rRegion.index;
	}

	SPDLOG_DEBUG("SECTREE_MANAGER::GetMapIndex({}, {})", x, y);

	std::vector<TMapRegion>::iterator i;
	for (i = m_vec_mapRegion.begin(); i !=m_vec_mapRegion.end(); ++i)
	{
		TMapRegion & rRegion = *i;
		SPDLOG_DEBUG("{}: ({}, {}) ~ ({}, {})", rRegion.index, rRegion.sx, rRegion.sy, rRegion.ex, rRegion.ey);
	}

	return 0;
}

int SECTREE_MANAGER::Build(const char * c_pszListFileName, const char* c_pszMapBasePath)
{
    SPDLOG_TRACE("[BUILD] Build {} {} ", c_pszListFileName, c_pszMapBasePath );

	FILE* fp = fopen(c_pszListFileName, "r");

	if (NULL == fp)
		return 0;

	char buf[256 + 1];
	char szFilename[1024];
	char szMapName[256];
	int iIndex;

	while (fgets(buf, 256, fp))
	{
		*strrchr(buf, '\n') = '\0';

		if (!strncmp(buf, "//", 2) || *buf == '#')
			continue;

		sscanf(buf, " %d %s ", &iIndex, szMapName);

		snprintf(szFilename, sizeof(szFilename), "%s/%s/Setting.txt", c_pszMapBasePath, szMapName);

		TMapSetting setting;
		setting.iIndex = iIndex;

		if (!LoadSettingFile(iIndex, szFilename, setting))
		{
			SPDLOG_ERROR("can't load file {} in LoadSettingFile", szFilename);
			fclose(fp);
			return 0;
		}

		snprintf(szFilename, sizeof(szFilename), "%s/%s/Town.txt", c_pszMapBasePath, szMapName);

		if (!LoadMapRegion(szFilename, setting, szMapName))
		{
			SPDLOG_ERROR("can't load file {} in LoadMapRegion", szFilename);
			fclose(fp);
			return 0;
		}

        SPDLOG_TRACE("[BUILD] Build {} {} {} ",c_pszMapBasePath, szMapName, iIndex );

		// ���� �� �������� �� ���� ���͸� �����ؾ� �ϴ°� Ȯ�� �Ѵ�.
		if (map_allow_find(iIndex))
		{
			LPSECTREE_MAP pkMapSectree = BuildSectreeFromSetting(setting);
			m_map_pkSectree.insert(std::map<DWORD, LPSECTREE_MAP>::value_type(iIndex, pkMapSectree));

			snprintf(szFilename, sizeof(szFilename), "%s/%s/server_attr", c_pszMapBasePath, szMapName);
			LoadAttribute(pkMapSectree, szFilename, setting);

			snprintf(szFilename, sizeof(szFilename), "%s/%s/regen.txt", c_pszMapBasePath, szMapName);
			regen_load(szFilename, setting.iIndex, setting.iBaseX, setting.iBaseY);

			snprintf(szFilename, sizeof(szFilename), "%s/%s/npc.txt", c_pszMapBasePath, szMapName);
			regen_load(szFilename, setting.iIndex, setting.iBaseX, setting.iBaseY);

			snprintf(szFilename, sizeof(szFilename), "%s/%s/boss.txt", c_pszMapBasePath, szMapName);
			regen_load(szFilename, setting.iIndex, setting.iBaseX, setting.iBaseY);

			snprintf(szFilename, sizeof(szFilename), "%s/%s/stone.txt", c_pszMapBasePath, szMapName);
			regen_load(szFilename, setting.iIndex, setting.iBaseX, setting.iBaseY);

			snprintf(szFilename, sizeof(szFilename), "%s/%s/dungeon.txt", c_pszMapBasePath, szMapName);
			LoadDungeon(iIndex, szFilename);

			pkMapSectree->Build();
		}
	}

	fclose(fp);

	return 1;
}

bool SECTREE_MANAGER::IsMovablePosition(int lMapIndex, int x, int y)
{
	LPSECTREE tree = SECTREE_MANAGER::instance().Get(lMapIndex, x, y);

	if (!tree)
		return false;

	return (!tree->IsAttr(x, y, ATTR_BLOCK | ATTR_OBJECT));
}

bool SECTREE_MANAGER::GetMovablePosition(int lMapIndex, int x, int y, PIXEL_POSITION & pos)
{
	int i = 0;

	do
	{
		int dx = x + aArroundCoords[i].x;
		int dy = y + aArroundCoords[i].y;

		LPSECTREE tree = SECTREE_MANAGER::instance().Get(lMapIndex, dx, dy);

		if (!tree)
			continue;

		if (!tree->IsAttr(dx, dy, ATTR_BLOCK | ATTR_OBJECT))
		{
			pos.x = dx;
			pos.y = dy;
			return true;
		}
	} while (++i < ARROUND_COORD_MAX_NUM);

	pos.x = x;
	pos.y = y;
	return false;
}

bool SECTREE_MANAGER::GetValidLocation(int lMapIndex, int x, int y, int & r_lValidMapIndex, PIXEL_POSITION & r_pos, BYTE empire)
{
	LPSECTREE_MAP pkSectreeMap = GetMap(lMapIndex);

	if (!pkSectreeMap)
	{
		if (lMapIndex >= 10000)
		{
/*			int m = lMapIndex / 10000;
			if (m == 216)
			{
				if (GetRecallPositionByEmpire (m, empire, r_pos))
				{
					r_lValidMapIndex = m;
					return true;
				}
				else 
					return false;
			}*/
			return GetValidLocation(lMapIndex / 10000, x, y, r_lValidMapIndex, r_pos);
		}
		else
		{
			SPDLOG_ERROR("cannot find sectree_map by map index {}", lMapIndex);
			return false;
		}
	}

	int lRealMapIndex = lMapIndex;

	if (lRealMapIndex >= 10000)
		lRealMapIndex = lRealMapIndex / 10000;

	std::vector<TMapRegion>::iterator it = m_vec_mapRegion.begin();

	while (it != m_vec_mapRegion.end())
	{
		TMapRegion & rRegion = *(it++);

		if (rRegion.index == lRealMapIndex)
		{
			LPSECTREE tree = pkSectreeMap->Find(x, y);

			if (!tree)
			{
				SPDLOG_ERROR("cannot find tree by {} {} (map index {})", x, y, lMapIndex);
				return false;
			}

			r_lValidMapIndex = lMapIndex;
			r_pos.x = x;
			r_pos.y = y;
			return true;
		}
	}

	SPDLOG_ERROR("invalid location (map index {} {} x {})", lRealMapIndex, x, y);
	return false;
}

bool SECTREE_MANAGER::GetRandomLocation(int lMapIndex, PIXEL_POSITION & r_pos, DWORD dwCurrentX, DWORD dwCurrentY, int iMaxDistance)
{
	LPSECTREE_MAP pkSectreeMap = GetMap(lMapIndex);

	if (!pkSectreeMap)
		return false;

	DWORD x, y;

	std::vector<TMapRegion>::iterator it = m_vec_mapRegion.begin();

	while (it != m_vec_mapRegion.end())
	{
		TMapRegion & rRegion = *(it++);

		if (rRegion.index != lMapIndex)
			continue;

		int i = 0;

		while (i++ < 100)
		{
			x = Random::get(rRegion.sx + 50, rRegion.ex - 50);
			y = Random::get(rRegion.sy + 50, rRegion.ey - 50);

			if (iMaxDistance != 0)
			{
				int d;

				d = abs((float)dwCurrentX - x);

				if (d > iMaxDistance)
				{
					if (x < dwCurrentX)
						x = dwCurrentX - iMaxDistance;
					else
						x = dwCurrentX + iMaxDistance;
				}

				d = abs((float)dwCurrentY - y);

				if (d > iMaxDistance)
				{
					if (y < dwCurrentY)
						y = dwCurrentY - iMaxDistance;
					else
						y = dwCurrentY + iMaxDistance;
				}
			}

			LPSECTREE tree = pkSectreeMap->Find(x, y);

			if (!tree)
				continue;

			if (tree->IsAttr(x, y, ATTR_BLOCK | ATTR_OBJECT))
				continue;

			r_pos.x = x;
			r_pos.y = y;
			return true;
		}
	}

	return false;
}

int SECTREE_MANAGER::CreatePrivateMap(int lMapIndex)
{
	if (lMapIndex >= 10000) // 10000�� �̻��� ���� ����. (Ȥ�� �̹� private �̴�)
		return 0;

	LPSECTREE_MAP pkMapSectree = GetMap(lMapIndex);

	if (!pkMapSectree)
	{
		SPDLOG_ERROR("Cannot find map index {}", lMapIndex);
		return 0;
	}

	// <Factor> Circular private map indexing
	int base = lMapIndex * 10000;
	int index_cap = 10000;
	if ( lMapIndex == 107 || lMapIndex == 108 || lMapIndex == 109 ) {
		index_cap = (test_server ? 1 : 51);
	}
	PrivateIndexMapType::iterator it = next_private_index_map_.find(lMapIndex);
	if (it == next_private_index_map_.end()) {
		it = next_private_index_map_.insert(PrivateIndexMapType::value_type(lMapIndex, 0)).first;
	}
	int i, next_index = it->second;
	for (i = 0; i < index_cap; ++i) {
		if (GetMap(base + next_index) == NULL) {
			break; // available
		}
		if (++next_index >= index_cap) {
			next_index = 0;
		}
	}
	if (i == index_cap) {
		// No available index
		return 0;
	}
	int lNewMapIndex = base + next_index;
	if (++next_index >= index_cap) {
		next_index = 0;
	}
	it->second = next_index;

	/*
	int i;

	for (i = 0; i < 10000; ++i)
	{
		if (!GetMap((lMapIndex * 10000) + i))
			break;
	}

	SPDLOG_TRACE("Create Dungeon : OrginalMapindex {} NewMapindex {}", lMapIndex, i );
	
	if ( lMapIndex == 107 || lMapIndex == 108 || lMapIndex == 109 )
	{
		if ( test_server )
		{
			if ( i > 0 )
				return NULL;
		}
		else
		{
			if ( i > 50 )
				return NULL;
			
		}
	}

	if (i == 10000)
	{
		SPDLOG_ERROR("not enough private map index (map_index {})", lMapIndex);
		return 0;
	}

	int lNewMapIndex = lMapIndex * 10000 + i;
	*/

	pkMapSectree = M2_NEW SECTREE_MAP(*pkMapSectree);
	m_map_pkSectree.insert(std::map<DWORD, LPSECTREE_MAP>::value_type(lNewMapIndex, pkMapSectree));

	SPDLOG_DEBUG("PRIVATE_MAP: {} created (original {})", lNewMapIndex, lMapIndex);
	return lNewMapIndex;
}

struct FDestroyPrivateMapEntity
{
	void operator() (LPENTITY ent)
	{
		if (ent->IsType(ENTITY_CHARACTER))
		{
			LPCHARACTER ch = (LPCHARACTER) ent;
			SPDLOG_DEBUG("PRIVAE_MAP: removing character {}", ch->GetName());

			if (ch->GetDesc())
				DESC_MANAGER::instance().DestroyDesc(ch->GetDesc());
			else
				M2_DESTROY_CHARACTER(ch);
		}
		else if (ent->IsType(ENTITY_ITEM))
		{
			LPITEM item = (LPITEM) ent;
			SPDLOG_DEBUG("PRIVATE_MAP: removing item {}", item->GetName());

			M2_DESTROY_ITEM(item);
		}
		else
			SPDLOG_ERROR("PRIVAE_MAP: trying to remove unknown entity {}", ent->GetType());
	}
};

void SECTREE_MANAGER::DestroyPrivateMap(int lMapIndex)
{
	if (lMapIndex < 10000) // private map �� �ε����� 10000 �̻� �̴�.
		return;

	LPSECTREE_MAP pkMapSectree = GetMap(lMapIndex);

	if (!pkMapSectree)
		return;

	// �� �� ���� ���� �����ϴ� �͵��� ���� ���ش�.
	// WARNING:
	// �� �ʿ� ������ � Sectree���� �������� ���� �� ����
	// ���� ���⼭ delete �� �� �����Ƿ� �����Ͱ� ���� �� ������
	// ���� ó���� �ؾ���
	FDestroyPrivateMapEntity f;
	pkMapSectree->for_each(f);

	m_map_pkSectree.erase(lMapIndex);
	M2_DELETE(pkMapSectree);

	SPDLOG_DEBUG("PRIVATE_MAP: {} destroyed", lMapIndex);
}

TAreaMap& SECTREE_MANAGER::GetDungeonArea(int lMapIndex)
{
	itertype(m_map_pkArea) it = m_map_pkArea.find(lMapIndex);

	if (it == m_map_pkArea.end())
	{
		return m_map_pkArea[-1]; // �ӽ÷� �� Area�� ����
	}
	return it->second;
}

void SECTREE_MANAGER::SendNPCPosition(LPCHARACTER ch)
{
	LPDESC d = ch->GetDesc();
	if (!d)
		return;

	int lMapIndex = ch->GetMapIndex();

	if (m_mapNPCPosition[lMapIndex].empty())
		return;

	TEMP_BUFFER buf;
	TPacketGCNPCPosition p;
	p.header = HEADER_GC_NPC_POSITION;
	p.count = m_mapNPCPosition[lMapIndex].size();

	TNPCPosition np;

	// TODO m_mapNPCPosition[lMapIndex] �� �����ּ���
	itertype(m_mapNPCPosition[lMapIndex]) it;

	for (it = m_mapNPCPosition[lMapIndex].begin(); it != m_mapNPCPosition[lMapIndex].end(); ++it)
	{
		np.bType = it->bType;
		strlcpy(np.name, it->name, sizeof(np.name));
		np.x = it->x;
		np.y = it->y;
		buf.write(&np, sizeof(np));
	}

	p.size = sizeof(p) + buf.size();

	if (buf.size())
	{
        d->RawPacket(&p, sizeof(TPacketGCNPCPosition));
		d->Packet(buf.read_peek(), buf.size());
	}
	else
		d->Packet(&p, sizeof(TPacketGCNPCPosition));
}

void SECTREE_MANAGER::InsertNPCPosition(int lMapIndex, BYTE bType, const char* szName, int x, int y)
{
	m_mapNPCPosition[lMapIndex].push_back(npc_info(bType, szName, x, y));
}

BYTE SECTREE_MANAGER::GetEmpireFromMapIndex(int lMapIndex)
{
	if (lMapIndex >= 1 && lMapIndex <= 20)
		return 1;

	if (lMapIndex >= 21 && lMapIndex <= 40)
		return 2;

	if (lMapIndex >= 41 && lMapIndex <= 60)
		return 3;

	if ( lMapIndex == 184 || lMapIndex == 185 )
		return 1;
	
	if ( lMapIndex == 186 || lMapIndex == 187 )
		return 2;
	
	if ( lMapIndex == 188 || lMapIndex == 189 )
		return 3;

	switch ( lMapIndex )
	{
		case 190 :
			return 1;
		case 191 :
			return 2;
		case 192 :
			return 3;
	}
	
	return 0;
}

class FRemoveIfAttr
{
	public:
		FRemoveIfAttr(LPSECTREE pkTree, DWORD dwAttr) : m_pkTree(pkTree), m_dwCheckAttr(dwAttr)
		{
		}

		void operator () (LPENTITY entity)
		{
			if (!m_pkTree->IsAttr(entity->GetX(), entity->GetY(), m_dwCheckAttr))
				return;

			if (entity->IsType(ENTITY_ITEM))
			{
				M2_DESTROY_ITEM((LPITEM) entity);
			}
			else if (entity->IsType(ENTITY_CHARACTER))
			{
				LPCHARACTER ch = (LPCHARACTER) entity;

				if (ch->IsPC())
				{
					PIXEL_POSITION pos;

					if (SECTREE_MANAGER::instance().GetRecallPositionByEmpire(ch->GetMapIndex(), ch->GetEmpire(), pos))
						ch->WarpSet(pos.x, pos.y);
					else
						ch->WarpSet(EMPIRE_START_X(ch->GetEmpire()), EMPIRE_START_Y(ch->GetEmpire()));
				}
				else
					ch->Dead();
			}
		}

		LPSECTREE m_pkTree;
		DWORD m_dwCheckAttr;
};

bool SECTREE_MANAGER::ForAttrRegionCell( int lMapIndex, int lCX, int lCY, DWORD dwAttr, EAttrRegionMode mode )
{
	SECTREEID id;

	id.coord.x = lCX / (SECTREE_SIZE / CELL_SIZE);
	id.coord.y = lCY / (SECTREE_SIZE / CELL_SIZE);

	int lTreeCX = id.coord.x * (SECTREE_SIZE / CELL_SIZE);
	int lTreeCY = id.coord.y * (SECTREE_SIZE / CELL_SIZE);

	LPSECTREE pSec = Get( lMapIndex, id.package );
	if ( !pSec )
		return false;

	switch (mode)
	{
		case ATTR_REGION_MODE_SET:
			pSec->SetAttribute( lCX - lTreeCX, lCY - lTreeCY, dwAttr );
			break;

		case ATTR_REGION_MODE_REMOVE:
			pSec->RemoveAttribute( lCX - lTreeCX, lCY - lTreeCY, dwAttr );
			break;

		case ATTR_REGION_MODE_CHECK:
			if ( pSec->IsAttr( lCX * CELL_SIZE, lCY * CELL_SIZE, ATTR_OBJECT ) )
				return true;
			break;

		default:
			SPDLOG_ERROR("Unknown region mode {}", (int) mode);
			break;
	}

	return false;
}

bool SECTREE_MANAGER::ForAttrRegionRightAngle( int lMapIndex, int lCX, int lCY, int lCW, int lCH, int lRotate, DWORD dwAttr, EAttrRegionMode mode )
{
	if (1 == lRotate/90 || 3 == lRotate/90)
	{
		for (int x = 0; x < lCH; ++x)
			for (int y = 0; y < lCW; ++y)
			{
				if ( ForAttrRegionCell( lMapIndex, lCX + x, lCY + y, dwAttr, mode ) )
					return true;
			}
	}
	if (0 == lRotate/90 || 2 == lRotate/90)
	{
		for (int x = 0; x < lCW; ++x)
			for (int y = 0; y < lCH; ++y)
			{
				if ( ForAttrRegionCell( lMapIndex, lCX + x, lCY + y, dwAttr, mode) )
					return true;
			}
	}

	return mode == ATTR_REGION_MODE_CHECK ? false : true;
}

bool SECTREE_MANAGER::ForAttrRegionFreeAngle( int lMapIndex, int lCX, int lCY, int lCW, int lCH, int lRotate, DWORD dwAttr, EAttrRegionMode mode )
{
	float fx1 = (-lCW/2) * sinf(float(lRotate)/180.0f*3.14f) + (-lCH/2) * cosf(float(lRotate)/180.0f*3.14f);
	float fy1 = (-lCW/2) * cosf(float(lRotate)/180.0f*3.14f) - (-lCH/2) * sinf(float(lRotate)/180.0f*3.14f);

	float fx2 = (+lCW/2) * sinf(float(lRotate)/180.0f*3.14f) + (-lCH/2) * cosf(float(lRotate)/180.0f*3.14f);
	float fy2 = (+lCW/2) * cosf(float(lRotate)/180.0f*3.14f) - (-lCH/2) * sinf(float(lRotate)/180.0f*3.14f);

	float fx3 = (-lCW/2) * sinf(float(lRotate)/180.0f*3.14f) + (+lCH/2) * cosf(float(lRotate)/180.0f*3.14f);
	float fy3 = (-lCW/2) * cosf(float(lRotate)/180.0f*3.14f) - (+lCH/2) * sinf(float(lRotate)/180.0f*3.14f);

	float fx4 = (+lCW/2) * sinf(float(lRotate)/180.0f*3.14f) + (+lCH/2) * cosf(float(lRotate)/180.0f*3.14f);
	float fy4 = (+lCW/2) * cosf(float(lRotate)/180.0f*3.14f) - (+lCH/2) * sinf(float(lRotate)/180.0f*3.14f);

	float fdx1 = fx2 - fx1;
	float fdy1 = fy2 - fy1;
	float fdx2 = fx1 - fx3;
	float fdy2 = fy1 - fy3;

	if (0 == fdx1 || 0 == fdx2)
	{
		SPDLOG_ERROR("SECTREE_MANAGER::ForAttrRegion - Unhandled exception. MapIndex: {}", lMapIndex );
		return false;
	}

	float fTilt1 = float(fdy1) / float(fdx1);
	float fTilt2 = float(fdy2) / float(fdx2);
	float fb1 = fy1 - fTilt1*fx1;
	float fb2 = fy1 - fTilt2*fx1;
	float fb3 = fy4 - fTilt1*fx4;
	float fb4 = fy4 - fTilt2*fx4;

	float fxMin = std::min(fx1, std::min(fx2, std::min(fx3, fx4)));
	float fxMax = std::max(fx1, std::max(fx2, std::max(fx3, fx4)));
	for (int i = int(fxMin); i < int(fxMax); ++i)
	{
		float fyValue1 = fTilt1*i + std::min(fb1, fb3);
		float fyValue2 = fTilt2*i + std::min(fb2, fb4);

		float fyValue3 = fTilt1*i + std::max(fb1, fb3);
		float fyValue4 = fTilt2*i + std::max(fb2, fb4);

		float fMinValue;
		float fMaxValue;
		if (abs(int(fyValue1)) < abs(int(fyValue2)))
			fMaxValue = fyValue1;
		else
			fMaxValue = fyValue2;
		if (abs(int(fyValue3)) < abs(int(fyValue4)))
			fMinValue = fyValue3;
		else
			fMinValue = fyValue4;

		for (int j = int(std::min(fMinValue, fMaxValue)); j < int(std::max(fMinValue, fMaxValue)); ++j) {
			if ( ForAttrRegionCell( lMapIndex, lCX + (lCW / 2) + i, lCY + (lCH / 2) + j, dwAttr, mode ) )
				return true;
		}
	}

	return mode == ATTR_REGION_MODE_CHECK ? false : true;
}

bool SECTREE_MANAGER::ForAttrRegion(int lMapIndex, int lStartX, int lStartY, int lEndX, int lEndY, int lRotate, DWORD dwAttr, EAttrRegionMode mode)
{
	LPSECTREE_MAP pkMapSectree = GetMap(lMapIndex);

	if (!pkMapSectree)
	{
		SPDLOG_ERROR("Cannot find SECTREE_MAP by map index {}", lMapIndex);
		return mode == ATTR_REGION_MODE_CHECK ? true : false;
	}

	//
	// ������ ��ǥ�� Cell �� ũ�⿡ ���� Ȯ���Ѵ�.
	//

	lStartX	-= lStartX % CELL_SIZE;
	lStartY	-= lStartY % CELL_SIZE;
	lEndX	+= lEndX % CELL_SIZE;
	lEndY	+= lEndY % CELL_SIZE;

	//
	// Cell ��ǥ�� ���Ѵ�.
	// 

	int lCX = lStartX / CELL_SIZE;
	int lCY = lStartY / CELL_SIZE;
	int lCW = (lEndX - lStartX) / CELL_SIZE;
	int lCH = (lEndY - lStartY) / CELL_SIZE;

	SPDLOG_DEBUG("ForAttrRegion {} {} ~ {} {}", lStartX, lStartY, lEndX, lEndY);

	lRotate = lRotate % 360;

	if (0 == lRotate % 90)
		return ForAttrRegionRightAngle( lMapIndex, lCX, lCY, lCW, lCH, lRotate, dwAttr, mode );

	return ForAttrRegionFreeAngle( lMapIndex, lCX, lCY, lCW, lCH, lRotate, dwAttr, mode );
}

bool SECTREE_MANAGER::SaveAttributeToImage(int lMapIndex, const char * c_pszFileName, LPSECTREE_MAP pMapSrc)
{
	LPSECTREE_MAP pMap = SECTREE_MANAGER::GetMap(lMapIndex);

	if (!pMap)
	{
		if (pMapSrc)
			pMap = pMapSrc;
		else
		{
			SPDLOG_ERROR("cannot find sectree_map {}", lMapIndex);
			return false;
		}
	}

	int iMapHeight = pMap->m_setting.iHeight / 128 / 200;
	int iMapWidth = pMap->m_setting.iWidth / 128 / 200;

	if (iMapHeight < 0 || iMapWidth < 0)
	{
		SPDLOG_ERROR("map size error w {} h {}", iMapWidth, iMapHeight);
		return false;
	}

	SPDLOG_DEBUG("SaveAttributeToImage w {} h {} file {}", iMapWidth, iMapHeight, c_pszFileName);

	CTargaImage image;

	image.Create(512 * iMapWidth, 512 * iMapHeight);

	DWORD * pdwDest = (DWORD *) image.GetBasePointer();

	int pixels = 0;
	int x, x2;
	int y, y2;

	DWORD * pdwLine = M2_NEW DWORD[SECTREE_SIZE / CELL_SIZE];

	for (y = 0; y < 4 * iMapHeight; ++y)
	{
		for (y2 = 0; y2 < SECTREE_SIZE / CELL_SIZE; ++y2)
		{
			for (x = 0; x < 4 * iMapWidth; ++x)
			{
				SECTREEID id;

				id.coord.x = x + pMap->m_setting.iBaseX / SECTREE_SIZE;
				id.coord.y = y + pMap->m_setting.iBaseY / SECTREE_SIZE;

				LPSECTREE pSec = pMap->Find(id.package);

				if (!pSec)
				{
					SPDLOG_ERROR("cannot get sectree for {} {} {} {}", (int) id.coord.x, (int) id.coord.y, pMap->m_setting.iBaseX, pMap->m_setting.iBaseY);
					continue;
				}

				pSec->m_pkAttribute->CopyRow(y2, pdwLine);

				if (!pdwLine)
				{
					SPDLOG_ERROR("cannot get attribute line pointer");
					M2_DELETE_ARRAY(pdwLine);
					continue;
				}

				for (x2 = 0; x2 < SECTREE_SIZE / CELL_SIZE; ++x2)
				{
					DWORD dwColor;

					if (IS_SET(pdwLine[x2], ATTR_WATER))
						dwColor = 0xff0000ff;
					else if (IS_SET(pdwLine[x2], ATTR_BANPK))
						dwColor = 0xff00ff00;
					else if (IS_SET(pdwLine[x2], ATTR_BLOCK))
						dwColor = 0xffff0000;
					else
						dwColor = 0xffffffff;

					*(pdwDest++) = dwColor;
					pixels++;
				}
			}
		}
	}

	M2_DELETE_ARRAY(pdwLine);

	if (image.Save(c_pszFileName))
	{
		SPDLOG_DEBUG("SECTREE: map {} attribute saved to {} ({} bytes)", lMapIndex, c_pszFileName, pixels);
		return true;
	}
	else
	{
		SPDLOG_ERROR("cannot save file, map_index {} filename {}", lMapIndex, c_pszFileName);
		return false;
	}
}

struct FPurgeMonsters
{
	void operator() (LPENTITY ent)
	{
		if ( ent->IsType(ENTITY_CHARACTER) == true )
		{
			LPCHARACTER lpChar = (LPCHARACTER)ent;

			if ( lpChar->IsMonster() == true && !lpChar->IsPet())
			{
				M2_DESTROY_CHARACTER(lpChar);
			}
		}
	}
};

void SECTREE_MANAGER::PurgeMonstersInMap(int lMapIndex)
{
	LPSECTREE_MAP sectree = SECTREE_MANAGER::instance().GetMap(lMapIndex);

	if ( sectree != NULL )
	{
		struct FPurgeMonsters f;

		sectree->for_each( f );
	}
}

struct FPurgeStones
{
	void operator() (LPENTITY ent)
	{
		if ( ent->IsType(ENTITY_CHARACTER) == true )
		{
			LPCHARACTER lpChar = (LPCHARACTER)ent;

			if ( lpChar->IsStone() == true )
			{
				M2_DESTROY_CHARACTER(lpChar);
			}
		}
	}
};

void SECTREE_MANAGER::PurgeStonesInMap(int lMapIndex)
{
	LPSECTREE_MAP sectree = SECTREE_MANAGER::instance().GetMap(lMapIndex);

	if ( sectree != NULL )
	{
		struct FPurgeStones f;

		sectree->for_each( f );
	}
}

struct FPurgeNPCs
{
	void operator() (LPENTITY ent)
	{
		if ( ent->IsType(ENTITY_CHARACTER) == true )
		{
			LPCHARACTER lpChar = (LPCHARACTER)ent;

			if ( lpChar->IsNPC() == true && !lpChar->IsPet())
			{
				M2_DESTROY_CHARACTER(lpChar);
			}
		}
	}
};

void SECTREE_MANAGER::PurgeNPCsInMap(int lMapIndex)
{
	LPSECTREE_MAP sectree = SECTREE_MANAGER::instance().GetMap(lMapIndex);

	if ( sectree != NULL )
	{
		struct FPurgeNPCs f;

		sectree->for_each( f );
	}
}

struct FCountMonsters
{
	std::map<VID, VID> m_map_Monsters;

	void operator() (LPENTITY ent)
	{
		if ( ent->IsType(ENTITY_CHARACTER) == true )
		{
			LPCHARACTER lpChar = (LPCHARACTER)ent;

			if ( lpChar->IsMonster() == true )
			{
				m_map_Monsters[lpChar->GetVID()] = lpChar->GetVID();
			}
		}
	}
};

size_t SECTREE_MANAGER::GetMonsterCountInMap(int lMapIndex)
{
	LPSECTREE_MAP sectree = SECTREE_MANAGER::instance().GetMap(lMapIndex);

	if ( sectree != NULL )
	{
		struct FCountMonsters f;

		sectree->for_each( f );

		return f.m_map_Monsters.size();
	}

	return 0;
}

struct FCountSpecifiedMonster
{
	DWORD SpecifiedVnum;
	size_t cnt;

	FCountSpecifiedMonster(DWORD id)
		: SpecifiedVnum(id), cnt(0)
	{}

	void operator() (LPENTITY ent)
	{
		if (true == ent->IsType(ENTITY_CHARACTER))
		{
			LPCHARACTER pChar = static_cast<LPCHARACTER>(ent);

			if (true == pChar->IsStone())
			{
				if (pChar->GetMobTable().dwVnum == SpecifiedVnum)
					cnt++;
			}
		}
	}
};

size_t SECTREE_MANAGER::GetMonsterCountInMap(int lMapIndex, DWORD dwVnum)
{
	LPSECTREE_MAP sectree = SECTREE_MANAGER::instance().GetMap(lMapIndex);

	if (NULL != sectree)
	{
		struct FCountSpecifiedMonster f(dwVnum);

		sectree->for_each( f );

		return f.cnt;
	}

	return 0;
}


