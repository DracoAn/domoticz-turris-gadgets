#include "stdafx.h"
#include "EnOceanESP3.h"
#include "../main/Logger.h"
#include "../main/Helper.h"
#include "../main/RFXtrx.h"
#include "../main/SQLHelper.h"

#include <string>
#include <algorithm>
#include <iostream>
#include <boost/bind.hpp>
#include "hardwaretypes.h"
#include "../main/localtime_r.h"

#include <ctime>

#define ENABLE_LOGGING

#define ENOCEAN_RETRY_DELAY 30

//Write/Read has to be done in sync with ESP3

#define ESP3_SYNC 0x55
#define ESP3_HEADER_LENGTH 0x4

#define round(a) ( int ) ( a + .5 )

extern const char* Get_EnoceanManufacturer(unsigned long ID);
extern const char* Get_Enocean4BSType(const int Org, const int Func, const int Type);

// the following lines are taken from EO300I API header file

//polynomial G(x) = x8 + x2 + x1 + x0 is used to generate the CRC8 table
const unsigned char crc8table[256] = { 
	0x00, 0x07, 0x0e, 0x09, 0x1c, 0x1b, 0x12, 0x15, 
	0x38, 0x3f, 0x36, 0x31, 0x24, 0x23, 0x2a, 0x2d, 
	0x70, 0x77, 0x7e, 0x79, 0x6c, 0x6b, 0x62, 0x65, 
	0x48, 0x4f, 0x46, 0x41, 0x54, 0x53, 0x5a, 0x5d, 
	0xe0, 0xe7, 0xee, 0xe9, 0xfc, 0xfb, 0xf2, 0xf5, 
	0xd8, 0xdf, 0xd6, 0xd1, 0xc4, 0xc3, 0xca, 0xcd, 
	0x90, 0x97, 0x9e, 0x99, 0x8c, 0x8b, 0x82, 0x85, 
	0xa8, 0xaf, 0xa6, 0xa1, 0xb4, 0xb3, 0xba, 0xbd,  
	0xc7, 0xc0, 0xc9, 0xce, 0xdb, 0xdc, 0xd5, 0xd2, 
	0xff, 0xf8, 0xf1, 0xf6, 0xe3, 0xe4, 0xed, 0xea, 
	0xb7, 0xb0, 0xb9, 0xbe, 0xab, 0xac, 0xa5, 0xa2, 
	0x8f, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9d, 0x9a, 
	0x27, 0x20, 0x29, 0x2e, 0x3b, 0x3c, 0x35, 0x32, 
	0x1f, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0d, 0x0a, 
	0x57, 0x50, 0x59, 0x5e, 0x4b, 0x4c, 0x45, 0x42, 
	0x6f, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7d, 0x7a, 
	0x89, 0x8e, 0x87, 0x80, 0x95, 0x92, 0x9b, 0x9c, 
	0xb1, 0xb6, 0xbf, 0xb8, 0xad, 0xaa, 0xa3, 0xa4, 
	0xf9, 0xfe, 0xf7, 0xf0, 0xe5, 0xe2, 0xeb, 0xec, 
	0xc1, 0xc6, 0xcf, 0xc8, 0xdd, 0xda, 0xd3, 0xd4, 
	0x69, 0x6e, 0x67, 0x60, 0x75, 0x72, 0x7b, 0x7c, 
	0x51, 0x56, 0x5f, 0x58, 0x4d, 0x4a, 0x43, 0x44, 
	0x19, 0x1e, 0x17, 0x10, 0x05, 0x02, 0x0b, 0x0c, 
	0x21, 0x26, 0x2f, 0x28, 0x3d, 0x3a, 0x33, 0x34, 
	0x4e, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5c, 0x5b, 
	0x76, 0x71, 0x78, 0x7f, 0x6A, 0x6d, 0x64, 0x63, 
	0x3e, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2c, 0x2b, 
	0x06, 0x01, 0x08, 0x0f, 0x1a, 0x1d, 0x14, 0x13, 
	0xae, 0xa9, 0xa0, 0xa7, 0xb2, 0xb5, 0xbc, 0xbb, 
	0x96, 0x91, 0x98, 0x9f, 0x8a, 0x8D, 0x84, 0x83, 
	0xde, 0xd9, 0xd0, 0xd7, 0xc2, 0xc5, 0xcc, 0xcb, 
	0xe6, 0xe1, 0xe8, 0xef, 0xfa, 0xfd, 0xf4, 0xf3 
}; 

#define proc_crc8(crc, data) (crc8table[crc ^ data])

#define SER_SYNCH_CODE 0x55
#define SER_HEADER_NR_BYTES 0x04

//! Packet structure (ESP3)
typedef struct
{
	// Amount of raw data bytes to be received. The most significant byte is sent/received first
	unsigned short u16DataLength;
	// Amount of optional data bytes to be received
	unsigned char u8OptionLength;
	// Packe type code
	unsigned char u8Type;
	// Data buffer: raw data + optional bytes
	unsigned char *u8DataBuffer;
} PACKET_SERIAL_TYPE;

//! Packet type (ESP3)
typedef enum
{
	PACKET_RESERVED 			= 0x00,	//! Reserved
	PACKET_RADIO 				= 0x01,	//! Radio telegram
	PACKET_RESPONSE				= 0x02,	//! Response to any packet
	PACKET_RADIO_SUB_TEL		= 0x03,	//! Radio sub telegram (EnOcean internal function )
	PACKET_EVENT 				= 0x04,	//! Event message
	PACKET_COMMON_COMMAND 		= 0x05,	//! Common command
	PACKET_SMART_ACK_COMMAND	= 0x06,	//! Smart Ack command
	PACKET_REMOTE_MAN_COMMAND	= 0x07,	//! Remote management command
	PACKET_PRODUCTION_COMMAND	= 0x08,	//! Production command
	PACKET_RADIO_MESSAGE		= 0x09,	//! Radio message (chained radio telegrams)
	PACKET_RADIO_ADVANCED		= 0x0a  //! Advanced Protocol radio telegram

} PACKET_TYPE;

//! Response type
typedef enum
{
	RET_OK 					= 0x00, //! OK ... command is understood and triggered
	RET_ERROR 				= 0x01, //! There is an error occured
	RET_NOT_SUPPORTED 		= 0x02, //! The functionality is not supported by that implementation
	RET_WRONG_PARAM 		= 0x03, //! There was a wrong parameter in the command
	RET_OPERATION_DENIED 	= 0x04, //! Example: memory access denied (code-protected)
	RET_USER				= 0x80	//! Return codes greater than 0x80 are used for commands with special return information, not commonly useable.
} RESPONSE_TYPE;

//! Common command enum
typedef enum
{
	CO_WR_SLEEP			= 1,	//! Order to enter in energy saving mode
	CO_WR_RESET			= 2,	//! Order to reset the device
	CO_RD_VERSION		= 3,	//! Read the device (SW) version / (HW) version, chip ID etc.
	CO_RD_SYS_LOG		= 4,	//! Read system log from device databank
	CO_WR_SYS_LOG		= 5,	//! Reset System log from device databank
	CO_WR_BIST			= 6,	//! Perform Flash BIST operation
	CO_WR_IDBASE		= 7,	//! Write ID range base number
	CO_RD_IDBASE		= 8,	//! Read ID range base number
	CO_WR_REPEATER		= 9,	//! Write Repeater Level off,1,2
	CO_RD_REPEATER		= 10,	//! Read Repeater Level off,1,2
	CO_WR_FILTER_ADD	= 11,	//! Add filter to filter list
	CO_WR_FILTER_DEL	= 12,	//! Delete filter from filter list
	CO_WR_FILTER_DEL_ALL= 13,	//! Delete filters
	CO_WR_FILTER_ENABLE	= 14,	//! Enable/Disable supplied filters
	CO_RD_FILTER		= 15,	//! Read supplied filters
	CO_WR_WAIT_MATURITY	= 16,	//! Waiting till end of maturity time before received radio telegrams will transmitted
	CO_WR_SUBTEL		= 17,	//! Enable/Disable transmitting additional subtelegram info
	CO_WR_MEM			= 18,	//! Write x bytes of the Flash, XRAM, RAM0 �.
	CO_RD_MEM			= 19,	//! Read x bytes of the Flash, XRAM, RAM0 �.
	CO_RD_MEM_ADDRESS	= 20,	//! Feedback about the used address and length of the config area and the Smart Ack Table
	CO_RD_SECURITY		= 21,	//! Read security informations (level, keys)
	CO_WR_SECURITY		= 22,	//! Write security informations (level, keys)
} COMMON_COMMAND_TYPE; 

typedef enum {
	RORG_RPS = 0xF6,
	RORG_1BS = 0xD5,
	RORG_4BS = 0xA5,
	RORG_VLD = 0xD2,
	RORG_MSC = 0xD1,
	RORG_ADT = 0xA6,
	RORG_SM_LRN_REQ = 0xC6,
	RORG_SM_LRN_ANS = 0xC7, 
	RORG_SM_REC = 0xA7,
	RORG_SYS_EX = 0xC5
} ESP3_RORG;

