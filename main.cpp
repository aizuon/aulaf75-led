// AULA F75 Keyboard LED Controller
//
// Turns off keyboard LEDs when the display goes to sleep, and restores them
// when the display wakes up. Uses the HID feature report protocol reverse
// engineered from the AULA F75 keyboard driver (CDevG5KB class).
//
// Protocol (HID Feature Reports, 520 bytes / 0x208):
//   Byte  0:     Report ID (0x06 for firmware v24)
//   Byte  1:     Command (0x04=SetLED, 0x84=GetLED, 0x0A=SetRGBTab)
//   Bytes 2-3:   Sub-command (typically 0x0000)
//   Byte  4:     Total packet count
//   Byte  5:     Packet index (0-based)
//   Bytes 6-7:   Chunk data size (little-endian uint16)
//   Bytes 8-519: Payload data (up to 512 bytes)
//
// LED Profile (128-byte payload via cmd 0x04 / 0x84):
//   Byte 10:     Brightness level (0x00=off, 0x01..0x0B)
//   Bytes 126-127: 0x5A 0xA5 validity signature

#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <initguid.h>
#include <shellapi.h>
#include <iostream>
#include <vector>
#include <cstring>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

// AULA F75 USB identifiers (from CDevG5KB::Init VID/PID matching)
constexpr USHORT AULA_VENDOR_ID = 0x258A;
constexpr USHORT AULA_PRODUCT_ID = 0x010C;

// HID usage page / usage for the LED control interface
// CDevG5KB::Init matches UsagePage=0xFF00 + Usage=1 (firmware v24)
// or UsagePage=0xFF02 + Usage=2 (firmware v26)
constexpr USAGE HID_USAGE_PAGE_LED_FW24 = 0xFF00;
constexpr USAGE HID_USAGE_LED_FW24 = 0x0001;
constexpr USAGE HID_USAGE_PAGE_LED_FW26 = 0xFF02;
constexpr USAGE HID_USAGE_LED_FW26 = 0x0002;

// HID protocol constants derived from CDevG5KB::AccessData reverse engineering
// Report ID is firmware-dependent: AccessData selects 6 or 9 based on fw version
constexpr BYTE HID_REPORT_ID_FW24 = 0x06;
constexpr BYTE HID_REPORT_ID_FW26 = 0x09;
constexpr BYTE CMD_SET_LED = 0x04;   // Write LED profile config
constexpr BYTE CMD_GET_LED = 0x84;   // Read LED profile config (0x04 | 0x80)
constexpr BYTE CMD_SET_RGB_TAB = 0x0A; // Write RGB color table (unused here, documented for protocol completeness)
constexpr ULONG HID_REPORT_SIZE = 520; // 0x208 bytes per feature report
constexpr size_t MAX_PAYLOAD_SIZE = 512;
constexpr size_t LED_PROFILE_SIZE = 128;
constexpr size_t LED_BRIGHTNESS_OFFSET = 10; // Brightness byte within profile payload
constexpr BYTE LED_PROFILE_SIG_LO = 0x5A;   // Validity signature low byte
constexpr BYTE LED_PROFILE_SIG_HI = 0xA5;   // Validity signature high byte
constexpr size_t LED_PROFILE_SIG_OFFSET = 126;

// Retry / timing constants from CDevG5KB::SendCommand / GetCommand
constexpr int MAX_RETRIES = 3;
constexpr DWORD RETRY_DELAY_MS = 200;
constexpr DWORD INTER_CMD_DELAY_MS = 20;

// Profile read retry constants from CDevG5KB::ApplySetting signature validation
constexpr int MAX_PROFILE_READ_RETRIES = 5;
constexpr DWORD PROFILE_READ_RETRY_DELAY_MS = 100;

// Delay before SetLED write when preceded by a GetLED read
// (from CDevG5KB::ApplySetting: Sleep(0x3C) between GetLED and SetLED)
constexpr DWORD PRE_WRITE_DELAY_MS = 60;

// HID packet layout matching CDevG5KB::AccessData protocol
#pragma pack(push, 1)
struct HidPacket
{
	BYTE reportId;
	BYTE command;
	BYTE subCommandLow;
	BYTE subCommandHigh;
	BYTE totalPackets;
	BYTE packetIndex;
	BYTE chunkSizeLow;
	BYTE chunkSizeHigh;
	BYTE payload[MAX_PAYLOAD_SIZE];
};
#pragma pack(pop)
static_assert(sizeof(HidPacket) == HID_REPORT_SIZE, "HidPacket must be exactly 520 bytes");

// Global state
HANDLE g_hDevice = INVALID_HANDLE_VALUE;
BYTE g_reportId = HID_REPORT_ID_FW24; // Set during device identification
bool g_ledOn = true;
BYTE g_savedBrightness = 0;

