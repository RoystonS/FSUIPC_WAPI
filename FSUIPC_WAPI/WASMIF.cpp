#include "WASMIF.h"
#include <SimConnect.h>
#include <sstream>
#include <iomanip>
#include <cmath>
#include "Logger.h"


using namespace CPlusPlusLogging;

WASMIF* WASMIF::m_Instance = 0;
int WASMIF::nextDefinitionID = 1; // 1 taken by config CDA
Logger* pLogger = nullptr;


WASMIF::WASMIF() {
	hSimConnect = 0;
	configTimer = 0;
	quit = 0;
	configReceived = FALSE;
	noLvarCDAs = 0;
	noHvarCDAs = 0;
	value_cda = nullptr;
	lvarUpdateFrequency = 0;
	InitializeCriticalSection(&lvarMutex);
	simConnection = SIMCONNECT_OPEN_CONFIGINDEX_LOCAL; // = -1
}

WASMIF::~WASMIF() {}

void WASMIF::setSimConfigConnection(int simConnection) {
	simConnection = simConnection;
}


WASMIF* WASMIF::GetInstance(HWND hWnd, int startEventNo, void (*loggerFunction)(const char* logString)) {
    if (m_Instance == 0)
    {
        m_Instance = new WASMIF();
		m_Instance->hWnd = hWnd;
		m_Instance->startEventNo = startEventNo;
		if (loggerFunction == nullptr) {
			pLogger = Logger::getInstance(".\\FSUIPC_WASMIF");
		}
		else {
			pLogger = Logger::getInstance(loggerFunction);
		}
    }
    return m_Instance;
}


WASMIF* WASMIF::GetInstance(HWND hWnd, void (*loggerFunction)(const char* logString)) {
	if (m_Instance == 0)
	{
		m_Instance = new WASMIF();
		m_Instance->hWnd = hWnd;
		m_Instance->startEventNo = EVENT_START_NO;
		if (loggerFunction == nullptr) {
			pLogger = Logger::getInstance(".\\FSUIPC_WASMIF");
		}
		else {
			pLogger = Logger::getInstance(loggerFunction);
		}
		m_Instance->lvarUpdateFrequency = 0;
		m_Instance->value_cda = nullptr;
	}
	return m_Instance;
}


void WASMIF::setLogLevel(LOGLEVEL logLevel) {
	pLogger->updateLogLevel((LogLevel)logLevel);
}


const char* WASMIF::getEventString(int eventNo) {
	std::stringstream stream;
	stream << "#0x" << std::hex << startEventNo + eventNo;
	std::string result(stream.str());

	return result.c_str();
}


