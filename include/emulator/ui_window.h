#pragma once
#ifdef _WIN32
#include <windows.h>
#else
#include "emulator/sdl_compat.h"
#endif
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <cstddef>
#include <cstdint>
#include "emulator/vm.h"
#include "emulator/input_manager.h"
#include "emulator/phone_profile.h"

#ifdef _WIN32
struct ID3D11DeviceContext;
#endif

namespace emu {

// A still frame of a phone, kept small and optionally saved to disk.
struct PhonePreview {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    std::chrono::steady_clock::time_point takenAt{};
    bool valid() const {
        return w > 0 && h > 0 && rgba.size() == (size_t)w * (size_t)h * 4;
    }
};

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool create(HINSTANCE hInst, int nCmdShow);
    void runMessageLoop();
    void setVm(IVirtualMachine* vm);
    // Test-only helpers so UI behavior can be verified without relying on
    // synthetic mouse coordinates. These are harmless in the shipping app.
    void debugToggleAntiAfk();
    void debugToggleKeepAwake();
    void debugOpenSettings();
    void debugCloseSettings();
    void debugOpenFirstPhone();
    void debugLeaveEmulator();
    void debugStopAll();
    void debugExitAll();
    void debugToggleListView();
    void debugCycleFilter();
    void debugOpenSettingsTab(int tab);
    void debugToggleFontSize();
    void debugOpenApkPicker(const std::string& path);
    void debugInstallTestApk(const std::string& path);
    IVirtualMachine* vm() const { return vm_; }
    HWND hwnd() const { return hwnd_; }
    RECT phoneRect() const { return phoneRect_; }

private:
    enum class UiMode { Manager, Emulator, Settings };
    enum class PhoneStatus { Stopped, Starting, Running, Parked, Paused, Error };

    struct Renderer;
    struct PhoneTexture;

    struct ResourceSample {
        double cpuPercent = 0.0;
        size_t ramBytes = 0;
        uint64_t cpuTime = 0;
        std::chrono::steady_clock::time_point takenAt{};
    };

    struct CardHit {
        int id = 0;
        RECT card{};
        RECT star{};
        RECT menu{};
    };

    struct ApkInstallDevice {
        int phoneId = 0;
        std::string name;
        size_t bytesDone = 0;
        size_t bytesTotal = 0;
        std::string stage;
        bool done = false;
        bool ok = false;
        std::atomic<bool> cancel{ false };
    };

    struct ApkInstallJob {
        std::string apkPath;
        std::string apkName;
        size_t bytesTotal = 0;
        std::vector<std::shared_ptr<ApkInstallDevice>> devices;
        mutable std::mutex mutex;
        bool finished = false;
        std::chrono::steady_clock::time_point finishedAt{};
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(HWND, UINT, WPARAM, LPARAM);

    bool initRenderer();
    void shutdownRenderer();
    void resizeRenderer();
    bool createPostShaders();
    bool ensurePostTargets(UINT width, UINT height);
    void releasePostTargets();
    void updatePostConstants(UINT width, UINT height);
    void renderPostPass();
    void renderFrame();
    void update();

    void drawManager();
    void drawSettings();
    void drawEmulator();
    void drawWindowControls();
    void drawDialogs();
    void drawCard(int id, float x, float y, float w, float h);
    void drawPhoneRow(int id, float x, float y, float w, float h);
    void drawApkPicker();
    void drawApkProgress();

    void ensurePhoneTexture(int id, int w, int h);
    void updatePhoneTexture(int id, const uint8_t* rgba, int w, int h);
#ifdef _WIN32
    void releaseTexture(PhoneTexture* tex, ID3D11DeviceContext* ctx);
#else
    void releaseTexture(PhoneTexture* tex, void* ctx);
#endif

