// HaloCraft.exe: one click starts everything. The Minecraft that plays inside Halo (the bundled
// Prism Launcher instance, hidden until Halo is up), Halo: MCC without anti-cheat (through Steam),
// and Spark, which loads the HaloCraft mod into MCC once it has started. Clicking it again while
// things run starts only what's missing. The bundle handling is ported from SkyCraft's
// skse/src/Launcher.cpp (MIT, chasmlol).
//
// Next to it: spark.dll, halocraft.dll, HaloCraft-Minecraft.zip (tools/package.ps1 puts them there).
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <format>
#include <string>

namespace fs = std::filesystem;

namespace {
    constexpr wchar_t kTitle[] = L"HaloCraft";
    constexpr wchar_t kMccExe[] = L"MCC-Win64-Shipping.exe";
    constexpr wchar_t kMccFromLibrary[] = L"steamapps\\common\\Halo The Master Chief Collection\\MCC\\Binaries\\Win64";

    void fail(const std::wstring& text) { MessageBoxW(nullptr, text.c_str(), kTitle, MB_ICONERROR | MB_OK); }

    fs::path here() {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return fs::path(path).parent_path();
    }

    fs::path localAppData() {
        wchar_t dir[MAX_PATH];
        return GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH) ? fs::path(dir) : fs::path();
    }

    // MCC's Binaries\Win64, in whichever Steam library has it (libraryfolders.vdf's "path" lines).
    fs::path findMcc() {
        wchar_t steam[MAX_PATH];
        DWORD size = sizeof(steam);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &size) != ERROR_SUCCESS)
            return {};
        std::ifstream vdf(fs::path(steam) / "steamapps" / "libraryfolders.vdf");
        for (std::string line; std::getline(vdf, line);) {
            const auto key = line.find("\"path\"");
            if (key == std::string::npos)
                continue;
            const auto open = line.find('"', key + 6), close = line.rfind('"');
            if (open == std::string::npos || close <= open)
                continue;
            std::string library = line.substr(open + 1, close - open - 1);
            for (auto at = library.find("\\\\"); at != std::string::npos; at = library.find("\\\\", at + 1))
                library.erase(at, 1);
            const fs::path bin = fs::path(library) / kMccFromLibrary;
            if (fs::exists(bin / kMccExe))
                return bin;
        }
        return {};
    }

    DWORD findProcess(const wchar_t* exe) {
        DWORD pid = 0;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return 0;
        PROCESSENTRY32W entry{ sizeof(entry) };
        for (BOOL more = Process32FirstW(snapshot, &entry); more && !pid; more = Process32NextW(snapshot, &entry))
            if (_wcsicmp(entry.szExeFile, exe) == 0)
                pid = entry.th32ProcessID;
        CloseHandle(snapshot);
        return pid;
    }

    bool hasModule(DWORD pid, const wchar_t* name) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        if (snapshot == INVALID_HANDLE_VALUE)
            return false;
        MODULEENTRY32W entry{ sizeof(entry) };
        bool found = false;
        for (BOOL more = Module32FirstW(snapshot, &entry); more && !found; more = Module32NextW(snapshot, &entry))
            found = _wcsicmp(entry.szModule, name) == 0;
        CloseHandle(snapshot);
        return found;
    }

    // The usual LoadLibrary in the target process.
    bool inject(DWORD pid, const fs::path& dll) {
        HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
        if (!process)
            return false;
        const std::wstring path = dll.wstring();
        const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD loaded = 0;
        if (remote && WriteProcessMemory(process, remote, path.c_str(), bytes, nullptr)) {
            const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
            if (HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr)) {
                WaitForSingleObject(thread, 30000);
                GetExitCodeThread(thread, &loaded);  // the module handle's low half: 0 if it didn't load
                CloseHandle(thread);
            }
        }
        if (remote)
            VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return loaded != 0;
    }

    // A Minecraft with the HaloCraft mod holds this while it runs (SkyLink.announceRunning).
    bool minecraftRunning() {
        HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\SkyCraft_v1_minecraft");
        if (mutex)
            CloseHandle(mutex);
        return mutex != nullptr;
    }

    // Unpacks the bundled Minecraft to %LOCALAPPDATA%\HaloCraft the first time, and again whenever
    // this HaloCraft brings a different one. Prism's own data there (the signed-in account, the
    // downloaded Minecraft and Java, the world) is kept; the instance and its mod jars are replaced,
    // so both halves always match.
    fs::path ensureBundle(const fs::path& zip) {
        const fs::path dir = localAppData() / "HaloCraft";
        const fs::path prism = dir / "Prism" / "prismlauncher.exe";
        std::error_code ec;
        const std::string stamp = std::format("{} {}", fs::file_size(zip, ec), fs::last_write_time(zip, ec).time_since_epoch().count());
        std::string installed;
        if (std::ifstream in{ dir / "bundle.stamp" }; in)
            std::getline(in, installed);
        if (installed == stamp && fs::exists(prism))
            return prism;
        fs::create_directories(dir, ec);
        for (const auto& entry : fs::directory_iterator(dir / "Prism" / "instances" / "HaloCraft" / ".minecraft" / "mods", ec)) {
            const auto name = entry.path().filename().string();
            if (name.starts_with("skycraft-") || name.starts_with("fabric-api-") || name.starts_with("e4mc-"))
                fs::remove(entry.path(), ec);
        }
        wchar_t system[MAX_PATH];
        GetSystemDirectoryW(system, MAX_PATH);
        std::wstring command = std::format(L"\"{}\\tar.exe\" -xf \"{}\" -C \"{}\"", system, zip.wstring(), dir.wstring());
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        DWORD code = 1;
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        if (code != 0 || !fs::exists(prism))
            return {};
        // Prism's settings: only the first time (after that they're the player's).
        if (!fs::exists(dir / "Prism" / "prismlauncher.cfg"))
            fs::copy_file(dir / "defaults" / "prismlauncher.cfg", dir / "Prism" / "prismlauncher.cfg", ec);
        std::ofstream(dir / "bundle.stamp") << stamp;
        return prism;
    }

    void open(const fs::path& file, const wchar_t* args = nullptr) {
        ShellExecuteW(nullptr, L"open", file.c_str(), args, file.has_parent_path() ? file.parent_path().c_str() : nullptr, SW_SHOWNORMAL);
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const fs::path dir = here();
    const fs::path spark = dir / "spark.dll", mod = dir / "halocraft.dll", bundle = dir / "HaloCraft-Minecraft.zip";
    for (const auto& f : { spark, mod, bundle })
        if (!fs::exists(f)) {
            fail(L"Missing " + f.filename().wstring() + L" next to HaloCraft.exe. Unzip the whole HaloCraft folder and run it from there.");
            return 1;
        }
    const fs::path mcc = findMcc();
    if (mcc.empty()) {
        fail(L"Couldn't find Halo: The Master Chief Collection in your Steam libraries. Install it through Steam first.");
        return 1;
    }

    // The mod goes where Spark looks for mods, Spark next to MCC (so MCC never holds this folder's
    // files). Both are locked while a running MCC has them loaded: then that copy stays.
    std::error_code ec;
    fs::create_directories(mcc / "mods", ec);
    fs::copy_file(mod, mcc / "mods" / "halocraft.dll", fs::copy_options::overwrite_existing, ec);
    fs::copy_file(spark, mcc / "spark.dll", fs::copy_options::overwrite_existing, ec);

    // Minecraft first: it starts hidden and waits on its title screen for Halo, and it's the slow one.
    if (!minecraftRunning()) {
        const fs::path prism = ensureBundle(bundle);
        if (prism.empty()) {
            fail(L"Couldn't unpack the bundled Minecraft to %LOCALAPPDATA%\\HaloCraft.");
            return 1;
        }
        open(prism, L"--launch HaloCraft");
    }

    DWORD pid = findProcess(kMccExe);
    if (!pid)
        ShellExecuteW(nullptr, L"open", L"steam://launch/976730/option2", nullptr, nullptr, SW_SHOWNORMAL);  // option2: anti-cheat disabled

    // Spark goes in once MCC has finished starting up (injecting earlier crashes it); PartyWin.dll
    // is among the last things MCC loads. Up to ten minutes: Steam may have to start, or update MCC.
    for (int waited = 0; !(pid && hasModule(pid, L"PartyWin.dll")); waited += 2) {
        if (waited > 600) {
            fail(L"Halo: MCC didn't start. Start it from Steam (choose \"Play with anti-cheat disabled\"), then run HaloCraft again.");
            return 1;
        }
        Sleep(2000);
        pid = findProcess(kMccExe);
    }
    if (hasModule(pid, L"spark.dll"))
        return 0;  // already in: nothing to do
    Sleep(5000);
    if (!inject(pid, mcc / "spark.dll")) {
        fail(L"Couldn't load Spark into Halo: MCC. Is MCC running with anti-cheat disabled?");
        return 1;
    }
    return 0;
}
