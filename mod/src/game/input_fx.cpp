#include "game/input_fx.hpp"
#include "game/memory.hpp"
#include "game/params.hpp"
#include "game/spawner.hpp"
#include "util/log.hpp"

#define DIRECTINPUT_VERSION 0x0800
#include <Windows.h>
#include <Xinput.h>
#include <dinput.h>
#include <timeapi.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "winmm.lib")

namespace chaos::inputfx {
namespace {

std::atomic<ULONGLONG> g_invert_until{0};
std::atomic<ULONGLONG> g_noroll_until{0};

bool Inverting() { return GetTickCount64() < g_invert_until.load(); }
bool NoRoll() { return GetTickCount64() < g_noroll_until.load(); }

SHORT Neg(SHORT v) { return v == -32768 ? 32767 : static_cast<SHORT>(-v); }

void MutatePad(XINPUT_GAMEPAD& pad) {
    if (Inverting()) {
        pad.sThumbLX = Neg(pad.sThumbLX);
        pad.sThumbLY = Neg(pad.sThumbLY);
    }
    // B / Circle is dodge on the default layout.
    if (NoRoll()) pad.wButtons &= static_cast<WORD>(~XINPUT_GAMEPAD_B);
}

// ---- XInput (controllers, also what Steam Input feeds) ----------------------

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_xi_get = nullptr;
XInputGetStateFn g_xi_get_ex = nullptr;

DWORD WINAPI HookXInputGetState(DWORD user, XINPUT_STATE* state) {
    const DWORD r = g_xi_get(user, state);
    if (r == ERROR_SUCCESS && state) MutatePad(state->Gamepad);
    return r;
}

DWORD WINAPI HookXInputGetStateEx(DWORD user, XINPUT_STATE* state) {
    const DWORD r = g_xi_get_ex(user, state);
    if (r == ERROR_SUCCESS && state) MutatePad(state->Gamepad);
    return r;
}

// ---- keyboard ---------------------------------------------------------------

void SwapBytes(uint8_t* keys, int a, int b) {
    std::swap(keys[a], keys[b]);
}

void MutateKeyboard(uint8_t* keys, int w, int a, int s, int d, int space) {
    if (Inverting()) {
        SwapBytes(keys, w, s);
        SwapBytes(keys, a, d);
    }
    if (NoRoll()) keys[space] = 0;
}

int SwapWasd(int code, int w, int a, int s, int d) {
    if (code == w) return s;
    if (code == s) return w;
    if (code == a) return d;
    if (code == d) return a;
    return code;
}

using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
using GetKeyStateFn = SHORT(WINAPI*)(int);
GetKeyboardStateFn g_get_kb_state = nullptr;
GetKeyStateFn g_get_key_state = nullptr;

BOOL WINAPI HookGetKeyboardState(PBYTE keys) {
    const BOOL ok = g_get_kb_state(keys);
    if (ok && keys) MutateKeyboard(keys, 'W', 'A', 'S', 'D', VK_SPACE);
    return ok;
}

SHORT WINAPI HookGetKeyState(int vk) {
    if (NoRoll() && vk == VK_SPACE) return 0;
    if (Inverting()) vk = SwapWasd(vk, 'W', 'A', 'S', 'D');
    return g_get_key_state(vk);
}

// ---- DirectInput (keyboard and non-XInput pads) -----------------------------

using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA,
                                                     LPDWORD, DWORD);
GetDeviceStateFn g_gds[2] = {};
GetDeviceDataFn g_gdd[2] = {};

std::mutex g_dev_mu;
struct DevInfo {
    DWORD type = 0;
    LONG x_min = 0, x_max = 0, y_min = 0, y_max = 0;
    bool have_range = false;
};
std::unordered_map<void*, DevInfo> g_devs;

DevInfo InfoOf(IDirectInputDevice8W* dev) {
    {
        std::lock_guard lock(g_dev_mu);
        auto it = g_devs.find(dev);
        if (it != g_devs.end()) return it->second;
    }
    DevInfo info;
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    if (SUCCEEDED(dev->GetCapabilities(&caps))) info.type = GET_DIDEVICE_TYPE(caps.dwDevType);
    if (info.type == DI8DEVTYPE_GAMEPAD || info.type == DI8DEVTYPE_JOYSTICK) {
        DIPROPRANGE r{};
        r.diph.dwSize = sizeof(r);
        r.diph.dwHeaderSize = sizeof(r.diph);
        r.diph.dwHow = DIPH_BYOFFSET;
        r.diph.dwObj = DIJOFS_X;
        const bool x = SUCCEEDED(dev->GetProperty(DIPROP_RANGE, &r.diph));
        info.x_min = r.lMin;
        info.x_max = r.lMax;
        r.diph.dwObj = DIJOFS_Y;
        const bool y = SUCCEEDED(dev->GetProperty(DIPROP_RANGE, &r.diph));
        info.y_min = r.lMin;
        info.y_max = r.lMax;
        info.have_range = x && y;
    }
    std::lock_guard lock(g_dev_mu);
    g_devs[dev] = info;
    return info;
}

void MutateDeviceState(IDirectInputDevice8W* dev, DWORD cb, void* data) {
    const DevInfo info = InfoOf(dev);
    if (info.type == DI8DEVTYPE_KEYBOARD && cb >= 256) {
        MutateKeyboard(static_cast<uint8_t*>(data), DIK_W, DIK_A, DIK_S, DIK_D, DIK_SPACE);
        return;
    }
    if (info.have_range && Inverting() && (cb == sizeof(DIJOYSTATE) || cb == sizeof(DIJOYSTATE2))) {
        auto* js = static_cast<DIJOYSTATE*>(data);
        js->lX = info.x_min + info.x_max - js->lX;
        js->lY = info.y_min + info.y_max - js->lY;
    }
}

template <int N>
HRESULT STDMETHODCALLTYPE HookGetDeviceState(IDirectInputDevice8W* self, DWORD cb, LPVOID data) {
    const HRESULT hr = g_gds[N](self, cb, data);
    if (SUCCEEDED(hr) && data && (Inverting() || NoRoll())) MutateDeviceState(self, cb, data);
    return hr;
}

template <int N>
HRESULT STDMETHODCALLTYPE HookGetDeviceData(IDirectInputDevice8W* self, DWORD cb, LPDIDEVICEOBJECTDATA rgdod,
                                            LPDWORD inout, DWORD flags) {
    const HRESULT hr = g_gdd[N](self, cb, rgdod, inout, flags);
    if (FAILED(hr) || !rgdod || !inout || cb < sizeof(DIDEVICEOBJECTDATA)) return hr;
    if (!Inverting() && !NoRoll()) return hr;
    if (InfoOf(self).type != DI8DEVTYPE_KEYBOARD) return hr;
    auto* p = reinterpret_cast<uint8_t*>(rgdod);
    for (DWORD i = 0; i < *inout; ++i) {
        auto* ev = reinterpret_cast<DIDEVICEOBJECTDATA*>(p + static_cast<size_t>(i) * cb);
        if (NoRoll() && ev->dwOfs == DIK_SPACE) ev->dwData = 0;
        if (Inverting()) ev->dwOfs = static_cast<DWORD>(SwapWasd(static_cast<int>(ev->dwOfs), DIK_W, DIK_A, DIK_S, DIK_D));
    }
    return hr;
}

// ---- patching ---------------------------------------------------------------

bool WritePtr(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// Points eldenring.exe's import of `target` (from `dll`) at `hook`.
int PatchImport(const char* dll, void* target, void* hook) {
    if (!target) return 0;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"eldenring.exe"));
    if (!base) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return 0;
    int n = 0;
    for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name;
         ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) != 0) continue;
        for (auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk); thunk->u1.Function;
             ++thunk) {
            if (reinterpret_cast<void*>(thunk->u1.Function) != target) continue;
            if (WritePtr(reinterpret_cast<void**>(&thunk->u1.Function), hook)) ++n;
        }
    }
    return n;
}