DWORD WINAPI WASMIF::SimConnectStart() {
	HRESULT hr;
	int CDAId = 1;

//	hr = SimConnect_MapClientEventToSimEvent(hSimConnect, EVENT_GET_CONFIG, getEventString(EVENT_GET_CONFIG));
	hr = SimConnect_MapClientEventToSimEvent(hSimConnect, EVENT_SET_LVAR, getEventString(EVENT_SET_LVAR));
	hr = SimConnect_MapClientEventToSimEvent(hSimConnect, EVENT_SET_HVAR, getEventString(EVENT_SET_HVAR));
	hr = SimConnect_MapClientEventToSimEvent(hSimConnect, EVENT_UPDATE_CDAS, getEventString(EVENT_UPDATE_CDAS));
	hr = SimConnect_MapClientEventToSimEvent(hSimConnect, EVENT_LIST_LVARS, getEventString(EVENT_LIST_LVARS));
	hr = SimConnect_MapClientEventToSimEvent(hSimConnect, EVENT_RELOAD, getEventString(EVENT_RELOAD));
	hr = SimConnect_MapClientEventToSimEvent(hSimConnect, EVENT_SET_LVARNEG, getEventString(EVENT_SET_LVARNEG));

	hr = SimConnect_SetNotificationGroupPriority(hSimConnect, 1, SIMCONNECT_GROUP_PRIORITY_HIGHEST);

	// Now register Client Data Area
	if (!SUCCEEDED(SimConnect_MapClientDataNameToID(hSimConnect, CONFIG_CDA_NAME, CDAId++)))
	{
		LOG_ERROR("SimConnect_MapClientDataNameToID failed!!!!");
	}
	if (!SUCCEEDED(SimConnect_AddToClientDataDefinition(hSimConnect, nextDefinitionID++, SIMCONNECT_CLIENTDATAOFFSET_AUTO, sizeof(CONFIG_CDA), 0, 0)))
	{
		LOG_ERROR("SimConnect_AddToClientDataDefinition failed!!!!");
	}
		
	// Register Lvar Set Values Client Data Area for write
	if (!SUCCEEDED(SimConnect_MapClientDataNameToID(hSimConnect, LVARVALUE_CDA_NAME, CDAId++)))
	{
		LOG_ERROR("SimConnect_MapClientDataNameToID failed!!!!");
	}
	if (!SUCCEEDED(SimConnect_AddToClientDataDefinition(hSimConnect, nextDefinitionID++, SIMCONNECT_CLIENTDATAOFFSET_AUTO, sizeof(CDASETLVAR), 0, 0)))
	{
		LOG_ERROR("SimConnect_AddToClientDataDefinition failed!!!!");
	}

	// Register Execute Calculator Code Client Data Area for write
	if (!SUCCEEDED(SimConnect_MapClientDataNameToID(hSimConnect, CCODE_CDA_NAME, CDAId++)))
	{
		LOG_ERROR("SimConnect_MapClientDataNameToID failed!!!!");
	}
	if (!SUCCEEDED(SimConnect_AddToClientDataDefinition(hSimConnect, nextDefinitionID++, SIMCONNECT_CLIENTDATAOFFSET_AUTO, sizeof(CDACALCCODE), 0, 0)))
	{
		LOG_ERROR("SimConnect_AddToClientDataDefinition failed!!!!");
	}

	// Initialise are CDA Id bank - this is responsible for:
	//     - allocating cda ids
	//     - mapping the id to the name
	//     - creating the CDA
	cdaIdBank = new CDAIdBank(CDAId, hSimConnect);

	// Set timer to request config data
	configTimer = SetTimer(hWnd, UINT_PTR(this), 500, &WASMIF::StaticConfigTimer);

	// Start message loop
	while (0 == quit) {
		SimConnect_CallDispatch(hSimConnect, MyDispatchProc, this);
		Sleep(1);
	}
	SimConnectEnd();

	hThread = NULL;

	return 0;
}


VOID CALLBACK WASMIF::StaticConfigTimer(HWND hWnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
	((WASMIF*)idEvent)->ConfigTimer(hWnd, uMsg, dwTime);
}

VOID CALLBACK WASMIF::StaticRequestDataTimer(HWND hWnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
	((WASMIF*)idEvent)->RequestDataTimer(hWnd, uMsg, dwTime);
}

VOID CALLBACK WASMIF::ConfigTimer(HWND hWnd, UINT uMsg, DWORD dwTime) {

	if (!SUCCEEDED(SimConnect_RequestClientData(hSimConnect, 1, EVENT_CONFIG_RECEIVED, 1,
		SIMCONNECT_CLIENT_DATA_PERIOD_ONCE, SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_DEFAULT, 0, 0, 0)))
		//		SIMCONNECT_CLIENT_DATA_PERIOD_ONCE, SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_CHANGED, 0, 0, 0)))
	{
		LOG_ERROR("SimConnect_RequestClientData for config failed!!!!");
	}
	else
		LOG_TRACE("Config data requested...");
}


VOID CALLBACK WASMIF::RequestDataTimer(HWND hWnd, UINT uMsg, DWORD dwTime) {
	// Send event to update lvar list
	if (!SUCCEEDED(SimConnect_TransmitClientEvent(hSimConnect, SIMCONNECT_SIMOBJECT_TYPE_USER, EVENT_UPDATE_CDAS, 0, SIMCONNECT_GROUP_PRIORITY_HIGHEST, SIMCONNECT_EVENT_FLAG_GROUPID_IS_PRIORITY)))
	{
		LOG_ERROR("SimConnect_RequestClientData for lvars failed!!!!");
	}
}


DWORD WINAPI WASMIF::StaticSimConnectThreadStart(void* Param) {
	WASMIF* This = (WASMIF*)Param;
	return This->SimConnectStart();
}


