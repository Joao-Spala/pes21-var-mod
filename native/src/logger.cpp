#include "logger.h"

#include <windows.h>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace var_mod {

namespace {
std::mutex g_logMutex;
FILE* g_logFile = nullptr;
} // namespace

void Logger::Init(const std::wstring& logFilePath) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
    g_logFile = _wfopen(logFilePath.c_str(), L"w");
    if (g_logFile) {
        // 16 KB full buffering eliminates synchronous HDD seek stalls on Render/Replay/Sim threads
        std::setvbuf(g_logFile, nullptr, _IOFBF, 16384);
    }
}

void Logger::Flush() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        std::fflush(g_logFile);
    }
}

void Logger::Shutdown() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        std::fflush(g_logFile);
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
}

void Logger::Log(LogLevel level, const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_logFile) {
        return;
    }

    const char* tag = "INF";
    switch (level) {
        case LogLevel::Debug: tag = "DBG"; break;
        case LogLevel::Info:  tag = "INF"; break;
        case LogLevel::Warn:  tag = "WRN"; break;
        case LogLevel::Error: tag = "ERR"; break;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::fprintf(g_logFile, "[%02u:%02u:%02u.%03u][%s] ",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, tag);

    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_logFile, fmt, args);
    va_end(args);

    std::fputc('\n', g_logFile);
    // Only force synchronous disk flush on warnings/errors so hot threads never block on HDD I/O
    if (level >= LogLevel::Warn) {
        std::fflush(g_logFile);
    }
}

} // namespace var_mod
