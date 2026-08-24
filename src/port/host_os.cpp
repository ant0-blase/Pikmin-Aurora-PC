#include "Dolphin/os.h"
#include "Dolphin/hio.h"
#include "Dolphin/vi.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {
struct ThreadState {
    OSThreadStartFunction fn{};
    void* arg{};
    void* result{};
    OSPriority priority{};
    std::thread thread;
    bool started{};
    bool finished{};
};
struct QueueState {
    std::mutex mutex;
    std::condition_variable can_read;
    std::condition_variable can_write;
    std::deque<OSMessage> values;
    size_t capacity{1};
};
struct MutexState { std::recursive_mutex mutex; };
struct CondState { std::condition_variable_any cv; };
struct SleepState { std::mutex mutex; std::condition_variable cv; uint64_t generation{}; };

std::mutex g_state_mutex;
std::unordered_map<OSThread*, std::shared_ptr<ThreadState>> g_threads;
std::unordered_map<OSMessageQueue*, std::shared_ptr<QueueState>> g_queues;
std::unordered_map<OSMutex*, std::shared_ptr<MutexState>> g_mutexes;
std::unordered_map<OSCond*, std::shared_ptr<CondState>> g_conds;
std::unordered_map<OSThreadQueue*, std::shared_ptr<SleepState>> g_sleeps;
OSThread g_main_thread{};
thread_local OSThread* g_current_thread = &g_main_thread;
thread_local OSContext* g_current_context = nullptr;

std::shared_ptr<QueueState> queue_state(OSMessageQueue* q) {
    std::lock_guard lock(g_state_mutex);
    auto& p = g_queues[q]; if (!p) p = std::make_shared<QueueState>(); return p;
}
std::shared_ptr<MutexState> mutex_state(OSMutex* m) {
    std::lock_guard lock(g_state_mutex);
    auto& p = g_mutexes[m]; if (!p) p = std::make_shared<MutexState>(); return p;
}
std::shared_ptr<CondState> cond_state(OSCond* c) {
    std::lock_guard lock(g_state_mutex);
    auto& p = g_conds[c]; if (!p) p = std::make_shared<CondState>(); return p;
}
std::shared_ptr<SleepState> sleep_state(OSThreadQueue* q) {
    std::lock_guard lock(g_state_mutex);
    auto& p = g_sleeps[q]; if (!p) p = std::make_shared<SleepState>(); return p;
}

std::atomic<u32> g_retrace{0};
std::atomic<u32> g_retrace_dispatched{0};
std::atomic<VIRetraceCallback> g_retrace_callback{nullptr};
std::once_flag g_vi_once;
void start_vi_clock() {
    std::call_once(g_vi_once, [] {
        std::thread([] {
            using clock = std::chrono::steady_clock;
            auto next = clock::now();
            constexpr auto period = std::chrono::nanoseconds(16683350); // ~59.94 Hz
            for (;;) {
                next += period;
                std::this_thread::sleep_until(next);
                // Only advance the host VI clock here. GameCube retrace callbacks
                // are not ordinary worker-thread callbacks; invoking Pikmin's
                // retraceProc from this detached std::thread races gsys, the DVD
                // queues, and section transitions. Pending callbacks are dispatched
                // explicitly by VIHostPumpCallbacks() on the main thread instead.
                ++g_retrace;
            }
        }).detach();
    });
}
}