//! Function return codes
typedef enum
{
	//! <b>0</b> - Action performed. No problem detected
	OK=0,							
	//! <b>1</b> - Action couldn't be carried out within a certain time.  
	TIME_OUT,		
	//! <b>2</b> - The write/erase/verify process failed, the flash page seems to be corrupted
	FLASH_HW_ERROR,				
	//! <b>3</b> - A new UART/SPI byte received
	NEW_RX_BYTE,				
	//! <b>4</b> - No new UART/SPI byte received	
	NO_RX_BYTE,					
	//! <b>5</b> - New telegram received
	NEW_RX_TEL,	  
	//! <b>6</b> - No new telegram received
	NO_RX_TEL,	  
	//! <b>7</b> - Checksum not valid
	NOT_VALID_CHKSUM,
	//! <b>8</b> - Telegram not valid  
	NOT_VALID_TEL,
	//! <b>9</b> - Buffer full, no space in Tx or Rx buffer
	BUFF_FULL,
	//! <b>10</b> - Address is out of memory
	ADDR_OUT_OF_MEM,
	//! <b>11</b> - Invalid function parameter
	NOT_VALID_PARAM,
	//! <b>12</b> - Built in self test failed
	BIST_FAILED,
	//! <b>13</b> - Before entering power down, the short term timer had timed out.	
	ST_TIMEOUT_BEFORE_SLEEP,
	//! <b>14</b> - Maximum number of filters reached, no more filter possible
	MAX_FILTER_REACHED,
	//! <b>15</b> - Filter to delete not found
	FILTER_NOT_FOUND,
	//! <b>16</b> - BaseID out of range
	BASEID_OUT_OF_RANGE,
	//! <b>17</b> - BaseID was changed 10 times, no more changes are allowed
	BASEID_MAX_REACHED,
	//! <b>18</b> - XTAL is not stable
	XTAL_NOT_STABLE,
	//! <b>19</b> - No telegram for transmission in queue  
	NO_TX_TEL,
	//!	<b>20</b> - Waiting before sending broadcast message
	TELEGRAM_WAIT,
	//!	<b>21</b> - Generic out of range return code
	OUT_OF_RANGE,
	//!	<b>22</b> - Function was not executed due to sending lock
	LOCK_SET,
	//! <b>23</b> - New telegram transmitted
	NEW_TX_TEL
} RETURN_TYPE;
// end of lines from EO300I API header file

/**
 * @defgroup bitmasks Bitmasks for various fields.
 * There are two definitions for every bit mask. First, the bit mask itself
 * and also the number of necessary shifts.
 * @{
 */
/**
 * @defgroup status_rps Status of telegram (for RPS telegrams)
 * Bitmasks for the status-field, if ORG = RPS.
 * @{
 */
#define S_RPS_T21 0x20
#define S_RPS_T21_SHIFT 5
#define S_RPS_NU  0x10
#define S_RPS_NU_SHIFT 4
#define S_RPS_RPC 0x0F
#define S_RPS_RPC_SHIFT 0
/*@}*/
/**
 * @defgroup status_rpc Status of telegram (for 1BS, 4BS, HRC or 6DT telegrams)
 * Bitmasks for the status-field, if ORG = 1BS, 4BS, HRC or 6DT.
 * @{
 */
#define S_RPC 0x0F
#define S_RPC_SHIFT 0
/*@}*/

/**
 * @defgroup data3 Meaning of data_byte 3 (for RPS telegrams, NU = 1)
 * Bitmasks for the data_byte3-field, if ORG = RPS and NU = 1.
 * @{
 */
#define DB3_RPS_NU_RID 0xC0
#define DB3_RPS_NU_RID_SHIFT 6
#define DB3_RPS_NU_UD  0x20
#define DB3_RPS_NU_UD_SHIFT 5
#define DB3_RPS_NU_PR  0x10
#define DB3_RPS_NU_PR_SHIFT 4
#define DB3_RPS_NU_SRID 0x0C
#define DB3_RPS_NU_SRID_SHIFT 2
#define DB3_RPS_NU_SUD 0x02
#define DB3_RPS_NU_SUD_SHIFT 1
#define DB3_RPS_NU_SA 0x01
#define DB3_RPS_NU_SA_SHIFT 0
/*@}*/

/**
 * @defgroup data3_1 Meaning of data_byte 3 (for RPS telegrams, NU = 0)
 * Bitmasks for the data_byte3-field, if ORG = RPS and NU = 0.
 * @{
 */
#define DB3_RPS_BUTTONS 0xE0
#define DB3_RPS_BUTTONS_SHIFT 5
#define DB3_RPS_PR 0x10
#define DB3_RPS_PR_SHIFT 4
/*@}*/

/**
 * @defgroup data0 Meaning of data_byte 0 (for 4BS telegrams)
 * Bitmasks for the data_byte0-field, if ORG = 4BS.
 * @{
 */
#define DB0_4BS_DI_3 0x08
#define DB0_4BS_DI_3_SHIFT 3
#define DB0_4BS_DI_2 0x04
#define DB0_4BS_DI_2_SHIFT 2
#define DB0_4BS_DI_1 0x02
#define DB0_4BS_DI_1_SHIFT 1
#define DB0_4BS_DI_0 0x01
#define DB0_4BS_DI_0_SHIFT 0
/*@}*/

/**
 * @defgroup data3_hrc Meaning of data_byte 3 (for HRC telegrams)
 * Bitmasks for the data_byte3-field, if ORG = HRC.
 * @{
 */
#define DB3_HRC_RID 0xC0
#define DB3_HRC_RID_SHIFT 6
#define DB3_HRC_UD  0x20
#define DB3_HRC_UD_SHIFT 5
#define DB3_HRC_PR  0x10
#define DB3_HRC_PR_SHIFT 4
#define DB3_HRC_SR  0x08
#define DB3_HRC_SR_SHIFT 3

bool CEnOceanESP3::sendFrame(unsigned char frametype, unsigned char *databuf, unsigned short datalen, unsigned char *optdata, unsigned char optdatalen)
{
	unsigned char crc=0;
	unsigned char buf[1024];
	int len=0;

	buf[len++]=SER_SYNCH_CODE;
	buf[len++]=(datalen >> 8) & 0xff; // len
	buf[len++]=datalen & 0xff;
	buf[len++]=optdatalen;
	buf[len++]=frametype;
	crc = proc_crc8(crc, buf[1]);
	crc = proc_crc8(crc, buf[2]);
	crc = proc_crc8(crc, buf[3]);
	crc = proc_crc8(crc, buf[4]);
	buf[len++]=crc;
	crc = 0;
	for (int i=0;i<datalen;i++) {
		buf[len]=databuf[i];
		crc=proc_crc8(crc, buf[len++]);
	}
	for (int i=0;i<optdatalen;i++) {
		buf[len]=optdata[i];
		crc=proc_crc8(crc, buf[len++]);
	}
	buf[len++]=crc;
	write((const char*)&buf,len);
	return true;
} 

bool CEnOceanESP3::sendFrameQueue(unsigned char frametype, unsigned char *databuf, unsigned short datalen, unsigned char *optdata, unsigned char optdatalen)
{
	unsigned char crc=0;
	unsigned char buf[1024];
	int len=0;

	buf[len++]=SER_SYNCH_CODE;
	buf[len++]=(datalen >> 8) & 0xff; // len
	buf[len++]=datalen & 0xff;
	buf[len++]=optdatalen;
	buf[len++]=frametype;
	crc = proc_crc8(crc, buf[1]);
	crc = proc_crc8(crc, buf[2]);
	crc = proc_crc8(crc, buf[3]);
	crc = proc_crc8(crc, buf[4]);
	buf[len++]=crc;
	crc = 0;
	for (int i=0;i<datalen;i++) {
		buf[len]=databuf[i];
		crc=proc_crc8(crc, buf[len++]);
	}
	for (int i=0;i<optdatalen;i++) {
		buf[len]=optdata[i];
		crc=proc_crc8(crc, buf[len++]);
	}
	buf[len++]=crc;
	Add2SendQueue((const char*)&buf,len);
	return true;
} 

CEnOceanESP3::CEnOceanESP3(const int ID, const std::string& devname, const int type)
{
	m_HwdID=ID;
	m_szSerialPort=devname;
    m_Type = type;
	m_bufferpos=0;
	memset(&m_buffer,0,sizeof(m_buffer));
	m_id_base=0;
	m_receivestate=ERS_SYNCBYTE;
	m_stoprequested=false;

	//Test
	//m_ReceivedPacketType = 0x01;
	//m_DataSize = 0x0A;
	//m_OptionalDataSize = 0x07;
	//m_bufferpos = 0;
	//m_buffer[m_bufferpos++] = 0xA5;
	//ParseData();
}

CEnOceanESP3::~CEnOceanESP3()
{
	clearReadCallback();
}

bool CEnOceanESP3::StartHardware()
{
	m_retrycntr=ENOCEAN_RETRY_DELAY*5; //will force reconnect first thing

	//Start worker thread
	m_thread = boost::shared_ptr<boost::thread>(new boost::thread(boost::bind(&CEnOceanESP3::Do_Work, this)));

	return (m_thread!=NULL);
}

bool CEnOceanESP3::StopHardware()
{
	m_stoprequested=true;
	if (m_thread)
	{
		m_thread->join();
		// Wait a while. The read thread might be reading. Adding this prevents a pointer error in the async serial class.
		sleep_milliseconds(10);
	}
	if (isOpen())
	{
		try {
			clearReadCallback();
			close();
			doClose();
			setErrorStatus(true);
		} catch(...)
		{
			//Don't throw from a Stop command
		}
	}
	m_bIsStarted=false;
	return true;
}


