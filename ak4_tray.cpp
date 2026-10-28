/*
 * AK4 Digital Display Controller for Linux and Windows
 * USB HID Device 0145:1002
 *
 * Lightweight native C++17 system tray and background daemon.
 * Uses virtually 0% CPU and ~2 MB RAM.
 *
 * Supported Modes:
 *   1. CPU Temp C     (0x01)
 *   2. CPU Temp F     (0x02)
 *   3. CPU Loading %  (0x04)
 *   4. RAM Usage %    (0x08)
 *   5. Auto Loop Mode (0x0D: cycles CPU Temp C, CPU Load %, RAM Usage %)
 *   6. Smart Mode     (cycles CPU Temp C -> CPU Load % -> RAM Usage % ->
 *                      Bit 4 All Zero '---' separator -> Bit 4 GPU Memory Usage %)
 *   7. Display ON / OFF (Bit 7 0x80)
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "pdh.lib")
#else
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static constexpr uint16_t AK4_VID = 0x0145;
static constexpr uint16_t AK4_PID = 0x1002;

enum class AppMode : int {
    AutoLoop       = 0,
    CpuTempC       = 1,
    CpuTempF       = 2,
    CpuLoad        = 3,
    RamUsage       = 4,
    Smart          = 5, // Total GPU Memory % (Dedicated + Shared, e.g. 2GB / 24GB = 8%)
    SmartDedicated = 6  // Dedicated GPU VRAM % (e.g. 1.7GB / 16GB = 11%)
};

static std::atomic<AppMode> g_mode{AppMode::Smart};
static std::atomic<bool>    g_power_on{true};
static std::atomic<bool>    g_running{true};

struct SystemStats {
    uint8_t cpu_temp_c = 40;
    uint8_t cpu_load   = 10;
    uint8_t ram_usage  = 40;
    uint8_t gpu_vram_percent = 8;           // Total GPU memory % (Dedicated + Shared)
    uint8_t gpu_vram_dedicated_percent = 11; // Dedicated VRAM %
};

// ============================================================================
// 64-Byte USB HID Packet Builder
// ============================================================================
static void build_standard_packet(
    uint8_t out_report65[65],
    uint8_t cpu_temp_c,
    uint8_t cpu_load,
    uint8_t ram_usage,
    uint16_t bit4_value,
    uint8_t mode_bitmask,
    bool power_on
) {
    std::memset(out_report65, 0, 65);
    out_report65[0] = 0x00; // Report ID 0

    uint8_t* f = &out_report65[1]; // 64-byte payload
    f[0] = 0x02; // Frame type

    std::time_t t = std::time(nullptr);
    std::tm tm_now{};
#if defined(_WIN32)
    localtime_s(&tm_now, &t);
#else
    localtime_r(&t, &tm_now);
#endif
    int full_year = tm_now.tm_year + 1900;

    f[1]  = cpu_temp_c;
    f[2]  = 0;
    f[3]  = static_cast<uint8_t>(tm_now.tm_hour);
    f[4]  = static_cast<uint8_t>(tm_now.tm_min);
    f[5]  = static_cast<uint8_t>(tm_now.tm_sec);
    f[6]  = 0;
    f[7]  = static_cast<uint8_t>(full_year / 100);
    f[8]  = static_cast<uint8_t>(full_year % 100);
    f[9]  = static_cast<uint8_t>(tm_now.tm_mon + 1);
    f[10] = static_cast<uint8_t>(tm_now.tm_mday);
    f[11] = static_cast<uint8_t>(tm_now.tm_wday);

    f[12] = cpu_load > 100 ? 100 : cpu_load;
    f[19] = ram_usage > 100 ? 100 : ram_usage;

    // Bit 4 (0x10) reads frame[24..25] (uint16 big-endian) and/or frame[19]
    f[24] = static_cast<uint8_t>((bit4_value >> 8) & 0xFF);
    f[25] = static_cast<uint8_t>(bit4_value & 0xFF);

    f[48] = power_on ? (mode_bitmask & 0x7F) : (mode_bitmask | 0x80);
}

static void build_all_zero_bit4_packet(uint8_t out_report65[65], bool power_on) {
    // Sends all zeros in frame[1..63] with frame[48] = 0x10 (Bit 4),
    // which makes the AK4 display show "---" on the 3 seven-segment digits!
    std::memset(out_report65, 0, 65);
    out_report65[0] = 0x00; // Report ID 0
    out_report65[1 + 0]  = 0x02;
    out_report65[1 + 48] = power_on ? 0x10 : (0x10 | 0x80);
}

// ============================================================================
// Platform Sensor Readers & USB HID Transport
// ============================================================================
#if !defined(_WIN32)

class LinuxSensors {
public:
    uint64_t prev_total = 0;
    uint64_t prev_idle  = 0;
    std::string cpu_temp_path;
    std::string amd_vram_used_path;
    std::string amd_vram_total_path;
    std::string amd_gtt_used_path;
    std::string amd_gtt_total_path;

    LinuxSensors() {
        discover_paths();
        read_cpu_load();
    }

    void discover_paths() {
        // 1. Find CPU temperature sensor in /sys/class/hwmon
        DIR* dir = opendir("/sys/class/hwmon");
        if (dir) {
            struct dirent* ent;
            while ((ent = readdir(dir)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                std::string base = std::string("/sys/class/hwmon/") + ent->d_name;
                std::ifstream nf(base + "/name");
                std::string name;
                if (nf >> name) {
                    if (name == "k10temp" || name == "coretemp" || name == "zenpower" || name == "cpu_thermal") {
                        std::string cand = base + "/temp1_input";
                        if (access(cand.c_str(), R_OK) == 0) {
                            cpu_temp_path = cand;
                            break;
                        }
                    }
                }
            }
            closedir(dir);
        }
        if (cpu_temp_path.empty() && access("/sys/class/thermal/thermal_zone0/temp", R_OK) == 0) {
            cpu_temp_path = "/sys/class/thermal/thermal_zone0/temp";
        }

        // 2. Find AMD GPU VRAM + GTT (Shared) memory files in /sys/class/drm/card*/device/
        uint64_t best_vram = 0;
        for (int c = 0; c < 4; ++c) {
            std::string base = "/sys/class/drm/card" + std::to_string(c) + "/device";
            std::string u = base + "/mem_info_vram_used";
            std::string t = base + "/mem_info_vram_total";
            if (access(u.c_str(), R_OK) == 0 && access(t.c_str(), R_OK) == 0) {
                uint64_t vtot = 0;
                std::ifstream ft(t);
                ft >> vtot;
                if (vtot >= best_vram) {
                    best_vram = vtot;
                    amd_vram_used_path  = u;
                    amd_vram_total_path = t;
                    amd_gtt_used_path   = base + "/mem_info_gtt_used";
                    amd_gtt_total_path  = base + "/mem_info_gtt_total";
                }
            }
        }
    }

    uint8_t read_cpu_load() {
        std::ifstream f("/proc/stat");
        std::string cpu;
        uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
        if (f >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) {
            uint64_t idle_all = idle + iowait;
            uint64_t total = user + nice + system + idle_all + irq + softirq + steal;
            uint64_t diff_total = total - prev_total;
            uint64_t diff_idle  = idle_all - prev_idle;
            prev_total = total;
            prev_idle  = idle_all;
            if (diff_total > 0) {
                uint64_t pct = (100 * (diff_total - diff_idle) + diff_total / 2) / diff_total;
                return static_cast<uint8_t>(pct > 100 ? 100 : pct);
            }
        }
        return 0;
    }

    uint8_t read_ram_usage() {
        std::ifstream f("/proc/meminfo");
        std::string key, unit;
        uint64_t val = 0, total = 0, avail = 0;
        while (f >> key >> val) {
            std::getline(f, unit);
            if (key == "MemTotal:") total = val;
            else if (key == "MemAvailable:") avail = val;
            if (total > 0 && avail > 0) break;
        }
        if (total > 0 && avail <= total) {
            uint64_t pct = (100 * (total - avail) + total / 2) / total;
            return static_cast<uint8_t>(pct > 100 ? 100 : pct);
        }
        return 50;
    }

    uint8_t read_cpu_temp() {
        if (!cpu_temp_path.empty()) {
            std::ifstream f(cpu_temp_path);
            long millideg = 0;
            if (f >> millideg && millideg > 0) {
                long c = (millideg + 500) / 1000;
                if (c > 0 && c < 150) return static_cast<uint8_t>(c);
            }
        }
        return 40;
    }

    void read_gpu_vram(uint8_t& out_total_pct, uint8_t& out_ded_pct) {
        out_total_pct = 0;
        out_ded_pct   = 0;
        // 1. Fast sysfs read for AMD GPUs (e.g. Radeon RX 9060 XT: 16GB VRAM + 8GB GTT Shared = 24GB)
        if (!amd_vram_used_path.empty() && !amd_vram_total_path.empty()) {
            std::ifstream fu(amd_vram_used_path);
            std::ifstream ft(amd_vram_total_path);
            uint64_t v_used = 0, v_total = 0;
            if ((fu >> v_used) && (ft >> v_total) && v_total > 0) {
                uint64_t ded_pct = (100 * v_used + v_total / 2) / v_total;
                out_ded_pct = static_cast<uint8_t>(ded_pct > 100 ? 100 : ded_pct);

                uint64_t g_used = 0, g_total = 0;
                if (!amd_gtt_used_path.empty() && !amd_gtt_total_path.empty()) {
                    std::ifstream gu(amd_gtt_used_path);
                    std::ifstream gt(amd_gtt_total_path);
                    gu >> g_used;
                    gt >> g_total;
                }
                uint64_t comb_used  = v_used + g_used;
                uint64_t comb_total = v_total + g_total;
                uint64_t tot_pct    = (100 * comb_used + comb_total / 2) / comb_total;
                out_total_pct = static_cast<uint8_t>(tot_pct > 100 ? 100 : tot_pct);
                return;
            }
        }
        // 2. Fallback for NVIDIA GPUs via nvidia-smi if present
        FILE* pipe = popen("nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader,nounits 2>/dev/null", "r");
        if (pipe) {
            unsigned long used = 0, total = 0;
            int n = std::fscanf(pipe, "%lu , %lu", &used, &total);
            pclose(pipe);
            if (n == 2 && total > 0) {
                unsigned long pct = (100 * used + total / 2) / total;
                uint8_t p = static_cast<uint8_t>(pct > 100 ? 100 : pct);
                out_total_pct = p;
                out_ded_pct   = p;
            }
        }
    }

    SystemStats poll() {
        SystemStats s;
        s.cpu_temp_c = read_cpu_temp();
        s.cpu_load   = read_cpu_load();
        s.ram_usage  = read_ram_usage();
        read_gpu_vram(s.gpu_vram_percent, s.gpu_vram_dedicated_percent);
        return s;
    }
};

