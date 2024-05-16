#include "stdafx.h"

#include <common/billing.h>
#include <common/building.h>
#include <common/VnumHelper.h>
#include "../../libgame/include/grid.h"

#include "ClientManager.h"

#include "Main.h"
#include "Config.h"
#include "DBManager.h"
#include "QID.h"
#include "GuildManager.h"
#include "PrivManager.h"
#include "MoneyLog.h"
#include "ItemAwardManager.h"
#include "Marriage.h"
#include "Monarch.h"
#include "BlockCountry.h"
#include "ItemIDRangeManager.h"
#include "Cache.h"
#ifdef __AUCTION__
#include "AuctionManager.h"
#endif
extern int g_iPlayerCacheFlushSeconds;
extern int g_iItemCacheFlushSeconds;
extern int g_test_server;
extern std::string g_stLocale;
extern std::string g_stLocaleNameColumn;
bool CreateItemTableFromRes(MYSQL_RES * res, std::vector<TPlayerItem> * pVec, DWORD dwPID);

DWORD g_dwUsageMax = 0;
DWORD g_dwUsageAvg = 0;

CPacketInfo g_query_info;
CPacketInfo g_item_info;

int g_item_count = 0;
int g_query_count[2];

static void AcceptConnection(
        evconnlistener* listener,
        evutil_socket_t fd,
        sockaddr* address,
        int socklen,
        void* ctx)
{
    // We got a new connection! We have to create a new peer.
    auto* clientManager = (CClientManager*) ctx;

    // Create a new buffer for the peer
    event_base *base = evconnlistener_get_base(listener);
    bufferevent *bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);

    // Create a new peer
    CPeer* peer = clientManager->AddPeer(bev, address);

    // Set the event handlers for this peer
    bufferevent_setcb(bev, DescReadHandler, DescWriteHandler, DescEventHandler, peer);

    // Enable the events
    bufferevent_enable(bev, EV_READ|EV_WRITE);
}

static void AcceptError(evconnlistener *listener, void *ctx) {
    struct event_base *base = evconnlistener_get_base(listener);
    int err = EVUTIL_SOCKET_ERROR();
    SPDLOG_CRITICAL("Got an error {} ({}) on the listener. Shutting down.", err, evutil_socket_error_to_string(err));

    event_base_loopexit(base, nullptr);
}

static void DescReadHandler(bufferevent *bev, void *ctx) {
    auto* peer = (CPeer*) ctx;

    if (peer == CClientManager::Instance().GetAuthPeer())
        SPDLOG_TRACE("AUTH_PEER_READ: size {}", peer->GetRecvLength());

    CClientManager::Instance().ProcessPackets(peer);
}

static void DescWriteHandler(bufferevent *bev, void *ctx) {
    auto* peer = (CPeer*) ctx;

    if (peer == CClientManager::Instance().GetAuthPeer())
        SPDLOG_TRACE("AUTH_PEER_WRITE: size {}", peer->GetSendLength());
}

static void DescEventHandler(bufferevent *bev, short events, void *ctx) {
    auto* peer = (CPeer*) ctx;

    if (events & BEV_EVENT_ERROR)
        SPDLOG_ERROR("PEER libevent error, handle: {}", peer->GetHandle());

    if (events & BEV_EVENT_EOF)
        SPDLOG_DEBUG("PEER disconnected: handle {}", peer->GetHandle());

    // Either the socket was closed or an error occured, therefore we can disconnect this peer.
    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
        CClientManager::Instance().RemovePeer(peer);
}

CClientManager::CClientManager() :
	m_pkAuthPeer(NULL),
	m_iPlayerIDStart(0),
	m_iPlayerDeleteLevelLimit(0),
	m_iPlayerDeleteLevelLimitLower(0),
	m_bChinaEventServer(false),
	m_iShopTableSize(0),
	m_pShopTable(NULL),
	m_iRefineTableSize(0),
	m_pRefineTable(NULL),
	m_bShutdowned(false),
	m_iCacheFlushCount(0),
	m_iCacheFlushCountLimit(200)
{
	m_itemRange.dwMin = 0;
	m_itemRange.dwMax = 0;
	m_itemRange.dwUsableItemIDMin = 0;

	memset(g_query_count, 0, sizeof(g_query_count));
}

CClientManager::~CClientManager()
{
	Destroy();
}

void CClientManager::SetPlayerIDStart(int iIDStart)
{
	m_iPlayerIDStart = iIDStart;
}

void CClientManager::Destroy()
{
	m_mChannelStatus.clear();

    // Close the peer connections and empty the peer list
	for (auto &peer: m_peerList)
		peer->Destroy();
	m_peerList.clear();

    // Free the libevent resources
	if (m_listener) {
        evconnlistener_free(m_listener);
        m_listener = nullptr;
    }

    if (m_base) {
        event_base_free(m_base);
        m_base = nullptr;
    }
}

bool CClientManager::Initialize()
{
	int tmpValue;
	
	//BOOT_LOCALIZATION
	if (!InitializeLocalization())
	{
		SPDLOG_ERROR("Failed Localization Infomation so exit");
		return false;
	}
		
	//END_BOOT_LOCALIZATION
	//ITEM_UNIQUE_ID
	
	if (!InitializeNowItemID())
	{
		SPDLOG_ERROR(" Item range Initialize Failed. Exit DBCache Server");
		return false;
	}
	//END_ITEM_UNIQUE_ID

	if (!InitializeTables())
	{
		SPDLOG_ERROR("Table Initialize FAILED");
		return false;
	}

	CGuildManager::instance().BootReserveWar();

	if (!CConfig::instance().GetValue("BIND_PORT", &tmpValue))
		tmpValue = 5300;

	char szBindIP[128];

	if (!CConfig::instance().GetValue("BIND_IP", szBindIP, 128))
		strlcpy(szBindIP, "0", sizeof(szBindIP));

    // Create a new libevent base and listen for new connections
    m_base = event_base_new();
    if (!m_base) {
        SPDLOG_ERROR("Libevent base initialization FAILED!");
        return false;
    }

    sockaddr_in sin = {};

    /* This is an INET address */
    sin.sin_family = AF_INET;

    sin.sin_addr.s_addr = inet_addr(szBindIP);
    sin.sin_port = htons(tmpValue);

    m_listener = evconnlistener_new_bind(
        m_base,
        AcceptConnection, this,
        LEV_OPT_CLOSE_ON_FREE|LEV_OPT_REUSEABLE, -1,
        (const sockaddr*)&sin, sizeof(sin)
    );
    if (!m_listener) {
        SPDLOG_ERROR("Libevent listener initialization FAILED!");
        return false;
    }
    evconnlistener_set_error_cb(m_listener, AcceptError);

	if (!CConfig::instance().GetValue("BACKUP_LIMIT_SEC", &tmpValue))
		tmpValue = 600;

	m_looping = true;

	if (!CConfig::instance().GetValue("PLAYER_DELETE_LEVEL_LIMIT", &m_iPlayerDeleteLevelLimit))
	{
		SPDLOG_ERROR("conf.txt: Cannot find PLAYER_DELETE_LEVEL_LIMIT, use default level {}", PLAYER_MAX_LEVEL_CONST + 1);
		m_iPlayerDeleteLevelLimit = PLAYER_MAX_LEVEL_CONST + 1;
	}

	if (!CConfig::instance().GetValue("PLAYER_DELETE_LEVEL_LIMIT_LOWER", &m_iPlayerDeleteLevelLimitLower))
	{
		m_iPlayerDeleteLevelLimitLower = 0;
	}

	SPDLOG_DEBUG("PLAYER_DELETE_LEVEL_LIMIT set to {}", m_iPlayerDeleteLevelLimit);
	SPDLOG_DEBUG("PLAYER_DELETE_LEVEL_LIMIT_LOWER set to {}", m_iPlayerDeleteLevelLimitLower);

	m_bChinaEventServer = false;

	int	iChinaEventServer = 0;

	if (CConfig::instance().GetValue("CHINA_EVENT_SERVER", &iChinaEventServer))
		m_bChinaEventServer = (iChinaEventServer);

	SPDLOG_DEBUG("CHINA_EVENT_SERVER {}", CClientManager::instance().IsChinaEventServer()?"true":"false");


	LoadEventFlag();

	// database character-set�� ������ ����
	if (g_stLocale == "big5" || g_stLocale == "sjis")
	    CDBManager::instance().QueryLocaleSet();

	return true;
}

void CClientManager::MainLoop()
{
	SQLMsg * tmp;

	SPDLOG_DEBUG("ClientManager pointer is {}", (void*) this);

	// ���η���
	while (!m_bShutdowned)
	{
		while ((tmp = CDBManager::instance().PopResult()))
		{
			AnalyzeQueryResult(tmp);
			delete tmp;
		}

		if (!Process())
			break;
    }

	//
	// ���η��� ����ó��
	//
	SPDLOG_DEBUG("MainLoop exited, Starting cache flushing");

	signal_timer_disable();

	itertype(m_map_playerCache) it = m_map_playerCache.begin();

	//�÷��̾� ���̺� ĳ�� �÷���	
	while (it != m_map_playerCache.end())
	{
		CPlayerTableCache * c = (it++)->second;

		c->Flush();
		delete c;
	}
	m_map_playerCache.clear();

	
	itertype(m_map_itemCache) it2 = m_map_itemCache.begin();
	//������ �÷���
	while (it2 != m_map_itemCache.end())
	{
		CItemCache * c = (it2++)->second;

		c->Flush();
		delete c;
	}
	m_map_itemCache.clear();

	// MYSHOP_PRICE_LIST
	//
	// ���λ��� ������ ���� ����Ʈ Flush
	//
	for (itertype(m_mapItemPriceListCache) itPriceList = m_mapItemPriceListCache.begin(); itPriceList != m_mapItemPriceListCache.end(); ++itPriceList)
	{
		CItemPriceListTableCache* pCache = itPriceList->second;
		pCache->Flush();
		delete pCache;
	}

	m_mapItemPriceListCache.clear();
	// END_OF_MYSHOP_PRICE_LIST
}

void CClientManager::Quit()
{
	m_bShutdowned = true;
}

void CClientManager::QUERY_BOOT(CPeer* peer, TPacketGDBoot * p)
{
	const BYTE bPacketVersion = 6; // BOOT ��Ŷ�� �ٲ𶧸��� ��ȣ�� �ø����� �Ѵ�.

	std::vector<tAdminInfo> vAdmin;
	std::vector<std::string> vHost;

	__GetHostInfo(vHost);
	__GetAdminInfo(p->szIP, vAdmin);	

	SPDLOG_DEBUG("QUERY_BOOT : AdminInfo (Request ServerIp {}) ", p->szIP);

	DWORD dwPacketSize = 
		sizeof(DWORD) +
		sizeof(BYTE) +
		sizeof(WORD) + sizeof(WORD) + sizeof(TMobTable) * m_vec_mobTable.size() +
		sizeof(WORD) + sizeof(WORD) + sizeof(TItemTable) * m_vec_itemTable.size() +
		sizeof(WORD) + sizeof(WORD) + sizeof(TShopTable) * m_iShopTableSize +
		sizeof(WORD) + sizeof(WORD) + sizeof(TSkillTable) * m_vec_skillTable.size() +
		sizeof(WORD) + sizeof(WORD) + sizeof(TRefineTable) * m_iRefineTableSize +
		sizeof(WORD) + sizeof(WORD) + sizeof(TItemAttrTable) * m_vec_itemAttrTable.size() +
		sizeof(WORD) + sizeof(WORD) + sizeof(TItemAttrTable) * m_vec_itemRareTable.size() +
		sizeof(WORD) + sizeof(WORD) + sizeof(TBanwordTable) * m_vec_banwordTable.size() +
		sizeof(WORD) + sizeof(WORD) + sizeof(building::TLand) * m_vec_kLandTable.size() +
		sizeof(WORD) + sizeof(WORD) + sizeof(building::TObjectProto) * m_vec_kObjectProto.size() + 
		sizeof(WORD) + sizeof(WORD) + sizeof(building::TObject) * m_map_pkObjectTable.size() +
#ifdef __AUCTION__
		sizeof(WORD) + sizeof(WORD) + sizeof(TPlayerItem) * AuctionManager::instance().GetAuctionItemSize() +
		sizeof(WORD) + sizeof(WORD) + sizeof(TAuctionItemInfo) * AuctionManager::instance().GetAuctionSize() +
		sizeof(WORD) + sizeof(WORD) + sizeof(TSaleItemInfo) * AuctionManager::instance().GetSaleSize() +
		sizeof(WORD) + sizeof(WORD) + sizeof(TWishItemInfo) * AuctionManager::instance().GetWishSize() +
		sizeof(WORD) + sizeof(WORD) + (sizeof(DWORD) + sizeof(DWORD) + sizeof(int)) * AuctionManager::instance().GetMyBidSize() +
#endif
		sizeof(time_t) + 
		sizeof(WORD) + sizeof(WORD) + sizeof(TItemIDRangeTable)*2 +
		//ADMIN_MANAGER
		sizeof(WORD) + sizeof(WORD) + 16 * vHost.size() +
		sizeof(WORD) + sizeof(WORD) +  sizeof(tAdminInfo) *  vAdmin.size() +
		//END_ADMIN_MANAGER
		sizeof(WORD) + sizeof(WORD) + sizeof(TMonarchInfo) + 
		sizeof(WORD) + sizeof(WORD) + sizeof(MonarchCandidacy)* CMonarch::instance().MonarchCandidacySize() +
		sizeof(WORD); 

	peer->EncodeHeader(HEADER_DG_BOOT, 0, dwPacketSize);
	peer->Encode(&dwPacketSize, sizeof(DWORD));
	peer->Encode(&bPacketVersion, sizeof(BYTE));

	SPDLOG_DEBUG("BOOT: PACKET: {}", dwPacketSize);
	SPDLOG_DEBUG("BOOT: VERSION: {}", bPacketVersion);

	SPDLOG_DEBUG("sizeof(TMobTable) = {}", sizeof(TMobTable));
	SPDLOG_DEBUG("sizeof(TItemTable) = {}", sizeof(TItemTable));
	SPDLOG_DEBUG("sizeof(TShopTable) = {}", sizeof(TShopTable));
	SPDLOG_DEBUG("sizeof(TSkillTable) = {}", sizeof(TSkillTable));
	SPDLOG_DEBUG("sizeof(TRefineTable) = {}", sizeof(TRefineTable));
	SPDLOG_DEBUG("sizeof(TItemAttrTable) = {}", sizeof(TItemAttrTable));
	SPDLOG_DEBUG("sizeof(TItemRareTable) = {}", sizeof(TItemAttrTable));
	SPDLOG_DEBUG("sizeof(TBanwordTable) = {}", sizeof(TBanwordTable));
	SPDLOG_DEBUG("sizeof(TLand) = {}", sizeof(building::TLand));
	SPDLOG_DEBUG("sizeof(TObjectProto) = {}", sizeof(building::TObjectProto));
	SPDLOG_DEBUG("sizeof(TObject) = {}", sizeof(building::TObject));
	//ADMIN_MANAGER
	SPDLOG_DEBUG("sizeof(tAdminInfo) = {} * {} ", sizeof(tAdminInfo) * vAdmin.size());
	//END_ADMIN_MANAGER
	SPDLOG_DEBUG("sizeof(TMonarchInfo) = {} * {}", sizeof(TMonarchInfo));

	peer->EncodeWORD(sizeof(TMobTable));
	peer->EncodeWORD(m_vec_mobTable.size());
	peer->Encode(&m_vec_mobTable[0], sizeof(TMobTable) * m_vec_mobTable.size());

	peer->EncodeWORD(sizeof(TItemTable));
	peer->EncodeWORD(m_vec_itemTable.size());
	peer->Encode(&m_vec_itemTable[0], sizeof(TItemTable) * m_vec_itemTable.size());

	peer->EncodeWORD(sizeof(TShopTable));
	peer->EncodeWORD(m_iShopTableSize);
	peer->Encode(m_pShopTable, sizeof(TShopTable) * m_iShopTableSize);

	peer->EncodeWORD(sizeof(TSkillTable));
	peer->EncodeWORD(m_vec_skillTable.size());
	peer->Encode(&m_vec_skillTable[0], sizeof(TSkillTable) * m_vec_skillTable.size());

	peer->EncodeWORD(sizeof(TRefineTable));
	peer->EncodeWORD(m_iRefineTableSize);
	peer->Encode(m_pRefineTable, sizeof(TRefineTable) * m_iRefineTableSize);

	peer->EncodeWORD(sizeof(TItemAttrTable));
	peer->EncodeWORD(m_vec_itemAttrTable.size());
	peer->Encode(&m_vec_itemAttrTable[0], sizeof(TItemAttrTable) * m_vec_itemAttrTable.size());

	peer->EncodeWORD(sizeof(TItemAttrTable));
	peer->EncodeWORD(m_vec_itemRareTable.size());
	peer->Encode(&m_vec_itemRareTable[0], sizeof(TItemAttrTable) * m_vec_itemRareTable.size());

	peer->EncodeWORD(sizeof(TBanwordTable));
	peer->EncodeWORD(m_vec_banwordTable.size());
	peer->Encode(&m_vec_banwordTable[0], sizeof(TBanwordTable) * m_vec_banwordTable.size());

	peer->EncodeWORD(sizeof(building::TLand));
	peer->EncodeWORD(m_vec_kLandTable.size());
	peer->Encode(&m_vec_kLandTable[0], sizeof(building::TLand) * m_vec_kLandTable.size());

	peer->EncodeWORD(sizeof(building::TObjectProto));
	peer->EncodeWORD(m_vec_kObjectProto.size());
	peer->Encode(&m_vec_kObjectProto[0], sizeof(building::TObjectProto) * m_vec_kObjectProto.size());

	peer->EncodeWORD(sizeof(building::TObject));
	peer->EncodeWORD(m_map_pkObjectTable.size());

	itertype(m_map_pkObjectTable) it = m_map_pkObjectTable.begin();

	while (it != m_map_pkObjectTable.end())
		peer->Encode((it++)->second, sizeof(building::TObject));

	// Auction Boot
#ifdef __AUCTION__
	AuctionManager::instance().Boot (peer);
#endif
	time_t now = time(0);
	peer->Encode(&now, sizeof(time_t));

	TItemIDRangeTable itemRange = CItemIDRangeManager::instance().GetRange();
	TItemIDRangeTable itemRangeSpare = CItemIDRangeManager::instance().GetRange();

	peer->EncodeWORD(sizeof(TItemIDRangeTable));
	peer->EncodeWORD(1);
	peer->Encode(&itemRange, sizeof(TItemIDRangeTable));
	peer->Encode(&itemRangeSpare, sizeof(TItemIDRangeTable));

	peer->SetItemIDRange(itemRange);
	peer->SetSpareItemIDRange(itemRangeSpare);

	//ADMIN_MANAGER
	peer->EncodeWORD(16);
	peer->EncodeWORD(vHost.size());

	for (size_t n = 0; n < vHost.size(); ++n)
	{
		peer->Encode(vHost[n].c_str(), 16);
		SPDLOG_DEBUG("GMHosts {}", vHost[n].c_str());
	}

	peer->EncodeWORD(sizeof(tAdminInfo));
	peer->EncodeWORD(vAdmin.size());

	for (size_t n = 0; n < vAdmin.size(); ++n)
	{
		peer->Encode(&vAdmin[n], sizeof(tAdminInfo));
		SPDLOG_DEBUG("Admin name {} ConntactIP {}", vAdmin[n].m_szName, vAdmin[n].m_szContactIP);
	}
	//END_ADMIN_MANAGER

	//MONARCH
	peer->EncodeWORD(sizeof(TMonarchInfo));
	peer->EncodeWORD(1);
	peer->Encode(CMonarch::instance().GetMonarch(), sizeof(TMonarchInfo));

	CMonarch::VEC_MONARCHCANDIDACY & rVecMonarchCandidacy = CMonarch::instance().GetVecMonarchCandidacy();
	
	size_t num_monarch_candidacy = CMonarch::instance().MonarchCandidacySize();
	peer->EncodeWORD(sizeof(MonarchCandidacy));
	peer->EncodeWORD(num_monarch_candidacy);
	if (num_monarch_candidacy != 0) {
		peer->Encode(&rVecMonarchCandidacy[0], sizeof(MonarchCandidacy) * num_monarch_candidacy);
	}
	//END_MONARCE

	SPDLOG_TRACE("MONARCHCandidacy Size {}", CMonarch::instance().MonarchCandidacySize());

	peer->EncodeWORD(0xffff);
}

void CClientManager::SendPartyOnSetup(CPeer* pkPeer)
{
	TPartyMap & pm = m_map_pkChannelParty[pkPeer->GetChannel()];

	for (itertype(pm) it_party = pm.begin(); it_party != pm.end(); ++it_party)
	{
		SPDLOG_DEBUG("PARTY SendPartyOnSetup Party [{}]", it_party->first);
		pkPeer->EncodeHeader(HEADER_DG_PARTY_CREATE, 0, sizeof(TPacketPartyCreate));
		pkPeer->Encode(&it_party->first, sizeof(DWORD));

		for (itertype(it_party->second) it_member = it_party->second.begin(); it_member != it_party->second.end(); ++it_member)
		{
			SPDLOG_DEBUG("PARTY SendPartyOnSetup Party [{}] Member [{}]", it_party->first, it_member->first);
			pkPeer->EncodeHeader(HEADER_DG_PARTY_ADD, 0, sizeof(TPacketPartyAdd));
			pkPeer->Encode(&it_party->first, sizeof(DWORD));
			pkPeer->Encode(&it_member->first, sizeof(DWORD));
			pkPeer->Encode(&it_member->second.bRole, sizeof(BYTE));

			pkPeer->EncodeHeader(HEADER_DG_PARTY_SET_MEMBER_LEVEL, 0, sizeof(TPacketPartySetMemberLevel));
			pkPeer->Encode(&it_party->first, sizeof(DWORD));
			pkPeer->Encode(&it_member->first, sizeof(DWORD));
			pkPeer->Encode(&it_member->second.bLevel, sizeof(BYTE));
		}
	}
}

void CClientManager::QUERY_PLAYER_COUNT(CPeer * pkPeer, TPlayerCountPacket * pPacket)
{
	pkPeer->SetUserCount(pPacket->dwCount);
}

