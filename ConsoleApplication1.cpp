#include <windows.h>
#include <TlHelp32.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <unordered_set>

#pragma warning(disable: 4996)

// ================== 路径配置 (根据你的实际路径修改) ==================
const std::string DUMPER_PATH = "C:\\Users\\Lenovo\\Desktop\\test1\\cs2-dumper.exe";
const std::string DUMPER_DIR = "C:\\Users\\Lenovo\\Desktop\\test1";
const std::string OFFSETS_FILE = DUMPER_DIR + "\\output\\offsets.json";
const std::string CLIENT_FILE = DUMPER_DIR + "\\output\\client_dll.json";

// ================== 偏移变量 ==================
DWORD64 dwEntityList = 0;
DWORD64 dwLocalPlayerController = 0;
DWORD64 dwViewMatrix = 0;
DWORD64 m_iHealth = 0;
DWORD64 m_iTeamNum = 0;
DWORD64 m_pGameSceneNode = 0;
DWORD64 m_hPlayerPawn = 0;
DWORD64 m_vecAbsOrigin = 0;
DWORD64 m_vOldOrigin = 5284; // 定死的旧坐标偏移

// ================== 数据结构 ==================
struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct Box { int x, y, w, h, hp; bool is_teammate; };

// ================== 全局变量 ==================
HANDLE hProcess = NULL;
DWORD64 clientBase = 0;
HWND gameHwnd = NULL;
int screenW = 0, screenH = 0;
float matrix[16];

std::vector<Box> boxes;
std::mutex boxMutex;

// ================== 工具函数：读取进程内存 ==================
template <typename T>
T Read(DWORD64 addr) {
    T buffer{};
    if (addr > 0x10000) ReadProcessMemory(hProcess, (LPCVOID)addr, &buffer, sizeof(T), nullptr);
    return buffer;
}