// ---------------------------------------------------------------------------
// Protocol helpers
// ---------------------------------------------------------------------------

void BuildPacket(HidPacket& packet, BYTE command, const BYTE* data, UINT16 dataSize)
{
	memset(&packet, 0, sizeof(packet));
	packet.reportId = g_reportId;
	packet.command = command;
	packet.totalPackets = 1;
	packet.packetIndex = 0;
	packet.chunkSizeLow = static_cast<BYTE>(dataSize & 0xFF);
	packet.chunkSizeHigh = static_cast<BYTE>((dataSize >> 8) & 0xFF);
	if (data && dataSize > 0)
		memcpy(packet.payload, data, dataSize);
}

bool SendFeatureReport(HANDLE hDevice, void* data, ULONG size)
{
	for (int attempt = 0; attempt < MAX_RETRIES; ++attempt)
	{
		if (HidD_SetFeature(hDevice, data, size))
			return true;
		Sleep(RETRY_DELAY_MS);
	}
	std::cerr << "SendFeatureReport failed, err=" << GetLastError() << std::endl;
	return false;
}

bool GetFeatureReport(HANDLE hDevice, void* data, ULONG size)
{
	for (int attempt = 0; attempt < MAX_RETRIES; ++attempt)
	{
		if (HidD_GetFeature(hDevice, data, size))
			return true;
		Sleep(RETRY_DELAY_MS);
	}
	std::cerr << "GetFeatureReport failed, err=" << GetLastError() << std::endl;
	return false;
}

// ---------------------------------------------------------------------------
// LED profile read / write (mirrors CDevG5KB::GetLED / SetLED)
// ---------------------------------------------------------------------------

bool ReadLedProfile(HANDLE hDevice, BYTE profileOut[LED_PROFILE_SIZE])
{
	// CDevG5KB::ApplySetting retries up to 5 times with 100ms delay
	// if the 0x5AA5 signature validation fails after a successful read.
	for (int retry = 0; retry < MAX_PROFILE_READ_RETRIES; ++retry)
	{
		// Step 1: Send GetLED request (cmd 0x84)
		HidPacket request;
		BuildPacket(request, CMD_GET_LED, nullptr, LED_PROFILE_SIZE);
		if (!SendFeatureReport(hDevice, &request, HID_REPORT_SIZE))
			return false;

		Sleep(INTER_CMD_DELAY_MS);

		// Step 2: Read back the response
		HidPacket response;
		memset(&response, 0, sizeof(response));
		response.reportId = g_reportId;
		if (!GetFeatureReport(hDevice, &response, HID_REPORT_SIZE))
			return false;

		// Step 3: Validate the 0x5AA5 signature at end of profile data
		if (response.payload[LED_PROFILE_SIG_OFFSET] == LED_PROFILE_SIG_LO &&
			response.payload[LED_PROFILE_SIG_OFFSET + 1] == LED_PROFILE_SIG_HI)
		{
			memcpy(profileOut, response.payload, LED_PROFILE_SIZE);
			return true;
		}

		std::cerr << "Invalid LED profile signature (attempt " << (retry + 1)
			<< "/" << MAX_PROFILE_READ_RETRIES << ")" << std::endl;
		Sleep(PROFILE_READ_RETRY_DELAY_MS);
	}

	std::cerr << "Failed to read valid LED profile after "
		<< MAX_PROFILE_READ_RETRIES << " attempts" << std::endl;
	return false;
}

bool WriteLedProfile(HANDLE hDevice, const BYTE profileData[LED_PROFILE_SIZE])
{
	// CDevG5KB::ApplySetting sleeps 60ms between GetLED and SetLED to let
	// the keyboard settle after a read before accepting a write.
	Sleep(PRE_WRITE_DELAY_MS);

	HidPacket packet;
	BuildPacket(packet, CMD_SET_LED, profileData, LED_PROFILE_SIZE);
	if (!SendFeatureReport(hDevice, &packet, HID_REPORT_SIZE))
		return false;

	Sleep(INTER_CMD_DELAY_MS);
	return true;
}

// ---------------------------------------------------------------------------
// High-level LED control
// ---------------------------------------------------------------------------

bool CaptureCurrentProfile(HANDLE hDevice)
{
	BYTE profile[LED_PROFILE_SIZE];
	if (!ReadLedProfile(hDevice, profile))
		return false;

	g_savedBrightness = profile[LED_BRIGHTNESS_OFFSET];

	std::cout << "Captured LED profile (brightness=" << static_cast<int>(g_savedBrightness) << ")"
		<< std::endl;
	return true;
}

