// ==================== memlens.h ====================
// MemLens — a small C++ memory profiler & leak detector.
//
// Save this as memlens.h and #include it before any other allocation
// happens in the translation unit you want profiled. It overrides
// global operator new/delete so it can record every allocation's
// size, call site, timestamp and thread, then emit a single
// memory_report.json that a separate dashboard (dashboard.html)
// renders.
//
// Fixed vs. the original prototype:
//   - real per-thread ids + a mutex around all shared state, so this
//     is now safe to use from a multi-threaded program
//   - double-free / invalid-free detection
//   - new[]/delete mismatch detection (new[] freed with plain delete
//     or vice versa)
//   - O(n log n) std::sort for the allocation-site ranking (was
//     bubble sort)
//   - the engine now only emits JSON; all HTML/CSS lives in
//     dashboard.html so the report can be redesigned without
//     recompiling anything
// =====================================================

#ifndef MEMLENS_H
#define MEMLENS_H

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ---- Threading compatibility shim ----
// Some MinGW distributions (the older mingw.org "mingw32" builds, as
// opposed to modern mingw-w64) ship a libstdc++ that was compiled
// without POSIX threading support. On those toolchains <mutex> and
// <thread> exist as files but declare nothing inside std:: at all —
// not a linker problem, a compile-time one. _GLIBCXX_HAS_GTHREADS is
// libstdc++'s own signal for "threading actually works here"; it's
// defined by <bits/c++config.h>, which every standard header above
// already pulled in, so it's reliable by this point. Non-libstdc++
// standard libraries (MSVC's, libc++) don't define this macro at all
// and virtually always support threading, so they take the "has
// threads" path unconditionally.
#if defined(__GLIBCXX__) && !defined(_GLIBCXX_HAS_GTHREADS)
  #define MEMLENS_HAS_STD_THREADS 0
#else
  #define MEMLENS_HAS_STD_THREADS 1
#endif

#if !MEMLENS_HAS_STD_THREADS
  #if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
  #else
    #error "MemLens: this toolchain's standard library lacks threading support " \
           "(std::mutex/std::thread aren't available) and isn't Windows, so no " \
           "fallback synchronization primitive is available either. Use a " \
           "toolchain with working <mutex>/<thread> support (e.g. a modern " \
           "GCC/Clang, or MSYS2's mingw-w64 on Windows instead of mingw.org's " \
           "mingw32 build)."
  #endif
#endif

// C++17 inline variables aren't implemented on some older compilers
// still in use (e.g. GCC < 7) even when -std=c++17 is passed. `static`
// at namespace scope gives internal linkage instead of the true
// one-definition-across-TUs behavior `inline` provides, but that
// distinction doesn't matter here: memlens.h is designed to be
// included by exactly one translation unit (the file being profiled).
#if __cplusplus >= 201703L
  #define MEMLENS_INLINE_VAR inline
#else
  #define MEMLENS_INLINE_VAR static
#endif

namespace memlens {

#if MEMLENS_HAS_STD_THREADS
  using MutexType = std::mutex;
  using LockGuardType = std::lock_guard<std::mutex>;
  using NativeThreadId = std::thread::id;
  inline NativeThreadId currentNativeThreadId() { return std::this_thread::get_id(); }
#else
  // Minimal native substitutes, used only on toolchains where
  // std::mutex/std::thread genuinely don't exist (see above).
  class MutexType {
    CRITICAL_SECTION cs_;
  public:
    MutexType() { InitializeCriticalSection(&cs_); }
    ~MutexType() { DeleteCriticalSection(&cs_); }
    MutexType(const MutexType&) = delete;
    MutexType& operator=(const MutexType&) = delete;
    void lock() { EnterCriticalSection(&cs_); }
    void unlock() { LeaveCriticalSection(&cs_); }
  };
  class LockGuardType {
    MutexType& m_;
  public:
    explicit LockGuardType(MutexType& m) : m_(m) { m_.lock(); }
    ~LockGuardType() { m_.unlock(); }
    LockGuardType(const LockGuardType&) = delete;
  };
  using NativeThreadId = unsigned long;
  inline NativeThreadId currentNativeThreadId() { return GetCurrentThreadId(); }
#endif

// Reentrancy guard. The global operator delete override below sees
// *every* delete in the whole process, including ones triggered by
// the profiler's own bookkeeping (its internal std::map/std::vector
// churn while generating a report, all while a lock is held). Without
// this guard, that internal churn would recurse back into
// recordDeallocation() and either self-deadlock on the non-recursive
// mutex, or mislabel the profiler's own scratch allocations as user
// double-frees. Any code path that touches the shared containers sets
// this before it starts, so nested/internal alloc traffic is passed
// straight through to malloc/free instead of being recorded.
MEMLENS_INLINE_VAR thread_local bool g_inMemLensCode = false;

struct ReentryGuard {
    bool previous;
    ReentryGuard() : previous(g_inMemLensCode) { g_inMemLensCode = true; }
    ~ReentryGuard() { g_inMemLensCode = previous; }
};

enum class AllocKind { Single, Array };

struct AllocationInfo {
    void* address = nullptr;
    size_t size = 0;
    std::string file = "unknown";
    int line = 0;
    std::string timestamp;
    int threadId = 0;
    bool freed = false;
    AllocKind kind = AllocKind::Single;
};

// A double-free or a new[]/delete mismatch on a pointer MemLens
// tracked. These are the same bug class ASan/valgrind flag as invalid
// frees — surfacing them is most of the value of a profiler like this.
struct FreeEvent {
    std::string type;   // "DoubleFree" | "AllocMismatch"
    void* address = nullptr;
    std::string detail;
    std::string timestamp;
    int threadId = 0;
};

class MemoryProfiler {
public:
    static MemoryProfiler& getInstance() {
        static MemoryProfiler instance;
        return instance;
    }