// ================== 极简 JSON 解析器 ==================
// 从文件中读取所有文本
std::string ReadFileToString(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// 查找键并返回其后面的十进制数字
DWORD64 ParseJSON(const std::string& content, const std::string& key) {
    size_t pos = content.find("\"" + key + "\"");
    if (pos == std::string::npos) return 0;

    pos = content.find(":", pos);
    if (pos == std::string::npos) return 0;

    // 截取数字部分
    std::string numStr = "";
    for (size_t i = pos + 1; i < content.size(); ++i) {
        if (isdigit(content[i])) {
            numStr += content[i];
        }
        else if (numStr.length() > 0) {
            break;
        }
    }
    if (numStr.empty()) return 0;
    return std::stoull(numStr);
}

// ================== 初始化挂载 ==================
bool Init() {
    std::cout << "正在自动运行 Dumper..." << std::endl;
    // 切换工作目录并运行 Dumper
    SetCurrentDirectoryA(DUMPER_DIR.c_str());
    system("cs2-dumper.exe > nul 2>&1");
    std::cout << "Dumper 运行完毕，正在解析 JSON..." << std::endl;

    // 读取 JSON 文件
    std::string offsetsContent = ReadFileToString(OFFSETS_FILE);
    std::string clientContent = ReadFileToString(CLIENT_FILE);

    if (offsetsContent.empty() || clientContent.empty()) {
        std::cout << "[错误] 找不到 JSON 文件！" << std::endl;
        return false;
    }

    // 解析偏移量 (Dumper 给出的是十进制，直接用)
    dwEntityList = ParseJSON(offsetsContent, "dwEntityList");
    dwLocalPlayerController = ParseJSON(offsetsContent, "dwLocalPlayerController");
    dwViewMatrix = ParseJSON(offsetsContent, "dwViewMatrix");

    m_iHealth = ParseJSON(clientContent, "m_iHealth");
    m_iTeamNum = ParseJSON(clientContent, "m_iTeamNum");
    m_pGameSceneNode = ParseJSON(clientContent, "m_pGameSceneNode");
    m_hPlayerPawn = ParseJSON(clientContent, "m_hPlayerPawn");
    m_vecAbsOrigin = ParseJSON(clientContent, "m_vecAbsOrigin");

    std::cout << "解析结果:" << std::endl;
    std::cout << "EntityList: " << std::hex << dwEntityList << std::endl;
    std::cout << "ViewMatrix: " << std::hex << dwViewMatrix << std::endl;

    if (!dwEntityList || !dwViewMatrix) {
        std::cout << "[警告] 偏移解析失败，可能 Dumper 报错了，数值为 0。" << std::endl;
        return false;
    }

    // 获取游戏进程
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe{ sizeof(pe) };
    if (Process32First(snap, &pe)) {
        do {
            if (std::wstring(pe.szExeFile) == L"cs2.exe") { pid = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    if (!pid) {
        std::cout << "[错误] 没找到 cs2.exe，请启动游戏！" << std::endl;
        return false;
    }

    hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProcess) {
        std::cout << "[错误] OpenProcess 失败，请管理员运行！" << std::endl;
        return false;
    }

    // 获取模块基址
    HANDLE modSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32 me{ sizeof(me) };
    if (Module32First(modSnap, &me)) {
        do {
            if (std::wstring(me.szModule) == L"client.dll") {
                clientBase = (DWORD64)me.modBaseAddr; break;
            }
        } while (Module32Next(modSnap, &me));
    }
    CloseHandle(modSnap);
    if (!clientBase) {
        std::cout << "[错误] 没找到 client.dll" << std::endl;
        return false;
    }

    // 获取窗口
    gameHwnd = FindWindowA(NULL, "Counter-Strike 2");
    if (!gameHwnd) {
        std::cout << "[错误] 找不到游戏窗口，请确认游戏在窗口化模式运行。" << std::endl;
        return false;
    }

    RECT rect;
    GetClientRect(gameHwnd, &rect);
    screenW = rect.right - rect.left;
    screenH = rect.bottom - rect.top;

    std::cout << "[成功] 初始化完毕，开始透视！" << std::endl;
    return true;
}

// ================== 坐标投影 ==================
bool W2S(const Vec3& pos, const float m[16], Vec2& out) {
    float cx = pos.x * m[0] + pos.y * m[1] + pos.z * m[2] + m[3];
    float cy = pos.x * m[4] + pos.y * m[5] + pos.z * m[6] + m[7];
    float cw = pos.x * m[12] + pos.y * m[13] + pos.z * m[14] + m[15];
    if (cw < 0.001f) return false;
    out.x = (screenW / 2.0f) * (cx / cw + 1.0f);
    out.y = (screenH / 2.0f) * (1.0f - cy / cw);
    return (out.x >= 0 && out.x <= screenW && out.y >= 0 && out.y <= screenH);
}

// ================== 核心读取线程 (原版暴力移植) ==================
void ESPThread() {
    while (true) {
        if (!hProcess || !clientBase || dwEntityList == 0) { Sleep(1000); continue; }

        ReadProcessMemory(hProcess, (LPCVOID)(clientBase + dwViewMatrix), matrix, sizeof(matrix), nullptr);

        DWORD64 ges = Read<DWORD64>(clientBase + dwEntityList);
        if (!ges) { Sleep(100); continue; }

        DWORD64 elist = Read<DWORD64>(ges + 0x10);
        if (!elist) { Sleep(100); continue; }

        DWORD64 localCtrl = Read<DWORD64>(clientBase + dwLocalPlayerController);
        if (!localCtrl) { Sleep(100); continue; }

        int myTeam = Read<int>(localCtrl + m_iTeamNum);
        std::vector<Box> localBoxes;
        std::unordered_set<DWORD64> drawn;

        static std::vector<unsigned char> chunkData(0x78 * 512);

        for (int chunkIdx = 0; chunkIdx < 128; ++chunkIdx) {
            for (int chkOff : {0x10, 0x8}) {
                DWORD64 chunk = Read<DWORD64>(elist + 0x8 * chunkIdx + chkOff);
                if (!chunk || chunk < 0x100000) continue;

                SIZE_T bytesRead = 0;
                if (!ReadProcessMemory(hProcess, (LPCVOID)chunk, chunkData.data(), chunkData.size(), &bytesRead)) {
                    continue;
                }

                for (int itemIdx = 0; itemIdx < 512; ++itemIdx) {
                    DWORD64 baseOffset = itemIdx * 0x78;
                    for (int entOff : {0x10, 0x0}) {
                        if (baseOffset + entOff + 8 > bytesRead) continue;

                        DWORD64 entity = *(DWORD64*)(chunkData.data() + baseOffset + entOff);
                        if (!entity || entity < 0x100000) continue;
                        if (drawn.count(entity)) continue;

                        int hp = Read<int>(entity + m_iHealth);
                        int team = Read<int>(entity + m_iTeamNum);

                        if (hp <= 0 || hp > 100 || (team != 2 && team != 3)) continue;

                        Vec3 origin = { 0, 0, 0 };
                        DWORD64 node = Read<DWORD64>(entity + m_pGameSceneNode);
                        if (node) {
                            ReadProcessMemory(hProcess, (LPCVOID)(node + m_vecAbsOrigin), &origin, sizeof(Vec3), nullptr);
                        }
                        if (origin.x == 0 && origin.y == 0) {
                            ReadProcessMemory(hProcess, (LPCVOID)(entity + m_vOldOrigin), &origin, sizeof(Vec3), nullptr);
                        }
                        if (origin.x == 0) continue;

                        Vec3 head = { origin.x, origin.y, origin.z + 72.0f };
                        Vec2 feet2d, head2d;
                        if (W2S(origin, matrix, feet2d) && W2S(head, matrix, head2d)) {
                            int hh = (int)(feet2d.y - head2d.y);
                            if (hh > 4) {
                                Box box;
                                box.w = hh / 2; box.x = (int)head2d.x - box.w / 2;
                                box.y = (int)head2d.y; box.h = hh; box.hp = hp;
                                box.is_teammate = (team == myTeam);
                                localBoxes.push_back(box);
                                drawn.insert(entity);
                            }
                        }
                    }
                }
            }
        }

        boxMutex.lock();
        boxes.swap(localBoxes);
        boxMutex.unlock();

        Sleep(10); // 稍微控制一下帧率
    }
}

// ================== 绘制 ==================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &ps.rcPaint, bgBrush);
        DeleteObject(bgBrush);

        boxMutex.lock();
        for (const auto& box : boxes) {
            COLORREF c = box.is_teammate ? RGB(0, 150, 255) : RGB(0, 255, 0);
            HPEN pen = CreatePen(PS_SOLID, 2, c);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, box.x, box.y, box.x + box.w, box.y + box.h);

            // 血条
            HBRUSH bgHp = CreateSolidBrush(RGB(0, 0, 0));
            HBRUSH fgHp = CreateSolidBrush(RGB(255, 0, 0));
            RECT hpRect = { box.x - 6, box.y, box.x - 2, box.y + box.h };
            FillRect(hdc, &hpRect, bgHp);
            hpRect.top = box.y + (int)(box.h * (1.0f - box.hp / 100.0f));
            FillRect(hdc, &hpRect, fgHp);

            DeleteObject(bgHp); DeleteObject(fgHp);
            SelectObject(hdc, oldPen); SelectObject(hdc, oldBrush);
            DeleteObject(pen);
        }
        boxMutex.unlock();
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int main() {
    SetConsoleTitleA("CS2 C++ 原版逻辑移植");

    if (!Init()) {
        std::cout << "初始化失败。请确认游戏已启动、已进地图，且以管理员身份运行。" << std::endl;
        system("pause");
        return 0;
    }

    // 启动内存读取线程
    std::thread espWorker(ESPThread);
    espWorker.detach();

    // 创建透明窗口
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"OverlayClass";
    RegisterClassExW(&wc);

    POINT pt = { 0, 0 };
    ClientToScreen(gameHwnd, &pt);

    HWND overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"ESP", WS_POPUP,
        pt.x, pt.y, screenW, screenH,
        NULL, NULL, wc.hInstance, NULL
    );
    SetLayeredWindowAttributes(overlay, RGB(0, 0, 0), 0, LWA_COLORKEY);
    ShowWindow(overlay, SW_SHOW);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        InvalidateRect(overlay, NULL, TRUE);
        Sleep(10);
    }
    return 0;
}