bool SetLedBrightness(HANDLE hDevice, BYTE brightness)
{
	// Read the current profile so we preserve all other settings
	BYTE profile[LED_PROFILE_SIZE];
	if (!ReadLedProfile(hDevice, profile))
		return false;

	profile[LED_BRIGHTNESS_OFFSET] = brightness;

	return WriteLedProfile(hDevice, profile);
}

// Turns off LEDs while atomically capturing the current brightness so it can
// be restored later.  This avoids relying on a stale g_savedBrightness from
// startup in case the user changed brightness through the AULA software.
bool TurnOffLeds(HANDLE hDevice)
{
	BYTE profile[LED_PROFILE_SIZE];
	if (!ReadLedProfile(hDevice, profile))
		return false;

	g_savedBrightness = profile[LED_BRIGHTNESS_OFFSET];
	profile[LED_BRIGHTNESS_OFFSET] = 0;

	return WriteLedProfile(hDevice, profile);
}

// ---------------------------------------------------------------------------
// HID device enumeration (mirrors CDevG5KB::Init device matching logic)
// ---------------------------------------------------------------------------

// Identifies whether a HID device is the AULA F75 LED control interface by
// validating VID/PID, usage page/usage, and feature report size exactly as
// CDevG5KB::Init does.  Returns the firmware-appropriate HID report ID
// (0x06 for fw24, 0x09 for fw26) on match, or 0 if the device doesn't match.
BYTE IdentifyAulaF75LedInterface(HANDLE hDevice)
{
	HIDD_ATTRIBUTES attrs = {};
	attrs.Size = sizeof(attrs);
	if (!HidD_GetAttributes(hDevice, &attrs))
		return 0;

	if (attrs.VendorID != AULA_VENDOR_ID || attrs.ProductID != AULA_PRODUCT_ID)
		return 0;

	PHIDP_PREPARSED_DATA preparsedData = nullptr;
	if (!HidD_GetPreparsedData(hDevice, &preparsedData))
		return 0;

	HIDP_CAPS caps = {};
	BYTE reportId = 0;
	if (HidP_GetCaps(preparsedData, &caps) == HIDP_STATUS_SUCCESS)
	{
		// Feature report must be 520 bytes (0x208) for the LED protocol
		if (caps.FeatureReportByteLength == HID_REPORT_SIZE)
		{
			// CDevG5KB::AccessData selects report ID based on firmware version:
			//   fw24 (UsagePage=0xFF00, Usage=1) -> Report ID 0x06
			//   fw26 (UsagePage=0xFF02, Usage=2) -> Report ID 0x09
			if (caps.UsagePage == HID_USAGE_PAGE_LED_FW24 && caps.Usage == HID_USAGE_LED_FW24)
				reportId = HID_REPORT_ID_FW24;
			else if (caps.UsagePage == HID_USAGE_PAGE_LED_FW26 && caps.Usage == HID_USAGE_LED_FW26)
				reportId = HID_REPORT_ID_FW26;
		}
	}

	HidD_FreePreparsedData(preparsedData);
	return reportId;
}