void CClientManager::QUERY_QUEST_SAVE(CPeer * pkPeer, TQuestTable * pTable, DWORD dwLen)
{
	if (0 != (dwLen % sizeof(TQuestTable)))
	{
		SPDLOG_ERROR("invalid packet size {}, sizeof(TQuestTable) == {}", dwLen, sizeof(TQuestTable));
		return;
	}

	int iSize = dwLen / sizeof(TQuestTable);

	char szQuery[1024];

	for (int i = 0; i < iSize; ++i, ++pTable)
	{
		if (pTable->lValue == 0)
		{
			snprintf(szQuery, sizeof(szQuery),
					"DELETE FROM quest%s WHERE dwPID=%d AND szName='%s' AND szState='%s'",
					GetTablePostfix(), pTable->dwPID, pTable->szName, pTable->szState);
		}
		else
		{
			snprintf(szQuery, sizeof(szQuery),
					"REPLACE INTO quest%s (dwPID, szName, szState, lValue) VALUES(%d, '%s', '%s', %d)",
					GetTablePostfix(), pTable->dwPID, pTable->szName, pTable->szState, pTable->lValue);
		}

		CDBManager::instance().ReturnQuery(szQuery, QID_QUEST_SAVE, pkPeer->GetHandle(), NULL);
	}
}

void CClientManager::QUERY_SAFEBOX_LOAD(CPeer * pkPeer, DWORD dwHandle, TSafeboxLoadPacket * packet, bool bMall)
{
	ClientHandleInfo * pi = new ClientHandleInfo(dwHandle);
	strlcpy(pi->safebox_password, packet->szPassword, sizeof(pi->safebox_password));
	pi->account_id = packet->dwID;
	pi->account_index = 0;
	pi->ip[0] = bMall ? 1 : 0;
	strlcpy(pi->login, packet->szLogin, sizeof(pi->login));

	char szQuery[QUERY_MAX_LEN];
	snprintf(szQuery, sizeof(szQuery),
			"SELECT account_id, size, password FROM safebox%s WHERE account_id=%u",
			GetTablePostfix(), packet->dwID);

    SPDLOG_TRACE("HEADER_GD_SAFEBOX_LOAD (handle: {} account.id {} is_mall {})", dwHandle, packet->dwID, bMall ? 1 : 0);

	CDBManager::instance().ReturnQuery(szQuery, QID_SAFEBOX_LOAD, pkPeer->GetHandle(), pi);
}

void CClientManager::RESULT_SAFEBOX_LOAD(CPeer * pkPeer, SQLMsg * msg)
{
	CQueryInfo * qi = (CQueryInfo *) msg->pvUserData;
	ClientHandleInfo * pi = (ClientHandleInfo *) qi->pvData;
	DWORD dwHandle = pi->dwHandle;

	// ���⿡�� ����ϴ� account_index�� ���� ������ ���Ѵ�.
	// ù��° �н����� �˾Ƴ��� ���� �ϴ� ������ 0
	// �ι�° ���� �����͸� ������ ������ 1

	if (pi->account_index == 0)
	{
		char szSafeboxPassword[SAFEBOX_PASSWORD_MAX_LEN + 1];
		strlcpy(szSafeboxPassword, pi->safebox_password, sizeof(szSafeboxPassword));

		TSafeboxTable * pSafebox = new TSafeboxTable;
		memset(pSafebox, 0, sizeof(TSafeboxTable));

		SQLResult * res = msg->Get();

		if (res->uiNumRows == 0)
		{
			if (strcmp("000000", szSafeboxPassword))
			{
				pkPeer->EncodeHeader(HEADER_DG_SAFEBOX_WRONG_PASSWORD, dwHandle, 0);
				delete pi;
				return;
			}
		}
		else
		{
			MYSQL_ROW row = mysql_fetch_row(res->pSQLResult);

			// ��й�ȣ�� Ʋ����..
			if (((!row[2] || !*row[2]) && strcmp("000000", szSafeboxPassword)) ||
				((row[2] && *row[2]) && strcmp(row[2], szSafeboxPassword)))
			{
				pkPeer->EncodeHeader(HEADER_DG_SAFEBOX_WRONG_PASSWORD, dwHandle, 0);
				delete pi;
				return;
			}

			if (!row[0])
				pSafebox->dwID = 0;
			else
				str_to_number(pSafebox->dwID, row[0]);

			if (!row[1])
				pSafebox->bSize = 0;
			else
				str_to_number(pSafebox->bSize, row[1]);
			/*
			   if (!row[3])
			   pSafebox->dwGold = 0;
			   else
			   pSafebox->dwGold = atoi(row[3]);
			   */
			if (pi->ip[0] == 1)
			{
				pSafebox->bSize = 1;
				SPDLOG_DEBUG("MALL id[{}] size[{}]", pSafebox->dwID, pSafebox->bSize);
			}
			else
				SPDLOG_DEBUG("SAFEBOX id[{}] size[{}]", pSafebox->dwID, pSafebox->bSize);
		}

		if (0 == pSafebox->dwID)
			pSafebox->dwID = pi->account_id;

		pi->pSafebox = pSafebox;

		char szQuery[512];
		snprintf(szQuery, sizeof(szQuery), 
				"SELECT id, window+0, pos, count, vnum, socket0, socket1, socket2, "
				"attrtype0, attrvalue0, "
				"attrtype1, attrvalue1, "
				"attrtype2, attrvalue2, "
				"attrtype3, attrvalue3, "
				"attrtype4, attrvalue4, "
				"attrtype5, attrvalue5, "
				"attrtype6, attrvalue6 "
				"FROM item%s WHERE owner_id=%d AND window='%s'",
				GetTablePostfix(), pi->account_id, pi->ip[0] == 0 ? "SAFEBOX" : "MALL");

		pi->account_index = 1;

		CDBManager::instance().ReturnQuery(szQuery, QID_SAFEBOX_LOAD, pkPeer->GetHandle(), pi);
	}
	else
	{

		if (!pi->pSafebox)
		{
			SPDLOG_ERROR("null safebox pointer!");
			delete pi;
			return;
		}


		// ������ ������ �־����Ƿ� ������ ��� â�� ����ִ� �� ó��
		// ���̱� ������ â�� �ƾ� �ȿ����°� ����
		if (!msg->Get()->pSQLResult)
		{
			SPDLOG_ERROR("null safebox result");
			delete pi;
			return;
		}

		static std::vector<TPlayerItem> s_items;
		CreateItemTableFromRes(msg->Get()->pSQLResult, &s_items, pi->account_id);

		std::set<TItemAward *> * pSet = ItemAwardManager::instance().GetByLogin(pi->login);

		if (pSet && !m_vec_itemTable.empty())
		{

			CGrid grid(5, std::max<int>(1, pi->pSafebox->bSize) * 9);
			bool bEscape = false;

			for (DWORD i = 0; i < s_items.size(); ++i)
			{
				TPlayerItem & r = s_items[i];

				itertype(m_map_itemTableByVnum) it = m_map_itemTableByVnum.find(r.vnum);

				if (it == m_map_itemTableByVnum.end())
				{
					bEscape = true;
					SPDLOG_ERROR("invalid item vnum {} in safebox: login {}", r.vnum, pi->login);
					break;
				}

				grid.Put(r.pos, 1, it->second->bSize);
			}

			if (!bEscape)
			{
				std::vector<std::pair<DWORD, DWORD> > vec_dwFinishedAwardID;

				typeof(pSet->begin()) it = pSet->begin();

				char szQuery[512];

				while (it != pSet->end())
				{
					TItemAward * pItemAward = *(it++);
					const DWORD& dwItemVnum = pItemAward->dwVnum;

					if (pItemAward->bTaken)
						continue;

					if (pi->ip[0] == 0 && pItemAward->bMall)
						continue;

					if (pi->ip[0] == 1 && !pItemAward->bMall)
						continue;

					itertype(m_map_itemTableByVnum) it = m_map_itemTableByVnum.find(pItemAward->dwVnum);

					if (it == m_map_itemTableByVnum.end())
					{
						SPDLOG_ERROR("invalid item vnum {} in item_award: login {}", pItemAward->dwVnum, pi->login);
						continue;
					}

					TItemTable * pItemTable = it->second;

					int iPos;

					if ((iPos = grid.FindBlank(1, it->second->bSize)) == -1)
						break;

					TPlayerItem item;
					memset(&item, 0, sizeof(TPlayerItem));

					DWORD dwSocket2 = 0;

					if (pItemTable->bType == ITEM_UNIQUE)
					{
						if (pItemAward->dwSocket2 != 0)
							dwSocket2 = pItemAward->dwSocket2;
						else
							dwSocket2 = pItemTable->alValues[0];
					}
					else if ((dwItemVnum == 50300 || dwItemVnum == 70037) && pItemAward->dwSocket0 == 0)
					{
						DWORD dwSkillIdx;
						DWORD dwSkillVnum;

						do
						{
							dwSkillIdx = Random::get<int>(0, m_vec_skillTable.size() - 1);

							dwSkillVnum = m_vec_skillTable[dwSkillIdx].dwVnum;

							if (!dwSkillVnum > 120)
								continue;

							break;
						} while (1);

						pItemAward->dwSocket0 = dwSkillVnum;
					}
					else
					{
						switch (dwItemVnum)
						{
							case 72723: case 72724: case 72725: case 72726:
							case 72727: case 72728: case 72729: case 72730:
							// ���ù��������� ������ �ϴ� �� ��ġ��� ������...
							// �׷��� �׳� �ϵ� �ڵ�. ���� ���ڿ� �ڵ����� �����۵�.
							case 76004: case 76005: case 76021: case 76022:
							case 79012: case 79013:
								if (pItemAward->dwSocket2 == 0)
								{
									dwSocket2 = pItemTable->alValues[0];
								}
								else
								{
									dwSocket2 = pItemAward->dwSocket2;
								}
								break;
						}
					}

					if (GetItemID () > m_itemRange.dwMax)
					{
						SPDLOG_ERROR("UNIQUE ID OVERFLOW!!");
						break;
					}

					{
						itertype(m_map_itemTableByVnum) it = m_map_itemTableByVnum.find (dwItemVnum);
						if (it == m_map_itemTableByVnum.end())
						{
							SPDLOG_ERROR("Invalid item(vnum : {}). It is not in m_map_itemTableByVnum.", dwItemVnum);
							continue;
						}
						TItemTable* item_table = it->second;
						if (item_table == NULL)
						{
							SPDLOG_ERROR("Invalid item_table (vnum : {}). It's value is NULL in m_map_itemTableByVnum.", dwItemVnum);
							continue;
						}
						if (0 == pItemAward->dwSocket0)
						{
							for (int i = 0; i < ITEM_LIMIT_MAX_NUM; i++)
							{
								if (LIMIT_REAL_TIME == item_table->aLimits[i].bType)
								{
									if (0 == item_table->aLimits[i].lValue)
										pItemAward->dwSocket0 = time(0) + 60 * 60 * 24 * 7;
									else
										pItemAward->dwSocket0 = time(0) + item_table->aLimits[i].lValue;

									break;
								}
								else if (LIMIT_REAL_TIME_START_FIRST_USE == item_table->aLimits[i].bType || LIMIT_TIMER_BASED_ON_WEAR == item_table->aLimits[i].bType)
								{
									if (0 == item_table->aLimits[i].lValue)
										pItemAward->dwSocket0 = 60 * 60 * 24 * 7;
									else
										pItemAward->dwSocket0 = item_table->aLimits[i].lValue;

									break;
								}
							}
						}

						snprintf(szQuery, sizeof(szQuery), 
								"INSERT INTO item%s (id, owner_id, window, pos, vnum, count, socket0, socket1, socket2) "
								"VALUES(%u, %u, '%s', %d, %u, %u, %u, %u, %u)",
								GetTablePostfix(),
								GainItemID(),
								pi->account_id,
								pi->ip[0] == 0 ? "SAFEBOX" : "MALL",
								iPos,
								pItemAward->dwVnum, pItemAward->dwCount, pItemAward->dwSocket0, pItemAward->dwSocket1, dwSocket2);
					}

					std::unique_ptr<SQLMsg> pmsg(CDBManager::instance().DirectQuery(szQuery));
					SQLResult * pRes = pmsg->Get();
					SPDLOG_DEBUG("SAFEBOX Query : [{}]", szQuery);

					if (pRes->uiAffectedRows == 0 || pRes->uiInsertID == 0 || pRes->uiAffectedRows == (uint32_t)-1)
						break;

					item.id = pmsg->Get()->uiInsertID;
					item.window = pi->ip[0] == 0 ? SAFEBOX : MALL,
					item.pos = iPos;
					item.count = pItemAward->dwCount;
					item.vnum = pItemAward->dwVnum;
					item.alSockets[0] = pItemAward->dwSocket0;
					item.alSockets[1] = pItemAward->dwSocket1;
					item.alSockets[2] = dwSocket2;
					s_items.push_back(item);

					vec_dwFinishedAwardID.push_back(std::make_pair(pItemAward->dwID, item.id));
					grid.Put(iPos, 1, it->second->bSize);
				}

				for (DWORD i = 0; i < vec_dwFinishedAwardID.size(); ++i)
					ItemAwardManager::instance().Taken(vec_dwFinishedAwardID[i].first, vec_dwFinishedAwardID[i].second);
			}
		}

		pi->pSafebox->wItemCount = s_items.size();

		pkPeer->EncodeHeader(pi->ip[0] == 0 ? HEADER_DG_SAFEBOX_LOAD : HEADER_DG_MALL_LOAD, dwHandle, sizeof(TSafeboxTable) + sizeof(TPlayerItem) * s_items.size());

		pkPeer->Encode(pi->pSafebox, sizeof(TSafeboxTable));

		if (!s_items.empty())
			pkPeer->Encode(&s_items[0], sizeof(TPlayerItem) * s_items.size());

		delete pi;
	}
}

void CClientManager::QUERY_SAFEBOX_CHANGE_SIZE(CPeer * pkPeer, DWORD dwHandle, TSafeboxChangeSizePacket * p)
{
	ClientHandleInfo * pi = new ClientHandleInfo(dwHandle);
	pi->account_index = p->bSize;	// account_index�� ������� �ӽ÷� ���

	char szQuery[QUERY_MAX_LEN];

	if (p->bSize == 1)
		snprintf(szQuery, sizeof(szQuery), "INSERT INTO safebox%s (account_id, size) VALUES(%u, %u)", GetTablePostfix(), p->dwID, p->bSize);
	else
		snprintf(szQuery, sizeof(szQuery), "UPDATE safebox%s SET size=%u WHERE account_id=%u", GetTablePostfix(), p->bSize, p->dwID);

	CDBManager::instance().ReturnQuery(szQuery, QID_SAFEBOX_CHANGE_SIZE, pkPeer->GetHandle(), pi);
}

void CClientManager::RESULT_SAFEBOX_CHANGE_SIZE(CPeer * pkPeer, SQLMsg * msg)
{
	CQueryInfo * qi = (CQueryInfo *) msg->pvUserData;
	ClientHandleInfo * p = (ClientHandleInfo *) qi->pvData;
	DWORD dwHandle = p->dwHandle;
	BYTE bSize = p->account_index;

	delete p;

	if (msg->Get()->uiNumRows > 0)
	{
		pkPeer->EncodeHeader(HEADER_DG_SAFEBOX_CHANGE_SIZE, dwHandle, sizeof(BYTE));
		pkPeer->EncodeBYTE(bSize);
	}
}

void CClientManager::QUERY_SAFEBOX_CHANGE_PASSWORD(CPeer * pkPeer, DWORD dwHandle, TSafeboxChangePasswordPacket * p)
{
	ClientHandleInfo * pi = new ClientHandleInfo(dwHandle);
	strlcpy(pi->safebox_password, p->szNewPassword, sizeof(pi->safebox_password));
	strlcpy(pi->login, p->szOldPassword, sizeof(pi->login));
	pi->account_id = p->dwID;

	char szQuery[QUERY_MAX_LEN];
	snprintf(szQuery, sizeof(szQuery), "SELECT password FROM safebox%s WHERE account_id=%u", GetTablePostfix(), p->dwID);

	CDBManager::instance().ReturnQuery(szQuery, QID_SAFEBOX_CHANGE_PASSWORD, pkPeer->GetHandle(), pi);
}

void CClientManager::RESULT_SAFEBOX_CHANGE_PASSWORD(CPeer * pkPeer, SQLMsg * msg)
{
	CQueryInfo * qi = (CQueryInfo *) msg->pvUserData;
	ClientHandleInfo * p = (ClientHandleInfo *) qi->pvData;
	DWORD dwHandle = p->dwHandle;

	if (msg->Get()->uiNumRows > 0)
	{
		MYSQL_ROW row = mysql_fetch_row(msg->Get()->pSQLResult);

		if (row[0] && *row[0] && !strcasecmp(row[0], p->login) || (!row[0] || !*row[0]) && !strcmp("000000", p->login))
		{
			char szQuery[QUERY_MAX_LEN];
			char escape_pwd[64];
			CDBManager::instance().EscapeString(escape_pwd, p->safebox_password, strlen(p->safebox_password));

			snprintf(szQuery, sizeof(szQuery), "UPDATE safebox%s SET password='%s' WHERE account_id=%u", GetTablePostfix(), escape_pwd, p->account_id);

			CDBManager::instance().ReturnQuery(szQuery, QID_SAFEBOX_CHANGE_PASSWORD_SECOND, pkPeer->GetHandle(), p);
			return;
		}
	}

	delete p;

	// Wrong old password
	pkPeer->EncodeHeader(HEADER_DG_SAFEBOX_CHANGE_PASSWORD_ANSWER, dwHandle, sizeof(BYTE));
	pkPeer->EncodeBYTE(0);
}

void CClientManager::RESULT_SAFEBOX_CHANGE_PASSWORD_SECOND(CPeer * pkPeer, SQLMsg * msg)
{
	CQueryInfo * qi = (CQueryInfo *) msg->pvUserData;
	ClientHandleInfo * p = (ClientHandleInfo *) qi->pvData;
	DWORD dwHandle = p->dwHandle;
	delete p;

	pkPeer->EncodeHeader(HEADER_DG_SAFEBOX_CHANGE_PASSWORD_ANSWER, dwHandle, sizeof(BYTE));
	pkPeer->EncodeBYTE(1);
}

// MYSHOP_PRICE_LIST
void CClientManager::RESULT_PRICELIST_LOAD(CPeer* peer, SQLMsg* pMsg)
{
	TItemPricelistReqInfo* pReqInfo = (TItemPricelistReqInfo*)static_cast<CQueryInfo*>(pMsg->pvUserData)->pvData;

	//
	// DB ���� �ε��� ������ Cache �� ����
	//

	TItemPriceListTable table;
	table.dwOwnerID = pReqInfo->second;
	table.byCount = 0;
	
	MYSQL_ROW row;

	while ((row = mysql_fetch_row(pMsg->Get()->pSQLResult)))
	{
		str_to_number(table.aPriceInfo[table.byCount].dwVnum, row[0]);
		str_to_number(table.aPriceInfo[table.byCount].dwPrice, row[1]);
		table.byCount++;
	}

	PutItemPriceListCache(&table);

	//
	// �ε��� �����͸� Game server �� ����
	//

	TPacketMyshopPricelistHeader header;

	header.dwOwnerID = pReqInfo->second;
	header.byCount = table.byCount;

	size_t sizePriceListSize = sizeof(TItemPriceInfo) * header.byCount;

	peer->EncodeHeader(HEADER_DG_MYSHOP_PRICELIST_RES, pReqInfo->first, sizeof(header) + sizePriceListSize);
	peer->Encode(&header, sizeof(header));
	peer->Encode(table.aPriceInfo, sizePriceListSize);

	SPDLOG_DEBUG("Load MyShopPricelist handle[{}] pid[{}] count[{}]", pReqInfo->first, pReqInfo->second, header.byCount);

	delete pReqInfo;
}

void CClientManager::RESULT_PRICELIST_LOAD_FOR_UPDATE(SQLMsg* pMsg)
{
	TItemPriceListTable* pUpdateTable = (TItemPriceListTable*)static_cast<CQueryInfo*>(pMsg->pvUserData)->pvData;

	//
	// DB ���� �ε��� ������ Cache �� ����
	//

	TItemPriceListTable table;
	table.dwOwnerID = pUpdateTable->dwOwnerID;
	table.byCount = 0;
	
	MYSQL_ROW row;

	while ((row = mysql_fetch_row(pMsg->Get()->pSQLResult)))
	{
		str_to_number(table.aPriceInfo[table.byCount].dwVnum, row[0]);
		str_to_number(table.aPriceInfo[table.byCount].dwPrice, row[1]);
		table.byCount++;
	}

	PutItemPriceListCache(&table);

	// Update cache
	GetItemPriceListCache(pUpdateTable->dwOwnerID)->UpdateList(pUpdateTable);

	delete pUpdateTable;
}
// END_OF_MYSHOP_PRICE_LIST

void CClientManager::QUERY_SAFEBOX_SAVE(CPeer * pkPeer, TSafeboxTable * pTable)
{
	char szQuery[QUERY_MAX_LEN];

	snprintf(szQuery, sizeof(szQuery),
			"UPDATE safebox%s SET gold='%u' WHERE account_id=%u", 
			GetTablePostfix(), pTable->dwGold, pTable->dwID);

	CDBManager::instance().ReturnQuery(szQuery, QID_SAFEBOX_SAVE, pkPeer->GetHandle(), NULL);
}

