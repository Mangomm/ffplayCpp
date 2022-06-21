#include <sstream>
#include <math.h>
#include <time.h>
#include "mem.h"
#include <fstream>
#include <mutex>
#include "videostate.h"
#include "myspdlog.h"

using namespace std;
using namespace N1::N2::N3;
#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)
#define FF_INITIATIVE_QUIT_EVENT    (SDL_USEREVENT + 3)	// 主动关闭视频

PacketQueue::PacketQueue() {

}

PacketQueue::~PacketQueue() {

}

int PacketQueue::packet_queue_init() {

    /*
    MyAVPacketList	*_first_pkt, *_last_pkt;
    int		_nb_packets;
    int		_size;
    int64_t		_duration;
    int		_abort_request;
    int		_serial;
    SDL_mutex	*_mutex;
    SDL_cond	*_cond;
    */

    _first_pkt = _last_pkt = NULL;
    _nb_packets = _size = 0;
    _duration = 0;
    _abort_request = 0;
    _serial = 0;
    _mutex = NULL;
    _cond = NULL;

    _mutex = SDL_CreateMutex();
    if (!_mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    _cond = SDL_CreateCond();
    if (!_cond) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    _abort_request = 1;// ffplay也置为1

    return 0;
}

void PacketQueue::packet_queue_flush()
{
    /*
    MyAVPacketList	*_first_pkt, *_last_pkt;
    int		_nb_packets;
    int		_size;
    int64_t		_duration;
    int		_abort_request;
    int		_serial;
    SDL_mutex	*_mutex;
    SDL_cond	*_cond;
    */

    MyAVPacketList *pkt, *pkt1;// pkt指向当前包，pkt1指向当前包的下一个包

    SDL_LockMutex(_mutex);

    // 遍历链表只用_first_pkt即可，_last_pkt的作用请看packet_queue_put_private.
    for (pkt = _first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);// 取消对数据包引用的缓冲区的引用，并将其余的数据包字段重置为默认值。注意flush_pkt也是在队列的，并且因为MyAVPacketList的AVPacket是局部变量，
                                   // 所以只能使用av_packet_unref统一处理，AVPacket可以自动回收；而二次开发时若节点的AVPacket是new出来的，回收时要用delete而不是av_packet_unref，
                                   // 所以应当注意单独使用av_packet_unref回收flush_pkt，因为flush_pkt是全局变量
        av_freep(&pkt);// 释放put时malloc的MyAVPacketList节点
    }
    _last_pkt = NULL;
    _first_pkt = NULL;
    _nb_packets = 0;
    _size = 0;
    _duration = 0;

    SDL_UnlockMutex(_mutex);
}

void PacketQueue::packet_queue_destroy()
{
    // 1. 先清除所有的节点
    packet_queue_flush();

    // 2. 释放内部资源
    if(_mutex){
        SDL_DestroyMutex(_mutex);
        _mutex = NULL;
    }
    if(_cond){
        SDL_DestroyCond(_cond);
        _cond = NULL;
    }
}

/**
* @brief 往包队列插入一个包。
* @param q 包队列。可能是音视频、字幕包队列。
* @param pkt 要插入的包。可能是数据包，也可能是个空包，用于刷掉帧队列剩余的帧。
* @return 成功0 失败-1
*/
int PacketQueue::packet_queue_put_private(AVPacket *pkt)
{
    MyAVPacketList *pkt1;

    // 1. 如果已中止，则放入失败
    if (_abort_request)
        return -1;

    // 2. 分配节点内存
    pkt1 = (MyAVPacketList*)av_malloc(sizeof(MyAVPacketList));
    if (!pkt1)  //内存不足，则返回失败
        return -1;

    // 3. 赋值，和判断是否插入的是flush_pkt包。
    // 没有做引用计数，那这里也说明av_read_frame不会释放替用户释放buffer。
    pkt1->pkt = *pkt; //拷贝AVPacket(浅拷贝，AVPacket.data等内存并没有拷贝)
    pkt1->next = NULL;
    if (pkt == &flush_pkt)//如果放入的是flush_pkt，需要增加队列的播放序列号，以区分不连续的两段数据。
    {
        _serial++;
        printf("q->serial = %d\n", _serial);
    }
    pkt1->serial = _serial;   //用队列序列号标记节点序列号。和上面的包队列->serial作用一样，上面的不变，这里的也不变，上面的变，这里的也会变。
                                //这里看到，添加flush_pkt时的这个节点的serial也是自增。

    /*
    * 4. 队列操作：如果last_pkt为空，说明队列是空的，新增节点为队头；例如包队列只有一个包时，first_pkt、last_pkt指向同一个包。
    *      注意last_pkt不是指向NULL，和平时的设计不一样，不过内部的next是可能指向NULL。
    * 否则，队列有数据，则让原队尾的next为新增节点。 最后将队尾指向新增节点。
    *
    * 他这个队列的特点：1）first_pkt只操作一次，永远指向首包； 2）last_pkt永远指向尾包，不会指向NULL，但尾包last_pkt内部的next永远指向NULL。
    */
    if (!_last_pkt)
        _first_pkt = pkt1;
    else
        _last_pkt->next = pkt1;
    _last_pkt = pkt1;

    // 5. 队列属性操作：增加节点数、cache大小、cache总时长, 用来控制队列的大小
    _nb_packets++;
    _size += pkt1->pkt.size + sizeof(*pkt1);// 每个包的大小由：实际数据 + MyAVPacketList的大小。
    _duration += pkt1->pkt.duration;        // 如果是空包，在av_init_packet时duration被赋值为0.

    /* XXX: should duplicate packet data in DV case */
    // 6. 发出信号，表明当前队列中有数据了，通知等待中的读线程可以取数据了。这里的读线程应该指解码线程，以便读取包进行解码。
    SDL_CondSignal(_cond);

    return 0;
}

/**
* @brief 往包队列插入一个包。
* @param q 包队列。可能是音视频、字幕包队列。
* @param pkt 要插入的包。可能是数据包，也可能是个空包，用于刷掉帧队列剩余的帧。
* @return 成功0 失败-1。 see packet_queue_put_private。
*/
int PacketQueue::packet_queue_put(AVPacket *pkt)
{
    int ret;

    // 1. 调用packet_queue_put_private往包队列put一个包。
    SDL_LockMutex(_mutex);
    ret = packet_queue_put_private(pkt);//主要实现
    SDL_UnlockMutex(_mutex);

    // 2. 如果不是flush_pkt包并且放入失败的话，需要释放掉，因为av_read_frame不会帮你释放
    // 只有队列中断或者malloc失败返回值才会小于0，所以不需要回收malloc的内容.
    if (pkt != &flush_pkt && ret < 0)
        av_packet_unref(pkt);

    return ret;
}

/**
* @brief 往包队列插入空包。插入空包说明码流数据读取完毕了，之前讲解码的时候说过刷空包是为了从解码器把所有帧都读出来。
* @param q 包队列。可能是音视频、字幕包队列。
* @param stream_index 对应流的下标。可能是音视频、字幕下标。
* @return 成功0 失败-1。 see packet_queue_put、packet_queue_put_private.
*/
int PacketQueue::packet_queue_put_nullpacket(int stream_index)
{
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);// 以默认值初始化除了data、size外的字段。data、size必须由用户设置。
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    return packet_queue_put(pkt);
}

void PacketQueue::packet_queue_abort()
{
    SDL_LockMutex(_mutex);

    _abort_request = 1;       // 请求退出.队列的设计带有中断标志，置为1的目的只有1个，就是使packet_queue_put、packet_queue_get这两个函数不能再操作。

    SDL_CondSignal(_cond);    // 释放一个条件信号.这里只是为了唤醒packet_queue_get中阻塞获取时(SDL_CondWait(_cond, _mutex);)，让其更快中断。

    SDL_UnlockMutex(_mutex);
}

/**
* @brief packet_queue_get
* @param q 包队列。
* @param pkt 传入传出参数，即MyAVPacketList.pkt。
* @param block 调用者是否需要在没节点可取的情况下阻塞等待。0非阻塞，1阻塞。
* @param serial 传入传出参数，即MyAVPacketList.serial。
* @return <0: aborted; =0: no packet; >0: has packet。
*
* 该队列的get的设计与rtsp-publish推流的PacketQueue的get类似。
* packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial);
*/
int PacketQueue::packet_queue_get(AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList *pkt1;
    int ret;

    // 1. 对包队列上锁
    SDL_LockMutex(_mutex);

    for (;;) {
        // 2. 用户请求包队列中断退出
        if (_abort_request) {
            ret = -1;
            break;
        }

        // 3. 从对头取出一个节点MyAVPacketList
        pkt1 = _first_pkt;
        if (pkt1) {                     // 队列中有数据
            _first_pkt = pkt1->next;	// 更新对头，此时队头为第二个节点
            if (!_first_pkt)			// 如果第二个包是空的话，那么此时队列为空，last_pkt也应该被置为空，回到最初的状态。
                _last_pkt = NULL;

            // 4. 更新相应的属性。 可以对比packet_queue_put_private，也是更新了这3个属性。
            _nb_packets--;								// 节点数减1
            _size -= pkt1->pkt.size + sizeof(*pkt1);    // cache大小扣除一个节点
            _duration -= pkt1->pkt.duration;            // 总时长扣除一个节点

            // 5. 真正获取AVPacket，这里发生一次AVPacket结构体拷贝，AVPacket的data只拷贝了指针
            *pkt = pkt1->pkt;
            if (serial) //如果需要输出serial，把serial输出. serial一般是解码队列里面的serial。
                *serial = pkt1->serial;

            // 6. 释放节点内存，因为在MyAVPacketList是malloc节点的内存。注只是释放节点，而不是释放AVPacket
            av_free(pkt1);
            ret = 1;

            break;
        }
        else if (!block) {    // 7. 队列中没有数据，且非阻塞调用
            ret = 0;
            break;
        }
        else {                // 8. 队列中没有数据，且阻塞调用，会阻塞在条件变量。
                              // 这里没有break。for循环的另一个作用是在条件变量被唤醒后，重复上述代码取出节点。
            SDL_CondWait(_cond, _mutex);
        }

    }// <== for (;;) end ==>

     // 9. 释放锁
    SDL_UnlockMutex(_mutex);

    return ret;
}

/**
* @brief 启动包队列。往队列插入一个flush_pkt包，标记启动了包队列。由于在读线程中，stream_component_open是比av_read_frame读包进队列早的，
*          所以解码线程是更快创建和flush_pkt是被首先放进对应的队列的，flush_pkt对应的serial加1。
* @param q 包队列。
* @return void。
*/
void PacketQueue::packet_queue_start()
{
    SDL_LockMutex(_mutex);
    _abort_request = 0;
    packet_queue_put_private(&flush_pkt); //这里放入了一个flush_pkt
    SDL_UnlockMutex(_mutex);
}

PacketQueue::pktStatus PacketQueue::packet_queue_get_status() {

    PacketQueue::pktStatus status = { 0 };

    SDL_LockMutex(_mutex);
    status.nbPackets = _nb_packets;
    status.size = _size;
    status.duration = _duration;
    SDL_UnlockMutex(_mutex);

    return status;
}

int PacketQueue::packet_queue_get_packets(){
    return _nb_packets;
}

int* PacketQueue::packet_queue_get_serial(){
    return &_serial;
}

int  PacketQueue::packet_queue_get_abort(){
    return _abort_request;
}

int  PacketQueue::packet_queue_get_duration(){
    return _duration;
}

int  PacketQueue::packet_queue_get_size(){
    return _size;
}

Clock::Clock() {

}

Clock::~Clock() {

}

void Clock::init_clock(int *queue_serial)
{
    /*
        double	_pts;
        double	_pts_drift;
        double	_last_updated;   // 最后一次更新的系统时钟
        double	_speed;          // 时钟速度控制，用于控制播放速度
        int	_serial;
        int	_paused;             // = 1 说明是暂停状态
        int *_queue_serial;      // 指向packet_serial，即指向当前包队列的指针，用于过时的时钟检测。
    */
    _speed = 1.0;                     // 播放速率
    _paused = 0;                      // 0是未暂停，1是暂停。
    _queue_serial = queue_serial;     // 时钟指向播放器中的包队列的serial。

    set_clock(NAN, -1);               // 注意上面是包队列的serial(初始化时值是0)，时钟内部还有一个自己的serial(初始化时值是-1)。
}

/**
 * @brief 三种情况：
 *          一：时钟序列号若与包队列serial不相等，返回NAN;
 *          二：暂停情况则先返回_pts;
 *          三：这一步是正常情况走的流程，分两个情况：
 *              1、若不是外部时钟，说明_speed播放速率没有改变，则返回_pts_drift + time求出的实时pts(与二返回的pts不一样，这个是动态计算得出的，更加准确)即可.
 *              2、若是外部时钟，说明_speed播放速率改变，则会根据上一次系统时间与本次系统时间的差以及配合_speed的值求出实际pts。
*/
double Clock::get_clock()
{
    if (*_queue_serial != _serial)
        return NAN; // 不是同一个播放序列，时钟是无效
    if (_paused) {
        return _pts;  // 暂停的时候返回的是pts
    }
    else {
        // 流正常播放一般都会走这里
        double time = av_gettime_relative() / 1000000.0;
        // 1）c->pts_drift + time：代表本次的pts，因为系统时钟time是一直变化的，需要利用它和set_clock对时得出的pts_drift重新计算。
        // 2）(time - c->last_updated)：代表此时系统时间和上一次系统时间的间隔。
        // 3）(1.0 - c->speed)：主要用来控制播放速度。
        // 4）所以(time - _last_updated) * (1.0 - _speed)就是求因外部时钟改变_speed后，两帧间隔的实际系统时间差。因为速度改变过，(time - _last_updated)肯定不是真正两个系统时间间隔。
        //  例如，假设_pts_drift + time=800s，(time - _last_updated)正常结果是=40ms，而_speed=0.9，
        //  那么得出真正的两帧系统时间差(time - _last_updated) * (1.0 - _speed)=4ms，那么800s-4ms=799.996s，所以播放速度变慢，会自动减少返回的pts以达到降速效果。
        // 同样假设速率变成1.1，结果是800.004s，所以播放速度变快，，会自动增加返回的pts以达到加速效果。

        // 5）外部时钟改变_speed的原理：包数少则减少_speed；包多则增加_speed。
        // 所以经过上面的例子得出：
        //      1、当包少时，速率变慢，例如0.9速率时，返回的pts是_pts_drift + time - (time - _last_updated) * (1.0 - _speed)=799.996s，而原本是800s，
        //          也就是说将动态计算的pts减去了4ms，最终使两帧间隔变长。简单说就是：本来正常是800s到下一帧800.04s的间距(假设正常两帧差是40ms)，由于降速，变成了799.996到下一帧800.04s的间距。
        //      2、当包多时，速率变快，同理例如1.1速率时，返回的pts是_pts_drift + time - (time - _last_updated) * (1.0 - _speed)=800.004s，而原本是800s，
        //          也就是说将动态计算的pts加上了4ms，最终使两帧间隔变短。简单说就是：本来正常是800s到下一帧800.04s的间距，由于加速，变成了800.004s到下一帧800.04s的间距。
        // 注：这里4）5）以及举例的例子只是自己对_speed的见解，可能不一定准确，但是目前这样理解个人感觉是最好的。1）2）3）的意思必定是正确的。如果没有用外部时钟，那么就不需要管_speed的值。
        return _pts_drift + time - (time - _last_updated) * (1.0 - _speed);
    }
}

void Clock::set_clock_at(double pts, int serial, double time)
{
    _pts = pts;						/* 当前帧的pts */
    _last_updated = time;           /* 最后更新的时间，实际上是当前的一个系统时间 */
    _pts_drift = _pts - time;       /* 当前帧pts和系统时间的差值，正常播放情况下两者的差值应该是比较固定的，因为两者都是以时间为基准进行线性增长 */
    _serial = serial;
}

void Clock::set_clock(double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;// 获取当前时间，单位秒。
    set_clock_at(pts, serial, time);
}

void Clock::set_clock_speed(double speed)
{
    set_clock(get_clock(), _serial);// 更新速率前，会先更新时钟的pts以及serial，pts是随着系统流逝的pts。
    _speed = speed;
}

/**
 * @brief 该函数主要是提供给外部时钟为主时钟时使用的，因为外部时钟需要同时参考视频、音频时钟。
 *          所以在视频的update_video_pts更新视频pts以及音频的sdl_audio_callback更新pts各自调用了
 *          一次sync_clock_to_slave，就是为了防止参考音视频时钟时误差过大，而进行的同步调整。
*/
void Clock::sync_clock_to_slave(Clock *slave)
{
    // 1. 获取主时钟随着实时时间流逝的pts
    double clock = get_clock();
    // 2. 获取从时钟随着实时时间流逝的pts
    double slave_clock = slave->get_clock();

    // 3. 通过set_clock使用从时钟的pts设置主时钟的pts
    // 1）从时钟有效并且主时钟无效；这个条件应该是主时钟未设置需要进行设置。
    // 2）或者 从时钟随着实时时间流逝的pts 与 主时钟随着实时时间流逝的pts 相差太大，需要根据从时钟对主时钟进行重设。
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(slave_clock, slave->_serial);
}

void Clock::set_clock_flush() {
    /*
        double	_pts;
        double	_pts_drift;
        double	_last_updated;   // 最后一次更新的系统时钟
        double	_speed;          // 时钟速度控制，用于控制播放速度
        int	_serial;
        int	_paused;             // = 1 说明是暂停状态
        int *_queue_serial;      // 指向packet_serial，即指向当前包队列的指针，用于过时的时钟检测。
    */
    _pts = 0;
    _pts_drift = 0;
    _last_updated = 0;
    _speed = 1;
    _serial = 0;
    _paused = 0;
    if (_queue_serial) {
        _queue_serial = 0;
    }

}

int Clock::get_serial() {
    return _serial;
}

int* Clock::get_serialc() {
    return &_serial;
}

double Clock::get_last_updated(){
    return _last_updated;
}

int Clock::get_paused(){
    return _paused;
}

void Clock::set_paused(int paused){
    _paused = paused;
}

double Clock::get_clock_pts(){
    return _pts;
}

FrameQueue::FrameQueue() {

}

FrameQueue::~FrameQueue() {

}

