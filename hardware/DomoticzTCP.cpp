#include "stdafx.h"
#include "DomoticzTCP.h"
#include "../main/Logger.h"
#include "../main/Helper.h"
#include "../main/localtime_r.h"
#include "../main/mainworker.h"

#define RETRY_DELAY 30

DomoticzTCP::DomoticzTCP(const int ID, const std::string &IPAddress, const unsigned short usIPPort, const std::string &username, const std::string &password):
m_username(username), m_password(password), m_szIPAddress(IPAddress)
{
	m_HwdID=ID;
#if defined WIN32
	int ret;
	//Init winsock
	WSADATA data;
	WORD version; 

	version = (MAKEWORD(2, 2)); 
	ret = WSAStartup(version, &data); 
	if (ret != 0) 
	{  
		ret = WSAGetLastError(); 

		if (ret == WSANOTINITIALISED) 
		{  
			_log.Log(LOG_ERROR,"Domoticz: Winsock could not be initialized!");
		}
	}
#endif
	m_socket=INVALID_SOCKET;
	m_stoprequested=false;
	m_usIPPort=usIPPort;
}

DomoticzTCP::~DomoticzTCP(void)
{
#if defined WIN32
	//
	// Release WinSock
	//
	WSACleanup();
#endif
}

bool DomoticzTCP::StartHardware()
{
	m_bIsStarted=true;

	m_stoprequested=false;

	memset(&m_addr,0,sizeof(sockaddr_in));
	m_addr.sin_family = AF_INET;
	m_addr.sin_port = htons(m_usIPPort);

	unsigned long ip;
	ip=inet_addr(m_szIPAddress.c_str());

	// if we have a error in the ip, it means we have entered a string
	if(ip!=INADDR_NONE)
	{
		m_addr.sin_addr.s_addr=ip;
	}
	else
	{
		// change Hostname in serveraddr
		hostent *he=gethostbyname(m_szIPAddress.c_str());
		if(he==NULL)
		{
			return false;
		}
		else
		{
			memcpy(&(m_addr.sin_addr),he->h_addr_list[0],4);
		}
	}

	char szIP[20];
	unsigned char *pAddress=(unsigned char *)&m_addr.sin_addr;
	sprintf(szIP,"%d.%d.%d.%d",pAddress[0],pAddress[1],pAddress[2],pAddress[3]);
	m_endpoint=szIP;

	m_retrycntr=RETRY_DELAY; //will force reconnect first thing

	//Start worker thread
	m_thread = boost::shared_ptr<boost::thread>(new boost::thread(boost::bind(&DomoticzTCP::Do_Work, this)));

	return (m_thread!=NULL);
}

bool DomoticzTCP::StopHardware()
{
	if (isConnected())
	{
		try {
			disconnect();
		} catch(...)
		{
			//Don't throw from a Stop command
		}
	}
	else {
		try {
			if (m_thread)
			{
				m_stoprequested = true;
				m_thread->join();
			}
		}
		catch (...)
		{
			//Don't throw from a Stop command
		}
	}
	m_bIsStarted=false;
	return true;
}

bool DomoticzTCP::ConnectInternal()
{
	m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (m_socket == INVALID_SOCKET)
	{
		_log.Log(LOG_ERROR,"Domoticz: TCP could not create a TCP/IP socket!");
		return false;
	}
/*
	//Set socket timeout to 2 minutes
#if !defined WIN32
	struct timeval tv;
	tv.tv_sec = 120;
	setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO,(struct timeval *)&tv,sizeof(struct timeval));
#else
	unsigned long nTimeout = 120*1000;
	setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&nTimeout, sizeof(DWORD));
#endif
*/
	// connect to the server
	int nRet;
	nRet = connect(m_socket,(const sockaddr*)&m_addr, sizeof(m_addr));
	if (nRet == SOCKET_ERROR)
	{
		closesocket(m_socket);
		m_socket=INVALID_SOCKET;
		_log.Log(LOG_ERROR,"Domoticz: TCP could not connect to: %s:%ld",m_szIPAddress.c_str(),m_usIPPort);
		return false;
	}

	_log.Log(LOG_STATUS, "Domoticz: TCP connected to: %s:%ld",m_szIPAddress.c_str(),m_usIPPort);

	if (m_username!="")
	{
		char szAuth[300];
		sprintf(szAuth,"AUTH;%s;%s",m_username.c_str(),m_password.c_str());
		WriteToHardware((const char*)&szAuth,(const unsigned char)strlen(szAuth));
	}

	sOnConnected(this);
	return true;
}

void DomoticzTCP::disconnect()
{
	m_stoprequested=true;
	if (m_socket!=INVALID_SOCKET)
	{
		closesocket(m_socket);	//will terminate the thread
		m_socket=INVALID_SOCKET;
		sleep_seconds(1);
	}
	//m_thread-> join();
}

void DomoticzTCP::Do_Work()
{
	char buf[100];
	int sec_counter = 0;
	while (!m_stoprequested)
	{
		if (
			(m_socket == INVALID_SOCKET)&&
			(!m_stoprequested)
			)
		{
			sleep_seconds(1);
			sec_counter++;

			if (sec_counter % 12 == 0) {
				mytime(&m_LastHeartbeat);
			}

			if (m_stoprequested)
				break;
			m_retrycntr++;
			if (m_retrycntr>=RETRY_DELAY)
			{
				m_retrycntr=0;
				if (!ConnectInternal())
				{
					_log.Log(LOG_STATUS,"Domoticz: retrying in %d seconds...",RETRY_DELAY);
				}
			}
		}
		else
		{
			//this could take a long time... maybe there will be no data received at all,
			//so it's no good to-do the heartbeat timing here
			m_LastHeartbeat = mytime(NULL);

			int bread=recv(m_socket,(char*)&buf,sizeof(buf),0);
			if (m_stoprequested)
				break;
			if (bread<=0) {
				_log.Log(LOG_ERROR,"Domoticz: TCP/IP connection closed! %s",m_szIPAddress.c_str());
				closesocket(m_socket);
				m_socket=INVALID_SOCKET;
				if (!m_stoprequested)
				{
					_log.Log(LOG_STATUS,"Domoticz: retrying in %d seconds...",RETRY_DELAY);
					m_retrycntr=0;
					continue;
				}
			}
			else
			{
				boost::lock_guard<boost::mutex> l(readQueueMutex);
				onRFXMessage((const unsigned char *)&buf,bread);
			}
		}
		
	}
	_log.Log(LOG_STATUS,"Domoticz: TCP/IP Worker stopped...");
} 

void DomoticzTCP::write(const char *data, size_t size)
{
	if (m_socket==INVALID_SOCKET)
		return; //not connected!
	send(m_socket,data,size,0);
}

bool DomoticzTCP::WriteToHardware(const char *pdata, const unsigned char length)
{
	if (isConnected())
	{
		write(pdata, length);
		return true;
	}
	return false;
}