void CClientManager::QUERY_EMPIRE_SELECT(CPeer * pkPeer, DWORD dwHandle, TEmpireSelectPacket * p)
{
	char szQuery[QUERY_MAX_LEN];

	snprintf(szQuery, sizeof(szQuery), "UPDATE player_index%s SET empire=%u WHERE id=%u", GetTablePostfix(), p->bEmpire, p->dwAccountID);
	delete CDBManager::instance().DirectQuery(szQuery);

	SPDLOG_DEBUG("EmpireSelect: {}", szQuery);
	{
		snprintf(szQuery, sizeof(szQuery),
				"SELECT pid1, pid2, pid3, pid4 FROM player_index%s WHERE id=%u", GetTablePostfix(), p->dwAccountID);

		std::unique_ptr<SQLMsg> pmsg(CDBManager::instance().DirectQuery(szQuery));

		SQLResult * pRes = pmsg->Get();

		if (pRes->uiNumRows)
		{
			SPDLOG_DEBUG("EMPIRE {}", pRes->uiNumRows);

			MYSQL_ROW row = mysql_fetch_row(pRes->pSQLResult);
			DWORD pids[3];

			UINT g_start_map[4] =
			{
				0,  // reserved
				1,  // �ż���
				21, // õ����
				41  // ���뱹
			};

			// FIXME share with game
			DWORD g_start_position[4][2]=
			{
				{      0,      0 },
				{ 469300, 964200 }, // �ż���
				{  55700, 157900 }, // õ����
				{ 969600, 278400 }  // ���뱹
			};

			for (int i = 0; i < 3; ++i)
			{
				str_to_number(pids[i], row[i]);
				SPDLOG_DEBUG("EMPIRE PIDS[{}]", pids[i]);

				if (pids[i])
				{
					SPDLOG_DEBUG("EMPIRE move to pid[{}] to villiage of {}, map_index {}",
							pids[i], p->bEmpire, g_start_map[p->bEmpire]);

					snprintf(szQuery, sizeof(szQuery), "UPDATE player%s SET map_index=%u,x=%u,y=%u WHERE id=%u", 
							GetTablePostfix(),
							g_start_map[p->bEmpire],
							g_start_position[p->bEmpire][0],
							g_start_position[p->bEmpire][1],
							pids[i]);

					std::unique_ptr<SQLMsg> pmsg2(CDBManager::instance().DirectQuery(szQuery));
				}
			}
		}
	}

	pkPeer->EncodeHeader(HEADER_DG_EMPIRE_SELECT, dwHandle, sizeof(BYTE));
	pkPeer->EncodeBYTE(p->bEmpire);
}

void CClientManager::QUERY_SETUP(CPeer * peer, DWORD dwHandle, const char * c_pData)
{
	TPacketGDSetup * p = (TPacketGDSetup *) c_pData;
	c_pData += sizeof(TPacketGDSetup);

	if (p->bAuthServer)
	{
		SPDLOG_DEBUG("AUTH_PEER ptr {}", (void*) peer);

		m_pkAuthPeer = peer;
		SendAllLoginToBilling();
		return;
	}

	peer->SetPublicIP(p->szPublicIP);
	peer->SetChannel(p->bChannel);
	peer->SetListenPort(p->wListenPort);
	peer->SetP2PPort(p->wP2PPort);
	peer->SetMaps(p->alMaps);

	//
	// � ���� � ������ �ִ��� ������
	//
	TMapLocation kMapLocations;

	strlcpy(kMapLocations.szHost, peer->GetPublicIP(), sizeof(kMapLocations.szHost));
	kMapLocations.wPort = peer->GetListenPort();
	memcpy(kMapLocations.alMaps, peer->GetMaps(), sizeof(kMapLocations.alMaps));

	BYTE bMapCount;

	std::vector<TMapLocation> vec_kMapLocations;

	if (peer->GetChannel() == 1)
	{
		for (itertype(m_peerList) i = m_peerList.begin(); i != m_peerList.end(); ++i)
		{
			CPeer * tmp = *i;

			if (tmp == peer)
				continue;

			if (!tmp->GetChannel())
				continue;

			if (tmp->GetChannel() == GUILD_WARP_WAR_CHANNEL || tmp->GetChannel() == peer->GetChannel())
			{
				TMapLocation kMapLocation2;
				strlcpy(kMapLocation2.szHost, tmp->GetPublicIP(), sizeof(kMapLocation2.szHost));
				kMapLocation2.wPort = tmp->GetListenPort();
				memcpy(kMapLocation2.alMaps, tmp->GetMaps(), sizeof(kMapLocation2.alMaps));
				vec_kMapLocations.push_back(kMapLocation2);

				tmp->EncodeHeader(HEADER_DG_MAP_LOCATIONS, 0, sizeof(BYTE) + sizeof(TMapLocation));
				bMapCount = 1;
				tmp->EncodeBYTE(bMapCount);
				tmp->Encode(&kMapLocations, sizeof(TMapLocation));
			}
		}
	}
	else if (peer->GetChannel() == GUILD_WARP_WAR_CHANNEL)
	{
		for (itertype(m_peerList) i = m_peerList.begin(); i != m_peerList.end(); ++i)
		{
			CPeer * tmp = *i;

			if (tmp == peer)
				continue;

			if (!tmp->GetChannel())
				continue;

			if (tmp->GetChannel() == 1 || tmp->GetChannel() == peer->GetChannel())
			{
				TMapLocation kMapLocation2;
				strlcpy(kMapLocation2.szHost, tmp->GetPublicIP(), sizeof(kMapLocation2.szHost));
				kMapLocation2.wPort = tmp->GetListenPort();
				memcpy(kMapLocation2.alMaps, tmp->GetMaps(), sizeof(kMapLocation2.alMaps));
				vec_kMapLocations.push_back(kMapLocation2);
			}

			tmp->EncodeHeader(HEADER_DG_MAP_LOCATIONS, 0, sizeof(BYTE) + sizeof(TMapLocation));
			bMapCount = 1;
			tmp->EncodeBYTE(bMapCount);
			tmp->Encode(&kMapLocations, sizeof(TMapLocation));
		}
	}
	else
	{
		for (itertype(m_peerList) i = m_peerList.begin(); i != m_peerList.end(); ++i)
		{
			CPeer * tmp = *i;

			if (tmp == peer)
				continue;

			if (!tmp->GetChannel())
				continue;

			if (tmp->GetChannel() == GUILD_WARP_WAR_CHANNEL || tmp->GetChannel() == peer->GetChannel())
			{
				TMapLocation kMapLocation2;

				strlcpy(kMapLocation2.szHost, tmp->GetPublicIP(), sizeof(kMapLocation2.szHost));
				kMapLocation2.wPort = tmp->GetListenPort();
				memcpy(kMapLocation2.alMaps, tmp->GetMaps(), sizeof(kMapLocation2.alMaps));

				vec_kMapLocations.push_back(kMapLocation2);
			}

			if (tmp->GetChannel() == peer->GetChannel())
			{
				tmp->EncodeHeader(HEADER_DG_MAP_LOCATIONS, 0, sizeof(BYTE) + sizeof(TMapLocation));
				bMapCount = 1;
				tmp->EncodeBYTE(bMapCount);
				tmp->Encode(&kMapLocations, sizeof(TMapLocation));
			}
		}
	}

	vec_kMapLocations.push_back(kMapLocations);

	peer->EncodeHeader(HEADER_DG_MAP_LOCATIONS, 0, sizeof(BYTE) + sizeof(TMapLocation) * vec_kMapLocations.size());
	bMapCount = vec_kMapLocations.size();
	peer->EncodeBYTE(bMapCount);
	peer->Encode(&vec_kMapLocations[0], sizeof(TMapLocation) * vec_kMapLocations.size());

	//
	// �¾� : ������ �Ǿ �ٸ� �Ǿ���� �����ϰ� �����. (P2P ���ؼ� ����)
	// 
	SPDLOG_DEBUG("SETUP: channel {} listen {} p2p {} count {}", peer->GetChannel(), p->wListenPort, p->wP2PPort, bMapCount);

	TPacketDGP2P p2pSetupPacket;
	p2pSetupPacket.wPort = peer->GetP2PPort();
	p2pSetupPacket.bChannel = peer->GetChannel();
	strlcpy(p2pSetupPacket.szHost, peer->GetPublicIP(), sizeof(p2pSetupPacket.szHost));

	for (itertype(m_peerList) i = m_peerList.begin(); i != m_peerList.end();++i)
	{
		CPeer * tmp = *i;

		if (tmp == peer)
			continue;

		// ä���� 0�̶�� ���� SETUP ��Ŷ�� ���� ���� �Ǿ� �Ǵ� auth��� ������ �� ����
		if (0 == tmp->GetChannel())
			continue;

		tmp->EncodeHeader(HEADER_DG_P2P, 0, sizeof(TPacketDGP2P));
		tmp->Encode(&p2pSetupPacket, sizeof(TPacketDGP2P));
	}

	//
	// �α��� �� �������� ������
	//
	TPacketLoginOnSetup * pck = (TPacketLoginOnSetup *) c_pData;;
	std::vector<TPacketBillingRepair> vec_repair;

	for (DWORD c = 0; c < p->dwLoginCount; ++c, ++pck)
	{
		CLoginData * pkLD = new CLoginData;

		pkLD->SetKey(pck->dwLoginKey);
		pkLD->SetClientKey(pck->adwClientKey);
		pkLD->SetIP(pck->szHost);

		TAccountTable & r = pkLD->GetAccountRef();

		r.id = pck->dwID;
		trim_and_lower(pck->szLogin, r.login, sizeof(r.login));
		strlcpy(r.social_id, pck->szSocialID, sizeof(r.social_id));
		strlcpy(r.passwd, "TEMP", sizeof(r.passwd));

		InsertLoginData(pkLD);

		if (InsertLogonAccount(pck->szLogin, peer->GetHandle(), pck->szHost))
		{
			SPDLOG_DEBUG("SETUP: login {} {} login_key {} host {}", pck->dwID, pck->szLogin, pck->dwLoginKey, pck->szHost);
			pkLD->SetPlay(true);

			if (m_pkAuthPeer)
			{
				TPacketBillingRepair pck_repair;
				pck_repair.dwLoginKey = pkLD->GetKey();
				strlcpy(pck_repair.szLogin, pck->szLogin, sizeof(pck_repair.szLogin));
				strlcpy(pck_repair.szHost, pck->szHost, sizeof(pck_repair.szHost));
				vec_repair.push_back(pck_repair);
			}
		}
		else
			SPDLOG_DEBUG("SETUP: login_fail {} {} login_key {}", pck->dwID, pck->szLogin, pck->dwLoginKey);
	}

	if (m_pkAuthPeer && !vec_repair.empty())
	{
		SPDLOG_DEBUG("REPAIR size {}", vec_repair.size());

		m_pkAuthPeer->EncodeHeader(HEADER_DG_BILLING_REPAIR, 0, sizeof(DWORD) + sizeof(TPacketBillingRepair) * vec_repair.size());
		m_pkAuthPeer->EncodeDWORD(vec_repair.size());
		m_pkAuthPeer->Encode(&vec_repair[0], sizeof(TPacketBillingRepair) * vec_repair.size());
	}

	SendPartyOnSetup(peer);
	CGuildManager::instance().OnSetup(peer);
	CPrivManager::instance().SendPrivOnSetup(peer);
	SendEventFlagsOnSetup(peer);
	marriage::CManager::instance().OnSetup(peer);
}

void CClientManager::QUERY_ITEM_FLUSH(CPeer * pkPeer, const char * c_pData)
{
	DWORD dwID = *(DWORD *) c_pData;

    SPDLOG_TRACE("HEADER_GD_ITEM_FLUSH: {}", dwID);

	CItemCache * c = GetItemCache(dwID);

	if (c)
		c->Flush();
}

void CClientManager::QUERY_ITEM_SAVE(CPeer * pkPeer, const char * c_pData)
{
	TPlayerItem * p = (TPlayerItem *) c_pData;

	// â��� ĳ������ �ʰ�, ĳ���� �ִ� �͵� �������� �Ѵ�.
	// auction�� �� ��Ʈ�� Ÿ�� �ʾƾ� �Ѵ�. EnrollInAuction�� Ÿ���Ѵ�.

	if (p->window == SAFEBOX || p->window == MALL)
	{
		CItemCache * c = GetItemCache(p->id);

		if (c)
		{
			TItemCacheSetPtrMap::iterator it = m_map_pkItemCacheSetPtr.find(c->Get()->owner);

			if (it != m_map_pkItemCacheSetPtr.end())
			{
				SPDLOG_TRACE("ITEM_CACHE: safebox owner {} id {}", c->Get()->owner, c->Get()->id);

				it->second->erase(c);
			}

			m_map_itemCache.erase(p->id);

			delete c;
		}
		char szQuery[512];

		snprintf(szQuery, sizeof(szQuery), 
			"REPLACE INTO item%s (id, owner_id, window, pos, count, vnum, socket0, socket1, socket2, "
			"attrtype0, attrvalue0, "
			"attrtype1, attrvalue1, "
			"attrtype2, attrvalue2, "
			"attrtype3, attrvalue3, "
			"attrtype4, attrvalue4, "
			"attrtype5, attrvalue5, "
			"attrtype6, attrvalue6) "
			"VALUES(%u, %u, %d, %d, %u, %u, %d, %d, %d, %hd, %hd, %hd, %hd, %hd, %hd, %hd, %hd, %hd, %hd, %hd, %hd, %hd, %hd)",
			GetTablePostfix(),
			p->id,
			p->owner,
			p->window,
			p->pos,
			p->count,
			p->vnum,
			p->alSockets[0],
			p->alSockets[1],
			p->alSockets[2],
			p->aAttr[0].bType, p->aAttr[0].sValue,
			p->aAttr[1].bType, p->aAttr[1].sValue,
			p->aAttr[2].bType, p->aAttr[2].sValue,
			p->aAttr[3].bType, p->aAttr[3].sValue,
			p->aAttr[4].bType, p->aAttr[4].sValue,
			p->aAttr[5].bType, p->aAttr[5].sValue,
			p->aAttr[6].bType, p->aAttr[6].sValue);

		CDBManager::instance().ReturnQuery(szQuery, QID_ITEM_SAVE, pkPeer->GetHandle(), NULL);
	}
#ifdef __AUCTION__
	else if (p->window == AUCTION)
	{
		SPDLOG_ERROR("invalid window. how can you enter this route?");
		return ;
	}
#endif
	else
	{
		SPDLOG_TRACE("QUERY_ITEM_SAVE => PutItemCache() owner {} id {} vnum {} ", p->owner, p->id, p->vnum);

		PutItemCache(p);
	}
}

CClientManager::TItemCacheSet * CClientManager::GetItemCacheSet(DWORD pid)
{
	TItemCacheSetPtrMap::iterator it = m_map_pkItemCacheSetPtr.find(pid);

	if (it == m_map_pkItemCacheSetPtr.end())
		return NULL;

	return it->second;
}

void CClientManager::CreateItemCacheSet(DWORD pid)
{
	if (m_map_pkItemCacheSetPtr.find(pid) != m_map_pkItemCacheSetPtr.end())
		return;

	TItemCacheSet * pSet = new TItemCacheSet;
	m_map_pkItemCacheSetPtr.insert(TItemCacheSetPtrMap::value_type(pid, pSet));

    SPDLOG_TRACE("ITEM_CACHE: new cache {}", pid);
}

void CClientManager::FlushItemCacheSet(DWORD pid)
{
	TItemCacheSetPtrMap::iterator it = m_map_pkItemCacheSetPtr.find(pid);

	if (it == m_map_pkItemCacheSetPtr.end())
	{
		SPDLOG_DEBUG("FLUSH_ITEMCACHESET : No ItemCacheSet pid({})", pid);
		return;
	}

	TItemCacheSet * pSet = it->second;
	TItemCacheSet::iterator it_set = pSet->begin();

	while (it_set != pSet->end())
	{
		CItemCache * c = *it_set++;
		c->Flush();

		m_map_itemCache.erase(c->Get()->id);
		delete c;
	}

	pSet->clear();
	delete pSet;

	m_map_pkItemCacheSetPtr.erase(it);

    SPDLOG_TRACE("FLUSH_ITEMCACHESET : Deleted pid({})", pid);
}

CItemCache * CClientManager::GetItemCache(DWORD id)
{
	TItemCacheMap::iterator it = m_map_itemCache.find(id);

	if (it == m_map_itemCache.end())
		return NULL;

	return it->second;
}

void CClientManager::PutItemCache(TPlayerItem * pNew, bool bSkipQuery)
{       
	CItemCache * c;     

	c = GetItemCache(pNew->id);
	
	// ������ ���� ����
	if (!c)
	{
        SPDLOG_TRACE("ITEM_CACHE: PutItemCache ==> New CItemCache id{} vnum{} new owner{}", pNew->id, pNew->vnum, pNew->owner);

		c = new CItemCache;
		m_map_itemCache.insert(TItemCacheMap::value_type(pNew->id, c));
	}
	// ������
	else
	{
        SPDLOG_TRACE("ITEM_CACHE: PutItemCache ==> Have Cache");

		// �����ڰ� Ʋ����
		if (pNew->owner != c->Get()->owner)
		{
			// �̹� �� �������� ������ �־��� ������ ���� �������� �����Ѵ�.
			TItemCacheSetPtrMap::iterator it = m_map_pkItemCacheSetPtr.find(c->Get()->owner);

			if (it != m_map_pkItemCacheSetPtr.end())
			{
                SPDLOG_TRACE("ITEM_CACHE: delete owner {} id {} new owner {}", c->Get()->owner, c->Get()->id, pNew->owner);
				it->second->erase(c);
			}
		}
	}

	// ���ο� ���� ������Ʈ 
	c->Put(pNew, bSkipQuery);
	
	TItemCacheSetPtrMap::iterator it = m_map_pkItemCacheSetPtr.find(c->Get()->owner);

	if (it != m_map_pkItemCacheSetPtr.end())
	{
        SPDLOG_TRACE("ITEM_CACHE: save {} id {}", c->Get()->owner, c->Get()->id);
		it->second->insert(c);
	}
	else
	{
		// ���� �����ڰ� �����Ƿ� �ٷ� �����ؾ� ���� ������ �� �� SQL�� �����Ͽ�
		// ���� �� �����Ƿ� �ٷ� �����Ѵ�.
        SPDLOG_TRACE("ITEM_CACHE: direct save {} id {}", c->Get()->owner, c->Get()->id);

		c->OnFlush();
	}
}

bool CClientManager::DeleteItemCache(DWORD dwID)
{
	CItemCache * c = GetItemCache(dwID);

	if (!c)
		return false;

	c->Delete();
	return true;
}

// MYSHOP_PRICE_LIST
CItemPriceListTableCache* CClientManager::GetItemPriceListCache(DWORD dwID)
{
	TItemPriceListCacheMap::iterator it = m_mapItemPriceListCache.find(dwID);

	if (it == m_mapItemPriceListCache.end())
		return NULL;

	return it->second;
}

void CClientManager::PutItemPriceListCache(const TItemPriceListTable* pItemPriceList)
{
	CItemPriceListTableCache* pCache = GetItemPriceListCache(pItemPriceList->dwOwnerID);

	if (!pCache)
	{
		pCache = new CItemPriceListTableCache;
		m_mapItemPriceListCache.insert(TItemPriceListCacheMap::value_type(pItemPriceList->dwOwnerID, pCache));
	}

	pCache->Put(const_cast<TItemPriceListTable*>(pItemPriceList), true);
}

void CClientManager::UpdatePlayerCache()
{
	TPlayerTableCacheMap::iterator it = m_map_playerCache.begin();

	while (it != m_map_playerCache.end())
	{
		CPlayerTableCache * c = (it++)->second;

		if (c->CheckTimeout())
		{
            SPDLOG_TRACE("UPDATE : UpdatePlayerCache() ==> FlushPlayerCache {} {} ", c->Get(false)->id, c->Get(false)->name);

			c->Flush();

			// Item Cache�� ������Ʈ
			UpdateItemCacheSet(c->Get()->id);
		}
		else if (c->CheckFlushTimeout())
			c->Flush();
	}
}
// END_OF_MYSHOP_PRICE_LIST

void CClientManager::SetCacheFlushCountLimit(int iLimit)
{
	m_iCacheFlushCountLimit = std::max(10, iLimit);
	SPDLOG_DEBUG("CACHE_FLUSH_LIMIT_PER_SECOND: {}", m_iCacheFlushCountLimit);
}

void CClientManager::UpdateItemCache()
{
	if (m_iCacheFlushCount >= m_iCacheFlushCountLimit)
		return;

	TItemCacheMap::iterator it = m_map_itemCache.begin();

	while (it != m_map_itemCache.end())
	{
		CItemCache * c = (it++)->second;

		// �������� Flush�� �Ѵ�.
		if (c->CheckFlushTimeout())
		{
			SPDLOG_TRACE("UpdateItemCache ==> Flush() vnum {} id owner {}", c->Get()->vnum, c->Get()->id, c->Get()->owner);

			c->Flush();

			if (++m_iCacheFlushCount >= m_iCacheFlushCountLimit)
				break;
		}
	}
}

void CClientManager::UpdateItemPriceListCache()
{
	TItemPriceListCacheMap::iterator it = m_mapItemPriceListCache.begin();

	while (it != m_mapItemPriceListCache.end())
	{
		CItemPriceListTableCache* pCache = it->second;

		if (pCache->CheckFlushTimeout())
		{
			pCache->Flush();
			m_mapItemPriceListCache.erase(it++);
		}
		else
			++it;
	}
}

void CClientManager::QUERY_ITEM_DESTROY(CPeer * pkPeer, const char * c_pData)
{
	DWORD dwID = *(DWORD *) c_pData;
	c_pData += sizeof(DWORD);

	DWORD dwPID = *(DWORD *) c_pData;

	if (!DeleteItemCache(dwID))
	{
		char szQuery[64];
		snprintf(szQuery, sizeof(szQuery), "DELETE FROM item%s WHERE id=%u", GetTablePostfix(), dwID);

        SPDLOG_TRACE("HEADER_GD_ITEM_DESTROY: PID {} ID {}", dwPID, dwID);

		if (dwPID == 0) // �ƹ��� ���� ����� �����ٸ�, �񵿱� ����
			CDBManager::instance().AsyncQuery(szQuery);
		else
			CDBManager::instance().ReturnQuery(szQuery, QID_ITEM_DESTROY, pkPeer->GetHandle(), NULL);
	}
}

void CClientManager::QUERY_FLUSH_CACHE(CPeer * pkPeer, const char * c_pData)
{
	DWORD dwPID = *(DWORD *) c_pData;

	CPlayerTableCache * pkCache = GetPlayerCache(dwPID);

	if (!pkCache)
		return;

	SPDLOG_DEBUG("FLUSH_CACHE: {}", dwPID);

	pkCache->Flush();
	FlushItemCacheSet(dwPID);

	m_map_playerCache.erase(dwPID);
	delete pkCache;
}

