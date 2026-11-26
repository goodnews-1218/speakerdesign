// Test-only C ABI stand-in. No USB access and no vendor DLL is loaded.
#include "Sdk.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <string>
#include <vector>
struct ViaimConnectSdk {
    ViaimConnectCallbacks callbacks=VIAIM_CONNECT_CALLBACKS_INIT;
    std::atomic<bool> running{false}, recording{false};
    std::atomic<bool> camera{false};
    std::thread worker;
};
#ifdef CONNECT_LAB_VIDEO
struct ViaimConnectCameraEnumeration {};
constexpr uint32_t width=320, height=180, fps=20;
#endif
static std::string scenario() {const char* p=std::getenv("CONNECT_LAB_TEST_SCENARIO");return p?p:"normal";}
static void event(ViaimConnectSdk* s,uint16_t id,ViaimConnectOperation operation,bool fail=false) {
    ViaimConnectEvent e=VIAIM_CONNECT_EVENT_INIT;e.command.id=id;e.command.operation=operation;e.protocol_result=fail?1:0;
    if(s->callbacks.event)s->callbacks.event(&e,s->callbacks.user_data);
}
extern "C" ViaimConnectResult __cdecl viaim_connect_create(const ViaimConnectCreateOptions*,ViaimConnectSdk** out) {
    *out=new ViaimConnectSdk;return VIAIM_CONNECT_OK;
}
extern "C" ViaimConnectResult __cdecl viaim_connect_set_log_level(ViaimConnectSdk*,ViaimConnectLogLevel){return 0;}
extern "C" ViaimConnectResult __cdecl viaim_connect_set_raw_log_flags(ViaimConnectSdk*,ViaimConnectRawLogFlags){return 0;}
extern "C" ViaimConnectResult __cdecl viaim_connect_open(ViaimConnectSdk* s,const ViaimConnectOpenOptions*,const ViaimConnectCallbacks* cb) {
    s->callbacks=*cb;s->running=true;
    s->worker=std::thread([s]{
        using namespace std::chrono_literals;
        std::this_thread::sleep_for(150ms);
        if(scenario()=="no_device")return;
        if(s->callbacks.connection_state)s->callbacks.connection_state(2,s->callbacks.user_data);
        uint32_t sequence=0;
        auto next=std::chrono::steady_clock::now();
        while(s->running) {
            next+=50ms;
            std::this_thread::sleep_until(next);
            if(!s->recording || scenario()=="no_audio")continue;
            if(scenario()=="disconnect" && sequence==4) {
                if(s->callbacks.connection_state)s->callbacks.connection_state(3,s->callbacks.user_data);
                s->recording=false;continue;
            }
            std::vector<uint8_t> bytes(1600,0x12);
#ifdef CONNECT_LAB_VIDEO
            if(s->camera) {
                std::vector<uint8_t> pixels(size_t(width)*height*3, 48);
                for(uint32_t y=0;y<height;++y) {
                    const auto x=(sequence*3)%width;
                    pixels[(size_t(y)*width+x)*3+1]=160;
                }
                ViaimConnectCameraFrame frame{};
                frame.struct_size=sizeof(frame);
                frame.pixel_format=VIAIM_CONNECT_CAMERA_PIXEL_FORMAT_BGR24;
                frame.width=width;frame.height=height;frame.plane_count=1;frame.strides[0]=width*3;
                frame.planes[0]={pixels.data(),pixels.size()};
                frame.sequence=sequence;
                if(s->callbacks.camera_frame)s->callbacks.camera_frame(&frame,s->callbacks.user_data);
                for(size_t i=0;i<bytes.size()/2;++i) {
                    const auto sample=static_cast<int16_t>(1800*std::sin(0.172787596*(sequence*800+i)));
                    bytes[2*i]=static_cast<uint8_t>(sample);
                    bytes[2*i+1]=static_cast<uint8_t>(static_cast<uint16_t>(sample)>>8);
                }
            }
#endif
            for(int source=0;source<2;source++) {
                ViaimConnectAudioPcmFrame f={};f.struct_size=sizeof(f);f.source=source;f.frame_sequence=sequence;
                f.sample_rate=scenario()=="wrong_format"?48000:16000;f.channel_count=1;f.bits_per_sample=16;
                f.sample_format=VIAIM_CONNECT_PCM_S16_LE;f.sample_count_per_channel=800;f.data=bytes.data();f.size=bytes.size();
                if(s->callbacks.audio_pcm)s->callbacks.audio_pcm(&f,s->callbacks.user_data);
            }
            // The callback must already have copied the borrowed memory.
            std::fill(bytes.begin(),bytes.end(),uint8_t{0xEE});++sequence;
        }
    });return VIAIM_CONNECT_OK;
}
extern "C" ViaimConnectResult __cdecl viaim_connect_send(ViaimConnectSdk* s,const ViaimConnectRequest* r) {
    event(s,r->command.id,VIAIM_CONNECT_OPERATION_RESPONSE);return 0;
}
static ViaimConnectResult start(ViaimConnectSdk* s) {
    event(s,VIAIM_CONNECT_COMMAND_FUNCTION_MODE,VIAIM_CONNECT_OPERATION_RESPONSE,scenario()=="reject_start");
    if(scenario()!="one_ack")event(s,VIAIM_CONNECT_COMMAND_FLASH_STATUS,VIAIM_CONNECT_OPERATION_RESPONSE);
    s->recording=true;return 0;
}
extern "C" ViaimConnectResult __cdecl viaim_connect_start_microphone_recording(ViaimConnectSdk* s){return start(s);}
extern "C" ViaimConnectResult __cdecl viaim_connect_start_speaker_recording(ViaimConnectSdk* s){return start(s);}
extern "C" ViaimConnectResult __cdecl viaim_connect_start_call_recording(ViaimConnectSdk* s){return start(s);}
extern "C" ViaimConnectResult __cdecl viaim_connect_stop_recording(ViaimConnectSdk* s) {
    s->recording=false;
    event(s,VIAIM_CONNECT_COMMAND_FUNCTION_MODE,VIAIM_CONNECT_OPERATION_RESPONSE);
    event(s,VIAIM_CONNECT_COMMAND_FLASH_STATUS,VIAIM_CONNECT_OPERATION_RESPONSE);return 0;
}
#ifdef CONNECT_LAB_VIDEO
extern "C" ViaimConnectResult __cdecl viaim_connect_camera_enumerate(
    ViaimConnectSdk*,ViaimConnectCameraEnumeration** out) {
    *out=new ViaimConnectCameraEnumeration;
    return VIAIM_CONNECT_OK;
}
extern "C" ViaimConnectResult __cdecl viaim_connect_camera_enumeration_get_device_info(
    const ViaimConnectCameraEnumeration*,ViaimConnectCameraDeviceInfo* info) {
    info->capability_count=1;
    return VIAIM_CONNECT_OK;
}
extern "C" ViaimConnectResult __cdecl viaim_connect_camera_enumeration_get_capability(
    const ViaimConnectCameraEnumeration*,uint32_t,ViaimConnectCameraCapability* cap) {
    cap->pixel_format=VIAIM_CONNECT_CAMERA_PIXEL_FORMAT_BGR24;
    cap->min_width=cap->max_width=width;
    cap->min_height=cap->max_height=height;
    cap->min_fps=cap->max_fps=fps;
    return VIAIM_CONNECT_OK;
}
extern "C" ViaimConnectResult __cdecl viaim_connect_camera_enumeration_destroy(
    ViaimConnectCameraEnumeration* enumeration) {
    delete enumeration;
    return VIAIM_CONNECT_OK;
}
extern "C" ViaimConnectResult __cdecl viaim_connect_camera_open_with_options(
    ViaimConnectSdk* s,const ViaimConnectCameraEnumeration*,const ViaimConnectCameraOpenOptions*) {
    s->camera=true;
    return VIAIM_CONNECT_OK;
}
#endif
extern "C" ViaimConnectResult __cdecl viaim_connect_camera_close(ViaimConnectSdk* s){s->camera=false;return 0;}
extern "C" ViaimConnectResult __cdecl viaim_connect_close(ViaimConnectSdk* s) {
    s->running=false;if(s->worker.joinable())s->worker.join();
    if(s->callbacks.connection_state)s->callbacks.connection_state(0,s->callbacks.user_data);return 0;
}
extern "C" ViaimConnectResult __cdecl viaim_connect_destroy(ViaimConnectSdk* s) {
    if(s->worker.joinable())viaim_connect_close(s);delete s;return 0;
}