    void recordAllocation(void* ptr, size_t size, const char* file, int line, AllocKind kind) {
        if (g_inMemLensCode) return;
        ReentryGuard guard;
        LockGuardType lock(mutex_);

        AllocationInfo info;
        info.address = ptr;
        info.size = size;
        info.file = file ? file : "unknown";
        info.line = line;
        info.timestamp = currentTimestamp();
        info.threadId = threadIdFor(currentNativeThreadId());
        info.freed = false;
        info.kind = kind;

        allocations_[ptr] = info;
        totalAllocations_++;
        currentMemoryUsage_ += size;
        peakMemoryUsage_ = std::max(peakMemoryUsage_, currentMemoryUsage_);

        std::ostringstream site;
        site << info.file << ":" << line;
        allocationSites_[site.str()]++;
    }

    // Returns true if it's safe to actually call free() on ptr. A
    // detected double-free returns false: calling the real free() a
    // second time is itself undefined behavior and would typically
    // abort the process before MemLens ever gets to write a report,
    // which defeats the point of catching it. Skipping the second
    // free leaks that block, which is a far better outcome than a
    // crash for a profiling tool to produce.
    bool recordDeallocation(void* ptr, AllocKind kind) {
        if (!ptr) return true;
        if (g_inMemLensCode) return true;
        ReentryGuard guard;
        LockGuardType lock(mutex_);

        auto it = allocations_.find(ptr);
        if (it == allocations_.end()) {
            // Not one of ours. The global operator delete override below
            // sees every delete in the process, including ones for
            // memory that was never allocated through the tracked
            // `new(file, line)` path (STL/library internals, other
            // translation units, etc). That's normal and expected, not
            // a bug in the profiled code, so we don't flag it.
            return true;
        }

        if (it->second.freed) {
            logFreeEvent("DoubleFree", ptr, "pointer was already freed earlier");
            return false;
        }

        if (it->second.kind != kind) {
            std::string detail = it->second.kind == AllocKind::Array
                ? "allocated with new[] but freed with delete"
                : "allocated with new but freed with delete[]";
            logFreeEvent("AllocMismatch", ptr, detail);
            // still honor the free so accounting stays correct
        }

        currentMemoryUsage_ -= it->second.size;
        it->second.freed = true;
        totalDeallocations_++;
        return true;
    }

    void detectLeaks() {
        ReentryGuard guard;
        LockGuardType lock(mutex_);
        leakList_.clear();
        for (const auto& kv : allocations_) {
            if (!kv.second.freed) leakList_.push_back(kv.second);
        }
    }

    void printSummary() {
        detectLeaks();
        ReentryGuard guard;
        LockGuardType lock(mutex_);
        std::cout << "\n========================================\n";
        std::cout << "  MemLens — Memory Profiler Summary\n";
        std::cout << "========================================\n\n";
        std::cout << "Total Allocations:   " << totalAllocations_ << "\n";
        std::cout << "Total Deallocations: " << totalDeallocations_ << "\n";
        std::cout << "Current Usage:       " << (currentMemoryUsage_ / 1024.0) << " KB\n";
        std::cout << "Peak Usage:          " << (peakMemoryUsage_ / 1024.0) << " KB\n";
        std::cout << "Memory Leaks:        " << leakList_.size() << "\n";
        std::cout << "Invalid Frees:       " << freeEvents_.size() << "\n";
        std::cout << "========================================\n\n";
    }

