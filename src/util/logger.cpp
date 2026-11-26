#include "emulator/logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>

namespace emu {

Logger::Logger() {
    file_.open("emulator.log", std::ios::app);
}

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::setLevel(LogLevel lvl) { level_ = lvl; }
void Logger::setFile(const std::string& path) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (file_.is_open()) file_.close();
    file_.open(path, std::ios::app);
}

void Logger::close() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

const char* Logger::levelName(LogLevel l) const {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?";
}

void Logger::log(LogLevel lvl, const std::string& msg) {
    if (lvl < level_) return;
    std::lock_guard<std::mutex> lk(mtx_);
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " [" << levelName(lvl) << "] " << msg << "\n";
    std::string line = os.str();
    std::cout << line;
    if (file_.is_open()) { file_ << line; file_.flush(); }
}

} // namespace emu