    void addPhone(const std::string& name, const std::string& templateName = "");
    IVirtualMachine* makeVmForProfile(PhoneProfile* profile, bool attachExisting);
    void reattachRunningPhones();
    void startPhone(int id);
    void openEmulator(int id);
    void leaveEmulator();
    void closePhone(int id);
    void closeAllPhones();
    void detachAllPhones();
    void exitEverything();
    void installTrayIcon();
    void removeTrayIcon();
    void hideToTray();
    void restoreFromTray();
    void openSettings();
    void closeSettings();
    void toggleFullscreen();
    void applyWindowCorners();
    RECT headerDragRect() const;
    void handleCardAction(int id, int action);
    void backupPhone(int id);
    void backupPhones(const std::vector<int>& ids);
    void deletePhones(const std::vector<int>& ids);
    PhoneProfile* findProfile(int id);
    std::vector<int> orderedProfileIds() const;
    PhoneStatus phoneStatus(int id) const;
    bool phoneMatchesFilters(int id) const;
    void sampleResources();
    void loadGlobalSettingsFromConfig();
    void syncGlobalSettingsToConfig() const;
    void applyPerformancePreset(const std::string& name);
    void applyParkedAudio() const;
    void applyThemeFromSettings();
    void rebuildFonts();
    void chooseApkFile();
    void openApkPicker(const std::string& path);
    void startApkInstall();
    void updateApkJobs();
    void cancelAllApkJobs();
    bool apkPanelVisible() const;
    void startUpdateCheck(bool manual);
    void persist() const;
    bool modalOpen() const;

    std::string previewPath(const PhoneProfile& p) const;
    void loadPreviews();
    bool capturePreview(int id, bool saveToDisk);
    void refreshPreviews(bool force);
    void removePreview(int id);

    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    IVirtualMachine* vm_ = nullptr;
    std::unordered_map<int, std::unique_ptr<IVirtualMachine>> vms_;
    std::unique_ptr<InputManager> input_;
    std::unique_ptr<Renderer> renderer_;
    std::unordered_map<int, std::unique_ptr<PhoneTexture>> textures_;

    RECT phoneRect_{};
    int winW_ = 1280, winH_ = 800;
    bool fullscreen_ = false;
    RECT savedRect_{};
    bool draggingWindow_ = false;
    POINT dragCursorOrigin_{};
    POINT dragWindowOrigin_{};

    UiMode mode_ = UiMode::Manager;
    UiMode settingsReturnMode_ = UiMode::Manager;
    int currentId_ = 0;
    ManagerState state_;
    std::string statePath_;

    std::unordered_map<int, PhonePreview> previews_;

    std::vector<CardHit> cardHits_;
    int menuPhoneId_ = 0;
    int menuX_ = 0, menuY_ = 0;
    bool menuRequested_ = false;

    // Manager search/filter/selection state.
    char searchBuffer_[128] = {};
    bool selectionMode_ = false;
    std::vector<int> selectedIds_;
    std::vector<int> deleteIds_;
    int templateIndex_ = 0;
    int instanceEditId_ = 0;
    bool fontRebuildPending_ = false;
    std::unordered_map<int, ResourceSample> resourceSamples_;
    std::chrono::steady_clock::time_point lastResourceSample_{};

    // Real APK install state.
    bool apkPickerOpen_ = false;
    std::chrono::steady_clock::time_point apkPickerOpenedAt_{};
    std::chrono::steady_clock::time_point menuOpenedAt_{};
    bool apkAllPhones_ = true;
    char apkSearchBuffer_[128] = {};
    std::string apkPath_;
    std::string apkName_;
    size_t apkSize_ = 0;
    std::vector<int> apkSelectedIds_;
    std::vector<std::shared_ptr<ApkInstallJob>> apkJobs_;
    std::vector<std::thread> apkThreads_;

    bool naming_ = false;
    bool nameFocus_ = false;
    int namingId_ = 0; // 0 = new phone, otherwise the phone being renamed
    char nameBuffer_[128] = {};
    bool confirmDelete_ = false;
    int deleteId_ = 0;
    bool showMessageModal_ = false;
    std::string modalTitle_;
    std::string modalMessage_;
    bool showFirstRunPrompt_ = false;
    std::string setupBanner_;

    // GitHub auto-updater (silent startup check + manual Settings button).
    bool updateCheckStarted_ = false;
    bool updateCheckManual_ = false;
    std::atomic<bool> updateCheckRunning_{ false };

    bool trayIcon_ = false;
    bool phoneHovered_ = false;
    bool touchActive_ = false;
    double fps_ = 0.0;
    uint64_t lastFrame_ = 0;
    uint64_t lastFpsFrames_ = 0;
    std::chrono::steady_clock::time_point lastFpsSample_;
    std::chrono::steady_clock::time_point createdAt_;
    std::chrono::steady_clock::time_point lastInputAt_;
    std::chrono::steady_clock::time_point lastAfkAt_;
    std::chrono::steady_clock::time_point lastAwakeAt_;
    int afkStep_ = 0;
};

} // namespace emu
