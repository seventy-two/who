#include "images.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <iterator>

#include "gif.h"
#include "net.h"

using Microsoft::WRL::ComPtr;

namespace who {

namespace {

// Thumbnails are scaled down to at most this wide when decoded.
constexpr UINT kMaxWidth = 480;
constexpr size_t kMaxEntries = 200;
// Animations are held as a texture per frame, so they're kept to a budget:
// each is scaled down to fit kAnimBytes, and the least recently played go
// once all of them pass kAnimsBytes.
constexpr size_t kAnimBytes = 96u << 20;
constexpr size_t kAnimsBytes = 384u << 20;
constexpr UINT kMaxFrames = 1000;

bool decode(IWICImagingFactory* wic, const std::string& bytes, UINT& w, UINT& h, std::vector<unsigned char>& rgba,
            UINT maxWidth = kMaxWidth) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory((BYTE*)bytes.data(), DWORD(bytes.size()))) ||
        FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0)
        return false;

    ComPtr<IWICBitmapSource> source = frame;
    if (w > maxWidth) {
        ComPtr<IWICBitmapScaler> scaler;
        UINT sh = std::max<UINT>(1, h * maxWidth / w);
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.Get(), maxWidth, sh, WICBitmapInterpolationModeFant)))
            return false;
        source = scaler;
        w = maxWidth;
        h = sh;
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(source.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    rgba.resize(size_t(w) * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(rgba.size()), rgba.data()));
}

// number reads a whole number from a GIF's metadata, or gives fallback.
int number(IWICMetadataQueryReader* q, const wchar_t* name, int fallback) {
    if (!q) return fallback;
    PROPVARIANT v;
    PropVariantInit(&v);
    int n = fallback;
    if (SUCCEEDED(q->GetMetadataByName(name, &v))) {
        if (v.vt == VT_UI1) n = v.bVal;
        else if (v.vt == VT_UI2) n = v.uiVal;
        else if (v.vt == VT_UI4) n = int(v.ulVal);
    }
    PropVariantClear(&v);
    return n;
}

bool rgbaOf(IWICImagingFactory* wic, IWICBitmapSource* src, UINT w, UINT h, std::vector<unsigned char>& rgba) {
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    rgba.resize(size_t(w) * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(rgba.size()), rgba.data()));
}