void CEnOceanESP3::Do_Work()
{
	int msec_counter=0;
	int sec_counter = 0;
	while (!m_stoprequested)
	{
		sleep_milliseconds(200);
		if (m_stoprequested)
			break;

		msec_counter++;
		if (msec_counter == 5)
		{
			msec_counter = 0;
			sec_counter++;
			if (sec_counter % 12 == 0)
			{
				m_LastHeartbeat = mytime(NULL);
			}
		}

		if (!isOpen())
		{
			if (m_retrycntr==0)
			{
				_log.Log(LOG_STATUS,"EnOcean: serial retrying in %d seconds...", ENOCEAN_RETRY_DELAY);
			}
			m_retrycntr++;
			if (m_retrycntr/5>=ENOCEAN_RETRY_DELAY)
			{
				m_retrycntr=0;
				m_bufferpos=0;
				OpenSerialDevice();
			}
		}
		if (m_sendqueue.size()>0)
		{
			boost::lock_guard<boost::mutex> l(m_sendMutex);

			std::vector<std::string>::iterator itt=m_sendqueue.begin();
			if (itt!=m_sendqueue.end())
			{
				std::string sBytes=*itt;
				write(sBytes.c_str(),sBytes.size());
				m_sendqueue.erase(itt);
			}
		}
	}
	_log.Log(LOG_STATUS,"EnOcean: Serial Worker stopped...");
}

void CEnOceanESP3::Add2SendQueue(const char* pData, const size_t length)
{
	std::string sBytes;
	sBytes.insert(0,pData,length);
	boost::lock_guard<boost::mutex> l(m_sendMutex);
	m_sendqueue.push_back(sBytes);
}


bool CEnOceanESP3::OpenSerialDevice()
{
	//Try to open the Serial Port
	try
	{
		open(m_szSerialPort, 57600); //ECP3 open with 57600
		_log.Log(LOG_STATUS,"EnOcean: Using serial port: %s", m_szSerialPort.c_str());
	}
	catch (boost::exception & e)
	{
		_log.Log(LOG_ERROR,"EnOcean: Error opening serial port!");
#ifdef _DEBUG
		_log.Log(LOG_ERROR,"-----------------\n%s\n----------------", boost::diagnostic_information(e).c_str());
#endif
		return false;
	}
	catch ( ... )
	{
		_log.Log(LOG_ERROR,"EnOcean: Error opening serial port!!!");
		return false;
	}
	m_bIsStarted=true;

	m_receivestate=ERS_SYNCBYTE;
	setReadCallback(boost::bind(&CEnOceanESP3::readCallback, this, _1, _2));
	sOnConnected(this);

	uint8_t buf[100];

	//Request BASE_ID
	m_bBaseIDRequested=true;
	buf[0] = CO_RD_IDBASE;
	sendFrameQueue(PACKET_COMMON_COMMAND,buf,1,NULL,0); 

	//Request Version
	buf[0] = CO_RD_VERSION;
	sendFrameQueue(PACKET_COMMON_COMMAND,buf,1,NULL,0); 

	return true;
}

void CEnOceanESP3::readCallback(const char *data, size_t len)
{
	boost::lock_guard<boost::mutex> l(readQueueMutex);
	size_t ii=0;

	while (ii<len)
	{
		const unsigned char c = data[ii];

		switch (m_receivestate)
		{
		case ERS_SYNCBYTE:
			if (c!=0x55)
				return;
			m_receivestate = ERS_HEADER;
			m_bufferpos=0;
			break;
		case ERS_HEADER:
			m_buffer[m_bufferpos++]=c;
			if (m_bufferpos==5)
			{
				m_DataSize=(m_buffer[0]<<8)|m_buffer[1];
				m_OptionalDataSize=m_buffer[2];
				m_ReceivedPacketType=m_buffer[3];
				unsigned char CRCH=m_buffer[4];

				unsigned char crc=0;
				crc = proc_crc8(crc, m_buffer[0]);
				crc = proc_crc8(crc, m_buffer[1]);
				crc = proc_crc8(crc, m_buffer[2]);
				crc = proc_crc8(crc, m_buffer[3]);

				if (CRCH==crc)
				{
					m_bufferpos=0;
					m_wantedlength=m_DataSize+m_OptionalDataSize;
					m_receivestate = ERS_DATA;
				}
				else
				{
					_log.Log(LOG_ERROR,"EnOcean: Frame Checksum Error!...");
					m_receivestate = ERS_SYNCBYTE;
				}
			}
			break;
		case ERS_DATA:
			m_buffer[m_bufferpos++] = c;
			if (m_bufferpos>=m_wantedlength)
			{
				m_receivestate = ERS_CHECKSUM;
			}
			break;
		case ERS_CHECKSUM:
			{
				unsigned char CRCD=c;
				unsigned char crc=0;
				for (int iCheck=0; iCheck<m_wantedlength; iCheck++)
					crc = proc_crc8(crc, m_buffer[iCheck]);
				if (CRCD==crc)
				{
					ParseData();
				}
				m_receivestate = ERS_SYNCBYTE;
			}
			break;
		}
		ii++;
	}

}

bool CEnOceanESP3::WriteToHardware(const char *pdata, const unsigned char length)
{
	if (m_id_base==0)
		return false;
	if (!isOpen())
		return false;
	RBUF *tsen=(RBUF*)pdata;
	if (tsen->LIGHTING2.packettype!=pTypeLighting2)
		return false; //only allowed to control switches

	unsigned long sID=(tsen->LIGHTING2.id1<<24)|(tsen->LIGHTING2.id2<<16)|(tsen->LIGHTING2.id3<<8)|tsen->LIGHTING2.id4;
	if ((sID<m_id_base)||(sID>m_id_base+129))
	{
		_log.Log(LOG_ERROR,"EnOcean: Can not switch with this DeviceID, use a switch created with our id_base!...");
		return false;
	}

	unsigned char buf[100];
	buf[0]=0xa5;
	buf[1]=0x2;
	buf[2]=100;	//level
	buf[3]=1;	//speed
	buf[4]=0x09; // Dim Off

	buf[5]=(sID >> 24) & 0xff;
	buf[6]=(sID >> 16) & 0xff;
	buf[7]=(sID >> 8) & 0xff;
	buf[8]=sID & 0xff;

	buf[9]=0x30; // status

	unsigned char RockerID=0;
	unsigned char Pressed=1;

	if (tsen->LIGHTING2.unitcode < 10)
	{
		RockerID = tsen->LIGHTING2.unitcode - 1;
	}
	else
		return false;//double not supported yet!


	//First we need to find out if this is a Dimmer switch,
	//because they are threaded differently
	bool bIsDimmer=false;
	int LastLevel=0;
	std::stringstream szQuery;
	std::vector<std::vector<std::string> > result;
	char szDeviceID[20];
	sprintf(szDeviceID,"%08X",(unsigned int)sID);
	szQuery << "SELECT SwitchType,LastLevel FROM DeviceStatus WHERE (HardwareID==" << m_HwdID << ") AND (DeviceID=='" << szDeviceID << "') AND (Unit==" << int(tsen->LIGHTING2.unitcode) << ")";
	result=m_sql.query(szQuery.str());
	if (result.size()>0)
	{
		_eSwitchType switchtype=(_eSwitchType)atoi(result[0][0].c_str());
		if (switchtype==STYPE_Dimmer)
			bIsDimmer=true;
		LastLevel=atoi(result[0][1].c_str());
	}

	int iLevel=tsen->LIGHTING2.level;
	int cmnd=tsen->LIGHTING2.cmnd;
	int orgcmd=cmnd;
	if ((tsen->LIGHTING2.level==0)&&(!bIsDimmer))
		cmnd=light2_sOff;
	else
	{
		if (cmnd==light2_sOn)
		{
			iLevel=LastLevel;
		}
		else
		{
			//scale to 0 - 100 %
			iLevel=tsen->LIGHTING2.level;
			if (iLevel>15)
				iLevel=15;
			float fLevel=(100.0f/15.0f)*float(iLevel);
			if (fLevel>99.0f)
				fLevel=100.0f;
			iLevel=int(fLevel);
		}
		cmnd=light2_sSetLevel;
	}

	if (cmnd!=light2_sSetLevel)
	{
		//On/Off
		unsigned char UpDown = 1;
		UpDown = ((cmnd != light2_sOff) && (cmnd != light2_sGroupOff));


		buf[1] = (RockerID<<DB3_RPS_NU_RID_SHIFT) | (UpDown<<DB3_RPS_NU_UD_SHIFT) | (Pressed<<DB3_RPS_NU_PR_SHIFT);//0x30;
		buf[9] = 0x30;

		sendFrameQueue(PACKET_RADIO,buf,10,NULL,0);

		//Next command is send a bit later (button release)
		buf[1] = 0;
		buf[9] = 0x20;
		sendFrameQueue(PACKET_RADIO,buf,10,NULL,0);
	}
	else
	{
		//Send dim value

		//Dim On DATA_BYTE0 = 0x09
		//Dim Off DATA_BYTE0 = 0x08

		buf[1]=2;
		buf[2]=iLevel;
		buf[3]=1;//very fast dimming

		if ((iLevel==0)||(orgcmd==light2_sOff))
			buf[4]=0x08; //Dim Off
		else
			buf[4]=0x09;//Dim On

		sendFrameQueue(PACKET_RADIO,buf,10,NULL,0);
	}
	return true;
}

