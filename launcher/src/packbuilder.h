// Builds the HD texture pack from the user's own Data/textures.zip (same pipeline as
// tools/make-texture-pack.py): Real-ESRGAN x4 upscaling, rebuilt alpha, mech normal maps.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "platform.h"

class PackBuilder {
public:
    ~PackBuilder();
    void Start(const fs::path& gameDir);
    bool Running() const { return running_; }
    // 0..1, or -1 while the step has no measurable progress
    float Progress();
    std::string Status();
    bool TakeFinished(bool& ok, std::string& msg);  // true once after the build ends

private:
    void Run();
    void SetStatus(const std::string& s);
    bool EnsureUpscaler(fs::path& exe, std::string& err);

    fs::path gameDir_, work_;
    std::thread thread_;
    std::atomic<bool> running_{false}, finished_{false}, ok_{false};
    std::atomic<int> phase_{0};  // 0 idle, 1 download, 2 extract, 3 upscale, 4 finalize
    std::atomic<int> done_{0}, total_{0};
    std::mutex mutex_;
    std::string status_, result_;
    double lastCount_ = 0;
    int counted_ = 0;
};
