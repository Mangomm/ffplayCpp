#-------------------------------------------------
#
# Project created by QtCreator 2022-06-08T16:56:37
#
#-------------------------------------------------

QT       -= gui

TARGET = ffplayCppDll
TEMPLATE = lib

DEFINES += FFPLAYCPPDLL_LIBRARY

# The following define makes your compiler emit warnings if you use
# any feature of Qt which has been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    properties.cpp \
        videostate.cpp \
    myspdlog.cpp

HEADERS += \
        properties.h \
        videostate.h \
        ffplaycppdll_global.h \ 
    myspdlog.h \
    instance.h

unix {
    target.path = /usr/lib
    INSTALLS += target
}

win32 {

    #自定义变量.注释使用64位，否则使用32位
    #DEFINES += USE_X32

    #contains(QT_ARCH, i386) {#系统的变量，使用自己的变量去控制感觉更好
    contains(DEFINES, USE_X32) {
    # x86环境
    INCLUDEPATH += $$PWD/ffmpeg-4.2/include
    INCLUDEPATH += $$PWD/SDL2/include
    INCLUDEPATH += $$PWD/spdlog

    LIBS += $$PWD/ffmpeg-4.2/lib/x86/avformat.lib   \
            $$PWD/ffmpeg-4.2/lib/x86/avcodec.lib    \
            $$PWD/ffmpeg-4.2/lib/x86/avdevice.lib   \
            $$PWD/ffmpeg-4.2/lib/x86/avfilter.lib   \
            $$PWD/ffmpeg-4.2/lib/x86/avutil.lib     \
            $$PWD/ffmpeg-4.2/lib/x86/postproc.lib   \
            $$PWD/ffmpeg-4.2/lib/x86/swresample.lib \
            $$PWD/ffmpeg-4.2/lib/x86/swscale.lib    \
            $$PWD/SDL2/lib/x86/SDL2.lib
    message("win32")
    message("hhhhhhhhhhhhh")

    }else{
    # x64环境
    INCLUDEPATH += $$PWD/ffmpeg-4.2/include
    INCLUDEPATH += $$PWD/SDL2/include
    INCLUDEPATH += $$PWD/spdlog

    LIBS += $$PWD/ffmpeg-4.2/lib/x64/avformat.lib   \
            $$PWD/ffmpeg-4.2/lib/x64/avcodec.lib    \
            $$PWD/ffmpeg-4.2/lib/x64/avdevice.lib   \
            $$PWD/ffmpeg-4.2/lib/x64/avfilter.lib   \
            $$PWD/ffmpeg-4.2/lib/x64/avutil.lib     \
            $$PWD/ffmpeg-4.2/lib/x64/postproc.lib   \
            $$PWD/ffmpeg-4.2/lib/x64/swresample.lib \
            $$PWD/ffmpeg-4.2/lib/x64/swscale.lib    \
            $$PWD/SDL2/lib/x64/SDL2.lib
    message("win64")
    message("wwwwwwwwwwwwwww")
    }

}