void CEnOceanESP3::SendDimmerTeachIn(const char *pdata, const unsigned char length)
{
	if (m_id_base==0)
		return;
	if (isOpen()) {
		RBUF *tsen = (RBUF*)pdata;
		if (tsen->LIGHTING2.packettype != pTypeLighting2)
			return; //only allowed to control switches
		unsigned long sID = (tsen->LIGHTING2.id1 << 24) | (tsen->LIGHTING2.id2 << 16) | (tsen->LIGHTING2.id3 << 8) | tsen->LIGHTING2.id4;
		if ((sID<m_id_base) || (sID>m_id_base + 129))
		{
			_log.Log(LOG_ERROR, "EnOcean: Can not switch with this DeviceID, use a switch created with our id_base!...");
			return;
		}

		unsigned char buf[100];
		buf[0] = 0xa5;
		buf[1] = 0x2;
		buf[2] = 0;
		buf[3] = 0;
		buf[4] = 0x0; // DB0.3=0 -> teach in

		buf[5] = (sID >> 24) & 0xff;
		buf[6] = (sID >> 16) & 0xff;
		buf[7] = (sID >> 8) & 0xff;
		buf[8] = sID & 0xff;

		buf[9] = 0x30; // status

		if (tsen->LIGHTING2.unitcode < 10)
		{
			unsigned char RockerID = 0;
			//unsigned char UpDown = 1;
			//unsigned char Pressed = 1;
			RockerID = tsen->LIGHTING2.unitcode - 1;
		}
		else
		{
			return;//double not supported yet!
		}
		sendFrame(PACKET_RADIO,buf,10,NULL,0);
	}
}

float CEnOceanESP3::GetValueRange(const float InValue, const float ScaleMax, const float ScaleMin, const float RangeMax, const float RangeMin)
{
	float vscale=ScaleMax-ScaleMin;
	if (vscale==0)
		return 0.0f;
	float vrange=RangeMax-RangeMin;
	if (vrange==0)
		return 0.0f;
	float multiplyer=vscale/vrange;
	return multiplyer*(InValue-RangeMin)+ScaleMin;
}

bool CEnOceanESP3::ParseData()
{
#ifdef ENABLE_LOGGING
	std::stringstream sstr;

	sstr << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (unsigned int)m_ReceivedPacketType << " (";
	sstr << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (unsigned int)m_DataSize << "/";
	sstr << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (unsigned int)m_OptionalDataSize << ") ";

	for (int idx=0;idx<m_bufferpos;idx++)
	{
		sstr << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (unsigned int)m_buffer[idx];
		if (idx!=m_bufferpos-1)
			sstr << " ";
	}
	_log.Log(LOG_STATUS,"EnOcean: %s",sstr.str().c_str());	
#endif

	if (m_ReceivedPacketType==PACKET_RESPONSE)
	{
		//Response
		unsigned char ResponseCode=m_buffer[0];
		if (ResponseCode!=0)
		{
			std::string szError="Unknown?";
			switch (ResponseCode)
			{
			case RET_ERROR:
				szError="RET_ERROR";
				break;
			case RET_NOT_SUPPORTED:
				szError="RET_NOT_SUPPORTED";
				break;
			case RET_WRONG_PARAM:
				szError="RET_WRONG_PARAM";
				break;
			case RET_OPERATION_DENIED:
				szError="RET_OPERATION_DENIED";
				break;
			}
			_log.Log(LOG_ERROR,"EnOcean: Response Error (Code: %d, %s)",ResponseCode,szError.c_str());
			return false;
		}
		if ((m_bBaseIDRequested)&&(m_bufferpos==6))
		{
			m_bBaseIDRequested=false;
			m_id_base = (m_buffer[1] << 24) + (m_buffer[2] << 16) + (m_buffer[3] << 8) + m_buffer[4];
			unsigned char changes_left=m_buffer[5];
			_log.Log(LOG_STATUS,"EnOcean: Transceiver ID_Base: 0x%08x",m_id_base);
		}
		if (m_bufferpos==33)
		{
			//Version Information
			_log.Log(LOG_STATUS,"EnOcean: Version_Info, App: %02x.%02x.%02x.%02x, API: %02x.%02x.%02x.%02x, ChipID: %02x.%02x.%02x.%02x, ChipVersion: %02x.%02x.%02x.%02x, Description: %s",
				m_buffer[1],m_buffer[2],m_buffer[3],m_buffer[4],
				m_buffer[5],m_buffer[6],m_buffer[7],m_buffer[8],
				m_buffer[9],m_buffer[10],m_buffer[11],m_buffer[12],
				m_buffer[13],m_buffer[14],m_buffer[15],m_buffer[16],
				(const char*)&m_buffer+17
				);
		}
		return true;
	}
	else if (m_ReceivedPacketType==PACKET_RADIO)
		ParseRadioDatagram();
	else
	{
		char szTmp[100];
		sprintf(szTmp,"Unhandled Packet Type (0x%02x)",m_ReceivedPacketType);
		_log.Log(LOG_STATUS,szTmp);
	}
/*
	enocean_data_structure *pFrame=(enocean_data_structure*)&m_buffer;
	unsigned char Checksum=enocean_calc_checksum(pFrame);
	if (Checksum!=pFrame->CHECKSUM)
		return false; //checksum Mismatch!

	long id = (pFrame->ID_BYTE3 << 24) + (pFrame->ID_BYTE2 << 16) + (pFrame->ID_BYTE1 << 8) + pFrame->ID_BYTE0;
	char szDeviceID[20];
	sprintf(szDeviceID,"%08X",(unsigned int)id);

	//Handle possible OK/Errors
	bool bStopProcessing=false;
	if (pFrame->H_SEQ_LENGTH==0x8B)
	{
		switch (pFrame->ORG)
		{
		case 0x58:
			//OK
#ifdef _DEBUG
			_log.Log(LOG_NORM,"EnOcean: OK");
#endif
			bStopProcessing=true;
			break;
		case 0x28:
			_log.Log(LOG_ERROR,"EnOcean: ERR_MODEM_NOTWANTEDACK");
			bStopProcessing=true;
			break;
		case 0x29:
			_log.Log(LOG_ERROR,"EnOcean: ERR_MODEM_NOTACK");
			bStopProcessing=true;
			break;
		case 0x0C:
			_log.Log(LOG_ERROR,"EnOcean: ERR_MODEM_DUP_ID");
			bStopProcessing=true;
			break;
		case 0x08:
			_log.Log(LOG_ERROR,"EnOcean: Error in H_SEQ");
			bStopProcessing=true;
			break;
		case 0x09:
			_log.Log(LOG_ERROR,"EnOcean: Error in LENGTH");
			bStopProcessing=true;
			break;
		case 0x0A:
			_log.Log(LOG_ERROR,"EnOcean: Error in CHECKSUM");
			bStopProcessing=true;
			break;
		case 0x0B:
			_log.Log(LOG_ERROR,"EnOcean: Error in ORG");
			bStopProcessing=true;
			break;
		case 0x22:
			_log.Log(LOG_ERROR,"EnOcean: ERR_TX_IDRANGE");
			bStopProcessing=true;
			break;
		case 0x1A:
			_log.Log(LOG_ERROR,"EnOcean: ERR_ IDRANGE");
			bStopProcessing=true;
			break;
		}
	}
	if (bStopProcessing)
		return true;

	switch (pFrame->ORG)
	{
	case C_ORG_INF_IDBASE:
		m_id_base = (pFrame->DATA_BYTE3 << 24) + (pFrame->DATA_BYTE2 << 16) + (pFrame->DATA_BYTE1 << 8) + pFrame->DATA_BYTE0;
		_log.Log(LOG_STATUS,"EnOcean: Transceiver ID_Base: 0x%08x",m_id_base);
		break;
	case C_ORG_RPS:
		if (pFrame->STATUS & S_RPS_NU) {
			//Rocker
			// NU == 1, N-Message
			unsigned char RockerID=(pFrame->DATA_BYTE3 & DB3_RPS_NU_RID) >> DB3_RPS_NU_RID_SHIFT;
			unsigned char UpDown=(pFrame->DATA_BYTE3 & DB3_RPS_NU_UD) >> DB3_RPS_NU_UD_SHIFT;
			unsigned char Pressed=(pFrame->DATA_BYTE3 & DB3_RPS_NU_PR)>>DB3_RPS_NU_PR_SHIFT;
			unsigned char SecondRockerID=(pFrame->DATA_BYTE3 & DB3_RPS_NU_SRID)>>DB3_RPS_NU_SRID_SHIFT;
			unsigned char SecondUpDown=(pFrame->DATA_BYTE3 & DB3_RPS_NU_SUD)>>DB3_RPS_NU_SUD_SHIFT;
			unsigned char SecondAction=(pFrame->DATA_BYTE3 & DB3_RPS_NU_SA)>>DB3_RPS_NU_SA_SHIFT;
#ifdef _DEBUG
			_log.Log(LOG_NORM,"Received RPS N-Message Node 0x%08x Rocker ID: %i UD: %i Pressed: %i Second Rocker ID: %i SUD: %i Second Action: %i",
				id,
				RockerID, 
				UpDown, 
				Pressed,
				SecondRockerID, 
				SecondUpDown,
				SecondAction);
#endif
			//We distinguish 3 types of buttons from a switch: Left/Right/Left+Right
			if (Pressed==1)
			{
				RBUF tsen;
				memset(&tsen,0,sizeof(RBUF));
				tsen.LIGHTING2.packetlength=sizeof(tsen.LIGHTING2)-1;
				tsen.LIGHTING2.packettype=pTypeLighting2;
				tsen.LIGHTING2.subtype=sTypeAC;
				tsen.LIGHTING2.seqnbr=0;
				tsen.LIGHTING2.id1=(BYTE)pFrame->ID_BYTE3;
				tsen.LIGHTING2.id2=(BYTE)pFrame->ID_BYTE2;
				tsen.LIGHTING2.id3=(BYTE)pFrame->ID_BYTE1;
				tsen.LIGHTING2.id4=(BYTE)pFrame->ID_BYTE0;
				tsen.LIGHTING2.level=0;
				tsen.LIGHTING2.rssi=12;

				if (SecondAction==0)
				{
					//Left/Right Up/Down
					tsen.LIGHTING2.unitcode=RockerID+1;
					tsen.LIGHTING2.cmnd=(UpDown==1)?light2_sOn:light2_sOff;
				}
				else
				{
					//Left+Right Up/Down
					tsen.LIGHTING2.unitcode=SecondRockerID+10;
					tsen.LIGHTING2.cmnd=(SecondUpDown==1)?light2_sOn:light2_sOff;
				}
				sDecodeRXMessage(this, (const unsigned char *)&tsen.LIGHTING2);
			}
		}
		break;
	case C_ORG_4BS:
		break;
	default:
		{
			char *pszHumenTxt=enocean_hexToHuman(pFrame);
			if (pszHumenTxt)
			{
				_log.Log(LOG_NORM, "EnOcean: %s", pszHumenTxt);
				free(pszHumenTxt);
			}
		}
		break;
	}
*/
    return true;
}

