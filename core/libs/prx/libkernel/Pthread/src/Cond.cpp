#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include "prx/libkernel/Pthread/include/Mutex.hpp"
#include "prx/libkernel/Pthread/include/Cond.hpp"
#include "prx/libkernel/Pthread/Posix/Common.hpp"
#include "prx/libkernel/Time/include/Time.hpp"
#include "prx/libkernel/Time/include/TimedWait.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <ProfileOutput.hpp>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr int sceTimedOut = static_cast<int>(0x8002003cu);
std::mutex condInitializationMutex;

// Local profiling: watch only conditions with a roughly 50 ms requested wait.
// Retain signal counts after discovery without logging every retry or wake.
struct CondProbe {
    std::uint64_t waits = 0, signals = 0, timeouts = 0;
    const void* signalCaller = nullptr;
    unsigned long signalThread = 0;
};
std::mutex condProbeMutex;
std::unordered_map<PthreadCond, CondProbe> condProbes;
bool probeEnabled() {
    static const bool enabled = std::getenv("APS5_TRACE_COND50") != nullptr || std::getenv("APS5_TRACE_COND_CALLER") != nullptr;
    return enabled;
}
std::uintptr_t probeCallerOffset(const void* caller) {
#ifdef _WIN32
    return reinterpret_cast<std::uintptr_t>(caller) - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
#else
    return reinterpret_cast<std::uintptr_t>(caller);
#endif
}
bool probeMatches(const void* caller, std::optional<std::uint64_t> deadline, std::uint64_t now) {
    static const auto target = [] {
        const auto* value = std::getenv("APS5_TRACE_COND_CALLER");
        return value ? std::strtoull(value, nullptr, 0) : 0ull;
    }();
    static const bool timed = std::getenv("APS5_TRACE_COND50") != nullptr;
    return (target && probeCallerOffset(caller) == target) ||
        (timed && deadline && *deadline >= now && *deadline - now >= 45000000ull && *deadline - now <= 55000000ull);
}
unsigned long probeThread() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return static_cast<unsigned long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}
void probeSignal(PthreadCond c, const void* caller, const char* kind) {
    if (!probeEnabled()) return;
    std::lock_guard lock(condProbeMutex);
    const auto found = condProbes.find(c);
    if (found == condProbes.end()) return;
    auto& p = found->second;
    ++p.signals;
    p.signalCaller = caller;
    p.signalThread = probeThread();
    if (p.signals <= 64u || p.signals % 100u == 0u) Diagnostics::ProfilePrint_nid_no_patch("[cond-probe] %s ns=%llu object=%p tid=%lu caller=%p exe_offset=0x%llx signals=%llu\n", kind, static_cast<unsigned long long>(TimedWait::NowNanos()), static_cast<void*>(c), p.signalThread, caller, static_cast<unsigned long long>(probeCallerOffset(caller)), static_cast<unsigned long long>(p.signals));
}
void probeWait(PthreadCond c, PthreadCond* slot, PthreadMutex* mutex, const void* caller, std::optional<std::uint64_t> deadline, bool ended, bool timeout) {
    if (!probeEnabled()) return;
    const auto now = TimedWait::NowNanos();
    std::lock_guard lock(condProbeMutex);
    auto found = condProbes.find(c);
    if (found == condProbes.end()) {
        if (ended || !probeMatches(caller, deadline, now)) return;
        found = condProbes.emplace(c, CondProbe{}).first;
    }
    auto& p = found->second;
    if (!ended) ++p.waits;
    else if (timeout) ++p.timeouts;
    if (p.waits <= 64u || p.waits % 100u == 0u) Diagnostics::ProfilePrint_nid_no_patch(
        "[cond-probe] %s ns=%llu object=%p slot=%p mutex=%p tid=%lu caller=%p waits=%llu timeouts=%llu signals=%llu last_signal_tid=%lu last_signal_caller=%p timeout=%d\n",
        ended ? "end" : "begin", static_cast<unsigned long long>(now), static_cast<void*>(c), static_cast<void*>(slot), static_cast<void*>(mutex), probeThread(), caller,
        static_cast<unsigned long long>(p.waits), static_cast<unsigned long long>(p.timeouts), static_cast<unsigned long long>(p.signals), p.signalThread, p.signalCaller, timeout);
}

PthreadCond destroyedCond() {
    return reinterpret_cast<PthreadCond>(std::uintptr_t{2});
}

PthreadCond resolveCond(PthreadCond* cond) {
    if (!cond)
        throw std::invalid_argument("Condition variable pointer is null");
    std::atomic_ref<PthreadCond> slot(*cond);
    if (const auto current = slot.load(std::memory_order_acquire); current && current != destroyedCond())
        return current;
    std::lock_guard lock(condInitializationMutex);
    const auto current = slot.load(std::memory_order_acquire);
    if (current == destroyedCond())
        throw std::runtime_error("Condition variable has been destroyed");
    if (current)
        return current;
    auto* created = new PthreadCondPrivate();
    slot.store(created, std::memory_order_release);
    return created;
}

PthreadMutex lockedMutex(PthreadMutex* mutex) {
    if (!mutex || !*mutex)
        throw std::invalid_argument("Mutex pointer is null");
    auto* current = *mutex;
    if (current->_owner.load(std::memory_order_acquire) != std::this_thread::get_id())
        throw std::runtime_error("Condition wait mutex is not owned by the current thread");
    return current;
}