    // The engine's only output. dashboard.html renders this.
    void generateJSONReport(const std::string& filename = "memory_report.json") {
        detectLeaks();
        ReentryGuard guard;
        LockGuardType lock(mutex_);

        std::vector<std::pair<std::string, size_t>> sites(allocationSites_.begin(), allocationSites_.end());
        std::sort(sites.begin(), sites.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        std::ofstream out(filename.c_str());
        out << "{\n";
        out << "  \"project\": \"MemLens\",\n";
        out << "  \"generatedAt\": \"" << currentTimestamp() << "\",\n";
        out << "  \"statistics\": {\n";
        out << "    \"totalAllocations\": " << totalAllocations_ << ",\n";
        out << "    \"totalDeallocations\": " << totalDeallocations_ << ",\n";
        out << "    \"currentMemoryUsage\": " << currentMemoryUsage_ << ",\n";
        out << "    \"peakMemoryUsage\": " << peakMemoryUsage_ << ",\n";
        out << "    \"leakCount\": " << leakList_.size() << ",\n";
        out << "    \"invalidFreeCount\": " << freeEvents_.size() << "\n";
        out << "  },\n";

        out << "  \"leaks\": [\n";
        for (size_t i = 0; i < leakList_.size(); i++) {
            const auto& leak = leakList_[i];
            out << "    {\n";
            out << "      \"address\": \"" << addressToString(leak.address) << "\",\n";
            out << "      \"size\": " << leak.size << ",\n";
            out << "      \"file\": \"" << jsonEscape(leak.file) << "\",\n";
            out << "      \"line\": " << leak.line << ",\n";
            out << "      \"timestamp\": \"" << leak.timestamp << "\",\n";
            out << "      \"threadId\": " << leak.threadId << ",\n";
            out << "      \"kind\": \"" << (leak.kind == AllocKind::Array ? "array" : "single") << "\"\n";
            out << "    }" << (i + 1 < leakList_.size() ? "," : "") << "\n";
        }
        out << "  ],\n";

        out << "  \"invalidFrees\": [\n";
        for (size_t i = 0; i < freeEvents_.size(); i++) {
            const auto& ev = freeEvents_[i];
            out << "    {\n";
            out << "      \"type\": \"" << ev.type << "\",\n";
            out << "      \"address\": \"" << addressToString(ev.address) << "\",\n";
            out << "      \"detail\": \"" << jsonEscape(ev.detail) << "\",\n";
            out << "      \"timestamp\": \"" << ev.timestamp << "\",\n";
            out << "      \"threadId\": " << ev.threadId << "\n";
            out << "    }" << (i + 1 < freeEvents_.size() ? "," : "") << "\n";
        }
        out << "  ],\n";

        out << "  \"allocationSites\": [\n";
        for (size_t i = 0; i < sites.size(); i++) {
            out << "    { \"location\": \"" << jsonEscape(sites[i].first) << "\", \"count\": " << sites[i].second << " }";
            out << (i + 1 < sites.size() ? "," : "") << "\n";
        }
        out << "  ],\n";

        // Every tracked allocation, freed or not — this is what the
        // dashboard's memory map view uses to show active/freed/leaked
        // blocks side by side, not just the leaked subset above.
        // Capped so a long-running program doesn't produce an
        // unbounded report; the statistics above always reflect the
        // true totals regardless of this cap.
        const size_t kMaxAllocationsInReport = 5000;
        out << "  \"allocations\": [\n";
        size_t emitted = 0;
        size_t total = allocations_.size();
        for (const auto& kv : allocations_) {
            if (emitted >= kMaxAllocationsInReport) break;
            const auto& a = kv.second;
            out << "    {\n";
            out << "      \"address\": \"" << addressToString(a.address) << "\",\n";
            out << "      \"size\": " << a.size << ",\n";
            out << "      \"file\": \"" << jsonEscape(a.file) << "\",\n";
            out << "      \"line\": " << a.line << ",\n";
            out << "      \"freed\": " << (a.freed ? "true" : "false") << ",\n";
            out << "      \"kind\": \"" << (a.kind == AllocKind::Array ? "array" : "single") << "\"\n";
            out << "    }" << (++emitted < std::min(total, kMaxAllocationsInReport) ? "," : "") << "\n";
        }
        out << "  ],\n";
        out << "  \"allocationsTruncated\": " << (total > kMaxAllocationsInReport ? "true" : "false") << "\n";
        out << "}\n";
        out.close();

        std::cout << "\nMemLens report written: " << filename << "\n";
        std::cout << "Open dashboard.html next to it (or drop the JSON onto the dashboard) to view it.\n\n";
    }

private:
    MemoryProfiler() = default;

    std::string currentTimestamp() {
        time_t now = time(nullptr);
        char buffer[32];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", localtime(&now));
        return std::string(buffer);
    }

    int threadIdFor(NativeThreadId id) {
        auto it = threadIdMap_.find(id);
        if (it != threadIdMap_.end()) return it->second;
        int assigned = static_cast<int>(threadIdMap_.size()) + 1;
        threadIdMap_[id] = assigned;
        return assigned;
    }

    void logFreeEvent(const char* type, void* ptr, const std::string& detail) {
        FreeEvent ev;
        ev.type = type;
        ev.address = ptr;
        ev.detail = detail;
        ev.timestamp = currentTimestamp();
        ev.threadId = threadIdFor(currentNativeThreadId());
        freeEvents_.push_back(ev);
        std::cerr << "[MemLens] " << type << " at " << addressToString(ptr) << ": " << detail << "\n";
    }

    static std::string addressToString(void* ptr) {
        std::ostringstream oss;
        oss << ptr;
        return oss.str();
    }

    static std::string jsonEscape(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (char c : in) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out;
    }

    MutexType mutex_;
    std::map<void*, AllocationInfo> allocations_;
    std::map<std::string, size_t> allocationSites_;
    std::map<NativeThreadId, int> threadIdMap_;
    std::vector<AllocationInfo> leakList_;
    std::vector<FreeEvent> freeEvents_;

    size_t totalAllocations_ = 0;
    size_t totalDeallocations_ = 0;
    size_t currentMemoryUsage_ = 0;
    size_t peakMemoryUsage_ = 0;
};

} // namespace memlens

// ---- global operator overrides ----
// The __FILE__/__LINE__ overloads are picked up by the `#define new`
// trick below, scoped to whatever translation unit includes this
// header. (Tracking allocations made inside the STL/other libraries
// needs a malloc/free hook — see the README's "what this doesn't
// catch yet" section.)

inline void* operator new(size_t size, const char* file, int line) {
    void* ptr = std::malloc(size);
    if (ptr) memlens::MemoryProfiler::getInstance().recordAllocation(ptr, size, file, line, memlens::AllocKind::Single);
    return ptr;
}

inline void* operator new[](size_t size, const char* file, int line) {
    void* ptr = std::malloc(size);
    if (ptr) memlens::MemoryProfiler::getInstance().recordAllocation(ptr, size, file, line, memlens::AllocKind::Array);
    return ptr;
}

// Matching placement-delete overloads: the compiler calls these only
// if the constructor throws during a `new(file, line) T(...)`
// expression, but they must exist or that code won't compile.
inline void operator delete(void* ptr, const char*, int) noexcept { std::free(ptr); }
inline void operator delete[](void* ptr, const char*, int) noexcept { std::free(ptr); }

// GCC/Clang default to C++14's sized-deallocation feature, which
// means a plain `delete p;` where the pointee's size is known at the
// call site resolves to operator delete(void*, size_t) — NOT
// operator delete(void*) — if a sized overload exists at all (a
// compiler-provided default one always implicitly does). Defining
// only the unsized overload, as the original prototype did, means it
// simply never gets called for ordinary object deletes: the compiler
// silently falls through to the default sized deallocation function
// instead, and every plain `delete` in the program goes untracked.
// Both the sized and unsized forms are defined here so real deletes
// are actually intercepted either way.
inline void operator delete(void* ptr) noexcept {
    if (ptr && memlens::MemoryProfiler::getInstance().recordDeallocation(ptr, memlens::AllocKind::Single)) {
        std::free(ptr);
    }
}
inline void operator delete(void* ptr, std::size_t) noexcept {
    if (ptr && memlens::MemoryProfiler::getInstance().recordDeallocation(ptr, memlens::AllocKind::Single)) {
        std::free(ptr);
    }
}

inline void operator delete[](void* ptr) noexcept {
    if (ptr && memlens::MemoryProfiler::getInstance().recordDeallocation(ptr, memlens::AllocKind::Array)) {
        std::free(ptr);
    }
}
inline void operator delete[](void* ptr, std::size_t) noexcept {
    if (ptr && memlens::MemoryProfiler::getInstance().recordDeallocation(ptr, memlens::AllocKind::Array)) {
        std::free(ptr);
    }
}

#define new new(__FILE__, __LINE__)

#endif // MEMLENS_H