// decodeAnimation decodes every frame of a GIF, put together into whole
// pictures and scaled to fit. Other images give their one frame.
bool decodeAnimation(IWICImagingFactory* wic, const std::string& bytes, UINT& w, UINT& h,
                     std::vector<std::vector<unsigned char>>& frames, std::vector<int>& delays) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    GUID format{};
    UINT count = 0;
    if (FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory((BYTE*)bytes.data(), DWORD(bytes.size()))) ||
        FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetContainerFormat(&format)) || FAILED(decoder->GetFrameCount(&count)) || count == 0)
        return false;
    if (format != GUID_ContainerFormatGif || count == 1) {
        frames.resize(1);
        delays = {100};
        return decode(wic, bytes, w, h, frames[0]);
    }
    count = std::min(count, kMaxFrames);

    ComPtr<IWICMetadataQueryReader> global;
    decoder->GetMetadataQueryReader(&global);
    ComPtr<IWICBitmapFrameDecode> first;
    UINT fw = 0, fh = 0;
    if (FAILED(decoder->GetFrame(0, &first)) || FAILED(first->GetSize(&fw, &fh))) return false;
    int cw = number(global.Get(), L"/logscrdesc/Width", 0), ch = number(global.Get(), L"/logscrdesc/Height", 0);
    if (cw <= 0 || ch <= 0) {
        cw = int(fw);
        ch = int(fh);
    }
    if (cw <= 0 || ch <= 0) return false;

    // The size to keep: no wider than thumbnails, and all frames in budget.
    double s = std::min(1.0, double(kMaxWidth) / cw);
    s = std::min(s, std::sqrt(double(kAnimBytes) / (double(count) * cw * ch * 4)));
    w = std::max<UINT>(1, UINT(cw * s));
    h = std::max<UINT>(1, UINT(ch * s));

    GifCanvas canvas(cw, ch);
    for (UINT i = 0; i < count; i++) {
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICMetadataQueryReader> q;
        GifFrame f;
        UINT pw = 0, ph = 0;
        if (FAILED(decoder->GetFrame(i, &frame)) || FAILED(frame->GetSize(&pw, &ph))) break;
        frame->GetMetadataQueryReader(&q);
        f.left = number(q.Get(), L"/imgdesc/Left", 0);
        f.top = number(q.Get(), L"/imgdesc/Top", 0);
        f.width = int(pw);
        f.height = int(ph);
        f.delayMs = GifDelayMs(number(q.Get(), L"/grctlext/Delay", 0));
        f.disposal = number(q.Get(), L"/grctlext/Disposal", 0);
        if (!rgbaOf(wic, frame.Get(), pw, ph, f.rgba)) break;
        canvas.Paint(f);

        std::vector<unsigned char> out;
        if (w == UINT(cw) && h == UINT(ch)) {
            out = canvas.Pixels();
        } else {
            ComPtr<IWICBitmap> bmp;
            ComPtr<IWICBitmapScaler> scaler;
            const auto& px = canvas.Pixels();
            if (FAILED(wic->CreateBitmapFromMemory(UINT(cw), UINT(ch), GUID_WICPixelFormat32bppRGBA, UINT(cw) * 4,
                                                   UINT(px.size()), (BYTE*)px.data(), &bmp)) ||
                FAILED(wic->CreateBitmapScaler(&scaler)) ||
                FAILED(scaler->Initialize(bmp.Get(), w, h, WICBitmapInterpolationModeFant)) ||
                !rgbaOf(wic, scaler.Get(), w, h, out))
                break;
        }
        frames.push_back(std::move(out));
        delays.push_back(f.delayMs);
    }
    return !frames.empty();
}

}  // namespace

ImageCache::ImageCache(ID3D11Device* device, std::function<void()> wake)
    : device_(device), wake_(std::move(wake)), thread_(&ImageCache::run, this) {}

ImageCache::~ImageCache() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
    for (auto& [url, e] : entries_)
        if (e.tex.srv) e.tex.srv->Release();
    for (auto& [url, a] : anims_)
        for (auto& t : a.frames) t.srv->Release();
}

void ImageCache::enqueue(const std::string& url, bool animated) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.push_back({url, animated});
    }
    cv_.notify_one();
}

const Texture* ImageCache::Get(const std::string& url) {
    if (url.empty()) return nullptr;
    auto it = entries_.find(url);
    if (it == entries_.end()) {
        evict();
        entries_[url].lastUsed = ++tick_;
        enqueue(url, false);
        return nullptr;
    }
    it->second.lastUsed = ++tick_;
    return it->second.tex.srv ? &it->second.tex : nullptr;
}

const Texture* ImageCache::Animated(const std::string& url, double now) {
    if (url.empty()) return nullptr;
    animatingNow_ = true;
    auto it = anims_.find(url);
    if (it == anims_.end()) {
        evictAnims();
        Anim& a = anims_[url];
        a.lastUsed = ++tick_;
        a.lastFrame = frame_;
        enqueue(url, true);
        return nullptr;
    }
    Anim& a = it->second;
    a.lastUsed = ++tick_;
    if (a.frames.empty()) return nullptr;
    // Not shown last frame: start again.
    if (a.lastFrame + 1 < frame_) a.start = now;
    a.lastFrame = frame_;
    return &a.frames[GifFrameAt(a.delays, (now - a.start) * 1000)];
}

void ImageCache::Tick() {
    frame_++;
    animating_ = animatingNow_;
    animatingNow_ = false;
}

