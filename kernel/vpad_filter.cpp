// iter-466 tooling: the virtual-pad-only filter for automated rolls. Contract: vpad_filter.hpp.
#include "vpad_filter.hpp"

#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
#include <setupapi.h>
#include <cstdio>
#include <cwchar>
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "setupapi.lib")
#endif

namespace sr {
namespace kernel {
namespace vpad {

#ifdef _WIN32
namespace {

std::string narrow(const wchar_t* w) {
  std::string s;
  for (; w && *w; ++w) s += (*w < 128) ? char(*w) : '?';
  return s;
}

// DEVPKEY_Device_InstanceId (devpkey.h), spelled out so no GUID object needs instantiating.
const DEVPROPKEY kInstanceId = {
    {0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};

}  // namespace

bool isVigemPad(const char* sdlPath, uint16_t vid, uint16_t pid, std::string* why) {
  auto fail = [&](const std::string& r) {
    if (why) *why = r;
    return false;
  };
  const bool knownId = (vid == 0x045E && pid == 0x028E) || (vid == 0x054C && pid == 0x05C4);
  if (!sdlPath || std::strncmp(sdlPath, "\\\\?\\", 4) != 0)
    return fail(std::string("no device path (") + (sdlPath ? sdlPath : "null") + ")");
  wchar_t wpath[1024];
  if (!MultiByteToWideChar(CP_UTF8, 0, sdlPath, -1, wpath, 1024)) return fail("path too long");
  wchar_t inst[512] = {};
  DEVPROPTYPE type = 0;
  ULONG size = sizeof(inst);
  if (CM_Get_Device_Interface_PropertyW(wpath, &kInstanceId, &type, reinterpret_cast<PBYTE>(inst), &size, 0) !=
      CR_SUCCESS)
    return fail("device interface not found");
  DEVINST dn = 0;
  if (CM_Locate_DevNodeW(&dn, inst, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
    return fail("device node not found: " + narrow(inst));
  std::string chain;
  bool onBus = false;
  for (int depth = 0; depth < 16; ++depth) {
    wchar_t id[512] = {};
    CM_Get_Device_IDW(dn, id, 512, 0);
    wchar_t svc[128] = {};
    ULONG len = sizeof(svc);
    if (CM_Get_DevNode_Registry_PropertyW(dn, CM_DRP_SERVICE, nullptr, svc, &len, 0) != CR_SUCCESS) svc[0] = 0;
    if (!chain.empty()) chain += " <- ";
    chain += narrow(id);
    if (svc[0]) chain += " [" + narrow(svc) + "]";
    if (_wcsicmp(svc, L"ViGEmBus") == 0) {
      onBus = true;
      break;
    }
    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, dn, 0) != CR_SUCCESS) break;
    dn = parent;
  }
  if (!onBus) return fail("not on the ViGEm bus: " + chain);
  if (!knownId) return fail("on the ViGEm bus but not a ViGEm target id: " + chain);
  if (why) *why = chain;
  return true;
}
int scanForTest(int* accepted) {
  // GUID_DEVINTERFACE_HID {4D1E55B2-F16F-11CF-88CB-001111000030}
  static const GUID kHid = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
  HDEVINFO set = SetupDiGetClassDevsW(&kHid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) return 0;
  int bad = 0, ok = 0, n = 0;
  SP_DEVICE_INTERFACE_DATA ifd{};
  ifd.cbSize = sizeof(ifd);
  for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &kHid, i, &ifd); ++i) {
    alignas(8) unsigned char buf[2048];
    auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf);
    det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof(buf), nullptr, nullptr)) continue;
    char path[1024];
    if (!WideCharToMultiByte(CP_UTF8, 0, det->DevicePath, -1, path, sizeof(path), nullptr, nullptr)) continue;
    std::string why;
    const bool acc = isVigemPad(path, 0x045E, 0x028E, &why);
    const bool onBus = why.find("[ViGEmBus]") != std::string::npos;
    ++n;
    if (acc) ++ok;
    if (acc && !onBus) ++bad;
    std::printf("[vpad-filter] %s %s\n    %s\n", acc ? "ACCEPTED" : "rejected", path, why.c_str());
  }
  SetupDiDestroyDeviceInfoList(set);
  std::printf("[vpad-filter] %d HID interfaces, %d accepted, %d accepted off the ViGEm bus\n", n, ok, bad);
  if (accepted) *accepted = ok;
  return bad;
}
#else
bool isVigemPad(const char*, uint16_t, uint16_t, std::string* why) {
  if (why) *why = "not Windows";
  return false;
}
int scanForTest(int* accepted) {
  if (accepted) *accepted = 0;
  return 0;
}
#endif

}  // namespace vpad
}  // namespace kernel
}  // namespace sr