bool WASMIF::start() {
	char szLogBuffer[256];
	DWORD workerThreadId = NULL;
	quit = 0;
	HRESULT hr;

	if (SUCCEEDED(hr = SimConnect_Open(&hSimConnect, "FSUIPC-WASM-IF", NULL, 0, NULL, simConnection)))
	{
		LOG_INFO("Connected to MSFS");

		if (hThread == NULL) {
			hThread = CreateThread(
				NULL,							// default security attributes
				0,								// use default stack size  
				StaticSimConnectThreadStart,	// thread function name
				(void*)this,					// argument to thread function 
				0,								// use default creation flags 
				&workerThreadId);				// returns the thread identifier 

			if (hThread == NULL) {
				LOG_ERROR("Error creating SimConnect thread");
				SimConnectEnd();
				return FALSE;
			}
		}
		return TRUE;
	}
	else {
		sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Failed on SimConnect Open: cannot connect: %s", hr == E_INVALIDARG ? "E_INVALIDARG":"E_FAIL");
		LOG_ERROR(szLogBuffer);
	}

	return FALSE;
}


void WASMIF::end() {
	quit = 1;
}


void WASMIF::SimConnectEnd() {
	char szLogBuffer[256];
	if (requestTimer) {
		KillTimer(hWnd, requestTimer);
		requestTimer = 0;
	}
	if (configTimer) {
		KillTimer(hWnd, configTimer);
		configTimer = 0;
	}

	// Clear Client Data Definitions
	// Drop existing CDAs
	if (value_cda) {
		if (!SUCCEEDED(SimConnect_ClearClientDataDefinition(hSimConnect, value_cda->getDefinitionId())))
		{
			sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Error clearing lvar value data definition with id=%d", value_cda->getId());
			LOG_ERROR(szLogBuffer);
		}
		else {
			delete value_cda;
			value_cda = 0;
		}
	}

	for (int i = 0; i < noLvarCDAs; i++)
	{
		if (!SUCCEEDED(SimConnect_ClearClientDataDefinition(hSimConnect, lvar_cdas[i]->getDefinitionId())))
		{
			sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Error clearing lvar data definition with id=%d", lvar_cdas[i]->getId());
			LOG_ERROR(szLogBuffer);
		}
		else {
			delete lvar_cdas[i];
			lvar_cdas[i] = 0;
		}
	}
	noLvarCDAs = 0;
	for (int i = 0; i < noHvarCDAs; i++)
	{
		if (!SUCCEEDED(SimConnect_ClearClientDataDefinition(hSimConnect, hvar_cdas[i]->getDefinitionId())))
		{
			sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Error clearing hvar data definition with id=%d", hvar_cdas[i]->getId());
			LOG_ERROR(szLogBuffer);
		}
		else {
			delete hvar_cdas[i];
			hvar_cdas[i] = 0;
		}
	}
	noHvarCDAs = 0;
	delete cdaIdBank;

	if (hSimConnect)
	{
		SimConnect_Close(hSimConnect);
		LOG_INFO("SimConnect_Close done");
	}

	hSimConnect = NULL;
}


void CALLBACK WASMIF::MyDispatchProc(SIMCONNECT_RECV* pData, DWORD cbData, void* pContext) {
	WASMIF* procThis = reinterpret_cast<WASMIF*>(pContext);
	procThis->DispatchProc(pData, cbData);
}