void CClientManager::QUERY_SMS(CPeer * pkPeer, TPacketGDSMS * pack)
{
	char szQuery[QUERY_MAX_LEN];

	char szMsg[256+1];
	//unsigned int len = CDBManager::instance().EscapeString(szMsg, pack->szMsg, strlen(pack->szMsg), SQL_ACCOUNT);
	unsigned int len = CDBManager::instance().EscapeString(szMsg, pack->szMsg, strlen(pack->szMsg));
	szMsg[len] = '\0';

	snprintf(szQuery, sizeof(szQuery),
			"INSERT INTO sms_pool (server, sender, receiver, mobile, msg) VALUES(%d, '%s', '%s', '%s', '%s')",
			(m_iPlayerIDStart + 2) / 3, pack->szFrom, pack->szTo, pack->szMobile, szMsg);

	CDBManager::instance().AsyncQuery(szQuery);
}

void CClientManager::QUERY_RELOAD_PROTO()
{
	if (!InitializeTables())
	{
		SPDLOG_ERROR("QUERY_RELOAD_PROTO: cannot load tables");
		return;
	}

	for (TPeerList::iterator i = m_peerList.begin(); i != m_peerList.end(); ++i)
	{
		CPeer * tmp = *i;

		if (!tmp->GetChannel())
			continue;

		tmp->EncodeHeader(HEADER_DG_RELOAD_PROTO, 0, 
				sizeof(WORD) + sizeof(TSkillTable) * m_vec_skillTable.size() +
				sizeof(WORD) + sizeof(TBanwordTable) * m_vec_banwordTable.size() +
				sizeof(WORD) + sizeof(TItemTable) * m_vec_itemTable.size() +
				sizeof(WORD) + sizeof(TMobTable) * m_vec_mobTable.size());

		tmp->EncodeWORD(m_vec_skillTable.size());
		tmp->Encode(&m_vec_skillTable[0], sizeof(TSkillTable) * m_vec_skillTable.size());

		tmp->EncodeWORD(m_vec_banwordTable.size());
		tmp->Encode(&m_vec_banwordTable[0], sizeof(TBanwordTable) * m_vec_banwordTable.size());

		tmp->EncodeWORD(m_vec_itemTable.size());
		tmp->Encode(&m_vec_itemTable[0], sizeof(TItemTable) * m_vec_itemTable.size());

		tmp->EncodeWORD(m_vec_mobTable.size());
		tmp->Encode(&m_vec_mobTable[0], sizeof(TMobTable) * m_vec_mobTable.size());
	}
}

// ADD_GUILD_PRIV_TIME
/**
 * @version	05/06/08 Bang2ni - ���ӽð� �߰�
 */
void CClientManager::AddGuildPriv(TPacketGiveGuildPriv* p)
{
	CPrivManager::instance().AddGuildPriv(p->guild_id, p->type, p->value, p->duration_sec);
}

void CClientManager::AddEmpirePriv(TPacketGiveEmpirePriv* p)
{
	CPrivManager::instance().AddEmpirePriv(p->empire, p->type, p->value, p->duration_sec);
}
// END_OF_ADD_GUILD_PRIV_TIME

void CClientManager::AddCharacterPriv(TPacketGiveCharacterPriv* p)
{
	CPrivManager::instance().AddCharPriv(p->pid, p->type, p->value);
}

void CClientManager::MoneyLog(TPacketMoneyLog* p)
{
	CMoneyLog::instance().AddLog(p->type, p->vnum, p->gold);
}

CLoginData * CClientManager::GetLoginData(DWORD dwKey)
{
	TLoginDataByLoginKey::iterator it = m_map_pkLoginData.find(dwKey);

	if (it == m_map_pkLoginData.end())
		return NULL;

	return it->second;
}

CLoginData * CClientManager::GetLoginDataByLogin(const char * c_pszLogin)
{
	char szLogin[LOGIN_MAX_LEN + 1];
	trim_and_lower(c_pszLogin, szLogin, sizeof(szLogin));

	TLoginDataByLogin::iterator it = m_map_pkLoginDataByLogin.find(szLogin);

	if (it == m_map_pkLoginDataByLogin.end())
		return NULL;

	return it->second;
}

CLoginData * CClientManager::GetLoginDataByAID(DWORD dwAID)
{
	TLoginDataByAID::iterator it = m_map_pkLoginDataByAID.find(dwAID);

	if (it == m_map_pkLoginDataByAID.end())
		return NULL;

	return it->second;
}

void CClientManager::InsertLoginData(CLoginData * pkLD)
{
	char szLogin[LOGIN_MAX_LEN + 1];
	trim_and_lower(pkLD->GetAccountRef().login, szLogin, sizeof(szLogin));

	m_map_pkLoginData.insert(std::make_pair(pkLD->GetKey(), pkLD));
	m_map_pkLoginDataByLogin.insert(std::make_pair(szLogin, pkLD));
	m_map_pkLoginDataByAID.insert(std::make_pair(pkLD->GetAccountRef().id, pkLD));
}

void CClientManager::DeleteLoginData(CLoginData * pkLD)
{
	m_map_pkLoginData.erase(pkLD->GetKey());
	m_map_pkLoginDataByLogin.erase(pkLD->GetAccountRef().login);
	m_map_pkLoginDataByAID.erase(pkLD->GetAccountRef().id);

	if (m_map_kLogonAccount.find(pkLD->GetAccountRef().login) == m_map_kLogonAccount.end())
		delete pkLD;
	else
		pkLD->SetDeleted(true);
}

void CClientManager::QUERY_AUTH_LOGIN(CPeer * pkPeer, DWORD dwHandle, TPacketGDAuthLogin * p)
{
	SPDLOG_TRACE("QUERY_AUTH_LOGIN {} {} {}", p->dwID, p->dwLoginKey, p->szLogin);
	CLoginData * pkLD = GetLoginDataByLogin(p->szLogin);

	if (pkLD)
	{
		DeleteLoginData(pkLD);
	}

	BYTE bResult;

	if (GetLoginData(p->dwLoginKey))
	{
		SPDLOG_ERROR("LoginData already exist key {} login {}", p->dwLoginKey, p->szLogin);
		bResult = 0;

		pkPeer->EncodeHeader(HEADER_DG_AUTH_LOGIN, dwHandle, sizeof(BYTE));
		pkPeer->EncodeBYTE(bResult);
	}
	else
	{
		CLoginData * pkLD = new CLoginData;

		pkLD->SetKey(p->dwLoginKey);
		pkLD->SetClientKey(p->adwClientKey);
		pkLD->SetBillType(p->bBillType);
		pkLD->SetBillID(p->dwBillID);
		pkLD->SetPremium(p->iPremiumTimes);

		TAccountTable & r = pkLD->GetAccountRef();

		r.id = p->dwID;
		trim_and_lower(p->szLogin, r.login, sizeof(r.login));
		strlcpy(r.social_id, p->szSocialID, sizeof(r.social_id));
		strlcpy(r.passwd, "TEMP", sizeof(r.passwd));

		SPDLOG_DEBUG("AUTH_LOGIN id({}) login({}) social_id({}) login_key({}), client_key({} {} {} {})",
				p->dwID, p->szLogin, p->szSocialID, p->dwLoginKey,
				p->adwClientKey[0], p->adwClientKey[1], p->adwClientKey[2], p->adwClientKey[3]);

		bResult = 1;

		InsertLoginData(pkLD);

		pkPeer->EncodeHeader(HEADER_DG_AUTH_LOGIN, dwHandle, sizeof(BYTE));
		pkPeer->EncodeBYTE(bResult);
	}
}

void CClientManager::BillingExpire(TPacketBillingExpire * p)
{
	char key[LOGIN_MAX_LEN + 1];
	trim_and_lower(p->szLogin, key, sizeof(key));

	switch (p->bBillType)
	{
		case BILLING_IP_TIME:
		case BILLING_IP_DAY:
			{
				DWORD dwIPID = 0;
				str_to_number(dwIPID, p->szLogin);

				TLogonAccountMap::iterator it = m_map_kLogonAccount.begin();

				while (it != m_map_kLogonAccount.end())
				{
					CLoginData * pkLD = (it++)->second;

					if (pkLD->GetBillID() == dwIPID)
					{
						CPeer * pkPeer = GetPeer(pkLD->GetConnectedPeerHandle());

						if (pkPeer)
						{
							strlcpy(p->szLogin, pkLD->GetAccountRef().login, sizeof(p->szLogin));
							pkPeer->EncodeHeader(HEADER_DG_BILLING_EXPIRE, 0, sizeof(TPacketBillingExpire));
							pkPeer->Encode(p, sizeof(TPacketBillingExpire));
						}
					}
				}
			}
			break;

		case BILLING_TIME:
		case BILLING_DAY:
			{
				TLogonAccountMap::iterator it = m_map_kLogonAccount.find(key);

				if (it != m_map_kLogonAccount.end())
				{
					CLoginData * pkLD = it->second;

					CPeer * pkPeer = GetPeer(pkLD->GetConnectedPeerHandle());

					if (pkPeer)
					{
						pkPeer->EncodeHeader(HEADER_DG_BILLING_EXPIRE, 0, sizeof(TPacketBillingExpire));
						pkPeer->Encode(p, sizeof(TPacketBillingExpire));
					}
				}
			}
			break;
	}
}

void CClientManager::BillingCheck(const char * data)
{
	if (!m_pkAuthPeer)
		return;

	time_t curTime = GetCurrentTime();

	DWORD dwCount = *(DWORD *) data;
	data += sizeof(DWORD);

	std::vector<DWORD> vec;

	SPDLOG_DEBUG("BillingCheck: size {}", dwCount);

	for (DWORD i = 0; i < dwCount; ++i)
	{
		DWORD dwKey = *(DWORD *) data;
		data += sizeof(DWORD);

		SPDLOG_DEBUG("BillingCheck: {}", dwKey);

		TLoginDataByLoginKey::iterator it = m_map_pkLoginData.find(dwKey);

		if (it == m_map_pkLoginData.end())
		{
			SPDLOG_DEBUG("BillingCheck: key not exist: {}", dwKey);
			vec.push_back(dwKey);
		}
		else
		{
			CLoginData * pkLD = it->second;

			if (!pkLD->IsPlay() && curTime - pkLD->GetLastPlayTime() > 180)
			{
				SPDLOG_DEBUG("BillingCheck: not login: {}", dwKey);
				vec.push_back(dwKey);
			}
		}
	}

	m_pkAuthPeer->EncodeHeader(HEADER_DG_BILLING_CHECK, 0, sizeof(DWORD) + sizeof(DWORD) * vec.size());
	m_pkAuthPeer->EncodeDWORD(vec.size());

	if (!vec.empty())
		m_pkAuthPeer->Encode(&vec[0], sizeof(DWORD) * vec.size());
}

void CClientManager::GuildDepositMoney(TPacketGDGuildMoney* p)
{
	CGuildManager::instance().DepositMoney(p->dwGuild, p->iGold);
}

void CClientManager::GuildWithdrawMoney(CPeer* peer, TPacketGDGuildMoney* p)
{
	CGuildManager::instance().WithdrawMoney(peer, p->dwGuild, p->iGold);
}

void CClientManager::GuildWithdrawMoneyGiveReply(TPacketGDGuildMoneyWithdrawGiveReply* p)
{
	CGuildManager::instance().WithdrawMoneyReply(p->dwGuild, p->bGiveSuccess, p->iChangeGold);
}

void CClientManager::GuildWarBet(TPacketGDGuildWarBet * p)
{
	CGuildManager::instance().Bet(p->dwWarID, p->szLogin, p->dwGold, p->dwGuild);
}

void CClientManager::SendAllLoginToBilling()
{
	if (!m_pkAuthPeer)
		return;

	std::vector<TPacketBillingRepair> vec;
	TPacketBillingRepair p;

	TLogonAccountMap::iterator it = m_map_kLogonAccount.begin();

	while (it != m_map_kLogonAccount.end())
	{
		CLoginData * pkLD = (it++)->second;

		p.dwLoginKey = pkLD->GetKey();
		strlcpy(p.szLogin, pkLD->GetAccountRef().login, sizeof(p.szLogin));
		strlcpy(p.szHost, pkLD->GetIP(), sizeof(p.szHost));
		SPDLOG_DEBUG("SendAllLoginToBilling {} {}", pkLD->GetAccountRef().login, pkLD->GetIP());
		vec.push_back(p);
	}

	if (!vec.empty())
	{
		m_pkAuthPeer->EncodeHeader(HEADER_DG_BILLING_REPAIR, 0, sizeof(DWORD) + sizeof(TPacketBillingRepair) * vec.size());
		m_pkAuthPeer->EncodeDWORD(vec.size());
		m_pkAuthPeer->Encode(&vec[0], sizeof(TPacketBillingRepair) * vec.size());
	}
}

void CClientManager::SendLoginToBilling(CLoginData * pkLD, bool bLogin)
{
	if (!m_pkAuthPeer)
		return;

	TPacketBillingLogin p;

	p.dwLoginKey = pkLD->GetKey();
	p.bLogin = bLogin ? 1 : 0;

	DWORD dwCount = 1;
	m_pkAuthPeer->EncodeHeader(HEADER_DG_BILLING_LOGIN, 0, sizeof(DWORD) + sizeof(TPacketBillingLogin));
	m_pkAuthPeer->EncodeDWORD(dwCount);
	m_pkAuthPeer->Encode(&p, sizeof(TPacketBillingLogin));
}

void CClientManager::CreateObject(TPacketGDCreateObject * p)
{
	using namespace building;

	char szQuery[512];

	snprintf(szQuery, sizeof(szQuery),
			"INSERT INTO object%s (land_id, vnum, map_index, x, y, x_rot, y_rot, z_rot) VALUES(%u, %u, %d, %d, %d, %f, %f, %f)",
			GetTablePostfix(), p->dwLandID, p->dwVnum, p->lMapIndex, p->x, p->y, p->xRot, p->yRot, p->zRot);

	std::unique_ptr<SQLMsg> pmsg(CDBManager::instance().DirectQuery(szQuery));

	if (pmsg->Get()->uiInsertID == 0)
	{
		SPDLOG_ERROR("cannot insert object");
		return;
	}

	TObject * pkObj = new TObject;

	memset(pkObj, 0, sizeof(TObject));

	pkObj->dwID = pmsg->Get()->uiInsertID;
	pkObj->dwVnum = p->dwVnum;
	pkObj->dwLandID = p->dwLandID;
	pkObj->lMapIndex = p->lMapIndex;
	pkObj->x = p->x;
	pkObj->y = p->y;
	pkObj->xRot = p->xRot;
	pkObj->yRot = p->yRot;
	pkObj->zRot = p->zRot;
	pkObj->lLife = 0;

	ForwardPacket(HEADER_DG_CREATE_OBJECT, pkObj, sizeof(TObject));

	m_map_pkObjectTable.insert(std::make_pair(pkObj->dwID, pkObj));
}

void CClientManager::DeleteObject(DWORD dwID)
{
	char szQuery[128];

	snprintf(szQuery, sizeof(szQuery), "DELETE FROM object%s WHERE id=%u", GetTablePostfix(), dwID);

	std::unique_ptr<SQLMsg> pmsg(CDBManager::instance().DirectQuery(szQuery));

	if (pmsg->Get()->uiAffectedRows == 0 || pmsg->Get()->uiAffectedRows == (uint32_t)-1)
	{
		SPDLOG_ERROR("no object by id {}", dwID);
		return;
	}

	itertype(m_map_pkObjectTable) it = m_map_pkObjectTable.find(dwID);

	if (it != m_map_pkObjectTable.end())
	{
		delete it->second;
		m_map_pkObjectTable.erase(it);
	}

	ForwardPacket(HEADER_DG_DELETE_OBJECT, &dwID, sizeof(DWORD));
}

void CClientManager::UpdateLand(DWORD * pdw)
{
	DWORD dwID = pdw[0];
	DWORD dwGuild = pdw[1];

	building::TLand * p = &m_vec_kLandTable[0];

	DWORD i;

	for (i = 0; i < m_vec_kLandTable.size(); ++i, ++p)
	{
		if (p->dwID == dwID)
		{
			char buf[256];
			snprintf(buf, sizeof(buf), "UPDATE land%s SET guild_id=%u WHERE id=%u", GetTablePostfix(), dwGuild, dwID);
			CDBManager::instance().AsyncQuery(buf);

			p->dwGuildID = dwGuild;
			break;
		}
	}

	if (i < m_vec_kLandTable.size())
		ForwardPacket(HEADER_DG_UPDATE_LAND, p, sizeof(building::TLand));
}

void CClientManager::VCard(TPacketGDVCard * p)
{
	SPDLOG_DEBUG("VCARD: {} {} {} {} {}",
			p->dwID, p->szSellCharacter, p->szSellAccount, p->szBuyCharacter, p->szBuyAccount);

	m_queue_vcard.push(*p);
}

void CClientManager::VCardProcess()
{
	if (!m_pkAuthPeer)
		return;

	while (!m_queue_vcard.empty())
	{
		m_pkAuthPeer->EncodeHeader(HEADER_DG_VCARD, 0, sizeof(TPacketGDVCard));
		m_pkAuthPeer->Encode(&m_queue_vcard.front(), sizeof(TPacketGDVCard));

		m_queue_vcard.pop();
	}
}

// BLOCK_CHAT
void CClientManager::BlockChat(TPacketBlockChat* p)
{
	char szQuery[256];

	if (g_stLocale == "sjis")
		snprintf(szQuery, sizeof(szQuery), "SELECT id FROM player%s WHERE name = '%s' collate sjis_japanese_ci", GetTablePostfix(), p->szName);
	else
		snprintf(szQuery, sizeof(szQuery), "SELECT id FROM player%s WHERE name = '%s'", GetTablePostfix(), p->szName);
	std::unique_ptr<SQLMsg> pmsg(CDBManager::instance().DirectQuery(szQuery));
	SQLResult * pRes = pmsg->Get();

	if (pRes->uiNumRows)
	{
		MYSQL_ROW row = mysql_fetch_row(pRes->pSQLResult);
		DWORD pid = strtoul(row[0], NULL, 10);

		TPacketGDAddAffect pa;
		pa.dwPID = pid;
		pa.elem.dwType = 223;
		pa.elem.bApplyOn = 0;
		pa.elem.lApplyValue = 0;
		pa.elem.dwFlag = 0;
		pa.elem.lDuration = p->lDuration;
		pa.elem.lSPCost = 0;
		QUERY_ADD_AFFECT(NULL, &pa);
	}
	else
	{
		// cannot find user with that name
	}
}
// END_OF_BLOCK_CHAT

void CClientManager::MarriageAdd(TPacketMarriageAdd * p)
{
	SPDLOG_DEBUG("MarriageAdd {} {} {} {}", p->dwPID1, p->dwPID2, p->szName1, p->szName2);
	marriage::CManager::instance().Add(p->dwPID1, p->dwPID2, p->szName1, p->szName2);
}

void CClientManager::MarriageUpdate(TPacketMarriageUpdate * p)
{
	SPDLOG_DEBUG("MarriageUpdate PID:{} {} LP:{} ST:{}", p->dwPID1, p->dwPID2, p->iLovePoint, p->byMarried);
	marriage::CManager::instance().Update(p->dwPID1, p->dwPID2, p->iLovePoint, p->byMarried);
}

void CClientManager::MarriageRemove(TPacketMarriageRemove * p)
{
	SPDLOG_DEBUG("MarriageRemove {} {}", p->dwPID1, p->dwPID2);
	marriage::CManager::instance().Remove(p->dwPID1, p->dwPID2);
}

void CClientManager::WeddingRequest(TPacketWeddingRequest * p)
{
	SPDLOG_DEBUG("WeddingRequest {} {}", p->dwPID1, p->dwPID2);
	ForwardPacket(HEADER_DG_WEDDING_REQUEST, p, sizeof(TPacketWeddingRequest));
	//marriage::CManager::instance().RegisterWedding(p->dwPID1, p->szName1, p->dwPID2, p->szName2);
}

void CClientManager::WeddingReady(TPacketWeddingReady * p)
{
	SPDLOG_DEBUG("WeddingReady {} {}", p->dwPID1, p->dwPID2);
	ForwardPacket(HEADER_DG_WEDDING_READY, p, sizeof(TPacketWeddingReady));
	marriage::CManager::instance().ReadyWedding(p->dwMapIndex, p->dwPID1, p->dwPID2);
}

void CClientManager::WeddingEnd(TPacketWeddingEnd * p)
{
	SPDLOG_DEBUG("WeddingEnd {} {}", p->dwPID1, p->dwPID2);
	marriage::CManager::instance().EndWedding(p->dwPID1, p->dwPID2);
}

//
// ĳ�ÿ� ���������� ������ ĳ�ø� ������Ʈ �ϰ� ĳ�ÿ� ���������� ���ٸ�
// �켱 ������ �����͸� �ε��� �ڿ� ������ ������ ĳ�ø� ����� ���� ���� ���������� ������Ʈ �Ѵ�.
//
void CClientManager::MyshopPricelistUpdate(const TPacketMyshopPricelistHeader* pPacket)
{
	if (pPacket->byCount > SHOP_PRICELIST_MAX_NUM)
	{
		SPDLOG_ERROR("count overflow!");
		return;
	}

	CItemPriceListTableCache* pCache = GetItemPriceListCache(pPacket->dwOwnerID);

	if (pCache)
	{
		TItemPriceListTable table;

		table.dwOwnerID = pPacket->dwOwnerID;
		table.byCount = pPacket->byCount;

		const TItemPriceInfo * pInfo = reinterpret_cast<const TItemPriceInfo*>(pPacket + sizeof(TPacketMyshopPricelistHeader));
		memcpy(table.aPriceInfo, pInfo, sizeof(TItemPriceInfo) * pPacket->byCount);

		pCache->UpdateList(&table);
	}
	else
	{
		TItemPriceListTable* pUpdateTable = new TItemPriceListTable;

		pUpdateTable->dwOwnerID = pPacket->dwOwnerID;
		pUpdateTable->byCount = pPacket->byCount;

		const TItemPriceInfo * pInfo = reinterpret_cast<const TItemPriceInfo*>(pPacket + sizeof(TPacketMyshopPricelistHeader));
		memcpy(pUpdateTable->aPriceInfo, pInfo, sizeof(TItemPriceInfo) * pPacket->byCount);

		char szQuery[QUERY_MAX_LEN];
		snprintf(szQuery, sizeof(szQuery), "SELECT item_vnum, price FROM myshop_pricelist%s WHERE owner_id=%u", GetTablePostfix(), pPacket->dwOwnerID);
		CDBManager::instance().ReturnQuery(szQuery, QID_ITEMPRICE_LOAD_FOR_UPDATE, 0, pUpdateTable);
	}
}