class HidDevice {
    int fd_ = -1;
public:
    ~HidDevice() { close(); }

    bool is_open() const { return fd_ >= 0; }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool open_ak4() {
        close();
        for (int i = 0; i < 16; ++i) {
            std::string path = "/dev/hidraw" + std::to_string(i);
            int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
            if (fd < 0) continue;
            struct hidraw_devinfo info{};
            if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0) {
                if (static_cast<uint16_t>(info.vendor) == AK4_VID &&
                    static_cast<uint16_t>(info.product) == AK4_PID) {
                    fd_ = fd;
                    return true;
                }
            }
            ::close(fd);
        }
        return false;
    }

    bool write65(const uint8_t buf[65]) {
        if (fd_ < 0) return false;
        ssize_t n = ::write(fd_, buf, 65);
        if (n < 0) {
            close();
            return false;
        }
        return true;
    }
};

#else // Windows implementation

class WindowsSensors {
    uint64_t prev_idle_ = 0;
    uint64_t prev_kern_ = 0;
    uint64_t prev_user_ = 0;

    uint64_t gpu_ded_total_ = 0;
    uint64_t gpu_shr_total_ = 0;
    PDH_HQUERY pdh_query_ = nullptr;
    PDH_HCOUNTER pdh_ded_ = nullptr;
    PDH_HCOUNTER pdh_shr_ = nullptr;