void WASMIF::DispatchProc(SIMCONNECT_RECV* pData, DWORD cbData) {
	char szLogBuffer[256];

	switch (pData->dwID)
	{
	case SIMCONNECT_RECV_ID_CLIENT_DATA:
	{
		SIMCONNECT_RECV_CLIENT_DATA* pObjData = (SIMCONNECT_RECV_CLIENT_DATA*)pData;

		switch (pObjData->dwRequestID)
		{
		case EVENT_CONFIG_RECEIVED:
		{
			LOG_DEBUG("SIMCONNECT_RECV_ID_CLIENT_DATA received: EVENT_CONFIG_RECEIVED");
			if (configTimer) {
				KillTimer(hWnd, configTimer);
				configTimer = 0;
			}
			if (requestTimer) {
				KillTimer(hWnd, requestTimer);
				requestTimer = 0;
			}


			// Drop existing CDAs
			if (value_cda) {
				if (!SUCCEEDED(SimConnect_ClearClientDataDefinition(hSimConnect, value_cda->getDefinitionId())))
				{
					sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Error clearing lvar value data definition with id=%d", value_cda->getId());
					LOG_ERROR(szLogBuffer);
				}
				cdaIdBank->returnId(value_cda->getId());
				delete value_cda;
				value_cda = 0;
			}
			
			for (int i = 0; i < noLvarCDAs; i++)
			{
				if (!SUCCEEDED(SimConnect_ClearClientDataDefinition(hSimConnect, lvar_cdas[i]->getDefinitionId())))
				{
					sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Error clearing lvar data definition with id=%d", lvar_cdas[i]->getId());
					LOG_ERROR(szLogBuffer);
				}
				cdaIdBank->returnId(lvar_cdas[i]->getId());
				delete lvar_cdas[i];
				lvar_cdas[i] = 0;
			}
			noLvarCDAs = 0;

			for (int i = 0; i < noHvarCDAs; i++)
			{
				if (!SUCCEEDED(SimConnect_ClearClientDataDefinition(hSimConnect, hvar_cdas[i]->getDefinitionId())))
				{
					sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Error clearing hvar data definition with id=%d", hvar_cdas[i]->getId());
					LOG_ERROR(szLogBuffer);
				}
				cdaIdBank->returnId(hvar_cdas[i]->getId());
				delete hvar_cdas[i];
				hvar_cdas[i] = 0;
			}
			noHvarCDAs = 0;

			CONFIG_CDA* configData = (CONFIG_CDA*)&(pObjData->dwData);

			for (int i = 0; i < MAX_NO_LVAR_CDAS + MAX_NO_HVAR_CDAS + 1; i++)
			{
				if (!configData->CDA_Size[i]) break;
				sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Config Data: name=%s, size=%d, type=%d", configData->CDA_Names[i], configData->CDA_Size[i], configData->CDA_Type[i]);
				LOG_DEBUG(szLogBuffer);
				if (configData->CDA_Type[i] == LVAR) noLvarCDAs++;
				else if (configData->CDA_Type[i] == HVAR) noHvarCDAs++;
			}

			if (!(noLvarCDAs + noHvarCDAs)) {
				LOG_TRACE("Empty config data received - requesting again");
				configTimer = SetTimer(hWnd, UINT_PTR(this), 500, &WASMIF::StaticConfigTimer);
				break;
			}

			// For each config CDA, we need to set a CDA element and request
			int lvarCount = 0;
			int hvarCount = 0;
			int valuesCount = 0;
			for (int i = 0; i < noLvarCDAs + noHvarCDAs + 1; i++)
			{
				// Need to allocate a CDA
				pair<string, int> cdaDetails = cdaIdBank->getId(configData->CDA_Size[i], configData->CDA_Names[i]);
				ClientDataArea* cda = new ClientDataArea(cdaDetails.first.c_str(), configData->CDA_Size[i], configData->CDA_Type[i]);
				cda->setId(cdaDetails.second);
				switch (configData->CDA_Type[i]) {
					case LVAR:
						lvar_cdas[lvarCount] = cda;
						break;
					case HVAR:
						hvar_cdas[hvarCount] = cda;
						break;
					case VALUE:
						value_cda = cda;
						break;
				}
				// Now set-up the definition
				if (!SUCCEEDED(SimConnect_AddToClientDataDefinition(hSimConnect, nextDefinitionID, SIMCONNECT_CLIENTDATAOFFSET_AUTO, configData->CDA_Size[i], 0, 0)))
				{
					sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Error adding client data definition id %d (%d)", nextDefinitionID, i);
					LOG_ERROR(szLogBuffer);
				}
				else
				{
					switch (configData->CDA_Type[i])
					{
					case LVAR:
						lvar_cdas[lvarCount]->setDefinitionId(nextDefinitionID);
						break;
					case HVAR:
						hvar_cdas[hvarCount]->setDefinitionId(nextDefinitionID);
						break;
					case VALUE:
						value_cda->setDefinitionId(nextDefinitionID);
						break;
					}
					sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Client data definition added with id=%d (size=%d)", nextDefinitionID, configData->CDA_Size[i]);
					LOG_DEBUG(szLogBuffer);
				}

				// Now, add lvars to data area
				HRESULT hr;
				switch (configData->CDA_Type[i]) {
					case LVAR:
						hr = SimConnect_RequestClientData(hSimConnect, cda->getId(),
								EVENT_LVARS_RECEIVED + lvarCount++, nextDefinitionID++, SIMCONNECT_CLIENT_DATA_PERIOD_ONCE, SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_DEFAULT);
						break;
					case HVAR:
						hr = SimConnect_RequestClientData(hSimConnect, cda->getId(),
								EVENT_HVARS_RECEIVED + hvarCount++, nextDefinitionID++, SIMCONNECT_CLIENT_DATA_PERIOD_ONCE, SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_DEFAULT); // SIMCONNECT_CLIENT_DATA_PERIOD_ON_SET?
						break;
					case VALUE:
						hr = SimConnect_RequestClientData(hSimConnect, cda->getId(),
								EVENT_VALUES_RECEIVED, nextDefinitionID++, SIMCONNECT_CLIENT_DATA_PERIOD_ON_SET, SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_CHANGED);
						break;
				}
				if (hr != S_OK) {
					sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Error requesting CDA '%s' with id=%d and definitionId=%d", configData->CDA_Names[i], cda->getId(), nextDefinitionID-1);
					LOG_ERROR(szLogBuffer);
					break;
				}
				else {
					sprintf_s(szLogBuffer, sizeof(szLogBuffer), "CDA '%s with id=%d and definitionId=%d requested", configData->CDA_Names[i], cda->getId(), nextDefinitionID - 1);
					LOG_DEBUG(szLogBuffer);
				}
			}

			// Request data on timer if set
			if (noLvarCDAs && this->getLvarUpdateFrequency()) {
				requestTimer = SetTimer(hWnd, UINT_PTR(this), 1000/(this->getLvarUpdateFrequency()), &WASMIF::StaticRequestDataTimer);
			}

			// Set to reload lvar/hvar tables when we receive the data
			hvarNames.clear();
			lvarNames.clear();
			lvarValues.clear();

			// Request config data again when set and changed
			if (!SUCCEEDED(SimConnect_RequestClientData(hSimConnect, 1, EVENT_CONFIG_RECEIVED, 1,
					SIMCONNECT_CLIENT_DATA_PERIOD_ON_SET, SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_CHANGED, 0, 0, 0))) {
				LOG_ERROR("SimConnect_RequestClientData for config updates failed!");
			}
			else
				LOG_TRACE("Config data updates requested.");
			break;
		}
		case EVENT_LVARS_RECEIVED: // Allow for 4 distinct lvar CDAs
		case EVENT_LVARS_RECEIVED + 1:
		case EVENT_LVARS_RECEIVED + 2:
		case EVENT_LVARS_RECEIVED + 3:
		{
			sprintf_s(szLogBuffer, sizeof(szLogBuffer), "EVENT_LVARS_RECEIVED: dwObjectID=%d, dwDefineID=%d, dwDefineCount=%d, dwentrynumber=%d, dwoutof=%d",
					pObjData->dwObjectID, pObjData->dwDefineID, pObjData->dwDefineCount, pObjData->dwentrynumber, pObjData->dwoutof);
			LOG_DEBUG(szLogBuffer);
			CDAName* lvars = (CDAName*)&(pObjData->dwData);
			// Find id of CDA
			int cdaId = 0;
			for (cdaId = 0; cdaId < MAX_NO_LVAR_CDAS; cdaId++)
			{
				if (lvar_cdas[cdaId]->getDefinitionId() == pObjData->dwDefineID) break;
			}
			if (cdaId < noLvarCDAs)
			{
				for (int i = 0; i < lvar_cdas[cdaId]->getNoItems(); i++)
				{
					sprintf_s(szLogBuffer, sizeof(szLogBuffer), "LVAR Data: name='%s'", lvars[i].name);
					LOG_TRACE(szLogBuffer);
					lvarNames.push_back(string(lvars[i].name));
					lvarValues.push_back(0.0);
				}
			}
			else {
				sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Error: CDA with id=%d not found", pObjData->dwObjectID);
				LOG_ERROR(szLogBuffer);
			}
			break;
		}
		case EVENT_HVARS_RECEIVED:
		case EVENT_HVARS_RECEIVED+1:
		case EVENT_HVARS_RECEIVED+2:
		case EVENT_HVARS_RECEIVED+3:
		{
			sprintf_s(szLogBuffer, sizeof(szLogBuffer), "EVENT_HVARS_RECEIVED: dwObjectID=%d, dwDefineID=%d, dwDefineCount=%d, dwentrynumber=%d, dwoutof=%d",
				pObjData->dwObjectID, pObjData->dwDefineID, pObjData->dwDefineCount, pObjData->dwentrynumber, pObjData->dwoutof);
			LOG_DEBUG(szLogBuffer);
			CDAName* hvars = (CDAName*)&(pObjData->dwData);
			// Find id of CDA
			int cdaId = 0;
			for (cdaId = 0; cdaId < MAX_NO_HVAR_CDAS; cdaId++)
			{
				if (hvar_cdas[cdaId]->getDefinitionId() == pObjData->dwDefineID) break;
			}
			if (cdaId < noHvarCDAs)
			{
				for (int i = 0; i < hvar_cdas[cdaId]->getNoItems(); i++)
				{
					sprintf_s(szLogBuffer, sizeof(szLogBuffer), "HVAR Data: ID=%03d, name='%s'", i, hvars[i].name);
					LOG_TRACE(szLogBuffer);
					hvarNames.push_back(string(hvars[i].name));
				}
			}
			else {
				sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Error: CDA with id=%d not found", pObjData->dwObjectID);
				LOG_ERROR(szLogBuffer);
			}
			break;
		}
		case EVENT_VALUES_RECEIVED:
		{
			sprintf_s(szLogBuffer, sizeof(szLogBuffer), "EVENT_VALUES_RECEIVED: dwObjectID=%d, dwDefineID=%d, dwDefineCount=%d, dwentrynumber=%d, dwoutof=%d",
				pObjData->dwObjectID, pObjData->dwDefineID, pObjData->dwDefineCount, pObjData->dwentrynumber, pObjData->dwoutof);
			LOG_DEBUG(szLogBuffer);

			CDAValue* values = (CDAValue*)&(pObjData->dwData);
			EnterCriticalSection(&lvarMutex);
			lvarValues.clear();
			for (int i = 0; i < value_cda->getNoItems(); i++)
			{
				sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Lvar value: ID=%03d, value=%f", i, values[i].value);
				LOG_TRACE(szLogBuffer);
				lvarValues.push_back(values[i].value);
			}
			LeaveCriticalSection(&lvarMutex);
			break;
		}
		case SIMCONNECT_RECV_ID_EXCEPTION: {
			SIMCONNECT_RECV_EXCEPTION* except = (SIMCONNECT_RECV_EXCEPTION*)pData;
			sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Simconnect Exception received: %d (dwSendID=%d)", except->dwException, except->dwSendID);
			LOG_ERROR(szLogBuffer);
			break;
		}

		default:
			LOG_TRACE("SIMCONNECT_RECV_ID_CLIENT_DATA received: default");
			break;
		}
		break;
	}

	case SIMCONNECT_RECV_ID_EVENT:
	{
		SIMCONNECT_RECV_EVENT* evt = (SIMCONNECT_RECV_EVENT*)pData;

		switch (evt->uEventID)
		{
		default:
			sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Unknow event received %d [%X]: %d", evt->uEventID, evt->uEventID, evt->dwData);
			LOG_TRACE(szLogBuffer);
			break;
		}

		break;
	}


	case SIMCONNECT_RECV_ID_QUIT:
	{
		quit = 1;
		break;
	}

	default:
		break;
	}
}