// MYSHOP_PRICE_LIST
// ĳ�õ� ���������� ������ ĳ�ø� �о� �ٷ� �����ϰ� ĳ�ÿ� ������ ������ DB �� ������ �Ѵ�.
//
void CClientManager::MyshopPricelistRequest(CPeer* peer, DWORD dwHandle, DWORD dwPlayerID)
{
	if (CItemPriceListTableCache* pCache = GetItemPriceListCache(dwPlayerID))
	{
		SPDLOG_DEBUG("Cache MyShopPricelist handle[{}] pid[{}]", dwHandle, dwPlayerID);

		TItemPriceListTable* pTable = pCache->Get(false);

		TPacketMyshopPricelistHeader header =
		{
			pTable->dwOwnerID,
			pTable->byCount
		};

		size_t sizePriceListSize = sizeof(TItemPriceInfo) * pTable->byCount;

		peer->EncodeHeader(HEADER_DG_MYSHOP_PRICELIST_RES, dwHandle, sizeof(header) + sizePriceListSize);
		peer->Encode(&header, sizeof(header));
		peer->Encode(pTable->aPriceInfo, sizePriceListSize);

	}
	else
	{
		SPDLOG_DEBUG("Query MyShopPricelist handle[{}] pid[{}]", dwHandle, dwPlayerID);

		char szQuery[QUERY_MAX_LEN];
		snprintf(szQuery, sizeof(szQuery), "SELECT item_vnum, price FROM myshop_pricelist%s WHERE owner_id=%u", GetTablePostfix(), dwPlayerID);
		CDBManager::instance().ReturnQuery(szQuery, QID_ITEMPRICE_LOAD, peer->GetHandle(), new TItemPricelistReqInfo(dwHandle, dwPlayerID));
	}
}
// END_OF_MYSHOP_PRICE_LIST

void CPacketInfo::Add(int header)
{
	itertype(m_map_info) it = m_map_info.find(header);

	if (it == m_map_info.end())
		m_map_info.insert(std::map<int, int>::value_type(header, 1));
	else
		++it->second;
}

void CPacketInfo::Reset()
{
	m_map_info.clear();
}

void CClientManager::ProcessPackets(CPeer * peer)
{
	BYTE		header;
	DWORD		dwHandle;
	DWORD		dwLength;
	const char * data = NULL;
	int			i = 0;
	int			iCount = 0;

	while (peer->PeekPacket(i, header, dwHandle, dwLength, &data))
	{
		m_bLastHeader = header;
		++iCount;

		switch (header)
		{
			case HEADER_GD_BOOT:
				QUERY_BOOT(peer, (TPacketGDBoot *) data);
				break;

			case HEADER_GD_HAMMER_OF_TOR:
				break;

			case HEADER_GD_LOGIN_BY_KEY:
				QUERY_LOGIN_BY_KEY(peer, dwHandle, (TPacketGDLoginByKey *) data);
				break;

			case HEADER_GD_LOGOUT:
				//SPDLOG_DEBUG("HEADER_GD_LOGOUT (handle: {} length: {})", dwHandle, dwLength);
				QUERY_LOGOUT(peer, dwHandle, data);
				break;

			case HEADER_GD_PLAYER_LOAD:
				SPDLOG_TRACE("HEADER_GD_PLAYER_LOAD (handle: {} length: {})", dwHandle, dwLength);
				QUERY_PLAYER_LOAD(peer, dwHandle, (TPlayerLoadPacket *) data);
				break;

			case HEADER_GD_PLAYER_SAVE:
				SPDLOG_TRACE("HEADER_GD_PLAYER_SAVE (handle: {} length: {})", dwHandle, dwLength);
				QUERY_PLAYER_SAVE(peer, dwHandle, (TPlayerTable *) data);
				break;

			case HEADER_GD_PLAYER_CREATE:
				SPDLOG_DEBUG("HEADER_GD_PLAYER_CREATE (handle: {} length: {})", dwHandle, dwLength);
				__QUERY_PLAYER_CREATE(peer, dwHandle, (TPlayerCreatePacket *) data);
				SPDLOG_DEBUG("END");
				break;

			case HEADER_GD_PLAYER_DELETE:
				SPDLOG_TRACE("HEADER_GD_PLAYER_DELETE (handle: {} length: {})", dwHandle, dwLength);
				__QUERY_PLAYER_DELETE(peer, dwHandle, (TPlayerDeletePacket *) data);
				break;

			case HEADER_GD_PLAYER_COUNT:
				QUERY_PLAYER_COUNT(peer, (TPlayerCountPacket *) data);
				break;

			case HEADER_GD_QUEST_SAVE:
				SPDLOG_TRACE("HEADER_GD_QUEST_SAVE (handle: {} length: {})", dwHandle, dwLength);
				QUERY_QUEST_SAVE(peer, (TQuestTable *) data, dwLength);
				break;

			case HEADER_GD_SAFEBOX_LOAD:
				QUERY_SAFEBOX_LOAD(peer, dwHandle, (TSafeboxLoadPacket *) data, 0);
				break;

			case HEADER_GD_SAFEBOX_SAVE:
				SPDLOG_TRACE("HEADER_GD_SAFEBOX_SAVE (handle: {} length: {})", dwHandle, dwLength);
				QUERY_SAFEBOX_SAVE(peer, (TSafeboxTable *) data);
				break;

			case HEADER_GD_SAFEBOX_CHANGE_SIZE:
				QUERY_SAFEBOX_CHANGE_SIZE(peer, dwHandle, (TSafeboxChangeSizePacket *) data);
				break;

			case HEADER_GD_SAFEBOX_CHANGE_PASSWORD:
				QUERY_SAFEBOX_CHANGE_PASSWORD(peer, dwHandle, (TSafeboxChangePasswordPacket *) data);
				break;

			case HEADER_GD_MALL_LOAD:
				QUERY_SAFEBOX_LOAD(peer, dwHandle, (TSafeboxLoadPacket *) data, 1);
				break;

			case HEADER_GD_EMPIRE_SELECT:
				QUERY_EMPIRE_SELECT(peer, dwHandle, (TEmpireSelectPacket *) data);
				break;

			case HEADER_GD_SETUP:
				QUERY_SETUP(peer, dwHandle, data);
				break;

			case HEADER_GD_GUILD_CREATE:
				GuildCreate(peer, *(DWORD *) data);
				break;

			case HEADER_GD_GUILD_SKILL_UPDATE:
				GuildSkillUpdate(peer, (TPacketGuildSkillUpdate *) data);		
				break;

			case HEADER_GD_GUILD_EXP_UPDATE:
				GuildExpUpdate(peer, (TPacketGuildExpUpdate *) data);
				break;

			case HEADER_GD_GUILD_ADD_MEMBER:
				GuildAddMember(peer, (TPacketGDGuildAddMember*) data);
				break;

			case HEADER_GD_GUILD_REMOVE_MEMBER:
				GuildRemoveMember(peer, (TPacketGuild*) data);
				break;

			case HEADER_GD_GUILD_CHANGE_GRADE:
				GuildChangeGrade(peer, (TPacketGuild*) data);
				break;

			case HEADER_GD_GUILD_CHANGE_MEMBER_DATA:
				GuildChangeMemberData(peer, (TPacketGuildChangeMemberData*) data);
				break;

			case HEADER_GD_GUILD_DISBAND:
				GuildDisband(peer, (TPacketGuild*) data);
				break;

			case HEADER_GD_GUILD_WAR:
				GuildWar(peer, (TPacketGuildWar*) data);
				break;

			case HEADER_GD_GUILD_WAR_SCORE:
				GuildWarScore(peer, (TPacketGuildWarScore*) data);
				break;

			case HEADER_GD_GUILD_CHANGE_LADDER_POINT:
				GuildChangeLadderPoint((TPacketGuildLadderPoint*) data);
				break;

			case HEADER_GD_GUILD_USE_SKILL:
				GuildUseSkill((TPacketGuildUseSkill*) data);
				break;

			case HEADER_GD_FLUSH_CACHE:
				QUERY_FLUSH_CACHE(peer, data);
				break;

			case HEADER_GD_ITEM_SAVE:
				QUERY_ITEM_SAVE(peer, data);
				break;

			case HEADER_GD_ITEM_DESTROY:
				QUERY_ITEM_DESTROY(peer, data);
				break;

			case HEADER_GD_ITEM_FLUSH:
				QUERY_ITEM_FLUSH(peer, data);
				break;

			case HEADER_GD_ADD_AFFECT:
				SPDLOG_TRACE("HEADER_GD_ADD_AFFECT");
				QUERY_ADD_AFFECT(peer, (TPacketGDAddAffect *) data);
				break;

			case HEADER_GD_REMOVE_AFFECT:
				SPDLOG_TRACE("HEADER_GD_REMOVE_AFFECT");
				QUERY_REMOVE_AFFECT(peer, (TPacketGDRemoveAffect *) data);
				break;

			case HEADER_GD_HIGHSCORE_REGISTER:
				QUERY_HIGHSCORE_REGISTER(peer, (TPacketGDHighscore *) data);
				break;

			case HEADER_GD_PARTY_CREATE:
				QUERY_PARTY_CREATE(peer, (TPacketPartyCreate*) data);
				break;

			case HEADER_GD_PARTY_DELETE:
				QUERY_PARTY_DELETE(peer, (TPacketPartyDelete*) data);
				break;

			case HEADER_GD_PARTY_ADD:
				QUERY_PARTY_ADD(peer, (TPacketPartyAdd*) data);
				break;

			case HEADER_GD_PARTY_REMOVE:
				QUERY_PARTY_REMOVE(peer, (TPacketPartyRemove*) data);
				break;

			case HEADER_GD_PARTY_STATE_CHANGE:
				QUERY_PARTY_STATE_CHANGE(peer, (TPacketPartyStateChange*) data);
				break;

			case HEADER_GD_PARTY_SET_MEMBER_LEVEL:
				QUERY_PARTY_SET_MEMBER_LEVEL(peer, (TPacketPartySetMemberLevel*) data);
				break;

			case HEADER_GD_RELOAD_PROTO:
				QUERY_RELOAD_PROTO();
				break;

			case HEADER_GD_CHANGE_NAME:
				QUERY_CHANGE_NAME(peer, dwHandle, (TPacketGDChangeName *) data);
				break;

			case HEADER_GD_SMS:
				QUERY_SMS(peer, (TPacketGDSMS *) data);
				break;

			case HEADER_GD_AUTH_LOGIN:
				QUERY_AUTH_LOGIN(peer, dwHandle, (TPacketGDAuthLogin *) data);
				break;

			case HEADER_GD_REQUEST_GUILD_PRIV:
				AddGuildPriv((TPacketGiveGuildPriv*)data);
				break;

			case HEADER_GD_REQUEST_EMPIRE_PRIV:
				AddEmpirePriv((TPacketGiveEmpirePriv*)data);
				break;

			case HEADER_GD_REQUEST_CHARACTER_PRIV:
				AddCharacterPriv((TPacketGiveCharacterPriv*) data);
				break;

			case HEADER_GD_MONEY_LOG:
				MoneyLog((TPacketMoneyLog*)data);
				break;

			case HEADER_GD_GUILD_DEPOSIT_MONEY:
				GuildDepositMoney((TPacketGDGuildMoney*)data);
				break;

			case HEADER_GD_GUILD_WITHDRAW_MONEY:
				GuildWithdrawMoney(peer, (TPacketGDGuildMoney*)data);
				break;

			case HEADER_GD_GUILD_WITHDRAW_MONEY_GIVE_REPLY:
				GuildWithdrawMoneyGiveReply((TPacketGDGuildMoneyWithdrawGiveReply*)data);
				break;

			case HEADER_GD_GUILD_WAR_BET:
				GuildWarBet((TPacketGDGuildWarBet *) data);
				break;

			case HEADER_GD_SET_EVENT_FLAG:
				SetEventFlag((TPacketSetEventFlag*) data);
				break;

			case HEADER_GD_BILLING_EXPIRE:
				BillingExpire((TPacketBillingExpire *) data);
				break;

			case HEADER_GD_BILLING_CHECK:
				BillingCheck(data);
				break;

			case HEADER_GD_CREATE_OBJECT:
				CreateObject((TPacketGDCreateObject *) data);
				break;

			case HEADER_GD_DELETE_OBJECT:
				DeleteObject(*(DWORD *) data);
				break;

			case HEADER_GD_UPDATE_LAND:
				UpdateLand((DWORD *) data);
				break;

			case HEADER_GD_VCARD:
				VCard((TPacketGDVCard *) data);
				break;

			case HEADER_GD_MARRIAGE_ADD:
				MarriageAdd((TPacketMarriageAdd *) data);
				break;

			case HEADER_GD_MARRIAGE_UPDATE:
				MarriageUpdate((TPacketMarriageUpdate *) data);
				break;

			case HEADER_GD_MARRIAGE_REMOVE:
				MarriageRemove((TPacketMarriageRemove *) data);
				break;

			case HEADER_GD_WEDDING_REQUEST:
				WeddingRequest((TPacketWeddingRequest *) data);
				break;

			case HEADER_GD_WEDDING_READY:
				WeddingReady((TPacketWeddingReady *) data);
				break;

			case HEADER_GD_WEDDING_END:
				WeddingEnd((TPacketWeddingEnd *) data);
				break;

				// BLOCK_CHAT
			case HEADER_GD_BLOCK_CHAT:
				BlockChat((TPacketBlockChat *) data);
				break;
				// END_OF_BLOCK_CHAT

				// MYSHOP_PRICE_LIST
			case HEADER_GD_MYSHOP_PRICELIST_UPDATE:
				MyshopPricelistUpdate((TPacketMyshopPricelistHeader*)data);
				break;

			case HEADER_GD_MYSHOP_PRICELIST_REQ:
				MyshopPricelistRequest(peer, dwHandle, *(DWORD*)data);
				break;
				// END_OF_MYSHOP_PRICE_LIST
		
				//RELOAD_ADMIN
			case HEADER_GD_RELOAD_ADMIN:
				ReloadAdmin(peer, (TPacketReloadAdmin*)data);
				break;
				//END_RELOAD_ADMIN

			case HEADER_GD_BREAK_MARRIAGE:
				BreakMarriage(peer, data);
				break;

			//MOANRCH
			case HEADER_GD_ELECT_MONARCH:
				Election(peer, dwHandle, data);
				break;

			case HEADER_GD_CANDIDACY:
				Candidacy(peer, dwHandle, data);
				break;

			case HEADER_GD_ADD_MONARCH_MONEY:
				AddMonarchMoney(peer, dwHandle, data);
				break;

			case HEADER_GD_DEC_MONARCH_MONEY:
				DecMonarchMoney(peer, dwHandle, data);
				break;

			case HEADER_GD_TAKE_MONARCH_MONEY:
				TakeMonarchMoney(peer, dwHandle, data);
				break;

			case HEADER_GD_COME_TO_VOTE:
				ComeToVote(peer, dwHandle, data);
				break;

			case HEADER_GD_RMCANDIDACY:		//< �ĺ� ���� (���)
				RMCandidacy(peer, dwHandle, data);
				break;

			case HEADER_GD_SETMONARCH:		///<���ּ��� (���)
				SetMonarch(peer, dwHandle, data);
				break;

			case HEADER_GD_RMMONARCH:		///<���ֻ���
				RMMonarch(peer, dwHandle, data);
				break;
			//END_MONARCH

			case HEADER_GD_CHANGE_MONARCH_LORD :
				ChangeMonarchLord(peer, dwHandle, (TPacketChangeMonarchLord*)data);
				break;

			case HEADER_GD_BLOCK_COUNTRY_IP:
				SPDLOG_DEBUG("HEADER_GD_BLOCK_COUNTRY_IP received");
				CBlockCountry::instance().SendBlockedCountryIp(peer);
				CBlockCountry::instance().SendBlockException(peer);
				break;

			case HEADER_GD_BLOCK_EXCEPTION:
				SPDLOG_DEBUG("HEADER_GD_BLOCK_EXCEPTION received");
				BlockException((TPacketBlockException*) data);
				break;

			case HEADER_GD_REQ_SPARE_ITEM_ID_RANGE :
				SendSpareItemIDRange(peer);
				break;

			case HEADER_GD_REQ_CHANGE_GUILD_MASTER :
				GuildChangeMaster((TPacketChangeGuildMaster*) data);
				break;

			case HEADER_GD_UPDATE_HORSE_NAME :
				UpdateHorseName((TPacketUpdateHorseName*) data, peer);
				break;

			case HEADER_GD_REQ_HORSE_NAME :
				AckHorseName(*(DWORD*)data, peer);
				break;

			case HEADER_GD_DC:
				DeleteLoginKey((TPacketDC*) data);
				break;

			case HEADER_GD_VALID_LOGOUT:
				ResetLastPlayerID((TPacketNeedLoginLogInfo*)data);
				break;

			case HEADER_GD_REQUEST_CHARGE_CASH:
				ChargeCash((TRequestChargeCash*)data);
				break;

			//delete gift notify icon

			case HEADER_GD_DELETE_AWARDID:
				DeleteAwardId((TPacketDeleteAwardID*) data);
				break;

			case HEADER_GD_UPDATE_CHANNELSTATUS:
				UpdateChannelStatus((SChannelStatus*) data);
				break;
			case HEADER_GD_REQUEST_CHANNELSTATUS:
				RequestChannelStatus(peer, dwHandle);
				break;
#ifdef __AUCTION__
			case HEADER_GD_COMMAND_AUCTION:
			{
				TPacketGDCommnadAuction* auction_data = (TPacketGDCommnadAuction*)data;
				
				switch (auction_data->get_cmd())
				{
				case AUCTION_ENR_AUC:
					EnrollInAuction (peer, dwHandle, (AuctionEnrollProductInfo*)data);
					break;
				case AUCTION_ENR_SALE:
					EnrollInSale (peer, dwHandle, (AuctionEnrollSaleInfo*)data);
					break;
				case AUCTION_ENR_WISH:
					EnrollInWish (peer, dwHandle, (AuctionEnrollWishInfo*)data);
					break;
				case AUCTION_BID:
					AuctionBid (peer, dwHandle, (AuctionBidInfo*)data);
					break;
				case AUCTION_IMME_PUR:
					AuctionImpur (peer, dwHandle, (AuctionImpurInfo*)data);
					break;
				case AUCTION_GET_AUC:
					AuctionGetAuctionedItem (peer, dwHandle, auction_data->get_item());
					break;
				case AUCTION_BUY_SOLD:
					AuctionBuySoldItem (peer, dwHandle, auction_data->get_item());
					break;
				case AUCTION_CANCEL_AUC:
					AuctionCancelAuction (peer, dwHandle, auction_data->get_item());
					break;
				case AUCTION_CANCEL_WISH:
					AuctionCancelWish (peer, dwHandle, auction_data->get_item());
					break;
				case AUCTION_CANCEL_SALE:
					AuctionCancelSale (peer, dwHandle, auction_data->get_item());
					break;
				case AUCTION_DELETE_AUCTION_ITEM:
					AuctionDeleteAuctionItem (peer, dwHandle, auction_data->get_item());
					break;
				case AUCTION_DELETE_SALE_ITEM:
					AuctionDeleteSaleItem (peer, dwHandle, auction_data->get_item());
					break;
				case AUCTION_REBID:
					AuctionReBid (peer, dwHandle, (AuctionBidInfo*)data);
					break;
//				case AUCTION_BID_CANCEL:
//					AuctionBidCancel (peer, dwHandle, data->get_item());
				default :
					break;
				}
			}
			break;
#endif
			default:					
				SPDLOG_ERROR("Unknown header (header: {} handle: {} length: {})", header, dwHandle, dwLength);
				break;
		}
	}

	peer->RecvEnd(i);
}

CPeer * CClientManager::AddPeer(bufferevent* bufev, sockaddr* addr)
{
	auto* pPeer = new CPeer;

	if (!pPeer->Accept(bufev, addr)) {
        delete pPeer;
        return nullptr;
    }

    m_peerList.push_front(pPeer);
    return pPeer;
}

void CClientManager::RemovePeer(CPeer * pPeer)
{
	if (m_pkAuthPeer == pPeer)
	{
		m_pkAuthPeer = NULL;
	}
	else
	{
		TLogonAccountMap::iterator it = m_map_kLogonAccount.begin();

		while (it != m_map_kLogonAccount.end())
		{
			CLoginData * pkLD = it->second;

			if (pkLD->GetConnectedPeerHandle() == pPeer->GetHandle())
			{
				if (pkLD->IsPlay())
				{
					pkLD->SetPlay(false);
					SendLoginToBilling(pkLD, false);
				}

				if (pkLD->IsDeleted())
				{
					SPDLOG_DEBUG("DELETING LoginData");
					delete pkLD;
				}

				m_map_kLogonAccount.erase(it++);
			}
			else
				++it;
		}
	}

	m_peerList.remove(pPeer);
	delete pPeer;
}

CPeer * CClientManager::GetPeer(IDENT ident)
{
	for (itertype(m_peerList) i = m_peerList.begin(); i != m_peerList.end();++i)
	{
		CPeer * tmp = *i;

		if (tmp->GetHandle() == ident)
			return tmp;
	}

	return NULL;
}

CPeer * CClientManager::GetAuthPeer()
{
    return m_pkAuthPeer;
}

CPeer * CClientManager::GetAnyPeer()
{
	if (m_peerList.empty())
		return NULL;

	return m_peerList.front();
}

