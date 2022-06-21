#include "myspdlog.h"
using namespace N1::N2::N3;


MySpdlog::MySpdlog() {

    _log_path = "";	// 默认日志路径
    _log_level = 1;	// 默认日志水平
}

MySpdlog::~MySpdlog() {
    spdlog::drop_all();
}

bool MySpdlog::ExecLog() {

    // 每日日志
    std::string log;
    if (_log_path.empty()) {
        log = "logs/daily.txt";
    }
    else {
        log = _log_path + "logs/daily.txt";
    }

    daily_logger = spdlog::daily_logger_mt("daily_logger", log, 0, 0);
    if (daily_logger == NULL) {
        return false;
    }
    daily_logger.get()->set_level(static_cast<spdlog::level::level_enum>(_log_level));

    //回滚日志
    // Create a file rotating logger with xxxmb size max and xxx rotated files
    //参1为日志器的名字,参2为日志的文字,参3为日志大小,参4为回滚个数.
    // auto max_size = 1048576 * 10;//10M
    // auto max_files = 7;//回滚xxx个
    // rotating_logger = spdlog::rotating_logger_mt("rotating_logger", "logs/rotating.txt", max_size, max_files);

    //设置单个输出格式.
    // daily_logger.get()->set_pattern("%Y-%m-%d %H:%M:%S [%n] [%^ %l %$] [thread %t] - <%s>|<%#>|<%!>, %v");
    //设置全局输出格式.
    // %n记录器名称.例如控制台,文件.
    // %^起始顏色范围(只能使用一次).%L消息的简短日志级别(建议用小写%l).%$:結束顏色范围(例如%^ [+++]%$%v)(只能使用一次)
    // %s：文件名.%#：行号. %!：函数名.
    //spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] --- [%^ %l %$] [t %t]--->>> %v");
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%l] [t %t] [%s:%#] --->>> %v");

    // 设置当出现 _log_level 或 更严重的错误时立刻刷新日志到  disk
    daily_logger->flush_on(static_cast<spdlog::level::level_enum>(_log_level));
    //rotating_logger->flush_on(spdlog::level::err);

    //spdlog::flush_every(std::chrono::seconds(3));//在dll程序会卡死？

    return true;
}

void MySpdlog::SetLogPath(std::string path) {
    _log_path = path;
}

void MySpdlog::SetLogLevel(short level) {
    _log_level = level;
}