void WASMIF::createAircraftLvarFile() {
	// Send event to create aircraft lvar file
	if (!SUCCEEDED(SimConnect_TransmitClientEvent(hSimConnect, SIMCONNECT_SIMOBJECT_TYPE_USER, EVENT_LIST_LVARS, 0, SIMCONNECT_GROUP_PRIORITY_HIGHEST, SIMCONNECT_EVENT_FLAG_GROUPID_IS_PRIORITY)))
	{
		LOG_ERROR("SimConnect_TransmitClientEvent for EVENT_LIST_LVARS failed!!!!");
	}

}


void WASMIF::reload() {
	// Send event to reload lvars (and re-create CDAs)
	if (!SUCCEEDED(SimConnect_TransmitClientEvent(hSimConnect, SIMCONNECT_SIMOBJECT_TYPE_USER, EVENT_RELOAD, 0, SIMCONNECT_GROUP_PRIORITY_HIGHEST, SIMCONNECT_EVENT_FLAG_GROUPID_IS_PRIORITY)))
	{
		LOG_ERROR("SimConnect_TransmitClientEvent for EVENT_RELOAD failed!!!!");
	}

}


void WASMIF::setLvarUpdateFrequency(int freq) {
	lvarUpdateFrequency = freq;
}


int WASMIF::getLvarUpdateFrequency() {
	return lvarUpdateFrequency;
}


