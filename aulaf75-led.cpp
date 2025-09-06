#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <dbt.h>
#include <initguid.h>
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <tlhelp32.h>
#include <shellapi.h>
#include <guiddef.h>
#include <thread>
#include <WinNT.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

HANDLE g_hDevice = nullptr;
bool g_ledOn = true;

static auto INIT_COMMAND_HEX =
	"06840000010080000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";

static auto FINAL_COMMAND_HEX =
	"060a000001000002000000000000000000000000000000000000000000ff000000ff000000ffffff00ff00ff00ffffffffffff000000ff000000ffffff00ff00ff00ffffffffffff000000ff000000ffffff00ff00ff00ffffffffffff000000ff000000ffffff00ff00ff00ffffffffffff000000ff000000ffffff00ff00ff00ffffffffffff000000ff000000ffffff00ff00ff00ffffffffffff000000ff000000ffffff00ff00ff00ffffffffffff000000ff000000ffffff00ff00ff00ffffffffffff000000ff000000ffffff00ff00ff00ffffffffffff000000ff000000ffffff00ff00ff00ffffffffffff000000ff000000ffffff00ff00ff00ffffffffffff000000ff000000ffffff00ff00ff00ffffffffffff000000ff000000ffffff00ff00ff00ffffffffff00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000005aa500000000";

static auto TURN_OFF_COMMAND_HEX =
	"06040000010080000003030100000404070000200100000000000300020100ff0a00000001000301000000000000000000000000000000000000000000000000ffff09470947094709470947094709470947094709470907094709470947094709470937093709370937074707470744074407440744074407440744040904040404040404045aa5000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";

static auto TURN_ON_COMMAND_HEX =
	"0604000001008000000303010000040407000b200100000000000300020100ff0a00000001000301000000000000000000000000000000000000000000000000ffff09470947094709470947094709470947094709470907094709470947094709470937093709370937074707470744074407440744074407440744040904040404040404045aa5000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";

std::vector<BYTE> HexStringToBytes(const std::string& hex)
{
	std::vector<BYTE> bytes;
	std::string clean;
	for (char c : hex)
	{
		if (!isspace(static_cast<unsigned char>(c)))
			clean.push_back(c);
	}
	if (clean.size() % 2 != 0) throw std::runtime_error("Invalid hex string");

	for (size_t i = 0; i < clean.size(); i += 2)
	{
		std::string byteStr = clean.substr(i, 2);
		auto b = static_cast<BYTE>(strtol(byteStr.c_str(), nullptr, 16));
		bytes.push_back(b);
	}
	return bytes;
}

