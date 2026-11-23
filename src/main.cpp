#include "Sdk.h"
#include "WavWriter.h"
#ifdef CONNECT_LAB_TEACHER_ANSWER
#include "../answers/LessonTask.h"
#else
#include "LessonTask.h"
#endif
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace std::chrono_literals;
static volatile std::sig_atomic_t interrupted=0;
static void signalHandler(int) { interrupted=1; }
static std::mutex consoleMutex;
static void log(const std::string& s) {
    std::lock_guard<std::mutex> lock(consoleMutex); std::cout << s << std::endl;
}

struct Block { bool mic; std::vector<uint8_t> bytes; };
struct Context {
    std::atomic<int> state{VIAIM_CONNECT_CONNECTION_CLOSED};
    std::atomic<bool> acceptAudio{false}, ioFailed{false};
    std::atomic<uint64_t> micBytes{0}, spkBytes{0}, dropped{0}, rejected{0};
    std::mutex mutex;
    std::condition_variable cv;
    bool awaiting=false, modeAck=false, flashAck=false, ackFailed=false;
    bool queryPending=false, queryOk=false;
    bool volumeQueryPending=false;
    bool quitWriter=false;
    size_t queuedBytes=0;
    std::deque<Block> blocks;
    std::filesystem::path directory;

    void writer() noexcept {
        try {
            std::unique_ptr<WavWriter> mic,spk;
            for(;;) {
                Block b;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock,[&]{return quitWriter || !blocks.empty();});
                    if(blocks.empty() && quitWriter) break;
                    b=std::move(blocks.front()); blocks.pop_front(); queuedBytes-=b.bytes.size();
                }
                auto& file=b.mic?mic:spk;
                if(!file) file=std::make_unique<WavWriter>(directory/(b.mic?"mic.wav":"spk.wav"));
                file->append(b.bytes.data(),b.bytes.size());
                (b.mic?micBytes:spkBytes).fetch_add(b.bytes.size());
            }
            if(mic) mic->finish();
            if(spk) spk->finish();
        } catch(const std::exception& e) { ioFailed=true; log(std::string("FILE_ERROR: ")+e.what()); }
    }
};

