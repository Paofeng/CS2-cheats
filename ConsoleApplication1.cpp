#include <windows.h>
#include <TlHelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <thread>
#include <unordered_set>

// ================== 路径 ==================
const char* DUMPER_DIR = "C:\\Users\\Lenovo\\Desktop\\test1";
const char* OFFSETS_FILE = "C:\\Users\\Lenovo\\Desktop\\test1\\output\\offsets.json";
const char* CLIENT_FILE = "C:\\Users\\Lenovo\\Desktop\\test1\\output\\client_dll.json";

// ================== 偏移量 ==================
DWORD64 dwEntityList = 0;
DWORD64 dwLocalPlayerController = 0;
DWORD64 dwViewMatrix = 0;
DWORD64 m_iHealth = 0;
DWORD64 m_iTeamNum = 0;
DWORD64 m_pGameSceneNode = 0;
DWORD64 m_vecAbsOrigin = 0;
DWORD64 m_vOldOrigin = 0;
DWORD64 m_hPlayerPawn = 0;

// ================== 数据结构 ==================
struct Vec3 { float x, y, z; };
struct Box { int x, y, w, h, hp; bool is_team; };

// ================== 全局 ==================
HANDLE  hProcess = NULL;
DWORD64 clientBase = 0;
HWND    gameHwnd = NULL;
int     screenW = 0, screenH = 0;
std::vector<Box> boxes;

// ================== 读内存 ==================
template <typename T>
T ReadMem(DWORD64 addr) {
    T val = {};
    if (addr > 0x10000)
        ReadProcessMemory(hProcess, (LPCVOID)addr, &val, sizeof(T), NULL);
    return val;
}

// ================== 读文件（修复版） ==================
char* ReadFileToBuffer(const char* path) {
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return NULL;

    DWORD size = GetFileSize(hFile, NULL);
    if (size == 0 || size == INVALID_FILE_SIZE) { CloseHandle(hFile); return NULL; }

    char* buffer = (char*)malloc(size + 1);
    if (!buffer) { CloseHandle(hFile); return NULL; }        // 修复 NULL 检查

    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buffer, size, &bytesRead, NULL)) {  // 修复返回值检查
        free(buffer);
        CloseHandle(hFile);
        return NULL;
    }
    buffer[size] = '\0';
    CloseHandle(hFile);
    return buffer;
}

// ================== 简单 JSON 取值 ==================
DWORD64 ParseJSON(const char* content, const char* key) {
    if (!content) return 0;
    char searchKey[128];
    sprintf_s(searchKey, "\"%s\"", key);
    const char* pos = strstr(content, searchKey);
    if (!pos) return 0;
    pos = strchr(pos, ':');
    if (!pos) return 0;
    return strtoull(pos + 1, NULL, 10);
}