void CEnOceanESP3::ParseRadioDatagram()
{
	char szTmp[100];
	if (m_OptionalDataSize == 7) 
	{
		sprintf(szTmp,"destination: 0x%02x%02x%02x%02x\nRSSI: %i",
			m_buffer[m_DataSize+1],m_buffer[m_DataSize+2],m_buffer[m_DataSize+3],m_buffer[m_DataSize+4],
			(100-m_buffer[m_DataSize+5])
			);
	}
	else {
		sprintf(szTmp, "Optional data size: %i",m_OptionalDataSize);
	}
	_log.Log(LOG_NORM, "EnOcean: %s", szTmp);
	switch (m_buffer[0])
	{
		case RORG_1BS: // 1 byte communication (Contacts/Switches)
			{
				sprintf(szTmp,"1BS data: Sender id: 0x%02x%02x%02x%02x Data: %02x",
					m_buffer[2],m_buffer[3],m_buffer[4],m_buffer[5],
					m_buffer[0]
				);

				_log.Log(LOG_NORM, "EnOcean: %s", szTmp);

				unsigned char DATA_BYTE0 = m_buffer[1];

				unsigned char ID_BYTE3  = m_buffer[2];
				unsigned char ID_BYTE2  = m_buffer[3];
				unsigned char ID_BYTE1  = m_buffer[4];
				unsigned char ID_BYTE0  = m_buffer[5];

				int UpDown=(DATA_BYTE0&1)==0;

				RBUF tsen;
				memset(&tsen,0,sizeof(RBUF));
				tsen.LIGHTING2.packetlength=sizeof(tsen.LIGHTING2)-1;
				tsen.LIGHTING2.packettype=pTypeLighting2;
				tsen.LIGHTING2.subtype=sTypeAC;
				tsen.LIGHTING2.seqnbr=0;

				tsen.LIGHTING2.id1=(BYTE)ID_BYTE3;
				tsen.LIGHTING2.id2=(BYTE)ID_BYTE2;
				tsen.LIGHTING2.id3=(BYTE)ID_BYTE1;
				tsen.LIGHTING2.id4=(BYTE)ID_BYTE0;
				tsen.LIGHTING2.level=0;
				tsen.LIGHTING2.rssi=12;
				tsen.LIGHTING2.unitcode=1;
				tsen.LIGHTING2.cmnd=(UpDown==1)?light2_sOn:light2_sOff;
				sDecodeRXMessage(this, (const unsigned char *)&tsen.LIGHTING2);
			}
			break;
		case RORG_4BS: // 4 byte communication
			{
				sprintf(szTmp,"4BS data: Sender id: 0x%02x%02x%02x%02x Status: %02x Data: %02x",
					m_buffer[5],m_buffer[6],m_buffer[7],m_buffer[8],
					m_buffer[9],
					m_buffer[3]
				);
				_log.Log(LOG_NORM, "EnOcean: %s", szTmp);

				unsigned char DATA_BYTE3 = m_buffer[1];
				unsigned char DATA_BYTE2 = m_buffer[2];
				unsigned char DATA_BYTE1 = m_buffer[3];
				unsigned char DATA_BYTE0 = m_buffer[4];

				unsigned char ID_BYTE3  = m_buffer[5];
				unsigned char ID_BYTE2  = m_buffer[6];
				unsigned char ID_BYTE1  = m_buffer[7];
				unsigned char ID_BYTE0  = m_buffer[8];

				long id = (ID_BYTE3 << 24) + (ID_BYTE2 << 16) + (ID_BYTE1 << 8) + ID_BYTE0;
				char szDeviceID[20];
				sprintf(szDeviceID,"%08X",(unsigned int)id);

				if ((DATA_BYTE0 & 0x08) == 0)
				{
					if (DATA_BYTE0 & 0x80)
					{
						//Teach in datagram

						//DB3		DB3/2	DB2/1			DB0
						//Profile	Type	Manufacturer-ID	LRN Type	RE2		RE1
						//6 Bit		7 Bit	11 Bit			1Bit		1Bit	1Bit	1Bit	1Bit	1Bit	1Bit	1Bit

						int manufacturer = ((DATA_BYTE2 & 7) << 8) | DATA_BYTE1;
						int profile = DATA_BYTE3 >> 2;
						int ttype = ((DATA_BYTE3 & 3) << 5) | (DATA_BYTE2 >> 3);
						_log.Log(LOG_NORM,"EnOcean: 4BS, Teach-in diagram: Sender_ID: 0x%08X\nManufacturer: 0x%02x (%s)\nProfile: 0x%02X\nType: 0x%02X (%s)", 
							id, manufacturer,Get_EnoceanManufacturer(manufacturer),
							profile,ttype,Get_Enocean4BSType(0xA5,profile,ttype));


						std::stringstream szQuery;
						std::vector<std::vector<std::string> > result;
						szQuery << "SELECT ID FROM EnoceanSensors WHERE (HardwareID==" << m_HwdID << ") AND (DeviceID=='" << szDeviceID << "')";
						result=m_sql.query(szQuery.str());
						if (result.size()<1)
						{
							//Add it to the database
							szQuery.clear();
							szQuery.str("");
							szQuery << "INSERT INTO EnoceanSensors (HardwareID, DeviceID, Manufacturer, Profile, [Type]) VALUES (" << m_HwdID << ",'" << szDeviceID << "'," << manufacturer << "," << profile << "," << ttype << ")";
							m_sql.query(szQuery.str());
						}

					}
				}
				else
				{
					//Following sensors need to have had a teach-in
					std::stringstream szQuery;
					std::vector<std::vector<std::string> > result;
					szQuery << "SELECT ID, Manufacturer, Profile, [Type] FROM EnoceanSensors WHERE (HardwareID==" << m_HwdID << ") AND (DeviceID=='" << szDeviceID << "')";
					result=m_sql.query(szQuery.str());
					if (result.size()<1)
					{
						_log.Log(LOG_NORM, "EnOcean: Need Teach-In for %s", szDeviceID);
						return;
					}
					int Manufacturer=atoi(result[0][1].c_str());
					int Profile=atoi(result[0][2].c_str());
					int iType=atoi(result[0][3].c_str());

					const std::string szST=Get_Enocean4BSType(0xA5,Profile,iType);

					if (szST=="AMR.Counter")
					{
						//0xA5, 0x12, 0x00, "Counter"
						unsigned long cvalue=(DATA_BYTE3<<16)|(DATA_BYTE2<<8)|(DATA_BYTE1);
						RBUF tsen;
						memset(&tsen,0,sizeof(RBUF));
						tsen.RFXMETER.packetlength=sizeof(tsen.RFXMETER)-1;
						tsen.RFXMETER.packettype=pTypeRFXMeter;
						tsen.RFXMETER.subtype=sTypeRFXMeterCount;
						tsen.RFXMETER.rssi=12;
						tsen.RFXMETER.id1=ID_BYTE2;
						tsen.RFXMETER.id2=ID_BYTE1;
						tsen.RFXMETER.count1 = (BYTE)((cvalue & 0xFF000000) >> 24);
						tsen.RFXMETER.count2 = (BYTE)((cvalue & 0x00FF0000) >> 16);
						tsen.RFXMETER.count3 = (BYTE)((cvalue & 0x0000FF00) >> 8);
						tsen.RFXMETER.count4 = (BYTE)(cvalue & 0x000000FF);
						sDecodeRXMessage(this, (const unsigned char *)&tsen.RFXMETER);
					}
					else if (szST=="AMR.Electricity")
					{
						//0xA5, 0x12, 0x01, "Electricity"
						int cvalue=(DATA_BYTE3<<16)|(DATA_BYTE2<<8)|(DATA_BYTE1);
						_tUsageMeter umeter;
						umeter.id1=(BYTE)ID_BYTE3;
						umeter.id2=(BYTE)ID_BYTE2;
						umeter.id3=(BYTE)ID_BYTE1;
						umeter.id4=(BYTE)ID_BYTE0;
						umeter.dunit=1;
						umeter.fusage=(float)cvalue;
						sDecodeRXMessage(this, (const unsigned char *)&umeter);
					}
					else if (szST=="AMR.Gas")
					{
						//0xA5, 0x12, 0x02, "Gas"
						unsigned long cvalue=(DATA_BYTE3<<16)|(DATA_BYTE2<<8)|(DATA_BYTE1);
						RBUF tsen;
						memset(&tsen,0,sizeof(RBUF));
						tsen.RFXMETER.packetlength=sizeof(tsen.RFXMETER)-1;
						tsen.RFXMETER.packettype=pTypeRFXMeter;
						tsen.RFXMETER.subtype=sTypeRFXMeterCount;
						tsen.RFXMETER.rssi=12;
						tsen.RFXMETER.id1=ID_BYTE2;
						tsen.RFXMETER.id2=ID_BYTE1;
						tsen.RFXMETER.count1 = (BYTE)((cvalue & 0xFF000000) >> 24);
						tsen.RFXMETER.count2 = (BYTE)((cvalue & 0x00FF0000) >> 16);
						tsen.RFXMETER.count3 = (BYTE)((cvalue & 0x0000FF00) >> 8);
						tsen.RFXMETER.count4 = (BYTE)(cvalue & 0x000000FF);
						sDecodeRXMessage(this, (const unsigned char *)&tsen.RFXMETER);
					}
					else if (szST=="AMR.Water")
					{
						//0xA5, 0x12, 0x03, "Water"
						unsigned long cvalue=(DATA_BYTE3<<16)|(DATA_BYTE2<<8)|(DATA_BYTE1);
						RBUF tsen;
						memset(&tsen,0,sizeof(RBUF));
						tsen.RFXMETER.packetlength=sizeof(tsen.RFXMETER)-1;
						tsen.RFXMETER.packettype=pTypeRFXMeter;
						tsen.RFXMETER.subtype=sTypeRFXMeterCount;
						tsen.RFXMETER.rssi=12;
						tsen.RFXMETER.id1=ID_BYTE2;
						tsen.RFXMETER.id2=ID_BYTE1;
						tsen.RFXMETER.count1 = (BYTE)((cvalue & 0xFF000000) >> 24);
						tsen.RFXMETER.count2 = (BYTE)((cvalue & 0x00FF0000) >> 16);
						tsen.RFXMETER.count3 = (BYTE)((cvalue & 0x0000FF00) >> 8);
						tsen.RFXMETER.count4 = (BYTE)(cvalue & 0x000000FF);
						sDecodeRXMessage(this, (const unsigned char *)&tsen.RFXMETER);
					}
					else if (szST.find("RoomOperatingPanel") == 0)
					{
						if (iType<0x0E)
						{
							// Room Sensor and Control Unit (EEP A5-10-01 ... A5-10-0D)
							// [Eltako FTR55D, FTR55H, Thermokon SR04 *, Thanos SR *, untested]
							// DATA_BYTE3 is the fan speed or night reduction for Eltako
							// DATA_BYTE2 is the setpoint where 0x00 = min ... 0xFF = max or
							// reference temperature for Eltako where 0x00 = 0�C ... 0xFF = 40�C
							// DATA_BYTE1 is the temperature where 0x00 = +40�C ... 0xFF = 0�C
							// DATA_BYTE0_bit_0 is the occupy button, pushbutton or slide switch
							float temp=GetValueRange(DATA_BYTE1,0,40);
							if (Manufacturer == 0x0D) 
							{
								//Eltako
								int nightReduction = 0;
								if (DATA_BYTE3 == 0x06)
									nightReduction = 1;
								else if (DATA_BYTE3 == 0x0C)
									nightReduction = 2;
								else if (DATA_BYTE3 == 0x13)
									nightReduction = 3;
								else if (DATA_BYTE3 == 0x19)
									nightReduction = 4;
								else if (DATA_BYTE3 == 0x1F)
									nightReduction = 5;
								float setpointTemp=GetValueRange(DATA_BYTE2,40);
							}
							else 
							{
								int fspeed = 3;
								if (DATA_BYTE3 >= 145)
									fspeed = 2;
								else if (DATA_BYTE3 >= 165)
									fspeed = 1;
								else if (DATA_BYTE3 >= 190)
									fspeed = 0;
								else if (DATA_BYTE3 >= 210)
									fspeed = -1; //auto
								int iswitch = DATA_BYTE0 & 1;
							}
							RBUF tsen;
							memset(&tsen,0,sizeof(RBUF));
							tsen.TEMP.packetlength=sizeof(tsen.TEMP)-1;
							tsen.TEMP.packettype=pTypeTEMP;
							tsen.TEMP.subtype=sTypeTEMP10;
							tsen.TEMP.id1=ID_BYTE2;
							tsen.TEMP.id2=ID_BYTE1;
							tsen.TEMP.battery_level=ID_BYTE0&0x0F;
							tsen.TEMP.rssi=(ID_BYTE0&0xF0)>>4;

							tsen.TEMP.tempsign=(temp>=0)?0:1;
							int at10=round(abs(temp*10.0f));
							tsen.TEMP.temperatureh=(BYTE)(at10/256);
							at10-=(tsen.TEMP.temperatureh*256);
							tsen.TEMP.temperaturel=(BYTE)(at10);
							sDecodeRXMessage(this, (const unsigned char *)&tsen.TEMP);
						}
					}
					else if (szST == "LightSensor.01")
					{
						// Light Sensor (EEP A5-06-01)
						// [Eltako FAH60, FAH63, FIH63, Thermokon SR65 LI, untested]
						// DATA_BYTE3 is the voltage where 0x00 = 0 V ... 0xFF = 5.1 V
						// DATA_BYTE3 is the low illuminance for Eltako devices where
						// min 0x00 = 0 lx, max 0xFF = 100 lx, if DATA_BYTE2 = 0
						// DATA_BYTE2 is the illuminance (ILL2) where min 0x00 = 300 lx, max 0xFF = 30000 lx
						// DATA_BYTE1 is the illuminance (ILL1) where min 0x00 = 600 lx, max 0xFF = 60000 lx
						// DATA_BYTE0_bit_0 is Range select where 0 = ILL1, 1 = ILL2
						float lux =0;
						if (Manufacturer == 0x0D)
						{
							if(DATA_BYTE2 == 0) {
								lux=GetValueRange(DATA_BYTE3,100);
							} else {
								lux=GetValueRange(DATA_BYTE2,30000,300);
							}
						} else {
							float voltage=GetValueRange(DATA_BYTE3,5100); //mV
							if(DATA_BYTE0 & 1) {
								lux=GetValueRange(DATA_BYTE2,30000,300);
							} else {
								lux=GetValueRange(DATA_BYTE1,60000,600);
							}
							RBUF tsen;
							memset(&tsen,0,sizeof(RBUF));
							tsen.RFXSENSOR.packetlength=sizeof(tsen.RFXSENSOR)-1;
							tsen.RFXSENSOR.packettype=pTypeRFXSensor;
							tsen.RFXSENSOR.subtype=sTypeRFXSensorVolt;
							tsen.RFXSENSOR.id=ID_BYTE1;
							tsen.RFXSENSOR.filler=ID_BYTE0&0x0F;
							tsen.RFXSENSOR.rssi=(ID_BYTE0&0xF0)>>4;
							tsen.RFXSENSOR.msg1 = (BYTE)(voltage/256);
							tsen.RFXSENSOR.msg2 = (BYTE)(voltage-(tsen.RFXSENSOR.msg1*256));
							sDecodeRXMessage(this, (const unsigned char *)&tsen.RFXSENSOR);
						}
						_tLightMeter lmeter;
						lmeter.id1=(BYTE)ID_BYTE3;
						lmeter.id2=(BYTE)ID_BYTE2;
						lmeter.id3=(BYTE)ID_BYTE1;
						lmeter.id4=(BYTE)ID_BYTE0;
						lmeter.dunit=1;
						lmeter.fLux=lux;
						sDecodeRXMessage(this, (const unsigned char *)&lmeter);
					}
					else if (szST.find("Temperature")==0)
					{
						//(EPP A5-02 01/30)
						float ScaleMax=0;
						float ScaleMin=0;
						if (iType==0x01) { ScaleMax=-40; ScaleMin=0; }
						else if (iType==0x02) { ScaleMax=-30; ScaleMin=10; }
						else if (iType==0x03) { ScaleMax=-20; ScaleMin=20; }
						else if (iType==0x04) { ScaleMax=-10; ScaleMin=30; }
						else if (iType==0x05) { ScaleMax=0; ScaleMin=40; }
						else if (iType==0x06) { ScaleMax=10; ScaleMin=50; }
						else if (iType==0x07) { ScaleMax=20; ScaleMin=60; }
						else if (iType==0x08) { ScaleMax=30; ScaleMin=70; }
						else if (iType==0x09) { ScaleMax=40; ScaleMin=80; }
						else if (iType==0x0A) { ScaleMax=50; ScaleMin=90; }
						else if (iType==0x0B) { ScaleMax=60; ScaleMin=100; }
						else if (iType==0x10) { ScaleMax=-60; ScaleMin=20; }
						else if (iType==0x11) { ScaleMax=-50; ScaleMin=30; }
						else if (iType==0x12) { ScaleMax=-40; ScaleMin=40; }
						else if (iType==0x13) { ScaleMax=-30; ScaleMin=50; }
						else if (iType==0x14) { ScaleMax=-20; ScaleMin=60; }
						else if (iType==0x15) { ScaleMax=-10; ScaleMin=70; }
						else if (iType==0x16) { ScaleMax=0; ScaleMin=80; }
						else if (iType==0x17) { ScaleMax=10; ScaleMin=90; }
						else if (iType==0x18) { ScaleMax=20; ScaleMin=100; }
						else if (iType==0x19) { ScaleMax=30; ScaleMin=110; }
						else if (iType==0x1A) { ScaleMax=40; ScaleMin=120; }
						else if (iType==0x1B) { ScaleMax=50; ScaleMin=130; }
						else if (iType==0x20) { ScaleMax=-10; ScaleMin=41.2f; }
						else if (iType==0x30) { ScaleMax=-40; ScaleMin=62.3f; }

						float temp;
						if (iType<0x20)
							temp=GetValueRange(DATA_BYTE1,ScaleMax,ScaleMin);
						else
							temp=GetValueRange(float(((DATA_BYTE2&3)<<8)|DATA_BYTE1),ScaleMax,ScaleMin); //10bit
						RBUF tsen;
						memset(&tsen,0,sizeof(RBUF));
						tsen.TEMP.packetlength=sizeof(tsen.TEMP)-1;
						tsen.TEMP.packettype=pTypeTEMP;
						tsen.TEMP.subtype=sTypeTEMP10;
						tsen.TEMP.id1=ID_BYTE2;
						tsen.TEMP.id2=ID_BYTE1;
						tsen.TEMP.battery_level=ID_BYTE0&0x0F;
						tsen.TEMP.rssi=(ID_BYTE0&0xF0)>>4;

						tsen.TEMP.tempsign=(temp>=0)?0:1;
						int at10=round(abs(temp*10.0f));
						tsen.TEMP.temperatureh=(BYTE)(at10/256);
						at10-=(tsen.TEMP.temperatureh*256);
						tsen.TEMP.temperaturel=(BYTE)(at10);
						sDecodeRXMessage(this, (const unsigned char *)&tsen.TEMP);
					}
					else if (szST == "TempHum")
					{
						//(EPP A5-04 01/02)
						float ScaleMax = 0;
						float ScaleMin = 0;
						if (iType == 0x01) { ScaleMax = 0; ScaleMin = 40; }
						else if (iType == 0x02) { ScaleMax = -20; ScaleMin = 60; }
						else if (iType == 0x03) { ScaleMax = -20; ScaleMin = 60; } //10bit?

						float temp = GetValueRange(DATA_BYTE1, ScaleMax, ScaleMin);
						float hum = GetValueRange(DATA_BYTE2, 100);
						RBUF tsen;
						memset(&tsen,0,sizeof(RBUF));
						tsen.TEMP_HUM.packetlength=sizeof(tsen.TEMP_HUM)-1;
						tsen.TEMP_HUM.packettype=pTypeTEMP_HUM;
						tsen.TEMP_HUM.subtype=sTypeTH5;
						tsen.TEMP_HUM.rssi=12;
						tsen.TEMP_HUM.id1=ID_BYTE2;
						tsen.TEMP_HUM.id2=ID_BYTE1;
						tsen.TEMP_HUM.battery_level=9;
						tsen.TEMP_HUM.tempsign=(temp>=0)?0:1;
						int at10=round(abs(temp*10.0f));
						tsen.TEMP_HUM.temperatureh=(BYTE)(at10/256);
						at10-=(tsen.TEMP_HUM.temperatureh*256);
						tsen.TEMP_HUM.temperaturel=(BYTE)(at10);
						tsen.TEMP_HUM.humidity=(BYTE)hum;
						tsen.TEMP_HUM.humidity_status=Get_Humidity_Level(tsen.TEMP_HUM.humidity);
						sDecodeRXMessage(this, (const unsigned char *)&tsen.TEMP_HUM);
					}
					else if (szST == "OccupancySensor.01")
					{
						//(EPP A5-07-01)
						if (DATA_BYTE3 < 251)
						{
							RBUF tsen;

							if (DATA_BYTE0 & 1)
							{
								//Voltage supported
								float voltage = GetValueRange(DATA_BYTE3, 5.0f, 0, 250, 0);
								memset(&tsen, 0, sizeof(RBUF));
								tsen.RFXSENSOR.packetlength = sizeof(tsen.RFXSENSOR) - 1;
								tsen.RFXSENSOR.packettype = pTypeRFXSensor;
								tsen.RFXSENSOR.subtype = sTypeRFXSensorVolt;
								tsen.RFXSENSOR.id = ID_BYTE1;
								tsen.RFXSENSOR.filler = ID_BYTE0 & 0x0F;
								tsen.RFXSENSOR.rssi = (ID_BYTE0 & 0xF0) >> 4;
								tsen.RFXSENSOR.msg1 = (BYTE)(voltage / 256);
								tsen.RFXSENSOR.msg2 = (BYTE)(voltage - (tsen.RFXSENSOR.msg1 * 256));
								sDecodeRXMessage(this, (const unsigned char *)&tsen.RFXSENSOR);
							}

							bool bPIROn = (DATA_BYTE1 > 127);
							memset(&tsen, 0, sizeof(RBUF));
							tsen.LIGHTING2.packetlength = sizeof(tsen.LIGHTING2) - 1;
							tsen.LIGHTING2.packettype = pTypeLighting2;
							tsen.LIGHTING2.subtype = sTypeAC;
							tsen.LIGHTING2.seqnbr = 0;

							tsen.LIGHTING2.id1 = (BYTE)ID_BYTE3;
							tsen.LIGHTING2.id2 = (BYTE)ID_BYTE2;
							tsen.LIGHTING2.id3 = (BYTE)ID_BYTE1;
							tsen.LIGHTING2.id4 = (BYTE)ID_BYTE0;
							tsen.LIGHTING2.level = 0;
							tsen.LIGHTING2.rssi = 12;
							tsen.LIGHTING2.unitcode = 1;
							tsen.LIGHTING2.cmnd = (bPIROn) ? light2_sOn : light2_sOff;
							sDecodeRXMessage(this, (const unsigned char *)&tsen.LIGHTING2);
						}
						else {
							//Error code
						}
					}
					else if (szST == "OccupancySensor.02")
					{
						//(EPP A5-07-02)
						if (DATA_BYTE3 < 251)
						{
							RBUF tsen;

							float voltage = GetValueRange(DATA_BYTE3, 5.0f, 0, 250, 0);
							memset(&tsen, 0, sizeof(RBUF));
							tsen.RFXSENSOR.packetlength = sizeof(tsen.RFXSENSOR) - 1;
							tsen.RFXSENSOR.packettype = pTypeRFXSensor;
							tsen.RFXSENSOR.subtype = sTypeRFXSensorVolt;
							tsen.RFXSENSOR.id = ID_BYTE1;
							tsen.RFXSENSOR.filler = ID_BYTE0 & 0x0F;
							tsen.RFXSENSOR.rssi = (ID_BYTE0 & 0xF0) >> 4;
							tsen.RFXSENSOR.msg1 = (BYTE)(voltage / 256);
							tsen.RFXSENSOR.msg2 = (BYTE)(voltage - (tsen.RFXSENSOR.msg1 * 256));
							sDecodeRXMessage(this, (const unsigned char *)&tsen.RFXSENSOR);

							bool bPIROn = (DATA_BYTE0 & 0x80)!=0;
							memset(&tsen, 0, sizeof(RBUF));
							tsen.LIGHTING2.packetlength = sizeof(tsen.LIGHTING2) - 1;
							tsen.LIGHTING2.packettype = pTypeLighting2;
							tsen.LIGHTING2.subtype = sTypeAC;
							tsen.LIGHTING2.seqnbr = 0;

							tsen.LIGHTING2.id1 = (BYTE)ID_BYTE3;
							tsen.LIGHTING2.id2 = (BYTE)ID_BYTE2;
							tsen.LIGHTING2.id3 = (BYTE)ID_BYTE1;
							tsen.LIGHTING2.id4 = (BYTE)ID_BYTE0;
							tsen.LIGHTING2.level = 0;
							tsen.LIGHTING2.rssi = 12;
							tsen.LIGHTING2.unitcode = 1;
							tsen.LIGHTING2.cmnd = (bPIROn) ? light2_sOn : light2_sOff;
							sDecodeRXMessage(this, (const unsigned char *)&tsen.LIGHTING2);
						}
						else {
							//Error code
						}
					}
					else if (szST == "OccupancySensor.03")
					{
						//(EPP A5-07-03)
						if (DATA_BYTE3 < 251)
						{
							RBUF tsen;

							float voltage = GetValueRange(DATA_BYTE3, 5.0f, 0, 250, 0);
							memset(&tsen, 0, sizeof(RBUF));
							tsen.RFXSENSOR.packetlength = sizeof(tsen.RFXSENSOR) - 1;
							tsen.RFXSENSOR.packettype = pTypeRFXSensor;
							tsen.RFXSENSOR.subtype = sTypeRFXSensorVolt;
							tsen.RFXSENSOR.id = ID_BYTE1;
							tsen.RFXSENSOR.filler = ID_BYTE0 & 0x0F;
							tsen.RFXSENSOR.rssi = (ID_BYTE0 & 0xF0) >> 4;
							tsen.RFXSENSOR.msg1 = (BYTE)(voltage / 256);
							tsen.RFXSENSOR.msg2 = (BYTE)(voltage - (tsen.RFXSENSOR.msg1 * 256));
							sDecodeRXMessage(this, (const unsigned char *)&tsen.RFXSENSOR);

							int lux = (DATA_BYTE2 << 2) | (DATA_BYTE1>>6);
							if (lux > 1000)
								lux = 1000;
							_tLightMeter lmeter;
							lmeter.id1 = (BYTE)ID_BYTE3;
							lmeter.id2 = (BYTE)ID_BYTE2;
							lmeter.id3 = (BYTE)ID_BYTE1;
							lmeter.id4 = (BYTE)ID_BYTE0;
							lmeter.dunit = 1;
							lmeter.fLux = (float)lux;
							sDecodeRXMessage(this, (const unsigned char *)&lmeter);

							bool bPIROn = (DATA_BYTE0 & 0x80)!=0;
							memset(&tsen, 0, sizeof(RBUF));
							tsen.LIGHTING2.packetlength = sizeof(tsen.LIGHTING2) - 1;
							tsen.LIGHTING2.packettype = pTypeLighting2;
							tsen.LIGHTING2.subtype = sTypeAC;
							tsen.LIGHTING2.seqnbr = 0;

							tsen.LIGHTING2.id1 = (BYTE)ID_BYTE3;
							tsen.LIGHTING2.id2 = (BYTE)ID_BYTE2;
							tsen.LIGHTING2.id3 = (BYTE)ID_BYTE1;
							tsen.LIGHTING2.id4 = (BYTE)ID_BYTE0;
							tsen.LIGHTING2.level = 0;
							tsen.LIGHTING2.rssi = 12;
							tsen.LIGHTING2.unitcode = 1;
							tsen.LIGHTING2.cmnd = (bPIROn) ? light2_sOn : light2_sOff;
							sDecodeRXMessage(this, (const unsigned char *)&tsen.LIGHTING2);
						}
						else {
							//Error code
						}
					}
				}
			}
			break;
		case RORG_RPS: // repeated switch communication
			{
/*
				sprintf(szTmp, "RPS data: Sender id: 0x%02x%02x%02x%02x Status: %02x Data: %02x",
					m_buffer[2],m_buffer[3],m_buffer[4],m_buffer[5],
					m_buffer[6],
					m_buffer[1]
				);
				_log.Log(LOG_NORM, "EnOcean: %s", szTmp);
				if (m_buffer[6] & (1 << 2))
				{
					_log.Log(LOG_NORM, "EnOcean: T21");
				}
*/
				unsigned char STATUS=m_buffer[6];

				unsigned char T21 = (m_buffer[6] & S_RPS_T21) >> S_RPS_T21_SHIFT;
				unsigned char NU = (m_buffer[6] & S_RPS_NU) >> S_RPS_NU_SHIFT;

				unsigned char ID_BYTE3=m_buffer[2];
				unsigned char ID_BYTE2=m_buffer[3];
				unsigned char ID_BYTE1=m_buffer[4];
				unsigned char ID_BYTE0=m_buffer[5];
				long id = (ID_BYTE3 << 24) + (ID_BYTE2 << 16) + (ID_BYTE1 << 8) + ID_BYTE0;
				char szDeviceID[20];
				sprintf(szDeviceID,"%08X",(unsigned int)id);

				if (STATUS & S_RPS_NU)
				{
					//Rocker

					unsigned char DATA_BYTE3=m_buffer[1];

					// NU == 1, N-Message
					unsigned char RockerID=(DATA_BYTE3 & DB3_RPS_NU_RID) >> DB3_RPS_NU_RID_SHIFT;
					unsigned char UpDown=(DATA_BYTE3 & DB3_RPS_NU_UD) >> DB3_RPS_NU_UD_SHIFT;
					unsigned char Pressed=(DATA_BYTE3 & DB3_RPS_NU_PR)>>DB3_RPS_NU_PR_SHIFT;
					unsigned char SecondRockerID=(DATA_BYTE3 & DB3_RPS_NU_SRID)>>DB3_RPS_NU_SRID_SHIFT;
					unsigned char SecondUpDown=(DATA_BYTE3 & DB3_RPS_NU_SUD)>>DB3_RPS_NU_SUD_SHIFT;
					unsigned char SecondAction=(DATA_BYTE3 & DB3_RPS_NU_SA)>>DB3_RPS_NU_SA_SHIFT;
#ifdef _DEBUG
					_log.Log(LOG_NORM,"Received RPS N-Message Node 0x%08x Rocker ID: %i UD: %i Pressed: %i Second Rocker ID: %i SUD: %i Second Action: %i",
						id,
						RockerID, 
						UpDown, 
						Pressed,
						SecondRockerID, 
						SecondUpDown,
						SecondAction);
#endif
					//We distinguish 3 types of buttons from a switch: Left/Right/Left+Right
					if (Pressed==1)
					{
						RBUF tsen;
						memset(&tsen,0,sizeof(RBUF));
						tsen.LIGHTING2.packetlength=sizeof(tsen.LIGHTING2)-1;
						tsen.LIGHTING2.packettype=pTypeLighting2;
						tsen.LIGHTING2.subtype=sTypeAC;
						tsen.LIGHTING2.seqnbr=0;

						tsen.LIGHTING2.id1=(BYTE)ID_BYTE3;
						tsen.LIGHTING2.id2=(BYTE)ID_BYTE2;
						tsen.LIGHTING2.id3=(BYTE)ID_BYTE1;
						tsen.LIGHTING2.id4=(BYTE)ID_BYTE0;
						tsen.LIGHTING2.level=0;
						tsen.LIGHTING2.rssi=12;

						if (SecondAction==0)
						{
							//Left/Right Up/Down
							tsen.LIGHTING2.unitcode=RockerID+1;
							tsen.LIGHTING2.cmnd=(UpDown==1)?light2_sOn:light2_sOff;
						}
						else
						{
							//Left+Right Up/Down
							tsen.LIGHTING2.unitcode=SecondRockerID+10;
							tsen.LIGHTING2.cmnd=(SecondUpDown==1)?light2_sOn:light2_sOff;
						}
						sDecodeRXMessage(this, (const unsigned char *)&tsen.LIGHTING2);
					}
				}
				else
				{
					if ((T21 == 1) && (NU == 0))
					{
						unsigned char DATA_BYTE3 = m_buffer[1];
						//unsigned char ButtonID = (DATA_BYTE3&DB3_RPS_BUTTONS) >> DB3_RPS_BUTTONS_SHIFT;
						//unsigned char Position = (DATA_BYTE3&DB3_RPS_PR) >> DB3_RPS_PR_SHIFT;

						unsigned char UpDown = !((DATA_BYTE3 == 0xD0) || (DATA_BYTE3 == 0xF0));

						//_log.Log(LOG_NORM, "Received RPS T21-Message Node 0x%08x Button ID: %02X UD: %i",id,ButtonID,UpDown);

						RBUF tsen;
						memset(&tsen, 0, sizeof(RBUF));
						tsen.LIGHTING2.packetlength = sizeof(tsen.LIGHTING2) - 1;
						tsen.LIGHTING2.packettype = pTypeLighting2;
						tsen.LIGHTING2.subtype = sTypeAC;
						tsen.LIGHTING2.seqnbr = 0;

						tsen.LIGHTING2.id1 = (BYTE)ID_BYTE3;
						tsen.LIGHTING2.id2 = (BYTE)ID_BYTE2;
						tsen.LIGHTING2.id3 = (BYTE)ID_BYTE1;
						tsen.LIGHTING2.id4 = (BYTE)ID_BYTE0;
						tsen.LIGHTING2.level = 0;
						tsen.LIGHTING2.rssi = 12;

						tsen.LIGHTING2.unitcode = 1;
						tsen.LIGHTING2.cmnd = (UpDown == 1) ? light2_sOn : light2_sOff;
						sDecodeRXMessage(this, (const unsigned char *)&tsen.LIGHTING2);

					}
				}
			}
			break;
		default:
			_log.Log(LOG_NORM, "EnOcean: Unhandled RORG (%02x)", m_buffer[0]);
			break;
	}
}
