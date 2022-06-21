#ifndef __FFPLAY__H__
#define __FFPLAY__H__

//#define USE_DLL_PLAY
int write_thread1(void *arg);

#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <string>
#include <atomic>
#include <windows.h>
#define CONFIG_AVFILTER 1
#define CONFIG_AVDEVICE 1
#define CONFIG_RTSP_DEMUXER 1
#define CONFIG_MMSH_PROTOCOL 1

extern "C"
{
#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/dict.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavcodec/avfft.h"
#include "libswresample/swresample.h"
#include "libavutil/display.h"

#if CONFIG_AVFILTER
# include "libavfilter/avfilter.h"
# include "libavfilter/buffersink.h"
# include "libavfilter/buffersrc.h"
#endif
}

#include <SDL.h>
#include <SDL_thread.h>
#ifdef _WIN32
#undef main /* We don't want SDL to override our main() */
#endif

namespace N1 {namespace N2 {namespace N3 {

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

    /* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
    /* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

    /* Step size for volume control in dB */
#define SDL_VOLUME_STEP (0.75)

    /* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04  // 40ms
    /* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1   // 100ms
    /* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.040
    /* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

    /* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

    /* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

    /* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
//#define AUDIO_DIFF_AVG_NB   20
#define AUDIO_DIFF_AVG_NB   30

    /* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

    /* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
    /* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define CURSOR_HIDE_DELAY 1000000

#define USE_ONEPASS_SUBTITLE_RENDER 1

#define VIDEO_PICTURE_QUEUE_SIZE	3       // 图像帧缓存数量
#define SUBPICTURE_QUEUE_SIZE		16      // 字幕帧缓存数量
#define SAMPLE_QUEUE_SIZE           9       // 采样帧缓存数量
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))


    // 一个特殊的packet，主要用来作为非连续的两端数据的“分界”标记：
    // 1）插入 flush_pkt 触发PacketQueue其对应的serial，加1操作；
    // 2）触发解码器清空自身缓存 avcodec_flush_buffers()，以备新序列的数据进行新解码
    static AVPacket flush_pkt;

    /**
    * ok
    * PacketQueue队列节点。
    */
    typedef struct MyAVPacketList {
        AVPacket		pkt;            //解封装后的数据
        struct MyAVPacketList	*next;  //下一个节点
        int			serial;             //播放序列
    } MyAVPacketList;

    /**
    * ok
    * PacketQueue队列。
    */
    class PacketQueue {
    public:
        PacketQueue();
        virtual ~PacketQueue();

    public:
        int packet_queue_init();
        void packet_queue_flush();

        int packet_queue_put(AVPacket *pkt);
        int packet_queue_put_nullpacket(int stream_index);
        int packet_queue_get(AVPacket *pkt, int block, int *serial);
        void packet_queue_start();

        void packet_queue_abort();
        void packet_queue_destroy();

    public:
        typedef struct pktStatus {
            int nbPackets;
            int size;
            int64_t		duration;
        };
        pktStatus packet_queue_get_status();
        int packet_queue_get_packets();
        int* packet_queue_get_serial();         // 获取serial的内存地址，只给时钟的_queue_serial使用
        int  packet_queue_get_abort();
        int  packet_queue_get_duration();
        int  packet_queue_get_size();

    private:
        int packet_queue_put_private(AVPacket *pkt);

    private:
        MyAVPacketList	*_first_pkt, *_last_pkt; // 队首，队尾指针
        int		_nb_packets;                     // 包数量，也就是队列元素数量
        int		_size;                           // 队列所有元素的数据大小总和
        int64_t		_duration;                   // 队列所有元素的数据播放持续时间
        int		_abort_request;                  // 用户退出请求标志.0未退出，1退出。
        int		_serial;                         // 播放序列号，和MyAVPacketList的serial作用相同，但改变的时序稍微有点不同
        SDL_mutex	*_mutex;                     // 用于维持PacketQueue的多线程安全(SDL_mutex可以按pthread_mutex_t理解）
        SDL_cond	*_cond;                      // 用于读、写线程相互通知(SDL_cond可以按pthread_cond_t理解)
    };