static void __cdecl onConnection(ViaimConnectConnectionState state,void* data) {
    auto& c=*static_cast<Context*>(data); c.state=state;
    // A disconnected session is not resumed automatically into the same recording.
    if(state!=VIAIM_CONNECT_CONNECTION_CONNECTED) c.acceptAudio=false;
    c.cv.notify_all();
    log("CONNECTION_STATE="+std::to_string(state));
}
static void __cdecl onError(ViaimConnectResult code,const char* message,void*) {
    // Avoid raw SDK messages: they can contain device identifiers or private paths.
    (void)message; log("SDK_ERROR="+std::to_string(code));
}
static void __cdecl onEvent(const ViaimConnectEvent* event,void* data) {
    if(!event) return;
    auto& c=*static_cast<Context*>(data);
    std::lock_guard<std::mutex> lock(c.mutex);
    if(c.queryPending && event->command.id==VIAIM_CONNECT_COMMAND_FUNCTION_MODE &&
       event->command.operation==VIAIM_CONNECT_OPERATION_RESPONSE) {
        c.queryPending=false; c.queryOk=event->protocol_result==0;
        log("QUERY_DEVICE_RESULT="+std::to_string(event->protocol_result));
    }
    if(c.volumeQueryPending && event->command.id==VIAIM_CONNECT_COMMAND_QUERY_VOLUME &&
       event->command.operation==VIAIM_CONNECT_OPERATION_RESPONSE) {
        c.volumeQueryPending=false;
        if(event->protocol_result!=0) {
            log("PC_VOLUME_QUERY_ERROR="+std::to_string(event->protocol_result));
        } else if(event->command.present_fields & VIAIM_CONNECT_VOLUME_FIELD_PC) {
            log("PC_VOLUME="+std::to_string(event->command.payload.volume.pc));
        } else {
            log("PC_VOLUME_UNAVAILABLE: response omitted PC volume");
        }
    }
    if(c.awaiting && event->command.operation==VIAIM_CONNECT_OPERATION_RESPONSE) {
        bool relevant=false;
        if(event->command.id==VIAIM_CONNECT_COMMAND_FUNCTION_MODE) {c.modeAck=true;relevant=true;}
        if(event->command.id==VIAIM_CONNECT_COMMAND_FLASH_STATUS) {c.flashAck=true;relevant=true;}
        if(relevant) {
            if(event->protocol_result!=0) c.ackFailed=true;
            log("RECORD_ACK command="+std::to_string(event->command.id)+
                " result="+std::to_string(event->protocol_result));
        }
    }
    c.cv.notify_all();
}
static void __cdecl onPcm(const ViaimConnectAudioPcmFrame* frame,void* data) {
    auto& c=*static_cast<Context*>(data);
    if(!c.acceptAudio || !frame || !frame->size) return;
    if(frame->sample_rate!=16000 || frame->channel_count!=1 || frame->bits_per_sample!=16 ||
       frame->sample_format!=VIAIM_CONNECT_PCM_S16_LE || !frame->data || frame->size%2 ||
       (frame->source!=VIAIM_CONNECT_AUDIO_SOURCE_MICROPHONE && frame->source!=VIAIM_CONNECT_AUDIO_SOURCE_SPEAKER)) {
        ++c.rejected; return;
    }
    try {
        std::lock_guard<std::mutex> lock(c.mutex);
        if(!c.acceptAudio) return;
        if(frame->size>2*1024*1024 || c.queuedBytes+frame->size>2*1024*1024) {++c.dropped;return;}
        // Borrowed SDK bytes are copied before the callback returns.
        c.blocks.push_back({frame->source==VIAIM_CONNECT_AUDIO_SOURCE_MICROPHONE,
                           std::vector<uint8_t>(frame->data,frame->data+frame->size)});
        c.queuedBytes+=frame->size;
        c.cv.notify_all();
    } catch(...) {c.ioFailed=true;}
}

static bool waitConnected(Context& c,int seconds) {
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
    while(std::chrono::steady_clock::now()<end && !interrupted) {
        if(c.state==VIAIM_CONNECT_CONNECTION_CONNECTED) return true;
        std::this_thread::sleep_for(50ms);
    }
    return false;
}
static void queryPcVolume(ViaimConnectSdk* sdk,Context& c) {
    ViaimConnectRequest request=VIAIM_CONNECT_REQUEST_INIT;
    request.command.id=VIAIM_CONNECT_COMMAND_QUERY_VOLUME;
    request.command.operation=VIAIM_CONNECT_OPERATION_QUERY;
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        c.volumeQueryPending=true;
    }
    const auto result=viaim_connect_send(sdk,&request);
    std::unique_lock<std::mutex> lock(c.mutex);
    if(result!=VIAIM_CONNECT_OK) {
        c.volumeQueryPending=false;
        log("PC_VOLUME_QUEUE_ERROR="+std::to_string(result));
    } else if(!c.cv.wait_for(lock,3s,[&]{return !c.volumeQueryPending;})) {
        c.volumeQueryPending=false;
        log("PC_VOLUME_QUERY_TIMEOUT");
    }
}
static bool recordingCommand(ViaimConnectSdk* sdk,Context& c,const std::string& mode,bool stop) {
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        c.awaiting=true; c.modeAck=false; c.flashAck=false; c.ackFailed=false;
    }
    ViaimConnectResult result=stop?viaim_connect_stop_recording(sdk):
        mode=="mic"?viaim_connect_start_microphone_recording(sdk):
        mode=="spk"?viaim_connect_start_speaker_recording(sdk):viaim_connect_start_call_recording(sdk);
    log(std::string(stop?"STOP_QUEUED=":"START_QUEUED=")+std::to_string(result));
    std::unique_lock<std::mutex> lock(c.mutex);
    if(result!=VIAIM_CONNECT_OK) {c.awaiting=false;return false;}
    // One recording batch at a time; each batch needs two device responses.
    bool ready=c.cv.wait_for(lock,3s,[&]{return c.ackFailed || (c.modeAck && c.flashAck) ||
        c.state!=VIAIM_CONNECT_CONNECTION_CONNECTED;});
    bool ok=ready && c.modeAck && c.flashAck && !c.ackFailed &&
        c.state==VIAIM_CONNECT_CONNECTION_CONNECTED;
    c.awaiting=false;
    log(std::string(stop?"STOP_CONFIRMED=":"START_CONFIRMED=")+(ok?"yes":"no"));
    return ok;
}

