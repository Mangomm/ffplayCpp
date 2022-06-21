#include <iostream>
#include <windows.h>
#include "videostate.h"
#include "properties.h"
using namespace std;
using namespace N1::N2::N3;

typedef HWND(WINAPI *PROCGETCONSOLEWINDOW)();

int main()
{
    cout << "Hello World!" << endl;

    HMODULE hKernel32 = GetModuleHandle(L"kernel32");
    PROCGETCONSOLEWINDOW GetConsoleWindow = (PROCGETCONSOLEWINDOW)GetProcAddress(hKernel32, "GetConsoleWindow");
    HWND hWnd = GetConsoleWindow();

    VideoState *v = new VideoState();
    if(!v){
        printf("new failed\n");
        return -1;
    }

    printf("Init\n");
    Properties pp;
    // 一 视频属性
    // 1.1 实时流属性
//    pp.SetProperty("play_url", "rtsp://admin:pwd@192.168.1.1/Streaming/Channels/1");
//    pp.SetProperty("fflags", "nobuffer");
//    pp.SetProperty("rtsp_transport", "tcp");
//    pp.SetProperty("analyzeduration", "2000000");

    // 1.2 文件属性
    pp.SetProperty("play_url", "./output.mkv");
    // 添加字幕播放
    //pp.SetProperty("vf", "subtitles=\"C:\\Users\\DELL\\Desktop\\output.mkv\":si=0");// 注意，过滤器使用绝对路径时会报错，avfilter_graph_parse_ptr返回-22
    pp.SetProperty("vf", "subtitles=./output.mkv:si=0");

    // 二 音频属性


    int ret = v->Init(hWnd, pp);
    if(!ret){
        printf("Init failed\n");
        return -1;
    }

    printf("Play\n");
    ret = v->Play();
    if(ret < 0){
        printf("Play failed\n");
        return -1;
    }

    // 按下任意键退出
    //getchar();
    Sleep(50000000);
    //Sleep(1000000000000);

    ret = v->Close();
    if (ret == false) {
        printf("Close failed\n");
        return -1;
    }

    return 0;
}