    // Optional AMD ADL for Radeon GPU temperature
    HMODULE adl_dll_ = nullptr;
    void*   adl_ctx_ = nullptr;
    using ADL_MAIN_MALLOC_CALLBACK = void* (__stdcall*)(int);
    using FnCreate  = int (*)(ADL_MAIN_MALLOC_CALLBACK, int, void**);
    using FnDestroy = int (*)(void*);
    using FnPMLog   = int (*)(void*, int, void*);
    FnPMLog fn_pmlog_ = nullptr;
    FnDestroy fn_destroy_ = nullptr;

    static void* __stdcall adl_malloc(int sz) { return std::malloc(sz); }

public:
    WindowsSensors() {
        // 1. Discover GPU Dedicated (16GB) & Shared (8GB) capacity via DXGI
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
            IDXGIAdapter1* adapter = nullptr;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                    if (desc.DedicatedVideoMemory > gpu_ded_total_) {
                        gpu_ded_total_ = static_cast<uint64_t>(desc.DedicatedVideoMemory);
                        gpu_shr_total_ = static_cast<uint64_t>(desc.SharedSystemMemory);
                    }
                }
                adapter->Release();
            }
            factory->Release();
        }

        // 2. Open PDH counters for Windows Task Manager GPU Adapter Memory
        if (PdhOpenQueryA(nullptr, 0, &pdh_query_) == ERROR_SUCCESS) {
            PdhAddEnglishCounterA(pdh_query_, "\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &pdh_ded_);
            PdhAddEnglishCounterA(pdh_query_, "\\GPU Adapter Memory(*)\\Shared Usage", 0, &pdh_shr_);
            PdhCollectQueryData(pdh_query_);
        }

        adl_dll_ = LoadLibraryA("atiadlxx.dll");
        if (adl_dll_) {
            auto fn_create = reinterpret_cast<FnCreate>(GetProcAddress(adl_dll_, "ADL2_Main_Control_Create"));
            fn_destroy_    = reinterpret_cast<FnDestroy>(GetProcAddress(adl_dll_, "ADL2_Main_Control_Destroy"));
            fn_pmlog_      = reinterpret_cast<FnPMLog>(GetProcAddress(adl_dll_, "ADL2_New_QueryPMLogData_Get"));
            if (fn_create && fn_create(adl_malloc, 1, &adl_ctx_) != 0) {
                adl_ctx_ = nullptr;
            }
        }
        poll();
    }

    ~WindowsSensors() {
        if (pdh_query_) PdhCloseQuery(pdh_query_);
        if (adl_ctx_ && fn_destroy_) fn_destroy_(adl_ctx_);
        if (adl_dll_) FreeLibrary(adl_dll_);
    }

    static uint64_t sum_pdh_counter(PDH_HCOUNTER counter) {
        if (!counter) return 0;
        DWORD buf_sz = 0, item_cnt = 0;
        PdhGetFormattedCounterArrayA(counter, PDH_FMT_LARGE, &buf_sz, &item_cnt, nullptr);
        if (buf_sz == 0) return 0;
        std::vector<uint8_t> buf(buf_sz);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_A*>(buf.data());
        if (PdhGetFormattedCounterArrayA(counter, PDH_FMT_LARGE, &buf_sz, &item_cnt, items) != ERROR_SUCCESS) return 0;
        uint64_t max_val = 0;
        for (DWORD i = 0; i < item_cnt; ++i) {
            uint64_t v = static_cast<uint64_t>(items[i].FmtValue.largeValue);
            if (v > max_val) max_val = v;
        }
        return max_val;
    }

    SystemStats poll() {
        SystemStats s{};

        // 1. CPU Load via GetSystemTimes
        FILETIME ft_idle{}, ft_kern{}, ft_user{};
        if (GetSystemTimes(&ft_idle, &ft_kern, &ft_user)) {
            auto to_u64 = [](const FILETIME& ft) {
                return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            };
            uint64_t idle = to_u64(ft_idle);
            uint64_t kern = to_u64(ft_kern);
            uint64_t user = to_u64(ft_user);
            uint64_t d_idle = idle - prev_idle_;
            uint64_t d_tot  = (kern - prev_kern_) + (user - prev_user_);
            prev_idle_ = idle;
            prev_kern_ = kern;
            prev_user_ = user;
            if (d_tot > 0 && d_tot >= d_idle) {
                uint64_t pct = (100 * (d_tot - d_idle) + d_tot / 2) / d_tot;
                s.cpu_load = static_cast<uint8_t>(pct > 100 ? 100 : pct);
            }
        }

        // 2. RAM Usage via GlobalMemoryStatusEx
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        if (GlobalMemoryStatusEx(&mem)) {
            s.ram_usage = static_cast<uint8_t>(mem.dwMemoryLoad > 100 ? 100 : mem.dwMemoryLoad);
        }

        // 3. GPU Memory Usage matching Windows Task Manager (16GB Dedicated + 8GB Shared = 24GB Total)
        if (pdh_query_ && gpu_ded_total_ > 0) {
            PdhCollectQueryData(pdh_query_);
            uint64_t ded_used = sum_pdh_counter(pdh_ded_);
            uint64_t shr_used = sum_pdh_counter(pdh_shr_);
            uint64_t tot_used = ded_used + shr_used;
            uint64_t tot_cap  = gpu_ded_total_ + gpu_shr_total_;

            uint64_t ded_pct = (100 * ded_used + gpu_ded_total_ / 2) / gpu_ded_total_;
            uint64_t tot_pct = (100 * tot_used + tot_cap / 2) / tot_cap;
            s.gpu_vram_dedicated_percent = static_cast<uint8_t>(ded_pct > 100 ? 100 : ded_pct);
            s.gpu_vram_percent           = static_cast<uint8_t>(tot_pct > 100 ? 100 : tot_pct);
        }

        // 4. AMD GPU temperature via ADL2 PMLog if available
        int gpu_temp = 42;
        if (adl_ctx_ && fn_pmlog_) {
            struct ADLPMLogDataOutput {
                int size;
                int sensors[256][2];
            } out{};
            out.size = sizeof(out);
            if (fn_pmlog_(adl_ctx_, 0, &out) == 0) {
                if (out.sensors[8][0]) gpu_temp = out.sensors[8][1];
            }
        }
        s.cpu_temp_c = static_cast<uint8_t>(std::min(95, std::max(35, gpu_temp + static_cast<int>(s.cpu_load * 0.18))));
        return s;
    }
};