    typedef struct AudioParams {
        int			freq;                   // 采样率
        int			channels;               // 通道数
        int64_t		channel_layout;         // 通道布局，比如2.1声道，5.1声道等
        enum AVSampleFormat	fmt;            // 音频采样格式，比如AV_SAMPLE_FMT_S16表示为有符号16bit深度，交错排列模式。
        int			frame_size;             // 一个采样单元占用的字节数（比如2通道时，则左右通道各采样一次合成一个采样单元）
        int			bytes_per_sec;          // 一秒时间的字节数，比如采样率48Khz，2 channel，16bit，则一秒48000*2*16/8=192000
    } AudioParams;

    /**
    * 封装的时钟结构体。
    * 这里讲的系统时钟 是通过av_gettime_relative()获取到的时钟，单位为微妙。
    */
    class Clock {
    public:
        Clock();
        ~Clock();

    public:
        void init_clock(int *queue_serial);
        void set_clock(double pts, int serial);
        void set_clock_at(double pts, int serial, double time);
        double get_clock();

        void set_clock_speed(double speed);
        void sync_clock_to_slave(Clock *slave);

    public:  // tyy code
        void set_clock_flush();
        int get_serial();
        int* get_serialc();
        double get_last_updated();
        int get_paused();
        void set_paused(int paused);

    private:
        double	_pts;            // 时钟基础, 当前帧(待播放)显示时间戳，播放后，当前帧变成上一帧

                                 // 当前pts与当前系统时钟的差值, audio、video对于该值是独立的
        double	_pts_drift;      // clock base minus time at which we updated the clock

                                 // 当前时钟(如视频时钟)最后一次更新时间，也可称当前时钟时间
        double	_last_updated;   // 最后一次更新的系统时钟

        double	_speed;          // 时钟速度控制，用于控制播放速度.视频和音频时钟都不改变该播放速度(默认是1)，只有外部时钟才会去调整这个速度(包多加快速度，包少减慢速度)。

                                 // 播放序列，所谓播放序列就是一段连续的播放动作，一个seek操作会启动一段新的播放序列
        int	_serial;             // clock is based on a packet with this serial

        int	_paused;             // = 1 说明是暂停状态

                                 // 指向packet_serial，即指向当前包队列的指针，用于过时的时钟检测。注意：这个_queue_serial是直接指向包队列的内存地址的。
        int *_queue_serial;      /* pointer to the current packet queue serial, used for obsolete clock detection */
    };

    /**
    * ok
    * Common struct for handling all types of decoded data and allocated render buffers.
    * 用于处理所有类型的解码数据和分配的呈现缓冲区的通用结构。即用于缓存解码后的数据
    */
    typedef struct Frame {
        AVFrame		*frame;         // 指向数据帧
        AVSubtitle	sub;            // 用于字幕
        int		serial;             // 帧序列，在seek的操作时serial会变化
        double		pts;            // 时间戳，单位为秒
        double		duration;       // 该帧持续时间，单位为秒
        int64_t		pos;            // 该帧在输入文件中的字节位置
        int		width;              // 图像宽度
        int		height;             // 图像高读
        int		format;             // 对于图像为(enum AVPixelFormat)，对于声音则为(enum AVSampleFormat)

        AVRational	sar;            // 图像的宽高比（16:9，4:3...），如果未知或未指定则为0/1
        int		uploaded;           // 用来记录该帧是否已经显示过？
        int		flip_v;             // = 1则垂直翻转， = 0则正常播放
    } Frame;