extern "C" {
void OSReport(const char* message, ...)
{
    if (!message) return;
    va_list args;
    va_start(args, message);
    std::vfprintf(stderr, message, args);
    va_end(args);
    std::fflush(stderr);
}

void OSPanic(const char* file, int line, const char* message, ...)
{
    std::fprintf(stderr, "\n[pikmin::panic] ");
    if (message) {
        va_list args;
        va_start(args, message);
        std::vfprintf(stderr, message, args);
        va_end(args);
    }
    std::fprintf(stderr, " in \"%s\" on line %d.\n", file ? file : "<unknown>", line);
    std::fflush(stderr);
    std::abort();
}

void DCInvalidateRange(void*, u32) {}
void DCFlushRange(void*, u32) {}
void DCStoreRange(void*, u32) {}
void DCFlushRangeNoSync(void*, u32) {}
void DCStoreRangeNoSync(void*, u32) {}
void DCZeroRange(void* p, u32 n) { if (p) std::memset(p, 0, n); }

BOOL OSDisableInterrupts() { return TRUE; }
BOOL OSRestoreInterrupts(BOOL old) { return old; }
BOOL OSEnableInterrupts() { return TRUE; }

void OSClearContext(OSContext* c) { if (c) std::memset(c, 0, sizeof(*c)); }
OSContext* OSGetCurrentContext() { return g_current_context; }
void OSSetCurrentContext(OSContext* c) { g_current_context = c; }
u32 OSGetStackPointer() { return 0; }

void OSInitThreadQueue(OSThreadQueue* q) { if (q) { q->head = nullptr; q->tail = nullptr; } (void)sleep_state(q); }
OSThread* OSGetCurrentThread() { return g_current_thread; }
BOOL OSIsThreadTerminated(OSThread* t) {
    std::lock_guard lock(g_state_mutex); auto it=g_threads.find(t); return it!=g_threads.end() && it->second->finished;
}
s32 OSDisableScheduler() { return 0; }
s32 OSEnableScheduler() { return 0; }
void OSYieldThread() { std::this_thread::yield(); }
BOOL OSCreateThread(OSThread* t, OSThreadStartFunction fn, void* arg, void*, u32, OSPriority prio, u16) {
    if (!t || !fn) return FALSE;
    auto st=std::make_shared<ThreadState>(); st->fn=fn; st->arg=arg; st->priority=prio;
    std::lock_guard lock(g_state_mutex); g_threads[t]=st; return TRUE;
}
s32 OSResumeThread(OSThread* t) {
    std::shared_ptr<ThreadState> st; { std::lock_guard lock(g_state_mutex); auto it=g_threads.find(t); if(it==g_threads.end()) return 0; st=it->second; }
    if (!st->started) { st->started=true; st->thread=std::thread([t,st]{ g_current_thread=t; st->result=st->fn(st->arg); st->finished=true; }); }
    return 0;
}
s32 OSSuspendThread(OSThread*) { return 0; }
void OSCancelThread(OSThread* t) { std::lock_guard lock(g_state_mutex); auto it=g_threads.find(t); if(it!=g_threads.end() && it->second->thread.joinable()) it->second->thread.detach(); }
void OSDetachThread(OSThread* t) { std::lock_guard lock(g_state_mutex); auto it=g_threads.find(t); if(it!=g_threads.end() && it->second->thread.joinable()) it->second->thread.detach(); }
BOOL OSJoinThread(OSThread* t, void** out) {
    std::shared_ptr<ThreadState> st; { std::lock_guard lock(g_state_mutex); auto it=g_threads.find(t); if(it==g_threads.end()) return FALSE; st=it->second; }
    if(st->thread.joinable()) st->thread.join(); if(out) *out=st->result; return TRUE;
}
void OSExitThread(void*) { std::terminate(); }
OSPriority OSGetThreadPriority(OSThread* t) { std::lock_guard lock(g_state_mutex); auto it=g_threads.find(t); return it==g_threads.end()?0:it->second->priority; }
BOOL OSIsThreadSuspended(OSThread*) { return FALSE; }
BOOL OSSetThreadPriority(OSThread*, OSPriority) { return TRUE; }
void OSSleepThread(OSThreadQueue* q) { auto st=sleep_state(q); std::unique_lock l(st->mutex); auto g=st->generation; st->cv.wait(l,[&]{return st->generation!=g;}); }
void OSWakeupThread(OSThreadQueue* q) { auto st=sleep_state(q); {std::lock_guard l(st->mutex); ++st->generation;} st->cv.notify_all(); }
s32 OSCheckActiveThreads() { std::lock_guard lock(g_state_mutex); return static_cast<s32>(g_threads.size()+1); }

void OSInitMessageQueue(OSMessageQueue* q, OSMessage*, s32 count) { auto st=queue_state(q); std::lock_guard l(st->mutex); st->values.clear(); st->capacity=count>0?static_cast<size_t>(count):1; }
BOOL OSSendMessage(OSMessageQueue* q, OSMessage msg, s32 flags) { auto st=queue_state(q); std::unique_lock l(st->mutex); if(flags==OS_MESSAGE_BLOCK) st->can_write.wait(l,[&]{return st->values.size()<st->capacity;}); else if(st->values.size()>=st->capacity) return FALSE; st->values.push_back(msg); l.unlock(); st->can_read.notify_one(); return TRUE; }
BOOL OSJamMessage(OSMessageQueue* q, OSMessage msg, s32 flags) { auto st=queue_state(q); std::unique_lock l(st->mutex); if(flags==OS_MESSAGE_BLOCK) st->can_write.wait(l,[&]{return st->values.size()<st->capacity;}); else if(st->values.size()>=st->capacity) return FALSE; st->values.push_front(msg); l.unlock(); st->can_read.notify_one(); return TRUE; }
BOOL OSReceiveMessage(OSMessageQueue* q, OSMessage* out, s32 flags) { auto st=queue_state(q); std::unique_lock l(st->mutex); if(flags==OS_MESSAGE_BLOCK) st->can_read.wait(l,[&]{return !st->values.empty();}); else if(st->values.empty()) return FALSE; auto m=st->values.front(); st->values.pop_front(); if(out)*out=m; l.unlock(); st->can_write.notify_one(); return TRUE; }

void OSInitMutex(OSMutex* m) { (void)mutex_state(m); }
void OSLockMutex(OSMutex* m) { mutex_state(m)->mutex.lock(); }
void OSUnlockMutex(OSMutex* m) { mutex_state(m)->mutex.unlock(); }
BOOL OSTryLockMutex(OSMutex* m) { return mutex_state(m)->mutex.try_lock()?TRUE:FALSE; }
void OSInitCond(OSCond* c) { (void)cond_state(c); }
void OSWaitCond(OSCond* c, OSMutex* m) { auto ms=mutex_state(m); auto cs=cond_state(c); std::unique_lock<std::recursive_mutex> l(ms->mutex, std::adopt_lock); cs->cv.wait(l); l.release(); }
void OSSignalCond(OSCond* c) { cond_state(c)->cv.notify_all(); }

BOOL OSGetResetSwitchState() { return FALSE; }
void OSResetSystem(int, u32, BOOL) { std::exit(0); }
u32 OSGetSoundMode() { return 1; }
void OSSetSoundMode(u32) {}
u32 OSGetProgressiveMode() { return 1; }
void OSSetProgressiveMode(u32) {}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
    start_vi_clock();
    g_retrace_dispatched.store(g_retrace.load());
    return g_retrace_callback.exchange(cb);
}
u32 VIGetRetraceCount() { start_vi_clock(); return g_retrace.load(); }
void VIHostPumpCallbacks() {
    start_vi_clock();
    const u32 now = g_retrace.load();
    u32 dispatched = g_retrace_dispatched.load();
#if defined(TARGET_PC)
    static u32 hostPumpTraceCount = 0;
    const bool tracePump = hostPumpTraceCount < 8;
    if (tracePump) OSReport("[pikmin::vi] pump now=%u dispatched=%u pending=%u cb=%p\n", now, dispatched, now - dispatched, reinterpret_cast<void*>(g_retrace_callback.load()));
#endif
    if (dispatched >= now) {
#if defined(TARGET_PC)
        ++hostPumpTraceCount;
#endif
        return;
    }

    if (auto cb = g_retrace_callback.load()) {
        while (dispatched < now) {
            ++dispatched;
            cb(dispatched);
        }
    } else {
        dispatched = now;
    }
    g_retrace_dispatched.store(dispatched);
#if defined(TARGET_PC)
    if (tracePump) OSReport("[pikmin::vi] pump complete dispatched=%u\n", dispatched);
    ++hostPumpTraceCount;
#endif
}
void VIWaitForRetrace() {
    start_vi_clock();
    const u32 n = g_retrace.load();
    while (g_retrace.load() == n) std::this_thread::sleep_for(std::chrono::microseconds(250));
    // All TARGET_PC callers that remain after the loading-thread fix are main-thread
    // callers, so dispatch the retrace synchronously before returning.
    VIHostPumpCallbacks();
}
void VISetNextFrameBuffer(void*) {}
void VISetBlack(BOOL) {}
u32 VIGetDTVStatus(void) { return 0; }

BOOL HIOEnumDevices(HIOEnumCallback) { return FALSE; }
BOOL HIOInit(s32, HIOCallback) { return FALSE; }
BOOL HIOReadMailbox(u32*) { return FALSE; }
BOOL HIOWriteMailbox(u32) { return TRUE; }
BOOL HIORead(u32, void*, s32) { return FALSE; }
BOOL HIOWrite(u32, void*, s32) { return FALSE; }
BOOL HIOReadAsync(u32, void*, s32, HIOCallback) { return FALSE; }
BOOL HIOWriteAsync(u32, void*, s32, HIOCallback) { return FALSE; }
BOOL HIOReadStatus(u32*) { return FALSE; }
}
