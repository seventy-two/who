#include "voice.h"

#include <opus/opus.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>

#include "miniaudio/miniaudio.h"

namespace who {

bool DecodeOggOpus(const std::string& b, std::vector<float>& mono, std::string& error) {
    mono.clear();
    // Ogg pages carry segments; a packet is segments up to one under 255.
    std::vector<std::string> packets;
    std::string cur;
    size_t i = 0;
    while (i + 27 <= b.size()) {
        if (b.compare(i, 4, "OggS") != 0) {
            error = "not an Ogg file";
            return false;
        }
        int segs = (unsigned char)b[i + 26];
        if (i + 27 + segs > b.size()) break;
        const unsigned char* lace = (const unsigned char*)b.data() + i + 27;
        size_t body = i + 27 + segs;
        for (int s = 0; s < segs; s++) {
            if (body + lace[s] > b.size()) break;
            cur.append(b, body, lace[s]);
            body += lace[s];
            if (lace[s] < 255) {
                packets.push_back(std::move(cur));
                cur.clear();
            }
        }
        i = body;
    }
    if (packets.size() < 2 || packets[0].compare(0, 8, "OpusHead") != 0 || packets[0].size() < 19) {
        error = "not an Opus voice message";
        return false;
    }
    int channels = (unsigned char)packets[0][9];
    int preSkip = (unsigned char)packets[0][10] | ((unsigned char)packets[0][11] << 8);
    if (channels < 1 || channels > 2) {
        error = "unexpected channel count";
        return false;
    }
    int err = 0;
    OpusDecoder* dec = opus_decoder_create(48000, channels, &err);
    if (!dec || err != OPUS_OK) {
        error = "couldn't start the Opus decoder";
        return false;
    }
    std::vector<float> frame(5760 * channels);
    for (size_t p = 2; p < packets.size(); p++) {  // after OpusHead and OpusTags
        int n = opus_decode_float(dec, (const unsigned char*)packets[p].data(), opus_int32(packets[p].size()),
                                  frame.data(), 5760, 0);
        if (n <= 0) continue;
        for (int k = 0; k < n; k++) {
            float v = frame[k * channels];
            if (channels == 2) v = 0.5f * (v + frame[k * channels + 1]);
            mono.push_back(v);
        }
    }
    opus_decoder_destroy(dec);
    if (preSkip > 0 && size_t(preSkip) < mono.size()) mono.erase(mono.begin(), mono.begin() + preSkip);
    if (mono.empty()) {
        error = "the voice message was empty";
        return false;
    }
    return true;
}

struct VoicePlayer::Impl {
    ma_device device{};
    bool open = false;
    std::vector<float> samples;
    std::atomic<size_t> pos{0};
    std::atomic<bool> paused{false};

    static void callback(ma_device* d, void* out, const void*, ma_uint32 frames) {
        auto* self = static_cast<Impl*>(d->pUserData);
        float* o = static_cast<float*>(out);
        size_t p = self->pos;
        for (ma_uint32 f = 0; f < frames; f++) {
            float v = 0;
            if (!self->paused && p < self->samples.size()) v = self->samples[p++];
            o[f * 2] = o[f * 2 + 1] = v;
        }
        self->pos = p;
    }

    void close() {
        if (open) ma_device_uninit(&device);
        open = false;
    }
};

VoicePlayer::VoicePlayer() : impl_(std::make_unique<Impl>()) {}
VoicePlayer::~VoicePlayer() { impl_->close(); }

bool VoicePlayer::Play(const std::string& key, const std::string& path, std::string& error) {
    Stop();
    std::ifstream f(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)), {});
    std::vector<float> mono;
    if (!DecodeOggOpus(bytes, mono, error)) return false;
    impl_->samples = std::move(mono);
    impl_->pos = 0;
    impl_->paused = false;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = 48000;
    cfg.dataCallback = Impl::callback;
    cfg.pUserData = impl_.get();
    if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
        error = "no audio output";
        return false;
    }
    impl_->open = true;
    ma_device_start(&impl_->device);
    key_ = key;
    return true;
}

void VoicePlayer::Pause(bool paused) { impl_->paused = paused; }

void VoicePlayer::Stop() {
    impl_->close();
    impl_->samples.clear();
    impl_->pos = 0;
    key_.clear();
}

void VoicePlayer::Seek(double fraction) {
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    impl_->pos = size_t(fraction * impl_->samples.size());
}

bool VoicePlayer::Playing() const { return impl_->open && !impl_->paused && impl_->pos < impl_->samples.size(); }
bool VoicePlayer::Paused() const { return impl_->open && impl_->paused; }
double VoicePlayer::Position() const { return impl_->pos / 48000.0; }
double VoicePlayer::Duration() const { return impl_->samples.size() / 48000.0; }

}  // namespace who