// DB �Ŵ����� ���� ���� ����� ó���Ѵ�.
//
// @version	05/06/10 Bang2ni - �������� ���� ����(QID_ITEMPRICE_XXX) �߰�
int CClientManager::AnalyzeQueryResult(SQLMsg * msg)
{
	CQueryInfo * qi = (CQueryInfo *) msg->pvUserData;
	CPeer * peer = GetPeer(qi->dwIdent);

#ifdef _TEST
	if (qi->iType != QID_ITEM_AWARD_LOAD)
	SPDLOG_DEBUG("AnalyzeQueryResult {}", qi->iType);
#endif
	switch (qi->iType)
	{
		case QID_ITEM_AWARD_LOAD:
			ItemAwardManager::instance().Load(msg);
			delete qi;
			return true;

		case QID_GUILD_RANKING:
			CGuildManager::instance().ResultRanking(msg->Get()->pSQLResult);
			break;

			// MYSHOP_PRICE_LIST
		case QID_ITEMPRICE_LOAD_FOR_UPDATE:
			RESULT_PRICELIST_LOAD_FOR_UPDATE(msg);
			break;
			// END_OF_MYSHOP_PRICE_LIST
	}

	if (!peer)
	{	
		//SPDLOG_ERROR("CClientManager::AnalyzeQueryResult: peer not exist anymore. (ident: {})", qi->dwIdent);
		delete qi;
		return true;
	}

	switch (qi->iType)
	{
		case QID_PLAYER:
		case QID_ITEM:
		case QID_QUEST:
		case QID_AFFECT:
			RESULT_COMPOSITE_PLAYER(peer, msg, qi->iType);
			break;

		case QID_LOGIN:
			RESULT_LOGIN(peer, msg);
			break;

		case QID_SAFEBOX_LOAD:
			SPDLOG_DEBUG("QUERY_RESULT: HEADER_GD_SAFEBOX_LOAD");
			RESULT_SAFEBOX_LOAD(peer, msg);
			break;

		case QID_SAFEBOX_CHANGE_SIZE:
			SPDLOG_DEBUG("QUERY_RESULT: HEADER_GD_SAFEBOX_CHANGE_SIZE");
			RESULT_SAFEBOX_CHANGE_SIZE(peer, msg);
			break;

		case QID_SAFEBOX_CHANGE_PASSWORD:
			SPDLOG_DEBUG("QUERY_RESULT: HEADER_GD_SAFEBOX_CHANGE_PASSWORD {}", (void*) msg);
			RESULT_SAFEBOX_CHANGE_PASSWORD(peer, msg);
			break;

		case QID_SAFEBOX_CHANGE_PASSWORD_SECOND:
			SPDLOG_DEBUG("QUERY_RESULT: HEADER_GD_SAFEBOX_CHANGE_PASSWORD {}", (void*) msg);
			RESULT_SAFEBOX_CHANGE_PASSWORD_SECOND(peer, msg);
			break;

		case QID_HIGHSCORE_REGISTER:
			SPDLOG_DEBUG("QUERY_RESULT: HEADER_GD_HIGHSCORE_REGISTER {}", (void*) msg);
			RESULT_HIGHSCORE_REGISTER(peer, msg);
			break;

		case QID_SAFEBOX_SAVE:
		case QID_ITEM_SAVE:
		case QID_ITEM_DESTROY:
		case QID_QUEST_SAVE:
		case QID_PLAYER_SAVE:
		case QID_ITEM_AWARD_TAKEN:
			break;

			// PLAYER_INDEX_CREATE_BUG_FIX	
		case QID_PLAYER_INDEX_CREATE:
			RESULT_PLAYER_INDEX_CREATE(peer, msg);
			break;
			// END_PLAYER_INDEX_CREATE_BUG_FIX	

		case QID_PLAYER_DELETE:
			__RESULT_PLAYER_DELETE(peer, msg);
			break;

		case QID_LOGIN_BY_KEY:
			RESULT_LOGIN_BY_KEY(peer, msg);
			break;

			// MYSHOP_PRICE_LIST
		case QID_ITEMPRICE_LOAD:
			RESULT_PRICELIST_LOAD(peer, msg);
			break;
			// END_OF_MYSHOP_PRICE_LIST

		default:
			SPDLOG_DEBUG("CClientManager::AnalyzeQueryResult unknown query result type: {}, str: {}", qi->iType, msg->stQuery);
			break;
	}

	delete qi;
	return true;
}

void UsageLog()
{   
	FILE* fp = NULL;

	time_t      ct;
	char        *time_s;
	struct tm   lt;

	int         avg = g_dwUsageAvg / 3600; // 60 �� * 60 ��

	fp = fopen("usage.txt", "a+");

	if (!fp)
		return;

	ct = time(0);
	lt = *localtime(&ct);
	time_s = asctime(&lt);

	time_s[strlen(time_s) - 1] = '\0';

	fprintf(fp, "| %4d %-15.15s | %5d | %5u |", lt.tm_year + 1900, time_s + 4, avg, g_dwUsageMax);

	fprintf(fp, "\n");
	fclose(fp);

	g_dwUsageMax = g_dwUsageAvg = 0;
}

int CClientManager::Process()
{
	int pulses;

	if (!(pulses = thecore_idle()))
		return 0;

	while (pulses--)
	{
		++thecore_heart->pulse;

		/*
		//30�и��� ����
		if (((thecore_pulse() % (60 * 30 * 10)) == 0))
		{
			g_iPlayerCacheFlushSeconds = std::max(60, rand() % 180);
			g_iItemCacheFlushSeconds = std::max(60, rand() % 180);
			SPDLOG_DEBUG("[SAVE_TIME]Change saving time item {} player {}", g_iPlayerCacheFlushSeconds, g_iItemCacheFlushSeconds);
		}
		*/

		if (!(thecore_heart->pulse % thecore_heart->passes_per_sec))
		{
			if (g_test_server)
			{
			
				if (!(thecore_heart->pulse % thecore_heart->passes_per_sec * 10))	
					
				{
					SPDLOG_TRACE("[{:9}] return {}/{}/{}/{} async {}/{}/{}/{}",
							thecore_heart->pulse,
							CDBManager::instance().CountReturnQuery(SQL_PLAYER),
							CDBManager::instance().CountReturnResult(SQL_PLAYER),
							CDBManager::instance().CountReturnQueryFinished(SQL_PLAYER),
							CDBManager::instance().CountReturnCopiedQuery(SQL_PLAYER),
							CDBManager::instance().CountAsyncQuery(SQL_PLAYER),
							CDBManager::instance().CountAsyncResult(SQL_PLAYER),
							CDBManager::instance().CountAsyncQueryFinished(SQL_PLAYER),
							CDBManager::instance().CountAsyncCopiedQuery(SQL_PLAYER));

					if ((thecore_heart->pulse % 50) == 0)
                        SPDLOG_TRACE("[{:9}] return {}/{}/{} async {}/{}/{}",
								thecore_heart->pulse,
								CDBManager::instance().CountReturnQuery(SQL_PLAYER),
								CDBManager::instance().CountReturnResult(SQL_PLAYER),
								CDBManager::instance().CountReturnQueryFinished(SQL_PLAYER),
								CDBManager::instance().CountAsyncQuery(SQL_PLAYER),
								CDBManager::instance().CountAsyncResult(SQL_PLAYER),
								CDBManager::instance().CountAsyncQueryFinished(SQL_PLAYER));
				}
			}
			else
			{
                SPDLOG_TRACE("[{:9}] return {}/{}/{}/{} async {}/{}/{}/{}",
						thecore_heart->pulse,
						CDBManager::instance().CountReturnQuery(SQL_PLAYER),
						CDBManager::instance().CountReturnResult(SQL_PLAYER),
						CDBManager::instance().CountReturnQueryFinished(SQL_PLAYER),
						CDBManager::instance().CountReturnCopiedQuery(SQL_PLAYER),
						CDBManager::instance().CountAsyncQuery(SQL_PLAYER),
						CDBManager::instance().CountAsyncResult(SQL_PLAYER),
						CDBManager::instance().CountAsyncQueryFinished(SQL_PLAYER),
						CDBManager::instance().CountAsyncCopiedQuery(SQL_PLAYER));

                if ((thecore_heart->pulse % 50) == 0)
                    SPDLOG_TRACE("[{:9}] return {}/{}/{} async {}/{}/{}",
                        thecore_heart->pulse,
                        CDBManager::instance().CountReturnQuery(SQL_PLAYER),
                        CDBManager::instance().CountReturnResult(SQL_PLAYER),
                        CDBManager::instance().CountReturnQueryFinished(SQL_PLAYER),
                        CDBManager::instance().CountAsyncQuery(SQL_PLAYER),
                        CDBManager::instance().CountAsyncResult(SQL_PLAYER),
                        CDBManager::instance().CountAsyncQueryFinished(SQL_PLAYER));
            }

			CDBManager::instance().ResetCounter();

			DWORD dwCount = CClientManager::instance().GetUserCount();

			g_dwUsageAvg += dwCount;
			g_dwUsageMax = std::max(g_dwUsageMax, dwCount);

			memset(&thecore_profiler[0], 0, sizeof(thecore_profiler));

			if (!(thecore_heart->pulse % (thecore_heart->passes_per_sec * 3600)))
				UsageLog();

			m_iCacheFlushCount = 0;


			//�÷��̾� �÷���
			UpdatePlayerCache();
			//������ �÷���
			UpdateItemCache();
			//�α׾ƿ��� ó��- ĳ���� �÷���
			UpdateLogoutPlayer();

			// MYSHOP_PRICE_LIST
			UpdateItemPriceListCache();
			// END_OF_MYSHOP_PRICE_LIST

			CGuildManager::instance().Update();
			CPrivManager::instance().Update();
			marriage::CManager::instance().Update();
		}

		if (!(thecore_heart->pulse % (thecore_heart->passes_per_sec * 5)))
		{
			ItemAwardManager::instance().RequestLoad();
		}

		if (!(thecore_heart->pulse % (thecore_heart->passes_per_sec * 10)))
		{
			/*
			char buf[4096 + 1];
			int len
			itertype(g_query_info.m_map_info) it;

			/////////////////////////////////////////////////////////////////
			buf[0] = '\0';
			len = 0;

			it = g_query_info.m_map_info.begin();

			int count = 0;

			while (it != g_query_info.m_map_info.end())
			{
				len += snprintf(buf + len, sizeof(buf) - len, "%2d %3d\n", it->first, it->second);
				count += it->second;
				it++;
			}

			SPDLOG_TRACE("QUERY:\n{}-------------------- MAX : {}", buf, count);
			g_query_info.Reset();
			*/
            SPDLOG_TRACE("QUERY: MAIN[{}] ASYNC[{}]", g_query_count[0], g_query_count[1]);
            g_query_count[0] = 0;
            g_query_count[1] = 0;
            /////////////////////////////////////////////////////////////////

            /////////////////////////////////////////////////////////////////
            /*
            buf[0] = '\0';
            len = 0;

            it = g_item_info.m_map_info.begin();

            count = 0;
            while (it != g_item_info.m_map_info.end())
            {
                len += snprintf(buf + len, sizeof(buf) - len, "%5d %3d\n", it->first, it->second);
                count += it->second;
                it++;
            }

            SPDLOG_TRACE("ITEM:\n{}-------------------- MAX : {}", buf, count);
            g_item_info.Reset();
            */
            SPDLOG_TRACE("ITEM:{}", g_item_count);
            g_item_count = 0;
            /////////////////////////////////////////////////////////////////
        }

        if (!(thecore_heart->pulse % (thecore_heart->passes_per_sec * 60)))    // 60�ʿ� �ѹ�
        {
            // ����ũ �������� ���� �ð��� ������.
            CClientManager::instance().SendTime();
        }

        if (!(thecore_heart->pulse % (thecore_heart->passes_per_sec * 3600)))    // �ѽð��� �ѹ�
        {
            CMoneyLog::instance().Save();
		}
	}

    // Process network events
    event_base_loop(m_base, EVLOOP_NONBLOCK);

	VCardProcess();
	return 1;
}

DWORD CClientManager::GetUserCount()
{
	// �ܼ��� �α��� ī��Ʈ�� ����.. --;
	return m_map_kLogonAccount.size();
}

void CClientManager::SendAllGuildSkillRechargePacket()
{
	ForwardPacket(HEADER_DG_GUILD_SKILL_RECHARGE, NULL, 0);
}

void CClientManager::SendTime()
{
	time_t now = GetCurrentTime();
	ForwardPacket(HEADER_DG_TIME, &now, sizeof(time_t));
}

void CClientManager::ForwardPacket(BYTE header, const void* data, int size, BYTE bChannel, CPeer* except)
{
	for (itertype(m_peerList) it = m_peerList.begin(); it != m_peerList.end(); ++it)
	{
		CPeer * peer = *it;

		if (peer == except)
			continue;

		if (!peer->GetChannel())
			continue;

		if (bChannel && peer->GetChannel() != bChannel)
			continue;

		peer->EncodeHeader(header, 0, size);

		if (size > 0 && data)
			peer->Encode(data, size);
	}
}

void CClientManager::SendNotice(const char * c_pszFormat, ...)
{
	char szBuf[255+1];
	va_list args;

	va_start(args, c_pszFormat);
	int len = vsnprintf(szBuf, sizeof(szBuf), c_pszFormat, args);
	va_end(args);
	szBuf[len] = '\0';

	ForwardPacket(HEADER_DG_NOTICE, szBuf, len + 1);
}

time_t CClientManager::GetCurrentTime()
{
	return time(0);
}

// ITEM_UNIQUE_ID
bool CClientManager::InitializeNowItemID()
{
	DWORD dwMin, dwMax;

	//������ ID�� �ʱ�ȭ �Ѵ�.
	if (!CConfig::instance().GetTwoValue("ITEM_ID_RANGE", &dwMin, &dwMax))
	{
		SPDLOG_ERROR("conf.txt: Cannot find ITEM_ID_RANGE [start_item_id] [end_item_id]");
		return false;
	}

	SPDLOG_DEBUG("ItemRange From File {} ~ {} ", dwMin, dwMax);
	
	if (CItemIDRangeManager::instance().BuildRange(dwMin, dwMax, m_itemRange) == false)
	{
		SPDLOG_ERROR("Can not build ITEM_ID_RANGE");
		return false;
	}
	
	SPDLOG_DEBUG(" Init Success Start {} End {} Now {}", m_itemRange.dwMin, m_itemRange.dwMax, m_itemRange.dwUsableItemIDMin);

	return true;
}

DWORD CClientManager::GainItemID()
{
	return m_itemRange.dwUsableItemIDMin++;
}

DWORD CClientManager::GetItemID()
{
	return m_itemRange.dwUsableItemIDMin;
}
// ITEM_UNIQUE_ID_END
//BOOT_LOCALIZATION

bool CClientManager::InitializeLocalization() 
{
	char szQuery[512];	
	snprintf(szQuery, sizeof(szQuery), "SELECT mValue, mKey FROM locale");
	SQLMsg * pMsg = CDBManager::instance().DirectQuery(szQuery, SQL_COMMON);

	if (pMsg->Get()->uiNumRows == 0)
	{
		SPDLOG_ERROR("InitializeLocalization() ==> DirectQuery failed({})", szQuery);
		delete pMsg;
		return false;
	}

	SPDLOG_DEBUG("InitializeLocalization() - LoadLocaleTable(count:{})", pMsg->Get()->uiNumRows);

	m_vec_Locale.clear();

	MYSQL_ROW row = NULL;

	for (int n = 0; (row = mysql_fetch_row(pMsg->Get()->pSQLResult)) != NULL; ++n)
	{
		int col = 0;
		tLocale locale;

		strlcpy(locale.szValue, row[col++], sizeof(locale.szValue));
		strlcpy(locale.szKey, row[col++], sizeof(locale.szKey));

		//DB_NAME_COLUMN Setting		
		if (strcmp(locale.szKey, "LOCALE") == 0)
		{
			if (strcmp(locale.szValue, "cibn") == 0)	
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "gb2312");

				g_stLocale = "gb2312";		
				g_stLocaleNameColumn = "gb2312name";	
			}
			else if (strcmp(locale.szValue, "ymir") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "euckr";
				g_stLocaleNameColumn = "name";	
			}	
			else if (strcmp(locale.szValue, "japan") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "sjis");

				g_stLocale = "sjis";		
				g_stLocaleNameColumn = "locale_name";	
			}
			else if (strcmp(locale.szValue, "english") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "germany") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "latin1";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "france") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "latin1";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "italy") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "latin1";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "spain") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "latin1";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "uk") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "latin1";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "turkey") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "latin5";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "poland") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "latin2";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "portugal") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "latin1";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "hongkong") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "big5");

				g_stLocale = "big5";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "newcibn") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "gb2312");

				g_stLocale = "gb2312";
				g_stLocaleNameColumn = "gb2312name";
			}
			else if (strcmp(locale.szValue, "korea") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "euckr";
				g_stLocaleNameColumn = "name";
			}
			else if (strcmp(locale.szValue, "canada") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "latin1");

				g_stLocale = "latin1";
				g_stLocaleNameColumn = "gb2312name";
			}
			else if (strcmp(locale.szValue, "brazil") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "latin1");

				g_stLocale = "latin1";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "greek") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "latin1");

				g_stLocale = "greek";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "russia") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "latin1");

				g_stLocale = "cp1251";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "denmark") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "latin1");

				g_stLocale = "latin1";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "bulgaria") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "latin1");

				g_stLocale = "cp1251";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "croatia") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "latin1");

				g_stLocale = "cp1251";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "mexico") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "latin1";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "arabia") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "cp1256";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "czech") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "latin2";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "hungary") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "latin2";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "romania") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "latin2";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "netherlands") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "latin1";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "singapore") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "latin1");

				g_stLocale = "latin1";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "vietnam") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "latin1");

				g_stLocale = "latin1";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "thailand") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "latin1");

				g_stLocale = "latin1";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "usa") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "latin1");

				g_stLocale = "latin1";
				g_stLocaleNameColumn = "locale_name";
			}
			else if (strcmp(locale.szValue, "we_korea") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "euckr");

				g_stLocale = "euckr";
				g_stLocaleNameColumn = "name";
			}
			else if (strcmp(locale.szValue, "taiwan") == 0)
			{
				SPDLOG_DEBUG("locale[LOCALE] = {}", locale.szValue);

				if (g_stLocale != locale.szValue)
					SPDLOG_DEBUG("Changed g_stLocale {} to {}", g_stLocale.c_str(), "big5");
				g_stLocale = "big5";
				g_stLocaleNameColumn = "locale_name";
			}
			else
			{
				SPDLOG_ERROR("locale[LOCALE] = UNKNOWN({})", locale.szValue);
				exit(0);
			}

			CDBManager::instance().SetLocale(g_stLocale.c_str());
		}
		else if (strcmp(locale.szKey, "DB_NAME_COLUMN") == 0)
		{
			SPDLOG_DEBUG("locale[DB_NAME_COLUMN] = {}", locale.szValue);
			g_stLocaleNameColumn = locale.szValue;	
		}
		else
		{
			SPDLOG_DEBUG("locale[UNKNOWN_KEY({})] = {}", locale.szKey, locale.szValue);
		}
		m_vec_Locale.push_back(locale);
	}	

	delete pMsg;

	return true;
}
//END_BOOT_LOCALIZATION
//ADMIN_MANAGER

bool CClientManager::__GetAdminInfo(const char *szIP, std::vector<tAdminInfo> & rAdminVec)
{
	//szIP == NULL �ϰ��  ��缭���� ��� ������ ���´�.
	char szQuery[512];
	snprintf(szQuery, sizeof(szQuery),
			"SELECT mID,mAccount,mName,mContactIP,mServerIP,mAuthority FROM gmlist WHERE mServerIP='ALL' or mServerIP='%s'",
		   	szIP ? szIP : "ALL");

	SQLMsg * pMsg = CDBManager::instance().DirectQuery(szQuery, SQL_COMMON);

	if (pMsg->Get()->uiNumRows == 0)
	{
		SPDLOG_ERROR("__GetAdminInfo() ==> DirectQuery failed({})", szQuery);
		delete pMsg;
		return false;
	}

	MYSQL_ROW row;
	rAdminVec.reserve(pMsg->Get()->uiNumRows);

	while ((row = mysql_fetch_row(pMsg->Get()->pSQLResult)))
	{
		int idx = 0;
		tAdminInfo Info;

		str_to_number(Info.m_ID, row[idx++]);
		trim_and_lower(row[idx++], Info.m_szAccount, sizeof(Info.m_szAccount));
		strlcpy(Info.m_szName, row[idx++], sizeof(Info.m_szName));
		strlcpy(Info.m_szContactIP, row[idx++], sizeof(Info.m_szContactIP));
		strlcpy(Info.m_szServerIP, row[idx++], sizeof(Info.m_szServerIP));
		std::string stAuth = row[idx++];

		if (!stAuth.compare("IMPLEMENTOR"))
			Info.m_Authority = GM_IMPLEMENTOR;
		else if (!stAuth.compare("GOD"))
			Info.m_Authority = GM_GOD; 
		else if (!stAuth.compare("HIGH_WIZARD"))
			Info.m_Authority = GM_HIGH_WIZARD;
		else if (!stAuth.compare("LOW_WIZARD")) 
			Info.m_Authority = GM_LOW_WIZARD;
		else if (!stAuth.compare("WIZARD"))
			Info.m_Authority = GM_WIZARD;
		else 
			continue;

		rAdminVec.push_back(Info);

		SPDLOG_DEBUG("GM: PID {} Login {} Character {} ContactIP {} ServerIP {} Authority {}[{}]",
			   	Info.m_ID, Info.m_szAccount, Info.m_szName, Info.m_szContactIP, Info.m_szServerIP, Info.m_Authority, stAuth.c_str());
	}

	delete pMsg;

	return true;
}

bool CClientManager::__GetHostInfo(std::vector<std::string> & rIPVec)
{
	char szQuery[512];
	snprintf(szQuery, sizeof(szQuery), "SELECT mIP FROM gmhost");
	SQLMsg * pMsg = CDBManager::instance().DirectQuery(szQuery, SQL_COMMON);

	if (pMsg->Get()->uiNumRows == 0)
	{
		SPDLOG_ERROR("__GetHostInfo() ==> DirectQuery failed({})", szQuery);
		delete pMsg;
		return false;
	}

	rIPVec.reserve(pMsg->Get()->uiNumRows);

	MYSQL_ROW row; 

	while ((row = mysql_fetch_row(pMsg->Get()->pSQLResult)))
	{
		if (row[0] && *row[0])
		{
			rIPVec.push_back(row[0]);
			SPDLOG_DEBUG("GMHOST: {}", row[0]);
		}
	}

	delete pMsg;
	return true;
}
//END_ADMIN_MANAGER