void HookDirectInput() {
    HMODULE di_mod = GetModuleHandleW(L"dinput8.dll");
    if (!di_mod) di_mod = LoadLibraryW(L"dinput8.dll");
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto create = di_mod ? reinterpret_cast<CreateFn>(GetProcAddress(di_mod, "DirectInput8Create")) : nullptr;
    if (!create) {
        log::Warn("Input: DirectInput8Create not found");
        return;
    }
    const IID* iids[2] = {&IID_IDirectInput8W, &IID_IDirectInput8A};
    void** done[2] = {};
    for (int k = 0; k < 2; ++k) {
        IDirectInput8W* di = nullptr;
        if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, *iids[k],
                          reinterpret_cast<void**>(&di), nullptr)) || !di) {
            continue;
        }
        IDirectInputDevice8W* dev = nullptr;
        if (SUCCEEDED(di->CreateDevice(GUID_SysKeyboard, &dev, nullptr)) && dev) {
            void** vt = *reinterpret_cast<void***>(dev);
            if (vt != done[0]) {
                done[k] = vt;
                if (k == 0) {
                    g_gds[0] = reinterpret_cast<GetDeviceStateFn>(vt[9]);
                    g_gdd[0] = reinterpret_cast<GetDeviceDataFn>(vt[10]);
                    WritePtr(&vt[9], reinterpret_cast<void*>(&HookGetDeviceState<0>));
                    WritePtr(&vt[10], reinterpret_cast<void*>(&HookGetDeviceData<0>));
                } else {
                    g_gds[1] = reinterpret_cast<GetDeviceStateFn>(vt[9]);
                    g_gdd[1] = reinterpret_cast<GetDeviceDataFn>(vt[10]);
                    WritePtr(&vt[9], reinterpret_cast<void*>(&HookGetDeviceState<1>));
                    WritePtr(&vt[10], reinterpret_cast<void*>(&HookGetDeviceData<1>));
                }
            }
            dev->Release();
        }
        di->Release();
    }
    log::Info(std::string("Input: DirectInput hooks W=") + (done[0] ? "1" : "0") + " A=" + (done[1] ? "1" : "0"));
}

