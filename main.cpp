// main.cpp

#include <Windows.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <random>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;

// ===============================
// D3D GLOBALS
// ===============================
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// ===============================
// Helper functions for D3D
// ===============================
void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupDevice() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

// ===============================
// HWID Check Function
// ===============================
std::string GetSystemHWID() {
    HKEY hKey;
    char szData[256];
    DWORD dwBufferSize = sizeof(szData);
    
    LONG result = RegOpenKeyExA(
        HKEY_LOCAL_MACHINE, 
        "SOFTWARE\\Microsoft\\Cryptography", 
        0, 
        KEY_READ | KEY_WOW64_64KEY, 
        &hKey
    );

    if (result == ERROR_SUCCESS) {
        result = RegQueryValueExA(
            hKey, 
            "MachineGuid", 
            NULL, 
            NULL, 
            (LPBYTE)szData, 
            &dwBufferSize
        );
        RegCloseKey(hKey);

        if (result == ERROR_SUCCESS) {
            return std::string(szData);
        }
    }
    return "";
}

// ===============================
// EXE and Directories
// ===============================
fs::path GetExeDirectory() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    return fs::path(buffer).parent_path();
}

fs::path GetPresetsDirectory() {
    fs::path dir = GetExeDirectory() / L"presets";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

fs::path GetAdvancedPresetsDirectory() {
    fs::path dir = GetExeDirectory() / L"advanced_presets";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

void SavePreset(const std::string& name, float moveX, float moveY, int intervalMs, bool jitterEnabled, int jitterEveryTicks) {
    fs::path filePath = GetPresetsDirectory() / (name + ".txt");
    std::ofstream out(filePath);
    if (!out.is_open()) return;
    out << moveX << "\n" << moveY << "\n" << intervalMs << "\n" << (jitterEnabled ? 1 : 0) << "\n" << jitterEveryTicks << "\n";
}

bool LoadPreset(const std::string& name, float& moveX, float& moveY, int& intervalMs, bool& jitterEnabled, int& jitterEveryTicks) {
    fs::path filePath = GetPresetsDirectory() / (name + ".txt");
    std::ifstream in(filePath);
    if (!in.is_open()) return false;
    int jitterFlag;
    if (!(in >> moveX >> moveY >> intervalMs >> jitterFlag >> jitterEveryTicks))
        return false;
    jitterEnabled = (jitterFlag != 0);
    return true;
}

// ===============================
// Advanced Step Structure & State
// ===============================
struct AdvancedStep {
    float moveX;
    float moveY;
    int durationMs;
    float offsetRange;
};

static bool advancedEnabled = false;
static std::vector<AdvancedStep> advancedSteps;
static char advancedPresetBuffer[64] = "default";
static float globalOffsetMultiplier = 1.0f;

// Execution state for Advanced Mode
static int advCurrentStep = 0;
static DWORD advStepStartTime = 0;
static DWORD advLastMoveTime = 0;
static bool advWasHolding = false;

void SaveAdvancedPreset(const std::string& name, const std::vector<AdvancedStep>& steps) {
    fs::path filePath = GetAdvancedPresetsDirectory() / (name + ".txt");
    std::ofstream out(filePath);
    if (!out.is_open()) return;
    out << steps.size() << "\n";
    for (const auto& step : steps) {
        out << step.moveX << " " << step.moveY << " " << step.durationMs << " " << step.offsetRange << "\n";
    }
}

bool LoadAdvancedPreset(const std::string& name, std::vector<AdvancedStep>& steps) {
    fs::path filePath = GetAdvancedPresetsDirectory() / (name + ".txt");
    std::ifstream in(filePath);
    if (!in.is_open()) return false;
    size_t count = 0;
    if (!(in >> count)) return false;
    steps.clear();
    for (size_t i = 0; i < count; ++i) {
        AdvancedStep step;
        if (!(in >> step.moveX >> step.moveY >> step.durationMs >> step.offsetRange)) return false;
        steps.push_back(step);
    }
    return true;
}

// ===============================
// Global state for Recording
// ===============================
enum RecordingMode {
    REC_NONE = 0,
    REC_SHORTCUT,   
    REC_SPAM,
    REC_AUTOCLICK
};

static RecordingMode currentRecMode = REC_NONE;
static int selectedMacroIndex = -1;

// ===============================
// Auto Clicker State
// ===============================
static bool autoClickerEnabled = false;
static int autoClickKey = VK_LBUTTON;
static int autoClickIntervalMs = 50;
static DWORD lastAutoClickTime = 0;
static bool isAutoClickKeyDown = false;

// ===============================
// WndProc declaration
// ===============================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ===============================
// Helper: Get human-readable key name
// ===============================
std::string GetKeyNameTextFromVK(int vk_code) {
    if (vk_code == 0) return "None";
    if (vk_code == VK_LBUTTON) return "LMB";
    if (vk_code == VK_RBUTTON) return "RMB";
    if (vk_code == VK_MBUTTON) return "MMB";
    char buf[128];
    UINT scanCode = MapVirtualKey(vk_code, MAPVK_VK_TO_VSC);
    LPARAM lParam = (scanCode << 16);
    if (GetKeyNameTextA(lParam, buf, 128))
        return std::string(buf);
    else
        return "VK_" + std::to_string(vk_code);
}

// ===============================
// Macro structure
// ===============================
struct Macro {
    int shortcutKey;   
    int spamKey;       
    bool holdMode;     
    bool active;       
    int intervalMs;    
    DWORD lastToggleTime; 
    DWORD lastSpamTime;   
    bool wasKeyDown;   
    bool isSpamKeyDown;   
};
static Macro macros[10];

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    // ===============================
    // HWID SECURITY CHECK
    // ===============================
    std::string protectedFolderPath = "C:\\ProtectedApp";
    std::string hwidFilePath = protectedFolderPath + "\\hwid.txt";

    std::string currentHWID = GetSystemHWID();
    if (!currentHWID.empty()) {
        std::error_code ec;
        fs::create_directories(protectedFolderPath, ec);

        std::ifstream inFile(hwidFilePath);
        if (!inFile.is_open()) {
            // First run: save the current HWID
            std::ofstream outFile(hwidFilePath);
            if (outFile.is_open()) {
                outFile << currentHWID;
                outFile.close();
            }
        } else {
            // Subsequent runs: compare stored HWID
            std::string storedHWID;
            std::getline(inFile, storedHWID);
            inFile.close();

            if (!storedHWID.empty() && storedHWID != currentHWID) {
                // Mismatch found, close instantly
                ExitProcess(0);
            }
        }
    }
    // ===============================

    timeBeginPeriod(1);

    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0, 0, hInstance, nullptr, nullptr, nullptr, nullptr, L"MouseMover", nullptr };
    RegisterClassEx(&wc);

    // Calculate center screen position for initial window
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int initialWidth = 600;
    int initialHeight = 760;
    int initialX = (screenWidth - initialWidth) / 2;
    int initialY = (screenHeight - initialHeight) / 2;

    // Create a layered, topmost popup window centered on screen
    HWND hwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_LAYERED,
        wc.lpszClassName, 
        L"Mouse Mover & Macro Tool", 
        WS_POPUP, 
        initialX, initialY, initialWidth, initialHeight, 
        nullptr, nullptr, hInstance, nullptr
    );

    // Set overall window transparency alpha
    SetLayeredWindowAttributes(hwnd, 0, 240, LWA_ALPHA);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };

    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK) {
        return 1;
    }

    CreateRenderTarget();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    float moveX = 0.0f;
    float moveY = 0.0f;
    int intervalMs = 16;
    bool jitterEnabled = false;
    int jitterEveryTicks = 5;
    int tickCount = 0;
    float currentOffsetX = 0.0f;
    float currentOffsetY = 0.0f;
    bool done = false;
    DWORD lastMoveTime = 0;
    char presetBuffer[64] = "default";

    LoadPreset("default", moveX, moveY, intervalMs, jitterEnabled, jitterEveryTicks);
    LoadAdvancedPreset("default", advancedSteps);

    std::mt19937 rng((unsigned)GetTickCount());
    std::uniform_real_distribution<float> jitterDist(-0.5f, 0.5f);

    for (int i = 0; i < 10; i++) {
        macros[i].shortcutKey = 0;
        macros[i].spamKey = 0;
        macros[i].holdMode = false;
        macros[i].active = false;
        macros[i].intervalMs = 50;
        macros[i].lastToggleTime = 0;
        macros[i].lastSpamTime = 0;
        macros[i].wasKeyDown = false;
        macros[i].isSpamKeyDown = false;
    }

    bool isVisible = true;
    bool wasShiftDown = false;

    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        
        if (done) break; 

        DWORD now = GetTickCount();

        // --- RIGHT SHIFT TOGGLE VISIBILITY ---
        bool shiftPressed = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
        if (shiftPressed && !wasShiftDown) {
            isVisible = !isVisible;
            ShowWindow(hwnd, isVisible ? SW_SHOW : SW_HIDE);
        }
        wasShiftDown = shiftPressed;
        
        bool capsOn = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
        bool holdBothMouse = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) && (GetAsyncKeyState(VK_RBUTTON) & 0x8000);

        // --- ADVANCED MODE LOGIC (Overrides Basic) ---
        bool advHoldingCondition = capsOn && holdBothMouse;
        if (advancedEnabled && !advancedSteps.empty() && currentRecMode == REC_NONE) {
            if (advHoldingCondition) {
                if (!advWasHolding) {
                    advCurrentStep = 0;
                    advStepStartTime = now;
                    advLastMoveTime = 0;
                    advWasHolding = true;
                }

                if (advCurrentStep < (int)advancedSteps.size()) {
                    auto& step = advancedSteps[advCurrentStep];
                    int safeDuration = (step.durationMs > 0) ? step.durationMs : 1;
                    DWORD elapsed = now - advStepStartTime;

                    if (elapsed < (DWORD)safeDuration) {
                        if (now - advLastMoveTime >= 1) {
                            float ox = 0.0f, oy = 0.0f;
                            float effectiveRange = step.offsetRange * globalOffsetMultiplier;
                            if (effectiveRange > 0.0f) {
                                std::uniform_real_distribution<float> stepJitter(-effectiveRange, effectiveRange);
                                ox = stepJitter(rng);
                                oy = stepJitter(rng);
                            }
                            INPUT input = {0};
                            input.type = INPUT_MOUSE;
                            input.mi.dx = (LONG)(step.moveX + ox);
                            input.mi.dy = (LONG)(step.moveY + oy);
                            input.mi.dwFlags = MOUSEEVENTF_MOVE;
                            SendInput(1, &input, sizeof(INPUT));
                            advLastMoveTime = now;
                        }
                    } else {
                        advCurrentStep++;
                        advStepStartTime = now;
                    }
                }
            } else {
                advWasHolding = false;
                advCurrentStep = 0;
            }
        } else {
            advWasHolding = false;
            advCurrentStep = 0;
        }

        // --- BASIC MOUSE MOVER LOGIC ---
        if (!advancedEnabled && capsOn && holdBothMouse && (now - lastMoveTime >= (DWORD)intervalMs)) {
            if (jitterEnabled) {
                tickCount++;
                if (tickCount >= jitterEveryTicks) {
                    currentOffsetX = jitterDist(rng);
                    currentOffsetY = jitterDist(rng);
                    tickCount = 0;
                }
            } else {
                currentOffsetX = 0.0f;
                currentOffsetY = 0.0f;
                tickCount = 0;
            }
            INPUT input = {0};
            input.type = INPUT_MOUSE;
            input.mi.dx = (LONG)(moveX + currentOffsetX);
            input.mi.dy = (LONG)(moveY + currentOffsetY);
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &input, sizeof(INPUT));
            lastMoveTime = now;
        }

        // --- AUTO CLICKER LOGIC ---
        bool capsOff = !capsOn;
        if (autoClickerEnabled && autoClickKey != 0 && holdBothMouse && capsOff && currentRecMode == REC_NONE) {
            int interval = (autoClickIntervalMs > 0) ? autoClickIntervalMs : 1;

            if (!isAutoClickKeyDown) {
                if (now - lastAutoClickTime >= (DWORD)interval) {
                    INPUT ki = {0};
                    if (autoClickKey == VK_LBUTTON || autoClickKey == VK_RBUTTON || autoClickKey == VK_MBUTTON) {
                        ki.type = INPUT_MOUSE;
                        if (autoClickKey == VK_LBUTTON) ki.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                        else if (autoClickKey == VK_RBUTTON) ki.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
                        else if (autoClickKey == VK_MBUTTON) ki.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
                    } else {
                        ki.type = INPUT_KEYBOARD;
                        ki.ki.wVk = (WORD)autoClickKey;
                        ki.ki.wScan = (WORD)MapVirtualKey(ki.ki.wVk, MAPVK_VK_TO_VSC);
                    }
                    SendInput(1, &ki, sizeof(INPUT));
                    isAutoClickKeyDown = true;
                    lastAutoClickTime = now;
                }
            } else {
                DWORD holdDuration = (interval < 20) ? (interval / 2 > 0 ? interval / 2 : 1) : 20;
                if (now - lastAutoClickTime >= holdDuration) {
                    INPUT ki = {0};
                    if (autoClickKey == VK_LBUTTON || autoClickKey == VK_RBUTTON || autoClickKey == VK_MBUTTON) {
                        ki.type = INPUT_MOUSE;
                        if (autoClickKey == VK_LBUTTON) ki.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                        else if (autoClickKey == VK_RBUTTON) ki.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
                        else if (autoClickKey == VK_MBUTTON) ki.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
                    } else {
                        ki.type = INPUT_KEYBOARD;
                        ki.ki.wVk = (WORD)autoClickKey;
                        ki.ki.wScan = (WORD)MapVirtualKey(ki.ki.wVk, MAPVK_VK_TO_VSC);
                        ki.ki.dwFlags = KEYEVENTF_KEYUP;
                    }
                    SendInput(1, &ki, sizeof(INPUT));
                    isAutoClickKeyDown = false;
                    lastAutoClickTime = now;
                }
            }
        } else {
            if (isAutoClickKeyDown) {
                INPUT ki = {0};
                if (autoClickKey == VK_LBUTTON || autoClickKey == VK_RBUTTON || autoClickKey == VK_MBUTTON) {
                    ki.type = INPUT_MOUSE;
                    if (autoClickKey == VK_LBUTTON) ki.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                    else if (autoClickKey == VK_RBUTTON) ki.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
                    else if (autoClickKey == VK_MBUTTON) ki.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
                } else {
                    ki.type = INPUT_KEYBOARD;
                    ki.ki.wVk = (WORD)autoClickKey;
                    ki.ki.wScan = (WORD)MapVirtualKey(ki.ki.wVk, MAPVK_VK_TO_VSC);
                    ki.ki.dwFlags = KEYEVENTF_KEYUP;
                }
                SendInput(1, &ki, sizeof(INPUT));
                isAutoClickKeyDown = false;
            }
        }

        // --- SPAM & HOLD-TO-LOOP LOGIC ---
        for (int i = 0; i < 10; i++) {
            if (macros[i].shortcutKey == 0 || macros[i].spamKey == 0 || currentRecMode != REC_NONE) continue;
            
            bool keyDown = (GetAsyncKeyState(macros[i].shortcutKey) & 0x8000) != 0;
            bool keyPressed = keyDown && !macros[i].wasKeyDown; 
            macros[i].wasKeyDown = keyDown;

            int targetKey = macros[i].spamKey;
            if (targetKey <= 0) continue;

            WORD vk = (WORD)targetKey;
            WORD scan = (WORD)MapVirtualKey(vk, MAPVK_VK_TO_VSC);

            bool shouldBeActive = false;

            if (macros[i].holdMode) {
                shouldBeActive = keyDown;
            } else {
                if (keyPressed && (now - macros[i].lastToggleTime >= 250)) {
                    macros[i].active = !macros[i].active;
                    macros[i].lastToggleTime = now;
                    macros[i].lastSpamTime = now; 
                }
                shouldBeActive = macros[i].active;
            }

            if (shouldBeActive) {
                int interval = (macros[i].intervalMs > 0) ? macros[i].intervalMs : 1;

                if (!macros[i].isSpamKeyDown) {
                    if (now - macros[i].lastSpamTime >= (DWORD)interval) {
                        INPUT ki = {0};
                        ki.type = INPUT_KEYBOARD;
                        ki.ki.wVk = vk;
                        ki.ki.wScan = scan;
                        SendInput(1, &ki, sizeof(INPUT));
                        macros[i].isSpamKeyDown = true;
                        macros[i].lastSpamTime = now;
                    }
                } else {
                    DWORD holdDuration = (interval < 20) ? (interval / 2 > 0 ? interval / 2 : 1) : 20;
                    if (now - macros[i].lastSpamTime >= holdDuration) {
                        INPUT ki = {0};
                        ki.type = INPUT_KEYBOARD;
                        ki.ki.wVk = vk;
                        ki.ki.wScan = scan;
                        ki.ki.dwFlags = KEYEVENTF_KEYUP;
                        SendInput(1, &ki, sizeof(INPUT));
                        macros[i].isSpamKeyDown = false;
                        macros[i].lastSpamTime = now;
                    }
                }
            } else {
                if (macros[i].isSpamKeyDown) {
                    INPUT ki = {0};
                    ki.type = INPUT_KEYBOARD;
                    ki.ki.wVk = vk;
                    ki.ki.wScan = scan;
                    ki.ki.dwFlags = KEYEVENTF_KEYUP;
                    SendInput(1, &ki, sizeof(INPUT));
                    macros[i].isSpamKeyDown = false;
                }
            }
        }

        Sleep(1);

        if (!isVisible) continue;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Non-draggable, non-resizable overlay window centered on screen
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.12f, 0.82f));
        ImGui::Begin("Mouse Mover & Macro Tool", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        
        ImGui::Text("Caps Lock: %s", capsOn ? "ON (active)" : "OFF (disabled)");
        ImGui::Separator();

        if (ImGui::BeginTabBar("MainTabBar")) {
            // ================= BASIC TAB =================
            if (ImGui::BeginTabItem("Basic")) {
                ImGui::Text("Hold BOTH mouse buttons to move");
                ImGui::Separator();
                
                ImGui::InputFloat("Y  (+down  / -up)", &moveY, 1.0f, 5.0f, "%.1f");
                ImGui::InputFloat("X  (+right / -left)", &moveX, 1.0f, 5.0f, "%.1f");
                ImGui::SliderInt("Interval (ms)", &intervalMs, 1, 100);
                
                ImGui::Separator();
                ImGui::Checkbox("Jitter Offset", &jitterEnabled);
                if (jitterEnabled) {
                    ImGui::SliderInt("Jitter Every N Ticks", &jitterEveryTicks, 1, 20);
                    ImGui::Text("Current offset: (%.2f, %.2f)", currentOffsetX, currentOffsetY);
                }

                ImGui::Separator();
                ImGui::Text("Presets");
                ImGui::InputText("Name", presetBuffer, IM_ARRAYSIZE(presetBuffer));
                if (ImGui::Button("Save Preset")) {
                    SavePreset(presetBuffer, moveX, moveY, intervalMs, jitterEnabled, jitterEveryTicks);
                }
                ImGui::SameLine();
                if (ImGui::Button("Load Preset")) {
                    LoadPreset(presetBuffer, moveX, moveY, intervalMs, jitterEnabled, jitterEveryTicks);
                }

                // --- MACROS UI ---
                ImGui::Separator();
                ImGui::Text("Macros Configuration");
                for (int i = 0; i < 5; i++) {
                    ImGui::PushID(i);
                    
                    std::string shortcutName = GetKeyNameTextFromVK(macros[i].shortcutKey);
                    std::string spamName = GetKeyNameTextFromVK(macros[i].spamKey);
                    if (macros[i].active && !macros[i].holdMode) shortcutName += " [ON]";

                    ImGui::Text("Sc:");
                    ImGui::SameLine();
                    if (currentRecMode == REC_SHORTCUT && selectedMacroIndex == i) {
                        if (ImGui::Button("Press##sc", ImVec2(65, 0))) { currentRecMode = REC_NONE; }
                    } else {
                        std::string btnLabel = shortcutName + "##sc_btn";
                        if (ImGui::Button(btnLabel.c_str(), ImVec2(65, 0))) {
                            currentRecMode = REC_SHORTCUT;
                            selectedMacroIndex = i;
                        }
                    }

                    ImGui::SameLine();
                    ImGui::Text("Spam:");
                    ImGui::SameLine();
                    if (currentRecMode == REC_SPAM && selectedMacroIndex == i) {
                        if (ImGui::Button("Press##spam", ImVec2(65, 0))) { currentRecMode = REC_NONE; }
                    } else {
                        std::string btnLabel2 = spamName + "##spam_btn";
                        if (ImGui::Button(btnLabel2.c_str(), ImVec2(65, 0))) {
                            currentRecMode = REC_SPAM;
                            selectedMacroIndex = i;
                        }
                    }

                    ImGui::SameLine();
                    ImGui::Checkbox("Hold", &macros[i].holdMode);
                    
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(45);
                    ImGui::InputInt("##ms", &macros[i].intervalMs, 0, 0);
                    ImGui::SameLine();
                    ImGui::Text("ms");

                    ImGui::SameLine();
                    if (ImGui::Button("X")) {
                        macros[i].shortcutKey = 0;
                        macros[i].spamKey = 0;
                        macros[i].active = false;
                        macros[i].wasKeyDown = false;
                        macros[i].isSpamKeyDown = false;
                    }

                    ImGui::PopID();
                }

                // --- AUTO CLICKER UI ---
                ImGui::Separator();
                ImGui::Text("Auto Clicker Configuration");
                ImGui::Checkbox("Enable Auto Clicker", &autoClickerEnabled);
                ImGui::TextWrapped("Trigger: Hold LMB + RMB simultaneously & Caps Lock OFF");
                
                std::string autoClickKeyName = GetKeyNameTextFromVK(autoClickKey);
                ImGui::Text("Click Key:");
                ImGui::SameLine();
                if (currentRecMode == REC_AUTOCLICK) {
                    if (ImGui::Button("Press##autoclick", ImVec2(75, 0))) { currentRecMode = REC_NONE; }
                } else {
                    std::string btnLabel = autoClickKeyName + "##autoclick_btn";
                    if (ImGui::Button(btnLabel.c_str(), ImVec2(75, 0))) {
                        currentRecMode = REC_AUTOCLICK;
                    }
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(45);
                ImGui::InputInt("##ac_ms", &autoClickIntervalMs, 0, 0);
                ImGui::SameLine();
                ImGui::Text("ms");

                ImGui::EndTabItem();
            }

            // ================= ADVANCED TAB =================
            if (ImGui::BeginTabItem("Advanced")) {
                ImGui::Checkbox("Enable Advanced Mode (Overrides Basic)", &advancedEnabled);
                ImGui::TextWrapped("Trigger: Caps Lock ON + Hold LMB + RMB. Runs sequence once per hold.");
                ImGui::Separator();

                ImGui::SliderFloat("Global Offset Multiplier", &globalOffsetMultiplier, 0.0f, 5.0f, "%.1f");

                if (ImGui::Button("Add Step")) {
                    advancedSteps.push_back({1.0f, 1.0f, 100, 0.5f});
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear All Steps")) {
                    advancedSteps.clear();
                }

                ImGui::Separator();
                ImGui::Text("Advanced Presets");
                ImGui::InputText("Adv Preset Name", advancedPresetBuffer, IM_ARRAYSIZE(advancedPresetBuffer));
                if (ImGui::Button("Save Advanced Preset")) {
                    SaveAdvancedPreset(advancedPresetBuffer, advancedSteps);
                }
                ImGui::SameLine();
                if (ImGui::Button("Load Advanced Preset")) {
                    LoadAdvancedPreset(advancedPresetBuffer, advancedSteps);
                }

                ImGui::Separator();
                ImGui::BeginChild("AdvancedStepsChild", ImVec2(0, 300), true);
                
                int deleteIndex = -1;
                int duplicateIndex = -1;
                for (int i = 0; i < (int)advancedSteps.size(); i++) {
                    ImGui::PushID(i);
                    ImGui::Text("#%d", i + 1);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60);
                    ImGui::InputFloat("Y", &advancedSteps[i].moveY, 0.0f, 0.0f, "%.1f");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60);
                    ImGui::InputFloat("X", &advancedSteps[i].moveX, 0.0f, 0.0f, "%.1f");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(70);
                    ImGui::InputInt("ms", &advancedSteps[i].durationMs, 0, 0);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60);
                    ImGui::InputFloat("Offset", &advancedSteps[i].offsetRange, 0.0f, 0.0f, "%.1f");
                    
                    ImGui::SameLine();
                    if (ImGui::Button("Duplicate")) {
                        duplicateIndex = i;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Delete")) {
                        deleteIndex = i;
                    }
                    ImGui::PopID();
                }
                
                if (duplicateIndex != -1) {
                    advancedSteps.insert(advancedSteps.begin() + duplicateIndex + 1, advancedSteps[duplicateIndex]);
                }
                if (deleteIndex != -1) {
                    advancedSteps.erase(advancedSteps.begin() + deleteIndex);
                }

                ImGui::EndChild();

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        // Capture ImGui window size to dynamically scale the background and window to the menu content
        ImVec2 winSize = ImGui::GetWindowSize();
        ImGui::End();
        ImGui::PopStyleColor(); // Pop WindowBg style

        RECT winRect;
        GetWindowRect(hwnd, &winRect);
        int curW = winRect.right - winRect.left;
        int curH = winRect.bottom - winRect.top;

        if (winSize.x > 50 && winSize.y > 50 && (abs(winSize.x - (float)curW) > 2.0f || abs(winSize.y - (float)curH) > 2.0f)) {
            int newW = (int)winSize.x;
            int newH = (int)winSize.y;
            int currentScreenWidth = GetSystemMetrics(SM_CXSCREEN);
            int currentScreenHeight = GetSystemMetrics(SM_CYSCREEN);
            int newX = (currentScreenWidth - newW) / 2;
            int newY = (currentScreenHeight - newH) / 2;
            SetWindowPos(hwnd, HWND_TOPMOST, newX, newY, newW, newH, SWP_NOACTIVATE);
        }

        ImGui::Render();
        const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    timeEndPeriod(1);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, hInstance);
    return 0;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_KEYDOWN:
        {
            if (currentRecMode != REC_NONE)
            {
                int vk = (int)wParam;
                if (currentRecMode == REC_AUTOCLICK)
                {
                    autoClickKey = vk;
                    currentRecMode = REC_NONE;
                    return 0;
                }
                else if (selectedMacroIndex >= 0)
                {
                    if (currentRecMode == REC_SHORTCUT) {
                        macros[selectedMacroIndex].shortcutKey = vk;
                    } else if (currentRecMode == REC_SPAM) {
                        macros[selectedMacroIndex].spamKey = vk;
                    }
                    currentRecMode = REC_NONE;
                    selectedMacroIndex = -1;
                    return 0;
                }
            }
            break;
        }
        case WM_LBUTTONDOWN:
        {
            if (currentRecMode != REC_NONE)
            {
                if (currentRecMode == REC_AUTOCLICK)
                {
                    autoClickKey = VK_LBUTTON;
                    currentRecMode = REC_NONE;
                    return 0;
                }
                else if (currentRecMode == REC_SHORTCUT && selectedMacroIndex >= 0)
                {
                    macros[selectedMacroIndex].shortcutKey = VK_LBUTTON;
                    currentRecMode = REC_NONE;
                    selectedMacroIndex = -1;
                    return 0;
                }
            }
            break;
        }
        case WM_RBUTTONDOWN:
        {
            if (currentRecMode != REC_NONE)
            {
                if (currentRecMode == REC_AUTOCLICK)
                {
                    autoClickKey = VK_RBUTTON;
                    currentRecMode = REC_NONE;
                    return 0;
                }
                else if (currentRecMode == REC_SHORTCUT && selectedMacroIndex >= 0)
                {
                    macros[selectedMacroIndex].shortcutKey = VK_RBUTTON;
                    currentRecMode = REC_NONE;
                    selectedMacroIndex = -1;
                    return 0;
                }
            }
            break;
        }
        case WM_SIZE:
        {
            if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
            {
                if (g_pSwapChain)
                {
                    if (g_mainRenderTargetView) {
                        g_mainRenderTargetView->Release();
                        g_mainRenderTargetView = nullptr;
                    }

                    g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                    CreateRenderTarget();
                }
            }
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProc(hWnd, msg, wParam, lParam);
    }
}