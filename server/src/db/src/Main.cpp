#include "stdafx.h"
#include "Config.h"
#include "Peer.h"
#include "DBManager.h"
#include "ClientManager.h"
#include "GuildManager.h"
#include "ItemAwardManager.h"
#include "HB.h"
#include "PrivManager.h"
#include "MoneyLog.h"
#include "Marriage.h"
#include "Monarch.h"
#include "BlockCountry.h"
#include "ItemIDRangeManager.h"
#include <version.h>
#ifdef __AUCTION__
#include "AuctionManager.h"
#endif
#include <signal.h>

void SetPlayerDBName(const char* c_pszPlayerDBName);
void SetTablePostfix(const char* c_pszTablePostfix);
int Start();

std::string g_stTablePostfix;
std::string g_stLocaleNameColumn = "name";
std::string g_stLocale = "euckr";
std::string g_stPlayerDBName = "";


bool g_bHotBackup = false;
BOOL g_test_server = false;

//���� ��
int g_iPlayerCacheFlushSeconds = 60*7;
int g_iItemCacheFlushSeconds = 60*5;

//g_iLogoutSeconds ��ġ�� g_iPlayerCacheFlushSeconds �� g_iItemCacheFlushSeconds ���� ���� �Ѵ�.
int g_iLogoutSeconds = 60*10;


// MYSHOP_PRICE_LIST
int g_iItemPriceListTableCacheFlushSeconds = 540;
// END_OF_MYSHOP_PRICE_LIST

#ifdef __FreeBSD__
extern const char * _malloc_options;
#endif

void emergency_sig(int sig)
{
	if (sig == SIGSEGV)
		SPDLOG_DEBUG("SIGNAL: SIGSEGV");
	else if (sig == SIGUSR1)
		SPDLOG_DEBUG("SIGNAL: SIGUSR1");

	if (sig == SIGSEGV)
		abort();
}

int main()
{
	WriteVersion();
    log_init();

#ifdef __FreeBSD__
	_malloc_options = "A";
#endif

	CConfig Config;
	CDBManager DBManager; 
	CClientManager ClientManager;
	PlayerHB player_hb;
	CGuildManager GuildManager;
	CPrivManager PrivManager;
	CMoneyLog MoneyLog;
	ItemAwardManager ItemAwardManager;
	marriage::CManager MarriageManager;
	CMonarch Monarch;
	CBlockCountry	BlockCountry;
	CItemIDRangeManager ItemIDRangeManager;
#ifdef __AUCTION__
	AuctionManager auctionManager;
#endif
	if (!Start())
		return 1;

	GuildManager.Initialize();
	MarriageManager.Initialize();
	BlockCountry.Load();
	ItemIDRangeManager.Build();
#ifdef __AUCTION__
	AuctionManager::instance().Initialize();
#endif
	SPDLOG_DEBUG("Metin2DBCacheServer Start");

	CClientManager::instance().MainLoop();

	signal_timer_disable();

	DBManager.Quit();
	int iCount;

	while (1)
	{
		iCount = 0;

		iCount += CDBManager::instance().CountReturnQuery(SQL_PLAYER);
		iCount += CDBManager::instance().CountAsyncQuery(SQL_PLAYER);

		if (iCount == 0)
			break;

		usleep(1000);
		SPDLOG_DEBUG("WAITING_QUERY_COUNT {}", iCount);
	}

    log_destroy();

	return 1;
}

void emptybeat(LPHEART heart, int pulse)
{
	if (!(pulse % heart->passes_per_sec))	// 1�ʿ� �ѹ�
	{
	}
}