double WASMIF::getLvar(int lvarID) {
	EnterCriticalSection(&lvarMutex);
	double result = lvarValues.at(lvarID);
	LeaveCriticalSection(&lvarMutex);

	return result;
}


double WASMIF::getLvar(const char* lvarName) {
	int lvarId;
	for (lvarId = 0; lvarId < lvarNames.size(); lvarId++)
		if (!strcmp(lvarName, lvarNames.at(lvarId).c_str())) break;

	EnterCriticalSection(&lvarMutex);
	double result = lvarId < lvarValues.size() ? lvarValues.at(lvarId) : 0.0;
	LeaveCriticalSection(&lvarMutex);

	return result;
}

void WASMIF::getLvarValues(map<string, double >& returnMap) {

	for (int lvarId = 0; lvarId < lvarNames.size(); lvarId++) {
		EnterCriticalSection(&lvarMutex);
		returnMap.insert(make_pair(lvarNames.at(lvarId), lvarValues.at(lvarId)));
		LeaveCriticalSection(&lvarMutex);
	}
}

void WASMIF::setLvar(unsigned short id, unsigned short value) {

	DWORD param;
	BYTE* p = (BYTE*)&param;

	memcpy(p, &id, 2);
	memcpy(p + 2, &value, 2);
	setLvar(param);
}


