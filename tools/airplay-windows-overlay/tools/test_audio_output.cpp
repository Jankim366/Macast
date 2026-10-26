#include "audio/audio_output.h"
#include "log.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    ap::set_log_enabled(true);
    ap::audio::SdlAudioOutput output;
    if (!output.start(44100, 2)) {
        std::cerr << "FAIL: SDL audio output could not be opened\n";
        return 1;
    }

    // Exercise the complete queue path without making a test tone audible.
    std::vector<int16_t> silence(4410 * 2, 0);
    output.push(silence.data(), static_cast<int>(silence.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    output.clear();
    output.stop();
    std::cout << "PASS: SDL output opened and accepted queued PCM\n";
    return 0;
}