// ================== 初始化 ==================
bool Init() {
    printf("[Info] Running dumper...\n");
    SetCurrentDirectoryA(DUMPER_DIR);
    system("cs2-dumper.exe > nul 2>&1");

    char* offsetData = ReadFileToBuffer(OFFSETS_FILE);
    char* clientData = ReadFileToBuffer(CLIENT_FILE);
    if (!offsetData || !clientData) {
        printf("[Error] JSON not found\n");
        free(offsetData);
        free(clientData);
        return false;
    }

    dwEntityList = ParseJSON(offsetData, "dwEntityList");
    dwLocalPlayerController = ParseJSON(offsetData, "dwLocalPlayerController");
    dwViewMatrix = ParseJSON(offsetData, "dwViewMatrix");
    m_iHealth = ParseJSON(clientData, "m_iHealth");
    m_iTeamNum = ParseJSON(clientData, "m_iTeamNum");
    m_pGameSceneNode = ParseJSON(clientData, "m_pGameSceneNode");
    m_vecAbsOrigin = ParseJSON(clientData, "m_vecAbsOrigin");
    m_vOldOrigin = ParseJSON(clientData, "m_vOldOrigin");  // 读不到就是 0
    m_hPlayerPawn = ParseJSON(clientData, "m_hPlayerPawn");

    free(offsetData);
    free(clientData);

    printf("[Info] EntityList=0x%llX ViewMatrix=0x%llX\n", dwEntityList, dwViewMatrix);

    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (wcscmp(pe.szExeFile, L"cs2.exe") == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (!pid) { printf("[Error] cs2.exe not found\n"); return false; }

    hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProcess) { printf("[Error] OpenProcess failed (need admin)\n"); return false; }

    HANDLE modSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32W me = { sizeof(me) };
    if (Module32FirstW(modSnap, &me)) {
        do {
            if (wcscmp(me.szModule, L"client.dll") == 0) {
                clientBase = (DWORD64)me.modBaseAddr;
                break;
            }
        } while (Module32NextW(modSnap, &me));
    }
    CloseHandle(modSnap);
    if (!clientBase) { printf("[Error] client.dll not found\n"); return false; }

    gameHwnd = FindWindowA(NULL, "Counter-Strike 2");
    if (!gameHwnd) { printf("[Error] Game window not found (use borderless)\n"); return false; }

    RECT r; GetClientRect(gameHwnd, &r);
    screenW = r.right - r.left;
    screenH = r.bottom - r.top;
    return true;
}

// ================== 世界坐标转屏幕 ==================
bool WorldToScreen(const float m[16], Vec3 p, int& sx, int& sy) {
    float cx = p.x * m[0] + p.y * m[1] + p.z * m[2] + m[3];
    float cy = p.x * m[4] + p.y * m[5] + p.z * m[6] + m[7];
    float cw = p.x * m[12] + p.y * m[13] + p.z * m[14] + m[15];
    if (cw < 0.001f) return false;
    sx = (int)((screenW / 2.0f) * (cx / cw + 1.0f));
    sy = (int)((screenH / 2.0f) * (1.0f - cy / cw));
    return true;
}

// ================== 后台线程 ==================
void ESPThread() {
    float matrix[16];
    static BYTE chunkBuf[0x78 * 512];

    std::vector<DWORD64> cachedEntities;   // 缓存的敌人实体地址
    DWORD lastFullScan = 0;

    while (true) {
        if (!hProcess || !clientBase) { Sleep(1000); continue; }

        ReadProcessMemory(hProcess, (LPCVOID)(clientBase + dwViewMatrix),
            matrix, sizeof(matrix), NULL);

        DWORD64 ges = ReadMem<DWORD64>(clientBase + dwEntityList);
        DWORD64 local = ReadMem<DWORD64>(clientBase + dwLocalPlayerController);
        if (!ges || !local) { Sleep(100); continue; }

        DWORD64 elist = ReadMem<DWORD64>(ges + 0x10);
        if (!elist) { Sleep(100); continue; }

        int myTeam = ReadMem<int>(local + m_iTeamNum);

        // ========== 每 500ms 全扫一次，重建实体缓存 ==========
        DWORD now = GetTickCount();
        if (cachedEntities.empty() || now - lastFullScan > 500) {
            cachedEntities.clear();
            std::unordered_set<DWORD64> seen;

            for (int ci = 0; ci < 256; ci++) {
                DWORD64 chunk = ReadMem<DWORD64>(elist + 0x8 * ci + 0x8);
                if (!chunk || chunk < 0x100000) continue;

                SIZE_T bytesRead = 0;
                BOOL ok = ReadProcessMemory(hProcess, (LPCVOID)chunk,
                    chunkBuf, sizeof(chunkBuf), &bytesRead);
                int itemCount = ok ? (int)(bytesRead / 0x78) : 0;

                for (int item = 0; item < 512; item++) {
                    if (ci == 0 && item == 0) continue;

                    DWORD64 ent = 0;
                    if (item < itemCount) {
                        memcpy(&ent, chunkBuf + item * 0x78, 8);
                    }
                    else {
                        ent = ReadMem<DWORD64>(chunk + 0x78 * item);
                    }

                    if (!ent || ent < 0x100000 || seen.count(ent)) continue;
                    seen.insert(ent);

                    int hp = ReadMem<int>(ent + m_iHealth);
                    int team = ReadMem<int>(ent + m_iTeamNum);
                    if (hp <= 0 || hp > 100 || (team != 2 && team != 3)) continue;

                    cachedEntities.push_back(ent);
                }
            }
            lastFullScan = now;
        }

        // ========== 中间帧只更新缓存实体的坐标，几十次读，极快 ==========
        std::vector<Box> tmp;
        for (size_t k = 0; k < cachedEntities.size(); k++) {
            DWORD64 ent = cachedEntities[k];

            int hp = ReadMem<int>(ent + m_iHealth);
            int team = ReadMem<int>(ent + m_iTeamNum);
            if (hp <= 0 || hp > 100 || (team != 2 && team != 3)) continue;

            DWORD64 node = ReadMem<DWORD64>(ent + m_pGameSceneNode);
            if (!node) continue;

            Vec3 foot = ReadMem<Vec3>(node + m_vecAbsOrigin);
            if (foot.x == 0 && m_vOldOrigin)
                foot = ReadMem<Vec3>(ent + m_vOldOrigin);
            if (foot.x == 0) continue;

            Vec3 head = { foot.x, foot.y, foot.z + 72.0f };
            int fx, fy, hx, hy;
            if (!WorldToScreen(matrix, foot, fx, fy)) continue;
            if (!WorldToScreen(matrix, head, hx, hy)) continue;

            int h = fy - hy;
            if (h <= 4) continue;

            Box b;
            b.x = fx - h / 4; b.y = hy;
            b.w = h / 2;      b.h = h;
            b.hp = hp;        b.is_team = (team == myTeam);
            tmp.push_back(b);
        }

        boxes.swap(tmp);
        Sleep(16);
    }
}

