#include "stdafx.h"
#include "constants.h"
#include "login_data.h"

extern std::string g_stBlockDate; 

CLoginData::CLoginData()
{
	m_dwKey = 0;
	memset(m_adwClientKey, 0, sizeof(m_adwClientKey));
	m_bBillType = 0;
	m_dwBillID = 0;
	m_dwConnectedPeerHandle = 0;
	m_dwLogonTime = 0;
	m_lRemainSecs = 0;
	memset(m_szIP, 0, sizeof(m_szIP));
	m_bBilling = false;
	m_bDeleted = false;
	memset(m_aiPremiumTimes, 0, sizeof(m_aiPremiumTimes));
}

void CLoginData::SetClientKey(const DWORD * c_pdwClientKey)
{
	memcpy(&m_adwClientKey, c_pdwClientKey, sizeof(DWORD) * 4);
}

const DWORD * CLoginData::GetClientKey()
{
	return &m_adwClientKey[0];
}

void CLoginData::SetKey(DWORD dwKey)
{
	m_dwKey = dwKey;
}

DWORD CLoginData::GetKey()
{
	return m_dwKey;
}

void CLoginData::SetBillType(BYTE bType)
{
	m_bBillType = bType;
}

DWORD CLoginData::GetBillID() 
{
	return m_dwBillID;
}

void CLoginData::SetBillID(DWORD dwID)
{
	m_dwBillID = dwID;
}

BYTE CLoginData::GetBillType()
{
	return m_bBillType;
}

void CLoginData::SetConnectedPeerHandle(DWORD dwHandle)
{
	m_dwConnectedPeerHandle = dwHandle;
}

DWORD CLoginData::GetConnectedPeerHandle()
{
	return m_dwConnectedPeerHandle;
}

void CLoginData::SetLogonTime()
{
	m_dwLogonTime = get_dword_time();
}

DWORD CLoginData::GetLogonTime()
{
	return m_dwLogonTime;
}

void CLoginData::SetIP(const char * c_pszIP)
{
	strlcpy(m_szIP, c_pszIP, sizeof(m_szIP));
}

const char * CLoginData::GetIP()
{
	return m_szIP;
}

void CLoginData::SetRemainSecs(int l)
{
	m_lRemainSecs = l;
	SPDLOG_DEBUG("SetRemainSecs {} {} type {}", m_stLogin, m_lRemainSecs, m_bBillType);
}

int CLoginData::GetRemainSecs()
{
	return m_lRemainSecs;
}

void CLoginData::SetBilling(bool bOn)
{
	if (bOn)
	{
		SPDLOG_DEBUG("BILLING: ON {} key {} ptr {}", m_stLogin, m_dwKey, (void*) this);
		SetLogonTime();
	}
	else
		SPDLOG_DEBUG("BILLING: OFF {} key {} ptr {}", m_stLogin, m_dwKey, (void*) this);

	m_bBilling = bOn;
}

bool CLoginData::IsBilling()
{
	return m_bBilling;
}

void CLoginData::SetDeleted(bool bSet)
{
	m_bDeleted = bSet;
}

bool CLoginData::IsDeleted()
{
	return m_bDeleted;
}

void CLoginData::SetLogin(const char * c_pszLogin)
{
	m_stLogin = c_pszLogin;
}

const char * CLoginData::GetLogin()
{
	return m_stLogin.c_str();
}

void CLoginData::SetPremium(int * paiPremiumTimes)
{
	memcpy(m_aiPremiumTimes, paiPremiumTimes, sizeof(m_aiPremiumTimes));
}

int CLoginData::GetPremium(BYTE type)
{
	if (type >= PREMIUM_MAX_NUM)
		return 0;

	return m_aiPremiumTimes[type];
}

int * CLoginData::GetPremiumPtr()
{
	return &m_aiPremiumTimes[0];
}