int FrameQueue::frame_queue_init(PacketQueue *pktq, int max_size, int keep_last)
{
    /*
        Frame	_queue[FRAME_QUEUE_SIZE];        // FRAME_QUEUE_SIZE  最大size, 数字太大时会占用大量的内存，需要注意该值的设置
        int		_rindex;                         // 读索引。待播放时读取此帧进行播放，播放后此帧成为上一帧
        int		_windex;                         // 写索引
        int		_size;                           // 当前总帧数
        int		_max_size;                       // 可存储最大帧数
        int		_keep_last;                      // = 1说明要在队列里面保持最后一帧的数据不释放，只在销毁队列的时候才将其真正释放
        int		_rindex_shown;                   // 初始化为0，配合keep_last=1使用
        SDL_mutex	*_mutex;                     // 互斥量
        SDL_cond	*_cond;                      // 条件变量
        PacketQueue	*_pktq;                      // 数据包缓冲队列
    */

    int i;
    memset(&_queue, 0, sizeof(Frame) * FRAME_QUEUE_SIZE);
    _rindex = 0;
    _windex = 0;
    _size = 0;
    _max_size = 0;
    _keep_last = 0;
    _rindex_shown = 0;
    _mutex = NULL;
    _cond = NULL;
    _pktq = NULL;

    if (!(_mutex = SDL_CreateMutex())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    if (!(_cond = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    _pktq = pktq;                                         // 帧队列里面保持着播放器里面的包队列，pktq是播放器内部的包队列

    _max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);        // 表示用户输入的max_size的值在[负无穷,FRAME_QUEUE_SIZE]，但结合实际，负数和0应该是不存在
    _keep_last = !!keep_last;                             // 连续关系非运算，相当于直接赋值。关系运算符结果不是0就是1.
    for (i = 0; i < _max_size; i++)
        if (!(_queue[i].frame = av_frame_alloc()))        // 分配AVFrame结构体，个数为最大个数f->max_size
            return AVERROR(ENOMEM);

    return 0;
}

void FrameQueue::frame_queue_unref_item(Frame *vp)
{
    av_frame_unref(vp->frame);	/* 释放数据，不是释放AVFrame本身 */
    avsubtitle_free(&vp->sub);  /* 释放给定字幕结构中所有已分配的数据 */
}

void FrameQueue::frame_queue_destory()
{
    int i;
    // 1 释放AVFrame以及内部的数据缓冲区引用
    for (i = 0; i < _max_size; i++) {
        Frame *vp = &_queue[i];
        // 1.1 释放对vp->frame中的数据缓冲区的引用，注意不是释放frame对象本身
        frame_queue_unref_item(vp);

        // 1.2 释放vp->frame对象
        av_frame_free(&vp->frame);
    }

    // 2 释放互斥锁和条件变量
    SDL_DestroyMutex(_mutex);
    SDL_DestroyCond(_cond);
}

void FrameQueue::frame_queue_signal()
{
    SDL_LockMutex(_mutex);
    SDL_CondSignal(_cond);
    SDL_UnlockMutex(_mutex);
}

// _rindex_shown=0，表示获取待显示帧
// _rindex_shown=1，表示获取已显示帧
Frame *FrameQueue::frame_queue_peek_last()
{
    return &_queue[_rindex];
}

// 获取待显示帧
Frame *FrameQueue::frame_queue_peek()
{
    return &_queue[(_rindex + _rindex_shown) % _max_size];
}

// 获取待显示帧的下一帧
Frame *FrameQueue::frame_queue_peek_next()
{
    return &_queue[(_rindex + _rindex_shown + 1) % _max_size];
}

Frame *FrameQueue::frame_queue_peek_writable()
{
    /* wait until we have space to put a new frame */
    SDL_LockMutex(_mutex);
    while (_size >= _max_size &&    // 当前帧数到达最大帧数，阻塞在条件变量，等待消费唤醒后才能再写入。这里说明帧队列的大小是包含：已经显示的帧+未显示的帧。所以当已显示的帧显示完毕后，该帧进行抓图是没问题的，数据还没被置空。
        /*!_pktq->_abort_request) {*/	/* 或者用户请求退出(由packetqueue记录用户是否退出，帧队列不记录)，1表示退出 */
        !_pktq->packet_queue_get_abort()) {
        SDL_CondWait(_cond, _mutex);
    }
    SDL_UnlockMutex(_mutex);

    //if (_pktq->_abort_request)		/* 检查是不是要退出.这里不上锁是最好的，因为在while阻塞在条件变量时，其它线程置中断=1，那么此时肯定不会再有其它线程在操作_abort_request */
    if (_pktq->packet_queue_get_abort())
        return NULL;

    return &_queue[_windex];        // windex的自增是在frame_queue_push内处理的。
}

Frame *FrameQueue::frame_queue_peek_readable()
{
    /* wait until we have a readable a new frame */
    SDL_LockMutex(_mutex);
    while (_size - _rindex_shown <= 0 &&
        /*!_pktq->_abort_request) {*/
        !_pktq->packet_queue_get_abort()) {
        SDL_CondWait(_cond, _mutex);
    }
    SDL_UnlockMutex(_mutex);

    //if (_pktq->_abort_request)
    if (_pktq->packet_queue_get_abort())
        return NULL;

    return &_queue[(_rindex + _rindex_shown) % _max_size];  // 除以f->max_size：因为帧队列有可能大于max_size的，所以必须确保使用的是已经开辟的帧内存。
                                                            // 例如音频，max_size=9(init时赋值)，queue帧队列是15，但是queue->frame是没有alloc内存的，所以必须除以max_size，确保合法。
}

void FrameQueue::frame_queue_push()
{
    // 1. 如果写下标到末尾再+1，则回到开头。
    // 写下标不用加锁是因为，写下标只在写线程维护，同样读下标只在读线程维护，所以不用上锁。而下面的size则需要，因为读写线程都用到。
    if (++_windex == _max_size)
        _windex = 0;

    // 2. 更新帧队列的帧数量，并且唤醒。
    SDL_LockMutex(_mutex);
    _size++;
    SDL_CondSignal(_cond);    // 当_readable在等待时则可以唤醒
    SDL_UnlockMutex(_mutex);
}

void FrameQueue::frame_queue_next()
{
    // 1. 当keep_last为1, rindex_show为0时不去更新rindex,也不释放当前frame
    if (_keep_last && !_rindex_shown) {
        _rindex_shown = 1; // 第一次进来没有更新，对应的frame就没有释放
        return;
    }

    // 到这里，必定是已经缓存了一帧，所以释放_rindex就是释放已经显示的这一帧。

    // 2. 释放当前frame的数据，并更新读索引rindex与当前队列的总帧数。
    frame_queue_unref_item(&_queue[_rindex]);
    if (++_rindex == _max_size)// 如果此时rindex是队列的最尾部元素，则读索引更新为开头索引即0.
        _rindex = 0;
    SDL_LockMutex(_mutex);
    _size--;                      // 当前队列总帧数减1.
    SDL_CondSignal(_cond);
    SDL_UnlockMutex(_mutex);
}

int FrameQueue::frame_queue_nb_remaining()
{
    // 因为这里用到_size，个人写的话会上锁，但是ffplay直接不加锁，就是可能觉得上不上锁返回的待显示帧数都差不多，所以直接没上锁
    //return _size - _rindex_shown;

    // tyycode
    SDL_LockMutex(_mutex);
    int unPresentNum = _size - _rindex_shown;
    SDL_UnlockMutex(_mutex);
    return unPresentNum;
}

/*
 * 返回已经显示的帧的pos位置，并且serial与包队列相同即serial要求是最新的。否则返回-1. 该函数用于seek，只在事件处理时被调用.
 * 同样不用上锁：1. _rindex_shown：在frame_queue_next被改变，而frame_queue_next只在消费线程使用，且和frame_queue_last_pos是分顺序调用，
 *                  或者说frame_queue_next只写，而frame_queue_last_pos只读，所以同不同步意义不大；
 *             2. fp->serial：是在解码时赋值的，赋值完就放到队列中，相当于一个完整安全的变量，只要队列安全就行，理解起来很简单，所以也是安全的，且同不同步意义不大；
 *             3. _pktq->_serial：同样只在读线程时发生seek会改变，其它线程只读，所以同不同步意义不大，不需要上锁
*/
int64_t FrameQueue::frame_queue_last_pos()
{
    // 返回上一帧的pos位置，由于只有调用frame_queue_next了，rindex_shown才会置1，也就是说，只有出过队列，才会存在上一帧，
    // 没有出过队列那么rindex_shown=0，不会存在上一帧，所以返回-1.
    Frame *fp = &_queue[_rindex];
    //if (_rindex_shown && fp->serial == _pktq->_serial)
    if (_rindex_shown && fp->serial == *_pktq->packet_queue_get_serial())
        return fp->pos;
    else
        return -1;
}

int FrameQueue::frame_queue_get_rindex_shown(){
    return _rindex_shown;
}

SDL_mutex *FrameQueue::frame_queue_get_mutex(){
    return _mutex;
}

Decoder::Decoder() {

}

Decoder::~Decoder() {

}

void Decoder::decoder_init(AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) {

    /*
        AVPacket _pkt;
        PacketQueue	*_queue;             // 数据包队列
        AVCodecContext	*_avctx;         // 解码器上下文
        int		_pkt_serial;             // 包序列
        int		_finished;               // =0，解码器处于工作状态；=非0，解码器处于空闲状态
        int		_packet_pending;         // =0，解码器处于异常状态，需要考虑重置解码器；=1，解码器处于正常状态
        SDL_cond	*_empty_queue_cond;  // 检查到packet队列空时发送 signal缓存read_thread读取数据
        int64_t		_start_pts;          // 初始化时是stream的start time
        AVRational	_start_pts_tb;       // 初始化时是stream的time_base
        int64_t		_next_pts;           // 记录最近一次解码后的frame的pts，当解出来的部分帧没有有效的pts时则使用next_pts进行推算
        AVRational	_next_pts_tb;        // next_pts的单位
        SDL_Thread	*_decoder_tid;       // 线程句柄
    */

    memset(&_pkt, 0, sizeof(AVPacket));
    _queue = NULL;
    _avctx = NULL;
    _pkt_serial = 0;
    _finished = 0;
    _packet_pending = 0;
    _empty_queue_cond = NULL;
    _start_pts = 0;
    _start_pts_tb = { 0, 0 };
    _next_pts = 0;
    _next_pts_tb = { 0, 0 };
    _decoder_tid = NULL;

    _avctx = avctx;                           // 解码器上下文
    _queue = queue;                           // 绑定对应的packet queue
    _empty_queue_cond = empty_queue_cond;     // 绑定read_thread线程的continue_read_thread
    _start_pts = AV_NOPTS_VALUE;              // 起始设置为无效
    _pkt_serial = -1;                         // 起始设置为-1
}

int Decoder::decoder_decode_frame(AVFrame *frame, AVSubtitle *sub) {
    int ret = AVERROR(EAGAIN);

    for (;;) {
        AVPacket pkt;

        // 1. 流连续情况下获取解码后的帧
        // 1.1 先判断是否是同一播放序列的数据，不是的话不会进行receive。 解码队列中的包队列的serial(d->queue->serial)会在插入flush_pkt时自增
        // 但个人感觉这个if可以去掉，因为下面不是同一序列的pkt会被直接释放，不会送进解码。
        //if (_queue->_serial == _pkt_serial) {
        if (*_queue->packet_queue_get_serial() == _pkt_serial) {
            do {
                //if (_queue->_abort_request) {
                if (_queue->packet_queue_get_abort()) {
                    // 是否请求退出
                    return -1;
                }

                // 1.2. 获取解码帧。avcodec_receive_frame好像是要在avcodec_send_packet前接收的，可以看看以前的笔记。
                switch (_avctx->codec_type) {
                case AVMEDIA_TYPE_VIDEO:
                    ret = avcodec_receive_frame(_avctx, frame);    // 这里是拿到真正解码一帧的数据，并且best_effort_timestamp、pts均是这个函数内部赋值的
                    //printf("frame pts:%ld, dts:%ld\n", frame->pts, frame->pkt_dts);
                    if (ret >= 0) {
                        //if (decoder_reorder_pts == -1)
                        if (-1 == -1) {
                            // 正常走这里的路径。使用各种启发式算法估计帧时间戳，单位为流的时基。
                            frame->pts = frame->best_effort_timestamp;
                        }
                        //else if (!decoder_reorder_pts) {
                        else if (0) {
                            frame->pts = frame->pkt_dts;
                        }
                    }
                    break;

                case AVMEDIA_TYPE_AUDIO:
                    ret = avcodec_receive_frame(_avctx, frame);
                    if (ret >= 0) {
                        AVRational tb = { 1, frame->sample_rate };    // 音频解码是转成以采样率作为tb计算pts的
                        if (frame->pts != AV_NOPTS_VALUE) {
                            /*
                            * 如果frame->pts正常则先将其从pkt_timebase转成{1, frame->sample_rate}。
                            * pkt_timebase实质就是stream->time_base。
                            * av_rescale_q公式：axbq/cq=ax(numb/denb)/(numc/denc);
                            * 而numb和numc通常是1，所以axbq/cq=ax(1/denb)/(1/denc)=(a/denb)*denc;
                            * 既有：a = a*denc / denb;  此时a的单位是采样频率。所以下面统计下一帧的next_pts时，
                            * 可以直接加上采样点个数nb_samples。因为此时frame->pts可以认为就是已经采样到的采样点总个数。
                            * 例如采样率=44100，nb_samples=1024，那么加上1024相当于加上1024/44.1k=0.023s。
                            * 例如采样率=8000，nb_samples=320，那么加上320相当于加上320/8k=0.04s。
                            * 更具体(以采样率=8k为例)：首帧frame->pts=8696，此时单位是采样频率，并且采样点按照320递增，那么下一帧frame->pts=8696+320=9016.
                            * 那么即使按照采样频率为单位，那么8696/8k=1.087;9016/8l=1.127。 1.127-1.087=0.04s，仍然是一帧的间隔，那么这样算就是没错的。
                            *
                            * 注意：有人可能在debug时出现这个疑惑：第一次进来获取到解码帧frame->pts=8696，它是代表这一帧或者这一秒的采样个数吗？
                            * 如果是的话，1s内不是最大采集8000个采样点吗？ 那按照pts是采样点个数的话，不是超过这个范围了吗？
                            *  答(个人理解)：首次进来frame->pts=8696，意思并不是这一帧或者这一秒的采样个数，而应该是此时读到的一个初始值，这个值可以认为是用户从网络将包解码后输入解码器。
                            *  解码器再根据其进行赋了一个初始值而已，所以它此时应该代表这个采样点个数的起始值，相当于我们传统的0初始值而已。
                            *  通过debug发现，频率=8k时，每帧的采样点个数是320左右。44.1k，采样点个数是1024左右。
                            * 实际上也类似视频，视频起始frame->pts值一般也是非0值，例如192000，单位是90k，那么起始大概是2.1333s。
                            */
                            frame->pts = av_rescale_q(frame->pts, _avctx->pkt_timebase, tb);//将参2单位转成参3单位
                        }
                        else if (_next_pts != AV_NOPTS_VALUE) {
                            // 如果frame->pts不正常则使用上一帧更新的next_pts和next_pts_tb。
                            // 转成{1, frame->sample_rate}。
                            frame->pts = av_rescale_q(_next_pts, _next_pts_tb, tb);
                        }
                        if (frame->pts != AV_NOPTS_VALUE) {
                            // 记录下一帧的pts和tb。 根据当前帧的pts和nb_samples预估下一帧的pts。
                            // 首次进来之前next_pts、next_pts_tb是未赋值的。
                            _next_pts = frame->pts + frame->nb_samples;
                            _next_pts_tb = tb; // 设置timebase。
                        }
                    }
                    break;

                }// <== switch end ==>

                 // 1.3. 检查解码是否已经结束，解码结束返回0
                if (ret == AVERROR_EOF) {
                    _finished = _pkt_serial;        // 这里看到，解码结束后，会标记封装的解码器的finished=pkt_serial。
                    //printf("avcodec_flush_buffers %s(%d)\n", __FUNCTION__, __LINE__);
                    SPDWARN("avcodec_flush_buffers decoder_decode_frame end, {}", _avctx->codec_type);
                    avcodec_flush_buffers(_avctx);
                    return 0;
                }

                // 1.4. 正常解码返回1
                if (ret >= 0)
                    return 1;

            } while (ret != AVERROR(EAGAIN));   // 1.5 没帧可读时ret返回EAGIN，需要继续送packet

        }// <== if(d->queue->serial == d->pkt_serial) end ==>

         // 若进了if，上面dowhile退出是因为解码器avcodec_receive_frame时返回了EAGAIN。

         // 2 获取一个packet，如果播放序列不一致(数据不连续)则过滤掉“过时”的packet
        do {
            // 2.1 如果没有数据可读则唤醒read_thread, 实际是continue_read_thread SDL_cond
            // 这里实际判断是大概，因为读线程此时可能上锁往队列扔包，但这的判断并未上锁，当然这里只是唤醒催读线程，不做也可以，但做了更好。
            //if (_queue->_nb_packets == 0)
            if (_queue->packet_queue_get_packets() == 0)
                SDL_CondSignal(_empty_queue_cond);// 通知read_thread放入packet

            // 2.2 如果还有pending的packet则使用它。 packet_pending为1表示解码器异常导致有个包没成功解码，需要重新解码
            if (_packet_pending) {
                av_packet_move_ref(&pkt, &_pkt);// 将参2中的每个字段移动到参1并重置参2。类似unique_lock的所有权变动
                _packet_pending = 0;
            }
            else {
                // 2.3 阻塞式读取packet。-1表示用户请求中断。
                if (_queue->packet_queue_get(&pkt, 1, &_pkt_serial) < 0)
                    return -1;
            }
            //if (_queue->_serial != _pkt_serial) {
            if (*_queue->packet_queue_get_serial() != _pkt_serial) {
                // tyycode
                //printf("+++++%s(%d) discontinue:queue->serial: %d, pkt_serial: %d\n", __FUNCTION__, __LINE__, *(_queue->packet_queue_get_serial()), _pkt_serial);
                SPDWARN("+++++ discontinue:queue->serial: {}, pkt_serial: {}", *(_queue->packet_queue_get_serial()), _pkt_serial);
                av_packet_unref(&pkt); // fixed me? 释放要过滤的packet
            }
        } while (*_queue->packet_queue_get_serial() != _pkt_serial);
        //} while (_queue->_serial != _pkt_serial);// 如果不是同一播放序列(流不连续)则继续读取

        // 到下面，拿到的pkt肯定是与最新的d->queue->serial相等。注意：d->pkt_serial的值是由packet_queue_get()的参4传入时，由队列节点MyAVPacketList的serial更新的。
        // 这样你就理解为什么是判断d->queue->serial != d->pkt_serial，而不是判断pkt->serial，更何况pkt本身就没有serial这个成员，MyAVPacketList才有。

        // 3 若packet是flush_pkt则不送，会重置解码器，清空解码器里面的缓存帧(说明seek到来)，否则将packet送入解码器。
        if (pkt.data == flush_pkt.data) {// flush_pkt也是符合d->queue->serial=d->pkt_serial的，所以需要处理。
                                         // when seeking or when switching to a different stream
                                         // 遇到seek操作或者切换国/粤时，重置，相当于重新开始播放。
            avcodec_flush_buffers(_avctx);    // 清空解码器里面的缓存帧
            _finished = 0;                    // 重置为0
            _next_pts = _start_pts;         // 主要用在了audio
            _next_pts_tb = _start_pts_tb;   // 主要用在了audio
        }
        else {
            if (_avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {// 主要是字幕相关，因为上面的switch (d->avctx->codec_type)是没有处理过字幕的内容。
                int got_frame = 0;
                ret = avcodec_decode_subtitle2(_avctx, sub, &got_frame, &pkt);//错误时返回负值，否则返回所使用的字节数。
                if (ret < 0) {
                    // 小于0，标记为AVERROR(EAGAIN)并释放这个pkt，等下一次for循环再继续get pkt去解码。
                    // 否则大于0的情况下，还需要看got_frame的值。
                    ret = AVERROR(EAGAIN);
                }
                else {
                    if (got_frame && !pkt.data) {// 解字幕成功，但是该pkt.data为空，这是什么意思，pkt.data可能为空吗？答：看注释，是可能的，因为输入输出有延时所以这个函数内部会
                                                 // 进行pkt.data置空，以冲刷解码器尾部的数据直到冲刷完成。所以会存在这种情况需要处理。
                        _packet_pending = 1;              // 标记为1.
                        av_packet_move_ref(&_pkt, &pkt);  // 此时d->pkt夺走该pkt的所有权。
                    }
                    // got_frame非0说明解字幕成功，为0说明没有字幕可以压缩。
                    // 看avcodec_decode_subtitle2函数的注释，可能出现四种情况：
                    //                1. got_frame=1，且pkt.data不为空，这是正常解码的情况.
                    //                2. got_frame=1，且pkt.data为空，解码器可能暂时出现问题，保存该pkt，EAGAIN，等下一次循环再使用该pkt进行解码.
                    //                3. got_frame=0，且pkt.data不为空，解码失败，释放该pkt，从包队列读取新的包进行解码，ret也是置为EAGAIN。
                    //                4. got_frame=0，且pkt.data为空，读取到空包进行解码了，说明解码结束，置为EOF。
                    ret = got_frame ? 0 : (pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
                }
            }
            else {
                if (avcodec_send_packet(_avctx, &pkt) == AVERROR(EAGAIN)) {
                    // 如果走进来这里，说明avcodec_send_packet、avcodec_receive_frame都返回EAGAIN，这是不正常的，需要保存这一个包到解码器中，等编码器恢复正常再拿来使用
                    av_log(_avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                    _packet_pending = 1;
                    av_packet_move_ref(&_pkt, &pkt);
                }
            }

            av_packet_unref(&pkt);	// 调用avcodec_send_packet后，pkt仍属于调用者，所以一定要自己去释放音视频数据
        }

    }//<== for end ==>

}

void Decoder::decoder_destroy() {
    av_packet_unref(&_pkt);
    avcodec_free_context(&_avctx);
}

void Decoder::decoder_abort(FrameQueue *fq)
{
    _queue->packet_queue_abort();   // 终止packet队列，packetQueue的abort_request被置为1
    fq->frame_queue_signal();         // 唤醒Frame队列, 以便退出.主要是唤醒帧队列阻塞在frame_queue_peek_writable/frame_queue_peek_readable的时候，以便让程序回到解码线程中退出。
    SDL_WaitThread(_decoder_tid, NULL);   // 上面唤醒帧队列后，就可以回收解码线程，然后等待解码线程退出
    _decoder_tid = NULL;          // 线程ID重置
    _queue->packet_queue_flush();   // 情况packet队列，并释放数据
}

int Decoder::decoder_start(int(*fn)(void *), const char *thread_name, void* arg) {
    _queue->packet_queue_start();                               // 启用对应的packet 队列。在decoder_init时，解码器的queue指向播放器对应的队列。
    _decoder_tid = SDL_CreateThread(fn, thread_name, arg);      // 创建解码线程
    if (!_decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }

    return 0;
}

void Decoder::decoder_set_start_pts(int64_t start_pts){
    _start_pts = start_pts;
}

void Decoder::decoder_set_start_pts_tb(AVRational start_pts_tb){
    _start_pts_tb = start_pts_tb;
}

int Decoder::decoder_get_serial(){
    return _pkt_serial;
}

int Decoder::decoder_get_finished(){
    return _finished;
}

void Decoder::decoder_set_finished(int finished){
    _finished = finished;
}

AVCodecContext *Decoder::decoder_get_avctx(){
    return _avctx;
}

#if 1

static const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {  // FFmpeg PIX_FMT to SDL_PIX的映射关系
    { AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
    { AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
    { AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
    { AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
    { AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
    { AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
    { AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
    { AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
    { AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
    { AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
    { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
    { AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
    { AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
    { AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
    { AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
    { AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
    { AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
    { AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN },
};


VideoState::VideoState() {
    static once_flag flaglog;
    std::call_once(flaglog, [&] {
        MySpdlog *myspdlog = MySpdlog::GetInstance();
        //myspdlog->SetLogPath(g_ProjectPath);
        myspdlog->SetLogLevel(1);
        myspdlog->ExecLog();
        int64_t t1, t2;
        t1 = av_gettime_relative();
        SPDINFO("ExecLog");
        t2 = av_gettime_relative();
        auto t3 = (t2 - t1) / 1000.0;
        SPDINFO("elp(ms): {}", t3);
    });
}

VideoState::~VideoState() {
    //stream_close();
    //if (_write_tid != NULL) {
    //	SDL_WaitThread(_write_tid, NULL);
    //	_write_tid = NULL;
    //}
}

int decode_interrupt_cb(void *ctx)
{
    VideoState *is = (VideoState *)ctx;
    return is->get_read_thread_abort();
    //return is->_abort_request;
}

int is_realtime(AVFormatContext *s)
{
    // 1. 根据输入的复用器的名字判断是否是网络流，一般是这个条件满足，例如s->iformat->name = "rtsp"。
    if (!strcmp(s->iformat->name, "rtp")
        || !strcmp(s->iformat->name, "rtsp")
        || !strcmp(s->iformat->name, "sdp")
        )
        return 1;

    // 2. 根据是否打开了网络io和url前缀是否是对应的网络流协议来判断
    if (s->pb && (!strncmp(s->url, "rtp:", 4)
        || !strncmp(s->url, "udp:", 4)
        )
        )
        return 1;
    return 0;
}

int64_t VideoState::get_valid_channel_layout(int64_t channel_layout, int channels)
{
    // av_get_channel_layout_nb_channels: 返回通道布局中的通道数量。
    if (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels)
        return channel_layout;
    else
        return 0;
}

// 本函数不用考虑内存的回收.
#if CONFIG_AVFILTER
int VideoState::configure_filtergraph(AVFilterGraph *graph, const char *filtergraph, AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret, i;
    int nb_filters = graph->nb_filters;     // 不考虑filtergraph是否有值，先保存简单过滤器的个数
    AVFilterInOut *outputs = NULL, *inputs = NULL;

    // 1. 判断用户是否输入了复杂的过滤器字符串，如果有，则在简单过滤器结构的基础上，再添加复杂过滤的结构。
    if (filtergraph) {
        // 1.1 开辟输入输出的AVFilterInOut内存(09-05-crop-flip没开，开不开应该问题不大，不过还没测试过)
        // 注：如果有复杂的滤波器过程，使用AVFilterInOut进行连接；没有则则直接调用avfilter_link进行连接。
        outputs = avfilter_inout_alloc();
        inputs = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        outputs->name = av_strdup("in");
        outputs->filter_ctx = source_ctx;       // 输入的AVFilterInOut->filter_ctx附属在输入源AVFilterContext上(看09-05的滤波过程图)
        outputs->pad_idx = 0;					// 输入的pad端口，一个滤波器可以有多个输入。
        outputs->next = NULL;

        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink_ctx;         // 输出的AVFilterInOut->filter_ctx附属在输出源AVFilterContext上
        inputs->pad_idx = 0;				   // 输出的pad端口，一个滤波器可以有多个输出。因为上面输入是0，那么我们输出也是0。
        inputs->next = NULL;
        // 这里留个疑问：为啥outputs指向源AVFilterContext，而inputs指向输出AVFilterContext？
        // 09-05-crop-flip的例子在解析复杂字符串时，不需要对AVFilterInOut *outputs, *inputs;进行这个8行语句赋值，后续可以自行测试。

        // 1.2 1）解析字符串；2）并且将滤波图的集合放在inputs、outputs中。 与avfilter_graph_parse2功能是一样的，参考(09-05-crop-flip)
        // 注意这里成功的话，根据复杂的字符串结构，输入输出的AVFilterContext会有Link操作，所以不需要再次调用avfilter_link
        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
            goto fail;
    }
    // 2. 否则将输入输出的AVFilterContext进行Link
    else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }

    /*
    * Reorder the filters to ensure that inputs of the custom filters are merged first。
    * 3. 重新排序筛选器，以确保自定义筛选器的输入首先合并。
    * 因为我们这里是可能添加了一些复杂的过滤器，所以需要进行重排。例如简单过滤器是2个，复杂过滤器1个。
    * 此时graph->nb_filters=3，原本的nb_filters=2，那么需要对3-2该过滤器进行重排。
    * 重排算法也很简单，就是将复杂过滤器往最前面放，例如上面例子，假设下标0、1是输入输出，2是复杂过滤器，那么
    * 换完0、1、2下标分别是：复杂过滤器、输出过滤器、输入过滤器。当然不一定准确，我们也不需要非得理解内部处理，
    * 只需要知道这样去做即可，当然你也可以深入，但感觉没必要浪费时间。
    */
    for (i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

    // 4. 提交整个滤波图
    ret = avfilter_graph_config(graph, NULL);

fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);

    return ret;
}

// 该函数只需要回收_agraph，filt_asrc、filt_asink是不需要回收的
int VideoState::configure_audio_filters(const char *afilters, int force_output_format)
{
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };// 初始化过去的asink输出是s16格式。
    int sample_rates[2] = { 0, -1 };
    int64_t channel_layouts[2] = { 0, -1 };
    int channels[2] = { 0, -1 };
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    char aresample_swr_opts[512] = "";
    AVDictionaryEntry *e = NULL;
    char asrc_args[256];
    int ret;

    // 1.  开辟系统管理avfilter的结构体
    avfilter_graph_free(&_agraph);
    if (!(_agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    _agraph->nb_threads = _filter_nbthreads;// 一般是0

    // 2. 遍历字典sws_dict，并将里面的选项以key=value的形式保存在aresample_swr_opts，并最终使用av_opt_set设置到系统过滤器。
    while ((e = av_dict_get(_swr_opts, "", e, AV_DICT_IGNORE_SUFFIX)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);// av_strlcatf是追加字符串

    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts) - 1] = '\0';// 长度末尾补0，所以上面追加完字符串的冒号：能被去掉。

    // 把_swr_opts的内容设置到_agraph中，那么此时这个系统过滤器在过滤时就会使用这些选项。see https://blog.csdn.net/qq_17368865/article/details/79101659
    // 等价于：av_opt_set(pCodecCtx,“b”,”400000”,0);等参数的设置。所以与下面configure_filtergraph是否设置复杂过滤器字符串是不一样的。
    av_opt_set(_agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    // 3. 使用简单方法创建输入滤波器AVFilterContext(获取输入源filter--->buffer). 输入源都是需要传asrc_args相关参数的。
    // 此时输入源的audio_filter_src是从AVCodecContext读取的。
    // 创建简单的滤波器只需要在avfilter_graph_create_filter时添加字符串参数asrc_args；而复杂的滤波器需要用到avfilter_graph_parse2解析字符串。
    // 3.1 创建输入滤波器AVFilterContext，并设置音频相关参数。
    ret = snprintf(asrc_args, sizeof(asrc_args),
        "sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
        _audio_filter_src.freq, av_get_sample_fmt_name(_audio_filter_src.fmt), _audio_filter_src.channels, 1, _audio_filter_src.freq);
    if (_audio_filter_src.channel_layout)// 如果通道布局不为0，把通道布局也设置到输入滤波器中。
        snprintf(asrc_args + ret, sizeof(asrc_args) - ret, ":channel_layout=0x%lld", _audio_filter_src.channel_layout);

    ret = avfilter_graph_create_filter(&filt_asrc, avfilter_get_by_name("abuffer"), "ffplay_abuffer", asrc_args, NULL, _agraph);
    if (ret < 0)
        goto end;

    // 4. 使用简单方法创建输出滤波器AVFilterContext
    // 注：
    // 1）下面abuffer、abuffersink(视频是buffer、buffersink)的名字应该是固定的，在复杂字符串过滤器的get例子同理，不过前后需要补一些字符串内容。
    // 例如 mainsrc_ctx = avfilter_graph_get_filter(filter_graph, "Parsed_buffer_0");
    // 这个参2的名字一定是 "Parsed_" + "系统过滤器名字" + "_" + "本次字符串中系统过滤器的序号" 的格式吗？
    // 确实是，可以看filter_graph->filters->name变量
    // 2）而下面ffplay_abuffer、ffplay_abuffersink可以任意。
    ret = avfilter_graph_create_filter(&filt_asink, avfilter_get_by_name("abuffersink"), "ffplay_abuffersink", NULL, NULL, _agraph);
    if (ret < 0)
        goto end;

    /*
    * 1）av_int_list_length： #define av_int_list_length(list, term)	av_int_list_length_for_size(sizeof(*(list)), list, term)。
    * 参1 list：指向列表的指针。
    * 参2 term：列表终结符，通常是0或者-1。
    * 返回值：返回列表的长度(即返回元素的长度，例如"abc"，那么返回3.)，以元素为单位，不计算终止符。
    * 该函数具体看av_int_list_length_for_size。
    *
    * 2）av_int_list_length_for_size：unsigned av_int_list_length_for_size	(unsigned elsize, const void *list, uint64_t term)。
    * 参1 elsize：每个列表元素的字节大小(只有1、2、4或8)。例如下面的传进来是sample_fmts,  AV_SAMPLE_FMT_NONE。
    *              那么elsize=sizeof(*(list))=8字节。  数组以指针的大小计算，64位机是8字节，32位机器是4字节。
    * 参2 list：指向列表的指针。
    * 参3 term：列表终结符，通常是0或者-1。
    * 返回值：返回列表的长度(即返回元素的个数，例如"abc"，那么返回3.)，以元素为单位，不计算终止符。
    *
    * 3）av_opt_set_bin：就是和av_opt_set这些一样，只是参数3后面不一样而已，没啥好说的。
    *
    * 4）所以av_opt_set_int_list：av_opt_set_int_list(obj, name, val, term, flags)。
    * 参1 obj：一个带有AVClass结构体成员的结构体，并且AVClass是该结构体首个成员，由于该结构体首地址和第一个成员(即AVClass)是一样的，
    * 所以看到FFmpeg描述av_opt_set_int_list的obj参数时，写成了"要设置选项的AVClass对象"，其实看回av_opt_set等函数族的参数描述---"第一个元素是AVClass指针的结构体。"
    * 两者意思是一样的，只是表达不一样。
    * 参2 name：要设置的key选项。
    * 参3 val：key选项的值。
    * 参4 term 终结符。
    * 参5：查找方式，暂未深入该参数。
    * 返回值：0成功，负数失败。
    *
    * 即总结av_opt_set_int_list函数作用：先检查val值是否合法，否则使用二进制的方式进行设置到AVClass中的option成员当中。
    * 一般支持av_opt_set的有AVFilterContext、AVFormatContext、AVCodecContext，他们内部的第一个成员必定是AVClass，
    * 并且AVClass的option成员指向对应的静态数组，以便查看用户设置的选项是否被支持。
    *
    * 注：INT_MAX、INT_MIN是 'signed int'可以保存的最小值和最大值。
    *
    * 5）关于av_opt_set函数族的使用和理解，可参考https://blog.csdn.net/qq_17368865/article/details/79101659。
    * 6）关于官方文档的参数和返回值的理解(包含源码)，可参考：http://ffmpeg.org/doxygen/trunk/group__opt__set__funcs.html#gac06fc2b2e32f67f067ed7aaec163447f。
    */

    // 初始化设置音频输出过滤器filt_asink为s16和all_channel_counts=1.
    if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts, AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    // 5. 若指定强制输出音频格式，则先初始相关参数到输出过滤器filt_asink，以准备重采样.
    // 不过在这里还没有调audio_open，所以audio_tgt内部全是0，即这里都是初始化。
    // 通过FFmpeg文档，下面的初始化应该最终被设置到 BufferSinkContext。
    // see http://ffmpeg.org/doxygen/trunk/buffersink_8c_source.html
    if (force_output_format) {
        channel_layouts[0] = _audio_tgt.channel_layout;
        channels[0] = _audio_tgt.channels;
        sample_rates[0] = _audio_tgt.freq;
        if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_layouts", channel_layouts, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_counts", channels, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "sample_rates", sample_rates, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
    }

    // 上面看到，原过滤器和输出过滤器是没有Link的。

    // 6. 判断是否需要添加复杂过滤器，并且将输入输出过滤器AVFilterContext进行Link，最后提交整个滤波图。
    //      不管视频还是音频，都是调用这个函数处理复杂过滤器字符串。
    if ((ret = configure_filtergraph(_agraph, afilters, filt_asrc, filt_asink)) < 0)
        goto end;

    // 7. 经过上面的过程处理后，得到输出的过滤器filt_asink，并保存输入的过滤器filt_asrc
    _in_audio_filter = filt_asrc;
    _out_audio_filter = filt_asink;

end:
    if (ret < 0) {
        avfilter_graph_free(&_agraph);
        _agraph = NULL;
    }


    return ret;
}
#endif  /* CONFIG_AVFILTER */

int VideoState::get_master_sync_type() {
    if (_av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (_video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;	 /* 如果没有视频成分则使用 audio master */
    }
    else if (_av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (_audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            //return AV_SYNC_EXTERNAL_CLOCK;	 /* 没有音频的时候那就用外部时钟 */
            return AV_SYNC_VIDEO_MASTER;		 /* 没有音频的时候那就用视频时钟 */
    }
    else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

double VideoState::get_master_clock()
{
    double val;

    switch (get_master_sync_type()) {
    case AV_SYNC_VIDEO_MASTER:
        val = _vidclk.get_clock();
        break;
    case AV_SYNC_AUDIO_MASTER:
        val = _audclk.get_clock();
        break;
    default:
        val = _extclk.get_clock();// get_clock是获取随着实时时间流逝的pts，很简单。
        break;
    }
    return val;
}

int VideoState::synchronize_audio(int nb_samples)
{
    int wanted_nb_samples = nb_samples;

    // 1. 视频、外部是主时钟，需要调整帧的采样点个数
    /* if not master, then we try to remove or add samples to correct the clock */
    if (get_master_sync_type() != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        // 1.1 求出音频时钟与主时钟的差值。正数音频超前，负数音频落后。单位秒。
        diff = _audclk.get_clock() - get_master_clock();

        // 1.2 误差在 AV_NOSYNC_THRESHOLD(10s) 范围再来看看要不要调整。
        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            //printf("1 audio_diff_cum: %lf, diff: %lf, audio_diff_avg_coef: %lf\n", is->audio_diff_cum, diff, is->audio_diff_avg_coef);
            /*
            * audio_diff_cum代表本次的误差(diff)，加上历史的权重比误差(is->audio_diff_avg_coef * is->audio_diff_cum)之和。
            * 并且AUDIO_DIFF_AVG_NB次数越多，历史的权重比误差占比越小。
            */
            _audio_diff_cum = diff + _audio_diff_avg_coef * _audio_diff_cum;
            //printf("2 audio_diff_cum: %lf\n", is->audio_diff_cum);

            // 1.3 连续20次不同步才进行校正，不到也会直接返回原本的采样点个数。
            if (_audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate(没有足够的度量来做出正确的估计) */
                _audio_diff_avg_count++;
            }
            else {
                // 1.4 否则进行校正，但依然需要判断。
                /* estimate the A-V difference。估算A-V的差值。
                * 计算后的avg_diff并不是一次过校正完本次diff的全部误差，而是先校正一部分，剩下的根据下一次的误差在校正。
                * 例如假设AUDIO_DIFF_AVG_NB=4，前3次的diff分别是2s、2s、1s，本次的diff是2s，那么得出：audio_diff_cum=5.104s.
                * 所以avg_diff=5.104x0.2=1.0208s，而不是校正完本次的2s。也就是平滑校正，这样做可以让下面的采样点变化尽量小，从而优化声音的变尖或者变粗。
                * 因为同样的频率下，采样点变大或者变小了，会导致声音变形。
                */
                avg_diff = _audio_diff_cum * (1.0 - _audio_diff_avg_coef);
                //avg_diff = diff; // 这样也可以，但是没有上面平滑。

                // 1.5 如果avg_diff大于同步阈值，则进行调整，否则在[-audio_diff_threshold, +audio_diff_threshold]范围内不调整，与视频类似。
                if (fabs(avg_diff) >= _audio_diff_threshold) {
                    /*
                    * 根据公式：采样点个数/采样频率=秒数，得出：采样点个数=秒数*采样频率。
                    * 例如diff超前了0.02s，采样频率是44.1k，那么求出采样点个数是：0.02s*44.1k=882.这个值代表超前的采样点个数。
                    * 然后wanted_nb_samples=原本采样点 + 求出的超前的采样点个数；例如1024+882.
                    * 上面也说到了，ffplay考虑到同样频率下，采样点变化过大或者过小的情况，会有一个变化区间[min_nb_samples，max_nb_samples]
                    * 所以本次同步后采样点个数变成：1024*110/100=1126.4=1126左右(取整)。
                    *
                    * 注意是乘以is->audio_src.freq变量的采样频率值，是从audio_decode_frame()每次在重采样参数改变后，从解码帧中获取的值，也就是上一次的帧的采样频率值。
                    */
                    wanted_nb_samples = nb_samples + (int)(diff * _audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    // av_clip 用来限制wanted_nb_samples最终落在 min_nb_samples~max_nb_samples
                    // nb_samples *（90%~110%）
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }

                /*
                * (非音频才会打印，例如视频同步-sync video)
                * 这里看打印看前三个值即可：
                * 1）例如：diff=0.402868 adiff=0.388198 sample_diff=32 apts=2.280 audio_diff_threshold=0.064000
                * 上面看到前3个值都是正数，说明此时是超前，采样点需要增加。
                * 2）例如：diff=-1.154783 adiff=-1.137120 sample_diff=-32 apts=2.240 audio_diff_threshold=0.064000
                * 上面看到前3个值都是负数，说明此时是落后，采样点需要减少。
                *
                * 3）总结：若超前，前3个打印值都是正数；若落后，前3个打印值都是负数。
                */
                //av_log(NULL, AV_LOG_INFO, "diff=%f avgdiff=%f sample_diff=%d apts=%0.3f audio_diff_threshold=%f\n",
                //	diff, avg_diff, wanted_nb_samples - nb_samples,
                //	is->audio_clock, is->audio_diff_threshold);
            }
        }
        else {
            // 大于 AV_NOSYNC_THRESHOLD 阈值，该干嘛就干嘛，不做处理了。
            /* too big difference : may be initial PTS errors, so
            reset A-V filter */
            _audio_diff_avg_count = 0;
            _audio_diff_cum = 0;   // 恢复正常后重置为0
        }
    }

    // 2. 否则是音频主时钟直接返回原采样点个数
    return wanted_nb_samples;
}

int VideoState::audio_decode_frame()
{
    int data_size, resampled_data_size;
    int64_t dec_channel_layout;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;

    // 暂停直接返回-1，让音频回调补充0数据，播放静音。
    if (_paused)
        return -1;

    /* 1. 获取一帧解码后的音频帧。
    * 1.1 首先判断音频帧队列是否有未显示的，如果有则获取并出队，但是若serial不是最新会重新读取(因为在解码线程看到，帧队列是有可能含有不是最新的serial的帧)；
    *      没有则判断，若经过的时间大于阈值的一半，直接返回-1，否则休眠1ms再判断。
    *
    * 注:
    * 留个疑问1：若休眠完后，刚好一帧数据被读走，在frame_queue_peek_readable会阻塞在条件变量吗？
    * 答：应该不存在，因为本场景只有这里读取音频帧而已，并未有其它地方竞争读取。
    * 留个疑问2：读取到的帧会因frame_queue_unref_item被清空掉数据吗？
    * 答：不会，音频帧队列同样是keep_last+rindex_shown机制，第一次不会进行释放，所以后面也不会。
    */
    do {
#if defined(_WIN32)
        while (_sampq.frame_queue_nb_remaining() == 0) {
            /*
            * 1）(av_gettime_relative() - audio_callback_time)：此次SDL回调sdl_audio_callback到现在的时间。
            * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec：阈值，和audio_diff_threshold算法一样。除以2代表阈值的一半。乘以1000000LL代表需要单位相同才能比较。
            * if表示：若帧队列一直没数据并超过阈值的一半时间，则补充数据。若第一次帧队列为空，并且满足if，补充一次数据；若还是同样的SDL回调，并且第二次帧队列仍是空，if肯定满足，
            * 因为audio_callback_time一样，而实时时间增大，所以继续补充数据，直至补充完SDL的len。
            *
            * 2）更深层次的理解，is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec实际是一帧音频的播放时长，
            * 因为audio_hw_buf_size就是在audio_open打开时返回的音频缓存大小，单位是字节，通过采样点个数求出；而audio_tgt.bytes_per_sec就是通过ch*fmt*freq求出，单位也是字节。
            * 加上平时求音频一帧播放时长：采样点个数(samples)/采样频率(freq)；根据两者求出的播放时长是一样的，那么有公式：
            * audio_hw_buf_size / audio_tgt.bytes_per_sec = samples / freq; 代入audio_tgt.bytes_per_sec = ch*fmt*freq；
            * audio_hw_buf_size / ch*fmt*freq = samples / freq;化简后：
            * audio_hw_buf_size / ch*fmt = samples; 因为这里是采用s16的交错模式进行输出的，所以ch=1，fmt=2。最终得出：
            * audio_hw_buf_size / 2 = samples；
            * 根据s16格式，采样点个数和字节数就是2倍的关系，所以推断的公式是完全成立的。
            *
            * 3）换句话说，下面if意思就简单了，即：若帧队列一直没数据，并且每次调用audio_decode_frame都超过一帧的一半时长，那么补充默认的数据。
            *
            * 注：上面都是按照 一次SDL回调sdl_audio_callback 进行解释的。
            */
            if ((av_gettime_relative() - _audio_callback_time) > 1000000LL * _audio_hw_buf_size / _audio_tgt.bytes_per_sec / 2) {
                return -1;
            }

            av_usleep(1000);
        }
#endif
        // 若队列头部可读，则由af指向可读帧
        if (!(af = _sampq.frame_queue_peek_readable()))
            return -1;

        _sampq.frame_queue_next();
    //} while (af->serial != _audioq._serial);
      } while (af->serial != *_audioq.packet_queue_get_serial());

    // 2. 根据frame中指定的音频参数获取缓冲区的大小 af->frame->channels * af->frame->nb_samples * 2
    data_size = av_samples_get_buffer_size(NULL, af->frame->channels, af->frame->nb_samples, (AVSampleFormat)af->frame->format, 1);
    // 3. 获取声道布局。
    // 获取规则：若存在通道布局 且 通道布局获取的通道数和已有通道数相等，则获取该通道数；
    //          否则根据已有通道数来获取默认的通道布局。
    dec_channel_layout =
        (af->frame->channel_layout &&
            af->frame->channels == av_get_channel_layout_nb_channels(af->frame->channel_layout)) ?
        af->frame->channel_layout : av_get_default_channel_layout(af->frame->channels);

    // 4. 获取样本数校正值：若同步时钟是音频，则不调整样本数；否则根据同步需要调整样本数
    /*
    * 1）音频如何同步到视频？
    *  答：类似视频同步到音频(音频为主时钟)。视频为主时钟的话，无非就是音频超前或者音频落后。
    *      而音频超前了，只要多放几个采样点就能等待视频；音频落后了，那么就少播放几个采样点就能追上视频，这是视频为主时钟的主要思路。
    *      但是注意，音频的丢弃或者增长并不能随意增加，必须通过重采样进行，如果人为挑选某些采样点丢弃或者增加，会导致音频不连续，这与视频有区别。
    *      具体可以看pdf。
    * 2）音频同步到视频为啥在audio_decode_frame()函数做呢？
    *  答：参考视频，都是获取到一帧数据后，然后判断其是否能够显示，如果能则直接显示；否则进行相应的休眠处理。
    * 3）在2）的基础上，即音频同步到视频为啥在audio_decode_frame()函数做，并且要在重采样之前做呢？
    *  答：只是因为，想要重采样，就必须知道重采样想要采样点的个数，那么这个想要的采样点个数如何获取？就是根据音频超前或者落后，
    *      得出对应的采样点，这样我们就能够进行重采样。否则重采样在同步之前处理，想要的采样点个数是未知的。
    *      在同步和重采样后，返回的数据就能通过SDL回调进行显示了，进而做到音视频同步。
    *
    */
    wanted_nb_samples = synchronize_audio(af->frame->nb_samples);

    // 5. 判断是否需要进行重采样。若需要这里先进行初始化。
    // is->audio_tgt是SDL可接受的音频帧数，是audio_open()中取得的参数
    // 在audio_open()函数中又有"is->audio_src = is->audio_tgt""
    // 此处表示：如果frame中的音频参数 == is->audio_src == is->audio_tgt，
    // 那音频重采样的过程就免了(因此时is->swr_ctr是NULL)
    // 否则使用frame(源)和is->audio_tgt(目标)中的音频参数来设置is->swr_ctx，
    // 并使用frame中的音频参数来赋值is->audio_src
    if (af->frame->format != _audio_src.fmt || // 采样格式
        dec_channel_layout != _audio_src.channel_layout || // 通道布局
        af->frame->sample_rate != _audio_src.freq || // 采样率
                                                        // 第4个条件, 要改变样本数量, 那就是需要初始化重采样。
                                                        // samples不同且swr_ctx没有初始化。 因为已经初始化可以直接重采样，和上面不一样，上面3个参数一旦改变，必须重新初始化。
        (wanted_nb_samples != af->frame->nb_samples && !_swr_ctx)
        )
    {
        swr_free(&_swr_ctx);
        // 5.1 开辟重采样器以及设置参数。
        _swr_ctx = swr_alloc_set_opts(NULL,
            _audio_tgt.channel_layout,  // 目标输出
            _audio_tgt.fmt,
            _audio_tgt.freq,
            dec_channel_layout,            // 数据源
            (AVSampleFormat)af->frame->format,
            af->frame->sample_rate,
            0, NULL);
        // 5.2 重采样器初始化。
        if (!_swr_ctx || swr_init(_swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                af->frame->sample_rate, av_get_sample_fmt_name((AVSampleFormat)af->frame->format), af->frame->channels,
                _audio_tgt.freq, av_get_sample_fmt_name(_audio_tgt.fmt), _audio_tgt.channels);
            swr_free(&_swr_ctx);
            return -1;
        }

        /*
        * 更新源音频信息。留个疑问，这样不会导致上面判断是否重采样错误吗？
        * 即本来输出设备只支持输出一种格式，输入设备是另一种格式，赋值后因为源和帧的格式相同，导致无法重采样到和输出格式一样的格式。
        * 答：不会，首先要理解audio_src、audio_tgt的作用。
        * 1）audio_src的作用是：保存上一次帧的重采样3元祖，用于判断是否需要重新初始化重采样器，
        * 由于首次时is->audio_src = is->audio_tgt，所以从输出过滤器获取的frame应该也是一样的。因为在audio_thread线程的configure_audio_filters配置
        * 输出过滤器时，会被强制配置与audio_tgt一样的格式。
        *
        * 2）audio_tgt的作用是：比较简单，就是固定为重采样后的输出格式，该变量从audio_open调用后，不会被改变，它是SDL从硬件设备读取到的硬件支持参数。
        * 3）那么理解这里的代码就简单了：
        *      例如开始有is->audio_src = is->audio_tgt，不会进行重采样；
        *      假设frame的重采样参数改变，if条件满足，那么重采样器初始化，audio_src更新为frame的参数；
        *      假设frame的重采样参数再次改变，frame与上一次的frame(即audio_src)不一样，if条件满足，那么需要进行重新重采样器初始化，以此类推。。。
        */
        _audio_src.channel_layout = dec_channel_layout;
        _audio_src.channels = af->frame->channels;
        _audio_src.freq = af->frame->sample_rate;
        _audio_src.fmt = (AVSampleFormat)af->frame->format;
    }

    // 5.3 开始重采样前的参数计算
    if (_swr_ctx) {
        // 5.3.1 获取重采样的输入数据
        // 重采样输入参数1：输入音频样本数是af->frame->nb_samples
        // 重采样输入参数2：输入音频缓冲区
        const uint8_t **in = (const uint8_t **)af->frame->extended_data; // data[0] data[1]

                                                                         // 5.3.2 获取重采样后的存储缓存，计算重采样输出的采样点个数、以及获取存储输出采样点个数的缓存总大小。
                                                                         // 具体算法详看，不难：https://blog.csdn.net/weixin_44517656/article/details/117849297
                                                                         // 重采样输出参数1：输出音频缓冲区尺寸
        uint8_t **out = &_audio_buf1; //真正分配缓存audio_buf1，最终给到audio_buf使用
                                         // 重采样输出参数2：输出音频缓冲区
        int out_count = (int64_t)wanted_nb_samples * _audio_tgt.freq / af->frame->sample_rate
            + 256;// 留个疑问，计算重采样输出的采样点个数为啥加上256？？？ see 下面if(len2 == out_count)的解释。
        int out_size = av_samples_get_buffer_size(NULL, _audio_tgt.channels,
            out_count, _audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }

        // 5.3.3 如果frame中的样本数经过校正，则条件成立，设置重采样补偿，当需要时FFmpeg会自己进行补偿。
        if (wanted_nb_samples != af->frame->nb_samples) {
            /*
            * 1）swr_set_compensation：激活重采样补偿(“软”补偿)。这个函数是在需要时在swr_next_pts()中内部调用。
            * 参1：重采样器。
            * 参2：每个样本PTS的增量(差距)。实际上就是校正前后求出的输出采样点个数之差。
            * 参3：需要补偿的样本数量。实际上就是校正后，输出的采样点数量。
            * 返回值：>=0成功，小于0返回错误码。
            */

            // 5.3.3.1 求出增量。
            // 算法也很简单，和上面求out_count是一样的：
            // 1）首先通过未经过修正的源采样点个数，求出原本正常的输出采样点个数：af->frame->nb_samples * is->audio_tgt.freq / af->frame->sample_rate;
            // 2）然后再通过经过校正后的采样点个数，求出输出采样点个数：wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate;
            // 3）然后2）-1），作减法合并表达式即可得到下面的公式。
            int sample_delta = (wanted_nb_samples - af->frame->nb_samples) * _audio_tgt.freq
                / af->frame->sample_rate;
            // 5.3.3.2 求出要补偿的样本数量。注意，要补偿的样本数量指的是校正后的采样点个数，不要将其当成5.3.3.1。
            int compensation_distance = wanted_nb_samples * _audio_tgt.freq / af->frame->sample_rate;
            // swr_set_compensation
            if (swr_set_compensation(_swr_ctx, sample_delta, compensation_distance) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }

        // 5.4 为audio_buf1开辟内存。
        // 1）av_fast_realloc：在buffer不足的情况下，重新分配内存(内部应该会把旧的释放，具体看源码)，否则不做处理。
        // 2）av_fast_malloc：与av_fast_realloc一样，但FFMPEG官方说更安全高效，避免可能发生内存泄漏。see http://ffmpeg.org/pipermail/ffmpeg-cvslog/2011-May/036992.html。
        // 3）关于FFMPEG更多的开辟堆内存操作，see https://www.cnblogs.com/tocy/p/ffmpeg-libavutil-details.html。
        av_fast_malloc(&_audio_buf1, &_audio_buf1_size, out_size);// audio_buf1、audio_buf1_size初始值是NULL和0.
        if (!_audio_buf1)
            return AVERROR(ENOMEM);

        // 5.5 真正调用音频重采样的函数：返回值是重采样后得到的音频数据中单个声道的样本数。
        // swr_convert函数可以详看：https://blog.csdn.net/weixin_44517656/article/details/117849297
        len2 = swr_convert(_swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            /*
            * 这里看到ffpaly的做法，当重采样后返回的样本数和缓冲区相等，他认为缓冲区太小了。
            * 也就是说，他上面计算输出样本数的缓冲区大小时，加上256的目的就是为了扩大缓冲区，而不是增加输出的采样点个数。
            * 从而得出，上面计算out_count的意思是指：音频输出缓冲区大小而不是指输出采样点个数。而在swr_convert参3你可以认为是输出采样点个数，
            * 因为即使你这样传，它也不会每次按照最大的输出采样点个数给你返回，例如out_count=1024，可能实际返回给你921.
            * 这也就是我们可以对这个out_count进行调整增大的意思。
            *
            * 这里他重新初始化了重采样器，具体目的未知，后续可以自己研究。可以这样做：把swr_init注释掉，
            * 并添加对应打印或者getchar()出现时，让它停在这里。或者添加打印观察，看视频有什么异常。
            */
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(_swr_ctx) < 0)
                swr_free(&_swr_ctx);
        }

        // 5.7 保存采样后的数据以及重采样返回的一帧音频数据大小(以字节为单位)
        // 这里可以看到，audio_buf1是实际开辟了内存的，而audio_buf只是简单指向它。
        _audio_buf = _audio_buf1;
        resampled_data_size = len2 * _audio_tgt.channels * av_get_bytes_per_sample(_audio_tgt.fmt);// 获取重采样帧的字节大小，也可av_samples_get_buffer_size()获取
    }
    else {
        // 未经重采样，则将指针指向frame中的音频数据
        _audio_buf = af->frame->data[0]; // s16交错模式data[0], fltp data[0] data[1]
        resampled_data_size = data_size;
    }

    // 6. 更新音频时钟audio_clock与audio_clock_serial。但是注意没有更新is->audclk这个变量的时钟，留个疑问。
    audio_clock0 = _audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(af->pts))            // (double) af->frame->nb_samples / af->frame->sample_rate求出的就是一个音频帧所占时长
        _audio_clock = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
    else
        _audio_clock = NAN;
    _audio_clock_serial = af->serial;// 帧的serial由is->auddec.pkt_serial获取。

#ifdef DEBUG
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
            is->audio_clock - last_clock,
            is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif

    // 7. 返回一帧音频数据的实际大小，无论是否进行了重采样。
    return resampled_data_size;
}

void VideoState::SdlAudioCallback(void *opaque, Uint8 *stream, int len){
    int audio_size, len1;

    // 在while调用audio_decode_frame可能产生延迟，当超过一定延时时需要进行处理。例如获取不到解码帧，需要补充数据。
    // audio_callback_time就是这个作用(作用之一，下面尾部还有一个另外的作用)，用于判断每次补充数据时，若此时帧队列没数据，
    // 则是否已经超过一定阈值，若超过需要人为进行补充数据。  至于补多少次，由 SDL的传入参数len 和 帧队列是否有数据 决定。
    _audio_callback_time = av_gettime_relative();

    // 1. 循环读取，直到读取到SDL需要的足够的数据len，才会退出循环。
    while (len > 0) {
        /* 读取逻辑：
        * (1)如果is->audio_buf_index >= is->audio_buf_size，说明audio_buf消耗完了，
        * 则调用audio_decode_frame重新填充audio_buf。
        *
        * (2)如果is->audio_buf_index < is->audio_buf_size则说明上次拷贝还剩余一些数据，
        * 先拷贝到stream再调用audio_decode_frame
        *
        */
        // 1.1 数据不足，进行补充.
        if (_audio_buf_index >= _audio_buf_size) {
            audio_size = audio_decode_frame();
            if (audio_size < 0) {
                /* if error, just output silence */
                /*
                * 当出现错误时，这里置audio_buf为空，不会拷贝数据到stream，不过因为memset stream为0，相当于拷贝了0数据，
                * 所以SDL这次回调会有静音数据。但是时钟还是需要进行更新。
                * 留个疑问，audio_decode_frame怎么才会返回负数？
                *
                * 注意：只有数据不足时，才有可能报错(因为只有消耗完数据才进来补充)。所以报错时将audio_buf置空，虽然audio_buf仍可能还有正常数据或者填充的0数据，
                * 但是已经被拷贝到stream了，所以置空是安全的(例以len=1024，每次补充512，第一次512正常，第二次补充失败的例子理解)，这一点对理解audio_callback_time有作用。
                */
                _audio_buf = NULL;
                // 这里不是相当于直接等于SDL_AUDIO_MIN_BUFFER_SIZE吗？ffplay又除以乘以搞得那么复杂？
                // ffpaly获取解码帧报错时，固定以512个字节填充。
                _audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / _audio_tgt.frame_size * _audio_tgt.frame_size;
            }
            else {
                //if (is->show_mode != VideoState::SHOW_MODE_VIDEO)// 显示音频波形，禁用视频才会进来。main函数添加：display_disable = 1;可先忽略。
                //	update_sample_display(is, (int16_t *)is->_audio_buf, audio_size);

                _audio_buf_size = audio_size; // 讲字节 多少字节。保存本次获取到的帧数据。
            }

            // 不管是否读取帧队列的数据成功，都要重置audio_buf_index。
            _audio_buf_index = 0;
        }

        // 来到这里，audio_buf肯定是有数据的，要么是刚补充完，要么是还还有剩余的数据。

        // 1.2 计算本次循环要拷贝的长度，根据缓冲区剩余大小量力而行进行拷贝。len是SDL本次回调要拷贝的总数据量，
        //      len1是本次循环拷贝的数据量，只是充当一个临时变量。
        len1 = _audio_buf_size - _audio_buf_index;
        if (len1 > len)  // 例如len1 = 4096 > len = 3000
            len1 = len;

        // 2. 拷贝数据到stream中，并会根据audio_volume决定如何输出audio_buf。
        /* 2.1 判断是否为静音，以及当前音量的大小，如果音量为最大则直接拷贝数据；否则进行混音。 */
        if (!_muted && _audio_buf && _audio_volume == SDL_MIX_MAXVOLUME)
            memcpy(stream, (uint8_t *)_audio_buf + _audio_buf_index, len1);
        else {
            /*
            * SDL_MixAudioFormat：进行混音。
            * 参1：混音输出目的地。
            * 参2：要进行混音的输入源。
            * 参3：表示所需的音频格式。
            * 参4：要混音的长度。
            * 参5：混音大小，范围[0,128]。
            */
            memset(stream, 0, len1);
            // 调整音量
            /* 如果处于mute状态则直接使用stream填0数据,因为上面memset为0了，暂停时is->audio_buf = NULL */
            if (!_muted && _audio_buf)
                SDL_MixAudioFormat(stream, (uint8_t *)_audio_buf + _audio_buf_index,
                    AUDIO_S16SYS, len1, _audio_volume);
        }

        // 3. 更新还能拷贝到SDL硬件缓冲区的大小len及其指针，以便进入下一次拷贝。
        //      不过只有当拷贝不足len时，才会进行第二次while，进行下一次的拷贝。
        len -= len1;
        stream += len1;

        /* 4. 更新is->audio_buf_index，指向audio_buf中未被拷贝到stream的数据（剩余数据）的起始位置 */
        _audio_buf_index += len1;

    }// <== while (len > 0) end ==>

     // 来到这里，说明本次回调已经从audio_buf拷贝到足够的数据到stream中了。

     // 5. 设置时钟。
     // 保存audio_buf还没写入SDL缓存的大小
    _audio_write_buf_size = _audio_buf_size - _audio_buf_index;

    if (!isnan(_audio_clock)) {
        _audclk.set_clock_at(_audio_clock - (double)(2 * _audio_hw_buf_size + _audio_write_buf_size) / _audio_tgt.bytes_per_sec,
            _audio_clock_serial,
            _audio_callback_time / 1000000.0);// 设置音频时钟。 audio_callback_time / 1000000.0代表SDL内部缓冲区的起始pts的那一刻的实时时间。
                                             // 因为本回调刚进来，说明SDL外部缓冲区开始补充数据，而SDL内部缓冲区刚好在播放数据。
        _extclk.sync_clock_to_slave(&_audclk);// 根据从时钟判断是否需要调整主时钟
    }
}

void sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
    VideoState *is = (VideoState*)opaque;
    int ret;

    is->SdlAudioCallback(opaque, stream, len);
}

// 该函数只需要回收_audio_dev。重点看wanted_spec.samples的计算以及SDL_OpenAudioDevice函数
int VideoState::audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
    SDL_AudioSpec wanted_spec, spec;// 期望的SDL参数，实际从硬件中获取到的SDL音频参数。
    const char *env;
    static const int next_nb_channels[] = { 0, 0, 1, 6, 2, 6, 4, 6 };// map表
    static const int next_sample_rates[] = { 0, 44100, 48000, 96000, 192000 };
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

    // 1. 若环境变量有设置，优先从环境变量取得声道数和声道布局
    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    }

    // 2. 如果通道布局是空或者通道布局与通道数不匹配，则按照默认的通道数进行获取通道布局。
    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        //#define AV_CH_LAYOUT_STEREO_DOWNMIX    (AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT)
        //#define AV_CH_STEREO_LEFT            0x20000000
        //#define AV_CH_STEREO_RIGHT           0x40000000
        // AV_CH_LAYOUT_STEREO_DOWNMIX=00100000 00000000 00000000 00000000 | 01000000 00000000 00000000 00000000 = 01100000 00000000 00000000 00000000
        // ~AV_CH_LAYOUT_STEREO_DOWNMIX = 10011111 11111111 11111111 11111111
        // 例如wanted_channel_layout=4时， 00000000 00000000 00000000 00000100 & 10011111 11111111 11111111 11111111 = 4。
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }

    // 3. 根据channel_layout获取nb_channels，当传入参数wanted_nb_channels不匹配时，此处会作修正。和2一样。
    wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.channels = wanted_nb_channels;

    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }

    // 4. 从采样率数组中找到第一个小于传入参数wanted_sample_rate的值
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;

    // 音频采样格式有两大类型：planar和packed，假设一个双声道音频文件，一个左声道采样点记作L，一个右声道采样点记作R，则：
    // planar存储格式：(plane1)LLLLLLLL...LLLL (plane2)RRRRRRRR...RRRR
    // packed存储格式：(plane1)LRLRLRLR...........................LRLR
    // 在这两种采样类型下，又细分多种采样格式，如AV_SAMPLE_FMT_S16、AV_SAMPLE_FMT_S16P等，
    // 注意SDL2.0目前不支持planar格式
    // channel_layout是int64_t类型，表示音频声道布局，每bit代表一个特定的声道，参考channel_layout.h中的定义，一目了然
    // 数据量(bits/秒) = 采样率(Hz) * 采样深度(bit) * 声道数。采样深度应该是指一个采样点所占的字节数，转换成bit单位即可。
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;

    /*
    * 5. 计算出样本中的音频缓冲区大小，即下面的wanted_spec.samples。
    * 理解音频缓存大小的一个重要公式推导：
    * 假如你希望1s装载 m 次 AudioBuffer(采样点个数)，那么1s装载的音频数据就是m x AudioBuffer。而音频播放1s需要多少数据？
    * 有个公式：1s数据大小 = 采样率 x 声道数 x 每个样本大小。
    * 所以AudioBuffer = （采样率 x 声道数 x 每个样本大小）/ m。
    * 由于为了适配各种音频输出，不会选择立体声，而选择交错模式s16进行输出，所以声道数(通道数)必定是1，每个样本大小占2字节。
    * 这里的m在ffplay是SDL_AUDIO_MAX_CALLBACKS_PER_SEC = 30.
    * 得出AudioBuffer = 采样率x1x2/m = 2x(采样率/m);
    * 但是不能直接这样带进去算，需要调用av_log2求出对应的幂次方数，最终确定每次采样点的最大个数。
    * 因为(采样率/m)就是平均每次的采样个数。
    * 所以最终公式应该是：AudioBuffer = 2^n。n由(采样率/m)求出。
    *
    * 也就得出下面的表达式：2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC);
    * 注意这里求出的AudioBuffer是采样点个数，在调用SDL_OpenAudioDevice后，会被转成字节单位保存在spec.size中。
    * 例如这里求出AudioBuffer=512采样点，假设s16，1个通道。那么spec.size=512x2x1=1024。
    * 再例如：AudioBuffer=2048采样点，假设s16，2个通道。那么spec.size=2048x2x2=8192。
    * spec.size与wanted_spec.samples的转换公式是：spec.size = samples * byte_per_sample * channels;
    *
    * 一次读取多长的数据
    * SDL_AUDIO_MAX_CALLBACKS_PER_SEC一秒最多回调次数，避免频繁的回调。
    *  Audio buffer size in samples (power of 2，即2的幂次方)。
    */
    // av_log2应该是对齐成2的n次方吧。例如freq=8000，每秒30次，最终返回8.具体可以看源码是如何处理的。
    // 大概估计是8k/30=266.6，2*2^7<266.7<2*2^8.因为缓存要大于实际的，所以返回8。
    // 44.1k/30=1470，2*2^9<1470<2*2^10.因为缓存要大于实际的，所以返回10。
    int tyycode1 = av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC);
    int tyycode2 = 2 << tyycode1;// 2左移8次方，即2*2的8次幂=512.多乘以一个2是因为它本身底数就是2了。
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;

    // 6. 打开音频设备并创建音频处理线程(实际是回调)。下面while如果打开音频失败，会一直尝试不同的通道、通道布局、帧率进行打开，直到帧率为0.
    // 期望的参数是wanted_spec，实际得到的硬件参数是spec。
    // 1) SDL提供两种使音频设备取得音频数据方法：
    //    a. push，SDL以特定的频率调用回调函数，在回调函数中取得音频数据
    //    b. pull，用户程序以特定的频率调用SDL_QueueAudio()，向音频设备提供数据。此种情况wanted_spec.callback=NULL
    // 2) 音频设备打开后播放静音，不启动回调，调用SDL_PauseAudio(0)后启动回调，开始正常播放音频
    /*
    * SDL_OpenAudioDevice()：
    * 参1：设备名字。最合理应该传NULL，为NULL时，等价于SDL_OpenAudio()。
    * 参2：一般传0即可。
    * 参3：期望的参数。
    * 参4：实际获取到的硬件参数。
    * 参5：一些权限宏的配置。
    * 返回值：0失败。>=2成功。因为1被旧版本的SDL_OpenAudio()占用了。
    */
    // 这个while循环可以参考https://blog.csdn.net/u012117034/article/details/122872548.
    while (!(_audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
            wanted_spec.channels, wanted_spec.freq, SDL_GetError());// 返回0，报警告，估计硬件不支持。
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {// 一直匹配不同的帧率通道，通道布局，直到帧率为0，返回-1。
                av_log(NULL, AV_LOG_ERROR, "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
    }

    // 7. 检查打开音频设备的实际参数：采样格式
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR,
            "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }

    // 8. 检查打开音频设备的实际参数：声道数
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);// 使用实际的硬件通道数获取通道布局。
        if (!wanted_channel_layout) {
            av_log(NULL, AV_LOG_ERROR,
                "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    // 9. 利用SDL获取到音频硬件的实际参数后，赋值给FFmpeg类型的结构，即指针形参audio_hw_params进行传出。
    // wanted_spec是期望的参数，spec是实际的参数，wanted_spec和spec都是SDL中的结构。
    // 此处audio_hw_params是FFmpeg中的参数，输出参数供上级函数使用
    // audio_hw_params保存的参数，就是在做重采样的时候要转成的格式。
    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels = spec.channels;
    /* audio_hw_params->frame_size这里只是计算一个采样点占用的字节数 */
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
    /* audio_hw_params->bytes_per_sec则是计算1s占用的字节数，会配合返回值spec.size，
    * 用来求算阈值audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec */
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }

    // 返回音频缓冲区大小，单位是字节，对比上面的wanted_spec.samples，单位是采样点。最终转换成字节结果是一样的。
    return spec.size;	/* SDL内部缓存的数据字节, samples * channels *byte_per_sample，在SDL_OpenAudioDevice打开时被改变。 */
}

inline int VideoState::cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
    enum AVSampleFormat fmt2, int64_t channel_count2)
{
    /*
    * If channel count == 1, planar and non-planar formats are the same。
    * 如果通道计数== 1，平面和非平面格式相同。
    *
    * 1）av_get_packed_sample_fmt：获取给定样本格式的包装替代形式，
    *      如果传入的sample_fmt已经是打包格式，则返回的格式与输入的格式相同。
    * 返回值：错误返回 给定样本格式的替代格式或AV_SAMPLE_FMT_NONE。
    *
    * 这样比是因为：当通道数都为1时，不管是包格式还是交错模式，返回都是包格式进行统一比较，即确保比较单位是一样的。
    * 如果通道数不是1的话，那就只能直接比较通道数和格式是否相等了。
    */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

int VideoState::AudioThread(){

    AVFrame *frame = av_frame_alloc();  // 分配解码帧
    Frame *af;
#if CONFIG_AVFILTER
    int last_serial = -1;
    int64_t dec_channel_layout;
    int reconfigure;
#endif
    int got_frame = 0;  // 是否读取到帧
    AVRational tb;      // timebase
    int ret = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    do {
        // 1. 读取解码帧
        if ((got_frame = _auddec.decoder_decode_frame(frame, NULL)) < 0)
            goto the_end;

        // 2. 读取帧成功
        if (got_frame) {
            tb = { 1, frame->sample_rate };// 设置为sample_rate为timebase

#if CONFIG_AVFILTER

            // 3. 判断过滤器源格式、通道数、通道布局、频率以及解码器的serial是否一样，若全部一样记录为0，否则只要有一个不一样，记录为1.
            dec_channel_layout = get_valid_channel_layout(frame->channel_layout, frame->channels);
            reconfigure =
                cmp_audio_fmts(_audio_filter_src.fmt, _audio_filter_src.channels, (AVSampleFormat)frame->format, frame->channels)
                ||
                _audio_filter_src.channel_layout != dec_channel_layout ||
                _audio_filter_src.freq != frame->sample_rate ||
                _auddec.decoder_get_serial() != last_serial;
                //_auddec._pkt_serial != last_serial;

            // 4. 如果不一样，更新当前帧的音频相关信息到源过滤器当中，并且重新 根据输入源 配置过滤器。
            // 输入源首先以avctx的音频参数为准，当遇到与解码后的帧的音频参数不一样，输入源改为参考解码帧。
            if (reconfigure) {
                char buf1[1024], buf2[1024];
                av_get_channel_layout_string(buf1, sizeof(buf1), -1, _audio_filter_src.channel_layout);
                av_get_channel_layout_string(buf2, sizeof(buf2), -1, dec_channel_layout);// 获取通道布局的字符串描述
                av_log(NULL, AV_LOG_DEBUG,
                    "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                    _audio_filter_src.freq, _audio_filter_src.channels, av_get_sample_fmt_name(_audio_filter_src.fmt), buf1, last_serial,
                    frame->sample_rate, frame->channels, av_get_sample_fmt_name((AVSampleFormat)frame->format), buf2, _auddec.decoder_get_serial());

                _audio_filter_src.fmt = (AVSampleFormat)frame->format;
                _audio_filter_src.channels = frame->channels;
                _audio_filter_src.channel_layout = dec_channel_layout;
                _audio_filter_src.freq = frame->sample_rate;
                last_serial = _auddec.decoder_get_serial();// 一般开始是这里不等，因为last_serial初始值=-1

                if ((ret = configure_audio_filters(_afilters, 1)) < 0)// 注意这里是强制了输出格式force_output_format=1的。
                    goto the_end;
            }

            // 5. 重新配置过滤器后，那么就可以往过滤器中输入解码一帧。
            if ((ret = av_buffersrc_add_frame(_in_audio_filter, frame)) < 0)
                goto the_end;

            // 6. 从输出过滤器读取过滤后的一帧音频。
            // while一般从这里的 av_buffersink_get_frame_flags 退出，第二次再读时，因为输出过滤器没有帧可读会返回AVERROR(EAGAIN)。
            while ((ret = av_buffersink_get_frame_flags(_out_audio_filter, frame, 0)) >= 0) {
                // 6.1 从输出过滤器中更新时基
                tb = av_buffersink_get_time_base(_out_audio_filter);
#endif
                // 6.2 获取可写Frame。如果没有可写帧，会阻塞等待，直到有可写帧；当用户中断包队列 返回NULL。
                if (!(af = _sampq.frame_queue_peek_writable()))  // 获取可写帧。留个疑问，视频好像没有获取可写帧？答：都有的。封装在queue_picture()。
                    goto the_end;

                // 6.3 设置Frame pts、pos、serial、duration
                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = frame->pkt_pos;
                af->serial = _auddec.decoder_get_serial();
                af->duration = av_q2d({ frame->nb_samples, frame->sample_rate });

                // 6.4 保存AVFrame到帧队列，和更新帧队列的 写坐标windex 以及 大小size。
                av_frame_move_ref(af->frame, frame);// frame数据拷贝到af->frame和重置frame，但frame的内存还是可以继续使用的。
                _sampq.frame_queue_push();       // 实际只更新帧队列的写坐标windex和大小size

#if CONFIG_AVFILTER
                                                    /*
                                                    * 有什么用？留个疑问。
                                                    * 答(个人理解)：看上面decoder_decode_frame()的代码，只有包队列的serial==解码器的pkt_serial时才能获取到解码帧，
                                                    * 也就是说，在decoder_decode_frame()出来时，两者肯定是相等的，至于不等，那么就是从decoder_decode_frame()到这里的代码之间，用户进行了seek操作，
                                                    * 导致了包队列的serial != 解码器的pkt_serial。那么区别就是提前break掉，ret会按照最近一次>=0退出，
                                                    * 而不加这个语句就是av_buffersink_get_frame_flags时读到ret == AVERROR(EAGAIN)退出。
                                                    * 此时可以看到帧仍然是会被放进帧队列的。
                                                    * 想测试也不难，将break注掉，随便打印东西，seek的时候看视频是否正常即可。
                                                    */
                if (*_audioq.packet_queue_get_serial() != _auddec.decoder_get_serial()) {
                    printf("tyycode+++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n");
                    break;
                }

            }

            if (ret == AVERROR_EOF) // 检查解码是否已经结束，解码结束返回0。哪里返回0了？留个疑问
                //_auddec._finished = _auddec._pkt_serial;
                _auddec.decoder_set_finished(_auddec.decoder_get_serial());
#endif
        }// <== if (got_frame) ==>

    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
    /*
    * 上面为什么ret == AVERROR_EOF结束了还继续回到do while循环？留个疑问。
    * 答：1）这是因为，即使结束了，此时包队列可能还会有包，需要继续进行解码到帧队列当中。
    * 2）而此时读线程当中，同样会继续读取，到时会一直读到AVERROR_EOF，所以会刷一次空包(eof标记决定，所以只会刷一次)，然后continue继续判断帧队列是否播放完毕。
    * 3）然后这里的解码线程audio_thread最后读到空包后，同样应该是会被放在帧队列，然后解码线程就会继续调用decoder_decode_frame，由于没包，
    * 4）所以解码线程会阻塞在packet_queue_get，等待读线程显示完最后一帧。如果设置了自动退出，那么读线程直接退出，否则会一直处于for循环，但啥也不做，等待中断。
    * 5）最终的中断我看了一下，是由do_exit内的stream_close的request_abort=1进行中断的。而do_exit是有SDL的事件进行调用，我们实际需求可以看情况进行触发中断信号。
    */

the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&_agraph);
#endif
    av_frame_free(&frame);
    return ret;
}

// 音频解码线程
static int audio_thread(void *arg)
{
    VideoState *is = (VideoState*)arg;
    int ret;
    if(!is){
        printf("+++++++++is is null in audio_thread()\n");
        return -1;
    }

    ret = is->AudioThread();

    printf("+++++++++audio_thread() exit, ret: {}\n", ret);
    return 0;
}

int VideoState::get_video_frame(AVFrame *frame)
{
    int got_picture;

    // 1. 获取解码后的视频帧。解码帧后。帧带有pts
    if ((got_picture = _viddec.decoder_decode_frame(frame, NULL)) < 0) {
        return -1; // 返回-1意味着要退出解码线程, 所以要分析decoder_decode_frame什么情况下返回-1
    }

    // 2. 判断是否要drop掉该帧(视频同步不会进来)。该机制的目的是在放入帧队列前先drop掉过时的视频帧。
    //    注意返回值got_picture=0表示解码结束了，不会进来。
    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(_video_st->time_base) * frame->pts;    // 计算出秒为单位的pts，即pts*(num/den)

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(_ic, _video_st, frame);// 视频帧采样长宽比。这里留个疑问是啥意思。或者先不理。

        if (_framedrop  >0 || // 允许drop帧
            (_framedrop && get_master_sync_type() != AV_SYNC_VIDEO_MASTER))// 非视频同步模式。一般只有视频的话，默认外部时钟是主时钟，最好手动设为视频，这一点需要注意。
        {
            if (frame->pts != AV_NOPTS_VALUE) { // pts值有效
                double diff = dpts - get_master_clock();  // 计算出解码后一帧的pts与当前主时钟的差值。
                if (!isnan(diff) &&                         // 差值有效。isnan函数：返回1表示是NAN，返回0表示非NAN。
                    fabs(diff) < AV_NOSYNC_THRESHOLD && // 差值在可同步范围，可以drop掉进行校正，但是大于10s认为输入文件本身的时间戳有问题， 画面不能随便drop掉，比较单位是秒。
                    diff - _frame_last_filter_delay < 0 &&       // 和过滤器有关系，不太懂，留个疑问
                    _viddec.decoder_get_serial() == _vidclk.get_serial() &&   // 同一序列的包，不太懂，留个疑问
                    _videoq.packet_queue_get_packets()) { // packet队列至少有1帧数据，为啥要求至少队列有一个包才能drop，不太懂，留个疑问

                    _frame_drops_early++;     // 记录已经丢帧的数量
                    printf("%s(%d) diff: %lfs, drop frame, drops frame_drops_early: %d\n", __FUNCTION__, __LINE__, diff, _frame_drops_early);
                    av_frame_unref(frame);
                    got_picture = 0;

                }
            }
        }

    }// <== if(got_picture) end ==>

    return got_picture;
}

double VideoState::get_rotation(AVStream *st)
{
    /*
    * av_stream_get_side_data()：从信息流中获取边信息。
    * 参1：流；参2：所需的边信息类型；参3：用于存储边信息大小的指针(可选)；
    * 存在返回数据指针，，否则为NULL。
    *
    * AV_PKT_DATA_DISPLAYMATRIX：这个边数据包含一个描述仿射的3x3变换矩阵转换，
    * 需要应用到解码的视频帧正确的显示。数据的详细描述请参见libavutil/display.h
    */
    uint8_t* displaymatrix = av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, NULL);
    if (displaymatrix) {
        printf("displaymatrix: %d\n", *displaymatrix);
    }

    /*
    * av_display_rotation_get()：提取变换矩阵的旋转分量。
    * 参1：转换矩阵。
    * 返回转换旋转帧的角度(以度为单位)逆时针方向。角度将在[-180.0,180.0]范围内。如果矩阵是奇异的则返回NaN。
    * @note：浮点数本质上是不精确的，所以调用者是建议在使用前将返回值舍入到最接近的整数。
    */
    double theta = 0;
    if (displaymatrix)
        theta = -av_display_rotation_get((int32_t*)displaymatrix);

    theta -= 360 * floor(theta / 360 + 0.9 / 360);

    if (fabs(theta - 90 * round(theta / 90)) > 2)
        av_log(NULL, AV_LOG_WARNING, "Odd rotation angle.\n"
            "If you want to help, upload a sample "
            "of this file to ftp://upload.ffmpeg.org/incoming/ "
            "and contact the ffmpeg-devel mailing list. (ffmpeg-devel@ffmpeg.org)");// 上传样本，找ffmpeg帮忙

    return theta;
}

int VideoState::configure_video_filters(AVFilterGraph *graph, const char *vfilters, AVFrame *frame)
{
    enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];// 求出数组元素个数
    char sws_flags_str[512] = "";
    char buffersrc_args[256];
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecParameters *codecpar = _video_st->codecpar;
    AVRational fr = av_guess_frame_rate(_ic, _video_st, NULL);
    AVDictionaryEntry *e = NULL;
    int nb_pix_fmts = 0;
    int i, j;

    // 1. 找出SDL和FFmpeg都支持的pix_format，保存在局部数组pix_fmts中。
    // num_texture_formats是一个SDL_PixelFormatEnum数组。see http://wiki.libsdl.org/SDL_RendererInfo
    for (i = 0; i < _renderer_info.num_texture_formats; i++) {// 遍历SDL支持的纹理格式，实际就是图片的pix_format
        for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; j++) {// 遍历FFmpeg支持的pix_format(通过映射的关系来搭建桥梁判断)
            if (_renderer_info.texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {// 如果SDL和FFmpeg都支持的pix_format，保存在局部数组pix_fmts中。
                pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
                break;  // 找到一个先退出内层for，进行下一个查找。
            }
        }
    }
    pix_fmts[nb_pix_fmts] = AV_PIX_FMT_NONE;// 末尾补空

                                            // 2. 遍历字典sws_dict
    while ((e = av_dict_get(_sws_dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
        if (!strcmp(e->key, "sws_flags")) {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);// 追加字符串
        }
        else
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
    }
    if (strlen(sws_flags_str))
        sws_flags_str[strlen(sws_flags_str) - 1] = '\0';

    graph->scale_sws_opts = av_strdup(sws_flags_str);// scale_sws_opts是用于自动插入比例过滤器的SWS选项

                                                     // 3. 使用简单方法创建输入滤波器AVFilterContext
    snprintf(buffersrc_args, sizeof(buffersrc_args),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        frame->width, frame->height, frame->format,
        _video_st->time_base.num, _video_st->time_base.den,
        codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
    if (fr.num && fr.den)
        av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

    if ((ret = avfilter_graph_create_filter(&filt_src,
        avfilter_get_by_name("buffer"),
        "ffplay_buffer", buffersrc_args, NULL,
        graph)) < 0)
        goto fail;

    // 4. 使用简单方法创建输出滤波器AVFilterContext
    ret = avfilter_graph_create_filter(&filt_out,
        avfilter_get_by_name("buffersink"),
        "ffplay_buffersink", NULL, NULL, graph);
    if (ret < 0)
        goto fail;

    if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto fail;

    last_filter = filt_out;

    /*
    * Note: this macro adds a filter before the lastly added filter, so the
    * processing order of the filters is in reverse。
    * 意思是：AVFilterContext串每次是通过avfilter_link从末尾往头链起来的，顺序是倒转的。
    */
#define INSERT_FILT(name, arg) do {                                          \
    AVFilterContext *filt_ctx;                                               \
    \
    ret = avfilter_graph_create_filter(&filt_ctx,                            \
    avfilter_get_by_name(name),           \
    "ffplay_" name, arg, NULL, graph);    \
    if (ret < 0)                                                             \
    goto fail;                                                           \
    \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
    if (ret < 0)                                                             \
    goto fail;                                                           \
    \
    last_filter = filt_ctx;                                                  \
} while (0)

    // 5. 判断是否有旋转角度，如果有需要使用对应的过滤器进行处理；没有则不会添加
    // 额外的过滤器处理，只有输入输出过滤器。
    /*
    * 一 图片的相关旋转操作命令：
    * 1）垂直翻转：                      ffmpeg -i fan.jpg -vf vflip -y vflip.png
    * 2）水平翻转：                      ffmpeg -i fan.jpg -vf hflip -y hflip.png
    * 3）顺时针旋转60°(PI代表180°)：      ffmpeg -i fan.jpg -vf rotate=PI/3 -y rotate60.png
    * 4）顺时针旋转90°：                 ffmpeg -i fan.jpg -vf rotate=90*PI/180 -y rotate90.png
    * 5）逆时针旋转90°(负号代表逆时针，正号代表顺时针)：ffmpeg -i fan.jpg -vf rotate=-90*PI/180 -y rotate90-.png
    * 6）逆时针旋转90°：                  ffmpeg -i fan.jpg -vf transpose=2 -y transpose2.png
    * rotate、transpose的值具体使用ffmpeg -h filter=filtername去查看。
    * 注意1：上面的图片使用ffprobe去看不会有metadata元数据，所以自然不会有rotate与Side data里面的displaymatrix。只有视频才有。
    * 注意2：使用是rotate带有黑底的，例如上面的rotate60.png。图片的很好理解，都是以原图进行正常的旋转，没有难度。
    *
    *
    * 二 视频文件相关旋转的操作：
    * 1.1 使用rotate选项：
    * 1） ffmpeg -i 2_audio.mp4 -metadata:s:v rotate='90' -codec copy 2_audio_rotate90.mp4
    * 但是这个命令实际效果是：画面变成逆时针的90°操作。使用ffprobe一看：
    *     Metadata:
    rotate          : 270
    handler_name    : VideoHandler
    Side data:
    displaymatrix: rotation of 90.00 degrees
    Stream #0:1(und): Audio: aac (LC) (mp4a / 0x6134706D), 44100 Hz, stereo, fltp, 184 kb/s (default)
    Metadata:
    handler_name    : 粤语
    * 可以看到rotate是270°，但displaymatrix确实是转了90°。
    *
    * 2）ffmpeg -i 2_audio.mp4 -metadata:s:v rotate='270' -codec copy 2_audio_rotate270.mp4
    * 同样rotate='270'时，画面变成顺时针90°的操作。rotate=90，displaymatrix=rotation of -90.00 degrees。
    *
    * 3）ffmpeg -i 2_audio.mp4 -metadata:s:v rotate='180' -codec copy 2_audio_rotate180.mp4
    * 而180的画面是倒转的，这个可以理解。rotate=180，displaymatrix=rotation of -180.00 degrees。
    *
    * 2.1 使用transpose选项
    * 1）ffmpeg -i 2_audio.mp4  -vf transpose=1 -codec copy 2_audio_transpose90.mp4(顺时针90°)
    * 2）ffmpeg -i 2_audio.mp4  -vf transpose=2 2_audio_transpose-90.mp4(逆时针90°，不能加-codec copy，否则与transpose冲突)
    * 上面命令按预期正常顺时针的旋转了90°和逆时针旋转90°的画面，但是使用ffprobe看不到rotate或者displaymatrix对应的角度。
    *
    * 3.1 使用rotate+transpose选项
    * 1） ffmpeg -i 2_audio.mp4 -vf transpose=1 -metadata:s:v rotate='90' -vcodec libx264 2_audio_rotate90.mp4
    * 2）ffmpeg -i 2_audio.mp4 -vf transpose=1 -metadata:s:v rotate='180' -vcodec libx264 2_audio_rotate180.mp4
    * 3）ffmpeg -i 2_audio.mp4 -vf transpose=1 -metadata:s:v rotate='270' -vcodec libx264 2_audio_rotate270.mp4
    * 只要使用了transpose选项，rotate就会失效。例如运行上面三个命令，实际只顺时针旋转了90°，即transpose=1的效果，并且，只要存在transpose，它和2.1一样，
    *   使用ffprobe看不到rotate或者displaymatrix对应的角度，这种情况是我们不愿意看到的。所以经过分析，我们最终还是得回到只有rotate选项的情况。
    *
    * 目前我们先记着1.1的三种情况的结果就行，后续有空再深入研究旋转，并且实时流一般都会返回theta=0，不会有旋转的操作。
    */
    if (_autorotate) {
        double theta = get_rotation(_video_st);

        if (fabs(theta - 90) < 1.0) {
            INSERT_FILT("transpose", "clock");// 转换过滤器，clock代表，顺时针旋转，等价于命令transpose=1。
                                              // 可用ffmpeg -h filter=transpose查看。查看所有filter：ffmpeg -filters
        }
        else if (fabs(theta - 180) < 1.0) {
            INSERT_FILT("hflip", NULL);// 镜像左右反转过滤器，
            INSERT_FILT("vflip", NULL);// 镜像上下反转过滤器，经过这个过滤器处理后，画面会反转，类似水中倒影。
        }
        else if (fabs(theta - 270) < 1.0) {
            INSERT_FILT("transpose", "cclock");// 逆时针旋转，等价于命令transpose=2.
        }
        else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            INSERT_FILT("rotate", rotate_buf);// 旋转角度过滤器
        }
    }

    // 上面看到，除了原过滤器没有Link，其余都是通过了INSERT_FILT进行了Link的。

    // 6. 判断是否需要添加复杂过滤器，并且将输入输出过滤器AVFilterContext进行Link，最后提交整个滤波图。
    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
        goto fail;

    // 7. 经过上面的过程处理后，得到输出的过滤器filt_out，并保存输入的过滤器filt_src
    _in_video_filter = filt_src;
    _out_video_filter = filt_out;

fail:
    return ret;
}

int VideoState::queue_picture(AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    Frame *vp;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
        av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    // 1. 获取队列中可写的帧
    if (!(vp = _pictq.frame_queue_peek_writable())) // 检测队列是否有可写空间
        return -1;      // 请求退出则返回-1

    // 执行到这步说明已经获取到了可写入的Frame

    // 2. 使用传进来的参数对Frame的内部成员进行赋值。
    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;                           // 0 表示该帧还没显示

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;                              // 该帧在输入文件中的字节位置
    vp->serial = serial;                        // 帧队列里面的serial是由解码器里面的pkt_serial赋值，而pkt_serial由包队列的MyAVPacketList节点赋值。

    set_default_window_size(vp->width, vp->height, vp->sar);    // 更新解码后的实际要显示图片的宽高
    av_frame_move_ref(vp->frame, src_frame);    // 这里才是真正的push，将src中所有数据转移到dst中，并复位src。
                                                // 夺走所有权，但和C++有区别，这里可以认为frame会重新开辟一份内存并进行内容拷贝，原来的src_frame内容被重置但内存不会被释放。
                                                // 为啥这么想呢？因为我们看到video_thread调用完本函数后，明明已经push到队列了，但还是调用av_frame_unref(frame)释放src_frame。

    // 3. push到Frame帧队列，但frame_queue_push函数里面只是更新属性，
    // 真正的push应该是通过frame_queue_peek_writable获取地址，再av_frame_move_ref。
    _pictq.frame_queue_push();               // 更新写索引位置

    return 0;
}

int VideoState::VideoThread(){
    AVFrame *frame = av_frame_alloc();  // 分配解码帧
    double pts;                         // pts
    double duration;                    // 帧持续时间
    int ret;

    //1 获取stream timebase
    AVRational tb = _video_st->time_base; // 获取stream timebase
                                             //2 获取帧率，以便计算每帧picture的duration。
                                             // 估计优先选择流中的帧率(https://blog.csdn.net/weixin_44517656/article/details/110355462)，具体看av_guess_frame_rate。
    AVRational frame_rate = av_guess_frame_rate(_ic, _video_st, NULL);
    _frame_rate = (frame_rate.num && frame_rate.den ? av_q2d({ frame_rate.num, frame_rate.den }) : 25.0);

#if CONFIG_AVFILTER
    AVFilterGraph *graph = NULL;
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = (AVPixelFormat)-2;
    int last_serial = -1;
    int last_vfilter_idx = 0;

#endif

    if (!frame)
        return AVERROR(ENOMEM);


    for (;;) {  // 循环取出视频解码的帧数据
                // 3 获取解码后的视频帧
        ret = get_video_frame(frame);
        if (ret < 0)
            goto the_end;   //解码结束, 什么时候会结束
        if (!ret)           //没有解码得到画面, 什么情况下会得不到解后的帧。实际上get_video_frame解码到文件末尾也会返回0.
            continue;

#if CONFIG_AVFILTER
        if (last_w != frame->width
            || last_h != frame->height
            || last_format != frame->format
            || last_serial != _viddec.decoder_get_serial()  // 解码器的pkt_serial就是每个节点的serial，它会不断更新，具体看packet_queue_get。
            || last_vfilter_idx != _vfilter_idx) {
            av_log(NULL, AV_LOG_DEBUG,
                "Video frame changed from size:%dx%d format:%s serial:%d ---to--- size:%dx%d format:%s serial:%d\n",
                last_w, last_h,
                (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                frame->width, frame->height,
                (const char *)av_x_if_null(av_get_pix_fmt_name((AVPixelFormat)frame->format), "none"), _viddec.decoder_get_serial());

            avfilter_graph_free(&graph);        // 即使graph也是安全的。
            // 1. 创建系统滤波
            graph = avfilter_graph_alloc();
            if (!graph) {
                ret = AVERROR(ENOMEM);
                goto the_end;
            }
            graph->nb_threads = _filter_nbthreads;

            // 2. 配置相关过滤器保存在播放器实例中。内部使用了简单过滤器+复杂的字符串过滤器的组合。
            if ((ret = configure_video_filters(graph, _vfilters_list ? _vfilters_list[_vfilter_idx] : NULL, frame)) < 0) {
                SDL_Event event;
                event.type = FF_QUIT_EVENT;
                event.user.data1 = this;
                SDL_PushEvent(&event);
                goto the_end;
            }

            // 这里主要是从解码帧保存一些已知的内容，判断下一帧到来时，和之前的内容是否一致。
            filt_in = _in_video_filter; // 获取经过configure_video_filters处理后的输入输出过滤器
            filt_out = _out_video_filter;
            last_w = frame->width;          // 保存上一次解码帧的分辨率，经过configure_video_filters前后，帧的分辨率应该是不会变的，例如是1920x1080，经过过滤器处理应该还是1920x1080。
            last_h = frame->height;
            last_format = (AVPixelFormat)frame->format;
            last_serial = _viddec.decoder_get_serial();
            last_vfilter_idx = _vfilter_idx; // 保存上一次过滤器的下标，主要用来改变configure_video_filters参3复杂过滤器的选项
            frame_rate = av_buffersink_get_frame_rate(filt_out);
        }

        // 4. 上面配置好过滤器后，现在就可以往输入过滤器添加解码后的一帧数据，进行过滤处理了。
        // 实际上过滤器的流程就是：配置好过滤器后，添加帧，输出处理后的帧，就是这么简单。
        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {// 一般只会执行一次while，第二次会在av_buffersink_get_frame_flags中break掉，进入下一次的for循环
            _frame_last_returned_time = av_gettime_relative() / 1000000.0;

            // 5. 获取处理后的一帧
            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);// 与av_buffersink_get_frame一样的。
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    //_viddec._finished = _viddec._pkt_serial;
                    _viddec.decoder_set_finished(_viddec.decoder_get_serial());
                }
                ret = 0;
                break;
            }

            // 6. 每一次从过滤器中获取一帧的延时，若大于不同步的阈值，不做处理，将其赋值为0
            // (感觉他起名不好，他认为是上一次的延时，我感觉这一次的延时更恰当和更好理解)
            _frame_last_filter_delay = av_gettime_relative() / 1000000.0 - _frame_last_returned_time;
            if (fabs(_frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                _frame_last_filter_delay = 0;

            // 更新时基，从过滤器中获取，一开始从流中获取
            tb = av_buffersink_get_time_base(filt_out);
#endif
            //printf("tyycode frame_rate:%d/%d, tb:%d/%d\n", frame_rate.num, frame_rate.den, tb.num, tb.den);// 对于实时流，帧率一般都是固定为25,时基固定为90k

            // 7. 计算帧持续时间和换算pts值为秒
            // 1/帧率 = duration 单位秒, 没有帧率时则设置为0, 有帧率时计算出帧间隔，单位转成double。依赖滤波器求出帧率，再求出帧时长
            duration = (frame_rate.num && frame_rate.den ? av_q2d({ frame_rate.den, frame_rate.num }) : 0);
            // 根据AVStream timebase计算出pts值, 单位为秒
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            // 8. 将解码后的视频帧插入队列
            ret = queue_picture(frame, pts, duration, frame->pkt_pos, _viddec.decoder_get_serial());
            // 9. 释放frame对应的数据
            av_frame_unref(frame);

#if CONFIG_AVFILTER
            if (*_videoq.packet_queue_get_serial() != _viddec.decoder_get_serial())
                break;

        }//<== while end ==>
#endif

        if (ret < 0) // 返回值小于0则退出线程
            goto the_end;

    }// <== for (;;) end ==>

the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&graph);
#endif
    av_frame_free(&frame);
    return 0;
}

// 视频解码线程
static int video_thread(void *arg)
{
    VideoState *is = (VideoState*)arg;
    int ret;
    if(!is){
        //printf("+++++++++is is null in video_thread\n");
        SPDERROR("+++++++++is is null in video_thread");
        return -1;
    }

    ret = is->VideoThread();

    //printf("video_thread exit, ret: %d\n", ret);
    SPDINFO("video_thread exit, ret: {}", ret);
    return 0;
}

int VideoState::SubtitleThread(){
    Frame *sp;
    int got_subtitle;
    double pts;

    for (;;) {
        // 1. 从字幕队列中获取可写入的一帧，准备用于存储解码后的字幕帧。
        if (!(sp = _subpq.frame_queue_peek_writable()))
            return 0;

        // 2. 开始解码字幕pkt，解码后的字幕保存在sp->sub中。
        // 音视频参2都是传Frame，只有字幕传NULL，参3则相反，音视频传NULL，字幕传AVSubtitle。
        if ((got_subtitle = _subdec.decoder_decode_frame(NULL, &sp->sub)) < 0)
            break;

        pts = 0;

        // 3. 解码成功且字幕格式要求是图形，则更新相关信息和将帧放在帧队列中。
        // 想字幕直接支持ass格式，可以参考https://blog.csdn.net/qq_40212938/article/details/108041998.
        if (got_subtitle && sp->sub.format == 0) {
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double)AV_TIME_BASE;// 转成秒

            // 更新自定义字幕帧的相关信息
            sp->pts = pts;
            //sp->serial = _subdec.pkt_serial;
            //sp->width = _subdec.avctx->width;
            //sp->height = _subdec.avctx->height;
            sp->serial = _subdec.decoder_get_serial();
            sp->width = _subdec.decoder_get_avctx()->width;
            sp->height = _subdec.decoder_get_avctx()->height;
            sp->uploaded = 0;

            /* now we can update the picture count */
            _subpq.frame_queue_push();
        } else if (got_subtitle) {// 4. 解码成功但sp->sub.format != 0的情况，则丢弃该字幕。
            avsubtitle_free(&sp->sub);
        }
    }

    return 0;
}

int subtitle_thread(void *arg)
{
    VideoState *is = (VideoState*)arg;
    int ret;
    if(!is){
        //printf("+++++++++is is null in subtitle_thread\n");
        SPDERROR("+++++++++is is null in subtitle_thread");
        return -1;
    }

    ret = is->SubtitleThread();

    //printf("subtitle_thread exit, ret: %d\n");
    SPDINFO("subtitle_thread exit, ret: {}", ret);
    return 0;
}

AVCodecContext *VideoState::ConfigureCodec(int stream_index){

    AVCodecContext *avctx;
    AVCodec *codec;
    AVDictionary *opts = NULL;
    AVDictionaryEntry *t = NULL;
    const char *forced_codec_name = NULL;
    int stream_lowres = _lowres;             // 用于输入的解码分辨率，但是最终由流的编码器最低支持的分辨率决定该值
    int ret;

    /* 1. 为解码器分配一个编解码器上下文结构体 */
    avctx = avcodec_alloc_context3(NULL);
    if (!avctx)
        return NULL;

    /* 2. 将对应音视频码流中的编解码器信息，拷贝到新分配的编解码器上下文结构体 */
    ret = avcodec_parameters_to_context(avctx, _ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;

    // 3. 设置pkt_timebase
    avctx->pkt_timebase = _ic->streams[stream_index]->time_base;

    /* 4. 根据codec_id查找解码器 */
    codec = avcodec_find_decoder(avctx->codec_id);

    /* 5. 保存流下标last_audio_stream、last_subtitle_stream、last_video_stream，用于记录，方便进行其它操作。例如切换国语/粤语 */
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        _last_audio_stream = stream_index;
        forced_codec_name = _audio_codec_name;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        _last_subtitle_stream = stream_index;
        forced_codec_name = _subtitle_codec_name;
        break;
    case AVMEDIA_TYPE_VIDEO:
        _last_video_stream = stream_index;
        forced_codec_name = _video_codec_name;
        break;
    }

    /* 6. 如果用户指定了解码器的名字，则重新寻找解码器 */
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name)
            av_log(NULL, AV_LOG_WARNING, "No codec could be found with name '%s'\n", forced_codec_name);
        else
            av_log(NULL, AV_LOG_WARNING, "No decoder could be found for codec %s\n", avcodec_get_name(avctx->codec_id));

        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id; // 用户指定了解码器并找到的情况下，需要给avctx->codec_id重新赋值。

    /* 7. 给解码器的以哪种分辨率进行解码。 会进行检查用户输入的最大低分辨率是否被解码器支持 */
    if (stream_lowres > codec->max_lowres) {// codec->max_lowres: 解码器支持的最大低分辨率值
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n", codec->max_lowres);
        stream_lowres = codec->max_lowres;
    }
    avctx->lowres = stream_lowres;// 一般设为0即可

    // 用户是否设置了加快解码速度
    if (_fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;// 允许不符合规范的加速技巧。估计是加快解码速度

    /* 8. 设置相关选项，然后解码器与解码器上下文关联。 */
    //opts = filter_codec_opts(_codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);

    // av_opt_set_int(avctx, "refcounted_frames", 1, 0);等同于avctx->refcounted_frames = 1;
    // 设置为1的时候表示解码出来的frames引用永久有效，需要手动释放
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        av_dict_set(&opts, "refcounted_frames", "1", 0);

    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {// key为空并且参4是AV_DICT_IGNORE_SUFFIX：代表遍历所有的字典条目。
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }

    ret = 0;
fail:
    return ret == 0 ? avctx : NULL;
}

int VideoState::stream_component_open(int stream_index)
{
    AVFormatContext *ic = _ic;           // 从播放器获取输入封装上下文
    AVCodecContext *avctx;
    AVCodec *codec;
    const char *forced_codec_name = NULL;
    AVDictionary *opts = NULL;
    AVDictionaryEntry *t = NULL;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int ret = 0;
    int lowres = 0;
    int stream_lowres = lowres;             // 用于输入的解码分辨率，但是最终由流的编码器最低支持的分辨率决定该值

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;

    avctx = ConfigureCodec(stream_index);
    if(!avctx){
        //printf("++++++++++ConfigureCodec failed\n");
        SPDERROR("++++++++++ConfigureCodec failed\n");
        goto fail;
    }

    _eof = 0;                                               // 这里赋值为0的意义是什么？应该只需要在读线程处理吧？这个函数就是读线程调用的。
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT; // 丢弃无用的数据包，如0大小的数据包在avi

    switch (avctx->codec_type) {

    case AVMEDIA_TYPE_AUDIO: {
#if CONFIG_AVFILTER
        {
            // 这里是音频filter的一些处理。
            AVFilterContext *sink;

            // 1）保存音频过滤器src的相关参数。avctx的信息是从第二步的流参数拷贝过来的。
            _audio_filter_src.freq = avctx->sample_rate;
            _audio_filter_src.channels = avctx->channels;
            _audio_filter_src.channel_layout = get_valid_channel_layout(avctx->channel_layout, avctx->channels);
            _audio_filter_src.fmt = avctx->sample_fmt;

            // 2）初始化配置音频过滤器。
            // 注意这里不强制输出音频格式。因为未调用audio_open，此时音频的输出格式audio_tgt内部值全是0，并未读取到对应的参数。
            // 但是输入的音频格式的知道的，所以可以先指定。
            // 在解码线程才是真正的配置。
            if ((ret = configure_audio_filters(_afilters, 0)) < 0)
                goto fail;

            // 3）获取音频的输出过滤器及其音频相关信息(实际上是获取输入的相关信息)。上面配置之后，out_audio_filter就是输出的过滤器。
            // 注意下面的内容只有 avfilter_graph_config 提交了系统滤波器才能获取到这些参数。
            // 并且通过debug发现，输入过滤器的值和下面的值的一样的，并且因为输出过滤器out_audio_filter是没有这些参数值的，
            // 所以猜测此时获取到的内容应该是从输入过滤器中读取的(其实看else的内容也可以确定是从输入获取的)。想更确定的可以去看看源码，但没必要，知道就行。
            sink = _out_audio_filter;
            sample_rate = av_buffersink_get_sample_rate(sink);
            nb_channels = av_buffersink_get_channels(sink);
            channel_layout = av_buffersink_get_channel_layout(sink);
        }
#else
        //从avctx(即AVCodecContext)中获取音频格式参数
        sample_rate = avctx->sample_rate;
        nb_channels = avctx->channels;
        channel_layout = avctx->channel_layout;
#endif

        /* prepare audio output 准备音频输出 */
        // 4）调用audio_open打开音频，获取对应的硬件参数，保存到FFmpeg类型的结构体进行传出(audio_tgt)，
        //      返回值表示输出设备的缓冲区大小，内部SDL会启动相应的回调函数
        if ((ret = audio_open(this, channel_layout, nb_channels, sample_rate, &_audio_tgt)) < 0)
            goto fail;

        // 5）.1 初始化音频的缓存相关数据
        _audio_hw_buf_size = ret;    // 保存SDL的音频缓存大小
        _audio_src = _audio_tgt;  // 暂且将数据源参数等同于目标输出参数，注audio_filter_src仍然保存着流中原本的信息，与audio_src不是同一变量。
                                  //初始化audio_buf相关参数
        _audio_buf_size = 0;
        _audio_buf_index = 0;

        /* 5）.2 init averaging filter 初始化averaging滤镜, 下面3个变量只有在非audio master时使用，audio_diff_cum也是。 */
        /*
        * 1）exp(x):返回以e为底的x次方，即e^x。注意和对数不一样，对数公式有：y=log(a)(x)，a是低，那么有：x=a^y。和这个函数不一样，不要代进去混淆了。
        * 2）log(x)：以e为底的对数。这个才可以代入对数公式。例如log(10)=y，那么e^y=10，y≈2.30258509。其中e=2.718281828...
        *
        * 3）log(0.01)=y，那么e^y=0.01，y≈-4.60517018(计算器计出来即可)。根据对数的图，这个结果没问题。
        * 那么log(0.01) / AUDIO_DIFF_AVG_NB = -4.60517018 / 20 ≈ -0.2302585092。
        * 那么exp(-0.2302585092) ≈ e^(-0.2302585092)=0.7943282347242815。可以使用pow(a,n)去验证。
        */
        double tyycodeLog1 = log(0.01);                         // -4.60517018
        double tyycodeLog2 = tyycodeLog1 / AUDIO_DIFF_AVG_NB;   // -0.2302585092
        double tyycodeLog3 = exp(tyycodeLog2);                  // 0.7943282347242815
        double tyycodeVerity = pow(2.718281828, -0.2302585092); // 验证，结果是接近的，说明没错。
        _audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB); // 0.794。转成数学公式是：e^(ln(0.01)/20)
        _audio_diff_avg_count = 0;
        /* 由于我们没有精确的音频数据填充FIFO,故只有在大于该阈值时才进行校正音频同步 */
        _audio_diff_threshold = (double)(_audio_hw_buf_size) / _audio_tgt.bytes_per_sec;// 例如1024/8000*1*2=1024/16000=0.064

                                                                                        // 5）.3 初始化播放器的音频流及其下标
        _audio_stream = stream_index;            // 获取audio的stream索引
        _audio_st = ic->streams[stream_index];   // 获取audio的stream

                                                 // 5）.4初始化解码器队列
        _auddec.decoder_init(avctx, &_audioq, _continue_read_thread);    // 音视频、字幕的队满队空都是使用同一个条件变量去做的。

        /* 6）判断is->ic->iformat->flags是否有这3个宏其中之一，若只要有一个，就代表不允许这样操作。
        * 若无，则不会进行初始化。例如flags=64=01000000 & 11100000 00000000 = 0，没有这些宏代表允许这样操作。
        * 并且若read_seek是空的话，才会初始化start_pts、start_pts_tb。
        *
        * AVFMT_NOBINSEARCH：Format不允许通过read_timestamp返回二进制搜索。
        * AVFMT_NOGENSEARCH：格式不允许退回到通用搜索。
        * AVFMT_NO_BYTE_SEEK：格式不允许按字节查找。
        * AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK = 11100000 00000000.
        *
        * is->ic->iformat->read_seek：回调函数，用于在流组件stream_index中查找相对于帧的给定时间戳。
        *
        * 上面想表达的意思基本就是：如果不支持二进制、不支持通用、不支持字节查找，那么只能通过pts查找了。这个应该是如何操作seek的处理。
        */
        if ((_ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !_ic->iformat->read_seek) {
            //_auddec._start_pts = _audio_st->start_time;
            //_auddec._start_pts_tb = _audio_st->time_base;
            _auddec.decoder_set_start_pts(_audio_st->start_time);
            _auddec.decoder_set_start_pts_tb(_audio_st->time_base);
        }

        // 7）启动音频解码线程
        if ((ret = _auddec.decoder_start(audio_thread, "audio_decoder", this)) < 0)
            goto out;

        SPDINFO("audio_thread start");
        // 8）需要开始播放，才会有声音，注释掉它是不会有声音的。参2传0代表开始播放，非0表示暂停。
        //  see http://wiki.libsdl.org/SDL_PauseAudioDevice
        SDL_PauseAudioDevice(_audio_dev, 0);
        break;
    }

    case AVMEDIA_TYPE_VIDEO: {
        _video_stream = stream_index;            // 获取video的stream索引
        _video_st = ic->streams[stream_index];   // 获取video的stream
                                                 // 初始化ffplay封装的视频解码器

        _viddec.decoder_init(avctx, &_videoq, _continue_read_thread);
        // 启动视频频解码线程
        if ((ret = _viddec.decoder_start(video_thread, "video_decoder", this)) < 0)
            goto out;

        SPDINFO("video_thread start");
        _queue_attachments_req = 1; // 使能请求mp3、aac等音频文件的封面
        break;
    }

    // 视频是类似逻辑处理
    case AVMEDIA_TYPE_SUBTITLE: {
        _subtitle_stream = stream_index;
        _subtitle_st = ic->streams[stream_index];

        _subdec.decoder_init(avctx, &_subtitleq, _continue_read_thread);
        if ((ret = _subdec.decoder_start(subtitle_thread, "subtitle_decoder", this)) < 0)
            goto out;

        SPDINFO("subtitle_thread start");
        break;
    }

    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_dict_free(&opts);

    return ret;
}

/* pause or resume the video */
void VideoState::stream_toggle_pause()
{
    // 如果当前是暂停 -> 恢复播放
    // 正常播放 -> 暂停
    if (_paused) {// 当前是暂停，那这个时候进来这个函数就是要恢复播放
                     /* 恢复暂停状态时也需要恢复时钟，需要更新vidclk */
                     // 加上 暂停->恢复 经过的时间

                     // 本if的内容可以全部注掉，与不注掉的区别是：注掉会drop一帧，并且会造成视频求delay时的diff值变大(会自动通过同步处理，所以不用担心)，不注掉则不会。
                     // 但是注意必须全部注掉，若只注掉frame_timer的内容而不set_clock()，会导致暂停重新播放后画面卡主，原因是程序以为视频超前了，需要等待。
                     // 反正大家可以根据自己的感受去做优化即可。
        _frame_timer += av_gettime_relative() / 1000000.0 - _vidclk.get_last_updated();
        if (_read_pause_return != AVERROR(ENOSYS)) {
            _vidclk.set_paused(0);
        }

        // 设置时钟的意义，暂停状态下读取的是单纯pts
        // 重新矫正video时钟
        _vidclk.set_clock(_vidclk.get_clock(), _vidclk.get_serial());
    }

    _extclk.set_clock(_extclk.get_clock(), _extclk.get_serial());
    // 更新播放器的状态 + 3个时钟的状态：切换 pause/resume 两种状态
    //_paused = _audclk._paused = _vidclk._paused = _extclk._paused = !_paused;
    _audclk.set_paused(!_paused);
    _vidclk.set_paused(!_paused);
    _extclk.set_paused(!_paused);
    _paused = !_paused;
    printf("is->step = %d; stream_toggle_pause\n", _step);
}

void VideoState::step_to_next_frame()
{
    /* if the stream is paused unpause it, then step(如果流被暂停，则取消暂停，然后执行step) */
    if (_paused)
        stream_toggle_pause();
    _step = 1;
    printf("is->step = 1; step_to_next_frame\n");
}

// 该函数后面可以写到PacketQueue
int VideoState::stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    // 下面这样判断只是大概判断的，因为此时解码线程是有可能上锁对包队列进行操作的，所以是不准确即不同步的，ffplay可能为了更快吧。
    return stream_id < 0 || // 没有该流
        queue->packet_queue_get_abort() || // 请求退出
        (st->disposition & AV_DISPOSITION_ATTACHED_PIC) || // 是ATTACHED_PIC
        queue->packet_queue_get_packets() > MIN_FRAMES // packet数>25
        && (!queue->packet_queue_get_duration() ||     // 满足PacketQueue总时长为0
            av_q2d(st->time_base) * queue->packet_queue_get_duration() > 1.0); //或总时长超过1s，实际上就是有25帧。
}

void VideoState::stream_seek(int64_t pos, int64_t rel, int seek_by_bytes)
{
    // 只有seek_req为0才进来，所以即使用户多次触发请求事件，也是按照一次处理。
    if (!_seek_req) {
        _seek_pos = pos; // 按时间单位是微秒，按字节单位是byte
        _seek_rel = rel;
        _seek_flags &= ~AVSEEK_FLAG_BYTE;        // 不按字节的方式去seek
        if (seek_by_bytes)
            _seek_flags |= AVSEEK_FLAG_BYTE;     // 强制按字节的方式去seek
        _seek_req = 1;                           // 请求seek， 在read_thread线程seek成功才将其置为0
        SDL_CondSignal(_continue_read_thread);
    }
}

int VideoState::get_read_thread_abort(){
    return _abort_request;
}

std::string VideoState::get_ffmpeg_error(int errVal) {
    char str_error[512] = { 0 };
    av_strerror(errVal, str_error, sizeof(str_error) - 1);
    return str_error;
}

void VideoState::stream_close(){

    // 动态(线程/callback)的先停止退出
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    _abort_request = 1;				// 播放器的请求退出.读线程会因=1退出。
    //_abort_request_w = 1;

    if (_read_tid != NULL) {
        SDL_WaitThread(_read_tid, NULL);	// 等待数据读取线程退出
        _read_tid = NULL;
    }
    //// tyycode. 回收写线程。写线程不能在本函数回收，虽然主动回收可以，但是被动回收时，会在写线程中卡死.
    //if (_write_tid != NULL) {
    //	SDL_WaitThread(_write_tid, NULL);
    //	_write_tid = NULL;
    //}

    /* close each stream */
    if (_audio_stream >= 0)
        stream_component_close(_audio_stream);
    if (_video_stream >= 0)
        stream_component_close(_video_stream);
    if (_subtitle_stream >= 0)
        stream_component_close(_subtitle_stream);

    avformat_close_input(&_ic);


    /* 清理包队列 */
    _videoq.packet_queue_destroy();
    _audioq.packet_queue_destroy();
    _subtitleq.packet_queue_destroy();


    /* 清理帧队列(free all pictures) */
    _pictq.frame_queue_destory();
    _sampq.frame_queue_destory();
    _subpq.frame_queue_destory();

    if (_continue_read_thread != NULL) {
        SDL_DestroyCond(_continue_read_thread);
        _continue_read_thread = NULL;
    }

    sws_freeContext(_img_convert_ctx);
    sws_freeContext(_sub_convert_ctx);

    /* 由av_strdup(filename)开辟的内存，需要释放 */
    //av_free(is->filename);

    // tyycode.
    //_audclk.set_clock_flush();
    //_vidclk.set_clock_flush();
    //_extclk.set_clock_flush();

    if (_vis_texture)
        SDL_DestroyTexture(_vis_texture);
    if (_vid_texture)
        SDL_DestroyTexture(_vid_texture);
    if (_sub_texture)
        SDL_DestroyTexture(_sub_texture);

//	if (_renderer) {
//		SDL_DestroyRenderer(_renderer);
//		_renderer = NULL;
//	}
//
//	if (_window) {
//		SDL_DestroyWindow(_window);
//		_window = NULL;
//	}
//
//#if CONFIG_AVFILTER
//	av_freep(&_vfilters_list);
//#endif
//
//	avfilter_graph_free(&_agraph);
//	_agraph = NULL;
//
//	avformat_network_deinit();
//	SDL_Quit();
}

int VideoState::InitFmtCtx(){

    int ret = 0, err;
    AVFormatContext *ic = NULL;
    AVDictionaryEntry *t = NULL;
    int scan_all_pmts_set = 0;

    // 1. 创建上下文结构体，这个结构体是最上层的结构体，表示输入上下文
    ic = avformat_alloc_context();
    if (!ic) {
        //ret = PLAY_MEMORY_ERROR;
        ret = -1;
        goto fail;
    }

    /* 2. 设置中断回调函数，如果出错或者退出，就根据目前程序设置的状态选择继续check或者直接退出 */
    /* 当执行耗时操作时（一般是在执行while或者for循环的数据读取时），会调用interrupt_callback.callback
    * 回调函数中返回1则代表ffmpeg结束耗时操作退出当前函数的调用
    * 回调函数中返回0则代表ffmpeg内部继续执行耗时操作，直到完成既定的任务(比如读取到既定的数据包)
    */
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = this;

    /*
    * 特定选项处理
    * scan_all_pmts 是 mpegts的一个选项，这里在没有设定该选项的时候，强制设为1。
    *
    * av_dict_get：从字典获取一个条目，参1是字典，里面保存着返回的条目，参2是key，
    *                  参3一般传NULL，表示获取第一个匹配的key的字典，不为空代表找到的key的前面的key必须也要匹配。
    *                  参4是宏，具体看注释即可。
    * av_dict_set：设置一个key，是否覆盖和参4有关。看函数注释即可，不难。
    */

    if(_properties.HasProperty("fflags")){
        av_dict_set(&_format_opts, "fflags", "nobuffer", 0);
    }
    if(_properties.HasProperty("rtsp_transport")){
        av_dict_set(&_format_opts, "rtsp_transport", "tcp", 0);
    }
    if(_properties.HasProperty("rtsp_transport")){
        av_dict_set(&_format_opts, "analyzeduration", "2000000", 0);
    }

    if (!av_dict_get(_format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {// 如果没设置scan_all_pmts，则进行设置该key
        av_dict_set(&_format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);// 设置时不会覆盖现有的条目，即有该key时，不会进行设置。
        scan_all_pmts_set = 1;// 强制设为1
    }

    /* 3. 打开文件，主要是探测协议类型，如果是网络文件则创建网络链接等 */
    // 注意：-fflags nobuffer最终是通过format_opts在这里设置的，
    //  而format_opts中的-fflags nobuffer，是通过parse_options调用到回调函数opt_defalut从命令行参数获取并进行设置的。
    // nobuffer debug相关可参考：https://blog.51cto.com/fengyuzaitu/3028132.
    // 字典相关可参考：https://www.jianshu.com/p/89f2da631e16?utm_medium=timeline(包含avformat_open_input参4支持的参数)
    // 关于延时选项可参考：https://www.it1352.com/2040727.html
    err = avformat_open_input(&ic, _filename.c_str(), _iformat, &_format_opts);
    if (err < 0) {
        //printf("filename: %s, %d\n", _filename.c_str(), err);
        SPDERROR("filename: {}, {}", _filename.c_str(), err);
        ret = -1;
        goto fail;
    }
    // 执行完avformat_open_input后，会被重新置为NULL
    if (scan_all_pmts_set)
        av_dict_set(&_format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

    // key为空并且参4是AV_DICT_IGNORE_SUFFIX：代表返回该字典的第一个条目。即判定该字典是否为空。
    // 由于avformat_open_input会调用完后会将format_opts有效的条目置空，所以此时还有条目，代表该条目是不合法的。
    if ((t = av_dict_get(_format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        //av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        SPDINFO("Option {} not found.", t->key);
        ret = -2;
        goto fail;
    }

    // videoState的ic指向分配的ic
    _ic = ic;

    // 默认genpts是0，不产生缺失的pts。
    if (_genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;// 生成缺失的pts，即使它需要解析未来的帧

    /*
    * 该函数将导致全局端数据被注入到下一个包中的每个流，以及在任何后续的seek之后。
    * 看源码可以看到，该函数很简单，就是将s的所有AVStream的inject_global_side_data字段设置为1.
    * see https://blog.csdn.net/chngu40648/article/details/100653452?spm=1001.2014.3001.5501
    */
    av_format_inject_global_side_data(_ic);


    // 4. 探测媒体类型，可得到当前文件的封装格式，音视频编码参数等信息
    ret = avformat_find_stream_info(_ic, nullptr);
    if (ret < 0) {
        //SPDERROR("avformat_find_stream_info faild, errstr: {}", is->get_ffmpeg_error(ret).c_str());
        ret = -3;
        goto fail;
    }

fail:

    return ret;
}

/**
 * @brief 通过视频流的分辨率，来计算出SDL屏幕的显示窗口的起始坐标、宽高，
 *          结果保存在局部变量rect，但是只用到宽高，最终结果保存在全局变量default_width、default_height。
 *
 * @param width  视频的真实宽度。
 * @param height 视频的真实高度。
 * @param sar 宽高比，这里只是参考值，内部calculate_display_rect会进行修改。
 * @return void。
 *
 * set_default_window_size(codecpar->width, codecpar->height, sar);
 */
void VideoState::set_default_window_size(int width, int height, AVRational sar){

    SDL_Rect rect;

    // 1. 判断用户是否指定宽高进行渲染，如果有则按照用户选择，没有则按照视频流实际高度进行计算。
    // screen_width和screen_height可以在ffplay启动时设置 -x screen_width -y screen_height获取指定 的宽高，
    // 如果没有指定，则max_height = height，即是视频帧的高度。
    int max_width  = _screen_width  ? _screen_width  : INT_MAX;
    int max_height = _screen_height ? _screen_height : INT_MAX;
    if (max_width == INT_MAX && max_height == INT_MAX){
        max_height = height;                                    // 没有指定最大高度时，则使用传进来视频的高度作为高度计算
        //max_width  = width;                                   // tyycode，最好加，不然scr_width是一个很大的数值，虽然效果一样。
    }


    // 2. 通过用户指定的显示宽高，以及视频流实际的分辨率，得出默认渲染的宽高。
    // 重点在calculate_display_rect()函数，计算完后，SDL显示屏幕的起始坐标、宽高保存在rect。
    calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
    _default_width  = rect.w;
    _default_height = rect.h;
}

// 4.2.0 和4.0有区别
/**
 * @brief 将帧宽高按照sar最大适配到窗口。算法是这样的：
 *  1）如果用户不指定SDL显示屏幕的大小，则按照视频流分辨率的真实大小进行显示。例如测试时不加-x-y参数。
 *  2）若用户指定SDL显示屏幕的大小，按照视频流分辨率的真实大小的宽高比进行计算，先以用户的-x宽度为基准，若不符合则再以-y高度为基准，最终得出用户指定的屏幕显示宽高和起始坐标。
 *
 * 总结：总的思路就是，如何将视频流不一样的分辨率，输出到指定的屏幕上面显示。这也是我们面对的需求。
 *
 * @param rect      获取到的SDL屏幕起始位置和解码后的画面的宽高。
 * @param scr_xleft 窗口显示起始x位置,这里说的是内部显示的坐标, 不是窗口在整个屏幕的起始位置。
 * @param scr_ytop  窗口显示起始y位置。
 * @param scr_width 窗口宽度，可以是视频帧的原分辨率或者是用户指定的-x参数。
 * @param scr_height窗口高度，可以是视频帧的原分辨率或者是用户指定的-y参数。
 * @param pic_width 视频显示帧的真实宽度。
 * @param pic_height视频显示帧的真实高度。
 * @param pic_sar   显示帧宽高比，只是参考该传入参数。
 *
 * calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
 */
void VideoState::calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar)
{
    AVRational aspect_ratio = pic_sar; // 比率
    int64_t width, height, x, y;

    // 1. 判断传进来的宽高比是否有效，无效会被置为1:1。
    // 例如(9,16)与(0,1)比较之后，返回1，9>0，所以不会被置为1:1。
    // av_cmp_q：比较两个比率，比率1大于比率2返回1，等于返回0，小于返回-1.  这里参2传(0,1)是为了判断传进来的参数pic_sar的分子是否>0
    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
        aspect_ratio = av_make_q(1, 1);// 如果aspect_ratio是负数或者为0,设置为1:1。一般实时流都会走这里。

    // 2. 重新计算宽高比，转成真正的播放比例。不过大多数执行到这后，aspect_ratio={1,1}。所以宽高比就是pic_width/pic_height,一般是1920/1080=16:9。
    // 他这里曾经提到过如果传进来的pic_sar不是1:1的，那么aspect_ratio宽高比将被重新计算，
    // 导致宽高也会不一样。在一些录制可能会存在问题，这里先留个疑问。
    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    // 3. 根据上面得出的宽高比，求出用户指定的屏幕显示宽高。
    /* XXX: we suppose the screen has a 1.0 pixel ratio(我们假设屏幕的像素比为1.0) */
    // 计算显示视频帧区域的宽高.
    // 下面可以用用户指定-x 720 -y 480去代入理解。或者不传-x -y参数去理解，不难。
    // 先以高度为基准
    height = scr_height;
    // &~1, 表示如果结果是奇数的话，减去1，取偶数宽度，~1二进制为1110
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;// 例如用户指定720x480窗口播放，那么height=480，第一码流是1920/1080=16:9。算出whidth=853.333&~1=852.
    if (width > scr_width) {
        // 当以高度为基准,发现计算出来的结果在用户想要显示的窗口宽度不足时，调整为以窗口宽度为基准。不过需要注意：计算公式是不一样的，可以由宽高比相等的公式求出。x1/y1=x2/y2.
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;// 例如720x9/16=405&~1=404
    }

    // 4. 计算显示视频帧区域的起始坐标（在显示窗口内部的区域）
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;// 不论以高度还是宽度为基准，求出的x、y都会有一个是0.
    rect->x = scr_xleft + x;
    rect->y = scr_ytop  + y;
    rect->w = FFMAX((int)width,  1);
    rect->h = FFMAX((int)height, 1);
}

int VideoState::ReadThread(){

    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];      // 记录将要播放的音视频流下标。

    AVPacket pkt1, *pkt = &pkt1;
    int64_t stream_start_time;
    int pkt_in_play_range = 0;
    AVDictionaryEntry *t;
    SDL_mutex *wait_mutex = NULL;
    int scan_all_pmts_set = 0;
    int64_t pkt_ts;

    // 一、准备流程
    wait_mutex = SDL_CreateMutex();
    if (!wait_mutex) {
        SPDERROR("SDL_CreateMutex(): {}", SDL_GetError());
        //ret = PLAY_MEMORY_ERROR;
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(st_index, -1, sizeof(st_index));
    // 初始化为-1,如果一直为-1说明没相应stream
    _last_video_stream = _video_stream = -1;
    _last_audio_stream = _audio_stream = -1;
    _last_subtitle_stream = _subtitle_stream = -1;

    _eof = 0;    // =1是表明数据读取完毕

    ret = InitFmtCtx();
    if(ret < 0){
        ret = -1;
        goto fail;
    }

    if (_ic->pb)
        // 如果由于错误或eof而无法读取，则为True
        // FIXME hack, ffplay maybe should not use avio_feof() to test for the end
        _ic->pb->eof_reached = 0;

    // 初始化seek_by_bytes，表示是否按照字节数来进行seek
    if (_seek_by_bytes < 0) {
        // 0100000000001000000000000000(67,141,632) & 001000000000(0x0200=512) = 0
        int flag = _ic->iformat->flags & AVFMT_TS_DISCONT;   // 格式允许时间戳不连续。注意，muxers总是需要有效的(单调的)时间戳
        int cmp = strcmp("ogg", _ic->iformat->name);         // 例如ic->iformat->name = "mov,mp4,m4a,3gp,3g2,mj2"
        _seek_by_bytes = !!(flag) && cmp;                    // 首次执行运算后，应该是0字节。
    }
    _max_frame_duration = (_ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;// 一帧最大时长，为啥是10.0或者3600.0，一般值是3600.

    // 获取左上角的主题，但不是这里设置左上角的主题
    if (!window_title && (t = av_dict_get(_ic->metadata, "title", NULL, 0)))
        window_title = av_asprintf("%s - %s", t->value, _filename.c_str());

    /* if seeking requested, we execute it */
    /* 5. 检测是否指定播放起始时间。 若命令行设置了(-ss 00:00:30)，会在opt_seek设置start_time，设完后单位变为AV_TIME_BASE */
    if (_start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = _start_time;
        /* add the stream start time */
        if (_ic->start_time != AV_NOPTS_VALUE)
            // 为啥还要加上ic->start_time，感觉不加也可以，留个疑问。
            // 答：因为ic->start_time代表首帧的开始时间，一般是0.如果不是0，需要加上首帧的时间。代表首帧开始时间+你想要跳过的时间=知道播放器的起始时间。很简单。
            timestamp += _ic->start_time;

        // seek的指定的位置开始播放
        ret = avformat_seek_file(_ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            //av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
            //	is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    /* 是否为实时流媒体 */
    _realtime = is_realtime(_ic);

    /*
    * 打印关于输入或输出格式的详细信息。
    * 例如持续时间，比特率，流，容器，程序，元数据，侧数据，编解码器和时基。
    */
    //if (show_status)
    //	av_dump_format(ic, 0, is->filename, 0);

    // 6. 查找AVStream
    // 6.1 根据用户指定来查找流。 若用户命令传参进来，wanted_stream_spec会保存用户指定的流下标，st_index用于此时的记录想要播放的下标。
    for (i = 0; i < _ic->nb_streams; i++) {
        AVStream *st = _ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && _wanted_stream_spec[type] && st_index[type] == -1) {
            if (avformat_match_stream_specifier(_ic, st, _wanted_stream_spec[type]) > 0)
                st_index[type] = i;
        }
    }
    // 检测用户是否有输错流的种类(种类不是指视频、音频。而是指音频下有哪些种类，例如国语，粤语)。
    // 例如音频只有0和1两路(粤语和国语)，但你输入了 -ast 2，实际下标只有0和1，并没2。
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (_wanted_stream_spec[i] && st_index[i] == -1) {
            // 报错例子: "Stream specifier 2 does not match any audio stream"
            printf("Stream specifier %d does not match any %s stream\n", _wanted_stream_spec[i], av_get_media_type_string((AVMediaType)i));
            // st_index[i] = INT_MAX;
            st_index[i] = -1; // 报错，最好将对应的流的种类也置为-1，增强代码健壮性。
        }
    }

    /*
    *  int av_find_best_stream(AVFormatContext *ic,
    enum AVMediaType type,          // 要选择的流类型
    int wanted_stream_nb,           // 目标流索引，-1时会参考相关流。
    int related_stream,             // 相关流(参考流)索引。
    AVCodec **decoder_ret,
    int flags);
    */

    // 6.2 利用av_find_best_stream选择流。
    // 文件实时流都可以走这个流程，并且对应实时流，大多数之后走这个流程，因为实时流基本不存在多个同样的流，所以无法走上面指定的流播放。
    // 估计内部和for(int i = 0; i < _ic->nb_streams; i++)遍历去找的做法类似
    if (!_video_disable)
        // 若参3不为-1，则按照用户的选择流，为-1则自动选择；视频的相关流直接置为-1。
        st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(_ic, AVMEDIA_TYPE_VIDEO, st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    if (!_audio_disable)
        // 如果目标流参3是-1或者指定越界，则会参考相关流(即视频流)进行返回，一般默认返回第一个流或者最大的流，但不一定。
        st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(_ic, AVMEDIA_TYPE_AUDIO, st_index[AVMEDIA_TYPE_AUDIO], st_index[AVMEDIA_TYPE_VIDEO], NULL, 0);
    if (!_video_disable && !_subtitle_disable)
        // 字幕：如果目标流参3是-1，则会参考相关流(优先音频流，再考虑视频流)进行返回，一般默认返回第一个流或者最大的流，但不一定。
        st_index[AVMEDIA_TYPE_SUBTITLE] = av_find_best_stream(_ic, AVMEDIA_TYPE_SUBTITLE, st_index[AVMEDIA_TYPE_SUBTITLE],
        (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ? st_index[AVMEDIA_TYPE_AUDIO] : st_index[AVMEDIA_TYPE_VIDEO]), NULL, 0);


    // 通过上面的第6点，就能找到了对应的想要的音视频各自单独的流下标。

    // 这里应该还是一个默认值SHOW_MODE_NONE
    _show_mode = VideoState::SHOW_MODE_NONE;

    // 7. 从待处理流中获取相关参数，设置显示窗口的宽度、高度及宽高比
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = _ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        // 根据流和帧宽高比猜测视频帧的像素宽高比（像素的宽高比，注意不是图像的）
        // 为啥要猜呢？因为帧宽高比由编解码器设置，但流宽高比由解复用器设置，因此这两者可能是不相等的
        AVRational sar = av_guess_sample_aspect_ratio(_ic, st, NULL);// 实时流一般起始是{0, 1}，文件是{1,1}，不过后面会在set_default_window_size里面被重设
        if (codecpar->width) {
            // 设置显示窗口的大小和宽高比
            set_default_window_size(codecpar->width, codecpar->height, sar);
        }
    }

    /* open the streams */
    /* 8. 打开视频、音频解码器。在此会打开相应解码器，并创建相应的解码线程。 */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {// 如果有音频流则打开音频流
        stream_component_open(st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) { // 如果有视频流则打开视频流
        ret = stream_component_open(st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (_show_mode == VideoState::SHOW_MODE_NONE) {
        //选择怎么显示，如果视频打开成功，就显示视频画面，否则，显示音频对应的频谱图
        _show_mode = ret >= 0 ? VideoState::SHOW_MODE_VIDEO : VideoState::SHOW_MODE_RDFT;
    }

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) { // 如果有字幕流则打开字幕流
        stream_component_open(st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (_video_stream < 0 && _audio_stream < 0) {
        //SPDERROR("Failed to open file '{}' or configure filtergraph", is->_filename);
        //ret = PLAY_FF_NO_AV_STREAM_OR_FILTER;
        ret = -4;
        goto fail;
    }

    // buffer是否需要无穷大 并且 是实时流。
    if (_infinite_buffer < 0 && _realtime)
        _infinite_buffer = 1;    // 如果是实时流

    while (1) {
        ret = av_read_frame(_ic, pkt);
        if (ret >= 0) {
            if ((pkt->flags & AV_PKT_FLAG_KEY) && _video_stream == pkt->stream_index) {
                _videoq.packet_queue_put(pkt);
                break;
            }
            else {
                av_packet_unref(pkt);// // 不入队列则直接释放数据
            }
        }
    }


    /*
    * 二、For循环流程
    */
    for (;;) {
        // 1 检测是否退出
        if (_abort_request)
            break;

        // 2 检测是否暂停/继续，更新last_paused，以及网络流的状态。
        if (_paused != _last_paused) {
            _last_paused = _paused;
            if (_paused)
                _read_pause_return = av_read_pause(_ic); // 网络流的时候有用
            else
                av_read_play(_ic);
        }

        // 暂停 并且是 (rtsp或者是mmsh协议，那么睡眠并continue到for)
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (_paused && (!strcmp(_ic->iformat->name, "rtsp") || (_ic->pb && !strncmp(_filename.c_str(), "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            // 等待10ms，避免立马尝试下一个Packet
            SDL_Delay(10);
            continue;
        }
#endif
        // 3 检测是否seek(读线程的快进快退seek是从这里开始的)。
        if (_seek_req) { // 是否有seek请求
            int64_t seek_target = _seek_pos; // 目标位置
            int64_t seek_min = _seek_rel > 0 ? seek_target - _seek_rel + 2 : INT64_MIN;
            int64_t seek_max = _seek_rel < 0 ? seek_target - _seek_rel - 2 : INT64_MAX;
            // 前进seek seek_rel>0
            //seek_min    = seek_target - is->seek_rel + 2;
            //seek_max    = INT64_MAX;
            // 后退seek seek_rel<0
            //seek_min = INT64_MIN;
            //seek_max = seek_target + |seek_rel| -2;
            //seek_rel =0  鼠标直接seek
            //seek_min = INT64_MIN;
            //seek_max = INT64_MAX;

            /*
            * FIXME the +-2 is due to rounding being not done in the correct direction in generation
            *  of the seek_pos/seek_rel variables. 修复由于四舍五入，没有在seek_pos/seek_rel变量的正确方向上进行.
            */
            ret = avformat_seek_file(_ic, -1, seek_min, seek_target, seek_max, _seek_flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n", _ic->url);
            }
            else {
                /* seek的时候，要把原先的数据清空，并重启解码器，put flush_pkt的目的是告知解码线程需要reset decoder.
                * 其中：
                * 清空packet队列：音视频流、字幕流都在本read_frame读线程处理。
                * 清空帧队列在：音频 sdl_audio_callback->audio_decode_frame->do while (af->serial != is->audioq.serial)处理。
                *             视频 video_refresh->if (vp->serial != is->videoq.serial)。
                * 重置解码器在：音频 audio_thread->decoder_decode_frame->if (pkt.data == flush_pkt.data)时处理。
                *             视频 video_thread->get_video_frame->decoder_decode_frame->if (pkt.data == flush_pkt.data)时处理。
                *              实际重置解码器时，音视频是一样的，最后都调用decoder_decode_frame。
                */
                if (_audio_stream >= 0) { // 如果有音频流
                    _audioq.packet_queue_flush();    // 清空packet队列数据
                                                        // 放入flush pkt, 用来开起新的一个播放序列, 解码器读取到flush_pkt会重置解码器以及清空帧队列。
                    _audioq.packet_queue_put(&flush_pkt);
                }
                if (_subtitle_stream >= 0) { // 如果有字幕流
                    _subtitleq.packet_queue_flush(); // 和上同理
                    _subtitleq.packet_queue_put(&flush_pkt);
                }
                if (_video_stream >= 0) {    // 如果有视频流
                    _videoq.packet_queue_flush();    // 和上同理
                    _videoq.packet_queue_put(&flush_pkt);
                }
                if (_seek_flags & AVSEEK_FLAG_BYTE) {
                    _extclk.set_clock(NAN, 0);
                }
                else {
                    _extclk.set_clock(seek_target / (double)AV_TIME_BASE, 0);// 时间戳方式seek，seek_target / (double)AV_TIME_BASE代表seek目标位置的时间点，单位微秒。
                }
            }
            _seek_req = 0;               // 这里看到，如果用户多次触发seek请求，实际只会处理一次。
            _queue_attachments_req = 1;
            _eof = 0;                   // 细节。0未读取完毕，1完毕。
            if (_paused)
                step_to_next_frame(); // 暂停状态下需要播放seek后的第一帧。
        }

        // 4 检测video是否为attached_pic
        if (_queue_attachments_req) {
            // attached_pic 附带的图片。比如说一些MP3，AAC音频文件附带的专辑封面，所以需要注意的是音频文件不一定只存在音频流本身
            if (_video_st && _video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                AVPacket copy = { 0 };
                if ((ret = av_packet_ref(&copy, &_video_st->attached_pic)) < 0)
                    goto fail;
                _videoq.packet_queue_put(&copy);
                _videoq.packet_queue_put_nullpacket(_video_stream);
            }
            _queue_attachments_req = 0;
        }

        // 5 检测队列是否已经有足够数据。
        // 下面只是粗略判断是否为满，因为此时解码线程可能从队列中读取包，而这里是没有上锁的；与解码线程的判断队列为空一样，也是粗略判断。
        // 也就是说，队列为满或者为空时，是可粗略判断的，但是队列放入取出绝对不能。
        /* 缓存队列有足够的包，不需要继续读取数据 */
        // 下面if条件：不满足缓存无穷大的包数据 && (音视频、字幕队列大于设定的15M || 音视频、字幕队列都有足够的包) 的情况下，才认为已经读取到了足够的数据包
        if (_infinite_buffer < 1 &&      // 不满足缓存无穷大的包数据
            (_audioq.packet_queue_get_size() + _videoq.packet_queue_get_size() + _subtitleq.packet_queue_get_size() > MAX_QUEUE_SIZE ||       // 音视频、字幕队列大于设定的15M
                 (stream_has_enough_packets(_audio_st, _audio_stream, &_audioq) &&  // 音视频、字幕队列都有足够的包，才认为已经读取到了足够的数据包
                    stream_has_enough_packets(_video_st, _video_stream, &_videoq) &&
                    stream_has_enough_packets(_subtitle_st, _subtitle_stream, &_subtitleq)))) {

            /* wait 10 ms */
            SDL_LockMutex(wait_mutex);
            // 如果没有唤醒则超时10ms退出，比如在seek操作时这里会被唤醒。
            // 重点：ffplay的packetqueue的锁+条件变量的设计原理：
            // 1）这里额外添加了一个局部变量锁wait_mutex + continue_read_thread，是为了让锁的粒度更小，让读线程可以SDL_CondWaitTimeout超时退出。
            // 2）因为我们平时都是使用一把锁+一个条件变量，这里使用两把锁+两个条件变量。具体看代码目录下的测试代码testWaitMutex.cpp。
            // 3）ffplay的packetqueue的锁+条件变量的设计就是测试代码中的" 3. 解决的方法xxx"的思路。
            SDL_CondWaitTimeout(_continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }

        // 6 检测码流是否已经播放结束。只有非暂停 并且 音视频都播放完毕 才认为播放结束。
        // auddec.finished == is->audioq.serial会在对应的解码器线程标记，例如音频的audio_thread
        if (!_paused /*非暂停*/
            && // 这里的执行是因为码流读取完毕后 插入空包所致
            (!_audio_st // 没有音频流，那肯定就是认为播放完毕
                || (_auddec.decoder_get_finished() == *_audioq.packet_queue_get_serial() // 或者音频播放完毕(is->auddec.finished相等is->audioq.serial 并且 帧队列帧数为0)
                    && _sampq.frame_queue_nb_remaining() == 0))
            && (!_video_st // 没有视频流，那肯定就是认为播放完毕
                || (_viddec.decoder_get_finished() == *_videoq.packet_queue_get_serial() // 或者视频播放完毕._finished=_serial，在decoder_decode_frame解码判断结束时赋值。
                    && _pictq.frame_queue_nb_remaining() == 0)))
        {
            if (_realtime) {// tyycode, 实时流直接退出.
                //ret = PLAY_FF_EOF;
                ret = -1;
                //SPDERROR("_realtime encounters eof, url: {}", _filename.c_str());
                goto fail;
            }
            if (_loop != 1 /* 是否循环播放 */ && (!_loop || --_loop)) {// 这个if条件留个疑问
                // stream_seek不是ffmpeg的函数，是ffplay封装的，每次seek的时候会调用
                stream_seek(_start_time != AV_NOPTS_VALUE ? _start_time : 0, 0, 0);
            }
            else if (_autoexit) {  // b 是否自动退出
                //SPDWARN("_autoexit, url: {}", _filename.c_str());
                //ret = PLAY_FF_EOF;
                ret = -1;
                goto fail;
            }
        }

        // 7. 读取媒体数据，得到的是音视频分离后、解码前的数据
        ret = av_read_frame(_ic, pkt); // 调用不会释放pkt的数据，需要我们自己去释放packet的数据
        // 8 检测数据是否读取完毕。
        if (ret < 0) {
            // 这里是读到文件尾部正常退出(但是实时流rtsp连接异常断开也会这里)，所以需要刷空包去清空解码器中剩余的数据去显示。但个人觉得使用avio_feof判断不太正确。
            // 正常退出流程：1）先发送空包，以清空pkt队列、帧队列、解码器，因为读线程av_read_frame会一直EOF，然后continue，所以一直在第6步等待解码线程清空的工作；
            //					2）解码线程读到空包清空解码器后，解码线程会解码读到EOF，标记为_finished=_serial；
            //                  3）最终读线程在上面第6步退出。
            //              所以读到EOF后，刷空就能把pkt队列、帧队列、解码器内的内容清除掉。
            // avio_feof：当且仅当在文件末尾或读取时发生错误时返回非零。
            // 真正读到文件末尾 或者 文件读取错误 并且 文件还未标记读取完毕，此时认为数据读取完毕。
            // 在这里，avio_feof的作用是判断是否读取错误，因为读到文件末尾由AVERROR_EOF判断了。
            if ((ret == AVERROR_EOF || avio_feof(_ic->pb)) && !_eof)
            {
                // 插入空包说明码流数据读取完毕了，之前讲解码的时候说过刷空包是为了从解码器把所有帧都读出来
                if (_video_stream >= 0)
                    _videoq.packet_queue_put_nullpacket(_video_stream);// 这里看到插入空包时，可以不需要关心其返回值。
                if (_audio_stream >= 0)
                    _audioq.packet_queue_put_nullpacket(_audio_stream);
                if (_subtitle_stream >= 0)
                    _subtitleq.packet_queue_put_nullpacket(_subtitle_stream);
                _eof = 1;        // 标记文件读取完毕
                //SPDWARN("read_frame read EOF, url: {}", _filename.c_str());
            }
            // 这里是意外退出的，所以就不刷空包显示剩余的数据，直接退出。
            if (_ic->pb && _ic->pb->error) {
                // 读取错误，读数据线程直接退出。
                //SPDERROR("{} ic->pb->error, errstr: {}", _filename.c_str(), get_ffmpeg_error(ret).c_str());
                //ret = PLAY_FF_PB_ERROR;
                ret = -1;
                //break;
                goto fail;
            }

            // 这里应该是AVERROR(EAGAIN)，代表此时读不到数据，需要睡眠一下再读。
            // AVERROR_EOF也会执行这里的内容。
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(_continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);

            continue;		// 继续循环
        }else {
            // 成功读取一个pkt，eof标记为0
            _eof = 0;
        }

        // 9 检测是否在播放范围内
        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = _ic->streams[pkt->stream_index]->start_time; // 获取流的起始时间(不是上面ic里面的起始时间)。debug发现流里面的start_time，文件或者实时流起始都不是0.
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;      // 获取packet的时间戳。为空则使用dts。单位是AVStream->time_base。
        // 这里的duration是在命令行时用来指定播放长度
        //int tyytest = _duration == AV_NOPTS_VALUE;                  // 没有设置的话，一直是true.
        pkt_in_play_range = _duration == AV_NOPTS_VALUE ||
            (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
            av_q2d(_ic->streams[pkt->stream_index]->time_base) -
            (double)(_start_time != AV_NOPTS_VALUE ? _start_time : 0) / 1000000
            <= ((double)_duration / 1000000);// 两次start_time是不一样的注意，留个疑问。不过没有设置duration就不需要理会||后面的内容。
                                            // 答：pkt_ts、stream_start_time单位都是AVStream->time_base，所以需要除以该单位转成秒单位，
                                            // ic->start_time单位是AV_TIME_BASE，同样需要除以一百万转成秒，再计算
                                            // pkt_ts - stream_start_time表示该流经过的时长，而再减去start_time，是因为start_time才是真正的起始，stream_start_time可能是在
                                            // start_time后一点才记录的，所以需要减去，不过一般start_time是AV_NOPTS_VALUE相当于0.所以该流已经播放的时长按照
                                            // pkt_ts - stream_start_time计算也是没有太大问题的。

        // 10 将音视频数据分别送入相应的queue中
        if (pkt->stream_index == _audio_stream && pkt_in_play_range) {
            _audioq.packet_queue_put(pkt);
        }
        else if (pkt->stream_index == _video_stream && pkt_in_play_range
            && !(_video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {// 是视频流&&在播放范围&&不是音频的封面
                                                                            //printf("pkt pts:%ld, dts:%ld\n", pkt->pts, pkt->dts);
            _videoq.packet_queue_put(pkt);
        }
        else if (pkt->stream_index == _subtitle_stream && pkt_in_play_range) {
            _subtitleq.packet_queue_put(pkt);
        }
        else {
            av_packet_unref(pkt);// // 不入队列则直接释放数据
        }

    }// <== for(;;) end ==>

     // 三 退出线程处理
    ret = 0;// 只有break才会走到这里.break退出读线程的有：1. _abort_request=1;

    // goto fail：1. for之前的失败都走这里; 2. attached_pic; 3. 实时流遇到eof 以及 _autoexit自动退出
fail:
    if (_ic && !_ic){
        avformat_close_input(&_ic);
    }

    if (_abort_request) {
        SDL_Event event;

        event.type = FF_INITIATIVE_QUIT_EVENT;
        event.user.data1 = this;
        SDL_PushEvent(&event);
    }

    if (ret != 0) {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = this;
        SDL_PushEvent(&event);
    }

    if (wait_mutex) {
        SDL_DestroyMutex(wait_mutex);
        wait_mutex = NULL;
    }

    //SPDWARN("read_thread exit, ret: {}", ret);
    printf("read_thread exit, ret: %d\n", ret);
    return 0;
}

int read_thread(void *arg)
{
    VideoState *is = (VideoState*)arg;
    if(NULL == is){
        return -1;
    }

    int ret = is->ReadThread();// 将读线程封装到C++，方便调用内部成员.
    if(ret != 0){
        //printf("ReadThread error, errno: %d\n", ret);
        SPDERROR("ReadThread error, errno: {}", ret);
        return ret;
    }

    //printf("ReadThread exit, errno: %d\n", ret);
    SPDERROR("ReadThread exit, errno: {}", ret);
    return 0;
}

void VideoState::read_thread_close() {
    if (_ic) {
        avformat_close_input(&_ic);
        _ic = NULL;
    }
}

/**
 * 程序开头先设置动态库的搜索路径，将当前路径去除(SetDllDirectory函数传空字符串即可)，不做为dll的搜索路径，
 * 防止由于windows下的DLL搜索路径顺序缺陷造成的DLL劫持问题。
 */
void init_dynload(void)
{
#ifdef _WIN32
    /* Calling SetDllDirectory with the empty string (but not NULL) removes the
     * current working directory from the DLL search path as a security pre-caution. */
    // 意思是防止由于windows下的DLL搜索路径顺序造成的DLL劫持问题。
    // 具体看 https://blog.csdn.net/magictong/article/details/6931520。
    SetDllDirectory(__TEXT(""));
#endif
}

void *VideoState::grow_array(void *array, int elem_size, int *size, int new_size)
{
    if (new_size >= INT_MAX / elem_size) {
        av_log(NULL, AV_LOG_ERROR, "Array too big.\n");
        return NULL;
        //exit_program(1);
    }
    if (*size < new_size) {
        uint8_t *tmp = (uint8_t *)av_realloc_array(array, new_size, elem_size);
        if (!tmp) {
            av_log(NULL, AV_LOG_ERROR, "Could not alloc buffer.\n");
            return NULL;
            //exit_program(1);
        }
        memset(tmp + *size*elem_size, 0, (new_size-*size) * elem_size);
        *size = new_size;
        return tmp;
    }
    return array;
}

#define GROW_ARRAY(array, nb_elems)\
    array = grow_array(array, sizeof(*array), &nb_elems, nb_elems + 1)

#if CONFIG_AVFILTER
int VideoState::opt_add_vfilter(void *optctx, const char *opt, const char *arg)
{
    //GROW_ARRAY((void*)_vfilters_list, _nb_vfilters);
    _vfilters_list = (const char**)grow_array((void*)_vfilters_list, sizeof(*_vfilters_list), &_nb_vfilters, _nb_vfilters + 1);
    _vfilters_list[_nb_vfilters - 1] = (const char*)arg;
    return 0;
}
#endif

bool VideoState::Init(HWND handle, Properties pp) {

    int flags;

    _properties = pp;
    if(!_properties.HasProperty("play_url")){
        return false;
    }
    _filename = _properties.GetProperty("play_url");

    if(_properties.HasProperty("vf")){
        opt_add_vfilter(NULL, NULL, _properties.GetProperty("vf"));//给视频添加过滤器，用于显示字幕，可以看到即使字幕代码被注释掉，仍然可以通过过滤器进行显示.
    }


#ifdef USE_DLL_PLAY
#else
   init_dynload();
#endif

    /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    // 3. SDL的初始化
    flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;// flags = 00110001
    /* 是否运行音频 */
    if (_audio_disable) {
        flags &= ~SDL_INIT_AUDIO;// 00110001 & 1110 1111 = 0010 0001，即去掉音频标志位
    }
    else {
        /* Try to work around an occasional ALSA buffer underflow issue when the
        * period size is NPOT due to ALSA resampling by forcing the buffer size.
        * 尝试解决一个偶然的ALSA缓冲区下溢问题，周期大小是NPOT，由于ALSA重采样强迫缓冲区大小。
        */
        if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
            SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);
    }

    if (_video_disable)
        flags &= ~SDL_INIT_VIDEO;// 同理，去掉视频标志位

    // SDL_Init函数参考：https://blog.csdn.net/qq_25333681/article/details/89787836
    if (SDL_Init(flags)) {
        //SPDERROR("Could not initialize SDL, errno: {}\n", SDL_GetError());
        //SPDERROR("Did you set the DISPLAY variable?");
        return false;
    }

    /*
    * 从队列删除系统和用户的事件。
    *  该函数允许您设置处理某些事件的状态。
    * -如果state设置为::SDL_IGNORE，该事件将自动从事件队列中删除，不会过滤事件。
    *
    * -如果state设置为::SDL_ENABLE，则该事件将被处理正常。
    *
    * -如果state设置为::SDL_QUERY, SDL_EventState()将返回指定事件的当前处理状态。
    */
    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    //av_init_packet(&_flush_pkt);				// 初始化flush_packet
    //_flush_pkt.data = (uint8_t *)&_flush_pkt;   // 初始化为数据指向自己本身
    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *)&flush_pkt;

    // 4. 创建窗口
#ifdef USE_DLL_PLAY
    if(NULL == handle){
        return false;
    }
    //  返回创建完成的窗口的ID。如果创建失败则返回0。
    _hDisplayWindow = handle;
    _window = SDL_CreateWindowFrom(_hDisplayWindow);
    SDL_ShowWindow(_window);// 这里不调用的话，destory窗口后，第二次播放会无画面.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");// 创建优先级提示，"1"或"linear" - 表示 线性滤波(OpenGL和Direct3D支持)
    if (_window) {
        // 创建renderer(see details https://blog.csdn.net/leixiaohua1020/article/details/40723085)
#ifdef USE_X32
        // 使用32位的ffmpeg+SDL的dll时，SDL_RENDERER_SOFTWARE才有画面，而使用SDL_RENDERER_ACCELERATED会黑屏，
        // 因为32是为windows xp准备的，xp只支持SDL_RENDERER_SOFTWARE参数。
        // 而64位的ffmpeg+SDL的dll时，SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC正常。
        // 使用SDL_RENDERER_SOFTWARE参数时，可以明显看到渲染速度是慢了差不多1s的
        _renderer = SDL_CreateRenderer(_window, -1, SDL_RENDERER_SOFTWARE);
#else
        //使用硬件加速，并且设置和显示器的刷新率同步
        _renderer = SDL_CreateRenderer(_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
#endif
        if (!_renderer) {
            //SPDWARN("Failed to initialize a hardware accelerated renderer: {}", SDL_GetError());
            _renderer = SDL_CreateRenderer(_window, -1, 0);
        }
        if (_renderer) {
            if (!SDL_GetRendererInfo(_renderer, &_renderer_info)) {
                //SPDINFO("Initialized {} renderer.\n", _renderer_info.name);
            }
        }
    }

    // 窗口、渲染器、渲染器中可用纹理格式的数量其中一个失败，程序都退出
    if (!_window || !_renderer || !_renderer_info.num_texture_formats) {
        av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
        //SPDERROR("Failed to create window or renderer: {}", SDL_GetError());
        return false;
    }

#else
        if (!_display_disable) {
            int flags = SDL_WINDOW_HIDDEN;      // 窗口不可见
            if (_alwaysontop)                    // alwaysontop是否置顶，不过设置了也没用，应该和版本是否支持有关系
    #if SDL_VERSION_ATLEAST(2,0,5)
                flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    #else
                av_log(NULL, AV_LOG_WARNING, "Your SDL version doesn't support SDL_WINDOW_ALWAYS_ON_TOP. Feature will be inactive.\n");
    #endif

            // 是否禁用窗口大小调整
            if (_borderless)
                flags |= SDL_WINDOW_BORDERLESS; // 没有窗口装饰，即没有最外层
            else
                flags |= SDL_WINDOW_RESIZABLE;  // 窗口可以调整大小

            //  返回创建完成的窗口的ID。如果创建失败则返回0。
            const char program_name[] = "ffplay";
            _window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, _default_width, _default_height, flags);
            //SDL_ShowWindow(_window);
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");// 创建优先级提示，"1"或"linear" - 表示 线性滤波(OpenGL和Direct3D支持)
            if (_window) {
                // 创建renderer(see details https://blog.csdn.net/leixiaohua1020/article/details/40723085)
                _renderer = SDL_CreateRenderer(_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);//使用硬件加速，并且设置和显示器的刷新率同步
                if (!_renderer) {
                    av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
                    _renderer = SDL_CreateRenderer(_window, -1, 0);
                }
                if (_renderer) {
                    if (!SDL_GetRendererInfo(_renderer, &_renderer_info))
                        av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", _renderer_info.name);
                }
            }

            // 窗口、渲染器、渲染器中可用纹理格式的数量其中一个失败，程序都退出
            if (!_window || !_renderer || !_renderer_info.num_texture_formats) {
                av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
                //do_exit(NULL);
                return false;
            }
        }
    }
#endif

    _ytop = 0;
    _xleft = 0;

    /* 初始化视频帧、音频帧、字幕帧队列 */
    // 注意帧视频音频队列的keep_last是传1的，字幕是0.
    if (_pictq.frame_queue_init(&_videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        return false;
    if (_subpq.frame_queue_init(&_subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        return false;
    if (_sampq.frame_queue_init(&_audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        return false;

    /* 初始化视频包、音频包、字幕包队列 */
    if (_videoq.packet_queue_init() < 0 || _audioq.packet_queue_init() < 0 || _subtitleq.packet_queue_init() < 0)
        return false;

    /* 创建读线程的条件变量 */
    if (!(_continue_read_thread = SDL_CreateCond())) {
        //SPDERROR("create _continue_read_thread SDL_CreateCond() failed, errno: {}", SDL_GetError());
        return false;
    }

    /*
    * 初始化时钟
    * 时钟序列->queue_serial，实际上指向的是包队列里面的serial，例如is->videoq.serial，默认初始化时是0。
    */
//    _vidclk.init_clock(&_videoq._serial);
//    _audclk.init_clock(&_audioq._serial);
//    _extclk.init_clock(&_extclk._serial);
    _vidclk.init_clock(_videoq.packet_queue_get_serial());
    _audclk.init_clock(_audioq.packet_queue_get_serial());
    _extclk.init_clock(_extclk.get_serialc());
    _audio_clock_serial = -1;

    /* 初始化音量 */
    _startup_volume = 50;

    _startup_volume = av_clip(_startup_volume, 0, 100);
    _startup_volume = av_clip(SDL_MIX_MAXVOLUME * _startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
    _audio_volume = _startup_volume;
    _muted = 0;									 // =1静音，=0则正常
    _av_sync_type = AV_SYNC_AUDIO_MASTER;        // 音视频同步类型，默认音频同步！！！
    //_av_sync_type = AV_SYNC_VIDEO_MASTER;
    _frame_rate = 25.0;
    _captureCount = 0;
    _capturePath = "";
    _cursor_hidden = 0;

    SPDINFO("Init ok");
    return true;
}

double VideoState::vp_duration(Frame *vp, Frame *nextvp)
{
    if (vp->serial == nextvp->serial) { // 同一播放序列，序列连续的情况下
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration)         // duration 数值异常
            || duration <= 0    // pts值没有递增时
            || duration > _max_frame_duration    // 超过了最大帧范围
            ) {
            return vp->duration;	 /* 异常时以帧时间为基准(1秒/帧率) */
        }
        else {
            return duration; //使用两帧pts差值计算duration，一般情况下也是走的这个分支
        }
    }
    else {        // 不同播放序列, 序列不连续则返回0
        return 0.0;
    }
}

double VideoState::compute_target_delay(double delay)
{
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */

    /* 1. 只有当前主Clock源不是video才往下走，否则直接返回delay */
    if (get_master_sync_type() != AV_SYNC_VIDEO_MASTER) {
        /* if video is slave, we try to correct big delays by
        duplicating or deleting a frame。
        通过重复帧或者删除帧来纠正延迟。
        */

        // 2. 求出从时钟与主时钟的pts差值。diff作用是：判断视频超前或者落后的大小，正数超前，负数落后。
        diff = _vidclk.get_clock() - get_master_clock();

        /* skip or repeat frame. We take into account the
        delay to compute the threshold. I still don't know
        if it is the best guess
        */
        // 3. 根据delay求出对应的音视频同步精度阈值。
        // sync_threshold作用是：根据视频落后或者超前的差距大小(即diff值)，调整对应的音视频同步幅度。
        // 如果落后或者超前差距大于这个大小，则进行同步调整，否则差距在-sync_threshold ~ +sync_threshold以内，不做处理。
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));// 确保在[40,100]ms的范围。

        // 4. diff在max_frame_duration内则往下走，否则diff超过max_frame_duration以上的pts，直接返回delay。
        // 依赖新的一帧的pts去调用update_video_pts去更新pts，以让diff恢复正常。如果diff一直处于>max_frame_duration的状态，
        // 那么是无法同步的，那就不管了，不过这种情况不怎么存在，一般是由于外部操作导致的。
        if (!isnan(diff) && fabs(diff) < _max_frame_duration) {
            /*
            * 5. 视频落后音频了。并且落后的diff值 <= 负的音视频同步的阈值。
            * -sync_threshold在[-100,-40]ms的范围，所以只有diff在[负无穷,-sync_threshold]的范围才会满足条件(可以画个图或者看音频同步pdf的图)。
            * 5.1 例如假设delay=0.05s，-sync_threshold=-0.05s，再假设diff=0.2s-1s=-0.8s，得出delay=0.05s+(-0.8s)=-0.75s，
            * 经过FFMAX处理，最终返回0.
            * 5.2 而若diff在 > -sync_threshold的落后情况，那么也是直接返回delay。例如这个例子的diff=0.96s-1s=-0.04s到0之前的值。
            * 注意diff=0以及正数是不行的，正数代表超前而不是落后了，0不满足fabs(diff)条件。
            */
            if (diff <= -sync_threshold) {
                delay = FFMAX(0, delay + diff); // 上一帧持续的时间往小的方向去调整
            }
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {
                /* 6. 视频超前了 */
                // 这里的意思是当diff值太大时，需要使用diff去计算delay。
                // 例如，当传入的帧间隔delay过大，那么计算出来的sync_threshold也会很大，所以只有diff比这个阈值sync_threshold更大才能进来这个流程。
                // 以具体值为例，假设delay传进来=0.2s(200ms)，那么sync_threshold=0.1s(100ms)，所以只有diff大于100ms才能进来。
                // 假设diff=1s(1000ms)，那么计算后得到最终的delay=0.2s+1s=1.2s。最终音视频同步后，这一帧还需要显示1.2s才能显示下一帧。
                // 其中睡的0.2s是代表这一帧到下一帧还需要等待的时长，1s是要等待音频的时长，也就是音视频需要同步的时长。
                // 视频超前
                // ffpaly直接使用浮点数比较不太好，后续可以进行优化

                delay = delay + diff; // 上一帧持续时间往大的方向去调整
                //av_log(NULL, AV_LOG_INFO, "video: delay=%0.3f A-V=%f\n", delay, -diff);//A-V：代表音频减去视频的差距
            }
            else if (diff >= sync_threshold) {
                /* 7. 同样是视频超前了，只不过处理的方式不一样，上面的超前值比较大 */
                // 上一帧持续时间往大的方向去调整
                // 例如delay=0.05s，diff=(160-100)ms=0.06s，sync_threshold=0.05s，那么不满足delay > AV_SYNC_FRAMEDUP_THRESHOLD的条件，
                // 按照delay = 2 * delay计算，delay=0.1s。实际上和delay = delay + diff=0.11s差了10ms，但是由于delay每次都是动态计算的，这种误差可以认为是可忽略的，
                // 不过有些人不喜欢这种误差情况，就把ffplay的AV_SYNC_FRAMEDUP_THRESHOLD=0.1的值改成0.04，尽量让它走上面的流程。
                // 总之，大家可以按照自己的实际情况和想法去测试，去达到最好的要求即可，尚且ffplay的作者也不知道这种同步处理是否是最好的。
                delay = 2 * delay;
            }
            else {
                // 8. 音视频同步精度在 -sync_threshold ~ +sync_threshold，不做处理。
                // 其他条件就是 delay = delay; 维持原来的delay, 依靠frame_timer+duration和当前时间进行对比

                // 例如get_clock(&is->vidclk) - get_master_clock(is)获取到的值是120ms与100ms，那么diff=20ms。delay=40ms传入，那么sync_threshold=40.
                // is->max_frame_duration一般是3600，那么由于20在阈值40内，所以会来到这个else流程。
            }
        }
        else if (fabs(diff) < _max_frame_duration) {
            // tyycode
            //printf("fabs(diff) > max_frame_duration\n");
        }
    }
    else {
        // 9. 如果是以video为同步，则直接返回last_duration
    }

    //av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);

    return delay;
}

void VideoState::update_video_pts(double pts, int64_t pos, int serial) {
    /* update current video pts */

    // 以sync=video为例：
    // 1）假设第一次，pts传进来为2，set_clock时time假设是102；那么pts_drift=2-102=-100;这个是update_video_pts函数调用set_clock设置视频时钟的步骤。
    // 2）然后调用sync_clock_to_slave。其步骤是：
    //      2.1 内部首先获取随时间流逝的pts，由于首次extclk是nan，所以内部获取的clock是nan；
    //      2.2 然后因为刚刚设了视频的时钟，并且1和2）的调用时间很短可以忽略不计，那么slave_clock为-100+102=2；
    //      2.3 然后就是调用set_clock()用从时钟设置主时钟，那么此时主时钟就和从时钟可以认为是一样的。
    //      即视频同步时，外部时钟的结构体被用来作为主时钟，但不是真正的主时钟，只是使用它的结构体记录值而已。
    _vidclk.set_clock(pts, serial);
    _extclk.sync_clock_to_slave(&_vidclk);// 参1为外部时钟是因为，当设置了外部时钟为主时钟时，需要参考视频与音频，所以视频的时钟也要更新到外部时钟。
}

void VideoState::get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode)
{
    int i;
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;

    // 1. 如果FFmpeg格式为下面四种，则获取SDL的混合模式并通过参数传出。如果不是，SDL的混合模式是SDL_BLENDMODE_NONE。
    if (format == AV_PIX_FMT_RGB32 ||
        format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32 ||
        format == AV_PIX_FMT_BGR32_1)
        *sdl_blendmode = SDL_BLENDMODE_BLEND;

    // 2. 遍历FFmpeg PIX_FMT to SDL_PIX的映射关系数组，如果在数组中找到该格式，
    //    说明FFmpeg和SDL都支持该格式，那么直接返回，由参数传出。找不到则为SDL_PIXELFORMAT_UNKNOWN。
    for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
        if (format == sdl_texture_format_map[i].format) {
            *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            return;
        }
    }
}

int VideoState::realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture)
{
    Uint32 format;
    int access, w, h;

    /*
    * 1）SDL_QueryTexture：用于检索纹理本身的基本设置，包括格式，访问，宽度和高度。
    */

    // 1. 若传进来的帧属性与纹理的像素属性不一样，该纹理会释放掉并重新创建。
    if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format) {
        void *pixels;
        int pitch;
        if (*texture)
            SDL_DestroyTexture(*texture);
        if (!(*texture = SDL_CreateTexture(_renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
            return -1;
        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
            return -1;
        if (init_texture) {
            if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)// 要访问纹理的像素数据，必须使用函数SDL_LockTexture()
                return -1;
            // 这里能完全初始化纹理吗？
            // 例如yuv数据，总字节数大小是1.5xnew_widthxnew_height。注：yuv420中，一个像素包含1.5个字节.
            // 一般new_width=pitch，所以这里只将pitch * new_height置0，剩余的0.5pitch * new_height呢？留个疑问
            memset(pixels, 0, pitch * new_height);          // 初始化纹理的像素数据。pitch：一行像素数据中的字节数，包括行之间的填充。字节数通常由纹理的格式决定。
                                                            // 所以pitch * new_height就是这个纹理的总字节大小。
                                                            // 例如960x540，格式为YUV420p，一个像素包含一个y和0.25u和0.25v，那么一行像素数据中的字节数pitch=960(1+0.25+0.25)=1440
                                                            // 例如1920x1080，格式为ARGB，一个像素包含a、r、g、b各一个，那么一行像素数据中的字节数pitch=1920x4=7680
            SPDINFO("realloc_texture: {}", pitch * new_height);
            SDL_UnlockTexture(*texture);
        }
        av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height, SDL_GetPixelFormatName(new_format));
    }

    return 0;
}


int VideoState::upload_texture(SDL_Texture **tex, AVFrame *frame, struct SwsContext **img_convert_ctx) {
    int ret = 0;
    Uint32 sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;

    // 1. 根据frame中的图像格式(FFmpeg像素格式)，获取对应的SDL像素格式和blendmode
    get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);

    // 2. 判断是否需要重新开辟纹理。
    // 参数tex实际是&is->vid_texture。
    if (realloc_texture(tex,
        sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt,/* 为空则使用ARGB8888 */
        frame->width, frame->height, sdl_blendmode, 0) < 0)
        return -1;

    // 3. 根据sdl_pix_fmt从AVFrame中取数据填充纹理
    switch (sdl_pix_fmt) {
        // 3.1 frame格式是SDL不支持的格式，则需要进行图像格式转换，转换为目标格式AV_PIX_FMT_BGRA，对应SDL_PIXELFORMAT_BGRA32。
        //     这应该只发生在我们不使用avfilter的情况下。
    case SDL_PIXELFORMAT_UNKNOWN:
        /*
        * 1）sws_getCachedContext：检查上下文是否可以重用，否则重新分配一个新的上下文。
        * 如果context是NULL，只需调用sws_getContext()来获取一个新的上下文。否则，检查参数是否已经保存在上下文中，
        * 如果是这种情况，返回当前的上下文。否则，释放上下文并获得一个新的上下文新参数。
        *
        * 2）sws_scale：尺寸转换函数：
        * 参1：上下文。
        * 参2：包含指向源切片平面的指针的数组。
        * 参3：包含源图像每个平面的步长的数组。
        * 参4：要处理的切片在源图像中的位置，即切片第一行在图像中从0开始计算的数字。
        * 参5：源切片的高度，即切片中的行数。
        * 参6：数组，其中包含指向目标图像平面的指针，即输出指针。
        * 参7：该数组包含目标图像的每个平面的步长。由于这里是RGB，所以数组长度为4即可。即三种颜色加上对比度RGB+A。
        */
        *img_convert_ctx = sws_getCachedContext(*img_convert_ctx,
            frame->width, frame->height, (AVPixelFormat)frame->format,
            frame->width, frame->height, AV_PIX_FMT_BGRA,   // 若SDL不支持，默认转成AV_PIX_FMT_BGRA
            _sws_flags, NULL, NULL, NULL);
        if (*img_convert_ctx != NULL) {
            uint8_t *pixels[4]; // 之前取Texture的缓存。用于保存输出的帧数据缓存。例如帧格式是yuv或者rgb，那么数组大小是3，若是argb，那么大小是4，也就是帧的格式的分量最多是4个。所以这里取数组大小是4.
            int pitch[4];       // pitch：包含着pixels每个数组的长度。即每个分量的长度，例如yuv，y是1024，那么pitch[0]=1024.
            if (!SDL_LockTexture(*tex, NULL, (void **)pixels, pitch)) {
                sws_scale(*img_convert_ctx, (const uint8_t * const *)frame->data, frame->linesize,
                    0, frame->height, pixels, pitch);// 通过这个函数，就将frame的数据填充到纹理当中。
                SDL_UnlockTexture(*tex);
            }
        }
        else {
            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
            ret = -1;
        }
        break;

        // 3.2 frame格式对应SDL_PIXELFORMAT_IYUV，不用进行图像格式转换，对应的是FFmpeg的AV_PIX_FMT_YUV420P。
        //      直接调用SDL_UpdateYUVTexture()更新SDL texture。
        // 经常更新纹理使用SDL_LockTexture+SDL_UnlockTexture。不经常则可以直接使用SDL_UpdateTexture、SDL_UpdateYUVTexture等SDL_Updatexxx函数。
    case SDL_PIXELFORMAT_IYUV:
        if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {// yuv的3平面长度都是正数的情况下，大多数走这里
            ret = SDL_UpdateYUVTexture(*tex, NULL,
                frame->data[0], frame->linesize[0],
                frame->data[1], frame->linesize[1],
                frame->data[2], frame->linesize[2]);
        }
        else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {// yuv的3平面长度都是负数的情况下，这里可以先忽略，后续再研究
            ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0],
                frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
                frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
        }
        else {
            // 不支持混合的正负线宽
            av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
            return -1;
        }
        break;

        // 3.3 frame格式对应其他SDL像素格式，不用进行图像格式转换，直接调用SDL_UpdateTexture()更新SDL texture
    default:
        if (frame->linesize[0] < 0) {
            ret = SDL_UpdateTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);// 这里可以先忽略，后续再研究
        }
        else {
            ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
        }
        break;
    }

    return ret;
}

void VideoState::set_sdl_yuv_conversion_mode(AVFrame *frame)
{
#if SDL_VERSION_ATLEAST(2,0,8)
    SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
    if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 || frame->format == AV_PIX_FMT_UYVY422)) {
        if (frame->color_range == AVCOL_RANGE_JPEG)
            mode = SDL_YUV_CONVERSION_JPEG;
        else if (frame->colorspace == AVCOL_SPC_BT709)
            mode = SDL_YUV_CONVERSION_BT709;
        else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M || frame->colorspace == AVCOL_SPC_SMPTE240M)
            mode = SDL_YUV_CONVERSION_BT601;
    }

    // 最终调用这个函数来设置YUV转换模式
    SDL_SetYUVConversionMode(mode);
#endif
}

/**
* @param filename 编码后的帧要保存的文件。
* @param frame 要编码的帧。jpeg实际上就是编码后的图片，所以jpeg图片字节数不大。
*/
int VideoState::save_frame_to_jpeg(const char *filename, const AVFrame* frame)
{
    char error_msg_buf[256] = { 0 };

    // 1 查找编码器与开辟编码器上下文
    AVCodec *jpeg_codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!jpeg_codec)
        return -1;
    AVCodecContext *jpeg_codec_ctx = avcodec_alloc_context3(jpeg_codec);
    if (!jpeg_codec_ctx)
        return -2;

    // 2 初始化编码器上下文
    jpeg_codec_ctx->pix_fmt = AV_PIX_FMT_YUVJ420P; // NOTE: Don't use AV_PIX_FMT_YUV420P，否则会与原图有色差
    jpeg_codec_ctx->width = frame->width;
    jpeg_codec_ctx->height = frame->height;
    jpeg_codec_ctx->time_base = { 1, 25 };
    jpeg_codec_ctx->framerate = { 25, 1 };
    AVDictionary *encoder_opts = NULL;
    //    av_dict_set(&encoder_opts, "qscale:v", "2", 0);
    av_dict_set(&encoder_opts, "flags", "+qscale", 0);
    av_dict_set(&encoder_opts, "qmax", "2", 0);
    av_dict_set(&encoder_opts, "qmin", "2", 0);

    // 3 关联编码器上下文与编码器，以及设置相关选项。
    int ret = avcodec_open2(jpeg_codec_ctx, jpeg_codec, &encoder_opts);
    if (ret < 0) {
        printf("avcodec open failed:%s\n", av_make_error_string(error_msg_buf, sizeof(error_msg_buf), ret));
        avcodec_free_context(&jpeg_codec_ctx);
        return -3;
    }
    av_dict_free(&encoder_opts);

    // 4 开始编码
    // frame->packet编码(yuv->h264)，packet->frame解码(h264->yuv)
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;
    // 4.1 发送帧进行编码
    ret = avcodec_send_frame(jpeg_codec_ctx, frame);
    if (ret < 0) {
        printf("Error: %s\n", av_make_error_string(error_msg_buf, sizeof(error_msg_buf), ret));
        avcodec_free_context(&jpeg_codec_ctx);
        return -4;
    }
    ret = 0;
    while (ret >= 0) {
        // 4.2 接收编码后的数据。
        ret = avcodec_receive_packet(jpeg_codec_ctx, &pkt);
        if (ret == AVERROR(EAGAIN))
            continue;
        if (ret == AVERROR_EOF) {
            ret = 0;
            break;
        }

        // 4.3 保存编码后的数据
        FILE *outfile = fopen(filename, "wb");
        if (!outfile) {
            printf("fopen %s failed\n", filename);
            ret = -1;
            break;
        }
        if (fwrite((char*)pkt.data, 1, pkt.size, outfile) == pkt.size) {
            ret = 0;
        }
        else {
            printf("fwrite %s failed\n", filename);
            ret = -1;
        }
        fclose(outfile);

        ret = 0;
        break;
    }

    avcodec_free_context(&jpeg_codec_ctx);
    return ret;
}

/**
 * AVFrame保存成bmp注意点：
 * 1）需要先转成RGB格式(这里转成32位的真彩色图，24位的也可以)，在进行保存，因为bmp存储的数据是使用RGB格式。
 * 2）y、u、v以及RGB等格式的存储方式有两种存储方式：一种是从上往下扫描，另一种是从下往上扫描。
 *	  所以如果我们截图看到是上下翻转的，需要进行存储方式的转换，有两种翻转处理：
 *		1）手动翻转图像。2）在保存bmp时，改变bmpinfo.biHeight的正负号即可。
 * bmp格式可以参考：https://blog.csdn.net/aidem_brown/article/details/80500637
 * 手动上下翻转的原理可以参考：https://blog.csdn.net/qq_36568418/article/details/113563986
*/
int VideoState::save_frame_to_bmp(const char *filename, const AVFrame* frame) {

    // 1 创建一个BGRA的AVFrame帧，用于YUV420p转换BGRA
    // 1.1 先开辟pFrameRGB。
    AVFrame* pFrameRGB = av_frame_alloc();
    if (nullptr == pFrameRGB) {
        return -1;
    }
    // 1.2 给pFrameRGB的data开辟内容。
    // 1.2.1 先给包含或包含实际的图像数据，猜想大概是提供的ffmpeg操作，以便填充pFrameRGB->data的数据成平面模式。这个大小应该指AV_PIX_FMT_BGRA的大小
    uint8_t *pictureBuf = (uint8_t*)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_BGRA, frame->width, frame->height, 1));
    // 1.2.2 往pFrameRGB->data, pFrameRGB->linesize填充数据。
    av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, pictureBuf, AV_PIX_FMT_BGRA, frame->width, frame->height, 1);

    // 2 设置图像转换上下文
    struct SwsContext *pSwsCtx = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format, frame->width, frame->height,
        AV_PIX_FMT_BGRA, SWS_BICUBIC, NULL, NULL, NULL);

    // 2.1 翻转图像，否则生成的图像是上下调到的.
    // 翻转原理：RGB位图有两种存储方式：一种是从上往下扫描，另一种是从下往上扫描。
    // 从上扫描的图像第一行的首地址即为图像Buffer的起始地址，linesize为正数即可；
    // 而从下往上扫描的图像第一行的首地址为：Buffer + linesize*(height-1)，linesize需要为相反数，即变成负数。
    AVFrame* pFrame = const_cast<AVFrame*>(frame);// 这里改变frame的源数据没有问题，因为frame已经显示过
    pFrame->data[0] += pFrame->linesize[0] * (frame->height - 1);
    pFrame->linesize[0] *= -1;
    pFrame->data[1] += pFrame->linesize[1] * (frame->height / 2 - 1);
    pFrame->linesize[1] *= -1;
    pFrame->data[2] += pFrame->linesize[2] * (frame->height / 2 - 1);
    pFrame->linesize[2] *= -1;

    // 2.2 转换图像格式，将解压出来的YUV420P的图像转换为BRGA的图像
    sws_scale(pSwsCtx, frame->data, frame->linesize, 0, frame->height, pFrameRGB->data, pFrameRGB->linesize);

    // 3 保存
    SaveAsBMP(filename, pFrameRGB, frame->width, frame->height, 0, 32);

    sws_freeContext(pSwsCtx);
    av_free(pictureBuf);
    pictureBuf = nullptr;
    av_frame_free(&pFrameRGB);

    return true;
}

bool VideoState::SaveAsBMP(const char *filename, AVFrame *pFrameRGB, int width, int height, int index, int bpp)
{
    FILE *fp;
    BITMAPFILEHEADER bmpheader;
    BITMAPINFOHEADER bmpinfo;
    fopen_s(&fp, filename, "wb+");
    if (nullptr == fp) {
        return FALSE;
    }

    /** setting the bmp parameter */
    // 1. 设置bmp的文件头
    bmpheader.bfType = 0x4d42;
    bmpheader.bfSize = (sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)) + (width * height * bpp / 8);
    bmpheader.bfReserved1 = 0;
    bmpheader.bfReserved2 = 0;
    bmpheader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    // 2. 设置bmp的信息头
    bmpinfo.biSize = sizeof(BITMAPINFOHEADER);
    bmpinfo.biWidth = width;
    //bmpinfo.biHeight = -height;// 可以控制图像的上下翻转，也可以在sws_scale之前收到翻转。
    bmpinfo.biHeight = height;
    bmpinfo.biPlanes = 1;
    bmpinfo.biBitCount = bpp;
    bmpinfo.biCompression = BI_RGB;
    //bmpinfo.biSizeImage = (width * bpp + 31) / 32 * 4 * height;// 不包含文件头、信息头、颜色表的数据大小，即真正数据的大小。
    bmpinfo.biSizeImage = width * height * bpp / 8;			   // 等价于：width * height * bpp / 8
    bmpinfo.biXPelsPerMeter = 100;
    bmpinfo.biYPelsPerMeter = 100;
    bmpinfo.biClrUsed = 0;
    bmpinfo.biClrImportant = 0;

    // 3. 依次写入文件头、信息头、真正的rgb数据。
    fwrite(&bmpheader, sizeof(bmpheader), 1, fp);
    fwrite(&bmpinfo, sizeof(bmpinfo), 1, fp);
    fwrite(pFrameRGB->data[0], width * height * bpp / 8, 1, fp);

    fclose(fp);

    return true;
}


void VideoState::video_image_display()
{
    Frame *vp;
    Frame *sp = NULL;
    SDL_Rect rect;

    // 1. 获取未显示的一帧
    // keep_last+rindex_shown保存上一帧的作用就出来了,我们是有调用frame_queue_next, 但最近出队列的帧并没有真正销毁
    // 所以这里可以读取出来显示.
    // 注意：在video_refresh我们称frame_queue_peek_last()获取到是已显示的帧，但是这里是调用frame_queue_peek_last获取到的是未显示的帧。
    // 因为在显示之前我们不是调用了frame_queue_next嘛。
    vp = _pictq.frame_queue_peek_last();

    // 2. 将字幕更新到纹理。
    if (_subtitle_st) {
        // 2.1 判断队列是否有字幕帧，有则处理，否则啥也不做。
        if (_subpq.frame_queue_nb_remaining() > 0) {
            sp = _subpq.frame_queue_peek();

            // 2.2 start_display_time是什么意思？留个疑问
            //printf("tyycode, sp->pts: %lf, sp->sub.start_display_time: %u\n", sp->pts, sp->sub.start_display_time);
            SPDINFO("tyycode, sp->pts: {}, sp->sub.start_display_time: {}", sp->pts, sp->sub.start_display_time);
            if (vp->pts >= sp->pts + ((float)sp->sub.start_display_time / 1000)) {

                // 2.3 如果未显示，则处理，否则啥也不做。
                if (!sp->uploaded) {
                    uint8_t* pixels[4];
                    int pitch[4];
                    int i;
                    // 2.4 如果字幕帧没有分辨率，则以视频帧分辨率赋值。
                    if (!sp->width || !sp->height) {
                        sp->width = vp->width;
                        sp->height = vp->height;
                    }
                    // 2.5 不管字幕的纹理是否存在，都释放然后重新开辟。
                    // 这里看到，字幕纹理结构都是以argb形式存在，即一个像素占4个字节。
                    if (realloc_texture(&_sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height, SDL_BLENDMODE_BLEND, 1) < 0)
                        return;

                    // 2.6 将字幕帧的每个子矩形进行格式转换，转换后的数据保存在字幕纹理sub_texture中。
                    for (i = 0; i < sp->sub.num_rects; i++) {
                        AVSubtitleRect *sub_rect = sp->sub.rects[i];
                        // 重置sub_rect的x、y、w、h，并使用av_clip确保x、y、w、h在[0, xxx]范围。
                        // 实际上就是确保子矩形在字幕帧这个矩形的范围内。
                        sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
                        sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                        sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
                        sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                        // 检查上下文是否可以重用，否则重新分配一个新的上下文。
                        // 从8位的调色板AV_PIX_FMT_PAL8格式转到AV_PIX_FMT_BGRA的格式。子矩形应该默认就是AV_PIX_FMT_PAL8的格式。
                        _sub_convert_ctx = sws_getCachedContext(_sub_convert_ctx,
                            sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                            sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                            0, NULL, NULL, NULL);
                        if (!_sub_convert_ctx) {
                            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                            return;
                        }

                        // 此时，得到了输出格式为AV_PIX_FMT_BGRA字幕尺寸格式变换上下文，那么可以调用sws_scale进行转换了。
                        // 转换的意义在于使SDL支持这种格式进行渲染。
                        // 首先锁定字幕纹理中(相当于字幕帧矩形)的子矩形的区域。pixels、pitch分别是指锁定的子矩形的数据与一行字节数，可看函数声明。
                        if (!SDL_LockTexture(_sub_texture, (SDL_Rect *)sub_rect, (void **)pixels, pitch)) {
                            sws_scale(_sub_convert_ctx, (const uint8_t * const *)sub_rect->data, sub_rect->linesize,
                                0, sub_rect->h, pixels, pitch);// 转完格式后，就将sub_rect->data的数据填充到纹理当中，等待渲染即可显示。
                            SDL_UnlockTexture(_sub_texture);
                        }
                    }// <== for (i = 0; i < sp->sub.num_rects; i++) end ==>

                    sp->uploaded = 1;// 标记为已显示。
                }
            }
            else
                sp = NULL;
        }
    }

    // 3. 计算要视频帧宽度要在屏幕显示的大小。
    // 将帧宽高按照sar最大适配到窗口，并通过rect返回视频帧在窗口的显示位置和宽高
    calculate_display_rect(&rect, _xleft, _ytop, _width, _height, vp->width, vp->height, vp->sar);
    //    rect.x = rect.w /2;   // 测试
    //    rect.w = rect.w /2;   // 视频的缩放实际不是用sws， 缩放是sdl去做的

    // 4. 如果没有显示则将帧的数据填充到纹理当中。 但还没显示，显示需要在渲染器显示。
    if (!vp->uploaded) {
        // 把yuv数据更新到vid_texture
        if (upload_texture(&_vid_texture, vp->frame, &_img_convert_ctx) < 0)
            return;
        vp->uploaded = 1;       // 标记该帧已显示
        vp->flip_v = vp->frame->linesize[0] < 0;// 记录是正常播放还是垂直翻转播放，即frame->linesize[0]为负数时代表垂直翻转播放，
                                                // 到时理解upload_texture内部判断frame->linesize[0]为负，可以容易理解。
    }
    // 16屏，1920x1080,缓存5帧，内存大概占用240M左右(不考虑内存对齐)。
    //int si = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, vp->frame->width, vp->frame->height, 1);// 计算AVFrame一帧大小，只与帧格式以及宽高有关，与linesize无关。

    // 经过上面的处理，视频帧、字幕帧都已经转换为SDL支持的显示格式，并都保存在各自的纹理当中，剩下就是渲染显示了。


    // 5. 拷贝视频的纹理到渲染器。具体是设置yuv的转换格式以及拷贝视频纹理的像素数据到渲染器。
    /*
    * SDL_RenderCopyEx：将部分源纹理复制到当前渲染目标，并围绕给定的中心旋转角度。
    * 参1 renderer：渲染器。
    * 参2 texture：纹理。
    * 参3 srcrect：指向源矩形的指针，或者为NULL，表示整个纹理。
    * 参4 dstrect：指向目标矩形的指针，或者为NULL，表示整个渲染目标。
    * 参5 angle：表示将应用于参4的旋转的角度.
    * 参6 center：A pointer to a point indicating the point around which dstrect
    *      will be rotated (if NULL, rotation will be done aroud dstrect.w/2, dstrect.h/2)。
    * 参7 flip：一个SDL_RendererFlip值，说明应该在纹理上执行哪些翻转动作。
    */
    set_sdl_yuv_conversion_mode(vp->frame);// 根据帧的属性来设置yuv的转换格式
#ifdef USE_DLL_PLAY
    SDL_RenderCopy(_renderer, _vid_texture, NULL, NULL);
#else
    // SDL_CreateWindowFrom方式创建窗口播放的话，不能手动调用SDL_SetWindowSize调整大小，否则黑屏，大小SDL会自动调整。
    // 因不能手动调整大小，所以这里的SDL_RenderCopyEx的参数4rect作用不大，因为这会导致视频帧与窗口大小不一致。
    SDL_RenderCopyEx(_renderer, _vid_texture, NULL, &rect, 0, NULL, (SDL_RendererFlip)(vp->flip_v ? SDL_FLIP_VERTICAL : 0));  // 到这一步后，我们只需要执行SDL_RenderPresent就可以显示图片了
#endif
    set_sdl_yuv_conversion_mode(NULL);      // 重设为自动选择，传空表示自动选择yuv的转换格式(SDL_YUV_CONVERSION_AUTOMATIC)。

    // 6. 拷贝字幕的纹理到渲染器。
    if (sp) {// 有字幕的才会进来
#if USE_ONEPASS_SUBTITLE_RENDER
        SDL_RenderCopy(_renderer, _sub_texture, NULL, &rect);
#else
        int i;
        double xratio = (double)rect.w / (double)sp->width;
        double yratio = (double)rect.h / (double)sp->height;
        for (i = 0; i < sp->sub.num_rects; i++) {
            SDL_Rect *sub_rect = (SDL_Rect*)sp->sub.rects[i];
            SDL_Rect target = { .x = rect.x + sub_rect->x * xratio,
                .y = rect.y + sub_rect->y * yratio,
                .w = sub_rect->w * xratio,
                .h = sub_rect->h * yratio };
            SDL_RenderCopy(renderer, is->sub_texture, sub_rect, &target);
        }
#endif
    }
}

// video_open是改变_width、_height的值。
// 而set_default_window_size是改变_default_width、_default_height.
int VideoState::video_open()
{
    int w,h;

    w = _screen_width ? _screen_width : _default_width;
    h = _screen_height ? _screen_height : _default_height;

    if (!window_title)
        window_title = _filename.c_str();
    SDL_SetWindowTitle(_window, "window_title");

    SDL_SetWindowSize(_window, w, h);
    SDL_SetWindowPosition(_window, _screen_left, _screen_top);
    if (_is_full_screen)
        SDL_SetWindowFullscreen(_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_ShowWindow(_window);// 最终显示窗口的函数调用

    _width  = w;         // 保存屏幕要显示的宽度和高度
    _height = h;

    return 0;
}

void VideoState::video_display()
{

#ifdef USE_DLL_PLAY
    if (!_width){
        _width = _screen_width ? _screen_width : _default_width;
        _height = _screen_height ? _screen_height : _default_height;
    }

#else
    if (!_width)
        video_open(); //如果窗口未显示，则显示窗口
#endif

    // 1. 设置渲染器绘图颜色
    //SDL_SetWindowSize(_window, _width, _height);
    SDL_SetRenderDrawColor(_renderer, 0, 0, 0, 255);

    // 2. 每次重新画图一帧都应该清除掉渲染器的内容
    SDL_RenderClear(_renderer);

    // 3. 图形化显示仅有音轨的文件
    if (_audio_st && _show_mode != SHOW_MODE_VIDEO){
        //video_audio_display(is);
    }else if (_video_st)  // 4. 显示一帧视频画面。 video_image_display内部实际上就是做SDL_RenderCopy，即帧数据填充到纹理，以及纹理数据拷贝到渲染器。
        video_image_display();

    // 经过上面，视频帧、字幕帧的纹理数据都均拷贝到渲染器了，那么直接显示即可。

    // 5. 显示到渲染器(最终显示一帧图片的函数)。
    SDL_RenderPresent(_renderer);

    // tyycode.在显示完该帧后进行抓图比较好.
    if (_captureCount > 0 && _capturePath.empty() == false) {
        Frame *vp = _pictq.frame_queue_peek_last();
        //int ret = save_frame_to_jpeg(_capturePath.c_str(), vp->frame);
        int ret = save_frame_to_bmp(_capturePath.c_str(), vp->frame);
        if (ret < 0) {
            //record log.
            SPDINFO("capture failed, path: {}", _capturePath.c_str());
        }
        _captureCount--;
        if (_captureCount < 0) {
            _captureCount = 0;
        }
    }
}

#if 0
int write_thread(void *arg) {

    VideoState *is = (VideoState*)arg;
    double time, remaining_time = 0.0;
    Frame *sp, *sp2;

    while (1) {

        if (is->_abort_request_w) {
            //SPDWARN("write thread exit");
            break;
        }

        if (remaining_time > 0.0) {//sleep控制画面输出的时机
            av_usleep((int64_t)(remaining_time * 1000000.0)); // remaining_time <= REFRESH_RATE
        }
        remaining_time = REFRESH_RATE;

        if (is->_show_mode != VideoState::SHOW_MODE_NONE && (!is->_paused || is->_force_refresh)) {

            // 只有符合上面的3个条件才更新视频
            //video_refresh(is, &remaining_time);
            // 1. 没有暂停，音视频同步是外部时钟(只有视频时ffplay默认使用外部时钟)，并且是实时流时：
            if (!is->_paused && is->get_master_sync_type() == AV_SYNC_EXTERNAL_CLOCK && is->_realtime)
                //check_external_clock_speed(is);

            // 2. 没有禁用视频，显示模式不是视频，并且有音频流。这里估计是音频封面，留个疑问
            // debug 带有音、视频的实时流或者文件：show_mode=SHOW_MODE_VIDEO (0x0000)，都不会进来
            if (!is->_display_disable && is->_show_mode != VideoState::SHOW_MODE_VIDEO && is->_audio_st) {
                //time = av_gettime_relative() / 1000000.0;
                //if (is->_force_refresh || is->_last_vis_time + is->_rdftspeed < time) {
                //	video_display(is);
                //	is->_last_vis_time = time;
                //}time
                //*remaining_time = FFMIN(*remaining_time, is->last_vis_time + rdftspeed - );
            }

            // 3. 如果有视频流
            if (is->_video_st) {
            retry:
                // 3.1 帧队列是否为空
                if (is->_pictq.frame_queue_nb_remaining() == 0) {
                    // nothing to do, no picture to display in the queue
                    // 什么都不做，队列中没有图像可显示
                }
                else { // 重点是音视频同步
                    double last_duration, duration, delay;
                    Frame *vp, *lastvp;
                    PacketQueue::pktStatus qStatus;

                    qStatus = is->_videoq.packet_queue_get_status();

                    /* dequeue the picture */
                    // 从队列取出上一个Frame
                    lastvp = is->_pictq.frame_queue_peek_last(); // 读取上一帧
                    vp = is->_pictq.frame_queue_peek();          // 读取待显示帧

                    // 3.2 vp->serial是否是当前系序列下的包。帧队列的serial由解码器的serial赋值，具体看queue_picture()。
                    // 凡是serial是否最新，都应该与包队列的serial比较，因为seek后首先影响的是包队列。
                    // 这里表明，一旦serial与最新的serial不相等，不仅包队列里面的旧的serial包要舍弃不能送进解码，已经解码后的旧的serial同样要舍弃，不能在帧队列保存了。
                    if (vp->serial != is->_videoq._serial) {
                        // 如果不是最新的播放序列，则将其出队列，以尽快读取最新序列的帧
                        is->_pictq.frame_queue_next();   // here
                        goto retry;
                    }

                    // 更新上一帧的播放时间，因为上面serial不一样时，帧出队列，goto retry后lastvp会被更新
                    if (lastvp->serial != vp->serial) {
                        // 新的播放序列重置当前时间
                        is->_frame_timer = av_gettime_relative() / 1000000.0;
                    }

                    // 暂停状态会一直显示正在显示的帧，不做同步处理。
                    if (is->_paused)
                    {
                        goto display;
                        printf("视频暂停is->paused");
                    }

                    last_duration = is->vp_duration(lastvp, vp);

                    delay = is->compute_target_delay(last_duration);
                    //printf("last_duration: %lf, delay: %lf\n", last_duration, delay);

                    time = av_gettime_relative() / 1000000.0;
                    // is->frame_timer 实际上就是上一帧lastvp的播放时间,
                    // is->frame_timer + delay 是待显示帧vp该播放的时间
                    if (time < is->_frame_timer + delay) {
                        remaining_time = FFMIN(is->_frame_timer + delay - time, remaining_time);// 正常应睡眠时间。remaining_time=REFRESH_RATE默认睡0.01s
                        if (is->_av_sync_type != AV_SYNC_VIDEO_MASTER) {
                            goto display;
                        }
                        else {
                            if (qStatus.nbPackets >= 10) {
                                delay = (remaining_time - 0.02 > 10 ? remaining_time - 0.02 : 0.005);
                                remaining_time = delay;
                            }
                            else if (qStatus.nbPackets <= 3) {
                                SDL_CondSignal(is->_continue_read_thread);
                                delay = (remaining_time + 0.005);
                                remaining_time = delay;
                            }
                            else {
                                // 包数合理，正常睡眠
                                //remaining_time = delay;
                            }

                            av_usleep((int64_t)(remaining_time * 1000000.0));
                            remaining_time = 0;
                        }
                    }

                    is->_frame_timer += delay;   // 更新上一帧为当前帧的播放时间
                    if (delay > 0 && time - is->_frame_timer > AV_SYNC_THRESHOLD_MAX) {// 0.1s
                        is->_frame_timer = time; //如果上一帧播放时间和系统时间差距太大，就纠正为系统时间
                    }

                    SDL_LockMutex(is->_pictq._mutex);
                    if (!isnan(vp->pts))
                        is->update_video_pts(vp->pts, vp->pos, vp->serial); // 更新video时钟
                    SDL_UnlockMutex(is->_pictq._mutex);

                    /*
                    * 因为上面compute_target_delay的sync_threshold同步阈值最差可能是[-0.1s,0.1s]，是可能存在一帧到两帧的落后情况。
                    * 例如delay传入是0.1s，那么sync_threshold=0.1s，假设音视频的pts差距diff是在0.1s以内(当然这里只是假设不一定成立)，
                    * 那么delay还是直接返回传入的delay=0.1s，不睡眠，is->frame_timer += delay直接更新并且不会纠正，
                    * 那么若满足下面的if条件，主要条件是：若实时时间time > 本帧的pts + duration，本帧的pts就是frame_time，因为上面刚好加上了delay。
                    * 即实时时间大于下一帧的pts，证明确实落后了一帧，需要drop帧，以追上播放速度。
                    */
                    // 丢帧逻辑(主时钟为视频不会进来)
                    if (is->_pictq.frame_queue_nb_remaining() > 1) {//有nextvp才会检测是否该丢帧
                        Frame *nextvp = is->_pictq.frame_queue_peek_next();
                        duration = is->vp_duration(vp, nextvp);
                        // 非逐帧 并且 (强制刷新帧 或者 不强制刷新帧的情况下视频不是主时钟) 并且 视频确实落后一帧，
                        // 那么进行drop帧
                        if (!is->_step        // 非逐帧模式才检测是否需要丢帧 is->step==1 为逐帧播放
                            && (is->_framedrop>0 ||      // cpu解帧过慢
                            (is->_framedrop && is->get_master_sync_type() != AV_SYNC_VIDEO_MASTER)) // 非视频同步方式
                            && time > is->_frame_timer + duration // 确实落后了一帧数据
                            ) {
                            printf("%s(%d) dif:%lfs, drop frame\n", __FUNCTION__, __LINE__, (is->_frame_timer + duration) - time);
                            is->_frame_drops_late++;             // 统计丢帧情况
                            is->_pictq.frame_queue_next();       // vp出队，这里实现真正的丢帧

                            //(这里不能直接while丢帧，因为很可能audio clock重新对时了，这样delay值需要重新计算)
                            goto retry; // 回到函数开始位置，继续重试
                        }
                    }

                    // 来到这里，说明视频帧都是能播放的。但是需要判断字幕帧是否能播放，不能则会进行丢字幕帧或者清空正在播放的字幕帧。
                    // 判断流程：如果发生seek或者落后视频1-2帧：若字幕帧显示过或者正在显示，立马进行清空；若字幕帧没显示过，则立马出队列drop帧。
                    // 这里的代码让我觉得还是需要写一些demo去理解SDL_LockTexture、pixels、pitch的意义。例如如何处理字幕的格式，提取yuv各个分量的首地址等等。
                    //if (is->subtitle_st) {

                    //	while (frame_queue_nb_remaining(&is->subpq) > 0) {// 字幕的keep_last、rindex_shown始终都是0，音视频则会变成1.
                    //													  // 获取当前帧。
                    //		sp = frame_queue_peek(&is->subpq);

                    //		// 若队列存在2帧以上，那么获取下一帧。因为frame_queue_peek获取后rindex并未自增，所以sp、sp2是相邻的。
                    //		if (frame_queue_nb_remaining(&is->subpq) > 1)
                    //			sp2 = frame_queue_peek_next(&is->subpq);
                    //		else
                    //			sp2 = NULL;

                    //		if (sp->serial != is->subtitleq.serial  // 发生seek了
                    //												// 因为上面update_video_pts更新了vp即待显示帧的pts，所以is->vidclk.pts 就是表示待显示帧的pts。
                    //												// sp->sub.end_display_time表示字幕帧的显示时长。故sp->pts + ((float) sp->sub.end_display_time / 1000)表示当前字幕帧的结束时间戳。
                    //			|| (is->vidclk.pts > (sp->pts + ((float)sp->sub.end_display_time / 1000)))// 表示：字幕帧落后视频帧一帧。
                    //			|| (sp2 && is->vidclk.pts > (sp2->pts + ((float)sp2->sub.start_display_time / 1000))))// 同理表示：字幕帧落后视频帧两帧。
                    //		{
                    //			/*
                    //			* 字幕相关结构体解释：
                    //			* AVSubtitle：保存着字幕流相关的信息，可以认为是一个管理结构体。
                    //			* AVSubtitleRect：真正存储字幕的信息，每个AVSubtitleRect存储单独的字幕信息。从这个结构体看到，分别有data+linesize(pict将被舍弃)、text、ass指针，
                    //			*                  这是为了支持FFmpeg的3种字幕类型，具体谁有效，看type。例如type是text，那么text指针就保存着字幕的内容，其它两个指针没实际意义。
                    //			* AVSubtitleType：FFmpeg支持的字幕类型，3种，分别是：bitmap位图、text文本、ass。
                    //			*/
                    //			if (sp->uploaded) {
                    //				int i;
                    //				/*
                    //				* 下面两个for循环的操作大概意思是(个人理解，具体需要后续写demo去验证)：
                    //				* 1）例如一行字幕有4个字。那么sp->sub.num_rects=4.
                    //				* 2）而每个字又单独是一幅图像，所以同样需要遍历处理。
                    //				* 3）一幅图像的清空：按照高度进行遍历，宽度作为清空的长度即可(为啥左移2目前不是很懂)。这一步猜出pitch是这个图像的offset偏移位置。
                    //				*                  每次处理完一次纹理pixels，都需要相加进行地址偏移，而不能使用宽度w进行地址偏移。宽度是针对于图像大小，pitch才是地址的真正偏移。
                    //				*                  换句话说，操作图像用宽度，操作地址偏移用pitch。
                    //				* 4）处理完这个字后，回到外层循环继续处理下一个字，以此类推...。
                    //				*/

                    //				for (i = 0; i < sp->sub.num_rects; i++) {   // 遍历字幕信息AVSubtitleRect的个数。
                    //					AVSubtitleRect *sub_rect = sp->sub.rects[i];
                    //					uint8_t *pixels;                        // pixels、pitch的作用实际是提供给纹理用的，纹理又通过这个指针给调用者进行操作纹理中的数据。
                    //					int pitch, j;

                    //					// AVSubtitleRect可以转成SDL_Rect是因为结构体前4个成员的位置、数据类型一样。
                    //					if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)&pixels, &pitch)) {
                    //						for (j = 0; j < sub_rect->h; j++, pixels += pitch) {
                    //							memset(pixels, 0, sub_rect->w << 2);// 将纹理置空，相当于把画面的字幕清空(纹理相当于是一张纸)，宽度为啥要左移2(相当于乘以4)？留个疑问
                    //																// 答：我们创建纹理时传进的参数分辨率的单位是像素(理解这一点非常重要)，一行像素占多少字节由格式图片格式决定。
                    //																// 因为字幕的纹理创建时，格式为SDL_PIXELFORMAT_ARGB8888，所以一个像素占4个字节(一个像素包含ARGB)，那么一行像素占w*4，故左移2。
                    //						}
                    //						SDL_UnlockTexture(is->sub_texture);
                    //					}
                    //				}
                    //			}

                    //			frame_queue_next(&is->subpq);
                    //		}
                    //		else {
                    //			break;
                    //		}

                    //	}// <== while end ==>
                    //}// <== if (is->subtitle_st) end ==>

                    is->_pictq.frame_queue_next();   // 当前vp帧出队列，vp变成last_vp，这样video_display就能播放待显示帧。
                    is->_force_refresh = 1;          /* 说明需要刷新视频帧 */

                    if (is->_step && !is->_paused)
                        is->stream_toggle_pause();    // 逐帧的时候那继续进入暂停状态。音频在逐帧模式下不需要处理，因为它会自动按照暂停模式下的流程处理。所以暂停、播放、逐帧都比较简单。

                }// <== else end ==>

            display:
                /* display picture */
                if (!is->_display_disable && is->_force_refresh && is->_show_mode == VideoState::SHOW_MODE_VIDEO && is->_pictq._rindex_shown)// is->pictq.rindex_shown会在上面的frame_queue_next被置为1.
                    is->video_display(); // 重点是显示。see here

            }//<== if (is->video_st) end ==>


            is->_force_refresh = 0;


        }//<== if (is->show_mode != VideoState::SHOW_MODE_NONE && (!is->_paused || is->_force_refresh)) ==>

    }// <== while end ==>

    return 0;
}
#endif

void VideoState::video_refresh(double *remaining_time)
{
    double time;

    Frame *sp, *sp2;

    // 1. 没有暂停，音视频同步是外部时钟(只有视频时ffplay默认使用外部时钟)，并且是实时流时：
    if (!_paused && get_master_sync_type() == AV_SYNC_EXTERNAL_CLOCK && _realtime)
        //check_external_clock_speed(is);

    // 2. 没有禁用视频，显示模式不是视频，并且有音频流。这里估计是音频封面，留个疑问
    // debug 带有音、视频的实时流或者文件：show_mode=SHOW_MODE_VIDEO (0x0000)，都不会进来
    if (!_display_disable && _show_mode != VideoState::SHOW_MODE_VIDEO && _audio_st) {
        //time = av_gettime_relative() / 1000000.0;
        //if (is->_force_refresh || is->_last_vis_time + is->_rdftspeed < time) {
        //	video_display(is);
        //	is->_last_vis_time = time;
        //}time
        //*remaining_time = FFMIN(*remaining_time, is->last_vis_time + rdftspeed - );
    }

    // 3. 如果有视频流
    if (_video_st) {
    retry:
        // 3.1 帧队列是否为空
        if (_pictq.frame_queue_nb_remaining() == 0) {
            // nothing to do, no picture to display in the queue
            // 什么都不做，队列中没有图像可显示
            //printf("_pictq.frame_queue_nb_remaining = 0\n");
        }
        else { // 重点是音视频同步
            double last_duration, duration, delay;
            Frame *vp, *lastvp;
            int64_t t1, t2, t3;
            PacketQueue::pktStatus qStatus;

            qStatus = _videoq.packet_queue_get_status();

            /* dequeue the picture */
            // 从队列取出上一个Frame
            lastvp = _pictq.frame_queue_peek_last(); // 读取上一帧
            vp = _pictq.frame_queue_peek();          // 读取待显示帧

            // 3.2 vp->serial是否是当前系序列下的包。帧队列的serial由解码器的serial赋值，具体看queue_picture()。
            // 凡是serial是否最新，都应该与包队列的serial比较，因为seek后首先影响的是包队列。
            // 这里表明，一旦serial与最新的serial不相等，不仅包队列里面的旧的serial包要舍弃不能送进解码，已经解码后的旧的serial同样要舍弃，不能在帧队列保存了。
            if (vp->serial != *_videoq.packet_queue_get_serial()) {
                // 如果不是最新的播放序列，则将其出队列，以尽快读取最新序列的帧
                _pictq.frame_queue_next();   // here
                goto retry;
            }

            // 更新上一帧的播放时间，因为上面serial不一样时，帧出队列，goto retry后lastvp会被更新
            if (lastvp->serial != vp->serial) {
                // 新的播放序列重置当前时间
                _frame_timer = av_gettime_relative() / 1000000.0;
            }

            // 暂停状态会一直显示正在显示的帧，不做同步处理。
            if (_paused)
            {
                goto display;
                //printf("视频暂停is->paused");
            }

            /* compute nominal last_duration */
            //lastvp上一帧，vp当前帧 ，nextvp下一帧
            //last_duration：静态计算上一帧应显示的时长
            last_duration = vp_duration(lastvp, vp);

            // 经过compute__delay方法，动态计算出真正待显示帧vp需要等待的时间
            // 如果以video同步，则delay直接等于last_duration。
            // 如果以audio或外部时钟同步，则需要比对主时钟调整待显示帧vp要等待的时间。
            delay = compute_target_delay(last_duration); // 上一帧需要维持的时间
            //printf("last_duration: %lf, delay: %lf\n", last_duration, delay);

            time = av_gettime_relative() / 1000000.0;
            // is->frame_timer 实际上就是上一帧lastvp的播放时间,
            // is->frame_timer + delay 是待显示帧vp该播放的时间
            if (time < _frame_timer + delay) { //判断是否继续显示上一帧
                // 当前系统时刻还未到达上一帧的结束时刻，那么还应该继续显示上一帧。
                // 计算出最小等待时间
                *remaining_time = FFMIN(_frame_timer + delay - time, *remaining_time);// remaining_time=REFRESH_RATE默认睡0.01s

                //if (is->_av_sync_type != AV_SYNC_VIDEO_MASTER) {
                if (get_master_sync_type() != AV_SYNC_VIDEO_MASTER) {
                    goto display;
                }else {
                    if (qStatus.nbPackets >= 10) {
                        //delay = (*remaining_time - 0.03 > 0.01 ? *remaining_time - 0.03 : 0.005);
                        delay = 0.000;
                        *remaining_time = delay;
                        //av_usleep((int64_t)(*remaining_time * 1000000.0));// 包多的时候直接睡眠，不处理事件
                        //*remaining_time = 0;
                    }else if (qStatus.nbPackets >= 5) {
                        delay = 0.005;
                        *remaining_time = delay;
                        av_usleep((int64_t)(*remaining_time * 1000000.0));// 包多的时候直接睡眠，不处理事件
                        *remaining_time = 0;
                    }else if (qStatus.nbPackets >= 3) {
                        // 包数合理，正常睡眠
                        goto display;
                    }else {
                        SDL_CondSignal(_continue_read_thread);
                        delay = (*remaining_time + 0.005);
                        *remaining_time = delay;
                        goto display;
                    }

                    //av_usleep((int64_t)(*remaining_time * 1000000.0));// 这里直接睡眠的话，当事件到来只能播放完这一帧才能处理；如果不这样写，使用goto display的话，会因为由于该帧未到时间显示，
                                                                      // 导致一直睡0.01，直到实时时间time>is->_frame_timer + delay，所以无法有效减低延时.
                    //*remaining_time = 0;
                    //goto display;
                }
            }

            //{
            //	static int64_t last = av_gettime_relative();
            //	auto now	 = av_gettime_relative();
            //	SPDINFO("++packets: {}, elp(ms): {}, time: {}, ft: {}, delay: {}, rt: {}", qStatus.nbPackets, (now - last)/1000.0, time, is->_frame_timer, delay, *remaining_time);
            //	last = now;
            //}

            _frame_timer += delay;   // 更新上一帧为当前帧的播放时间
            if (delay > 0 && time - _frame_timer > AV_SYNC_THRESHOLD_MAX) {// 0.1s
                _frame_timer = time; //如果上一帧播放时间和系统时间差距太大，就纠正为系统时间
            }

            // 这里上锁的目的是？它想保护哪些内容？可以自行去看有哪些地方用到了_pictq._mutex。但是用到的地方并未有共同的变量。所以感觉去掉也没问题？
            SDL_LockMutex(_pictq.frame_queue_get_mutex());
            if (!isnan(vp->pts))
                update_video_pts(vp->pts, vp->pos, vp->serial); // 更新video时钟，以及更新从时钟(vidclk)到外部时钟(extclk)
            SDL_UnlockMutex(_pictq.frame_queue_get_mutex());

            // 下面是检测视频以及字幕是否落后需要丢帧.

            /*
            * 因为上面compute_target_delay的sync_threshold同步阈值最差可能是[-0.1s,0.1s]，是可能存在一帧到两帧的落后情况。
            * 例如delay传入是0.1s，那么sync_threshold=0.1s，假设音视频的pts差距diff是在0.1s以内(当然这里只是假设不一定成立)，
            * 那么delay还是直接返回传入的delay=0.1s，不睡眠，is->frame_timer += delay直接更新并且不会纠正，
            * 那么若满足下面的if条件，主要条件是：若实时时间time > 本帧的pts + duration，本帧的pts就是frame_time，因为上面刚好加上了delay。
            * 即实时时间大于下一帧的pts，证明确实落后了一帧，需要drop帧，以追上播放速度。
            */
            // 丢帧逻辑(主时钟为视频不会进来)
            // 视频落后才会丢帧
            if (_pictq.frame_queue_nb_remaining() > 1) {//有nextvp才会检测是否该丢帧
                Frame *nextvp = _pictq.frame_queue_peek_next();
                duration = vp_duration(vp, nextvp);
                // 非逐帧 并且 (强制刷新帧 或者 不强制刷新帧的情况下视频不是主时钟) 并且 视频确实落后一帧，
                // 那么进行drop帧
                if (!_step        // 非逐帧模式才检测是否需要丢帧 is->step==1 为逐帧播放
                    && (_framedrop>0 ||      // cpu解帧过慢
                    (_framedrop && get_master_sync_type() != AV_SYNC_VIDEO_MASTER)) // 非视频同步方式
                    && time > _frame_timer + duration // 确实落后了一帧数据
                    ) {
                    //printf("%s(%d) dif:%lfs, drop frame\n", __FUNCTION__, __LINE__, (is->_frame_timer + duration) - time);
                    SPDINFO("diff: {}, drop frame", (_frame_timer + duration) - time);
                    _frame_drops_late++;             // 统计丢帧情况
                    _pictq.frame_queue_next();       // vp出队，这里实现真正的丢帧

                    //(这里不能直接while丢帧，因为很可能audio clock重新对时了，这样delay值需要重新计算，需要考虑delay是因为丢帧用到的_frame_timer的计算依赖delay)
                    goto retry; // 回到函数开始位置，继续重试
                }
            }

#if 1
            // 来到这里，说明视频帧都是能播放的。但是需要判断字幕帧是否能播放，不能则会进行丢字幕帧或者清空正在播放的字幕帧。
            // 判断流程：如果发生seek或者落后视频1-2帧：若字幕帧显示过或者正在显示，立马进行清空；若字幕帧没显示过，则立马出队列drop帧。
            // 这里的代码让我觉得还是需要写一些demo去理解SDL_LockTexture、pixels、pitch的意义。例如如何处理字幕的格式，提取yuv各个分量的首地址等等。
            if (_subtitle_st) {
                while (_subpq.frame_queue_nb_remaining() > 0) {// 字幕的keep_last、rindex_shown始终都是0，音视频则会变成1.
                    // 获取当前帧。
                    sp = _subpq.frame_queue_peek();
                    // 若队列存在2帧以上，那么获取下一帧。因为frame_queue_peek获取后rindex并未自增，所以sp、sp2是相邻的。
                    if (_subpq.frame_queue_nb_remaining() > 1)
                        sp2 = _subpq.frame_queue_peek_next();
                    else
                        sp2 = NULL;

                    //if (sp->serial != *_subtitleq.packet_queue_get_serial()  // 发生seek了
                    // 因为上面update_video_pts更新了vp即待显示帧的pts，所以is->vidclk.pts 就是表示待显示帧的pts。
                    // sp->sub.end_display_time表示字幕帧的显示时长。故sp->pts + ((float) sp->sub.end_display_time / 1000)表示当前字幕帧的结束时间戳。
                    //    || (is->vidclk.pts > (sp->pts + ((float)sp->sub.end_display_time / 1000)))// 表示：字幕帧落后视频帧一帧。
                    //    || (sp2 && is->vidclk.pts > (sp2->pts + ((float)sp2->sub.start_display_time / 1000))))// 同理表示：字幕帧落后视频帧两帧。
                    auto sp2Start = (float)(sp2 ? sp2->sub.start_display_time : -1);
                    auto sp2End = (float)(sp2 ? sp2->sub.end_display_time : -1);
                    SPDINFO("sp, start: {}, end: {}; sp2, start: {}, end: {}",
                            sp->sub.start_display_time / 1000, sp->sub.end_display_time / 1000,
                            sp2Start / 1000, sp2End / 1000);
                    if (sp->serial != *_subtitleq.packet_queue_get_serial()
                        || (_vidclk.get_clock_pts() > (sp->pts + ((float)sp->sub.end_display_time / 1000)))
                        || (sp2 && _vidclk.get_clock_pts() > (sp2->pts + ((float)sp2->sub.start_display_time / 1000))))
                    {
                        /*
                        * 字幕相关结构体解释：
                        * AVSubtitle：保存着字幕流相关的信息，可以认为是一个管理结构体。
                        * AVSubtitleRect：真正存储字幕的信息，每个AVSubtitleRect存储单独的字幕信息。从这个结构体看到，分别有data+linesize(pict将被舍弃)、text、ass指针，
                        *                  这是为了支持FFmpeg的3种字幕类型，具体谁有效，看type。例如type是text，那么text指针就保存着字幕的内容，其它两个指针没实际意义。
                        * AVSubtitleType：FFmpeg支持的字幕类型，3种，分别是：bitmap位图、text文本、ass。
                        */
                        // 如果已经显示过该字幕帧，则在纹理将其清除。没显示过，则立马出队列drop帧。
                        if (sp->uploaded) {
                            int i;
                            /*
                            * 下面两个for循环的操作大概意思是(个人理解，具体需要后续写demo去验证)：
                            * 1）例如一行字幕有4个字。那么sp->sub.num_rects=4.
                            * 2）而每个字又单独是一幅图像，所以同样需要遍历处理。
                            * 3）一幅图像的清空：按照高度进行遍历，宽度作为清空的长度即可(为啥左移2目前不是很懂)。这一步猜出pitch是这个图像的offset偏移位置。
                            *                  每次处理完一次纹理pixels，都需要相加进行地址偏移，而不能使用宽度w进行地址偏移。宽度是针对于图像大小，pitch才是地址的真正偏移。
                            *                  换句话说，操作图像用宽度，操作地址偏移用pitch。
                            * 4）处理完这个字后，回到外层循环继续处理下一个字，以此类推...。
                            */

                            for (i = 0; i < sp->sub.num_rects; i++) {   // 遍历字幕信息AVSubtitleRect的个数。
                                AVSubtitleRect *sub_rect = sp->sub.rects[i];
                                uint8_t *pixels;                        // pixels、pitch的作用实际是提供给纹理用的，纹理又通过这个指针给调用者进行操作纹理中的数据。
                                int pitch, j;

                                // AVSubtitleRect可以转成SDL_Rect是因为结构体前4个成员的位置、数据类型一样。
                                if (!SDL_LockTexture(_sub_texture, (SDL_Rect *)sub_rect, (void **)&pixels, &pitch)) {
                                    SPDINFO("w: {}, h: {}, pixels: {}, pitch: {}, sub_rect->w << 2: {}",
                                            sub_rect->w, sub_rect->h, pixels, pitch, sub_rect->w << 2);
                                    for (j = 0; j < sub_rect->h; j++, pixels += pitch) {
                                        memset(pixels, 0, sub_rect->w << 2);// 将纹理置空，相当于把画面的字幕清空(纹理相当于是一张纸)，宽度为啥要左移2(相当于乘以4)？留个疑问
                                                                            // 答：我们创建纹理时传进的参数分辨率的单位是像素(理解这一点非常重要)，一行像素占多少字节由格式图片格式决定。
                                                                            // 因为字幕的纹理创建时，格式为SDL_PIXELFORMAT_ARGB8888，所以一个像素占4个字节(一个像素包含ARGB)，那么一行像素占w*4，故左移2。
                                                                            // 备注：操作纹理的数据都是以字节为单位，视频的yuv同理，不过yuv有linesize行大小记录字节数，更方便点，这里字幕需要自己换算.
                                        // 可以稍微参考https://blog.csdn.net/qq_42024067/article/details/104853345.
                                        //若pitch的值等价于sub_rect->w << 2，就说明猜想是对的.
                                    }
                                    SDL_UnlockTexture(_sub_texture);
                                }
                            }
                        }

                        _subpq.frame_queue_next();
                        // 这里看到，字幕丢帧是不需要goto retry的.

                    }else {
                        break;
                    }

                }// <== while end ==>
            }// <== if (is->subtitle_st) end ==>
#endif

            _pictq.frame_queue_next();   // 当前vp帧出队列，vp变成last_vp，这样video_display就能播放待显示帧。
            _force_refresh = 1;          /* 说明需要刷新视频帧 */

            if (_step && !_paused)
                stream_toggle_pause();    // 逐帧的时候那继续进入暂停状态。音频在逐帧模式下不需要处理，因为它会自动按照暂停模式下的流程处理。所以暂停、播放、逐帧都比较简单。

        }// <== else end ==>

    display:
        /* display picture */
        if (!_display_disable && _force_refresh && _show_mode == VideoState::SHOW_MODE_VIDEO && _pictq.frame_queue_get_rindex_shown())// is->pictq.rindex_shown会在上面的frame_queue_next被置为1.
            video_display(); // 重点是显示。see here

    }//<== if (is->video_st) end ==>


    _force_refresh = 0;
#if 0
    show_status = 0;
    if (show_status) {
        static int64_t last_time;
        int64_t cur_time;
        int aqsize, vqsize, sqsize;
        double av_diff;

        cur_time = av_gettime_relative();
        if (!last_time || (cur_time - last_time) >= 30000) {
            aqsize = 0;
            vqsize = 0;
            sqsize = 0;
            if (is->audio_st)
                aqsize = is->audioq.size;
            if (is->video_st)
                vqsize = is->videoq.size;
            if (is->subtitle_st)
                sqsize = is->subtitleq.size;
            av_diff = 0;
            if (is->audio_st && is->video_st)
                av_diff = get_clock(&is->audclk) - get_clock(&is->vidclk);
            else if (is->video_st)
                av_diff = get_master_clock(is) - get_clock(&is->vidclk);
            else if (is->audio_st)
                av_diff = get_master_clock(is) - get_clock(&is->audclk);
            av_log(NULL, AV_LOG_INFO,
                "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%"PRId64"/%"PRId64"   \r",
                get_master_clock(is),
                (is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
                av_diff,
                is->frame_drops_early + is->frame_drops_late,
                aqsize / 1024,
                vqsize / 1024,
                sqsize,
                is->video_st ? is->viddec.avctx->pts_correction_num_faulty_dts : 0,
                is->video_st ? is->viddec.avctx->pts_correction_num_faulty_pts : 0);
            fflush(stdout);
            last_time = cur_time;
        }
    }
#endif
}


void VideoState::refresh_loop_wait_event(SDL_Event *event) {
    double remaining_time = 0.0; /* 休眠等待，remaining_time的计算在video_refresh中 */

    /* 调用SDL_PeepEvents前先调用SDL_PumpEvents，将输入设备的事件抽到事件队列中 */
    SDL_PumpEvents();

    /*
    * SDL_PeepEvents：check是否有事件，比如鼠标移入显示区等，有就
    * 从事件队列中拿一个事件，放到event中，如果没有事件，则进入循环中
    * SDL_PeekEvents用于读取事件，在调用该函数之前，必须调用SDL_PumpEvents搜集键盘等事件
    */
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        // 如果光标没隐藏并且光标显示时长已经超过1ms，那么将其隐藏.
        // 这个功能就是实现如果用户在一定时间没有操作鼠标，那么将其隐藏.
        if (!_cursor_hidden && av_gettime_relative() - _cursor_last_shown > CURSOR_HIDE_DELAY) {
            /*
            * SDL_ShowCursor：切换光标是否显示。
            * 参数：1显示游标，0隐藏游标，-1查询当前游标状态。
            * 如果光标显示，返回1;如果光标隐藏，返回0。
            */
            SDL_ShowCursor(0);
            _cursor_hidden = 1;
        }

        /*
        * remaining_time就是用来进行音视频同步的。
        * 在video_refresh函数中，根据当前帧显示时刻(display time)和实际时刻(actual time)
        * 计算需要sleep的时间，保证帧按时显示
        */
        if (remaining_time > 0.0) {//sleep控制画面输出的时机
            av_usleep((int64_t)(remaining_time * 1000000.0)); // remaining_time <= REFRESH_RATE
        }

        remaining_time = REFRESH_RATE;
        if (_show_mode != VideoState::SHOW_MODE_NONE &&  // 显示模式不等于SHOW_MODE_NONE
            (!_paused						// 非暂停状态
                || _force_refresh)			// 强制刷新状态
            ) {
            // 只有符合上面的3个条件才更新视频
            video_refresh(&remaining_time);
        }

        /* 从输入设备中搜集事件，推动这些事件进入事件队列，更新事件队列的状态，
        * 不过它还有一个作用是进行视频子系统的设备状态更新，如果不调用这个函数，
        * 所显示的视频会在大约10秒后丢失色彩。没有调用SDL_PumpEvents，将不会
        * 有任何的输入设备事件进入队列，这种情况下，SDL就无法响应任何的键盘等硬件输入。
        */
        SDL_PumpEvents();
    }
}

int VideoState::WriteThread(){
    SDL_Event event;
    double incr, pos, frac;

#ifdef USE_DLL_PLAY
    // 设置写线程分离
    SDL_DetachThread(_write_tid);
#endif

    for (;;) {
        double x;

        refresh_loop_wait_event(&event);

        switch (event.type) {
        case FF_QUIT_EVENT:				/* 自定义事件,在这用于被动退出 */
            do_exit();
            //printf("FF_QUIT_EVENT\n");
            SPDERROR("write_thread recevice FF_QUIT_EVENT, url: {}", _filename.c_str());
            return -1;
        case FF_INITIATIVE_QUIT_EVENT:	/* 自定义事件,在这用于主动退出(用户主动调关闭) */
            //cur_stream->do_exit();
            SPDERROR("write_thread recevice FF_INITIATIVE_QUIT_EVENT, url: {}", _filename.c_str());
            //printf("FF_INITIATIVE_QUIT_EVENT\n");
            return 0;
        case SDL_WINDOWEVENT:		/* 窗口事件 */
            switch (event.window.event) {
            case SDL_WINDOWEVENT_SIZE_CHANGED:
                _screen_width  = _width  = event.window.data1;
                _screen_height = _height = event.window.data2;
                if (_vis_texture) {
                    SDL_DestroyTexture(_vis_texture);
                    _vis_texture = NULL;
                }
            case SDL_WINDOWEVENT_EXPOSED:
                _force_refresh = 1;
            }
            break;
        default:
            break;
        }
#if 0
        //printf("type: %d\n", event.type);
        switch (event.type) {
        case SDL_KEYDOWN:	/* 键盘事件 */
                            /*
                            * keysym记录了按键的信息，其结构为：
                            typedef struct SDL_Keysym
                            {
                            SDL_Scancode scancode;  // 键盘硬件产生的扫描码
                            SDL_Keycode sym;        // SDL所定义的虚拟码
                            Uint16 mod;             // 修饰键
                            Uint32 unused;          // 未使用，可能有些版本是按键的Unicode码
                            } SDL_Keysym;
                            */
                            // 如果用户设置了退出或者键盘按下Esc、q键，那么程序直接退出。
            if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                do_exit(cur_stream);
                break;
            }
            if (!cur_stream->width)
                continue;

            // 根据SDL的虚拟码进行处理键盘事件
            switch (event.key.keysym.sym) {
            case SDLK_f:
                toggle_full_screen(cur_stream);
                cur_stream->force_refresh = 1;
                break;
            case SDLK_p:
            case SDLK_SPACE: // 1. 按空格键触发暂停/恢复
                toggle_pause(cur_stream);
                break;
            case SDLK_m:    // 3. 静音
                toggle_mute(cur_stream);
                break;
            case SDLK_KP_MULTIPLY:
            case SDLK_0:    // 3. 增大音量
                update_volume(cur_stream, 1, SDL_VOLUME_STEP);
                break;
            case SDLK_KP_DIVIDE:
            case SDLK_9:    // 3. 减少音量
                update_volume(cur_stream, -1, SDL_VOLUME_STEP);
                break;
            case SDLK_s: // 2. Step to next frame
                step_to_next_frame(cur_stream);
                break;
            case SDLK_a:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                break;
            case SDLK_v:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                break;
            case SDLK_c:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_t:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_w:
#if CONFIG_AVFILTER
                if (cur_stream->show_mode == SHOW_MODE_VIDEO && cur_stream->vfilter_idx < nb_vfilters - 1) {
                    if (++cur_stream->vfilter_idx >= nb_vfilters)
                        cur_stream->vfilter_idx = 0;
                }
                else {
                    cur_stream->vfilter_idx = 0;
                    toggle_audio_display(cur_stream);
                }
#else
                toggle_audio_display(cur_stream);
#endif
                break;
            case SDLK_PAGEUP:
                if (cur_stream->ic->nb_chapters <= 1) {
                    incr = 600.0;
                    goto do_seek;
                }
                seek_chapter(cur_stream, 1);
                break;
            case SDLK_PAGEDOWN:
                if (cur_stream->ic->nb_chapters <= 1) {
                    incr = -600.0;
                    goto do_seek;
                }
                seek_chapter(cur_stream, -1);
                break;
            case SDLK_LEFT:             // 4. 快进快退seek
                incr = seek_interval ? -seek_interval : -10.0;
                goto do_seek;
            case SDLK_RIGHT:
                incr = seek_interval ? seek_interval : 10.0;
                goto do_seek;
            case SDLK_UP:
                incr = 60.0;
                goto do_seek;
            case SDLK_DOWN:
                incr = -60.0;
            do_seek:
                if (seek_by_bytes) {
                    pos = -1;
                    if (pos < 0 && cur_stream->video_stream >= 0)
                        pos = frame_queue_last_pos(&cur_stream->pictq);
                    if (pos < 0 && cur_stream->audio_stream >= 0)
                        pos = frame_queue_last_pos(&cur_stream->sampq);
                    if (pos < 0)
                        pos = avio_tell(cur_stream->ic->pb);
                    if (cur_stream->ic->bit_rate)
                        incr *= cur_stream->ic->bit_rate / 8.0;// cur_stream->ic->bit_rate / 8.0代表每秒的字节数，那么想要快进10s或者后退10s，只需要乘以它即可得到字节增量。
                    else
                        incr *= 180000.0;// 码率不存在默认按照每秒180k/bytes计算。
                    pos += incr;
                    stream_seek(cur_stream, pos, incr, 1);// 此时pos代表目标的字节位置，单位是字节。
                }
                else {
                    pos = get_master_clock(cur_stream);
                    if (isnan(pos))
                        pos = (double)cur_stream->seek_pos / AV_TIME_BASE;
                    pos += incr;    // 现在是秒的单位。start_time是首帧开始时间
                    if (cur_stream->ic->start_time != AV_NOPTS_VALUE && pos < cur_stream->ic->start_time / (double)AV_TIME_BASE)
                        pos = cur_stream->ic->start_time / (double)AV_TIME_BASE;// 如果后退的时间小于第一帧，则pos为第一帧的时间。
                    stream_seek(cur_stream, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);// 此时pos代表目标的时间戳位置，单位是秒。
                }
                break;
            default:
                break;
            }// <== switch (event.key.keysym.sym) end ==>

            break;
        case SDL_MOUSEBUTTONDOWN:			/* 5. 鼠标按下事件 里面的双击左键 */
            if (exit_on_mousedown) {
                do_exit(cur_stream);
                break;
            }
            if (event.button.button == SDL_BUTTON_LEFT) {
                static int64_t last_mouse_left_click = 0;
                if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                    //连续鼠标左键点击2次显示窗口间隔小于0.5秒，则进行全屏或者恢复原始窗口
                    toggle_full_screen(cur_stream);
                    cur_stream->force_refresh = 1;
                    last_mouse_left_click = 0;
                }
                else {
                    last_mouse_left_click = av_gettime_relative();
                }
            }
            // 这里注意，鼠标按下时间同样会触发鼠标移动，因为没有break。
        case SDL_MOUSEMOTION:		/* 4. 快进快退seek 鼠标移动事件 */
            if (cursor_hidden) {
                SDL_ShowCursor(1);
                cursor_hidden = 0;
            }
            cursor_last_shown = av_gettime_relative();
            //printf("tyytestMoveNum: %d, type: %d\n", tyytestMoveNum++, event.type);
            //int va1 = SDL_MOUSEMOTION;      // 1024
            //int va2 = SDL_MOUSEBUTTONDOWN;  // 1025
            if (event.type == SDL_MOUSEBUTTONDOWN) {// 留个疑问，为什么最外层event.type进入SDL_MOUSEMOTION后，这里还能进入SDL_MOUSEBUTTONDOWN。
                                                    // 答：这是因为进入SDL_MOUSEMOTION，ffplay是依赖SDL_MOUSEBUTTONDOWN没有break进入的，
                                                    // 所以进入SDL_MOUSEMOTION后，这里还能进入SDL_MOUSEBUTTONDOWN。
                if (event.button.button == SDL_BUTTON_LEFT) {
                    printf("tyyLEFT, x: %d\n", event.button.x);
                }
                if (event.button.button == SDL_BUTTON_RIGHT) {
                    printf("tyyRIGHT, x: %d\n", event.button.x);
                }
                if (event.button.button == SDL_BUTTON_MIDDLE) {
                    printf("tyyMIDDLE, x: %d\n", event.button.x);
                }

                if (event.button.button != SDL_BUTTON_RIGHT)
                    break;
                x = event.button.x;// 鼠标单击右键按下，该x坐标相对于正在播放的窗口，而不是电脑屏幕。
            }
            else {
                if (!(event.motion.state & SDL_BUTTON_RMASK))// 不存在SDL_BUTTON_RMASK=4，则直接break.SDL_BUTTON_RMASK代表什么事件暂未研究
                    break;// 一般单纯在正在播放的窗口触发移动事件，会从这里break。
                x = event.motion.x;
            }
            if (seek_by_bytes || cur_stream->ic->duration <= 0) {
                uint64_t size = avio_size(cur_stream->ic->pb); // 整个文件的字节
                stream_seek(cur_stream, size*x / cur_stream->width, 0, 1);// 和时间戳的求法一样，参考时间戳。
            }
            else {
                int64_t ts;
                int ns, hh, mm, ss;
                int tns, thh, tmm, tss;
                tns = cur_stream->ic->duration / 1000000LL;// 将视频总时长单位转成秒
                thh = tns / 3600;                          // 获取总时长的小时的位数
                tmm = (tns % 3600) / 60;                   // 获取总时长的分钟的位数
                tss = (tns % 60);                          // 获取总时长秒的位数
                                                           // 根据宽度作为x坐标轴，并划分总时长total后，那么x轴上的某一点a的时间点为：t=a/width*total.
                                                           // 例如简单举个例子，宽度为1920，总时长也是1920，那么每个点刚好占1s，假设a的坐标为1000，那么t就是1000s。即t=1000/1920*1920.
                frac = x / cur_stream->width;               // 求出这一点在x轴的占比。实际我们也可以求 这一点在总时长的占比 来计算。原理都是一样的。
                ns = frac * tns;                          // 求出播放到目标位置的时间戳，单位是秒。
                hh = ns / 3600;                           // 然后依次获取对应的时、分、秒的位数。
                mm = (ns % 3600) / 60;
                ss = (ns % 60);
                av_log(NULL, AV_LOG_INFO,
                    "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac * 100,
                    hh, mm, ss, thh, tmm, tss);
                ts = frac * cur_stream->ic->duration;
                if (cur_stream->ic->start_time != AV_NOPTS_VALUE)// 细节
                    ts += cur_stream->ic->start_time;
                stream_seek(cur_stream, ts, 0, 0);// rel增量是0时，代表鼠标事件的seek。
            }
            break;
        case SDL_WINDOWEVENT:		/* 窗口事件 */
            switch (event.window.event) {
            case SDL_WINDOWEVENT_SIZE_CHANGED:
                screen_width = cur_stream->width = event.window.data1;
                screen_height = cur_stream->height = event.window.data2;
                if (cur_stream->vis_texture) {
                    SDL_DestroyTexture(cur_stream->vis_texture);
                    cur_stream->vis_texture = NULL;
                }
            case SDL_WINDOWEVENT_EXPOSED:
                cur_stream->force_refresh = 1;
            }
            break;
        case SDL_QUIT:
        case FF_QUIT_EVENT:	/* ffplay自定义事件,用于主动退出 */
            do_exit(cur_stream);
            break;
        default:
            break;

        }//<== switch (event.type) end ==>

#endif
    }
}

int write_thread1(void *arg) {

    VideoState *is = (VideoState*)arg;
    if(NULL == is){
        return -1;
    }

    int ret = is->WriteThread();
    if(ret != 0){
        //printf("WriteThread error, errno: %d\n", ret);
        SPDERROR("WriteThread error, errno: {}", ret);
        return ret;
    }

    SPDINFO("WriteThread exit, errno: {}", ret);
    return 0;

}

void VideoState::do_exit() {

    stream_close();

    if (_renderer)
        SDL_DestroyRenderer(_renderer);
    if (_window)
        SDL_DestroyWindow(_window);

    //uninit_opts();
#if CONFIG_AVFILTER
    av_freep(&_vfilters_list);
#endif

    avfilter_graph_free(&_agraph);
    _agraph = NULL;

    avformat_network_deinit();
    //if (_show_status)
    //	printf("\n");

    SDL_Quit();
    //av_log(NULL, AV_LOG_QUIET, "%s", "");
    //exit(0);
}

int VideoState::Play() {

    /* 创建读线程 */
    _read_tid = NULL;
    _read_tid = SDL_CreateThread(read_thread, "read_thread", this);
    if (!_read_tid) {
        //returnVal = PLAY_FF_PLAY_FAILED;
        SPDERROR("create read thread failed, errno: {}", SDL_GetError());
        return -1;
    }

    _write_tid = NULL;
    _write_tid = SDL_CreateThread(write_thread1, "write_thread", this);
    if (!_write_tid) {
        //returnVal = PLAY_FF_PLAY_FAILED;
        SPDERROR("create write thread failed, errno: {}", SDL_GetError());
        return -2;
    }

    SPDINFO("Play ok");
    return 0;
}

void VideoState::stream_component_close(int stream_index)
{
    AVFormatContext *ic = _ic;
    AVCodecParameters *codecpar;

    // 非法的流下标直接返回
    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;

    // 获取流的编解码器参数
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        _auddec.decoder_abort(&_sampq);
        SDL_CloseAudioDevice(_audio_dev);
        _auddec.decoder_destroy();
        swr_free(&_swr_ctx);
        av_freep(&_audio_buf1);
        _audio_buf1_size = 0;
        _audio_buf = NULL;

        if (_rdft) {
            av_rdft_end(_rdft);
            av_freep(&_rdft_data);
            _rdft = NULL;
            _rdft_bits = 0;
        }
        break;
    case AVMEDIA_TYPE_VIDEO:
        _viddec.decoder_abort(&_pictq);
        _viddec.decoder_destroy();
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        _subdec.decoder_abort(&_subpq);
        _subdec.decoder_destroy();
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        _audio_st = NULL;
        _audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        _video_st = NULL;
        _video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        _subtitle_st = NULL;
        _subtitle_stream = -1;
        break;
    default:
        break;
    }
}

bool VideoState::Close() {

    //stream_close();
    do_exit();

    return true;
}

//void VideoState::DeInit() {
//	if (nullptr != _window) {
//		SDL_DestroyWindow(_window);
//		_window = NULL;
//	}
//
//	if (nullptr == _renderer) {
//		SDL_DestroyRenderer(_renderer);
//		_renderer = NULL;
//	}
//}

bool VideoState::SetAudioVolume(int volume) {

    if (volume > 100 || volume < 0) {
        return false;
    }

    double sdlVolueme = (volume / 100.0) * 128;// sdl的单位是[0, 128]，用户传进来的是[0, 100]
    _audio_volume = sdlVolueme;

    return true;
}

bool VideoState::SetMute(bool status) {

    _muted = status == 1 ? 1 : 0;
    return true;
}

bool VideoState::SetMute() {
    _muted = 0;
    return true;
}

int VideoState::GetAudioVolume() {
    return _muted == 0 ? (_audio_volume / 128.0) * 100 : 0;
}

bool VideoState::StartupCapture(std::string allPath) {
    _captureCount++;
    _capturePath = allPath;
    return true;
}

int VideoState::GetFps() {
    return _frame_rate;
}

double VideoState::GetFpsSafe() {
    return _frame_rate;
}

#endif