void WASMIF::setLvar(unsigned short id, const char* value) {
	char szLogBuffer[512];
	char* p;
	double converted = strtod(value, &p);
	if (*p) {
		// conversion failed because the input wasn't a number
		memcpy(&converted, value, sizeof(double));
		setLvar(id, converted);
		sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Setting lvar value as string: %.*s", 8, (char*)&converted);
		LOG_DEBUG(szLogBuffer);
	}
	else {
		// use converted
		// Check if we have an integer
		int i, r, n;
		r = sscanf_s(value, "%d%n", &i, &n);
		if (r == 1 && n == strlen(value)) {
			// converted is integer
			if (converted > 0 && converted < 65536) {
				unsigned short value = (unsigned short)converted;
				setLvar(id, value);
				sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Setting lvar value as unsigned short: %u", value);
				LOG_DEBUG(szLogBuffer);
			}
			else if (converted > -32769 && converted < 32768) {
				short value = (short)converted;
				setLvar(id, value);
				sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Setting lvar value as short: %u", value);
				LOG_DEBUG(szLogBuffer);
			}
		}
		else { // floating point number
			setLvar(id, converted);
			sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Setting lvar value as double: %f", converted);
			LOG_DEBUG(szLogBuffer);
		}
	}
}


void WASMIF::setLvar(unsigned short id, double value) {
	char szLogBuffer[256];
	DWORD dwLastID;
	CDASETLVAR lvar;
	lvar.id = id;
	lvar.lvarValue = value;
	if (!SUCCEEDED(SimConnect_SetClientData(hSimConnect, 2, 2, 0, 0, sizeof(CDASETLVAR), &lvar))) {
		sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Error setting Client Data lvar value: %d=%f", lvar.id, lvar.lvarValue);
		LOG_ERROR(szLogBuffer);
	}
	else {
		SimConnect_GetLastSentPacketID(hSimConnect, &dwLastID);
		sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Lvar set Client Data Area updated [requestID=%d]", dwLastID);
		LOG_TRACE(szLogBuffer);
	}
}


