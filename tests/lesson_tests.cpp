#include "LessonTask.h"
#include <iostream>
int main() {
    const bool ok=recordingSummary(0)=="mic_bytes=0; audio_seconds=0.00" &&
        recordingSummary(32000)=="mic_bytes=32000; audio_seconds=1.00" &&
        recordingSummary(48000)=="mic_bytes=48000; audio_seconds=1.50";
    if(!ok) std::cerr<<"Lesson exercise unfinished or output format incorrect\n";
    return ok?0:1;
}