    /**
    * ok
    * 这是一个循环队列，windex是指其中的首元素，rindex是指其中的尾部元素.
    * 1. ffplay为什么帧队列使用数组设计？
    * 答：因为假设解码线程解码时是临时new一个AVFream结构，然后放进vector之类的数组，那么这个数组能很大吗？
    *       肯定不能，因为解码后的帧是很占内存的，所以必须限制这个解码帧队列的大小。故使用栈数组是比较好的处理，
    *       让解码线程每次解码前都需要从这个栈数组中获取可写帧去解码。
    *       这个帧队列严格上也不算是一个循环队列。
    * 数组实现循环队列可以看https://blog.csdn.net/weixin_44517656/article/details/115605127。
    *
    * 2. 队列为满如何处理；为空又如何处理？
    * 答：因为这里写操作是解码线程，读操作是显示线程。所以当为满时，解码线程阻塞，显示线程尽快显示；为空时，显示线程阻塞，解码线程尽快解码。
    *
    * 3. 帧队列使用哪些变量时需要加锁？
    * 答：只需要在使用到_size时加锁即可。当然条件变量_cond是一个，总共两个。ffplay的帧队列设计的锁用得非常精妙，当然在这种场景可能使用，但是在
    *       日常开发时最好加锁，防止出现其它问题，更何况目前锁的耗时几乎可以忽略不计。程序跑起来后后面再谈优化锁是一种更好的方案。
    *
    * 帧队列的设计与平常的队列有点区别，因为我们需要限制队列的大小。当然也可以使用平常的队列设计，那么限制队列大小就是从外部去限制了，
    * 例如视频低延时处理时，获取包队列的包数后，去限制队列的大小。或者你也可以根据自己的想法去设计一个帧队列都是可以的，思想是无穷的。
    *
    * 帧队列设计原理(可以根据下面三点进行画图理解)，非常重要：
    * 1. 解码后数组即得到一帧数据(解码前需要获取到帧数组的元素才能解码)，待显示帧+1，即size加1；即解码时使用 _windex + _size。
    * 2. 若size大于0，那么读线程可以读进行显示，size减1；即显示时使用 _rindex + _size。
    * 3. _rindex与_windex互不干扰，初始值都是0，而_size是决定能否读写的关键，所以必须要理解好。
    */
    class FrameQueue {
    public:
        FrameQueue();
        ~FrameQueue();

    public:
        int frame_queue_init(PacketQueue *pktq, int max_size, int keep_last);

    public:
        Frame *frame_queue_peek_writable(); // 获取一帧用于解码，只有解码成功后，才会调用frame_queue_push对size加1.
        void frame_queue_push();            // frame_queue_push必定是配合frame_queue_peek_writable使用。

        Frame *frame_queue_peek_readable(); // 读取一帧待显示帧
        int frame_queue_nb_remaining();     //frame_queue_nb_remaining配合frame_queue_peek可实现frame_queue_peek_readable的功能，区别是前者相当于非阻塞，后者相当于阻塞(frame_queue_peek_readable)
        Frame *frame_queue_peek_last();
        Frame *frame_queue_peek();
        Frame *frame_queue_peek_next();
        void frame_queue_next();            // 首帧时：将该帧置为一帧已经显示的帧；其余时候是将已经显示的帧出队列并回收。

        void frame_queue_signal();
        int64_t frame_queue_last_pos();

    public:
        // 资源回收相关函数.
        void frame_queue_unref_item(Frame *vp);
        void frame_queue_destory();

        // tyycode
        int frame_queue_get_rindex_shown();
        SDL_mutex *frame_queue_get_mutex();

    private:
        Frame	_queue[FRAME_QUEUE_SIZE];        // FRAME_QUEUE_SIZE  最大size, 数字太大时会占用大量的内存，需要注意该值的设置
        int		_rindex;                         // 读索引。待播放时读取此帧进行播放，播放后此帧成为上一帧。只在消费线程使用，故可不加锁。
        int		_windex;                         // 写索引，包含索引本身。只在生产线程使用，故可不加锁。
        int		_size;                           // 当前实际的总帧数，可能包含一帧已显示的帧。生产、消费线程都使用，故需加锁。
        int		_max_size;                       // 可存储最大帧数
        int		_keep_last;                      // = 1说明要在队列里面保持最后一帧的数据不释放，只在销毁队列的时候才将其真正释放
        int		_rindex_shown;                   // 初始化为0，配合keep_last=1使用。用于给帧队列缓存一帧已显示的帧而创建的变量，=1时表示帧队列已经缓存了一帧已经显示过的帧，=0表示没缓存。
                                                 // 并且从frame_queue_next看到，出队列时，若_keep_last=1，_rindex_shown=0，size是没有减1的，所以size是可能包含一帧已显示的帧。
                                                 // 所以可读帧的大小需要减去 _rindex_shown，即 未显示帧数 = _size - _rindex_shown

        SDL_mutex	*_mutex;                     // 互斥量
        SDL_cond	*_cond;                      // 条件变量
        PacketQueue	*_pktq;                      // 数据包缓冲队列
    };

    /**
    *音视频同步方式，缺省以音频为基准
    */
    enum {
        AV_SYNC_AUDIO_MASTER,                   // 以音频为基准
        AV_SYNC_VIDEO_MASTER,                   // 以视频为基准
        AV_SYNC_EXTERNAL_CLOCK,                 // 以外部时钟为基准，synchronize to an external clock */
    };

    /**
    * 解码器封装
    */
    class Decoder {
    public:
        Decoder();
        ~Decoder();

