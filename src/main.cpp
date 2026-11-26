#include "Sdk.h"
#include "WavWriter.h"
#ifdef CONNECT_LAB_VIDEO
#include "VideoCapture.h"
#include <conio.h>
#endif
#ifdef CONNECT_LAB_VOICE
#include "VoiceSnapshot.h"
#endif
#ifdef CONNECT_LAB_TEACHER_ANSWER
#include "../answers/LessonTask.h"
#else
#include "LessonTask.h"
#endif
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <syncstream>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace std::chrono_literals;
static volatile std::sig_atomic_t interrupted=0;
static void signalHandler(int) { interrupted=1; }
static void log(const std::string& s) {
    std::osyncstream(std::cout) << s << '\n' << std::flush_emit;
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
#ifdef CONNECT_LAB_VIDEO
    std::unique_ptr<VideoCapture> video;
    int highlightButton=-1;
#endif
#ifdef CONNECT_LAB_VOICE
    std::unique_ptr<VoiceSnapshot> voice;
    bool voiceChat=false;
#endif

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
#ifdef CONNECT_LAB_VOICE
                if(!voiceChat)
#endif
                {
                    auto& file=b.mic?mic:spk;
                    if(!file) file=std::make_unique<WavWriter>(directory/(b.mic?"mic.wav":"spk.wav"));
                    file->append(b.bytes.data(),b.bytes.size());
                }
                auto& bytes=b.mic?micBytes:spkBytes;
                bytes.fetch_add(b.bytes.size());
#ifdef CONNECT_LAB_VIDEO
                if(video) {
                    (b.mic?mic:spk)->flush();
                    video->audioWritten(b.mic,bytes.load());
                }
#endif
#ifdef CONNECT_LAB_VOICE
                if(b.mic && voice) voice->submit(b.bytes);
#endif
            }
            if(mic) mic->finish();
            if(spk) spk->finish();
        } catch(const std::exception& e) { ioFailed=true; log(std::string("FILE_ERROR: ")+e.what()); }
    }
};

#ifdef CONNECT_LAB_VIDEO
static void __cdecl onCameraFrame(const ViaimConnectCameraFrame* frame,void* data) {
    if(frame) static_cast<Context*>(data)->video->receive(*frame);
}
static void __cdecl onCameraState(ViaimConnectCameraState state,void* data) {
    static_cast<Context*>(data)->video->cameraState(state);
}
#endif

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
#ifdef CONNECT_LAB_VIDEO
    constexpr auto keyFields=VIAIM_CONNECT_KEY_FIELD_KEY | VIAIM_CONNECT_KEY_FIELD_ACTION;
    if(c.video && event->command.id==VIAIM_CONNECT_COMMAND_KEY_REPORT &&
       event->command.operation==VIAIM_CONNECT_OPERATION_REPORT &&
       (event->command.present_fields & keyFields)==keyFields) {
        const auto& key=event->command.payload.key;
        log("DEVICE_KEY="+std::to_string(key.key)+" ACTION="+std::to_string(key.action));
        if(key.key==c.highlightButton && key.action==2) {
            try {
                if(!c.video->requestHighlight()) log("HIGHLIGHT_UNAVAILABLE");
            } catch(const std::exception& error) {log(std::string("HIGHLIGHT_ERROR: ")+error.what());}
        }
        return;
    }
#endif
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

static bool stopRequested(const std::filesystem::path& stopFile) {
    if (interrupted) return true;
    if (stopFile.empty()) return false;
    std::error_code error;
    return std::filesystem::exists(stopFile, error);
}

