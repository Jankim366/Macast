#pragma once
#include <cstdint>
namespace ap::airplay {
enum class MediaCommand {
    Play,
    Pause,
    PlayPause,
    Previous,
    Next,
    VolumeUp,
    VolumeDown,
    MuteToggle,
    Seek,
    Preempt,
};
}