HANDLE OpenKeyboardHidInterface()
{
	GUID hidGuid;
	HidD_GetHidGuid(&hidGuid);

	HDEVINFO deviceInfo = SetupDiGetClassDevs(
		&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (deviceInfo == INVALID_HANDLE_VALUE)
		return INVALID_HANDLE_VALUE;

	SP_DEVICE_INTERFACE_DATA interfaceData;
	interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

	for (DWORD index = 0;
		SetupDiEnumDeviceInterfaces(deviceInfo, nullptr, &hidGuid, index, &interfaceData);
		++index)
	{
		DWORD requiredSize = 0;
		SetupDiGetDeviceInterfaceDetail(deviceInfo, &interfaceData, nullptr, 0, &requiredSize, nullptr);

		std::vector<BYTE> detailBuffer(requiredSize);
		auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA*>(detailBuffer.data());
		detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

		if (!SetupDiGetDeviceInterfaceDetail(
			deviceInfo, &interfaceData, detail, requiredSize, nullptr, nullptr))
			continue;

		HANDLE hDevice = CreateFileW(
			detail->DevicePath,
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr, OPEN_EXISTING, 0, nullptr);

		if (hDevice == INVALID_HANDLE_VALUE)
			continue;

		BYTE reportId = IdentifyAulaF75LedInterface(hDevice);
		if (reportId != 0)
		{
			g_reportId = reportId;
			SetupDiDestroyDeviceInfoList(deviceInfo);
			return hDevice;
		}

		CloseHandle(hDevice);
	}

	SetupDiDestroyDeviceInfoList(deviceInfo);
	return INVALID_HANDLE_VALUE;
}

// Attempt to re-open the keyboard after a communication failure (e.g. the
// keyboard was briefly unplugged or the USB bus was reset during sleep).
bool TryReopenDevice()
{
	if (g_hDevice != INVALID_HANDLE_VALUE)
	{
		CloseHandle(g_hDevice);
		g_hDevice = INVALID_HANDLE_VALUE;
	}

	g_hDevice = OpenKeyboardHidInterface();
	if (g_hDevice != INVALID_HANDLE_VALUE)
	{
		std::cout << "Device reconnected" << std::endl;
		return true;
	}

	std::cerr << "Device reconnect failed" << std::endl;
	return false;
}

// ---------------------------------------------------------------------------
// Power event handling
// ---------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_POWERBROADCAST && wParam == PBT_POWERSETTINGCHANGE)
	{
		auto* setting = reinterpret_cast<POWERBROADCAST_SETTING*>(lParam);
		if (IsEqualGUID(setting->PowerSetting, GUID_CONSOLE_DISPLAY_STATE))
		{
			DWORD displayState = *reinterpret_cast<DWORD*>(setting->Data);

			if (displayState == 0 && g_ledOn) // Display OFF
			{
				std::cout << "Display OFF -> Turning LEDs OFF" << std::endl;
				bool ok = TurnOffLeds(g_hDevice);
				if (!ok && TryReopenDevice())
					ok = TurnOffLeds(g_hDevice);
				if (ok)
					g_ledOn = false;
			}
			else if (displayState == 1 && !g_ledOn) // Display ON
			{
				std::cout << "Display ON -> Restoring LEDs (brightness="
					<< static_cast<int>(g_savedBrightness) << ")" << std::endl;
				bool ok = SetLedBrightness(g_hDevice, g_savedBrightness);
				if (!ok && TryReopenDevice())
					ok = SetLedBrightness(g_hDevice, g_savedBrightness);
				if (ok)
					g_ledOn = true;
			}
		}
	}
	else if (msg == WM_DESTROY)
	{
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Admin elevation
// ---------------------------------------------------------------------------

bool IsRunningAsAdmin()
{
	BOOL isAdmin = FALSE;
	PSID adminGroup = nullptr;
	SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
	if (AllocateAndInitializeSid(&ntAuthority, 2,
		SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
		0, 0, 0, 0, 0, 0, &adminGroup))
	{
		CheckTokenMembership(nullptr, adminGroup, &isAdmin);
		FreeSid(adminGroup);
	}
	return isAdmin != FALSE;
}

void RelaunchAsAdmin()
{
	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);

	SHELLEXECUTEINFOW sei = { sizeof(sei) };
	sei.lpVerb = L"runas";
	sei.lpFile = exePath;
	sei.nShow = SW_SHOWNORMAL;

	if (!ShellExecuteExW(&sei))
	{
		std::cerr << "Failed to elevate to admin" << std::endl;
		ExitProcess(1);
	}
	ExitProcess(0);
}

// ---------------------------------------------------------------------------
// Debug console (Debug builds only)
// ---------------------------------------------------------------------------

void AttachDebugConsole()
{
	AllocConsole();
	FILE* f = nullptr;
	freopen_s(&f, "CONOUT$", "w", stdout);
	freopen_s(&f, "CONOUT$", "w", stderr);
	freopen_s(&f, "CONIN$", "r", stdin);
	std::cout.clear();
	std::cerr.clear();
	std::cin.clear();
	std::cout << "Debug console attached" << std::endl;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
	if (!IsRunningAsAdmin())
		RelaunchAsAdmin();

#ifdef _DEBUG
	AttachDebugConsole();
#endif

	// Open the AULA F75 HID LED control interface
	g_hDevice = OpenKeyboardHidInterface();
	if (g_hDevice == INVALID_HANDLE_VALUE)
	{
		std::cerr << "Failed to open keyboard HID interface" << std::endl;
		return 1;
	}

	std::cout << "Keyboard found (firmware "
		<< (g_reportId == HID_REPORT_ID_FW24 ? "v24" : "v26") << ")" << std::endl;

	// Read and cache the current LED profile so we can restore it later
	if (!CaptureCurrentProfile(g_hDevice))
	{
		std::cerr << "Failed to read initial LED profile" << std::endl;
		CloseHandle(g_hDevice);
		return 1;
	}

	// Create a message-only window for power setting notifications
	WNDCLASSW wc = {};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = L"AulaF75LedControl";
	RegisterClassW(&wc);

	HWND hwnd = CreateWindowExW(
		0, wc.lpszClassName, L"AulaF75LedControl",
		0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);

	HPOWERNOTIFY hNotify = RegisterPowerSettingNotification(
		hwnd, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);

	std::cout << "Listening for display power events..." << std::endl;

	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	if (hNotify)
		UnregisterPowerSettingNotification(hNotify);
	if (g_hDevice != INVALID_HANDLE_VALUE)
		CloseHandle(g_hDevice);

	return 0;
}