int main(int argc,char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
#ifdef CONNECT_LAB_SIMULATED
    log("SIMULATED SDK: generated test audio, not hardware evidence");
#endif
    std::string mode="mic";int seconds=10,timeout=15;unsigned long vid=0,pid=0;
    std::filesystem::path output="recordings";
    try {
        for(int i=1;i<argc;i+=2) {
            std::string key=argv[i];
            if(key=="--help") {
                std::cout<<"connect_lab --mode mic|spk|both --seconds 10 --output recordings [--vid 0x2207 --pid 0x0017] [--connect-timeout 15]\n";return 0;
            }
            if(i+1>=argc) throw std::runtime_error("Missing option value");
            std::string value=argv[i+1];size_t used=0;
            if(key=="--mode") mode=value;
            else if(key=="--output") output=value;
            else if(key=="--seconds" || key=="--connect-timeout") {
                int n=std::stoi(value,&used);if(used!=value.size() || n<1 || n>120) throw std::runtime_error("Duration must be 1..120 seconds");
                (key=="--seconds"?seconds:timeout)=n;
            } else if(key=="--vid" || key=="--pid") {
                auto n=std::stoul(value,&used,0);if(used!=value.size() || n==0 || n>65535) throw std::runtime_error("VID/PID must be 1..65535");
                (key=="--vid"?vid:pid)=n;
            } else throw std::runtime_error("Unknown option: "+key);
        }
        if(mode!="mic" && mode!="spk" && mode!="both") throw std::runtime_error("Invalid mode");
        if(bool(vid)!=bool(pid)) throw std::runtime_error("Set both --vid and --pid");
    } catch(const std::exception& e) {log(e.what());return 2;}
    std::signal(SIGINT,signalHandler);
    Context c;
    // Unique session directory; never silently overwrite an earlier recording.
    auto stamp=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    c.directory=output/("session-"+std::to_string(stamp));
    try {if(!std::filesystem::create_directories(c.directory)) throw std::runtime_error("Session directory already exists");}
    catch(const std::exception& e) {log(std::string("OUTPUT_ERROR: ")+e.what());return 2;}
    ViaimConnectSdk* sdk=nullptr;
    auto result=viaim_connect_create(nullptr,&sdk);
    if(result!=VIAIM_CONNECT_OK) {log("CREATE_ERROR="+std::to_string(result));return 2;}
    // The writer outlives SDK callbacks and drains only after SDK close/destroy.
    std::thread writer;
    try {writer=std::thread([&]{c.writer();});}
    catch(...) {viaim_connect_destroy(sdk);log("Cannot start writer thread");return 2;}
    viaim_connect_set_log_level(sdk,VIAIM_CONNECT_LOG_OFF);
    viaim_connect_set_raw_log_flags(sdk,VIAIM_CONNECT_RAW_LOG_NONE);
    ViaimConnectCallbacks callbacks=VIAIM_CONNECT_CALLBACKS_INIT;
    callbacks.connection_state=onConnection;callbacks.error=onError;callbacks.event=onEvent;
    callbacks.audio_pcm=onPcm;callbacks.user_data=&c;
    ViaimConnectOpenOptions options=VIAIM_CONNECT_OPEN_OPTIONS_INIT;
    if(vid) {options.vendor_id=static_cast<uint16_t>(vid);options.product_id=static_cast<uint16_t>(pid);}
    bool opened=false,startAttempted=false,recorded=false,stopConfirmed=false;
    try {
        result=viaim_connect_open(sdk,&options,&callbacks);opened=result==VIAIM_CONNECT_OK;
        if(!opened) throw std::runtime_error("OPEN_ERROR="+std::to_string(result));
        if(!waitConnected(c,timeout)) throw std::runtime_error("Connection timeout or cancelled; verify USB and VID/PID");
        ViaimConnectRequest request=VIAIM_CONNECT_REQUEST_INIT;
        request.command.id=VIAIM_CONNECT_COMMAND_FUNCTION_MODE;
        {std::lock_guard<std::mutex> lock(c.mutex);c.queryPending=true;}
        result=viaim_connect_send(sdk,&request);
        if(result!=VIAIM_CONNECT_OK) throw std::runtime_error("QUERY_QUEUE_ERROR="+std::to_string(result));
        {
            std::unique_lock<std::mutex> lock(c.mutex);
            if(!c.cv.wait_for(lock,3s,[&]{return !c.queryPending;}) || !c.queryOk)
                throw std::runtime_error("Device query failed or timed out");
        }
        queryPcVolume(sdk,c);
        if(interrupted) throw std::runtime_error("Recording cancelled before start");
        startAttempted=true;
        if(!recordingCommand(sdk,c,mode,false)) throw std::runtime_error("Recording start not confirmed");
        c.acceptAudio=true;
        log("Recording active; Ctrl+C stops early");
        auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
        auto next=std::chrono::steady_clock::now()+1s;
        while(std::chrono::steady_clock::now()<end && !interrupted && !c.ioFailed) {
            if(c.state!=VIAIM_CONNECT_CONNECTION_CONNECTED || !c.acceptAudio)
                throw std::runtime_error("Device disconnected; recording is incomplete");
            if(std::chrono::steady_clock::now()>=next) {
                log("MIC_BYTES="+std::to_string(c.micBytes)+" SPK_BYTES="+std::to_string(c.spkBytes));next+=1s;
            }
            std::this_thread::sleep_for(50ms);
        }
        recorded=!c.ioFailed;
    } catch(const std::exception& e) {log(std::string("LAB_ERROR: ")+e.what());}
    if(startAttempted && c.state==VIAIM_CONNECT_CONNECTION_CONNECTED)
        stopConfirmed=recordingCommand(sdk,c,mode,true);
    c.acceptAudio=false;
    // No camera is opened in the basic lab. Closing it is safe and idempotent.
    auto cameraResult=viaim_connect_camera_close(sdk);
    auto closeResult=opened?viaim_connect_close(sdk):VIAIM_CONNECT_OK;
    auto destroyResult=viaim_connect_destroy(sdk);sdk=nullptr;
    {std::lock_guard<std::mutex> lock(c.mutex);c.quitWriter=true;}c.cv.notify_all();writer.join();
    log("CLEANUP camera="+std::to_string(cameraResult)+" close="+std::to_string(closeResult)+" destroy="+std::to_string(destroyResult));
    log("MIC_BYTES="+std::to_string(c.micBytes)+" SPK_BYTES="+std::to_string(c.spkBytes)+
        " DROPPED="+std::to_string(c.dropped)+" REJECTED="+std::to_string(c.rejected));
    log(recordingSummary(c.micBytes));
    log("OUTPUT="+c.directory.string());
    bool hasAudio=(mode=="mic"?c.micBytes>0:mode=="spk"?c.spkBytes>0:c.micBytes>0 && c.spkBytes>0);
    bool ok=recorded && stopConfirmed && hasAudio && !c.ioFailed && c.dropped==0 && c.rejected==0 &&
        cameraResult==VIAIM_CONNECT_OK && closeResult==VIAIM_CONNECT_OK && destroyResult==VIAIM_CONNECT_OK;
    log(ok?"LAB_RESULT=PASS":"LAB_RESULT=INCOMPLETE");
    return ok?0:1;
}
