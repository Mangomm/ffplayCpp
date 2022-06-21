#ifndef MYSPDLOG_H
#define MYSPDLOG_H

#include <memory>

#include "instance.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/daily_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/rotating_file_sink.h"

namespace N1 {
    namespace N2 {
        namespace N3 {

            class MySpdlog : public N1::N2::N3::Instance<MySpdlog> {
                //class MySpdlog : public Instance<MySpdlog> {
                    //因为基类在GetInstance()中 new T() 时会调用到MySpdlog的构造函数
                friend class N1::N2::N3::Instance<MySpdlog>;
                friend class std::shared_ptr<MySpdlog>;

            private:
                MySpdlog();
                virtual ~MySpdlog();

            public:
                void SetLogPath(std::string path);
                void SetLogLevel(short level);
                bool ExecLog();

            private:
                std::shared_ptr<spdlog::logger> daily_logger;
                //std::shared_ptr<spdlog::logger> rotating_logger;

                std::string _log_path;
                short _log_level;

// 同时往控制台、日志文件输出
//spd 带行号的打印，获取每日日志器并设置控制台为默认日志器,使其同时输出console和文件
//这些宏实际上是对daily_logger进行操作
//#define SPDDEBUG(...) SPDLOG_LOGGER_DEBUG(spdlog::default_logger_raw(), __VA_ARGS__);SPDLOG_LOGGER_DEBUG(spdlog::get("daily_logger"), __VA_ARGS__)
//#define SPDINFO(...) SPDLOG_LOGGER_INFO(spdlog::default_logger_raw(), __VA_ARGS__);SPDLOG_LOGGER_INFO(spdlog::get("daily_logger"), __VA_ARGS__)
//#define SPDWARN(...) SPDLOG_LOGGER_WARN(spdlog::default_logger_raw(), __VA_ARGS__);SPDLOG_LOGGER_WARN(spdlog::get("daily_logger"), __VA_ARGS__)
//#define SPDERROR(...) SPDLOG_LOGGER_ERROR(spdlog::default_logger_raw(), __VA_ARGS__);SPDLOG_LOGGER_ERROR(spdlog::get("daily_logger"), __VA_ARGS__)
//#define SPDCRITICAL(...) SPDLOG_LOGGER_CRITICAL(spdlog::default_logger_raw(), __VA_ARGS__);SPDLOG_LOGGER_CRITICAL(spdlog::get("daily_logger"), __VA_ARGS__)

// 往控制台输出
//#define SPDDEBUG(...) SPDLOG_LOGGER_DEBUG(spdlog::default_logger_raw(), __VA_ARGS__)
//#define SPDINFO(...) SPDLOG_LOGGER_INFO(spdlog::default_logger_raw(), __VA_ARGS__)
//#define SPDWARN(...) SPDLOG_LOGGER_WARN(spdlog::default_logger_raw(), __VA_ARGS__)
//#define SPDERROR(...) SPDLOG_LOGGER_ERROR(spdlog::default_logger_raw(), __VA_ARGS__)
//#define SPDCRITICAL(...) SPDLOG_LOGGER_CRITICAL(spdlog::default_logger_raw(), __VA_ARGS__)
// 只往日志文件输出
#define SPDDEBUG(...) SPDLOG_LOGGER_DEBUG(spdlog::get("daily_logger"), __VA_ARGS__)
#define SPDINFO(...) SPDLOG_LOGGER_INFO(spdlog::get("daily_logger"), __VA_ARGS__)
#define SPDWARN(...) SPDLOG_LOGGER_WARN(spdlog::get("daily_logger"), __VA_ARGS__)
#define SPDERROR(...) SPDLOG_LOGGER_ERROR(spdlog::get("daily_logger"), __VA_ARGS__)
#define SPDCRITICAL(...) SPDLOG_LOGGER_CRITICAL(spdlog::get("daily_logger"), __VA_ARGS__)
            };

        }
    }
}

#endif // MYSPDLOG_H