    public:
        void decoder_init(AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond);
        int decoder_start(int(*fn)(void *), const char *thread_name, void* arg);

        int decoder_decode_frame(AVFrame *frame, AVSubtitle *sub);
        void decoder_destroy();// _avctx在这回收，后面改掉它
        void decoder_abort(FrameQueue *fq);// 线程在这回收，后面看看怎么处理.

    public://tyycode
       void decoder_set_start_pts(int64_t start_pts);
       void decoder_set_start_pts_tb(AVRational start_pts_tb);
       int decoder_get_serial();
       int decoder_get_finished();
       void decoder_set_finished(int finished);
       AVCodecContext *decoder_get_avctx();

    private:
        AVPacket _pkt;
        PacketQueue	*_queue;             // 数据包队列
        AVCodecContext	*_avctx;         // 解码器上下文
        int		_pkt_serial;             // 包序列
        int		_finished;               // =0，解码器处于工作状态；=非0，解码器处于空闲状态
        int		_packet_pending;         // =0，解码器处于异常状态，需要考虑重置解码器；=1，解码器处于正常状态
        SDL_cond	*_empty_queue_cond;  // 检查到packet队列空时发送 signal缓存read_thread读取数据
        int64_t		_start_pts;          // 初始化时是stream的start time
        AVRational	_start_pts_tb;       // 初始化时是stream的time_base
        int64_t		_next_pts;           // 只在音频有效。根据本次解码后的frame的pts，估计下一帧的pts，当解出来的部分帧没有有效的pts时则使用next_pts进行推算
        AVRational	_next_pts_tb;        // next_pts的单位
        SDL_Thread	*_decoder_tid;       // 线程句柄
    };

    /**
    * ok
    * 播放器封装。
    */
    //class VideoState : public AbstractPlayer, public CommonLooper
    class VideoState
    {
    public:
        VideoState();
        ~VideoState();

        bool Init(HWND handle, std::string url);// 初始化SDL相关以及其他变量

    private:
        int InitFmtCtx();// 初始化ffmpeg的fmtCtx
        void set_default_window_size(int width, int height, AVRational sar);
        void calculate_display_rect(SDL_Rect *rect, int scr_xleft, int scr_ytop, int scr_width, int scr_height, int pic_width, int pic_height, AVRational pic_sar);
        AVCodecContext *ConfigureCodec(int stream_index);
        int opt_add_vfilter(void *optctx, const char *opt, const char *arg);

    public:
        //void DeInit();
        int Play();
        bool Close();

        // 读线程
        int ReadThread();
        // 解码线程
        int AudioThread();
        int VideoThread();
        int SubtitleThread();
        // 消费线程
        void SdlAudioCallback(void *opaque, Uint8 *stream, int len);
        int WriteThread();

        // 视频消费线程内部函数
        void refresh_loop_wait_event(SDL_Event *event);
        void video_refresh(double *remaining_time);
        int video_open();


        // 设置音量
        bool SetAudioVolume(int volume);
        bool SetMute(bool status);
        bool SetMute();
        int GetAudioVolume();
        bool StartupCapture(std::string allPath);

        int GetFps();
        double GetFpsSafe();

        // 资源回收
        void stream_component_close(int stream_index);
        void stream_close();
        void do_exit();
        void read_thread_close();
        std::string get_ffmpeg_error(int errVal);

        int64_t get_valid_channel_layout(int64_t channel_layout, int channels);
        int stream_component_open(int stream_index);
        int configure_audio_filters(const char *afilters, int force_output_format);
        int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph, AVFilterContext *source_ctx, AVFilterContext *sink_ctx);
        int audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params);
        int audio_decode_frame();
        //int64_t get_valid_channel_layout(int64_t channel_layout, int channels);
        int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1, enum AVSampleFormat fmt2, int64_t channel_count2);

        int synchronize_audio(int nb_samples);
        int get_master_sync_type();
        double get_master_clock();

