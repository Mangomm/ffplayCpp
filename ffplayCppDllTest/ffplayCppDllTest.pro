TEMPLATE = app
CONFIG += console c++11
CONFIG -= app_bundle
#CONFIG -= qt
QT += core

SOURCES += main.cpp

win32 {

    #自定义变量.注释使用64位，否则使用32位
    #DEFINES += USE_X32

    #contains(QT_ARCH, i386) {#系统的变量，使用自己的变量去控制感觉更好
    contains(DEFINES, USE_X32) {
    # x86环境
    INCLUDEPATH += $$PWD/../ffplayCppDll/
    INCLUDEPATH += $$PWD/../ffplayCppDll/ffmpeg-4.2/include
    INCLUDEPATH += $$PWD/../ffplayCppDll/SDL2/include
    INCLUDEPATH += $$PWD/spdlog

    LIBS += $$PWD/../win32/debug/libffplayCppDll.a
    message("win32")
    message("hhhhhhhhhhhhh")

    }else{
    # x64环境
    INCLUDEPATH += $$PWD/../ffplayCppDll/
    INCLUDEPATH += $$PWD/../ffplayCppDll/ffmpeg-4.2/include
    INCLUDEPATH += $$PWD/../ffplayCppDll/SDL2/include
    INCLUDEPATH += $$PWD/../ffplayCppDll/spdlog

    LIBS += $$PWD/../win64/debug/libffplayCppDll.a
    message("win64")
    message("wwwwwwwwwwwwwww")
    }

}
