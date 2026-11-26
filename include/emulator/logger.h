#pragma once
#include <string>
#include <fstream>
#include <mutex>
#include <sstream>
#include <cstdio>
#include <utility>

namespace emu {

enum class LogLevel { Trace, Debug, Info, Warn, Error, Fatal };

class Logger {
public:
    static Logger& instance();
    void setLevel(LogLevel lvl);
    void setFile(const std::string& path);
    void log(LogLevel lvl, const std::string& msg);

    template <typename... Args>
    void log(LogLevel lvl, const char* fmt, Args&&... args) {
        char buf[4096];
        std::snprintf(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
        log(lvl, std::string(buf));
    }

private:
    Logger();
    LogLevel level_ = LogLevel::Info;
    std::ofstream file_;
    std::mutex mtx_;
    const char* levelName(LogLevel l) const;
};

#define LOG_TRACE(...) emu::Logger::instance().log(emu::LogLevel::Trace, __VA_ARGS__)
#define LOG_DEBUG(...) emu::Logger::instance().log(emu::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...)  emu::Logger::instance().log(emu::LogLevel::Info,  __VA_ARGS__)
#define LOG_WARN(...)  emu::Logger::instance().log(emu::LogLevel::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) emu::Logger::instance().log(emu::LogLevel::Error, __VA_ARGS__)
#define LOG_FATAL(...) emu::Logger::instance().log(emu::LogLevel::Fatal, __VA_ARGS__)

} // namespace emu