        int get_video_frame(AVFrame *frame);
        int configure_video_filters(AVFilterGraph *graph, const char *vfilters, AVFrame *frame);
        double get_rotation(AVStream *st);
        int queue_picture(AVFrame *src_frame, double pts, double duration, int64_t pos, int serial);
        void video_display();
        void video_image_display();
        int upload_texture(SDL_Texture **tex, AVFrame *frame, struct SwsContext **img_convert_ctx);
        void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode);
        int realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture);
        void set_sdl_yuv_conversion_mode(AVFrame *frame);

        // 音视频同步
        double vp_duration(Frame *vp, Frame *nextvp);
        double compute_target_delay(double delay);
        void update_video_pts(double pts, int64_t pos, int serial);

        void step_to_next_frame();
        void stream_toggle_pause();
        int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue);
        void stream_seek(int64_t pos, int64_t rel, int seek_by_bytes);

    public:
        int get_read_thread_abort();

    private:
        int save_frame_to_jpeg(const char *filename, const AVFrame* frame);
        int save_frame_to_bmp(const char *filename, const AVFrame* frame);
        bool SaveAsBMP(const char *filename, AVFrame *pFrameRGB, int width, int height, int index, int bpp);

    private:
        SDL_Thread	*_read_tid;      // 读线程句柄
        SDL_Thread	*_write_tid;     // 写线程句柄，代替ffplay的主线程
        AVInputFormat	*_iformat;   // 指向demuxer
        int		_abort_request = 0;      // =1时请求退出播放
        int	    _abort_request_w = 0;	 // 写线程的中断，=1时请求退出播放。
        int		_force_refresh;      // =1时需要刷新画面，请求立即刷新画面的意思
        int		_paused;             // =1时暂停，=0时播放
        int		_last_paused;        // 暂存“暂停”/“播放”状态
        int		_queue_attachments_req;
        int		_seek_req;           // 标识一次seek请求
        int		_seek_flags;         // seek标志，诸如AVSEEK_FLAG_BYTE等
        int64_t		_seek_pos;       // 请求seek的目标位置(当前位置+增量)
        int64_t		_seek_rel;       // 本次seek的位置增量
        int		_read_pause_return;

        // test

        // Init
        PacketQueue _videoq;         // 视频队列
        PacketQueue _audioq;		 // 音频packet队列
        PacketQueue _subtitleq;      // 字幕packet队列

        FrameQueue	_pictq;          // 视频Frame队列
        FrameQueue	_subpq;          // 字幕Frame队列
        FrameQueue	_sampq;          // 采样Frame队列

        Clock	_audclk;             // 音频时钟
        Clock	_vidclk;             // 视频时钟
        Clock	_extclk;             // 外部时钟

        std::string _filename;
        HWND _hDisplayWindow;		// c#传进来的句柄, tyy code
        SDL_Window *_window;						  // 窗口的ID
        SDL_Renderer *_renderer;					  // 渲染器
        SDL_RendererInfo _renderer_info = { 0 };	  // 渲染器的一些信息，主要用于查看错误
        int _width, _height, _xleft, _ytop;           // 宽、高，x起始坐标，y起始坐标
        SDL_cond *_continue_read_thread;             // 当读取数据队列满了后进入休眠时，可以通过该condition唤醒读线程
        int _audio_clock_serial;					  // 播放序列，seek可改变此值
        double _startup_volume = 50;                     // 起始音量
        double	_audio_volume;							  // 音量
        int			_muted;							  // =1静音，=0则正常
        int _av_sync_type;							  // 音视频同步类型, 默认audio master
        //int _preVolume;								  // tyycode，用于记录上一次的音量.
        //int _curVolume;								  // tyycode，用于记录目前的音量.
        // _audio_volume + _muted可以代替_preVolume、_curVolume的作用
        double _frame_rate;								// tyycode
        std::atomic<int> _captureCount;					// tyycode，是否有抓图请求，因为有可能多次请求，而我们还没进入抓图步骤，
                                                        // 所以我们使用int用于记录这些请求次数
        std::string _capturePath;						// tyycode，抓图路径



        // read thread
        int _video_stream;          // 视频流索引
        int _audio_stream;          // 音频流索引
        int _subtitle_stream;       // 字幕流索引
        int _last_video_stream, _last_audio_stream, _last_subtitle_stream;// 保留最近的相应audio、video、subtitle流的steam index
        int _eof;                    // 是否读取结束. 0未读取完毕 =1是表明数据读取完毕.
        AVFormatContext *_ic;        // iformat的上下文
        double _max_frame_duration;  // 一帧最大间隔. above this, we consider the jump a timestamp discontinuity
        int		_realtime;           // =1为实时流
        enum ShowMode {
            SHOW_MODE_NONE = -1,                // 无显示
            SHOW_MODE_VIDEO = 0,                // 显示视频
            SHOW_MODE_WAVES,                    // 显示波浪，音频
            SHOW_MODE_RDFT,                     // 自适应滤波器
            SHOW_MODE_NB
        } _show_mode;