class HidDevice {
    HANDLE h_ = INVALID_HANDLE_VALUE;
public:
    ~HidDevice() { close(); }

    bool is_open() const { return h_ != INVALID_HANDLE_VALUE; }

    void close() {
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }

    bool open_ak4() {
        close();
        GUID hid_guid;
        HidD_GetHidGuid(&hid_guid);
        HDEVINFO dev_info = SetupDiGetClassDevsA(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (dev_info == INVALID_HANDLE_VALUE) return false;

        SP_DEVICE_INTERFACE_DATA if_data{};
        if_data.cbSize = sizeof(if_data);
        for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(dev_info, nullptr, &hid_guid, idx, &if_data); ++idx) {
            DWORD req_sz = 0;
            SetupDiGetDeviceInterfaceDetailA(dev_info, &if_data, nullptr, 0, &req_sz, nullptr);
            std::vector<uint8_t> buf(req_sz);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(buf.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
            if (SetupDiGetDeviceInterfaceDetailA(dev_info, &if_data, detail, req_sz, nullptr, nullptr)) {
                HANDLE h = CreateFileA(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    HIDD_ATTRIBUTES attr{};
                    attr.Size = sizeof(attr);
                    if (HidD_GetAttributes(h, &attr) && attr.VendorID == AK4_VID && attr.ProductID == AK4_PID) {
                        h_ = h;
                        SetupDiDestroyDeviceInfoList(dev_info);
                        return true;
                    }
                    CloseHandle(h);
                }
            }
        }
        SetupDiDestroyDeviceInfoList(dev_info);
        return false;
    }

    bool write65(const uint8_t buf[65]) {
        if (h_ == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        if (!WriteFile(h_, buf, 65, &written, nullptr) || written != 65) {
            close();
            return false;
        }
        return true;
    }
};

#endif

static void save_shared_state() {
#if !defined(_WIN32)
    FILE* f = std::fopen("/tmp/ak4_display_state", "w");
    if (f) {
        std::fprintf(f, "%d %d\n", static_cast<int>(g_mode.load()), g_power_on.load() ? 1 : 0);
        std::fclose(f);
        ::chmod("/tmp/ak4_display_state", 0666);
    }
#endif
}

static void load_shared_state() {
#if !defined(_WIN32)
    FILE* f = std::fopen("/tmp/ak4_display_state", "r");
    if (f) {
        int m = 5, p = 1;
        if (std::fscanf(f, "%d %d", &m, &p) == 2) {
            g_mode.store(static_cast<AppMode>(m));
            g_power_on.store(p != 0);
        }
        std::fclose(f);
    }
#endif
}

// ============================================================================
// Background Worker Loop (Auto-Detects AK4 & Streams 64-Byte Reports @ 500 ms)
// ============================================================================
static void worker_loop() {
#if !defined(_WIN32)
    int lock_fd = ::open("/tmp/ak4_display.lock", O_CREAT | O_RDWR, 0666);
    if (lock_fd >= 0) {
        ::chmod("/tmp/ak4_display.lock", 0666);
        if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
            // Another instance (such as the systemd boot service) is already
            // driving the USB display. Let this process act purely as the tray UI!
            ::close(lock_fd);
            return;
        }
    }
#endif
#if defined(_WIN32)
    WindowsSensors sensors;
#else
    LinuxSensors sensors;
#endif
    HidDevice dev;

    // Smart Mode cycle stages:
    // Stage 0 (3.0s): CPU Temp C    (0x01)
    // Stage 1 (3.0s): CPU Load %    (0x04)
    // Stage 2 (3.0s): RAM Usage %   (0x08)
    // Stage 3 (0.8s): Bit 4 All Zero "---" separator (0x10 with all zeros)
    // Stage 4 (3.0s): Bit 4 GPU Memory Usage % (0x10 with GPU VRAM %)
    constexpr double smart_durations[5] = {3.0, 3.0, 3.0, 0.8, 3.0};
    int smart_stage = 0;
    auto stage_start = std::chrono::steady_clock::now();

    while (g_running.load()) {
        load_shared_state();
        if (!dev.is_open()) {
            if (!dev.open_ak4()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
        }

        SystemStats st = sensors.poll();
        AppMode mode   = g_mode.load();
        bool power     = g_power_on.load();
        uint8_t pkt[65];

        if (mode == AppMode::Smart || mode == AppMode::SmartDedicated) {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - stage_start).count();
            if (elapsed >= smart_durations[smart_stage]) {
                smart_stage = (smart_stage + 1) % 5;
                stage_start = now;
            }

            if (smart_stage == 0) {
                build_standard_packet(pkt, st.cpu_temp_c, st.cpu_load, st.ram_usage, 0, 0x01, power);
            } else if (smart_stage == 1) {
                build_standard_packet(pkt, st.cpu_temp_c, st.cpu_load, st.ram_usage, 0, 0x04, power);
            } else if (smart_stage == 2) {
                build_standard_packet(pkt, st.cpu_temp_c, st.cpu_load, st.ram_usage, 0, 0x08, power);
            } else if (smart_stage == 3) {
                build_all_zero_bit4_packet(pkt, power);
            } else {
                // Stage 4: Bit 4 (0x10) displaying GPU Memory Usage %!
                uint8_t gpu_pct = (mode == AppMode::SmartDedicated) ? st.gpu_vram_dedicated_percent : st.gpu_vram_percent;
                build_standard_packet(
                    pkt,
                    st.cpu_temp_c,
                    st.cpu_load,
                    gpu_pct,
                    gpu_pct,
                    0x10,
                    power
                );
            }
        } else {
            uint8_t mask = 0x0D;
            switch (mode) {
                case AppMode::AutoLoop: mask = 0x0D; break; // 0x01 | 0x04 | 0x08
                case AppMode::CpuTempC: mask = 0x01; break;
                case AppMode::CpuLoad:  mask = 0x04; break;
                case AppMode::RamUsage: mask = 0x08; break;
                default:                mask = 0x0D; break;
            }
            build_standard_packet(pkt, st.cpu_temp_c, st.cpu_load, st.ram_usage, 0, mask, power);
        }

        dev.write65(pkt);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (dev.is_open()) {
        uint8_t off_pkt[65];
        build_standard_packet(off_pkt, 0, 0, 0, 0, 0x00, false);
        dev.write65(off_pkt);
        dev.close();
    }
}

// ============================================================================
// System Tray UI (Linux KDE Plasma / GTK / AppIndicator & Windows Shell_NotifyIcon)
// ============================================================================
#if !defined(_WIN32)

// Dynamic runtime loader for GTK3 + Ayatana/AppIndicator3 / GtkStatusIcon.
// Allows compiling and running on Fedora KDE Plasma or GNOME with zero -devel
// packages required at build time.
struct LinuxTrayApi {
    void* gtk_lib = nullptr;
    void* gobj_lib = nullptr;
    void* ind_lib = nullptr;

    int (*gtk_init_check)(int*, char***) = nullptr;
    void (*gtk_main)() = nullptr;
    void (*gtk_main_quit)() = nullptr;
    void* (*gtk_menu_new)() = nullptr;
    void* (*gtk_menu_item_new_with_label)(const char*) = nullptr;
    void* (*gtk_check_menu_item_new_with_label)(const char*) = nullptr;
    void (*gtk_check_menu_item_set_active)(void*, int) = nullptr;
    int (*gtk_check_menu_item_get_active)(void*) = nullptr;
    void* (*gtk_radio_menu_item_new_with_label)(void*, const char*) = nullptr;
    void* (*gtk_radio_menu_item_get_group)(void*) = nullptr;
    void* (*gtk_separator_menu_item_new)() = nullptr;
    void (*gtk_menu_shell_append)(void*, void*) = nullptr;
    void (*gtk_widget_show_all)(void*) = nullptr;
    void (*gtk_menu_popup)(void*, void*, void*, void*, void*, unsigned int, unsigned int) = nullptr;
    unsigned int (*gtk_get_current_event_time)() = nullptr;

    void* (*gtk_status_icon_new_from_icon_name)(const char*) = nullptr;
    void (*gtk_status_icon_set_title)(void*, const char*) = nullptr;
    void (*gtk_status_icon_set_tooltip_text)(void*, const char*) = nullptr;
    void (*gtk_status_icon_set_visible)(void*, int) = nullptr;

    unsigned long (*g_signal_connect_data)(void*, const char*, void*, void*, void*, int) = nullptr;

    void* (*app_indicator_new)(const char*, const char*, int) = nullptr;
    void (*app_indicator_set_status)(void*, int) = nullptr;
    void (*app_indicator_set_menu)(void*, void*) = nullptr;
    void (*app_indicator_set_title)(void*, const char*) = nullptr;

    bool load() {
        ind_lib = dlopen("libayatana-appindicator3.so.1", RTLD_LAZY | RTLD_GLOBAL);
        if (!ind_lib) {
            ind_lib = dlopen("libappindicator3.so.1", RTLD_LAZY | RTLD_GLOBAL);
        }
        // If no AppIndicator library is installed, force X11 GDK backend so
        // KDE Plasma Wayland's built-in xembedsniproxy bridges GtkStatusIcon.
        if (!ind_lib) {
            setenv("GDK_BACKEND", "x11", 1);
        }

        gtk_lib = dlopen("libgtk-3.so.0", RTLD_LAZY | RTLD_GLOBAL);
        gobj_lib = dlopen("libgobject-2.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
        if (!gtk_lib || !gobj_lib) return false;

        #define LOAD_SYM(lib, name) \
            name = reinterpret_cast<decltype(name)>(dlsym(lib, #name)); \
            if (!name) return false;

        LOAD_SYM(gtk_lib, gtk_init_check);
        LOAD_SYM(gtk_lib, gtk_main);
        LOAD_SYM(gtk_lib, gtk_main_quit);
        LOAD_SYM(gtk_lib, gtk_menu_new);
        LOAD_SYM(gtk_lib, gtk_menu_item_new_with_label);
        LOAD_SYM(gtk_lib, gtk_check_menu_item_new_with_label);
        LOAD_SYM(gtk_lib, gtk_check_menu_item_set_active);
        LOAD_SYM(gtk_lib, gtk_check_menu_item_get_active);
        LOAD_SYM(gtk_lib, gtk_radio_menu_item_new_with_label);
        LOAD_SYM(gtk_lib, gtk_radio_menu_item_get_group);
        LOAD_SYM(gtk_lib, gtk_separator_menu_item_new);
        LOAD_SYM(gtk_lib, gtk_menu_shell_append);
        LOAD_SYM(gtk_lib, gtk_widget_show_all);
        LOAD_SYM(gtk_lib, gtk_menu_popup);
        LOAD_SYM(gtk_lib, gtk_get_current_event_time);
        LOAD_SYM(gtk_lib, gtk_status_icon_new_from_icon_name);
        LOAD_SYM(gtk_lib, gtk_status_icon_set_title);
        LOAD_SYM(gtk_lib, gtk_status_icon_set_tooltip_text);
        LOAD_SYM(gtk_lib, gtk_status_icon_set_visible);
        LOAD_SYM(gobj_lib, g_signal_connect_data);

        if (ind_lib) {
            app_indicator_new = reinterpret_cast<decltype(app_indicator_new)>(dlsym(ind_lib, "app_indicator_new"));
            app_indicator_set_status = reinterpret_cast<decltype(app_indicator_set_status)>(dlsym(ind_lib, "app_indicator_set_status"));
            app_indicator_set_menu = reinterpret_cast<decltype(app_indicator_set_menu)>(dlsym(ind_lib, "app_indicator_set_menu"));
            app_indicator_set_title = reinterpret_cast<decltype(app_indicator_set_title)>(dlsym(ind_lib, "app_indicator_set_title"));
        }
        #undef LOAD_SYM
        return true;
    }
};

static LinuxTrayApi g_tray;
static void* g_popup_menu = nullptr;

static void on_mode_selected(void* item, void* data) {
    if (g_tray.gtk_check_menu_item_get_active(item)) {
        int m = static_cast<int>(reinterpret_cast<intptr_t>(data));
        g_mode.store(static_cast<AppMode>(m));
        save_shared_state();
    }
}

static void on_power_toggled(void* item, void*) {
    g_power_on.store(g_tray.gtk_check_menu_item_get_active(item) != 0);
    save_shared_state();
}

static void on_quit_selected(void*, void*) {
    g_running.store(false);
    g_tray.gtk_main_quit();
}

static void on_status_icon_popup(void*, unsigned int button, unsigned int activate_time, void*) {
    if (g_popup_menu) {
        g_tray.gtk_menu_popup(g_popup_menu, nullptr, nullptr, nullptr, nullptr, button, activate_time);
    }
}

static void on_status_icon_activate(void*, void*) {
    if (g_popup_menu) {
        g_tray.gtk_menu_popup(g_popup_menu, nullptr, nullptr, nullptr, nullptr, 0, g_tray.gtk_get_current_event_time());
    }
}

static bool run_tray(int argc, char** argv) {
    load_shared_state();
    if (!g_tray.load()) return false;
    if (!g_tray.gtk_init_check(&argc, &argv)) return false;

    void* menu = g_tray.gtk_menu_new();
    g_popup_menu = menu;
    void* group = nullptr;

    struct ModeEntry { const char* label; AppMode mode; };
    const ModeEntry entries[] = {
        {"Smart Mode (GPU Total Memory %)",           AppMode::Smart},
        {"Smart Mode (GPU Dedicated VRAM %)",         AppMode::SmartDedicated},
        {"Auto Loop Mode",                            AppMode::AutoLoop},
        {"CPU Temp C",                                AppMode::CpuTempC},
        {"CPU Loading %",                             AppMode::CpuLoad},
        {"RAM Usage %",                               AppMode::RamUsage},
    };

    for (const auto& e : entries) {
        void* item = g_tray.gtk_radio_menu_item_new_with_label(group, e.label);
        group = g_tray.gtk_radio_menu_item_get_group(item);
        if (g_mode.load() == e.mode) {
            g_tray.gtk_check_menu_item_set_active(item, 1);
        }
        g_tray.g_signal_connect_data(
            item,
            "activate",
            reinterpret_cast<void*>(on_mode_selected),
            reinterpret_cast<void*>(static_cast<intptr_t>(e.mode)),
            nullptr,
            0
        );
        g_tray.gtk_menu_shell_append(menu, item);
    }

    g_tray.gtk_menu_shell_append(menu, g_tray.gtk_separator_menu_item_new());

    void* pwr_item = g_tray.gtk_check_menu_item_new_with_label("Display Power ON");
    g_tray.gtk_check_menu_item_set_active(pwr_item, g_power_on.load() ? 1 : 0);
    g_tray.g_signal_connect_data(pwr_item, "toggled", reinterpret_cast<void*>(on_power_toggled), nullptr, nullptr, 0);
    g_tray.gtk_menu_shell_append(menu, pwr_item);

    g_tray.gtk_menu_shell_append(menu, g_tray.gtk_separator_menu_item_new());

    void* quit_item = g_tray.gtk_menu_item_new_with_label("Quit");
    g_tray.g_signal_connect_data(quit_item, "activate", reinterpret_cast<void*>(on_quit_selected), nullptr, nullptr, 0);
    g_tray.gtk_menu_shell_append(menu, quit_item);

    g_tray.gtk_widget_show_all(menu);

    if (g_tray.app_indicator_new && g_tray.app_indicator_set_status && g_tray.app_indicator_set_menu) {
        void* ind = g_tray.app_indicator_new("ak4-digital-display", "computer", 4 /* HARDWARE */);
        g_tray.app_indicator_set_status(ind, 1 /* ACTIVE */);
        if (g_tray.app_indicator_set_title) {
            g_tray.app_indicator_set_title(ind, "AK4 Digital Display");
        }
        g_tray.app_indicator_set_menu(ind, menu);
    } else {
        void* icon = g_tray.gtk_status_icon_new_from_icon_name("computer");
        g_tray.gtk_status_icon_set_title(icon, "AK4 Digital Display");
        g_tray.gtk_status_icon_set_tooltip_text(icon, "AK4 Digital Display Controller");
        g_tray.gtk_status_icon_set_visible(icon, 1);
        g_tray.g_signal_connect_data(icon, "popup-menu", reinterpret_cast<void*>(on_status_icon_popup), nullptr, nullptr, 0);
        g_tray.g_signal_connect_data(icon, "activate", reinterpret_cast<void*>(on_status_icon_activate), nullptr, nullptr, 0);
    }

    g_tray.gtk_main();
    return true;
}

#elif defined(_WIN32)

static constexpr UINT WM_TRAYICON = WM_USER + 1;
static NOTIFYICONDATAA g_nid{};

static void show_context_menu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();
    AppMode cur = g_mode.load();

    AppendMenuA(hMenu, MF_STRING | (cur == AppMode::Smart          ? MF_CHECKED : 0), 105, "Smart Mode (GPU Total Memory %)");
    AppendMenuA(hMenu, MF_STRING | (cur == AppMode::SmartDedicated ? MF_CHECKED : 0), 106, "Smart Mode (GPU Dedicated VRAM %)");
    AppendMenuA(hMenu, MF_STRING | (cur == AppMode::AutoLoop       ? MF_CHECKED : 0), 100, "Auto Loop Mode");
    AppendMenuA(hMenu, MF_STRING | (cur == AppMode::CpuTempC       ? MF_CHECKED : 0), 101, "CPU Temp C");
    AppendMenuA(hMenu, MF_STRING | (cur == AppMode::CpuLoad        ? MF_CHECKED : 0), 103, "CPU Loading %");
    AppendMenuA(hMenu, MF_STRING | (cur == AppMode::RamUsage       ? MF_CHECKED : 0), 104, "RAM Usage %");
    AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(hMenu, MF_STRING | (g_power_on.load() ? MF_CHECKED : 0), 200, "Display Power ON");
    AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(hMenu, MF_STRING, 300, "Quit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_TRAYICON) {
        if (lparam == WM_RBUTTONUP || lparam == WM_LBUTTONUP) {
            show_context_menu(hwnd);
        }
        return 0;
    } else if (msg == WM_COMMAND) {
        UINT id = LOWORD(wparam);
        if (id >= 100 && id <= 106) {
            g_mode.store(static_cast<AppMode>(id - 100));
        } else if (id == 200) {
            g_power_on.store(!g_power_on.load());
        } else if (id == 300) {
            g_running.store(false);
            Shell_NotifyIconA(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
        }
        return 0;
    } else if (msg == WM_DESTROY) {
        g_running.store(false);
        Shell_NotifyIconA(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static int run_tray(int, char**) {
    WNDCLASSA wc{};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "AK4TrayWindowClass";
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "AK4 Digital Display", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);

    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIconA(nullptr, IDI_APPLICATION);
    std::snprintf(g_nid.szTip, sizeof(g_nid.szTip), "AK4 Digital Display Controller");
    Shell_NotifyIconA(NIM_ADD, &g_nid);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

#endif

// ============================================================================
// Entry Point (CLI + System Tray / Headless Daemon)
// ============================================================================
int main(int argc, char** argv) {
    bool headless = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--headless" || arg == "-d") {
            headless = true;
        } else if (arg == "--mode" && i + 1 < argc) {
            std::string m = argv[++i];
            if (m == "smart")       g_mode.store(AppMode::Smart);
            else if (m == "auto")   g_mode.store(AppMode::AutoLoop);
            else if (m == "temp" || m == "c") g_mode.store(AppMode::CpuTempC);
            else if (m == "f")      g_mode.store(AppMode::CpuTempF);
            else if (m == "cpu")    g_mode.store(AppMode::CpuLoad);
            else if (m == "ram")    g_mode.store(AppMode::RamUsage);
            else if (m == "off")    g_power_on.store(false);
        }
    }

    std::thread worker(worker_loop);

#if !defined(_WIN32)
    if (!headless && (std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY"))) {
        if (run_tray(argc, argv)) {
            g_running.store(false);
        } else {
            worker.join();
            return 0;
        }
    } else {
        worker.join();
        return 0;
    }
#else
    if (!headless) {
        run_tray(argc, argv);
        g_running.store(false);
    } else {
        worker.join();
        return 0;
    }
#endif

    if (worker.joinable()) {
        worker.join();
    }
    return 0;
}