//
// @version	05/06/13 Bang2ni - ������ �������� ĳ�� flush timeout ���� �߰�.
//
int Start()
{
	if (!CConfig::instance().LoadFile("conf.txt"))
	{
		SPDLOG_ERROR("Loading conf.txt failed.");
		return false;
	}

	if (!CConfig::instance().GetValue("TEST_SERVER", &g_test_server))
	{
		SPDLOG_INFO("Real Server");
	}
	else
        SPDLOG_INFO("Test Server");

	int tmpValue;

	int heart_beat = 50;
	if (!CConfig::instance().GetValue("CLIENT_HEART_FPS", &heart_beat))
	{
		SPDLOG_ERROR("Cannot find CLIENT_HEART_FPS configuration.");
		return false;
	}

	if (CConfig::instance().GetValue("LOG_LEVEL", &tmpValue))
	{
        SPDLOG_INFO("Setting log level to {}", tmpValue);
		tmpValue = std::clamp(tmpValue, SPDLOG_LEVEL_TRACE, SPDLOG_LEVEL_OFF);
		log_set_level(tmpValue);
	}

	thecore_init(heart_beat, emptybeat);
	signal_timer_enable(60);

	char szBuf[256+1];

	if (CConfig::instance().GetValue("LOCALE", szBuf, 256))
	{
		g_stLocale = szBuf;
		SPDLOG_INFO("LOCALE set to {}", g_stLocale.c_str());

		// CHINA_DISABLE_HOTBACKUP
		if ("gb2312" == g_stLocale)
		{
			SPDLOG_INFO("CIBN_LOCALE: DISABLE_HOTBACKUP");
			g_bHotBackup = false;
		}
		// END_OF_CHINA_DISABLE_HOTBACKUP
	}

	int iDisableHotBackup;
	if (CConfig::instance().GetValue("DISABLE_HOTBACKUP", &iDisableHotBackup))
	{
		if (iDisableHotBackup)
		{
            SPDLOG_INFO("CONFIG: DISABLE_HOTBACKUP");
			g_bHotBackup = false;
		}
	}


	if (!CConfig::instance().GetValue("TABLE_POSTFIX", szBuf, 256))
	{
		SPDLOG_WARN("TABLE_POSTFIX not configured use default");
		szBuf[0] = '\0';
	}

	SetTablePostfix(szBuf);

	if (CConfig::instance().GetValue("PLAYER_CACHE_FLUSH_SECONDS", szBuf, 256))
	{
		str_to_number(g_iPlayerCacheFlushSeconds, szBuf);
		SPDLOG_INFO("PLAYER_CACHE_FLUSH_SECONDS: {}", g_iPlayerCacheFlushSeconds);
	}

	if (CConfig::instance().GetValue("ITEM_CACHE_FLUSH_SECONDS", szBuf, 256))
	{
		str_to_number(g_iItemCacheFlushSeconds, szBuf);
        SPDLOG_INFO("ITEM_CACHE_FLUSH_SECONDS: {}", g_iItemCacheFlushSeconds);
	}

	// MYSHOP_PRICE_LIST
	if (CConfig::instance().GetValue("ITEM_PRICELIST_CACHE_FLUSH_SECONDS", szBuf, 256)) 
	{
		str_to_number(g_iItemPriceListTableCacheFlushSeconds, szBuf);
        SPDLOG_INFO("ITEM_PRICELIST_CACHE_FLUSH_SECONDS: {}", g_iItemPriceListTableCacheFlushSeconds);
	}
	// END_OF_MYSHOP_PRICE_LIST
	//
	if (CConfig::instance().GetValue("CACHE_FLUSH_LIMIT_PER_SECOND", szBuf, 256))
	{
		DWORD dwVal = 0; str_to_number(dwVal, szBuf);
		CClientManager::instance().SetCacheFlushCountLimit(dwVal);
	}

	int iIDStart;
	if (!CConfig::instance().GetValue("PLAYER_ID_START", &iIDStart))
	{
		SPDLOG_ERROR("PLAYER_ID_START not configured");
		return false;
	}

	CClientManager::instance().SetPlayerIDStart(iIDStart);

	if (CConfig::instance().GetValue("NAME_COLUMN", szBuf, 256))
	{
		SPDLOG_INFO("{} {}", g_stLocaleNameColumn, szBuf);
		g_stLocaleNameColumn = szBuf;
	}

	char szAddr[64], szDB[64], szUser[64], szPassword[64];
	int iPort;
	char line[256+1];

	if (CConfig::instance().GetValue("SQL_PLAYER", line, 256))
	{
		sscanf(line, " %s %s %s %s %d ", szAddr, szDB, szUser, szPassword, &iPort);
        SPDLOG_DEBUG("Connecting to MySQL server (player)");

		int iRetry = 5;

		do
		{
			if (CDBManager::instance().Connect(SQL_PLAYER, szAddr, iPort, szDB, szUser, szPassword))
			{
				SPDLOG_INFO("Connected to MySQL server (player)");
				break;
			}

			SPDLOG_ERROR("Connection to MySQL server (player) failed, retrying in 5 seconds");
			sleep(5);
		} while (iRetry--);
		SetPlayerDBName(szDB);
	}
	else
	{
        SPDLOG_ERROR("SQL_PLAYER not configured");
		return false;
	}

	if (CConfig::instance().GetValue("SQL_ACCOUNT", line, 256))
	{
		sscanf(line, " %s %s %s %s %d ", szAddr, szDB, szUser, szPassword, &iPort);
		SPDLOG_DEBUG("connecting to MySQL server (account)");

		int iRetry = 5;

		do
		{
			if (CDBManager::instance().Connect(SQL_ACCOUNT, szAddr, iPort, szDB, szUser, szPassword))
			{
				SPDLOG_DEBUG("   OK");
				break;
			}

			SPDLOG_DEBUG("   failed, retrying in 5 seconds");
			SPDLOG_ERROR("   failed, retrying in 5 seconds");
			sleep(5);
		} while (iRetry--);
		SPDLOG_INFO("Success ACCOUNT");
	}
	else
	{
		SPDLOG_ERROR("SQL_ACCOUNT not configured");
		return false;
	}

	if (CConfig::instance().GetValue("SQL_COMMON", line, 256))
	{
		sscanf(line, " %s %s %s %s %d ", szAddr, szDB, szUser, szPassword, &iPort);
		SPDLOG_DEBUG("connecting to MySQL server (common)");

		int iRetry = 5;

		do
		{
			if (CDBManager::instance().Connect(SQL_COMMON, szAddr, iPort, szDB, szUser, szPassword))
			{
				SPDLOG_DEBUG("   OK");
				break;
			}

			SPDLOG_DEBUG("   failed, retrying in 5 seconds");
			SPDLOG_ERROR("   failed, retrying in 5 seconds");
			sleep(5);
		} while (iRetry--);
        SPDLOG_INFO("Success COMMON");
	}
	else
	{
		SPDLOG_ERROR("SQL_COMMON not configured");
		return false;
	}

	if (CConfig::instance().GetValue("SQL_HOTBACKUP", line, 256))
	{
		sscanf(line, " %s %s %s %s %d ", szAddr, szDB, szUser, szPassword, &iPort);
		SPDLOG_DEBUG("connecting to MySQL server (hotbackup)");

		int iRetry = 5;

		do
		{
			if (CDBManager::instance().Connect(SQL_HOTBACKUP, szAddr, iPort, szDB, szUser, szPassword))
			{
				SPDLOG_DEBUG("   OK");
				break;
			}

			SPDLOG_DEBUG("   failed, retrying in 5 seconds");
			SPDLOG_ERROR("   failed, retrying in 5 seconds");
			sleep(5);
		}
		while (iRetry--);

        SPDLOG_INFO("Success HOTBACKUP");
	}
	else
	{
		SPDLOG_ERROR("SQL_HOTBACKUP not configured");
		return false;
	}

	if (!CClientManager::instance().Initialize())
	{
        SPDLOG_ERROR("ClientManager initialization failed");
		return false;
	}

    SPDLOG_INFO("ClientManager initialization OK");

    if (!PlayerHB::instance().Initialize())
	{
        SPDLOG_ERROR("cannot initialize player hotbackup");
		return false;
	}

#ifndef __WIN32__
	signal(SIGUSR1, emergency_sig);
#endif
	signal(SIGSEGV, emergency_sig);
	return true;
}

void SetTablePostfix(const char* c_pszTablePostfix)
{
	if (!c_pszTablePostfix || !*c_pszTablePostfix)
		g_stTablePostfix = "";
	else
		g_stTablePostfix = c_pszTablePostfix;
}

const char * GetTablePostfix()
{
	return g_stTablePostfix.c_str();
}

void SetPlayerDBName(const char* c_pszPlayerDBName)
{
	if (! c_pszPlayerDBName || ! *c_pszPlayerDBName)
		g_stPlayerDBName = "";
	else
	{
		g_stPlayerDBName = c_pszPlayerDBName;
		g_stPlayerDBName += ".";
	}
}

const char * GetPlayerDBName()
{
	return g_stPlayerDBName.c_str();
}