#if CONFIG_AVFILTER
        struct AudioParams _audio_filter_src;// 信息从avctx得到，而avctx的信息是从流的参数拷贝过来的。
#endif
#if CONFIG_AVFILTER
        int _vfilter_idx;
        AVFilterContext *_in_video_filter;           // the first filter in the video chain
        AVFilterContext *_out_video_filter;          // the last filter in the video chain
        AVFilterContext *_in_audio_filter;           // the first filter in the audio chain。输入滤波器实例。
        AVFilterContext *_out_audio_filter;          // the last filter in the audio chain。输出滤波器实例。
        AVFilterGraph *_agraph;                      // audio filter graph
#endif



        Decoder _auddec;             // 音频解码器
        Decoder _viddec;             // 视频解码器
        Decoder _subdec;             // 字幕解码器



        double			_audio_clock;            // 当前音频帧的PTS + 当前帧Duration


                                                 // 以下4个参数 非audio master同步方式使用
        double			_audio_diff_cum;         // used for AV difference average computation
        double			_audio_diff_avg_coef;
        double			_audio_diff_threshold;
        int             _audio_diff_avg_count;
        // end

        AVStream		*_audio_st;              // 音频流

        int             _audio_hw_buf_size;      // SDL音频缓冲区的大小(字节为单位)

                                                // 指向待播放的一帧音频数据，指向的数据区将被拷入SDL音频缓冲区。若经过重采样则指向audio_buf1，
                                                // 否则指向frame中的音频
        uint8_t			*_audio_buf;             // 指向需要重采样的数据
        uint8_t			*_audio_buf1;            // 指向重采样后的数据
        unsigned int		_audio_buf_size;     // 待播放的一帧音频数据(audio_buf指向)的大小
        unsigned int		_audio_buf1_size;    // 申请到的音频缓冲区audio_buf1的实际尺寸
        int			_audio_buf_index;            // 更新拷贝位置 当前音频帧中已拷入SDL音频缓冲区
                                                 // 的位置索引(指向第一个待拷贝字节)
                                                 // 当前音频帧中尚未拷入SDL音频缓冲区的数据量:
                                                 // audio_buf_size = audio_buf_index + audio_write_buf_size
        int			_audio_write_buf_size;


        struct AudioParams _audio_src;           // 音频frame的参数

        struct AudioParams _audio_tgt;           // SDL支持的音频参数，重采样转换：audio_src->audio_tgt
        struct SwrContext *_swr_ctx;             // 音频重采样context
        int _frame_drops_early;                  // 丢弃视频packet计数
        int _frame_drops_late;                   // 丢弃视频frame计数



        // 音频波形显示使用
        int16_t _sample_array[SAMPLE_ARRAY_SIZE];    // 采样数组
        int _sample_array_index;                     // 采样索引
        int _last_i_start;                           // 上一开始
        RDFTContext *_rdft;                          // 自适应滤波器上下文
        int _rdft_bits;                              // 自使用比特率
        FFTSample *_rdft_data;                       // 快速傅里叶采样

        int _xpos;
        double _last_vis_time;
        SDL_Texture *_vis_texture;                   // 音频纹理Texture

        SDL_Texture *_sub_texture;                   // 字幕显示
        SDL_Texture *_vid_texture;                   // 视频显示


        AVStream *_subtitle_st;                      // 字幕流

        double _frame_timer;                         // 记录最后一帧播放的时刻
        double _frame_last_returned_time;            // 上一次返回时间
        double _frame_last_filter_delay;             // 上一个过滤器延时


        AVStream *_video_st;                         // 视频流


        struct SwsContext *_img_convert_ctx;         // 视频尺寸格式变换
        struct SwsContext *_sub_convert_ctx;         // 字幕尺寸格式变换


        //char *_filename;                             // 文件名


        int _step;                                   // =1 步进播放模式, =0 其他模式


        /* tyy code(ffplay的全局变量) */
        //Init gloabl
        int _audio_disable = 0;								// 是否禁用音频，0启用(默认)；1禁用，禁用将不会播放声音。
        int _video_disable = 0;

        // read thread global
        int _seek_by_bytes = -1;
        int64_t _start_time = AV_NOPTS_VALUE;         // 指定开始播放的时间
        const char* _wanted_stream_spec[AVMEDIA_TYPE_NB] = { 0 };   // 若用户命令传参进来，wanted_stream_spec会保存用户指定的流下标，st_index用于此时的记录想要播放的下标。
        int _subtitle_disable = 0;