HANDLE OpenRGBInterface()
{
	GUID hidGuid;
	HidD_GetHidGuid(&hidGuid);

	auto deviceInfo = SetupDiGetClassDevs(&hidGuid, nullptr, nullptr,
	                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (deviceInfo == INVALID_HANDLE_VALUE)
	{
		throw std::runtime_error("SetupDiGetClassDevs failed");
	}

	SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
	deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

	DWORD index = 0;
	while (SetupDiEnumDeviceInterfaces(deviceInfo, nullptr, &hidGuid, index, &deviceInterfaceData))
	{
		DWORD requiredSize = 0;
		SetupDiGetDeviceInterfaceDetail(deviceInfo, &deviceInterfaceData, nullptr, 0, &requiredSize, nullptr);

		std::vector<BYTE> detailDataBuffer(requiredSize);
		auto detailData = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA*>(detailDataBuffer.data());
		detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

		if (SetupDiGetDeviceInterfaceDetail(deviceInfo, &deviceInterfaceData, detailData,
		                                    requiredSize, nullptr, nullptr))
		{
			std::wstring path(detailData->DevicePath);

			if (path.find(L"vid_258a") != std::wstring::npos &&
				path.find(L"pid_010c") != std::wstring::npos &&
				path.find(L"col06") != std::wstring::npos)
			{
				auto hDevice = CreateFile(path.c_str(),
				                          GENERIC_READ | GENERIC_WRITE,
				                          FILE_SHARE_READ | FILE_SHARE_WRITE,
				                          nullptr, OPEN_EXISTING,
				                          0, nullptr);

				if (hDevice != INVALID_HANDLE_VALUE)
				{
					SetupDiDestroyDeviceInfoList(deviceInfo);
					return hDevice;
				}
			}
		}
		index++;
	}

	SetupDiDestroyDeviceInfoList(deviceInfo);
	throw std::runtime_error("RGB interface not found");
}

void SendLedCommand(HANDLE hDevice, const std::vector<BYTE>& command)
{
	auto initCmd = HexStringToBytes(INIT_COMMAND_HEX);
	auto finalCmd = HexStringToBytes(FINAL_COMMAND_HEX);

	HidD_SetFeature(hDevice, initCmd.data(), static_cast<ULONG>(initCmd.size()));
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	std::vector<BYTE> buffer(520);
	buffer[0] = 0x06;
	HidD_GetFeature(hDevice, buffer.data(), static_cast<ULONG>(buffer.size()));
	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	HidD_SetFeature(hDevice, (PVOID)command.data(), static_cast<ULONG>(command.size()));
	std::this_thread::sleep_for(std::chrono::milliseconds(150));

	HidD_SetFeature(hDevice, finalCmd.data(), static_cast<ULONG>(finalCmd.size()));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_POWERBROADCAST && wParam == PBT_POWERSETTINGCHANGE)
	{
		auto* setting = reinterpret_cast<POWERBROADCAST_SETTING*>(lParam);
		if (IsEqualGUID(setting->PowerSetting, GUID_CONSOLE_DISPLAY_STATE))
		{
			DWORD state = *(DWORD*)setting->Data;
			if (state == 0 && g_ledOn)
			{
				std::cout << "Display OFF -> Turning LEDs OFF" << std::endl;
				auto cmd = HexStringToBytes(TURN_OFF_COMMAND_HEX);
				SendLedCommand(g_hDevice, cmd);
				g_ledOn = false;
			}
			else if (state == 1 && !g_ledOn)
			{
				std::cout << "Display ON -> Turning LEDs ON" << std::endl;
				auto cmd = HexStringToBytes(TURN_ON_COMMAND_HEX);
				SendLedCommand(g_hDevice, cmd);
				g_ledOn = true;
			}
		}
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool IsRunningAsAdmin()
{
	BOOL isAdmin = FALSE;
	PSID adminGroup;
	SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
	if (AllocateAndInitializeSid(&NtAuthority, 2,
	                             SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
	                             0, 0, 0, 0, 0, 0, &adminGroup))
	{
		CheckTokenMembership(nullptr, adminGroup, &isAdmin);
		FreeSid(adminGroup);
	}
	return isAdmin;
}

void RelaunchAsAdmin()
{
	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);

	SHELLEXECUTEINFOW sei = { sizeof(sei) };
	sei.lpVerb = L"runas";
	sei.lpFile = exePath;
	sei.hwnd = nullptr;
	sei.nShow = SW_SHOWNORMAL;

	if (!ShellExecuteExW(&sei))
	{
		std::cerr << "Failed to elevate to admin" << std::endl;
		exit(1);
	}
	exit(0);
}

void AttachDebugConsole()
{
	AllocConsole();
	FILE* f;
	freopen_s(&f, "CONOUT$", "w", stdout);
	freopen_s(&f, "CONOUT$", "w", stderr);
	freopen_s(&f, "CONIN$", "r", stdin);
	std::cout.clear();
	std::cerr.clear();
	std::cin.clear();
	std::cout << "Debug console attached!" << std::endl;
}

// -------------------- Main --------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
	if (!IsRunningAsAdmin())
	{
		RelaunchAsAdmin();
	}
#ifdef _DEBUG
	AttachDebugConsole();
#endif

	try
	{
		g_hDevice = OpenRGBInterface();
	}
	catch (const std::exception& e)
	{
		std::cerr << e.what() << std::endl;
		return 1;
	}

	WNDCLASS wc = {};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = L"LedControlWindow";
	RegisterClass(&wc);

	auto hwnd = CreateWindowEx(0, wc.lpszClassName, L"LedControlWindow",
	                           0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);

	auto hNotify = RegisterPowerSettingNotification(hwnd,
	                                                &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);

	std::cout << "Listening for display on/off events... (Ctrl+C to exit)" << std::endl;

	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	if (hNotify) UnregisterPowerSettingNotification(hNotify);
	if (g_hDevice) CloseHandle(g_hDevice);

	return 0;
}
