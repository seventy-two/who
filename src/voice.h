// Playing voice messages: WhatsApp's are Ogg Opus, decoded whole with
// libopus and played through miniaudio.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace who {

// DecodeOggOpus reads an Ogg Opus file into 48 kHz mono samples.
bool DecodeOggOpus(const std::string& bytes, std::vector<float>& mono, std::string& error);

class VoicePlayer {
public:
    VoicePlayer();
    ~VoicePlayer();

    // Play starts a file (decoding it first); key says what's playing.
    bool Play(const std::string& key, const std::string& path, std::string& error);
    void Pause(bool paused);
    void Stop();
    void Seek(double fraction);

    const std::string& Key() const { return key_; }
    bool Playing() const;  // started and not paused or finished
    bool Paused() const;
    double Position() const;  // seconds
    double Duration() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string key_;
};

}  // namespace who