void EnsureHooks() {
    static std::once_flag once;
    std::call_once(once, [] {
        int xi = 0;
        if (HMODULE x = GetModuleHandleW(L"xinput1_4.dll")) {
            g_xi_get = reinterpret_cast<XInputGetStateFn>(GetProcAddress(x, "XInputGetState"));
            g_xi_get_ex = reinterpret_cast<XInputGetStateFn>(GetProcAddress(x, MAKEINTRESOURCEA(100)));
            xi += PatchImport("xinput1_4.dll", reinterpret_cast<void*>(g_xi_get),
                              reinterpret_cast<void*>(&HookXInputGetState));
            xi += PatchImport("xinput1_4.dll", reinterpret_cast<void*>(g_xi_get_ex),
                              reinterpret_cast<void*>(&HookXInputGetStateEx));
        }
        int kb = 0;
        if (HMODULE u = GetModuleHandleW(L"user32.dll")) {
            g_get_kb_state = reinterpret_cast<GetKeyboardStateFn>(GetProcAddress(u, "GetKeyboardState"));
            g_get_key_state = reinterpret_cast<GetKeyStateFn>(GetProcAddress(u, "GetKeyState"));
            kb += PatchImport("user32.dll", reinterpret_cast<void*>(g_get_kb_state),
                              reinterpret_cast<void*>(&HookGetKeyboardState));
            kb += PatchImport("user32.dll", reinterpret_cast<void*>(g_get_key_state),
                              reinterpret_cast<void*>(&HookGetKeyState));
        }
        log::Info("Input: XInput imports hooked=" + std::to_string(xi) + " user32 key imports hooked=" +
                  std::to_string(kb));
        HookDirectInput();
    });
}

// ---- roll block on the action module ------------------------------------------

// CSChrActionRequestModule (ChrIns modules +0x80). ChrActions bits: sp_move 5,
// backstep 16, rolling 17, emergency step 25.
constexpr uintptr_t kModules = 0x190;
constexpr uintptr_t kActionRequest = 0x80;
constexpr uintptr_t kRequests = 0x10;
constexpr uintptr_t kNewPresses = 0x20;
constexpr uintptr_t kQueued = 0x38;
constexpr uintptr_t kDisabled = 0x40;
constexpr uint64_t kRollBits = (1ull << 5) | (1ull << 16) | (1ull << 17) | (1ull << 25);

std::atomic<bool> g_roll_thread{false};

uintptr_t ActionModule(uintptr_t chr) {
    uintptr_t modules = 0, mod = 0;
    if (!chr || !mem::ReadPtr(chr + kModules, modules) || !modules) return 0;
    if (!mem::ReadPtr(modules + kActionRequest, mod) || !mod) return 0;
    return params::RttiNameContains(mod, "ActionRequestModule") ? mod : 0;
}

void RollThread() {
    do {
        timeBeginPeriod(1);
        uintptr_t chr = 0, mod = 0;
        uint64_t had = 0;
        ULONGLONG next_lookup = 0;
        while (NoRoll()) {
        const ULONGLONG now = GetTickCount64();
            if (now >= next_lookup) {
                next_lookup = now + 100;
                const uintptr_t c = spawner::LocalPlayerChr();
                if (c != chr) {
                    chr = c;
                    mod = ActionModule(chr);
                    had = 0;
                    if (mod) mem::Read(mod + kDisabled, had);
                }
            }
            if (mod) {
                uint64_t v = 0;
                if (mem::Read(mod + kDisabled, v)) mem::Write(mod + kDisabled, v | kRollBits);
                for (uintptr_t off : {kRequests, kNewPresses, kQueued}) {
                    if (mem::Read(mod + off, v) && (v & kRollBits)) mem::Write(mod + off, v & ~kRollBits);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (mod && ActionModule(spawner::LocalPlayerChr()) == mod) {
            uint64_t v = 0;
            if (mem::Read(mod + kDisabled, v)) mem::Write(mod + kDisabled, (v & ~kRollBits) | (had & kRollBits));
        }
        timeEndPeriod(1);
        log::Info("No-roll ended");
        g_roll_thread = false;
        // A new request can land between the loop ending and the flag clearing.
    } while (NoRoll() && !g_roll_thread.exchange(true));
}

ULONGLONG Extend(std::atomic<ULONGLONG>& until, int ms) {
    if (ms < 100) ms = 100;
    if (ms > 120000) ms = 120000;
    const ULONGLONG target = GetTickCount64() + static_cast<ULONGLONG>(ms);
    ULONGLONG cur = until.load();
    while (cur < target && !until.compare_exchange_weak(cur, target)) {
    }
    return target;
}

}  // namespace

void BlockRoll(int ms) {
    EnsureHooks();
    Extend(g_noroll_until, ms);
    log::Info("No-roll for " + std::to_string(ms) + " ms");
    if (!g_roll_thread.exchange(true)) std::thread(RollThread).detach();
}

void InvertMovement(int ms) {
    EnsureHooks();
    Extend(g_invert_until, ms);
    log::Info("Inverted movement for " + std::to_string(ms) + " ms");
}

}  // namespace chaos::inputfx