static bool waitConnected(Context& c,int seconds,const std::filesystem::path& stopFile) {
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
    while(std::chrono::steady_clock::now()<end && !stopRequested(stopFile)) {
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

#ifdef CONNECT_LAB_VIDEO
static double ringNumber(const std::string& value, double minimum, double maximum) {
    size_t used = 0;
    const auto number = std::stod(value, &used);
    if (used != value.size() || !std::isfinite(number) || number < minimum || number > maximum)
        throw std::runtime_error("Ring setting is outside its supported range: " + value);
    return number;
}
#endif

static std::string utf8Path(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

#if defined(_WIN32) && !defined(__MINGW32__)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
#ifdef CONNECT_LAB_SIMULATED
    log("SIMULATED SDK: generated test audio, not hardware evidence");
#endif
    std::string mode="mic";int seconds=10,timeout=15;unsigned long vid=0,pid=0;
    std::filesystem::path output="recordings";
    std::filesystem::path stopFile;
    bool recordVideo=false, continuous=false;
    int segmentSeconds=120, highlightButton=-1;
#ifdef CONNECT_LAB_VIDEO
    bool useRing=false, ringDemo=false, ringOnly=false, ringTuning=false, fixedDuration=false;
    ring::HighlightOptions ringOptions;
#endif
#ifdef CONNECT_LAB_VOICE
    bool voiceSnapshot=false, voiceChat=false;
#endif
    try {
        for(int i=1;i<argc;) {
            const auto key = utf8Path(argv[i++]);
            if(key=="--help") {
                std::cout<<"connect_lab --mode mic|spk|both --seconds 10 --output recordings [--vid 0x2207 --pid 0x0017] [--connect-timeout 15] [--video]\n";
                std::cout<<"With --video, press S for a snapshot or H for a highlight (20 seconds before and after).\n"
                           "Use --continuous for video until Ctrl+C, or --seconds N for a fixed duration.\n"
                           "With --stop-file PATH, creating that file stops recording and saves the WAV.\n"
                           "Video segments default to 120 seconds; set --segment-seconds N to override.\n"
                           "Use --highlight-button 0..8 to mark on a matching device single-click.\n";
#ifdef CONNECT_LAB_VIDEO
                std::cout<<"Add --ring with --video for a PPG pulse overlay (BioScope must already be streaming).\n"
                           "Use --ring-monitor for standalone terminal pulse readings; --ring-demo uses synthetic PPG.\n"
                           "Add --ring-auto-highlight to save sustained pulse changes with video.\n"
                           "Ring options: --ring-baseline auto|90|70:80, --ring-change-percent 20,\n"
                           "              --ring-hold-seconds 6, --ring-calibration-seconds 30.\n";
#endif
#ifdef CONNECT_LAB_VOICE
                std::cout<<"Add --voice-snapshot with --video and a microphone mode for offline Chinese commands.\n";
                std::cout<<"For a spoken DeepSeek conversation, launch scripts/voice_chat.py.\n";
#endif
                return 0;
            }
            if(key=="--video") {recordVideo=true;continue;}
            if(key=="--continuous") {continuous=true;continue;}
#ifdef CONNECT_LAB_VIDEO
            if(key=="--ring") {useRing=true;continue;}
            if(key=="--ring-demo") {useRing=true;ringDemo=true;continue;}
            if(key=="--ring-monitor") {useRing=true;ringOnly=true;continue;}
            if(key=="--ring-auto-highlight") {ringOptions.enabled=true;continue;}
#endif
#ifdef CONNECT_LAB_VOICE
            if(key=="--voice-chat-input") {voiceChat=true;continue;}
#endif
            if(key=="--voice-snapshot") {
#ifdef CONNECT_LAB_VOICE
                voiceSnapshot=true;
                continue;
#else
                throw std::runtime_error("This build does not include voice snapshots");
#endif
            }
            if(i>=argc) throw std::runtime_error("Missing option value");
            const std::filesystem::path argument(argv[i++]);
            const auto value = utf8Path(argument);
            size_t used=0;
            if(key=="--mode") mode=value;
            else if(key=="--output") output=argument;
            else if(key=="--stop-file") stopFile=argument;
            else if(key=="--seconds" || key=="--connect-timeout" || key=="--segment-seconds") {
                const int limit=key=="--connect-timeout"?120:key=="--segment-seconds"?3600:86400;
                int n=std::stoi(value,&used);
                if(used!=value.size() || n<1 || n>limit)
                    throw std::runtime_error(key+" must be 1.."+std::to_string(limit)+" seconds");
                (key=="--seconds"?seconds:key=="--connect-timeout"?timeout:segmentSeconds)=n;
#ifdef CONNECT_LAB_VIDEO
                if(key=="--seconds") fixedDuration=true;
#endif
            }
#ifdef CONNECT_LAB_VIDEO
            else if(key=="--ring-baseline") {
                ringTuning=true;
                if(value=="auto") ringOptions.fixedRange.reset();
                else {
                    const auto separator=value.find(':');
                    const double low=ringNumber(value.substr(0,separator),35,220);
                    const double high=separator==std::string::npos ? low :
                        ringNumber(value.substr(separator+1),low,220);
                    ringOptions.fixedRange={low,high};
                }
            } else if(key=="--ring-change-percent") {
                ringTuning=true;
                ringOptions.changeFraction=ringNumber(value,1,50)/100;
            } else if(key=="--ring-hold-seconds" || key=="--ring-calibration-seconds") {
                ringTuning=true;
                const int n=std::stoi(value,&used);
                const bool calibration=key=="--ring-calibration-seconds";
                if(used!=value.size() || n<(calibration?10:2) || n>(calibration?300:60))
                    throw std::runtime_error("Invalid value for "+key);
                (calibration?ringOptions.calibration:ringOptions.hold)=std::chrono::seconds(n);
            }
#endif
            else if(key=="--highlight-button") {
                highlightButton=std::stoi(value,&used);
                if(used!=value.size() || highlightButton<0 || highlightButton>8)
                    throw std::runtime_error("--highlight-button must be 0..8");
            } else if(key=="--vid" || key=="--pid") {
                auto n=std::stoul(value,&used,0);if(used!=value.size() || n==0 || n>65535) throw std::runtime_error("VID/PID must be 1..65535");
                (key=="--vid"?vid:pid)=n;
            } else throw std::runtime_error("Unknown option: "+key);
        }
        if(mode!="mic" && mode!="spk" && mode!="both") throw std::runtime_error("Invalid mode");
        if(bool(vid)!=bool(pid)) throw std::runtime_error("Set both --vid and --pid");
        if(!recordVideo && (continuous || segmentSeconds!=120 || highlightButton>=0 || seconds>120))
            throw std::runtime_error("Long recording, continuous mode and highlight options require --video");
#ifdef CONNECT_LAB_VIDEO
        if(ringOnly && (recordVideo || ringOptions.enabled))
            throw std::runtime_error("--ring-monitor is a standalone command");
        if(useRing && !recordVideo && !ringOnly)
            throw std::runtime_error("Ring overlay requires --video; use --ring-monitor for terminal readings");
        if((ringOptions.enabled && !(useRing && recordVideo)) || (ringTuning && !ringOptions.enabled))
            throw std::runtime_error("Ring highlight settings require --video --ring --ring-auto-highlight");
#endif
#ifdef CONNECT_LAB_VOICE
        if(voiceSnapshot && (!recordVideo || mode=="spk"))
            throw std::runtime_error("--voice-snapshot requires --video and microphone audio");
        if(voiceChat && (recordVideo || voiceSnapshot || mode!="mic" ||
            GetFileType(GetStdHandle(STD_INPUT_HANDLE))!=FILE_TYPE_PIPE))
            throw std::runtime_error("Use scripts/voice_chat.py for standalone microphone conversation");
#endif
#ifndef CONNECT_LAB_VIDEO
        if(recordVideo) throw std::runtime_error("--video is available only in the Windows hardware build");
#endif
    } catch(const std::exception& e) {log(e.what());return 2;}
    std::signal(SIGINT,signalHandler);
#ifdef CONNECT_LAB_VIDEO
    if(ringOnly) {
        try {
            log("Reading PPG pulse; Ctrl+C stops monitoring");
            ring::Monitor monitor(ringDemo, {}, [](ring::Reading) {}, [](ring::Episode) {});
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
            while(!interrupted && (!fixedDuration || std::chrono::steady_clock::now()<end))
                std::this_thread::sleep_for(100ms);
            return 0;
        } catch(const std::exception& e) {log(e.what());return 2;}
    }
#endif
    Context c;
#ifdef CONNECT_LAB_VIDEO
    std::unique_ptr<ring::Monitor> ringMonitor;
#endif
#ifdef CONNECT_LAB_VOICE
    c.voiceChat=voiceChat;
#endif
    // Unique session directory; never silently overwrite an earlier recording.
    auto stamp=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    c.directory=output/("session-"+std::to_string(stamp));
    try {
#ifdef CONNECT_LAB_VOICE
        if(!voiceChat)
#endif
        {
            if(!std::filesystem::create_directories(c.directory)) throw std::runtime_error("Session directory already exists");
        }
#ifdef CONNECT_LAB_VIDEO
        if(recordVideo)
            c.video=std::make_unique<VideoCapture>(c.directory,mode!="spk",segmentSeconds,useRing,ringDemo);
        c.highlightButton=highlightButton;
#endif
#ifdef CONNECT_LAB_VOICE
        if(voiceSnapshot || voiceChat) {
            log("Loading local voice recognition model");
            const auto voiceLog=+[](const std::string& message) { log(message); };
            c.voice=voiceChat ? std::make_unique<VoiceSnapshot>(voiceLog)
                              : std::make_unique<VoiceSnapshot>(*c.video,voiceLog);
            c.voice->start();
        }
#endif
    }
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
#ifdef CONNECT_LAB_VIDEO
    if(c.video) {
        callbacks.camera_frame=onCameraFrame;
        callbacks.camera_state=onCameraState;
    }
#endif
    ViaimConnectOpenOptions options=VIAIM_CONNECT_OPEN_OPTIONS_INIT;
    if(vid) {options.vendor_id=static_cast<uint16_t>(vid);options.product_id=static_cast<uint16_t>(pid);}
    bool opened=false,startAttempted=false,recorded=false,stopConfirmed=false;
    try {
        result=viaim_connect_open(sdk,&options,&callbacks);opened=result==VIAIM_CONNECT_OK;
        if(!opened) throw std::runtime_error("OPEN_ERROR="+std::to_string(result));
        if(!waitConnected(c,timeout,stopFile)) throw std::runtime_error("Connection timeout or cancelled; verify USB and VID/PID");
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
        if(stopRequested(stopFile)) throw std::runtime_error("Recording cancelled before start");
        queryPcVolume(sdk,c);
#ifdef CONNECT_LAB_VIDEO
        if(c.video && !stopRequested(stopFile)) c.video->open(sdk);
#endif
        if(stopRequested(stopFile)) throw std::runtime_error("Recording cancelled before start");
        startAttempted=true;
        if(!recordingCommand(sdk,c,mode,false)) throw std::runtime_error("Recording start not confirmed");
#ifdef CONNECT_LAB_VIDEO
        if(c.video) {
            c.video->start();
            log("Video recording active; press S for a snapshot or H for a highlight");
            if(useRing) {
                auto* video=c.video.get();
                ringMonitor=std::make_unique<ring::Monitor>(ringDemo,ringOptions,
                    [video](ring::Reading reading) { video->ringReading(reading); },
                    [video](ring::Episode episode) {
                        if(!video->requestRingHighlight(episode)) log("RING_HIGHLIGHT_UNAVAILABLE");
                    });
            }
        }
#endif
#ifdef CONNECT_LAB_VOICE
        if(voiceChat) log("VOICE_CHAT_READY");
        else if(c.voice) log("Voice commands active; say 拍张照片 or 保存精彩片段, then pause");
#endif
        c.acceptAudio=true;
        log("Recording active; Ctrl+C stops early");
        auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
        auto next=std::chrono::steady_clock::now()+1s;
        while(!stopRequested(stopFile) && !c.ioFailed) {
#ifdef CONNECT_LAB_VOICE
            if(voiceChat) {
                const HANDLE input=GetStdHandle(STD_INPUT_HANDLE);
                DWORD available=0, received=0;
                char command=0;
                if(!PeekNamedPipe(input,nullptr,0,nullptr,&available,nullptr) ||
                    (available && (!ReadFile(input,&command,1,&received,nullptr) || command=='Q'))) break;
                if(command=='R') c.voice->resume();
            } else
#endif
            if(!continuous && std::chrono::steady_clock::now()>=end) break;
            if(c.state!=VIAIM_CONNECT_CONNECTION_CONNECTED || !c.acceptAudio)
                throw std::runtime_error("Device disconnected; recording is incomplete");
#ifdef CONNECT_LAB_VIDEO
            if(c.video && c.video->failed()) throw std::runtime_error("Video capture stopped working");
            if(c.video && _kbhit()) {
                const int key=_getch();
                if(key==0 || key==224) (void)_getch();
                else if(key=='s' || key=='S')
                    log(c.video->requestSnapshot()?"SNAPSHOT_REQUESTED":"SNAPSHOT_UNAVAILABLE");
                else if(key=='h' || key=='H') {
                    if(!c.video->requestHighlight()) log("HIGHLIGHT_UNAVAILABLE");
                }
            }
#endif
            if(std::chrono::steady_clock::now()>=next) {
#ifdef CONNECT_LAB_VOICE
                if(!voiceChat)
#endif
                    log("MIC_BYTES="+std::to_string(c.micBytes)+" SPK_BYTES="+std::to_string(c.spkBytes));
                next+=1s;
            }
            std::this_thread::sleep_for(50ms);
        }
        recorded=!c.ioFailed;
    } catch(const std::exception& e) {log(std::string("LAB_ERROR: ")+e.what());}
#ifdef CONNECT_LAB_VIDEO
    if(ringMonitor) ringMonitor->stop();
    if(c.video) c.video->stop();
#endif
#ifdef CONNECT_LAB_VOICE
    if(c.voice) c.voice->stop();
#endif
    if(startAttempted && c.state==VIAIM_CONNECT_CONNECTION_CONNECTED)
        stopConfirmed=recordingCommand(sdk,c,mode,true);
    c.acceptAudio=false;
    // Closing the camera is safe when it was not opened.
    auto cameraResult=viaim_connect_camera_close(sdk);
    auto closeResult=opened?viaim_connect_close(sdk):VIAIM_CONNECT_OK;
    auto destroyResult=viaim_connect_destroy(sdk);sdk=nullptr;
    {std::lock_guard<std::mutex> lock(c.mutex);c.quitWriter=true;}c.cv.notify_all();writer.join();
    bool videoOk=true;
#ifdef CONNECT_LAB_VIDEO
    if(c.video) videoOk=c.video->finish();
#endif
    log("CLEANUP camera="+std::to_string(cameraResult)+" close="+std::to_string(closeResult)+" destroy="+std::to_string(destroyResult));
    log("MIC_BYTES="+std::to_string(c.micBytes)+" SPK_BYTES="+std::to_string(c.spkBytes)+
        " DROPPED="+std::to_string(c.dropped)+" REJECTED="+std::to_string(c.rejected));
    log(recordingSummary(c.micBytes));
#ifdef CONNECT_LAB_VOICE
    if(!voiceChat)
#endif
        log("OUTPUT="+utf8Path(c.directory));
    bool hasAudio=(mode=="mic"?c.micBytes>0:mode=="spk"?c.spkBytes>0:c.micBytes>0 && c.spkBytes>0);
    bool ok=recorded && stopConfirmed && hasAudio && !c.ioFailed && c.dropped==0 && c.rejected==0 &&
        cameraResult==VIAIM_CONNECT_OK && closeResult==VIAIM_CONNECT_OK && destroyResult==VIAIM_CONNECT_OK && videoOk;
    log(ok?"LAB_RESULT=PASS":"LAB_RESULT=INCOMPLETE");
    return ok?0:1;
}
