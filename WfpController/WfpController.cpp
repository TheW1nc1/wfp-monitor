#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <winsvc.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <tlhelp32.h>
#pragma comment(lib, "Ws2_32.lib")

#include "../WfpMonitor/WfpMonitor.h"

using namespace std;

//
// Install/Uninstall Driver Service
//
bool ManageDriver(const wstring& serviceName, const wstring& driverPath, bool install) {
    SC_HANDLE scManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scManager) {
        wcerr << L"Failed to open SC Manager: " << GetLastError() << endl;
        return false;
    }

    if (install) {
        SC_HANDLE scService = CreateServiceW(
            scManager, serviceName.c_str(), serviceName.c_str(),
            SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
            driverPath.c_str(), NULL, NULL, NULL, NULL, NULL);

        if (!scService && GetLastError() != ERROR_SERVICE_EXISTS) {
            wcerr << L"Failed to create service: " << GetLastError() << endl;
            CloseServiceHandle(scManager);
            return false;
        }
        
        if (!scService) scService = OpenServiceW(scManager, serviceName.c_str(), SERVICE_ALL_ACCESS);
        if (scService) {
            if (!StartService(scService, 0, NULL) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
                wcerr << L"Failed to start service: " << GetLastError() << endl;
            } else {
                wcout << L"Service started successfully." << endl;
            }
            CloseServiceHandle(scService);
        }
    } else {
        SC_HANDLE scService = OpenServiceW(scManager, serviceName.c_str(), SERVICE_ALL_ACCESS);
        if (scService) {
            SERVICE_STATUS status;
            ControlService(scService, SERVICE_CONTROL_STOP, &status);
            DeleteService(scService);
            CloseServiceHandle(scService);
            wcout << L"Service stopped and deleted." << endl;
        }
    }

    CloseServiceHandle(scManager);
    return true;
}

DWORD GetProcessIdByName(const wstring& processName) {
    PROCESSENTRY32W processEntry;
    processEntry.dwSize = sizeof(PROCESSENTRY32W);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    if (Process32FirstW(snapshot, &processEntry)) {
        do {
            if (processName == processEntry.szExeFile) {
                CloseHandle(snapshot);
                return processEntry.th32ProcessID;
            }
        } while (Process32NextW(snapshot, &processEntry));
    }
    CloseHandle(snapshot);
    return 0;
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        wcout << L"Usage: WfpController.exe <path_to_test.exe>" << endl;
        return 1;
    }

    wstring exePath = argv[1];
    wstring testExeName = L"test.exe";
    
    // Extract filename if full path given
    size_t lastSlash = exePath.find_last_of(L"\\/");
    if (lastSlash != wstring::npos) {
        testExeName = exePath.substr(lastSlash + 1);
    }

    wchar_t currentDir[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, currentDir);
    wstring driverPath = wstring(currentDir) + L"\\WfpMonitor.sys";

    wcout << L"[*] Installing WfpMonitor driver..." << endl;
    if (!ManageDriver(L"WfpMonitor", driverPath, true)) {
        wcout << L"[-] Failed to load WfpMonitor driver. Ensure you run as Administrator." << endl;
        return 1;
    }

    HANDLE hDevice = CreateFileW(WFP_MONITOR_SYMLINK_NAME, GENERIC_READ | GENERIC_WRITE,
                                0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hDevice == INVALID_HANDLE_VALUE) {
        wcerr << L"Failed to open device: " << GetLastError() << endl;
        ManageDriver(L"WfpMonitor", driverPath, false);
        return 1;
    }

    wcout << L"[*] Starting target process: " << exePath << endl;
    
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessW(NULL, &exePath[0], NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        wcerr << L"Failed to start target process: " << GetLastError() << endl;
        CloseHandle(hDevice);
        ManageDriver(L"WfpMonitor", driverPath, false);
        return 1;
    }

    wcout << L"[*] Process started with PID: " << pi.dwProcessId << endl;

    // Send PID to driver
    WFP_MONITOR_SET_PID_IN setPidIn;
    setPidIn.ProcessId = pi.dwProcessId;
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDevice, IOCTL_WFP_MONITOR_SET_PID, &setPidIn, sizeof(setPidIn), NULL, 0, &bytesReturned, NULL)) {
        wcerr << L"Failed to send PID to driver: " << GetLastError() << endl;
    } else {
        wcout << L"[*] Driver successfully hooked to PID " << pi.dwProcessId << endl;
    }

    ResumeThread(pi.hThread);

    // Wait for the process to exit
    wcout << L"[*] Waiting for process to complete..." << endl;
    WaitForSingleObject(pi.hProcess, INFINITE);
    wcout << L"[*] Process exited." << endl;

    // Retrieve stats
    WFP_MONITOR_STATS_OUT statsOut = { 0 };
    if (!DeviceIoControl(hDevice, IOCTL_WFP_MONITOR_GET_STATS, NULL, 0, &statsOut, sizeof(statsOut), &bytesReturned, NULL)) {
        wcerr << L"Failed to get stats from driver: " << GetLastError() << endl;
    }

    CloseHandle(hDevice);

    wcout << L"[*] Stopping and uninstalling driver..." << endl;
    ManageDriver(L"WfpMonitor", driverPath, false);

    // Format results
    string resultStrDst = "ANY";
    if (statsOut.DestIp != 0) {
        char ipStr[INET_ADDRSTRLEN];
        // Convert from network byte order if necessary.
        // IP_REMOTE_ADDRESS comes in host byte order usually from FWPS_FIELD.
        // Let's assume it was taken in host byte order:
        struct in_addr addr;
        addr.S_un.S_addr = htonl(statsOut.DestIp);
        inet_ntop(AF_INET, &addr, ipStr, sizeof(ipStr));
        
        resultStrDst = string(ipStr) + ":" + to_string(statsOut.DestPort);
    }

    wcout << L"--------------------------------------" << endl;
    wcout << L"DST=" << wstring(resultStrDst.begin(), resultStrDst.end()) << endl;
    wcout << L"TX_BYTES=" << statsOut.TxBytes << endl;
    wcout << L"RX_BYTES=" << statsOut.RxBytes << endl;
    wcout << L"--------------------------------------" << endl;

    // Write to result.txt (in current directory)
    ofstream outFile("result.txt");
    if (outFile.is_open()) {
        outFile << "DST=" << resultStrDst << "\n";
        outFile << "TX_BYTES=" << statsOut.TxBytes << "\n";
        outFile << "RX_BYTES=" << statsOut.RxBytes << "\n";
        outFile.close();
        wcout << L"[*] Results written to result.txt." << endl;
    } else {
        wcerr << L"[-] Failed to open result.txt for writing." << endl;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return 0;
}
