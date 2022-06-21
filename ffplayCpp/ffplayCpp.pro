TEMPLATE = app
CONFIG += console c++11
CONFIG -= app_bundle
CONFIG -= qt

SOURCES += main.cpp \
    ffplay.cpp

win32 {
INCLUDEPATH += $$PWD/ffmpeg-4.2/include
INCLUDEPATH += $$PWD/SDL2/include

LIBS += $$PWD/ffmpeg-4.2/lib/x86/avformat.lib   \
        $$PWD/ffmpeg-4.2/lib/x86/avcodec.lib    \
        $$PWD/ffmpeg-4.2/lib/x86/avdevice.lib   \
        $$PWD/ffmpeg-4.2/lib/x86/avfilter.lib   \
        $$PWD/ffmpeg-4.2/lib/x86/avutil.lib     \
        $$PWD/ffmpeg-4.2/lib/x86/postproc.lib   \
        $$PWD/ffmpeg-4.2/lib/x86/swresample.lib \
        $$PWD/ffmpeg-4.2/lib/x86/swscale.lib    \
        $$PWD/SDL2/lib/x86/SDL2.lib \
        $$PWD/SDL2/lib/x86/SDL2main.lib \
}

HEADERS += \
    ffplay.h
