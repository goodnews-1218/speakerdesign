// Test-only C ABI stand-in. No USB access and no vendor DLL is loaded.
#include "Sdk.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <string>
#include <vector>
struct ViaimConnectSdk {
    ViaimConnectCallbacks callbacks=VIAIM_CONNECT_CALLBACKS_INIT;
    std::atomic<bool> running{false}, recording{false};
    std::thread worker;
};
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
        while(s->running) {
            std::this_thread::sleep_for(50ms);
            if(!s->recording || scenario()=="no_audio")continue;
            if(scenario()=="disconnect" && sequence==4) {
                if(s->callbacks.connection_state)s->callbacks.connection_state(3,s->callbacks.user_data);
                s->recording=false;continue;
            }
            std::vector<uint8_t> bytes(1600,0x12);
            for(int source=0;source<2;source++) {
                ViaimConnectAudioPcmFrame f={};f.struct_size=sizeof(f);f.source=source;f.frame_sequence=sequence;
                f.sample_rate=scenario()=="wrong_format"?48000:16000;f.channel_count=1;f.bits_per_sample=16;
                f.sample_format=VIAIM_CONNECT_PCM_S16_LE;f.sample_count_per_channel=800;f.data=bytes.data();f.size=bytes.size();
                if(s->callbacks.audio_pcm)s->callbacks.audio_pcm(&f,s->callbacks.user_data);
            }
            // The callback must already have copied the borrowed memory.
            std::fill(bytes.begin(),bytes.end(),0xEE);++sequence;
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
extern "C" ViaimConnectResult __cdecl viaim_connect_camera_close(ViaimConnectSdk*){return 0;}
extern "C" ViaimConnectResult __cdecl viaim_connect_close(ViaimConnectSdk* s) {
    s->running=false;if(s->worker.joinable())s->worker.join();
    if(s->callbacks.connection_state)s->callbacks.connection_state(0,s->callbacks.user_data);return 0;
}
extern "C" ViaimConnectResult __cdecl viaim_connect_destroy(ViaimConnectSdk* s) {
    if(s->worker.joinable())viaim_connect_close(s);delete s;return 0;
}
