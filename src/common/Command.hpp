// Comandos de rede -> loop principal (fila thread-safe).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "Types.hpp"

namespace SwitchCast {

struct NetworkCommand {
    enum class Type {
        Play,          // retoma (unpause)
        Pause,
        Stop,
        Seek,          // absoluto, em positionMs
        SeekRelative,  // relativo, em positionMs (pode ser negativo)
        SetVolume,     // volume 0..100
        SetMute,       // flag
        LoadMedia,
        AddSubtitle,   // text = URL
        CycleAudio,
        CycleSubtitle,
    };

    Type type = Type::Pause;
    MediaPayload media;
    int64_t positionMs = 0;
    int volume = 100;
    bool flag = false;
    std::string text;
};

// A rede produz comandos (várias threads); o loop principal consome (1 thread).
// A fila é limitada para impedir que um cliente mal comportado esgote a RAM.
class CommandQueue {
public:
    bool Push(NetworkCommand cmd) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mQueue.size() >= kMaxPending) return false;
        mQueue.push_back(std::move(cmd));
        return true;
    }

    bool Pop(NetworkCommand& out) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mQueue.empty()) return false;
        out = std::move(mQueue.front());
        mQueue.pop_front();
        return true;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mMutex);
        mQueue.clear();
    }

private:
    static constexpr std::size_t kMaxPending = 64;
    std::mutex mMutex;
    std::deque<NetworkCommand> mQueue;
};

}  // namespace SwitchCast