void ImageCache::evictAnims() {
    for (;;) {
        size_t total = 0;
        for (const auto& [url, a] : anims_) total += a.bytes;
        if (total < kAnimsBytes && anims_.size() < 64) return;
        auto oldest = anims_.end();
        for (auto it = anims_.begin(); it != anims_.end(); ++it)
            if (it->second.done && (oldest == anims_.end() || it->second.lastUsed < oldest->second.lastUsed)) oldest = it;
        if (oldest == anims_.end()) return;
        for (auto& t : oldest->second.frames) t.srv->Release();
        anims_.erase(oldest);
    }
}

void ImageCache::evict() {
    while (entries_.size() >= kMaxEntries) {
        auto oldest = std::min_element(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
            // Never evict one still loading; its result would be lost.
            if (a.second.done != b.second.done) return a.second.done;
            return a.second.lastUsed < b.second.lastUsed;
        });
        if (!oldest->second.done) return;
        if (oldest->second.tex.srv) oldest->second.tex.srv->Release();
        entries_.erase(oldest);
    }
}

void ImageCache::Provide(const std::string& key, std::string bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    provided_[key] = std::move(bytes);
}

void ImageCache::Upload() {
    std::vector<Decoded> ready;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ready.swap(decoded_);
    }
    for (auto& d : ready) {
        if (d.animated) {
            auto it = anims_.find(d.url);
            if (it == anims_.end()) continue;
            Anim& a = it->second;
            a.done = true;
            if (d.rgba.empty()) continue;
            for (size_t i = 0; i <= d.more.size(); i++) {
                Texture t = upload(d.width, d.height, i ? d.more[i - 1].data() : d.rgba.data());
                if (!t.srv) break;
                a.frames.push_back(t);
                a.delays.push_back(d.delays[i]);
                a.bytes += size_t(d.width) * d.height * 4;
            }
            a.start = 0;
            a.lastFrame = 0;  // so the first frame drawn starts it
            continue;
        }
        auto it = entries_.find(d.url);
        if (it == entries_.end()) continue;
        it->second.done = true;
        if (d.rgba.empty()) continue;
        it->second.tex = upload(d.width, d.height, d.rgba.data());
    }
}

Texture ImageCache::upload(int width, int height, const unsigned char* rgba) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{rgba, UINT(width * 4), 0};
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device_->CreateTexture2D(&desc, &data, &tex))) return {};
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(device_->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return {};
    return {srv, width, height};
}

void ImageCache::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IWICImagingFactory> wic;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        Http http;
        for (;;) {
            Job job;
            HttpResult r;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (stop_) break;
                job = std::move(queue_.front());
                queue_.pop_front();
                // Bytes supplied by Provide stand in for a download.
                if (auto it = provided_.find(job.url); !job.animated && it != provided_.end()) {
                    r.status = 200;
                    r.body = std::move(it->second);
                    provided_.erase(it);
                }
            }
            const std::string& url = job.url;
            Decoded d{url, job.animated};
            if (r.status == 0 && url.rfind("http", 0) == 0) {
                HttpRequest req;
                req.url = url;
                req.maxBytes = (job.animated ? 32 : 16) << 20;
                r = http.Send(req);
            }
            UINT w = 0, h = 0;
            std::vector<std::vector<unsigned char>> frames;
            if (job.animated && wic && r.ok() && decodeAnimation(wic.Get(), r.body, w, h, frames, d.delays)) {
                d.width = int(w);
                d.height = int(h);
                d.rgba = std::move(frames[0]);
                d.more.assign(std::make_move_iterator(frames.begin() + 1), std::make_move_iterator(frames.end()));
            } else if (!job.animated && wic && r.ok() &&
                       // "full:" keys are for the image viewer: kept big.
                       decode(wic.Get(), r.body, w, h, d.rgba, job.url.rfind("full:", 0) == 0 ? 2048 : kMaxWidth)) {
                d.width = int(w);
                d.height = int(h);
            } else {
                d.rgba.clear();
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                decoded_.push_back(std::move(d));
            }
            wake_();
        }
    }
    CoUninitialize();
}

}  // namespace who