void CClientManager::ReloadAdmin(CPeer*, TPacketReloadAdmin* p)
{
	std::vector<tAdminInfo> vAdmin;
	std::vector<std::string> vHost;
	
	__GetHostInfo(vHost);
	__GetAdminInfo(p->szIP, vAdmin);

	DWORD dwPacketSize = sizeof(WORD) + sizeof (WORD) + sizeof(tAdminInfo) * vAdmin.size() + 
		  sizeof(WORD) + sizeof(WORD) + 16 * vHost.size();	

	for (itertype(m_peerList) it = m_peerList.begin(); it != m_peerList.end(); ++it)
	{
		CPeer * peer = *it;

		if (!peer->GetChannel())
			continue;

		peer->EncodeHeader(HEADER_DG_RELOAD_ADMIN, 0, dwPacketSize);

		peer->EncodeWORD(16);
		peer->EncodeWORD(vHost.size());

		for (size_t n = 0; n < vHost.size(); ++n)
			peer->Encode(vHost[n].c_str(), 16);

		peer->EncodeWORD(sizeof(tAdminInfo));
		peer->EncodeWORD(vAdmin.size());

		for (size_t n = 0; n < vAdmin.size(); ++n)
			peer->Encode(&vAdmin[n], sizeof(tAdminInfo));
	}

	SPDLOG_DEBUG("ReloadAdmin End {}", p->szIP);
}

//BREAK_MARRIAGE
void CClientManager::BreakMarriage(CPeer * peer, const char * data)
{
	DWORD pid1, pid2;

	pid1 = *(int *) data;
	data += sizeof(int);

	pid2 = *(int *) data;
	data += sizeof(int);

	SPDLOG_DEBUG("Breaking off a marriage engagement! pid {} and pid {}", pid1, pid2);
	marriage::CManager::instance().Remove(pid1, pid2);
}
//END_BREAK_MARIIAGE

void CClientManager::UpdateItemCacheSet(DWORD pid)
{
	itertype(m_map_pkItemCacheSetPtr) it = m_map_pkItemCacheSetPtr.find(pid);

	if (it == m_map_pkItemCacheSetPtr.end())
	{
		SPDLOG_TRACE("UPDATE_ITEMCACHESET : UpdateItemCacheSet ==> No ItemCacheSet pid({})", pid);
		return;
	}

	TItemCacheSet * pSet = it->second;
	TItemCacheSet::iterator it_set = pSet->begin();

	while (it_set != pSet->end())
	{
		CItemCache * c = *it_set++;
		c->Flush();
	}

    SPDLOG_TRACE("UPDATE_ITEMCACHESET : UpdateItemCachsSet pid({})", pid);
}

void CClientManager::Election(CPeer * peer, DWORD dwHandle, const char* data)
{
	DWORD idx;
	DWORD selectingpid;

	idx = *(DWORD *) data;
	data += sizeof(DWORD);

	selectingpid = *(DWORD *) data;
	data += sizeof(DWORD);

	int Success = 0;

	if (!(Success = CMonarch::instance().VoteMonarch(selectingpid, idx)))
	{
		SPDLOG_TRACE("[MONARCH_VOTE] Failed {} {}", idx, selectingpid);
		peer->EncodeHeader(HEADER_DG_ELECT_MONARCH, dwHandle, sizeof(int));
		peer->Encode(&Success, sizeof(int));
		return;
	}
	else
	{
		SPDLOG_TRACE("[MONARCH_VOTE] Success {} {}", idx, selectingpid);
		peer->EncodeHeader(HEADER_DG_ELECT_MONARCH, dwHandle, sizeof(int));
		peer->Encode(&Success, sizeof(int));
		return;
	}

}
void CClientManager::Candidacy(CPeer *  peer, DWORD dwHandle, const char* data)
{
	DWORD pid;

	pid = *(DWORD *) data;
	data += sizeof(DWORD);

	if (!CMonarch::instance().AddCandidacy(pid, data))
	{
		SPDLOG_TRACE("[MONARCH_CANDIDACY] Failed {} {}", pid, data);

		peer->EncodeHeader(HEADER_DG_CANDIDACY, dwHandle, sizeof(int) + 32);
		peer->Encode(0, sizeof(int));
		peer->Encode(data, 32);
		return;
	}
	else
	{
		SPDLOG_TRACE("[MONARCH_CANDIDACY] Success {} {}", pid, data);

		for (itertype(m_peerList) it = m_peerList.begin(); it != m_peerList.end(); ++it)
		{
			CPeer * p = *it;

			if (!p->GetChannel())
				continue;

			if (0 && p->GetChannel() != 0)
				continue;

			if (p == peer)
			{	
				p->EncodeHeader(HEADER_DG_CANDIDACY, dwHandle, sizeof(int) + 32);
				p->Encode(&pid, sizeof(int));
				p->Encode(data, 32);
			}
			else
			{
				p->EncodeHeader(HEADER_DG_CANDIDACY, 0, sizeof(int) + 32);
				p->Encode(&pid, sizeof(int));
				p->Encode(data, 32);
			}
		}
	}
}

void CClientManager::AddMonarchMoney(CPeer * peer, DWORD dwHandle, const char * data)
{
	int Empire = *(int *) data;
	data += sizeof(int);

	int Money = *(int *) data;
	data += sizeof(int);

	SPDLOG_TRACE("[MONARCH] Add money Empire({}) Money({})", Empire, Money);

	CMonarch::instance().AddMoney(Empire, Money);
	
	for (itertype(m_peerList) it = m_peerList.begin(); it != m_peerList.end(); ++it)
	{
		CPeer * p = *it;

		if (!p->GetChannel())
			continue;

		if (p == peer)
		{	
			p->EncodeHeader(HEADER_DG_ADD_MONARCH_MONEY, dwHandle, sizeof(int) + sizeof(int));
			p->Encode(&Empire, sizeof(int));
			p->Encode(&Money, sizeof(int));
		}
		else
		{
			p->EncodeHeader(HEADER_DG_ADD_MONARCH_MONEY, 0, sizeof(int) + sizeof(int));
			p->Encode(&Empire, sizeof(int));
			p->Encode(&Money, sizeof(int));
		}

	}
}
void CClientManager::DecMonarchMoney(CPeer * peer, DWORD dwHandle, const char * data)
{
	int Empire = *(int *) data;
	data += sizeof(int);

	int Money = *(int *) data;
	data += sizeof(int);
		
	SPDLOG_TRACE("[MONARCH] Dec money Empire({}) Money({})", Empire, Money);

	CMonarch::instance().DecMoney(Empire, Money);
	
	for (itertype(m_peerList) it = m_peerList.begin(); it != m_peerList.end(); ++it)
	{
		CPeer * p = *it;

		if (!p->GetChannel())
			continue;

		if (p == peer)
		{	
			p->EncodeHeader(HEADER_DG_DEC_MONARCH_MONEY, dwHandle, sizeof(int) + sizeof(int));
			p->Encode(&Empire, sizeof(int));
			p->Encode(&Money, sizeof(int));
		}
		else
		{
			p->EncodeHeader(HEADER_DG_DEC_MONARCH_MONEY, 0, sizeof(int) + sizeof(int));
			p->Encode(&Empire, sizeof(int));
			p->Encode(&Money, sizeof(int));
		}
	}
}

void CClientManager::TakeMonarchMoney(CPeer * peer, DWORD dwHandle, const char * data)
{
	int Empire = *(int *) data;
	data += sizeof(int);

	DWORD pid = *(DWORD *) data;
	data += sizeof(int);

	int Money = *(int *) data;
	data += sizeof(int);

	SPDLOG_TRACE("[MONARCH] Take money Empire({}) Money({})", Empire, Money);

	if (CMonarch::instance().TakeMoney(Empire, pid, Money) == true)
	{
		peer->EncodeHeader(HEADER_DG_TAKE_MONARCH_MONEY, dwHandle, sizeof(int) + sizeof(int));
		peer->Encode(&Empire, sizeof(int));
		peer->Encode(&Money, sizeof(int));
	}
	else
	{
		Money = 0;
		peer->EncodeHeader(HEADER_DG_TAKE_MONARCH_MONEY, dwHandle, sizeof(int) + sizeof(int));
		peer->Encode(&Empire, sizeof(int));
		peer->Encode(&Money, sizeof(int));
	}
}

void CClientManager::ComeToVote(CPeer * peer, DWORD dwHandle, const char * data)
{
	CMonarch::instance().ElectMonarch();	
}

void CClientManager::RMCandidacy(CPeer * peer, DWORD dwHandle, const char * data)
{
	char szName[32];

	strlcpy(szName, data, sizeof(szName));
	SPDLOG_DEBUG("[MONARCH_GM] Remove candidacy name({})", szName);

	int iRet = CMonarch::instance().DelCandidacy(szName) ? 1 : 0;

	if (1 == iRet)
	{
		for (itertype(m_peerList) it = m_peerList.begin(); it != m_peerList.end(); ++it)
		{
			CPeer * p = *it;

			if (!p->GetChannel())
				continue;

			if (p == peer)
			{
				p->EncodeHeader(HEADER_DG_RMCANDIDACY, dwHandle, sizeof(int) + sizeof(szName));
				p->Encode(&iRet, sizeof(int));
				p->Encode(szName, sizeof(szName));
			}
			else
			{
				p->EncodeHeader(HEADER_DG_RMCANDIDACY, dwHandle, sizeof(int) + sizeof(szName));
				p->Encode(&iRet, sizeof(int));
				p->Encode(szName, sizeof(szName));
			}
		}
	}
	else
	{
		CPeer * p = peer;
		p->EncodeHeader(HEADER_DG_RMCANDIDACY, dwHandle, sizeof(int) + sizeof(szName));
		p->Encode(&iRet, sizeof(int));
		p->Encode(szName, sizeof(szName));
	}
}

void CClientManager::SetMonarch(CPeer * peer, DWORD dwHandle, const char * data)
{
	char szName[32];

	strlcpy(szName, data, sizeof(szName));

	SPDLOG_TRACE("[MONARCH_GM] Set Monarch name({})", szName);
	
	int iRet = CMonarch::instance().SetMonarch(szName) ? 1 : 0;

	if (1 == iRet)
	{
		for (itertype(m_peerList) it = m_peerList.begin(); it != m_peerList.end(); ++it)
		{
			CPeer * p = *it;

			if (!p->GetChannel())
				continue;

			if (p == peer)
			{
				p->EncodeHeader(HEADER_DG_RMCANDIDACY, dwHandle, sizeof(int) + sizeof(szName));
				p->Encode(&iRet, sizeof(int));
				p->Encode(szName, sizeof(szName));
			}
			else
			{
				p->EncodeHeader(HEADER_DG_RMCANDIDACY, dwHandle, sizeof(int) + sizeof(szName));
				p->Encode(&iRet, sizeof(int));
				p->Encode(szName, sizeof(szName));
			}
		}
	}
	else
	{
		CPeer * p = peer;
		p->EncodeHeader(HEADER_DG_RMCANDIDACY, dwHandle, sizeof(int) + sizeof(szName));
		p->Encode(&iRet, sizeof(int));
		p->Encode(szName, sizeof(szName));
	}
}

void CClientManager::RMMonarch(CPeer * peer, DWORD dwHandle, const char * data)
{
	char szName[32];

	strlcpy(szName, data, sizeof(szName));
	
	SPDLOG_TRACE("[MONARCH_GM] Remove Monarch name({})", szName);
	
	CMonarch::instance().DelMonarch(szName);
	
	int iRet = CMonarch::instance().DelMonarch(szName) ? 1 : 0;

	if (1 == iRet)
	{
		for (itertype(m_peerList) it = m_peerList.begin(); it != m_peerList.end(); ++it)
		{
			CPeer * p = *it;

			if (!p->GetChannel())
				continue;

			if (p == peer)
			{
				p->EncodeHeader(HEADER_DG_RMMONARCH, dwHandle, sizeof(int) + sizeof(szName));
				p->Encode(&iRet, sizeof(int));
				p->Encode(szName, sizeof(szName));
			}
			else
			{
				p->EncodeHeader(HEADER_DG_RMMONARCH, dwHandle, sizeof(int) + sizeof(szName));
				p->Encode(&iRet, sizeof(int));
				p->Encode(szName, sizeof(szName));
			}
		}
	}
	else
	{
		CPeer * p = peer;
		p->EncodeHeader(HEADER_DG_RMCANDIDACY, dwHandle, sizeof(int) + sizeof(szName));
		p->Encode(&iRet, sizeof(int));
		p->Encode(szName, sizeof(szName));
	}
}

void CClientManager::ChangeMonarchLord(CPeer * peer, DWORD dwHandle, TPacketChangeMonarchLord* info)
{
	char szQuery[1024];
	snprintf(szQuery, sizeof(szQuery), 
			"SELECT a.name, NOW() FROM player%s AS a, player_index%s AS b WHERE (a.account_id=b.id AND a.id=%u AND b.empire=%u) AND "
		    "(b.pid1=%u OR b.pid2=%u OR b.pid3=%u OR b.pid4=%u)", 
			GetTablePostfix(), GetTablePostfix(), info->dwPID, info->bEmpire,
		   	info->dwPID, info->dwPID, info->dwPID, info->dwPID);

	SQLMsg * pMsg = CDBManager::instance().DirectQuery(szQuery, SQL_PLAYER);

	if (pMsg->Get()->uiNumRows != 0)
	{
		TPacketChangeMonarchLordACK ack;
		ack.bEmpire = info->bEmpire;
		ack.dwPID = info->dwPID;
		
		MYSQL_ROW row = mysql_fetch_row(pMsg->Get()->pSQLResult);
		strlcpy(ack.szName, row[0], sizeof(ack.szName));
		strlcpy(ack.szDate, row[1], sizeof(ack.szDate));
		
		snprintf(szQuery, sizeof(szQuery), "UPDATE monarch SET pid=%u, windate=NOW() WHERE empire=%d", ack.dwPID, ack.bEmpire);
		SQLMsg* pMsg2 = CDBManager::instance().DirectQuery(szQuery, SQL_PLAYER);

		if (pMsg2->Get()->uiAffectedRows > 0)
		{
			CMonarch::instance().LoadMonarch();

			TMonarchInfo* newInfo = CMonarch::instance().GetMonarch();

			for (itertype(m_peerList) it = m_peerList.begin(); it != m_peerList.end(); it++)
			{
				CPeer* client = *it;

				client->EncodeHeader(HEADER_DG_CHANGE_MONARCH_LORD_ACK, 0, sizeof(TPacketChangeMonarchLordACK));
				client->Encode(&ack, sizeof(TPacketChangeMonarchLordACK));

				client->EncodeHeader(HEADER_DG_UPDATE_MONARCH_INFO, 0, sizeof(TMonarchInfo));
				client->Encode(newInfo, sizeof(TMonarchInfo));
			}
		}

		delete pMsg2;
	}

	delete pMsg;
}

void CClientManager::BlockException(TPacketBlockException *data)
{
	SPDLOG_DEBUG("[BLOCK_EXCEPTION] CMD({}) login({})", data->cmd, data->login);

	// save sql
	{
		char buf[1024];

		switch (data->cmd)
		{
			case BLOCK_EXCEPTION_CMD_ADD:
				snprintf(buf, sizeof(buf), "INSERT INTO block_exception VALUES('%s')", data->login);
				CDBManager::instance().AsyncQuery(buf, SQL_ACCOUNT);
				CBlockCountry::instance().AddBlockException(data->login);
				break;
			case BLOCK_EXCEPTION_CMD_DEL:
				snprintf(buf, sizeof(buf), "DELETE FROM block_exception VALUES('%s')", data->login);
				CDBManager::instance().AsyncQuery(buf, SQL_ACCOUNT);
				CBlockCountry::instance().DelBlockException(data->login);
				break;
			default:
				return;
		}

	}
	
	for (itertype(m_peerList) it = m_peerList.begin(); it != m_peerList.end(); ++it)
	{
		CPeer	*peer = *it;

		if (!peer->GetChannel())
			continue;

		CBlockCountry::instance().SendBlockExceptionOne(peer, data->login, data->cmd);
	}
}

void CClientManager::SendSpareItemIDRange(CPeer* peer)
{
	peer->SendSpareItemIDRange();
}

//
// Login Key�� �ʿ��� �����.
// 
void CClientManager::DeleteLoginKey(TPacketDC *data)
{
	char login[LOGIN_MAX_LEN+1] = {0};
	trim_and_lower(data->login, login, sizeof(login));

	CLoginData *pkLD = GetLoginDataByLogin(login);

	if (pkLD)
	{
		TLoginDataByLoginKey::iterator it = m_map_pkLoginData.find(pkLD->GetKey());

		if (it != m_map_pkLoginData.end())
			m_map_pkLoginData.erase(it);
	}
}

// delete gift notify icon
void CClientManager::DeleteAwardId(TPacketDeleteAwardID *data)
{
	//SPDLOG_DEBUG("data from game server arrived {}", data->dwID);
	std::map<DWORD, TItemAward *>::iterator it;
	it = ItemAwardManager::Instance().GetMapAward().find(data->dwID);
	if ( it != ItemAwardManager::Instance().GetMapAward().end() )
	{
		std::set<TItemAward *> & kSet = ItemAwardManager::Instance().GetMapkSetAwardByLogin()[it->second->szLogin];
		if(kSet.erase(it->second))
			SPDLOG_DEBUG("erase ItemAward id: {} from cache", data->dwID);
		ItemAwardManager::Instance().GetMapAward().erase(data->dwID);
	}
	else
	{
        SPDLOG_DEBUG("DELETE_AWARDID : could not find the id: {}", data->dwID);
	}

}

void CClientManager::UpdateChannelStatus(TChannelStatus* pData)
{
	TChannelStatusMap::iterator it = m_mChannelStatus.find(pData->nPort);
	if (it != m_mChannelStatus.end()) {
		it->second = pData->bStatus;
	}
	else {
		m_mChannelStatus.insert(TChannelStatusMap::value_type(pData->nPort, pData->bStatus));
	}
}

void CClientManager::RequestChannelStatus(CPeer* peer, DWORD dwHandle)
{
	const int nSize = m_mChannelStatus.size();
	peer->EncodeHeader(HEADER_DG_RESPOND_CHANNELSTATUS, dwHandle, sizeof(TChannelStatus)*nSize+sizeof(int));
	peer->Encode(&nSize, sizeof(int));
	for (TChannelStatusMap::iterator it = m_mChannelStatus.begin(); it != m_mChannelStatus.end(); it++) {
		peer->Encode(&it->first, sizeof(short));
		peer->Encode(&it->second, sizeof(BYTE));
	}
}

void CClientManager::ResetLastPlayerID(const TPacketNeedLoginLogInfo* data)
{
	CLoginData* pkLD = GetLoginDataByAID( data->dwPlayerID );

	if (NULL != pkLD)
	{
		pkLD->SetLastPlayerID( 0 );
	}
}

void CClientManager::ChargeCash(const TRequestChargeCash* packet)
{
	char szQuery[512];

	if (ERequestCharge_Cash == packet->eChargeType)
		sprintf(szQuery, "update account set `cash` = `cash` + %d where id = %d limit 1", packet->dwAmount, packet->dwAID);
	else if(ERequestCharge_Mileage == packet->eChargeType)
		sprintf(szQuery, "update account set `mileage` = `mileage` + %d where id = %d limit 1", packet->dwAmount, packet->dwAID);
	else
	{
		SPDLOG_ERROR("Invalid request charge type (type : {}, amount : {}, aid : {})", (int) packet->eChargeType, packet->dwAmount, packet->dwAID);
		return;
	}

	SPDLOG_ERROR("Request Charge (type : {}, amount : {}, aid : {})", (int) packet->eChargeType, packet->dwAmount, packet->dwAID);

	CDBManager::Instance().AsyncQuery(szQuery, SQL_ACCOUNT);
}

#ifdef __AUCTION__
void CClientManager::EnrollInAuction (CPeer * peer, DWORD owner_id, AuctionEnrollProductInfo* data)
{
	TPlayerTableCacheMap::iterator it = m_map_playerCache.find (owner_id);

	if (it == m_map_playerCache.end())
	{
		SPDLOG_ERROR("Invalid Player id {}. how can you get it?", owner_id);
		return;
	}
	CItemCache* c = GetItemCache (data->get_item_id());

	if (c == NULL)
	{
		SPDLOG_ERROR("Item {} doesn't exist in db cache.", data->get_item_id());
		return;
	}
	TPlayerItem* item = c->Get(false);

	if (item->owner != owner_id)
	{
		SPDLOG_ERROR("Player id {} doesn't have item {}.", owner_id, data->get_item_id());
		return;
	}
	// ���� �ð� + 24�ð� ��.
	time_t expired_time = time(0) + 24 * 60 * 60;
	TAuctionItemInfo auctioned_item_info (item->vnum, data->get_bid_price(), 
		data->get_impur_price(), owner_id, "", expired_time, data->get_item_id(), 0, data->get_empire());

	AuctionResult result = AuctionManager::instance().EnrollInAuction( c, auctioned_item_info );

	if (result <= AUCTION_FAIL)
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_ENR_AUC;
		enroll_result.target = data->get_item_id();
		enroll_result.result = result;
		peer->EncodeHeader(HEADER_DG_AUCTION_RESULT, owner_id, sizeof(TPacketDGResultAuction) + sizeof(TPlayerItem));
		peer->Encode(&enroll_result, sizeof(TPacketDGResultAuction));
		peer->Encode(c->Get(false), sizeof(TPlayerItem));
	}
	else
	{
		// ������ �ɽø� Auction�� ��� ������ ClientManager������ ����.
		TItemCacheSetPtrMap::iterator it = m_map_pkItemCacheSetPtr.find(item->owner);

		if (it != m_map_pkItemCacheSetPtr.end())
		{
			it->second->erase(c);
		}
		m_map_itemCache.erase(item->id);
		SPDLOG_DEBUG("Enroll In Auction Success. owner_id item_id {} {}", owner_id, item->id);

		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_ENR_AUC;
		enroll_result.target = data->get_item_id();
		enroll_result.result = result;
		for (TPeerList::iterator it = m_peerList.begin(); it != m_peerList.end(); it++)
		{
			(*it)->EncodeHeader(HEADER_DG_AUCTION_RESULT, owner_id, sizeof(TPacketDGResultAuction) + sizeof(TPlayerItem) + sizeof(TAuctionItemInfo));
			(*it)->Encode(&enroll_result, sizeof(TPacketDGResultAuction));
			(*it)->Encode(c->Get(false), sizeof(TPlayerItem));
			(*it)->Encode(&auctioned_item_info, sizeof(TAuctionItemInfo));
		}
	}

	return;
}