int waitUntil(PthreadCond* cond, PthreadMutex* mutex, std::optional<std::uint64_t> deadlineNanos, const void* caller) {
    auto* c = resolveCond(cond);
    auto* m = lockedMutex(mutex);
    bool timedOut = false;
    const auto waitStart = std::chrono::steady_clock::now();
    probeWait(c, cond, mutex, caller, deadlineNanos, false, false);
    struct Trace {
        const void* caller; const bool& timedOut; std::chrono::steady_clock::time_point start;
        PthreadCond c; PthreadCond* cond; PthreadMutex* mutex;
        ~Trace() { KernelTraceWait_nid_postfix("cond", caller, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), timedOut); probeWait(c, cond, mutex, caller, std::nullopt, true, timedOut); }
    } trace{caller, timedOut, waitStart, c, cond, mutex};
    if (m->_type == MutexType::Recursive) {
        std::unique_lock<std::recursive_timed_mutex> lock(m->_rmtx, std::adopt_lock);
        const auto previousCount = m->_count;
        m->_count = 0;
        m->_owner.store(std::thread::id{}, std::memory_order_release);
        if (deadlineNanos) timedOut = !c->_cv.WaitUntil(lock, *deadlineNanos);
        else c->_cv.Wait(lock);
        m->_owner.store(std::this_thread::get_id(), std::memory_order_release);
        m->_count = previousCount;
        lock.release();
        return timedOut ? sceTimedOut : 0;
    }
    std::unique_lock<std::timed_mutex> lock(m->_mtx, std::adopt_lock);
    m->_owner.store(std::thread::id{}, std::memory_order_release);
    if (deadlineNanos) timedOut = !c->_cv.WaitUntil(lock, *deadlineNanos);
    else c->_cv.Wait(lock);
    m->_owner.store(std::this_thread::get_id(), std::memory_order_release);
    lock.release();
    return timedOut ? sceTimedOut : 0;
}

}

int CondOperations::AbsoluteTimedwait(PthreadCond* cond, PthreadMutex* mutex, const KernelTimespec* abstime) {
    KernelUseconds usec = 0;
    if (!PosixThread::RelativeMicroseconds(resolveCond(cond)->_clockid, abstime, &usec))
        throw std::invalid_argument("Invalid absolute condition variable timeout");
    return waitUntil(cond, mutex, TimedWait::DeadlineNanos(usec), __builtin_return_address(0));
}

extern "C" {

int APS5_VABI scePthreadCondattrInit(PthreadCondattr* attr) {
    if (!attr)
        throw std::invalid_argument("Condition attribute pointer is null");
    *attr = new PthreadCondattrPrivate{0};
    return 0;
}

int APS5_VABI scePthreadCondattrDestroy(PthreadCondattr* attr) {
    if (!attr || !*attr)
        throw std::invalid_argument("Condition attributes are not initialized");
    delete *attr;
    *attr = nullptr;
    return 0;
}

int APS5_VABI scePthreadCondattrSetclock(PthreadCondattr* attr, KernelClockid clockId) {
    if (!attr || !*attr)
        throw std::invalid_argument("Condition attributes are not initialized");
    (*attr)->_clockid = static_cast<int>(clockId);
    return 0;
}

int APS5_VABI scePthreadCondInit(PthreadCond* cond, const PthreadCondattr* attr, const char*) {
    if (!cond)
        throw std::invalid_argument("Condition variable pointer is null");
    if (attr && !*attr)
        throw std::invalid_argument("Condition attributes are not initialized");
    auto replacement = std::make_unique<PthreadCondPrivate>();
    if (attr)
        replacement->_clockid = (*attr)->_clockid;
    std::lock_guard lock(condInitializationMutex);
    std::atomic_ref<PthreadCond>(*cond).store(replacement.release(), std::memory_order_release);
    return 0;
}

int APS5_VABI scePthreadCondDestroy(PthreadCond* cond) {
    if (!cond)
        throw std::invalid_argument("Condition variable pointer is null");
    std::lock_guard lock(condInitializationMutex);
    if (*cond == destroyedCond())
        throw std::runtime_error("Condition variable has already been destroyed");
    if (probeEnabled()) {
        std::lock_guard probeLock(condProbeMutex);
        condProbes.erase(*cond);
    }
    delete *cond;
    std::atomic_ref<PthreadCond>(*cond).store(destroyedCond(), std::memory_order_release);
    return 0;
}

int APS5_VABI scePthreadCondSignal(PthreadCond* cond) {
    auto* c = resolveCond(cond);
    probeSignal(c, __builtin_return_address(0), "signal");
    c->_cv.NotifyOne();
    return 0;
}

int APS5_VABI scePthreadCondBroadcast(PthreadCond* cond) {
    auto* c = resolveCond(cond);
    probeSignal(c, __builtin_return_address(0), "broadcast");
    c->_cv.NotifyAll();
    return 0;
}

int APS5_VABI scePthreadCondSignalto(PthreadCond* cond, Pthread thread) {
    (void)thread;
    auto* c = resolveCond(cond);
    probeSignal(c, __builtin_return_address(0), "signalto");
    c->_cv.NotifyAll();
    return 0;
}

int APS5_VABI scePthreadCondWait(PthreadCond* cond, PthreadMutex* mutex) {
    return waitUntil(cond, mutex, std::nullopt, __builtin_return_address(0));
}

int APS5_VABI scePthreadCondTimedwait(PthreadCond* cond, PthreadMutex* mutex, KernelUseconds usec) {
    return waitUntil(cond, mutex, TimedWait::DeadlineNanos(usec), __builtin_return_address(0));
}

}