#if CONFIG_AVFILTER
        const char **_vfilters_list = NULL;
        //const char _vfilters_list[1][50] = {"-vf subtitles=output.mkv:si=0"};
        int _nb_vfilters = 0;
        char *_afilters = NULL;                 // 用户音频的复杂过滤过程的字符串描述。
#endif
        int _filter_nbthreads = 0;  // filter线程数量

        // 一个特殊的packet，主要用来作为非连续的两端数据的“分界”标记：
        // 1）插入 flush_pkt 触发PacketQueue其对应的serial，加1操作；
        // 2）触发解码器清空自身缓存 avcodec_flush_buffers()，以备新序列的数据进行新解码
        AVPacket _flush_pkt;
        int _alwaysontop;                             // 是否顶置
        int _borderless;                              // 是否可以调整窗口大小。0可以；1不可以。

        AVInputFormat *_file_iformat;                 // 输入封装格式结构体，注AVFormatContext才是输入输出封装上下文

                                                      //int _av_sync_type = AV_SYNC_AUDIO_MASTER;   // 默认音频时钟同步
        AVDictionary *_sws_dict;
        AVDictionary *_swr_opts;// 用于保存音频过滤器的私有参数(类似AVCodecCtx的bit_rate这些)，在configure_audio_filters会给_agraph使用。
        AVDictionary *_format_opts, *_codec_opts, *_resample_opts;
        int _genpts = 0;
        int _find_stream_info = 1;

        const char *_window_title;

        int64_t _duration = AV_NOPTS_VALUE;
        int _show_status = 1;                         // 打印关于输入或输出格式的详细信息(tbr,tbn,tbc)，默认1代表打印



        //enum ShowMode _show_mode = SHOW_MODE_NONE;		// 给上面的show_mode赋值

        int _default_width = 640;                     // 最终在界面显示的宽.即渲染区域的宽，或者说是 解码后的图片的宽
        int _default_height = 480;                    // 最终在界面显示的高.即渲染区域的高，或者说是 解码后的图片的高
        int _screen_width = 0;                        // 用户指定播放窗口的宽度，即-x参数，中间参数
        int _screen_height = 0;                       // 用户指定播放窗口的高度，即-y参数，中间参数
        int _screen_left = SDL_WINDOWPOS_CENTERED;    // 显示视频窗口的x坐标，默认在居中
        int _screen_top = SDL_WINDOWPOS_CENTERED;     // 显示视频窗口的y坐标，默认居中
        int _lowres = 0;                              // 用户决定，用于输入的解码分辨率，但是最终由流的编码器最低支持的分辨率决定该值
        const char *_audio_codec_name = NULL;
        const char *_subtitle_codec_name = NULL;
        const char *_video_codec_name = NULL;
        int _fast = 0;                                 // 是否加速解码
        int64_t _audio_callback_time = 0;
        SDL_AudioDeviceID _audio_dev;// 打开音频设备后返回的设备ID。SDL_OpenAudio()始终返回1，SDL_OpenAudioDevice返回2以及2以上，这是为了兼容SDL不同版本。
        int _decoder_reorder_pts = -1;
        int _framedrop = -1;
        int _infinite_buffer = -1;
        int _loop = 1;        // 设置循环次数
        int _autoexit;
        int _cursor_hidden = 0;							// 鼠标是否隐藏.0显示，1隐藏
        int64_t _cursor_last_shown;						// 上一次鼠标显示时间戳。
        int _display_disable;                         // 是否显示视频，0显示(默认)，1不显示。置1后， 会导致video_disable会被置1.
        double _rdftspeed = 0.02;
        //const char *_input_filename;                  // 从命令行拿到的输入文件名，可以是网络流
        int _is_full_screen;
        //int64_t _audio_callback_time;
        unsigned _sws_flags = SWS_BICUBIC;
        int _autorotate = 1;
        const char *window_title;


        //int _loop = 1;        // 设置循环次数
        //int64_t _duration = AV_NOPTS_VALUE;
    };

}}}


#endif //
