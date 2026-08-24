#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/main.h>
#undef main

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(__linux__)
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#endif

int pikmin_game_main(int argc, char* argv[]);

#if defined(__linux__)
extern "C" {
char g_pikmin_crash_context[768] = "no native crash context set";
volatile std::sig_atomic_t g_pikmin_crash_context_len = 27;

void pikmin_set_crash_context(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(g_pikmin_crash_context, sizeof(g_pikmin_crash_context), fmt, args);
    va_end(args);

    if (written < 0) {
        g_pikmin_crash_context[0] = '\0';
        g_pikmin_crash_context_len = 0;
    } else if (written >= static_cast<int>(sizeof(g_pikmin_crash_context))) {
        g_pikmin_crash_context_len = static_cast<std::sig_atomic_t>(sizeof(g_pikmin_crash_context) - 1);
    } else {
        g_pikmin_crash_context_len = static_cast<std::sig_atomic_t>(written);
    }
}
}
#endif

namespace {
int env_int(const char* name, int fallback) {
    if (const char* v = std::getenv(name)) return std::atoi(v);
    return fallback;
}
bool env_bool(const char* name, bool fallback) {
    if (const char* v = std::getenv(name)) return std::strcmp(v, "0") != 0 && std::strcmp(v, "false") != 0;
    return fallback;
}

#if defined(__linux__)
void pikmin_crash_handler(int sig) {
    static volatile std::sig_atomic_t handling = 0;
    if (handling) _exit(128 + sig);
    handling = 1;

    static const char banner[] = "\n[pikmin::crash] fatal signal\n";
    static const char context_prefix[] = "[pikmin::crash-context] ";
    static const char trace_banner[] = "[pikmin::crash] native backtrace follows\n";
    static const char newline[] = "\n";
    (void)!write(STDERR_FILENO, banner, sizeof(banner) - 1);
    (void)!write(STDERR_FILENO, context_prefix, sizeof(context_prefix) - 1);
    const int context_len = g_pikmin_crash_context_len;
    if (context_len > 0 && context_len < static_cast<int>(sizeof(g_pikmin_crash_context))) {
        (void)!write(STDERR_FILENO, g_pikmin_crash_context, static_cast<size_t>(context_len));
    }
    (void)!write(STDERR_FILENO, newline, sizeof(newline) - 1);
    (void)!write(STDERR_FILENO, trace_banner, sizeof(trace_banner) - 1);
    void* frames[96];
    const int count = backtrace(frames, static_cast<int>(sizeof(frames) / sizeof(frames[0])));
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    _exit(128 + sig);
}

void install_crash_handler() {
    std::signal(SIGSEGV, pikmin_crash_handler);
    std::signal(SIGABRT, pikmin_crash_handler);
    std::signal(SIGBUS, pikmin_crash_handler);
    std::signal(SIGILL, pikmin_crash_handler);
    std::signal(SIGFPE, pikmin_crash_handler);
}
#endif

bool has_arg(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], name)) return true;
    }
    return false;
}

const char* find_disc(int argc, char** argv) {
    if (const char* p = std::getenv("PIKMIN_ISO")) return p;
    for (int i = 1; i < argc; ++i) {
        if ((!std::strcmp(argv[i], "--iso") || !std::strcmp(argv[i], "-i")) && i + 1 < argc) return argv[i + 1];
        if (argv[i][0] != '-') return argv[i];
    }
    return nullptr;
}
}

int aurora_main(int argc, char* argv[]) {
#if defined(__linux__)
    install_crash_handler();
#endif
    const char* disc = find_disc(argc, argv);
    if (!disc || !std::filesystem::exists(disc)) {
        std::fprintf(stderr, "Usage: %s --iso /path/to/Pikmin.iso\n", argv[0]);
        return 2;
    }

    AuroraConfig cfg{};
    cfg.appName = "Pikmin (Aurora native port)";
    cfg.desiredBackend = BACKEND_VULKAN;
    cfg.msaa = static_cast<uint32_t>(env_int("PIKMIN_MSAA", 1));
    cfg.maxTextureAnisotropy = static_cast<uint16_t>(env_int("PIKMIN_ANISO", 16));
    cfg.vsync = env_bool("PIKMIN_VSYNC", false); // VI host timing drives the original 30 fps cadence.
    cfg.startFullscreen = env_bool("PIKMIN_FULLSCREEN", false);
    if (has_arg(argc, argv, "--fullscreen")) cfg.startFullscreen = true;
    if (has_arg(argc, argv, "--windowed")) cfg.startFullscreen = false;
    cfg.pauseOnFocusLost = true;
    cfg.allowJoystickBackgroundEvents = false;
    cfg.mem1Size = MEM1_DEFAULT_SIZE;
    cfg.mem2Size = ARAM_DEFAULT_SIZE;
    cfg.logLevel = env_bool("PIKMIN_DEBUG", false) ? LOG_DEBUG : LOG_INFO;

    const AuroraInfo auroraInfo = aurora_initialize(argc, argv, &cfg);
    std::fprintf(stderr, "[pikmin::window] created window=%p fullscreen=%d; toggle=F11/Alt+Enter\n",
                 static_cast<void*>(auroraInfo.window), cfg.startFullscreen ? 1 : 0);
    if (aurora_get_backend() != BACKEND_VULKAN) {
        std::fprintf(stderr, "Aurora did not select Vulkan. Refusing fallback backend.\n");
        aurora_shutdown();
        return 3;
    }
    if (!aurora_dvd_open(disc)) {
        std::fprintf(stderr, "Failed to open disc image: %s\n", disc);
        aurora_shutdown();
        return 4;
    }

    const int result = pikmin_game_main(argc, argv);
    aurora_dvd_close();
    aurora_shutdown();
    return result;
}