void CClientManager::EnrollInSale (CPeer * peer, DWORD owner_id, AuctionEnrollSaleInfo* data)
{
	TPlayerTableCacheMap::iterator it = m_map_playerCache.find (owner_id);

	if (it == m_map_playerCache.end())
	{
		SPDLOG_ERROR("Invalid Player id {}. how can you get it?", owner_id);
		return;
	}

	CPlayerTableCache* player_cache = it->second;
	TPlayerTable* player = player_cache->Get(false);

	CItemCache* c = GetItemCache (data->get_item_id());

	if (c == NULL)
	{
		SPDLOG_ERROR("Item {} doesn't exist in db cache.", data->get_item_id());
		return;
	}
	TPlayerItem* item = c->Get(false);

	if (item->owner != owner_id)
	{
		SPDLOG_ERROR("Player id {} doesn't have item {}.", owner_id, data->get_item_id());
		return;
	}
	// ���� �ð� + 24�ð� ��.
	time_t expired_time = time(0) + 24 * 60 * 60;
	TSaleItemInfo sold_item_info (item->vnum, data->get_sale_price(), 
		owner_id, player->name, data->get_item_id(), data->get_wisher_id());

	AuctionResult result = AuctionManager::instance().EnrollInSale( c, sold_item_info );

	if (result <= AUCTION_FAIL)
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_ENR_SALE;
		enroll_result.target = data->get_item_id();
		enroll_result.result = result;
		peer->EncodeHeader(HEADER_DG_AUCTION_RESULT, owner_id, sizeof(TPacketDGResultAuction) + sizeof(TPlayerItem));
		peer->Encode(&enroll_result, sizeof(TPacketDGResultAuction));
		peer->Encode(c->Get(false), sizeof(TPlayerItem));
	}
	else
	{
		// ������ �ɽø� Auction�� ��� ������ ClientManager������ ����.
		TItemCacheSetPtrMap::iterator it = m_map_pkItemCacheSetPtr.find(item->owner);

		if (it != m_map_pkItemCacheSetPtr.end())
		{
			it->second->erase(c);
		}
		m_map_itemCache.erase(item->id);
		SPDLOG_DEBUG("Enroll In Sale Success. owner_id item_id {} {}", owner_id, item->id);

		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_ENR_SALE;
		enroll_result.target = data->get_item_id();
		enroll_result.result = result;

		for (TPeerList::iterator it = m_peerList.begin(); it != m_peerList.end(); it++)
		{
			(*it)->EncodeHeader(HEADER_DG_AUCTION_RESULT, owner_id, sizeof(TPacketDGResultAuction) + sizeof(TPlayerItem) + sizeof(TSaleItemInfo));
			(*it)->Encode(&enroll_result, sizeof(TPacketDGResultAuction));
			(*it)->Encode(c->Get(false), sizeof(TPlayerItem));
			(*it)->Encode(&sold_item_info, sizeof(TSaleItemInfo));
		}
	}

	return;
}

void CClientManager::EnrollInWish (CPeer * peer, DWORD wisher_id, AuctionEnrollWishInfo* data)
{
	TPlayerTableCacheMap::iterator it = m_map_playerCache.find (wisher_id);

	if (it == m_map_playerCache.end())
	{
		SPDLOG_ERROR("Invalid Player id {}. how can you get it?", wisher_id);
		return;
	}

	CPlayerTableCache* player_cache = it->second;
	TPlayerTable* player = player_cache->Get(false);

	// ���� �ð� + 24�ð� ��.
	time_t expired_time = time(0) + 24 * 60 * 60;
	TWishItemInfo wished_item_info (data->get_item_num(), data->get_wish_price(), wisher_id, player->name, expired_time, data->get_empire());

	AuctionResult result = AuctionManager::instance().EnrollInWish ( wished_item_info );

	if (result <= AUCTION_FAIL)
	{
		SPDLOG_DEBUG("Enroll In Wish Success. wisher_id item_num {} {}", wisher_id, data->get_item_num());

		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_ENR_WISH;
		enroll_result.target = data->get_item_num();
		enroll_result.result = result;
		peer->EncodeHeader(HEADER_DG_AUCTION_RESULT, wisher_id, sizeof(TPacketDGResultAuction));
		peer->Encode(&enroll_result, sizeof(TPacketDGResultAuction));
	}
	else
	{
		SPDLOG_DEBUG("Enroll In Wish Fail. wisher_id item_num {} {}", wisher_id, data->get_item_num());

		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_ENR_WISH;
		enroll_result.target = data->get_item_num();
		enroll_result.result = result;

		for (TPeerList::iterator it = m_peerList.begin(); it != m_peerList.end(); it++)
		{
			(*it)->EncodeHeader(HEADER_DG_AUCTION_RESULT, wisher_id, sizeof(TPacketDGResultAuction) + sizeof(TWishItemInfo));
			(*it)->Encode(&enroll_result, sizeof(TPacketDGResultAuction));
			(*it)->Encode(&wished_item_info, sizeof(TWishItemInfo));
		}
	}

	return;
}

void CClientManager::AuctionBid (CPeer * peer, DWORD bidder_id, AuctionBidInfo* data)
{
	TPlayerTableCacheMap::iterator it = m_map_playerCache.find (bidder_id);

	if (it == m_map_playerCache.end())
	{
		SPDLOG_ERROR("Invalid Player id {}. how can you get it?", bidder_id);
		return;
	}

	CPlayerTableCache* player_cache = it->second;
	TPlayerTable* player = player_cache->Get(false);

	AuctionResult result = AuctionManager::instance().Bid(bidder_id, player->name, data->get_item_id(), data->get_bid_price());

	if (result == AUCTION_FAIL)
	{
		SPDLOG_DEBUG("Bid Fail. bidder_id item_id {} {}", bidder_id, data->get_item_id());
	}
	else
	{
		SPDLOG_DEBUG("Bid Success. bidder_id item_id {} {}", bidder_id, data->get_item_id());
	}

	if (result <= AUCTION_FAIL)
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_BID;
		enroll_result.target = data->get_bid_price();
		enroll_result.result = result;

		peer->EncodeHeader(HEADER_DG_AUCTION_RESULT, bidder_id, sizeof(TPacketDGResultAuction) + sizeof(AuctionBidInfo));
		peer->Encode(&enroll_result, sizeof(TPacketDGResultAuction));
	}
	else
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_BID;
		enroll_result.target = data->get_item_id();
		enroll_result.result = result;

		TAuctionItemInfo* auctioned_item_info = AuctionManager::instance().GetAuctionItemInfoCache(data->get_item_id())->Get(false);
		
		for (TPeerList::iterator it = m_peerList.begin(); it != m_peerList.end(); it++)
		{
			(*it)->EncodeHeader(HEADER_DG_AUCTION_RESULT, bidder_id, sizeof(TPacketDGResultAuction) + sizeof(TAuctionItemInfo));
			(*it)->Encode(&enroll_result, sizeof(TPacketDGResultAuction));
			(*it)->Encode(auctioned_item_info, sizeof(TAuctionItemInfo));
		}

	}
	return;
}

void CClientManager::AuctionImpur (CPeer * peer, DWORD purchaser_id, AuctionImpurInfo* data)
{
	TPlayerTableCacheMap::iterator it = m_map_playerCache.find (purchaser_id);

	if (it == m_map_playerCache.end())
	{
		SPDLOG_ERROR("Invalid Player id {}. how can you get it?", purchaser_id);
		return;
	}

	CPlayerTableCache* player_cache = it->second;
	TPlayerTable* player = player_cache->Get(false);

	AuctionResult result = AuctionManager::instance().Impur(purchaser_id, player->name, data->get_item_id());

	if (result == AUCTION_FAIL)
	{
		SPDLOG_DEBUG("Impur Fail. purchaser_id item_id {} {}", purchaser_id, data->get_item_id());
	}
	else
	{
		SPDLOG_DEBUG("Impur Success. purchaser_id item_id {} {}", purchaser_id, data->get_item_id());
	}

	if (result <= AUCTION_FAIL)
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_IMME_PUR;
		enroll_result.target = data->get_item_id();
		enroll_result.result = result;

		peer->EncodeHeader(HEADER_DG_AUCTION_RESULT, purchaser_id, sizeof(TPacketDGResultAuction));
		peer->Encode(&enroll_result, sizeof(TPacketDGResultAuction));
	}
	else
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_IMME_PUR;
		enroll_result.target = data->get_item_id();
		enroll_result.result = result;

		TAuctionItemInfo* auctioned_item_info = AuctionManager::instance().GetAuctionItemInfoCache(data->get_item_id())->Get(false);
		for (TPeerList::iterator it = m_peerList.begin(); it != m_peerList.end(); it++)
		{
			(*it)->EncodeHeader(HEADER_DG_AUCTION_RESULT, purchaser_id, sizeof(TPacketDGResultAuction) + sizeof(TAuctionItemInfo));
			(*it)->Encode(&enroll_result, sizeof(TPacketDGResultAuction));
			(*it)->Encode(auctioned_item_info, sizeof(TAuctionItemInfo));
		}
	}
	return;
}

void CClientManager::AuctionGetAuctionedItem (CPeer * peer, DWORD actor_id, DWORD item_id)
{
	TPlayerTableCacheMap::iterator it = m_map_playerCache.find (actor_id);
	AuctionResult result = AUCTION_FAIL;
	if (it == m_map_playerCache.end())
	{
		SPDLOG_ERROR("Invalid Player id {}. how can you get it?", actor_id);
		return;
	}

	TPlayerItem item;
	result = AuctionManager::instance().GetAuctionedItem(actor_id, item_id, item);

	if (result <= AUCTION_FAIL)
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_GET_AUC;
		enroll_result.target = item_id;
		enroll_result.result = result;

		peer->EncodeHeader (HEADER_DG_AUCTION_RESULT, actor_id, sizeof(TPacketDGResultAuction));
		peer->Encode (&enroll_result, sizeof(TPacketDGResultAuction));
	}
	else
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_GET_AUC;
		enroll_result.target = item_id;
		enroll_result.result = result;

		for (TPeerList::iterator it = m_peerList.begin(); it != m_peerList.end(); it++)
		{
			(*it)->EncodeHeader (HEADER_DG_AUCTION_RESULT, actor_id, sizeof(TPacketDGResultAuction) + sizeof(TPlayerItem));
			(*it)->Encode (&enroll_result, sizeof(TPacketDGResultAuction));
			(*it)->Encode (&item, sizeof(TPlayerItem));
		}
	}
	return;
}

void CClientManager::AuctionBuySoldItem (CPeer * peer, DWORD actor_id, DWORD item_id)
{
	TPlayerTableCacheMap::iterator it = m_map_playerCache.find (actor_id);
	AuctionResult result = AUCTION_FAIL;
	if (it == m_map_playerCache.end())
	{
		SPDLOG_ERROR("Invalid Player id {}. how can you get it?", actor_id);
		return;
	}

	TPlayerItem item;
	result = AuctionManager::instance().BuySoldItem(actor_id, item_id, item);

	if (result <= AUCTION_FAIL)
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_BUY_SOLD;
		enroll_result.target = item_id;
		enroll_result.result = result;

		peer->EncodeHeader (HEADER_DG_AUCTION_RESULT, actor_id, sizeof(TPacketDGResultAuction));
		peer->Encode (&enroll_result, sizeof(TPacketDGResultAuction));
	}
	else
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_BUY_SOLD;
		enroll_result.target = item_id;
		enroll_result.result = result;

		for (TPeerList::iterator it = m_peerList.begin(); it != m_peerList.end(); it++)
		{
			(*it)->EncodeHeader (HEADER_DG_AUCTION_RESULT, actor_id, sizeof(TPacketDGResultAuction) + sizeof(TPlayerItem));
			(*it)->Encode (&enroll_result, sizeof(TPacketDGResultAuction));
			(*it)->Encode (&item, sizeof(TPlayerItem));
		}
	}
	return;
}

void CClientManager::AuctionCancelAuction (CPeer * peer, DWORD actor_id, DWORD item_id)
{
	TPlayerTableCacheMap::iterator it = m_map_playerCache.find (actor_id);
	AuctionResult result = AUCTION_FAIL;
	if (it == m_map_playerCache.end())
	{
		SPDLOG_ERROR("Invalid Player id {}. how can you get it?", actor_id);
		return;
	}

	TPlayerItem item;
	result = AuctionManager::instance().CancelAuction(actor_id, item_id, item);
	
	if (result <= AUCTION_FAIL)
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_CANCEL_AUC;
		enroll_result.target = item_id;
		enroll_result.result = result;

		peer->EncodeHeader (HEADER_DG_AUCTION_RESULT, actor_id, sizeof(TPacketDGResultAuction));
		peer->Encode (&enroll_result, sizeof(TPacketDGResultAuction));
	}
	else
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_CANCEL_AUC;
		enroll_result.target = item_id;
		enroll_result.result = result;

		for (TPeerList::iterator it = m_peerList.begin(); it != m_peerList.end(); it++)
		{
			(*it)->EncodeHeader (HEADER_DG_AUCTION_RESULT, actor_id, sizeof(TPacketDGResultAuction) + sizeof(TPlayerItem));
			(*it)->Encode (&enroll_result, sizeof(TPacketDGResultAuction));
			(*it)->Encode (&item, sizeof(TPlayerItem));
		}
	}
	return;
}

void CClientManager::AuctionCancelWish (CPeer * peer, DWORD actor_id, DWORD item_num)
{
	TPlayerTableCacheMap::iterator it = m_map_playerCache.find (actor_id);
	AuctionResult result = AUCTION_FAIL;
	if (it == m_map_playerCache.end())
	{
		SPDLOG_ERROR("Invalid Player id {}. how can you get it?", actor_id);
		return;
	}

	TPlayerItem item;
	result = AuctionManager::instance().CancelWish(actor_id, item_num);
	
	if (result <= AUCTION_FAIL)
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_CANCEL_WISH;
		enroll_result.target = item_num;
		enroll_result.result = result;

		peer->EncodeHeader (HEADER_DG_AUCTION_RESULT, actor_id, sizeof(TPacketDGResultAuction));
		peer->Encode (&enroll_result, sizeof(TPacketDGResultAuction));
	}
	else
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_CANCEL_WISH;
		enroll_result.target = item_num;
		enroll_result.result = result;

		for (TPeerList::iterator it = m_peerList.begin(); it != m_peerList.end(); it++)
		{
			(*it)->EncodeHeader (HEADER_DG_AUCTION_RESULT, actor_id, sizeof(TPacketDGResultAuction));
			(*it)->Encode (&enroll_result, sizeof(TPacketDGResultAuction));
		}
	}
	return;
}

void CClientManager::AuctionCancelSale (CPeer * peer, DWORD actor_id, DWORD item_id)
{
	TPlayerTableCacheMap::iterator it = m_map_playerCache.find (actor_id);
	AuctionResult result = AUCTION_FAIL;
	if (it == m_map_playerCache.end())
	{
		SPDLOG_ERROR("Invalid Player id {}. how can you get it?", actor_id);
		return;
	}

	TPlayerItem item;
	result = AuctionManager::instance().CancelSale(actor_id, item_id, item);
	
	if (result <= AUCTION_FAIL)
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_CANCEL_SALE;
		enroll_result.target = item_id;
		enroll_result.result = result;

		peer->EncodeHeader (HEADER_DG_AUCTION_RESULT, actor_id, sizeof(TPacketDGResultAuction));
		peer->Encode (&enroll_result, sizeof(TPacketDGResultAuction));
	}
	else
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_CANCEL_SALE;
		enroll_result.target = item_id;
		enroll_result.result = result;

		for (TPeerList::iterator it = m_peerList.begin(); it != m_peerList.end(); it++)
		{
			(*it)->EncodeHeader (HEADER_DG_AUCTION_RESULT, actor_id, sizeof(TPacketDGResultAuction) + sizeof(TPlayerItem));
			(*it)->Encode (&enroll_result, sizeof(TPacketDGResultAuction));
			(*it)->Encode (&item, sizeof(TPlayerItem));
		}
	}
	return;
}

void CClientManager::AuctionDeleteAuctionItem (CPeer * peer, DWORD actor_id, DWORD item_id)
{
	TPlayerTableCacheMap::iterator it = m_map_playerCache.find (actor_id);
	AuctionResult result = AUCTION_FAIL;
	if (it == m_map_playerCache.end())
	{
		SPDLOG_ERROR("Invalid Player id {}. how can you get it?", actor_id);
		return;
	}

	AuctionManager::instance().DeleteAuctionItem (actor_id, item_id);
}
void CClientManager::AuctionDeleteSaleItem (CPeer * peer, DWORD actor_id, DWORD item_id)
{
	TPlayerTableCacheMap::iterator it = m_map_playerCache.find (actor_id);
	AuctionResult result = AUCTION_FAIL;
	if (it == m_map_playerCache.end())
	{
		SPDLOG_ERROR("Invalid Player id {}. how can you get it?", actor_id);
		return;
	}

	AuctionManager::instance().DeleteSaleItem (actor_id, item_id);
}

// ReBid�� ���� �����ݾ׿� ���ؼ� �����Ѵ�.
// ReBid���� data->bid_price�� ���� �������� ��������
// �� �ݾ����� rebid�ϴ� ��.
// �̷��� �� ������ rebid�� ���� ���� ��,
// ������ ȣ�ָӴϿ��� �� ���� �����ֱ� ���ϰ� �ϱ� �����̴�.

void CClientManager::AuctionReBid (CPeer * peer, DWORD bidder_id, AuctionBidInfo* data)
{
	TPlayerTableCacheMap::iterator it = m_map_playerCache.find (bidder_id);

	if (it == m_map_playerCache.end())
	{
		SPDLOG_ERROR("Invalid Player id {}. how can you get it?", bidder_id);
		return;
	}

	CPlayerTableCache* player_cache = it->second;
	TPlayerTable* player = player_cache->Get(false);

	AuctionResult result = AuctionManager::instance().ReBid(bidder_id, player->name, data->get_item_id(), data->get_bid_price());

	if (result == AUCTION_FAIL)
	{
		SPDLOG_DEBUG("ReBid Fail. bidder_id item_id {} {}", bidder_id, data->get_item_id());
	}
	else
	{
		SPDLOG_DEBUG("ReBid Success. bidder_id item_id {} {}", bidder_id, data->get_item_id());
	}
	// �̰� FAIL�� ������ �ȵ�.
	// FAIL�� �� ���� ���°�, MyBid�� �ִ� bidder_id�� ���� �������� bidder_id���� ���� �� �� �ְŵ�?
	// �׷��Ƿ� �ٸ� ���� �� ���������� �۵��Ѵٰ� ���� �Ѵٸ�
	// �� ���� ���� ������ bidder_id�� MyBid�� �����Ѵ� �� ����, �װ� ����ȭ ������ ����.
	// �ٸ� ���� ������ �Ȱ��� bidder_id�� ���� ���� ���� ���� �����ϱ�.
	// �׷��Ƿ� �� ���� �������� BidCancel ����� db�� ���ȴٴ� ����,
	// �̹� �� �κп� ���ؼ��� �˻簡 �Ϻ��ϴٴ� ���̾�.
	// �׷��� Ȥ�ó� �;, ������� ���� fail �ڵ带 ���ܵд�.
	if (result <= AUCTION_FAIL)
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_REBID;
		enroll_result.target = data->get_item_id();
		enroll_result.result = result;

		peer->EncodeHeader(HEADER_DG_AUCTION_RESULT, bidder_id, sizeof(TPacketDGResultAuction) + sizeof(int));
		peer->Encode(&enroll_result, sizeof(TPacketDGResultAuction));
		peer->EncodeDWORD(data->get_bid_price());
	}
	else
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_REBID;
		enroll_result.target = data->get_item_id();
		enroll_result.result = result;

		TAuctionItemInfo* auctioned_item_info = AuctionManager::instance().GetAuctionItemInfoCache(data->get_item_id())->Get(false);
		
		for (TPeerList::iterator it = m_peerList.begin(); it != m_peerList.end(); it++)
		{
			(*it)->EncodeHeader(HEADER_DG_AUCTION_RESULT, bidder_id, sizeof(TPacketDGResultAuction) + sizeof(TAuctionItemInfo));
			(*it)->Encode(&enroll_result, sizeof(TPacketDGResultAuction));
			(*it)->Encode(auctioned_item_info, sizeof(TAuctionItemInfo));
		}
	}
	return;
}

void CClientManager::AuctionBidCancel (CPeer * peer, DWORD bidder_id, DWORD item_id)
{
	AuctionResult result = AuctionManager::instance().BidCancel (bidder_id, item_id);
	
	// �̰� FAIL�� ������ �ȵ�.
	// FAIL�� �� ���� ���°�, MyBid�� �ִ� bidder_id�� ���� �������� bidder_id���� ���� �� �� �ְŵ�?
	// �׷��Ƿ� �ٸ� ���� �� ���������� �۵��Ѵٰ� ���� �Ѵٸ�
	// �� ���� ���� ������ bidder_id�� MyBid�� �����Ѵ� �� ����, �װ� ����ȭ ������ ����.
	// �ٸ� ���� ������ �Ȱ��� bidder_id�� ���� ���� ���� ���� �����ϱ�.
	// �׷��Ƿ� �� ���� �������� BidCancel ����� db�� ���ȴٴ� ����,
	// �̹� �� �κп� ���ؼ��� �˻簡 �Ϻ��ϴٴ� ���̾�.
	// �׷��� Ȥ�ó� �;, ������� ���� fail �ڵ带 ���ܵд�.
	if (result <= AUCTION_FAIL)
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_BID_CANCEL;
		enroll_result.target = item_id;
		enroll_result.result = result;

		peer->EncodeHeader(HEADER_DG_AUCTION_RESULT, bidder_id, sizeof(TPacketDGResultAuction));
		peer->Encode(&enroll_result, sizeof(TPacketDGResultAuction));
	}
	else
	{
		TPacketDGResultAuction enroll_result;
		enroll_result.cmd = AUCTION_BID_CANCEL;
		enroll_result.target = item_id;
		enroll_result.result = result;

		peer->EncodeHeader(HEADER_DG_AUCTION_RESULT, bidder_id, sizeof(TPacketDGResultAuction));
		peer->Encode(&enroll_result, sizeof(TPacketDGResultAuction));
	}
}
#endif
