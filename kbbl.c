/*
 * Copyright 2023 PopGuac
 * Open sourced with ISC license. Refer to LICENSE for details.
 *
 * compile with mingw/gcc:
 *  gcc kbbl.c -o kbbl -s -mwindows -luuid -D_WIN32_WINNT=0x0600 -DWINVER=0x0600 -DWIN32_LEAN_AND_MEAN -D__USE_MINGW_ANSI_STDIO=0 -Wall -Wextra
 *
 * some extra warning flags that can be added to gcc compile:
 *  -Wall -Wextra -Wpedantic -Wshadow -Wmissing-prototypes -Wstrict-prototypes -Wbad-function-cast -Wcast-align -Wno-missing-field-initializers -Wduplicated-cond -Wduplicated-branches -Wrestrict -Wnull-dereference -Wjump-misses-init -Wdouble-promotion -Wformat=2 -Wdate-time
 *
 * compile with MSVC:
 *  cl -nologo /MD -Ox -Z7 -W2 "-Fekbbl.exe" kbbl.c advapi32.lib user32.lib
 *  cl -nologo /MD -Ox -Z7 -W4 -wd4127 -wd4204 "-Fekbbl.exe" kbbl.c advapi32.lib user32.lib
 *
 *
 * TODO
 *  setup github repo that will build everything on github's systems - including winring0; zip and upload a release file
 *  determine minimum windows version required; test on windows 11 and others. tested so far: win10
 *  check event viewer to make sure there's no errors or warnings being logged for the service
 *  make sure only 1 instance of program is running in server mode? if, for some reason, a 2nd instance is run - then error out?
 *  test installing/running from a directory containing spaces and weird unicode characters
 *
 * TODO: replace stdlib functions to remove dependency on msvcrt?
 *   exit -> ExitProcess
 *   fprintf vfprintf
 *   fopen fwrite fflush fclose
 *   memcpy
 *   strlen strcmp strrchr
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <tchar.h>
#include <windows.h>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
#include "OlsApiInit.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop
#endif


DEFINE_GUID(GUID_CONSOLE_DISPLAY_STATE, 0x6fe69556, 0x704a, 0x47a0, 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47);

// name of the windows service
const char* SVC_NAME = "LenovoKeyboardBacklightFix";
// name of pipe used by server & client to communicate when user toggles the backlight setting
const char* PIPENAME = "\\\\.\\Pipe\\LenovoKeyboardBacklightFixPipe";
// registry key where the state of the keyboard backlight setting is stored and reloaded on bootup
const char* REGKEY = "SOFTWARE\\LenovoKeyboardBacklightFix";
// registry value name for the previous key where the state of the keyboard backlight setting is stored and reloaded on bootup
const char* REGVALUENAME = "valWhenWake";


SERVICE_STATUS_HANDLE G_STATUS_HANDLE = NULL;
HANDLE G_STOP_EVENT = NULL;

uint8_t G_VAL_TO_RESTORE = 0;
UCHAR G_SLEEP_STATE = 0;
FILE* G_LOG = NULL;



/**
 * ############################################################################################
 * #
 * # The following chunk of code is derived from:
 * #    https://github.com/RehabMan/HP-ProBook-4x30s-Fan-Reset/blob/master/Fanreset/Fanreset.c
 * #
 * #############################################################################################
 */

// Registers of the embedded controller
#define EC_DATAPORT     (0x62)      // EC data io-port
#define EC_CTRLPORT     (0x66)      // EC control io-port

// Embedded controller status register bits
#define EC_STAT_OBF     (0x01)      // Output buffer full
#define EC_STAT_IBF     (0x02)      // Input buffer full

// Embedded controller commands
#define EC_CTRLPORT_READ    ((uint8_t)0x80)
#define EC_CTRLPORT_WRITE   ((uint8_t)0x81)

static int waitPortStatus(int mask, int wanted) {
	int timeout = 1000;
	int tick = 10;

	// wait until input on control port has desired state or times out
	int time;
	for (time = 0; time < timeout; time += tick) {
		uint8_t data = ReadIoPortByte(EC_CTRLPORT);

		// check for desired result
		if (wanted == (data & mask)) {
			return 1;
		}

		Sleep(tick);
	}

	return 0;
}

static int writeAndWait(int port, uint8_t data) {
	WriteIoPortByte(port, data);
	return waitPortStatus(EC_STAT_IBF, 0);
}

