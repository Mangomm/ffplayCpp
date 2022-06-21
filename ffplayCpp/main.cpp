#include <iostream>
#include "ffplay.h"
using namespace std;
using namespace N1::N2::N3;

int main()
{
    int ret;
    VideoState *v = new VideoState();
    if(!v){
        cout << "new VideoState failed!" << endl;
        return -1;
    }

    //ret = v->Init(NULL, "rtsp://admin:pwd@192.168.1.1/Streaming/Channels/1");
    ret = v->Init(NULL, "./output.mkv");
    if(!ret){
        delete v;
        v = NULL;
        cout << "Init VideoState failed!" << endl;
        return -1;
    }

    ret = v->Play();
    if(ret < 0){
        delete v;
        v = NULL;
        cout << "Play VideoState failed!" << endl;
        return -1;
    }

    // 注意：SDL在SDL_CreateWindow后的返回值，必须要在同一个线程中渲染，否则会出现未响应的情况.
    // 而使用SDL_CreateWindowFrom不会，所以SDL_CreateWindowFrom通常是用在dll；而SDL_CreateWindow则像ffplay这样，需要直接显示.
    write_thread1(v);

    Sleep(1000000000000);

    ret = v->Close();
    if(ret != true){
        delete v;
        v = NULL;
        cout << "Close VideoState failed!" << endl;
        return -1;
    }

    if(v){
        delete v;
        v = NULL;
    }

    return 0;
}
