/*
 * Copyright: JessMA Open Source (ldcsaa@gmail.com)
 *
 * Author	: Bruce Liang
 * Website	: https://github.com/ldcsaa
 * Project	: https://github.com/ldcsaa/HP-Socket
 * Blog		: http://www.cnblogs.com/ldcsaa
 * Wiki		: http://www.oschina.net/p/hp-socket
 * QQ Group	: 44636872, 75375912
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#include "TcpAgent.h"

#include "./common/FileHelper.h"

BOOL CTcpAgent::Start(LPCTSTR lpszBindAddress, BOOL bAsyncConnect)
{
	if(!CheckParams() || !CheckStarting())
		return FALSE;

	PrepareStart();

	if(ParseBindAddress(lpszBindAddress))
		if(CreateWorkerThreads())
		{
			m_bAsyncConnect	= bAsyncConnect;
			m_enState		= SS_STARTED;

			return TRUE;
		}

	EXECUTE_RESTORE_ERROR(Stop());

	return FALSE;
}

void CTcpAgent::SetLastError(EnSocketError code, LPCSTR func, int ec)
{
	m_enLastError = code;
	::SetLastError(ec);
}

BOOL CTcpAgent::CheckParams()
{
	if	((m_enSendPolicy >= SP_PACK && m_enSendPolicy <= SP_DIRECT)								&&
		(m_enOnSendSyncPolicy >= OSSP_NONE && m_enOnSendSyncPolicy <= OSSP_RECEIVE)				&&
		((int)m_dwSyncConnectTimeout > 0)														&&
		((int)m_dwMaxConnectionCount > 0 && m_dwMaxConnectionCount <= MAX_CONNECTION_COUNT)		&&
		((int)m_dwWorkerThreadCount > 0 && m_dwWorkerThreadCount <= MAX_WORKER_THREAD_COUNT)	&&
		((int)m_dwSocketBufferSize >= MIN_SOCKET_BUFFER_SIZE)									&&
		((int)m_dwFreeSocketObjLockTime >= 1000)												&&
		((int)m_dwFreeSocketObjPool >= 0)														&&
		((int)m_dwFreeBufferObjPool >= 0)														&&
		((int)m_dwFreeSocketObjHold >= 0)														&&
		((int)m_dwFreeBufferObjHold >= 0)														&&
		((int)m_dwKeepAliveTime >= 1000 || m_dwKeepAliveTime == 0)								&&
		((int)m_dwKeepAliveInterval >= 1000 || m_dwKeepAliveInterval == 0)						)
		return TRUE;

	SetLastError(SE_INVALID_PARAM, __FUNCTION__, ERROR_INVALID_PARAMETER);
	return FALSE;
}

void CTcpAgent::PrepareStart()
{
	m_bfActiveSockets.Reset(m_dwMaxConnectionCount);
	m_lsFreeSocket.Reset(m_dwFreeSocketObjPool);

	m_bfObjPool.SetItemCapacity(m_dwSocketBufferSize);
	m_bfObjPool.SetPoolSize(m_dwFreeBufferObjPool);
	m_bfObjPool.SetPoolHold(m_dwFreeBufferObjHold);

	m_bfObjPool.Prepare();

	m_rcBuffers = make_unique<CBufferPtr[]>(m_dwWorkerThreadCount);
	for_each(m_rcBuffers.get(), m_rcBuffers.get() + m_dwWorkerThreadCount, [this](CBufferPtr& buff) {buff.Malloc(m_dwSocketBufferSize);});
}

BOOL CTcpAgent::CheckStarting()
{
	CSpinLock locallock(m_csState);

	if(m_enState == SS_STOPPED)
		m_enState = SS_STARTING;
	else
	{
		SetLastError(SE_ILLEGAL_STATE, __FUNCTION__, ERROR_INVALID_STATE);
		return FALSE;
	}

	return TRUE;
}

BOOL CTcpAgent::CheckStoping()
{
	if(m_enState != SS_STOPPED)
	{
		CSpinLock locallock(m_csState);

		if(HasStarted())
		{
			m_enState = SS_STOPPING;
			return TRUE;
		}
	}

	SetLastError(SE_ILLEGAL_STATE, __FUNCTION__, ERROR_INVALID_STATE);

	return FALSE;
}

BOOL CTcpAgent::ParseBindAddress(LPCTSTR lpszBindAddress)
{
	if(::IsStrEmpty(lpszBindAddress))
		return TRUE;

	HP_SOCKADDR addr;

	if(::sockaddr_A_2_IN(lpszBindAddress, 0, addr))
	{
		SOCKET sock	= socket(addr.family, SOCK_STREAM, IPPROTO_TCP);

		if(sock != INVALID_SOCKET)
		{
			if(::bind(sock, addr.Addr(), addr.AddrSize()) != SOCKET_ERROR)
			{
				addr.Copy(m_soAddr);
				return TRUE;
			}
			else
				SetLastError(SE_SOCKET_BIND, __FUNCTION__, ::WSAGetLastError());

			::ManualCloseSocket(sock);
		}
		else
			SetLastError(SE_SOCKET_CREATE, __FUNCTION__, ::WSAGetLastError());
	}
	else
		SetLastError(SE_SOCKET_CREATE, __FUNCTION__, ::WSAGetLastError());

	return FALSE;
}

BOOL CTcpAgent::CreateWorkerThreads()
{
	DWORD dwWorkerThreadCount = m_dwWorkerThreadCount
#ifdef USE_EXTERNAL_GC
														+ 1
#endif
														;

	if(!m_ioDispatcher.Start(this, DEFAULT_WORKER_MAX_EVENT_COUNT, dwWorkerThreadCount))
	{
		SetLastError(SE_WORKER_THREAD_CREATE, __FUNCTION__, ::WSAGetLastError());
		return FALSE;
	}

#ifdef USE_EXTERNAL_GC
	m_fdGCTimer = m_ioDispatcher.AddTimer(m_dwWorkerThreadCount, GC_CHECK_INTERVAL, this);

	if(IS_INVALID_FD(m_fdGCTimer))
	{
		SetLastError(SE_GC_START, __FUNCTION__, ::WSAGetLastError());
		return FALSE;
	}
#endif

	return TRUE;
}

BOOL CTcpAgent::Stop()
{
	if(!CheckStoping())
		return FALSE;
	
	DisconnectClientSocket();
	WaitForClientSocketClose();
	WaitForWorkerThreadEnd();
	
	ReleaseClientSocket();

	FireShutdown();

	ReleaseFreeSocket();

	Reset();

	return TRUE;
}

void CTcpAgent::DisconnectClientSocket()
{
	::WaitFor(100);

	if(m_bfActiveSockets.Elements() == 0)
		return;

	TAgentSocketObjPtrPool::IndexSet indexes;
	m_bfActiveSockets.CopyIndexes(indexes);

	for(auto it = indexes.begin(), end = indexes.end(); it != end; ++it)
		Disconnect(*it);
}

void CTcpAgent::WaitForClientSocketClose()
{
	while(m_bfActiveSockets.Elements() > 0)
		::WaitFor(50);
}

void CTcpAgent::WaitForWorkerThreadEnd()
{
	m_ioDispatcher.Stop();
}

void CTcpAgent::ReleaseClientSocket()
{
	VERIFY(m_bfActiveSockets.IsEmpty());
	m_bfActiveSockets.Reset();
}

void CTcpAgent::ReleaseFreeSocket()
{
	m_lsFreeSocket.Clear();

#ifdef USE_EXTERNAL_GC
	if(IS_VALID_FD(m_fdGCTimer))
	{
		close(m_fdGCTimer);
		m_fdGCTimer = INVALID_FD;
	}
#endif

	ReleaseGCSocketObj(TRUE);
	VERIFY(m_lsGCSocket.IsEmpty());
}

void CTcpAgent::Reset()
{
	m_bfObjPool.Clear();
	m_phSocket.Reset();
	m_soAddr.Reset();

	m_rcBuffers = nullptr;

	m_enState = SS_STOPPED;

	m_evWait.SyncNotifyAll();
}

BOOL CTcpAgent::Connect(LPCTSTR lpszRemoteAddress, USHORT usPort, CONNID* pdwConnID, PVOID pExtra, USHORT usLocalPort, LPCTSTR lpszLocalAddress)
{
	ASSERT(lpszRemoteAddress && usPort != 0);

	if(!HasStarted())
	{
		::SetLastError(ERROR_INVALID_STATE);
		return FALSE;
	}

	if(!pdwConnID)
		pdwConnID = CreateLocalObject(CONNID);

	*pdwConnID = 0;

	HP_SOCKADDR addr;
	HP_SCOPE_HOST host(lpszRemoteAddress);
	SOCKET soClient = INVALID_SOCKET;

	DWORD result = CreateClientSocket(host.addr, usPort, lpszLocalAddress, usLocalPort, soClient, addr);

	if(result == NO_ERROR)
	{
		result = PrepareConnect(*pdwConnID, soClient);

		if(result == NO_ERROR)
			result = ConnectToServer(*pdwConnID, host.name, soClient, addr, pExtra);
	}

	if(result != NO_ERROR)
	{
		if(soClient != INVALID_SOCKET)
			::ManualCloseSocket(soClient);

		::SetLastError(result);
	}

	return (result == NO_ERROR);
}

int CTcpAgent::CreateClientSocket(LPCTSTR lpszRemoteAddress, USHORT usPort, LPCTSTR lpszLocalAddress, USHORT usLocalPort, SOCKET& soClient, HP_SOCKADDR& addr)
{
	if(!::GetSockAddrByHostName(lpszRemoteAddress, usPort, addr))
		return ERROR_ADDRNOTAVAIL;

	HP_SOCKADDR* lpBindAddr = &m_soAddr;

	if(::IsStrNotEmpty(lpszLocalAddress))
	{
		lpBindAddr = CreateLocalObject(HP_SOCKADDR);

		if(!::sockaddr_A_2_IN(lpszLocalAddress, 0, *lpBindAddr))
			return ::WSAGetLastError();
	}

	BOOL bBind = lpBindAddr->IsSpecified();

	if(bBind && lpBindAddr->family != addr.family)
		return ERROR_AFNOSUPPORT;

	int result	= NO_ERROR;
	soClient	= socket(addr.family, SOCK_STREAM, IPPROTO_TCP);

	if(soClient == INVALID_SOCKET)
		result = ::WSAGetLastError();
	else
	{
		BOOL bOnOff	= (m_dwKeepAliveTime > 0 && m_dwKeepAliveInterval > 0);
		VERIFY(IS_NO_ERROR(::SSO_KeepAliveVals(soClient, bOnOff, m_dwKeepAliveTime, m_dwKeepAliveInterval)));
		VERIFY(IS_NO_ERROR(::SSO_ReuseAddress(soClient, m_enReusePolicy)));
		VERIFY(IS_NO_ERROR(::SSO_NoDelay(soClient, m_bNoDelay)));

		if(bBind && usLocalPort == 0)
		{
			if(::bind(soClient, lpBindAddr->Addr(), lpBindAddr->AddrSize()) == SOCKET_ERROR)
				result = ::WSAGetLastError();
		}
		else if(usLocalPort != 0)
		{
			HP_SOCKADDR bindAddr = bBind ? *lpBindAddr : HP_SOCKADDR::AnyAddr(addr.family);

			bindAddr.SetPort(usLocalPort);

			if(::bind(soClient, bindAddr.Addr(), bindAddr.AddrSize()) == SOCKET_ERROR)
				result = ::WSAGetLastError();
		}
	}

	return result;
}

int CTcpAgent::PrepareConnect(CONNID& dwConnID, SOCKET soClient)
{
	if(!m_bfActiveSockets.AcquireLock(dwConnID))
		return ERROR_CONNECTION_COUNT_LIMIT;

	if(TRIGGER(FirePrepareConnect(dwConnID, soClient)) == HR_ERROR)
	{
		VERIFY(m_bfActiveSockets.ReleaseLock(dwConnID, nullptr));
		return ENSURE_ERROR_CANCELLED;
	}

	return NO_ERROR;
}

int CTcpAgent::ConnectToServer(CONNID dwConnID, LPCTSTR lpszRemoteHostName, SOCKET& soClient, const HP_SOCKADDR& addr, PVOID pExtra)
{
	TAgentSocketObj* pSocketObj = GetFreeSocketObj(dwConnID, soClient);
	AddClientSocketObj(dwConnID, pSocketObj, addr, lpszRemoteHostName, pExtra);

	int result = HAS_ERROR;

	VERIFY(::fcntl_SETFL(pSocketObj->socket, O_NOATIME | O_NONBLOCK | O_CLOEXEC));

	int rc = ::connect(pSocketObj->socket, addr.Addr(), addr.AddrSize());

	if(IS_NO_ERROR(rc) || IS_IO_PENDING_ERROR())
	{
		if(m_bAsyncConnect)
		{
			if(m_ioDispatcher.AddFD(pSocketObj->socket, EPOLLOUT, pSocketObj))
				result = NO_ERROR;
		}
		else
		{
			if(IS_HAS_ERROR(result))
				result = ::WaitForSocketWrite(pSocketObj->socket, m_dwSyncConnectTimeout);

			if(IS_NO_ERROR(result))
			{
				pSocketObj->SetConnected();

				if(TRIGGER(FireConnect(pSocketObj)) == HR_ERROR)
					result = ENSURE_ERROR_CANCELLED;
				else
				{
					UINT evts = (pSocketObj->IsPending() ? EPOLLOUT : 0) | (pSocketObj->IsPaused() ? 0 : EPOLLIN);

					if(!m_ioDispatcher.AddFD(pSocketObj->socket, evts | EPOLLRDHUP, pSocketObj))
						result = HAS_ERROR;
				}
			}
		}
	}

	if(result == HAS_ERROR)
		result = ::WSAGetLastError();
	if(result != NO_ERROR)
	{
		AddFreeSocketObj(pSocketObj, SCF_NONE);
		soClient = INVALID_SOCKET;
	}

	return result;
}

TAgentSocketObj* CTcpAgent::GetFreeSocketObj(CONNID dwConnID, SOCKET soClient)
{
	DWORD dwIndex;
	TAgentSocketObj* pSocketObj = nullptr;

	if(m_lsFreeSocket.TryLock(&pSocketObj, dwIndex))
	{
		if(::GetTimeGap32(pSocketObj->freeTime) >= m_dwFreeSocketObjLockTime)
			VERIFY(m_lsFreeSocket.ReleaseLock(nullptr, dwIndex));
		else
		{
			VERIFY(m_lsFreeSocket.ReleaseLock(pSocketObj, dwIndex));
			pSocketObj = nullptr;
		}
	}

	if(!pSocketObj) pSocketObj = CreateSocketObj();
	pSocketObj->Reset(dwConnID, soClient);

	return pSocketObj;
}

TAgentSocketObj* CTcpAgent::CreateSocketObj()
{
	return TAgentSocketObj::Construct(m_phSocket, m_bfObjPool);
}

void CTcpAgent::DeleteSocketObj(TAgentSocketObj* pSocketObj)
{
	TAgentSocketObj::Destruct(pSocketObj);
}

void CTcpAgent::AddFreeSocketObj(TAgentSocketObj* pSocketObj, EnSocketCloseFlag enFlag, EnSocketOperation enOperation, int iErrorCode)
{
	if(!InvalidSocketObj(pSocketObj))
		return;

	CloseClientSocketObj(pSocketObj, enFlag, enOperation, iErrorCode);

	m_bfActiveSockets.Remove(pSocketObj->connID);
	TAgentSocketObj::Release(pSocketObj);

#ifndef USE_EXTERNAL_GC
	ReleaseGCSocketObj();
#endif

	if(!m_lsFreeSocket.TryPut(pSocketObj))
		m_lsGCSocket.PushBack(pSocketObj);
}

void CTcpAgent::ReleaseGCSocketObj(BOOL bForce)
{
	::ReleaseGCObj(m_lsGCSocket, m_dwFreeSocketObjLockTime, bForce);
}

BOOL CTcpAgent::InvalidSocketObj(TAgentSocketObj* pSocketObj)
{
	return TAgentSocketObj::InvalidSocketObj(pSocketObj);
}

void CTcpAgent::AddClientSocketObj(CONNID dwConnID, TAgentSocketObj* pSocketObj, const HP_SOCKADDR& remoteAddr, LPCTSTR lpszRemoteHostName, PVOID pExtra)
{
	ASSERT(FindSocketObj(dwConnID) == nullptr);

	pSocketObj->connTime	= ::TimeGetTime();
	pSocketObj->activeTime	= pSocketObj->connTime;
	pSocketObj->host		= lpszRemoteHostName;
	pSocketObj->extra		= pExtra;

	pSocketObj->SetConnected(CST_CONNECTING);
	remoteAddr.Copy(pSocketObj->remoteAddr);

	VERIFY(m_bfActiveSockets.ReleaseLock(dwConnID, pSocketObj));
}

TAgentSocketObj* CTcpAgent::FindSocketObj(CONNID dwConnID)
{
	TAgentSocketObj* pSocketObj = nullptr;

	if(m_bfActiveSockets.Get(dwConnID, &pSocketObj) != TAgentSocketObjPtrPool::GR_VALID)
		pSocketObj = nullptr;

	return pSocketObj;
}

void CTcpAgent::CloseClientSocketObj(TAgentSocketObj* pSocketObj, EnSocketCloseFlag enFlag, EnSocketOperation enOperation, int iErrorCode, int iShutdownFlag)
{
	ASSERT(TAgentSocketObj::IsExist(pSocketObj));

	if(enFlag == SCF_CLOSE)
		FireClose(pSocketObj, SO_CLOSE, SE_OK);
	else if(enFlag == SCF_ERROR)
		FireClose(pSocketObj, enOperation, iErrorCode);

	SOCKET socket = pSocketObj->socket;
	pSocketObj->socket = INVALID_SOCKET;

	::ManualCloseSocket(socket, iShutdownFlag);
}

BOOL CTcpAgent::GetLocalAddress(CONNID dwConnID, TCHAR lpszAddress[], int& iAddressLen, USHORT& usPort)
{
	ASSERT(lpszAddress != nullptr && iAddressLen > 0);

	TAgentSocketObj* pSocketObj = FindSocketObj(dwConnID);

	if(!TAgentSocketObj::IsValid(pSocketObj))
	{
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
		return FALSE;
	}

	return ::GetSocketLocalAddress(pSocketObj->socket, lpszAddress, iAddressLen, usPort);
}

BOOL CTcpAgent::GetRemoteAddress(CONNID dwConnID, TCHAR lpszAddress[], int& iAddressLen, USHORT& usPort)
{
	ASSERT(lpszAddress != nullptr && iAddressLen > 0);

	TAgentSocketObj* pSocketObj = FindSocketObj(dwConnID);

	if(!TAgentSocketObj::IsExist(pSocketObj))
	{
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
		return FALSE;
	}

	ADDRESS_FAMILY usFamily;
	return ::sockaddr_IN_2_A(pSocketObj->remoteAddr, usFamily, lpszAddress, iAddressLen, usPort);
}

BOOL CTcpAgent::GetRemoteHost(CONNID dwConnID, TCHAR lpszHost[], int& iHostLen, USHORT& usPort)
{
	ASSERT(lpszHost != nullptr && iHostLen > 0);

	BOOL isOK					= FALSE;
	TAgentSocketObj* pSocketObj	= FindSocketObj(dwConnID);

	if(!TAgentSocketObj::IsExist(pSocketObj))
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
	else
	{
		int iLen = pSocketObj->host.GetLength() + 1;

		if(iHostLen >= iLen)
		{
			memcpy(lpszHost, CA2CT((LPCSTR)pSocketObj->host), iLen * sizeof(TCHAR));
			usPort = pSocketObj->remoteAddr.Port();

			isOK = TRUE;
		}

		iHostLen = iLen;
	}

	return isOK;
}

BOOL CTcpAgent::GetRemoteHost(CONNID dwConnID, LPCSTR* lpszHost, USHORT* pusPort)
{
	*lpszHost					= nullptr;
	TAgentSocketObj* pSocketObj	= FindSocketObj(dwConnID);

	if(!TAgentSocketObj::IsExist(pSocketObj))
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
	else
	{
		*lpszHost = pSocketObj->host;

		if(pusPort)
			*pusPort = pSocketObj->remoteAddr.Port();
	}

	return (*lpszHost != nullptr && (*lpszHost)[0] != 0);
}

BOOL CTcpAgent::SetConnectionExtra(CONNID dwConnID, PVOID pExtra)
{
	TAgentSocketObj* pSocketObj = FindSocketObj(dwConnID);
	return SetConnectionExtra(pSocketObj, pExtra);
}

BOOL CTcpAgent::SetConnectionExtra(TAgentSocketObj* pSocketObj, PVOID pExtra)
{
	if(!TAgentSocketObj::IsExist(pSocketObj))
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
	else
	{
		pSocketObj->extra = pExtra;
		return TRUE;
	}

	return FALSE;
}

BOOL CTcpAgent::GetConnectionExtra(CONNID dwConnID, PVOID* ppExtra)
{
	TAgentSocketObj* pSocketObj = FindSocketObj(dwConnID);
	return GetConnectionExtra(pSocketObj, ppExtra);
}

BOOL CTcpAgent::GetConnectionExtra(TAgentSocketObj* pSocketObj, PVOID* ppExtra)
{
	ASSERT(ppExtra != nullptr);

	if(!TAgentSocketObj::IsExist(pSocketObj))
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
	else
	{
		*ppExtra = pSocketObj->extra;
		return TRUE;
	}

	return FALSE;
}

BOOL CTcpAgent::SetConnectionReserved(CONNID dwConnID, PVOID pReserved)
{
	TAgentSocketObj* pSocketObj = FindSocketObj(dwConnID);
	return SetConnectionReserved(pSocketObj, pReserved);
}

BOOL CTcpAgent::SetConnectionReserved(TAgentSocketObj* pSocketObj, PVOID pReserved)
{
	if(!TAgentSocketObj::IsExist(pSocketObj))
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
	else
	{
		pSocketObj->reserved = pReserved;
		return TRUE;
	}

	return FALSE;
}

BOOL CTcpAgent::GetConnectionReserved(CONNID dwConnID, PVOID* ppReserved)
{
	TAgentSocketObj* pSocketObj = FindSocketObj(dwConnID);
	return GetConnectionReserved(pSocketObj, ppReserved);
}

BOOL CTcpAgent::GetConnectionReserved(TAgentSocketObj* pSocketObj, PVOID* ppReserved)
{
	ASSERT(ppReserved != nullptr);

	if(!TAgentSocketObj::IsExist(pSocketObj))
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
	else
	{
		*ppReserved = pSocketObj->reserved;
		return TRUE;
	}

	return FALSE;
}

BOOL CTcpAgent::SetConnectionReserved2(CONNID dwConnID, PVOID pReserved2)
{
	TAgentSocketObj* pSocketObj = FindSocketObj(dwConnID);
	return SetConnectionReserved2(pSocketObj, pReserved2);
}

BOOL CTcpAgent::SetConnectionReserved2(TAgentSocketObj* pSocketObj, PVOID pReserved2)
{
	if(!TAgentSocketObj::IsExist(pSocketObj))
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
	else
	{
		pSocketObj->reserved2 = pReserved2;
		return TRUE;
	}

	return FALSE;
}

BOOL CTcpAgent::GetConnectionReserved2(CONNID dwConnID, PVOID* ppReserved2)
{
	TAgentSocketObj* pSocketObj = FindSocketObj(dwConnID);
	return GetConnectionReserved2(pSocketObj, ppReserved2);
}

BOOL CTcpAgent::GetConnectionReserved2(TAgentSocketObj* pSocketObj, PVOID* ppReserved2)
{
	ASSERT(ppReserved2 != nullptr);

	if(!TAgentSocketObj::IsExist(pSocketObj))
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
	else
	{
		*ppReserved2 = pSocketObj->reserved2;
		return TRUE;
	}

	return FALSE;
}

BOOL CTcpAgent::IsPauseReceive(CONNID dwConnID, BOOL& bPaused)
{
	TAgentSocketObj* pSocketObj = FindSocketObj(dwConnID);

	if(!TAgentSocketObj::IsValid(pSocketObj))
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
	else
	{
		bPaused = pSocketObj->paused;
		return TRUE;
	}

	return FALSE;
}

BOOL CTcpAgent::IsConnected(CONNID dwConnID)
{
	TAgentSocketObj* pSocketObj = FindSocketObj(dwConnID);

	if(!TAgentSocketObj::IsValid(pSocketObj))
	{
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
		return FALSE;
	}

	return pSocketObj->HasConnected();
}

BOOL CTcpAgent::GetPendingDataLength(CONNID dwConnID, int& iPending)
{
	TAgentSocketObj* pSocketObj = FindSocketObj(dwConnID);

	if(!TAgentSocketObj::IsValid(pSocketObj))
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
	else
	{
		iPending = pSocketObj->Pending();
		return TRUE;
	}

	return FALSE;
}

DWORD CTcpAgent::GetConnectionCount()
{
	return m_bfActiveSockets.Elements();
}

BOOL CTcpAgent::GetAllConnectionIDs(CONNID pIDs[], DWORD& dwCount)
{
	return m_bfActiveSockets.GetAllElementIndexes(pIDs, dwCount);
}

BOOL CTcpAgent::GetConnectPeriod(CONNID dwConnID, DWORD& dwPeriod)
{
	BOOL isOK					= TRUE;
	TAgentSocketObj* pSocketObj	= FindSocketObj(dwConnID);

	if(TAgentSocketObj::IsValid(pSocketObj))
		dwPeriod = ::GetTimeGap32(pSocketObj->connTime);
	else
	{
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
		isOK = FALSE;
	}

	return isOK;
}

BOOL CTcpAgent::GetSilencePeriod(CONNID dwConnID, DWORD& dwPeriod)
{
	if(!m_bMarkSilence)
		return FALSE;

	BOOL isOK					= TRUE;
	TAgentSocketObj* pSocketObj	= FindSocketObj(dwConnID);

	if(TAgentSocketObj::IsValid(pSocketObj))
		dwPeriod = ::GetTimeGap32(pSocketObj->activeTime);
	else
	{
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
		isOK = FALSE;
	}

	return isOK;
}

BOOL CTcpAgent::Disconnect(CONNID dwConnID, BOOL bForce)
{
	TAgentSocketObj* pSocketObj = FindSocketObj(dwConnID);

	if(!TAgentSocketObj::IsValid(pSocketObj))
	{
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
		return FALSE;
	}

	return m_ioDispatcher.SendCommandByFD(pSocketObj->socket, DISP_CMD_DISCONNECT, dwConnID, bForce);
}

BOOL CTcpAgent::DisconnectLongConnections(DWORD dwPeriod, BOOL bForce)
{
	if(dwPeriod > MAX_CONNECTION_PERIOD)
		return FALSE;
	if(m_bfActiveSockets.Elements() == 0)
		return TRUE;

	DWORD now = ::TimeGetTime();

	TAgentSocketObjPtrPool::IndexSet indexes;
	m_bfActiveSockets.CopyIndexes(indexes);

	for(auto it = indexes.begin(), end = indexes.end(); it != end; ++it)
	{
		CONNID connID				= *it;
		TAgentSocketObj* pSocketObj	= FindSocketObj(connID);

		if(TAgentSocketObj::IsValid(pSocketObj) && (int)(now - pSocketObj->connTime) >= (int)dwPeriod)
			Disconnect(connID, bForce);
	}

	return TRUE;
}

BOOL CTcpAgent::DisconnectSilenceConnections(DWORD dwPeriod, BOOL bForce)
{
	if(!m_bMarkSilence)
		return FALSE;
	if(dwPeriod > MAX_CONNECTION_PERIOD)
		return FALSE;
	if(m_bfActiveSockets.Elements() == 0)
		return TRUE;

	DWORD now = ::TimeGetTime();

	TAgentSocketObjPtrPool::IndexSet indexes;
	m_bfActiveSockets.CopyIndexes(indexes);

	for(auto it = indexes.begin(), end = indexes.end(); it != end; ++it)
	{
		CONNID connID				= *it;
		TAgentSocketObj* pSocketObj	= FindSocketObj(connID);

		if(TAgentSocketObj::IsValid(pSocketObj) && (int)(now - pSocketObj->activeTime) >= (int)dwPeriod)
			Disconnect(connID, bForce);
	}

	return TRUE;
}

BOOL CTcpAgent::PauseReceive(CONNID dwConnID, BOOL bPause)
{
	TAgentSocketObj* pSocketObj = FindSocketObj(dwConnID);

	if(!TAgentSocketObj::IsValid(pSocketObj))
	{
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
		return FALSE;
	}

	if(!pSocketObj->HasConnected())
	{
		::SetLastError(ERROR_INVALID_STATE);
		return FALSE;
	}

	if(pSocketObj->paused == bPause)
		return TRUE;

	pSocketObj->paused = bPause;

	if(!bPause)
		return m_ioDispatcher.SendCommandByFD(pSocketObj->socket, DISP_CMD_UNPAUSE, pSocketObj->connID);

	return TRUE;
}

BOOL CTcpAgent::OnBeforeProcessIo(const TDispContext* pContext, PVOID pv, UINT events)
{
	if(pv == this)
	{
		ReleaseGCSocketObj(FALSE);
		::ReadTimer(m_fdGCTimer);

		return FALSE;
	}

	TAgentSocketObj* pSocketObj = (TAgentSocketObj*)(pv);

	if(!TAgentSocketObj::IsValid(pSocketObj))
		return FALSE;

	if(events & _EPOLL_ALL_ERROR_EVENTS)
		pSocketObj->SetConnected(FALSE);

	pSocketObj->Increment();

	if(!TAgentSocketObj::IsValid(pSocketObj))
	{
		pSocketObj->Decrement();
		return FALSE;
	}

	if(pSocketObj->IsConnecting())
	{
		HandleConnect(pContext, pSocketObj, events);

		pSocketObj->Decrement();
		return FALSE;
	}

	return TRUE;
}

VOID CTcpAgent::OnAfterProcessIo(const TDispContext* pContext, PVOID pv, UINT events, BOOL rs)
{
	TAgentSocketObj* pSocketObj = (TAgentSocketObj*)(pv);

	if(TAgentSocketObj::IsValid(pSocketObj))
	{
		ASSERT(rs && !(events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)));

		UINT evts = (pSocketObj->IsPending() ? EPOLLOUT : 0) | (pSocketObj->IsPaused() ? 0 : EPOLLIN);
		m_ioDispatcher.ModFD(pSocketObj->socket, evts | EPOLLRDHUP, pSocketObj);
	}

	pSocketObj->Decrement();
}

VOID CTcpAgent::OnCommand(const TDispContext* pContext, TDispCommand* pCmd)
{
	switch(pCmd->type)
	{
	case DISP_CMD_SEND:
		HandleCmdSend(pContext, (CONNID)(pCmd->wParam));
		break;
	case DISP_CMD_UNPAUSE:
		HandleCmdUnpause(pContext, (CONNID)(pCmd->wParam));
		break;
	case DISP_CMD_DISCONNECT:
		HandleCmdDisconnect(pContext, (CONNID)(pCmd->wParam), (BOOL)pCmd->lParam);
		break;
	}
}

VOID CTcpAgent::HandleCmdSend(const TDispContext* pContext, CONNID dwConnID)
{
	TAgentSocketObj* pSocketObj = FindSocketObj(dwConnID);

	if(TAgentSocketObj::IsValid(pSocketObj) && pSocketObj->IsPending())
		m_ioDispatcher.ProcessIo(pContext, pSocketObj, EPOLLOUT);
}

VOID CTcpAgent::HandleCmdUnpause(const TDispContext* pContext, CONNID dwConnID)
{
	TAgentSocketObj* pSocketObj = FindSocketObj(dwConnID);

	if(!TAgentSocketObj::IsValid(pSocketObj))
		return;

	if(BeforeUnpause(pSocketObj))
		m_ioDispatcher.ProcessIo(pContext, pSocketObj, EPOLLIN);
	else
		AddFreeSocketObj(pSocketObj, SCF_ERROR, SO_RECEIVE, ENSURE_ERROR_CANCELLED);
}

VOID CTcpAgent::HandleCmdDisconnect(const TDispContext* pContext, CONNID dwConnID, BOOL bForce)
{
	TAgentSocketObj* pSocketObj = FindSocketObj(dwConnID);

	if(TAgentSocketObj::IsValid(pSocketObj))
		m_ioDispatcher.ProcessIo(pContext, pSocketObj, EPOLLHUP);
}

BOOL CTcpAgent::OnReadyRead(const TDispContext* pContext, PVOID pv, UINT events)
{
	return HandleReceive(pContext, (TAgentSocketObj*)pv, RETRIVE_EVENT_FLAG_H(events));
}

BOOL CTcpAgent::OnReadyWrite(const TDispContext* pContext, PVOID pv, UINT events)
{
	return HandleSend(pContext, (TAgentSocketObj*)pv, RETRIVE_EVENT_FLAG_H(events));
}

BOOL CTcpAgent::OnHungUp(const TDispContext* pContext, PVOID pv, UINT events)
{
	return HandleClose(pContext, (TAgentSocketObj*)pv, SCF_CLOSE, events);
}

BOOL CTcpAgent::OnError(const TDispContext* pContext, PVOID pv, UINT events)
{
	return HandleClose(pContext, (TAgentSocketObj*)pv, SCF_ERROR, events);
}

VOID CTcpAgent::OnDispatchThreadStart(THR_ID tid)
{
	OnWorkerThreadStart(tid);
}

VOID CTcpAgent::OnDispatchThreadEnd(THR_ID tid)
{
	OnWorkerThreadEnd(tid);
}

BOOL CTcpAgent::HandleClose(const TDispContext* pContext, TAgentSocketObj* pSocketObj, EnSocketCloseFlag enFlag, UINT events)
{
	EnSocketOperation enOperation = SO_CLOSE;

	if(events & _EPOLL_HUNGUP_EVENTS)
		enOperation = SO_CLOSE;
	else if(events & EPOLLIN)
		enOperation = SO_RECEIVE;
	else if(events & EPOLLOUT)
		enOperation = SO_SEND;

	int iErrorCode = 0;

	if(enFlag == SCF_ERROR)
		iErrorCode = ::SSO_GetError(pSocketObj->socket);

	AddFreeSocketObj(pSocketObj, enFlag, enOperation, iErrorCode);

	return TRUE;
}

BOOL CTcpAgent::HandleConnect(const TDispContext* pContext, TAgentSocketObj* pSocketObj, UINT events)
{
	int code = ::SSO_GetError(pSocketObj->socket);

	if(!IS_NO_ERROR(code) || (events & _EPOLL_ERROR_EVENTS))
	{
		AddFreeSocketObj(pSocketObj, SCF_ERROR, SO_CONNECT, code);
		return FALSE;
	}

	if((events & (_EPOLL_HUNGUP_EVENTS | _EPOLL_READ_EVENTS)) || !(events & EPOLLOUT))
	{
		AddFreeSocketObj(pSocketObj, SCF_CLOSE, SO_CONNECT, SE_OK);
		return FALSE;
	}

	pSocketObj->SetConnected();

	if(TRIGGER(FireConnect(pSocketObj)) == HR_ERROR)
	{
		AddFreeSocketObj(pSocketObj, SCF_NONE);
		return FALSE;
	}

	UINT evts = (pSocketObj->IsPending() ? EPOLLOUT : 0) | (pSocketObj->IsPaused() ? 0 : EPOLLIN);
	
	if(!m_ioDispatcher.ModFD(pSocketObj->socket, evts | EPOLLRDHUP, pSocketObj))
	{
		AddFreeSocketObj(pSocketObj, SCF_ERROR, SO_CONNECT, ::WSAGetLastError());
		return FALSE;
	}

	return TRUE;
}

BOOL CTcpAgent::HandleReceive(const TDispContext* pContext, TAgentSocketObj* pSocketObj, int flag)
{
	ASSERT(TAgentSocketObj::IsValid(pSocketObj));

	if(m_bMarkSilence) pSocketObj->activeTime = ::TimeGetTime();

	CBufferPtr& buffer = m_rcBuffers[pContext->GetIndex()];

	int reads = flag ? -1 : MAX_CONTINUE_READS;

	for(int i = 0; i < reads || reads < 0; i++)
	{
		if(pSocketObj->paused)
			break;

		int rc = (int)read(pSocketObj->socket, buffer.Ptr(), buffer.Size());

		if(rc > 0)
		{
			if(TRIGGER(FireReceive(pSocketObj, buffer.Ptr(), rc)) == HR_ERROR)
			{
				TRACE("<C-CNNID: %zu> OnReceive() event return 'HR_ERROR', connection will be closed !", pSocketObj->connID);

				AddFreeSocketObj(pSocketObj, SCF_ERROR, SO_RECEIVE, ENSURE_ERROR_CANCELLED);
				return FALSE;
			}
		}
		else if(rc == 0)
		{
			AddFreeSocketObj(pSocketObj, SCF_CLOSE, SO_RECEIVE, SE_OK);
			return FALSE;
		}
		else
		{
			ASSERT(rc == SOCKET_ERROR);

			int code = ::WSAGetLastError();

			if(code == ERROR_WOULDBLOCK)
				break;

			AddFreeSocketObj(pSocketObj, SCF_ERROR, SO_RECEIVE, code);
			return FALSE;
		}
	}

	return TRUE;
}

BOOL CTcpAgent::HandleSend(const TDispContext* pContext, TAgentSocketObj* pSocketObj, int flag)
{
	ASSERT(TAgentSocketObj::IsValid(pSocketObj));

	if(!pSocketObj->IsPending())
		return TRUE;

	BOOL bBlocked	= FALSE;
	int writes		= flag ? -1 : MAX_CONTINUE_WRITES;

	TBufferObjList& sndBuff = pSocketObj->sndBuff;
	TItemPtr itPtr(sndBuff);

	for(int i = 0; i < writes || writes < 0; i++)
	{
		{
			CReentrantCriSecLock locallock(pSocketObj->csSend);
			itPtr = sndBuff.PopFront();
		}

		if(!itPtr.IsValid())
			break;

		ASSERT(!itPtr->IsEmpty());

		if(!SendItem(pSocketObj, itPtr, bBlocked))
			return FALSE;

		if(bBlocked)
		{
			ASSERT(!itPtr->IsEmpty());

			CReentrantCriSecLock locallock(pSocketObj->csSend);
			sndBuff.PushFront(itPtr.Detach());

			break;
		}
	}

	return TRUE;
}

BOOL CTcpAgent::SendItem(TAgentSocketObj* pSocketObj, TItem* pItem, BOOL& bBlocked)
{
	while(!pItem->IsEmpty())
	{
		int rc = (int)write(pSocketObj->socket, pItem->Ptr(), pItem->Size());

		if(rc > 0)
		{
			if(TRIGGER(FireSend(pSocketObj, pItem->Ptr(), rc)) == HR_ERROR)
			{
				TRACE("<C-CNNID: %zu> OnSend() event should not return 'HR_ERROR' !!", pSocketObj->connID);
				ASSERT(FALSE);
			}

			pItem->Reduce(rc);
		}
		else if(rc == SOCKET_ERROR)
		{
			int code = ::WSAGetLastError();

			if(code == ERROR_WOULDBLOCK)
			{
				bBlocked = TRUE;
				break;
			}
			else
			{
				AddFreeSocketObj(pSocketObj, SCF_ERROR, SO_SEND, code);
				return FALSE;
			}
		}
		else
			ASSERT(FALSE);
	}

	return TRUE;
}

BOOL CTcpAgent::Send(CONNID dwConnID, const BYTE* pBuffer, int iLength, int iOffset)
{
	ASSERT(pBuffer && iLength > 0);

	if(iOffset != 0) pBuffer += iOffset;

	WSABUF buffer;
	buffer.len = iLength;
	buffer.buf = (BYTE*)pBuffer;

	return SendPackets(dwConnID, &buffer, 1);
}

BOOL CTcpAgent::DoSendPackets(CONNID dwConnID, const WSABUF pBuffers[], int iCount)
{
	ASSERT(pBuffers && iCount > 0);

	TAgentSocketObj* pSocketObj = FindSocketObj(dwConnID);

	if(!TAgentSocketObj::IsValid(pSocketObj))
	{
		::SetLastError(ERROR_OBJECT_NOT_FOUND);
		return FALSE;
	}

	return DoSendPackets(pSocketObj, pBuffers, iCount);
}

BOOL CTcpAgent::DoSendPackets(TAgentSocketObj* pSocketObj, const WSABUF pBuffers[], int iCount)
{
	ASSERT(pSocketObj && pBuffers && iCount > 0);

	int result = NO_ERROR;

	if(!pSocketObj->HasConnected())
	{
		::SetLastError(ERROR_INVALID_STATE);
		return FALSE;
	}

	if(pBuffers && iCount > 0)
	{
		CLocalSafeCounter localcounter(*pSocketObj);
		CReentrantCriSecLock locallock(pSocketObj->csSend);

		if(TAgentSocketObj::IsValid(pSocketObj))
			result = SendInternal(pSocketObj, pBuffers, iCount);
		else
			result = ERROR_OBJECT_NOT_FOUND;
	}
	else
		result = ERROR_INVALID_PARAMETER;

	if(result != NO_ERROR)
		::SetLastError(result);

	return (result == NO_ERROR);
}

int CTcpAgent::SendInternal(TAgentSocketObj* pSocketObj, const WSABUF pBuffers[], int iCount)
{
	BOOL bPending = pSocketObj->IsPending();

	for(int i = 0; i < iCount; i++)
	{
		int iBufLen = pBuffers[i].len;

		if(iBufLen > 0)
		{
			BYTE* pBuffer = (BYTE*)pBuffers[i].buf;
			ASSERT(pBuffer);

			pSocketObj->sndBuff.Cat(pBuffer, iBufLen);
			ASSERT(pSocketObj->sndBuff.Length() > 0);
		}
	}

	if(!bPending && pSocketObj->IsPending())
	{
		if(!m_ioDispatcher.SendCommandByFD(pSocketObj->socket, DISP_CMD_SEND, pSocketObj->connID))
			return ::GetLastError();
	}

	return NO_ERROR;
}

BOOL CTcpAgent::SendSmallFile(CONNID dwConnID, LPCTSTR lpszFileName, const LPWSABUF pHead, const LPWSABUF pTail)
{
	CFile file;
	CFileMapping fmap;
	WSABUF szBuf[3];

	HRESULT hr = ::MakeSmallFilePackage(lpszFileName, file, fmap, szBuf, pHead, pTail);

	if(FAILED(hr))
	{
		::SetLastError(hr);
		return FALSE;
	}

	return SendPackets(dwConnID, szBuf, 3);
}