static int WriteByteToEC(uint8_t offset, uint8_t data) {
	if (!waitPortStatus(EC_STAT_IBF | EC_STAT_OBF, 0)) {
		return 0;
	}
	if (!writeAndWait(EC_CTRLPORT, EC_CTRLPORT_WRITE)) {
		return 0;
	}
	if (!writeAndWait(EC_DATAPORT, offset)) {
		return 0;
	}
	return writeAndWait(EC_DATAPORT, data);
}

static int ReadByteFromEC(uint8_t offset, uint8_t *pData) {
	if (!waitPortStatus(EC_STAT_IBF | EC_STAT_OBF, 0)) {
		return 0;
	}
	if (!writeAndWait(EC_CTRLPORT, EC_CTRLPORT_READ)) {
		return 0;
	}
	if (!writeAndWait(EC_DATAPORT, offset)) {
		return 0;
	}
	*pData = ReadIoPortByte(EC_DATAPORT);
	return 1;
}

/**
 * ############################################################################################
 */




#define LOGF(format, ...) mylogf(format "\n", __VA_ARGS__)

static void logWinErrorCode(const char* msg, DWORD errCode) {
	if (G_LOG == NULL) {
		return;
	}
	// TODO: add date/time to log message?
	msg = msg == NULL ? "" : msg;
	size_t msgLen = strlen(msg);
	LPSTR allocMsg = NULL;
	size_t allocMsgLen = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, errCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &allocMsg, 0, NULL);
	const char* missingEOL = msgLen == 0 || msg[msgLen - 1] == '\n' ? "" : "\n";
	fprintf(G_LOG, "%s%serr 0x%08lx: %s\n", msg, missingEOL, errCode, (allocMsg == NULL || allocMsgLen == 0) ? "" : allocMsg);
	fflush(G_LOG);
	LocalFree(allocMsg);
}

static void logWinError(const char* msg) {
	logWinErrorCode(msg, GetLastError());
}

static void mylogf(const char* format, ...) {
	if (G_LOG == NULL) {
		return;
	}

	// TODO: don't print with 2 separate calls
	char buff[64];
	time_t timeResult;
	time(&timeResult);
	struct tm* tmData = localtime(&timeResult);
	strftime(buff, sizeof(buff), "%Y-%m-%d_%H:%M:%S ", tmData);
	fputs(buff, G_LOG);

	va_list args;
	va_start(args, format);
	vfprintf(G_LOG, format, args);
	fflush(G_LOG);
	va_end(args);
}

static void logMsg(const char* msg) {
	mylogf("%s\n", msg);
}

static int regSetValueInternal(HKEY hKey, const char* subkey, const char* valueName, DWORD dwType, const BYTE* val, DWORD valLen) {
	int success = 0;
	HKEY hKey2;
	// TODO: open with KEY_SET_VALUE rather than KEY_ALL_ACCESS?
	LSTATUS err = RegOpenKeyEx(hKey, subkey, 0, KEY_ALL_ACCESS, &hKey2);
	if (err == ERROR_SUCCESS) {
		err = RegSetValueEx(hKey2, valueName, 0, dwType, val, valLen);
		if (err == ERROR_SUCCESS) {
			success = 1;
		} else {
			logWinErrorCode("RegSetValueEx failed", err);
		}
		RegCloseKey(hKey2);
	} else {
		logWinErrorCode("RegOpenKeyEx failed", err);
	}
	return success;
}

static int regSetDword(HKEY hKey, const char* subkey, const char* valueName, DWORD val) {
	return regSetValueInternal(hKey, subkey, valueName, REG_DWORD, (const BYTE*)&val, sizeof(val));
}

static int regGetDwordIfExists(HKEY hKey, const char* subkey, const char* valueName, DWORD* pVal) {
	int exists = 0;
	HKEY hKey2;
	LSTATUS err = RegOpenKeyEx(hKey, subkey, 0, KEY_READ, &hKey2);
	if (err == ERROR_SUCCESS) {
		DWORD dwType;
		DWORD valInReg;
		DWORD valInRegLen = sizeof(valInReg);
		err = RegQueryValueEx(hKey2, valueName, NULL, &dwType, (BYTE*) &valInReg, &valInRegLen);
		if (err == ERROR_SUCCESS && dwType == REG_DWORD) {
			exists = 1;
			if (pVal != NULL) {
				*pVal = valInReg;
			}
		}
		RegCloseKey(hKey2);
	}
	return exists;
}

