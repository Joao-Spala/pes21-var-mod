#pragma once

#include <cstdarg>
#include <string>

namespace var_mod {

enum class LogLevel {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
};

class Logger {
public:
    static void Init(const std::wstring& logFilePath);
    static void Flush();
    static void Shutdown();
    static void Log(LogLevel level, const char* fmt, ...);
};

#define VAR_LOG_DBG(fmt, ...) ::var_mod::Logger::Log(::var_mod::LogLevel::Debug, fmt, ##__VA_ARGS__)
#define VAR_LOG_INF(fmt, ...) ::var_mod::Logger::Log(::var_mod::LogLevel::Info,  fmt, ##__VA_ARGS__)
#define VAR_LOG_WRN(fmt, ...) ::var_mod::Logger::Log(::var_mod::LogLevel::Warn,  fmt, ##__VA_ARGS__)
#define VAR_LOG_ERR(fmt, ...) ::var_mod::Logger::Log(::var_mod::LogLevel::Error, fmt, ##__VA_ARGS__)

} // namespace var_mod