// ================== 画框窗口 ==================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        // 先画到内存位图
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, screenW, screenH);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        RECT full = { 0, 0, screenW, screenH };
        HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(memDC, &full, bg);
        DeleteObject(bg);

        HPEN penG = CreatePen(PS_SOLID, 2, RGB(0, 255, 0));
        HPEN penB = CreatePen(PS_SOLID, 2, RGB(0, 150, 255));
        HBRUSH brushBg = CreateSolidBrush(RGB(0, 0, 0));
        HBRUSH brushHp = CreateSolidBrush(RGB(255, 0, 0));
        SelectObject(memDC, GetStockObject(NULL_BRUSH));

        for (auto& b : boxes) {
            SelectObject(memDC, b.is_team ? penB : penG);
            Rectangle(memDC, b.x, b.y, b.x + b.w, b.y + b.h);

            RECT hr = { b.x - 6, b.y, b.x - 2, b.y + b.h };
            FillRect(memDC, &hr, brushBg);
            hr.top = b.y + (int)(b.h * (1.0f - b.hp / 100.0f));
            FillRect(memDC, &hr, brushHp);
        }

        // 一次性贴到窗口
        BitBlt(hdc, 0, 0, screenW, screenH, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        DeleteObject(penG); DeleteObject(penB);
        DeleteObject(brushBg); DeleteObject(brushHp);

        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ================== main ==================
int main() {
    if (!Init()) { printf("[Fatal] Init failed\n"); system("pause"); return 0; }

    std::thread(ESPThread).detach();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"ESPOverlay";
    RegisterClassExW(&wc);

    POINT pt = { 0, 0 };
    ClientToScreen(gameHwnd, &pt);
    HWND ov = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"ESP", WS_POPUP,
        pt.x, pt.y, screenW, screenH,
        NULL, NULL, wc.hInstance, NULL);

    SetLayeredWindowAttributes(ov, RGB(0, 0, 0), 0, LWA_COLORKEY);
    ShowWindow(ov, SW_SHOW);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        InvalidateRect(ov, NULL, FALSE);
        Sleep(16);
    }
    return 0;
}