void WASMIF::setLvar(unsigned short id, short value) {
	BOOL isNegative = FALSE;

	if (value < 0) {
		value = -value;
		isNegative = TRUE;
	}

	DWORD param;
	BYTE* p = (BYTE*)&param;

	memcpy(p, &id, 2);
	memcpy(p + 2, &value, 2);
	setLvar(param);
}


void WASMIF::setLvar(DWORD param) {
	char szLogBuffer[256];
	if (!SUCCEEDED(SimConnect_TransmitClientEvent(hSimConnect, SIMCONNECT_SIMOBJECT_TYPE_USER, EVENT_SET_LVAR, param, SIMCONNECT_GROUP_PRIORITY_HIGHEST, SIMCONNECT_EVENT_FLAG_GROUPID_IS_PRIORITY)))
	{
		LOG_ERROR("SimConnect_TransmitClientEvent for EVENT_SET_LVAR failed!!!!");
	}
	else {
		unsigned short value = static_cast<unsigned short>(param >> (2 * 8));
		unsigned short id = static_cast<unsigned short>(param % (1 << (2 * 8)));
		sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Control sent to set lvars with parameter %d (%X): lvarId=%u (%X), value=%u (%X)", param, param,
			id, id, value, value);
		LOG_DEBUG(szLogBuffer);
	}

}


void WASMIF::executeCalclatorCode(const char* code) {
	char szLogBuffer[MAX_CALC_CODE_SIZE + 64];
	DWORD dwLastID;
	CDACALCCODE ccode;
	strncpy_s(ccode.calcCode, sizeof(ccode.calcCode), code, MAX_CALC_CODE_SIZE);
	ccode.calcCode[MAX_CALC_CODE_SIZE - 1] = '\0';
	if (!SUCCEEDED(SimConnect_SetClientData(hSimConnect, 3, 3, 0, 0, sizeof(CDACALCCODE), &ccode))) {
		sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Error setting Client Data Calculator Code: '%s'", ccode.calcCode);
		LOG_ERROR(szLogBuffer);
	}
	else {
		SimConnect_GetLastSentPacketID(hSimConnect, &dwLastID);
		sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Calcultor Code Client Data Area updated [requestID=%d]", dwLastID);
		LOG_TRACE(szLogBuffer);
	}
}


void WASMIF::logLvars() {
	char szLogBuffer[256];
	for (int i = 0; i < lvarNames.size(); i++) {
		sprintf_s(szLogBuffer, sizeof(szLogBuffer), "ID=%03d %s = %f", i, lvarNames.at(i).c_str(), lvarValues.at(i));
		LOG_INFO(szLogBuffer);
	}
}


void WASMIF::getLvarList(unordered_map<int, string >& returnMap) {
	for (int i = 0; i < lvarNames.size(); i++) {
		returnMap.insert(make_pair(i, lvarNames.at(i)));
	}
}


void WASMIF::setHvar(int id) {
	char szLogBuffer[256];
	if (!SUCCEEDED(SimConnect_TransmitClientEvent(hSimConnect, SIMCONNECT_SIMOBJECT_TYPE_USER, EVENT_SET_HVAR, id, SIMCONNECT_GROUP_PRIORITY_HIGHEST, SIMCONNECT_EVENT_FLAG_GROUPID_IS_PRIORITY)))
	{
		LOG_ERROR("SimConnect_TransmitClientEvent for EVENT_SET_HVAR failed!!!!");
	}
	else {
		sprintf_s(szLogBuffer, sizeof(szLogBuffer), "Control sent to set hvar with id=%d (%X)", id, id);
		LOG_DEBUG(szLogBuffer);
	}
}


void WASMIF::logHvars() {
	char szLogBuffer[256];
	for (int i = 0; i < hvarNames.size(); i++) {
		sprintf_s(szLogBuffer, sizeof(szLogBuffer), "ID=%03d %s", i, hvarNames.at(i).c_str());
		LOG_INFO(szLogBuffer);
	}
}


void WASMIF::getHvarList(unordered_map<int, string >& returnMap) {
	for (int i = 0; i < hvarNames.size(); i++) {
		returnMap.insert(make_pair(i, hvarNames.at(i)));
	}
}

