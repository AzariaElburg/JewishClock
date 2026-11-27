// Code coverage of the simulators (2026-11-16): which of the clock's code the tests run, and how
// often. Only in a coverage build (sim/build.py / build_clock.py --coverage: llvm-mingw's clang,
// -fprofile-instr-generate, -DJC_COVERAGE); in a normal build these do nothing.
// The counts go to LLVM_PROFILE_FILE (the test runner sets one per test) every 10 s - a drill that
// kills the clock (power cut) loses at most that much - and before every _exit() (simCovFlush()).
// simCovDump(name): the counts so far to $JC_COV_DIR/<name>/<pid>.profraw, then from zero - how
// web_ui.py splits its one clock's counts by section (control port /coverage?name=...).
#include <string>
#include <cstdlib>

#ifdef JC_COVERAGE
#include <mutex>
#include <thread>
#include <chrono>
#include <filesystem>

extern "C" int  __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char*);
extern "C" void __llvm_profile_reset_counters(void);

static std::mutex& covLock() { static std::mutex m; return m; }
static std::string& covDefault() { static std::string s; return s; }

namespace {
struct CovStart {
    CovStart() {
        const char* f = std::getenv("LLVM_PROFILE_FILE");
        covDefault() = f ? f : "default-%p.profraw";
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                std::lock_guard<std::mutex> l(covLock());
                __llvm_profile_write_file();
            }
        }).detach();
    }
} s_covStart;
}

void simCovFlush() {
    std::lock_guard<std::mutex> l(covLock());
    __llvm_profile_write_file();
}

bool simCovDump(const std::string& name) {
    const char* dir = std::getenv("JC_COV_DIR");
    if (!dir || name.empty()) return false;
    std::lock_guard<std::mutex> l(covLock());
    std::error_code ec;   // the runtime makes the env file's folder, not one set here
    std::filesystem::create_directories(std::string(dir) + "/" + name, ec);
    std::string path = std::string(dir) + "/" + name + "/%p.profraw";
    __llvm_profile_set_filename(path.c_str());
    __llvm_profile_write_file();
    __llvm_profile_reset_counters();
    __llvm_profile_set_filename(covDefault().c_str());
    __llvm_profile_write_file();   // the default file from zero too (its earlier counts are in <name>'s)
    return true;
}
#else
void simCovFlush() {}
bool simCovDump(const std::string&) { return false; }
#endif