static int updateRegistry(DWORD valWhenWake) {
	return regSetDword(HKEY_LOCAL_MACHINE, REGKEY, REGVALUENAME, valWhenWake);
}

static int runPipeServer(HANDLE hStopEvent) {
	int success = 0;
	HANDLE hPipe = NULL;
	SECURITY_ATTRIBUTES sa = {0};
	PSECURITY_DESCRIPTOR pSD = NULL;

	// server process is running as admin (needed for WinRing0) and the client connecting to the pipe is not admin; relax security when creating the pipe
	// https://stackoverflow.com/a/38413449
	// TODO: is memory allocation actually required here? can the stack be used instead?
	pSD = (PSECURITY_DESCRIPTOR) LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
	if (pSD == NULL) {
		logWinError("LocalAlloc failed");
		goto cleanup;
	}
	if (!InitializeSecurityDescriptor(pSD, SECURITY_DESCRIPTOR_REVISION)) {
		logWinError("InitializeSecurityDescriptor failed");
		goto cleanup;
	}
	if (!SetSecurityDescriptorDacl(pSD, TRUE, NULL, FALSE)) {
		logWinError("SetSecurityDescriptorDacl failed");
		goto cleanup;
	}
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = pSD;
	sa.bInheritHandle = FALSE;

	hPipe = CreateNamedPipe(PIPENAME, PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES, 1, 1, NMPWAIT_USE_DEFAULT_WAIT, &sa);
	if (hPipe == INVALID_HANDLE_VALUE) {
		logWinError("CreateNamedPipe failed");
		goto cleanup;
	}

	while (1) {
		if (hStopEvent != NULL) {
			DWORD waitRes = WaitForSingleObject(hStopEvent, 0);
			if (waitRes == WAIT_OBJECT_0) {
				success = 1;
				goto cleanup;
			} else if (waitRes == WAIT_FAILED) {
				logWinError("WaitForSingleObject failed");
				goto cleanup;
			}
		}

		// note: ConnectNamedPipe() can return 0 if client connects in-between the call to CreateNamedPipe() and ConnectNamedPipe();
		//       must check GetLastError() for ERROR_PIPE_CONNECTED.
		if (!ConnectNamedPipe(hPipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
			logWinError("ConnectNamedPipe failed");
			goto cleanup;
		}

		char buff[1];
		DWORD numRead = 0;
		while (numRead < 1) {
			if (!ReadFile(hPipe, buff, sizeof(buff), &numRead, NULL)) {
				logWinError("ReadFile failed");
				goto cleanup;
			}
		}

		LOGF("Recv pipe event; sleep state: %d", G_SLEEP_STATE);

		// note: only read & store the keyboard value when system is in active mode (not when sleeping)
		if (G_SLEEP_STATE == 1) {
			if (ReadByteFromEC(0x0d, &G_VAL_TO_RESTORE)) {
				LOGF("Read val: %d", G_VAL_TO_RESTORE);
				if (!updateRegistry(G_VAL_TO_RESTORE)) {
					goto cleanup;
				}
			} else {
				logMsg("ReadByteFromEC failed");
			}
		}

		if (!DisconnectNamedPipe(hPipe)) {
			logWinError("DisconnectNamedPipe failed");
			goto cleanup;
		}
	}

	cleanup:
	if (hPipe != NULL) {
		CloseHandle(hPipe);
	}
	LocalFree(pSD);
	return success;
}

static int signalToggle(void) {
	HANDLE hPipe = CreateFile(PIPENAME, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (hPipe == INVALID_HANDLE_VALUE) {
		logWinError("CreateFile failed");
		return 0;
	}
	if (!WriteFile(hPipe, "1", 1, NULL, NULL)) {
		logWinError("WriteFile failed");
		CloseHandle(hPipe);
		return 0;
	}
	CloseHandle(hPipe);
	return 1;
}

static int loadWinRing0(void) {
	HMODULE lib;
	if (!InitOpenLibSys(&lib)) {
		logMsg("InitOpenLibSys failed");
		return 0;
	}
	DWORD dllStatus = GetDllStatus();
	if (dllStatus != OLS_DLL_NO_ERROR) {
		LOGF("GetDllStatus returned error %ld", dllStatus);
		return 0;
	}
	return 1;
}

static FILE* openLogFile(const char* logName) {
	// TODO: path to exe could include unicode characters - should use wide function calls?
	char pathbuff[MAX_PATH];
	DWORD pathLen = GetModuleFileName(NULL, pathbuff, sizeof(pathbuff));
	if (pathLen == 0) {
		logWinError("GetModuleFileName failed");
		return NULL;
	} else if (pathLen >= sizeof(pathbuff)) {
		logMsg("path to exe is too long");
		return NULL;
	}

	char* pos = strrchr(pathbuff, '\\');
	if (pos == NULL) {
		logMsg("last dir sep not found");
		return NULL;
	}
	pos[0] = 0;

	size_t pathBuffLen = strlen(pathbuff);
	if (pathBuffLen == 0) {
		logMsg("pathbuff is empty");
		return NULL;
	}
	size_t logNameLen = strlen(logName);
	if (pathBuffLen + 1 + logNameLen + 1 > MAX_PATH) {
		logMsg("log path is too long");
		return NULL;
	}
	pathbuff[pathBuffLen] = '\\';
	memcpy(pathbuff + pathBuffLen + 1, logName, logNameLen + 1);

	return fopen(pathbuff, "a");
}




static void restoreBacklightValue(void) {
	// TODO: make sleep time configurable - registry?
	// TODO: is this Sleep() necessary? how long should it be? seems like this is required else sometimes the backlight isn't restored properly when waking from monitor-off state
	//   why? maybe the order of events differs and somehow the keyboard backlight is changed by lenovo software after this event is processed
	// TODO: don't wait inside this event handler - should use a separate thread so caller isn't forced to wait?
	Sleep(500);
	if (WriteByteToEC(0x0d, G_VAL_TO_RESTORE)) {
		LOGF("restored val: %d", G_VAL_TO_RESTORE);
	} else {
		logMsg("WriteByteToEC failed");
	}
}

static DWORD WINAPI ServiceCtrlHandler(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext) {
	UNREFERENCED_PARAMETER(lpContext);

	switch (dwControl) {
		case SERVICE_CONTROL_STOP: {

			SERVICE_STATUS svcStatus = {0};
			svcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
			svcStatus.dwCurrentState = SERVICE_STOP_PENDING;
			svcStatus.dwCheckPoint = 4;
			if (!SetServiceStatus(G_STATUS_HANDLE, &svcStatus)) {
				logWinError("SetServiceStatus failed");
				// TODO: exit?
			}

			if (!SetEvent(G_STOP_EVENT)) {
				logWinError("SetEvent failed");
				// TODO: exit?
			}

			if (!signalToggle()) {
				// TODO: handle error?
				// TODO: exit?
			}

			break;
		}

		case SERVICE_CONTROL_POWEREVENT:
			if (dwEventType == PBT_POWERSETTINGCHANGE) {
				//logMsg("PBT_POWERSETTINGCHANGE");
				POWERBROADCAST_SETTING* stg = (POWERBROADCAST_SETTING*) lpEventData;
				if (stg != NULL) {
					if (IsEqualGUID(&stg->PowerSetting, &GUID_CONSOLE_DISPLAY_STATE) && stg->DataLength >= 1) {
						// The Data member is a DWORD with one of the following values.
						// 0x0 - The display is off.
						// 0x1 - The display is on.
						// 0x2 - The display is dimmed.
						UCHAR dataVal = stg->Data[0];
						LOGF("power-console-display state is now: %d", dataVal);
						G_SLEEP_STATE = dataVal;
						if (dataVal == 1) {
							restoreBacklightValue();
						}
					} else if (IsEqualGUID(&stg->PowerSetting, &GUID_LIDSWITCH_STATE_CHANGE) && stg->DataLength >= 1) {
						// The Data member is a DWORD that indicates the current lid state:
						// 0x0 - The lid is closed.
						// 0x1 - The lid is opened.
						UCHAR dataVal = stg->Data[0];
						LOGF("power-lid state is now: %d", dataVal);
						G_SLEEP_STATE = dataVal;
						if (dataVal == 1) {
							restoreBacklightValue();
						}
					}
				}
			}
			break;

		default:
			break;
	}

	// TODO: should sometimes return ERROR_CALL_NOT_IMPLEMENTED? depends on value of dwControl?
	return NO_ERROR;
}

static void WINAPI serviceMainCB(DWORD argc, LPTSTR *argv) {
	UNREFERENCED_PARAMETER(argc);
	UNREFERENCED_PARAMETER(argv);

	G_STATUS_HANDLE = RegisterServiceCtrlHandlerEx(SVC_NAME, ServiceCtrlHandler, NULL);
	if (G_STATUS_HANDLE == NULL) {
		logWinError("RegisterServiceCtrlHandlerEx failed");
		return;
	}

	SERVICE_STATUS svcStatus = {0};
	svcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	svcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
	svcStatus.dwCurrentState = SERVICE_RUNNING;
	if (!SetServiceStatus(G_STATUS_HANDLE, &svcStatus)) {
		logWinError("SetServiceStatus failed");
		ExitProcess(EXIT_FAILURE);
	}


	// TODO: rather than calling RegisterPowerSettingNotification() - can add "SERVICE_ACCEPT_POWEREVENT" to dwControlsAccepted above?
	HPOWERNOTIFY hNotify = RegisterPowerSettingNotification(G_STATUS_HANDLE, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_SERVICE_HANDLE);
	if (hNotify == NULL) {
		logWinError("RegisterPowerSettingNotification failed");
		ExitProcess(EXIT_FAILURE);
	}
	hNotify = RegisterPowerSettingNotification(G_STATUS_HANDLE, &GUID_LIDSWITCH_STATE_CHANGE, DEVICE_NOTIFY_SERVICE_HANDLE);
	if (hNotify == NULL) {
		logWinError("RegisterPowerSettingNotification failed");
		ExitProcess(EXIT_FAILURE);
	}

	runPipeServer(G_STOP_EVENT);
	CloseHandle(G_STOP_EVENT);
	G_STOP_EVENT = NULL;

	// TODO: close hNotify?


	svcStatus.dwControlsAccepted = 0;
	svcStatus.dwCurrentState = SERVICE_STOPPED;
	//svcStatus.dwWin32ExitCode = 0;
	svcStatus.dwCheckPoint = 3;
	if (!SetServiceStatus(G_STATUS_HANDLE, &svcStatus)) {
		logWinError("SetServiceStatus failed");
		ExitProcess(EXIT_FAILURE);
	}
}

int main(int argc, const char* argv[]) {
	char runToggle = 0;
	char runSvc = 0;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--log") == 0) {
			G_LOG = openLogFile("log.txt");
		} else if (strcmp(argv[i], "--toggled") == 0) {
			runToggle = 1;
		} else if (strcmp(argv[i], "--svc") == 0) {
			runSvc = 1;
		}
	}

	if (runToggle) {
		return signalToggle() ? EXIT_SUCCESS : EXIT_FAILURE;
	}
	if (!runSvc) {
		// require --svc or --toggled arg so that the program cannot be run by itself without any args
		fputs("invalid args\n", stderr);
		return EXIT_FAILURE;
	}


	// note: loading WinRing0 requires admin priveleges
	if (!loadWinRing0()) {
		return EXIT_FAILURE;
	}

	DWORD valInReg;
	if (regGetDwordIfExists(HKEY_LOCAL_MACHINE, REGKEY, REGVALUENAME, &valInReg)) {
		// if the registry entry exists then load/restore the previous value
		logMsg("restoring from registry");
		if (WriteByteToEC(0x0d, valInReg)) {
			G_VAL_TO_RESTORE = valInReg;
			LOGF("restored val: %ld", valInReg);
		} else {
			logMsg("WriteByteToEC failed");
			return EXIT_FAILURE;
		}
	} else {
		if (ReadByteFromEC(0x0d, &G_VAL_TO_RESTORE)) {
			LOGF("Read val: %d", G_VAL_TO_RESTORE);
			if (!updateRegistry(G_VAL_TO_RESTORE)) {
				return EXIT_FAILURE;
			}
		} else {
			logMsg("ReadByteFromEC failed");
			return EXIT_FAILURE;
		}
	}


	G_STOP_EVENT = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (G_STOP_EVENT == NULL) {
		logWinError("CreateEvent failed");
		return EXIT_FAILURE;
	}


	SERVICE_TABLE_ENTRY svcEntries[] = {
		{(char*) SVC_NAME, (LPSERVICE_MAIN_FUNCTION) serviceMainCB},
		{NULL, NULL}
	};

	// note: If StartServiceCtrlDispatcher succeeds, it connects the calling thread to the service control manager and
	//   does not return until all running services in the process have entered the SERVICE_STOPPED state.
	if (!StartServiceCtrlDispatcher(svcEntries)) {
		logWinError("StartServiceCtrlDispatcher failed");
		return EXIT_FAILURE;
	}

	// TODO: get and return a status code from elsewhere in the program - ie, service event handler function

	return EXIT_SUCCESS;
}
