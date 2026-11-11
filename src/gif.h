// Animated GIFs: putting a GIF's frames together into whole pictures, and
// working out which one shows when. Portable; the decoding is in images.cpp.
#pragma once

#include <string>
#include <vector>

namespace who {

// GifFrame is one frame as a GIF stores it: a patch of the picture, how long
// it shows, and what happens to its patch before the next frame.
struct GifFrame {
    int left = 0, top = 0, width = 0, height = 0;
    int delayMs = 100;
    // 0 or 1 leave the patch; 2 clears it; 3 puts back what was there.
    int disposal = 0;
    std::vector<unsigned char> rgba;  // width x height, transparent where alpha is 0
};

// GifCanvas paints frames one after another, as a browser would.
class GifCanvas {
public:
    GifCanvas(int width, int height);
    // Paint tidies away the last frame's patch as it asked, then draws this
    // frame's over the picture. Pixels is then the whole picture.
    void Paint(const GifFrame& f);
    const std::vector<unsigned char>& Pixels() const { return pixels_; }

private:
    void clip(const GifFrame& f, int& x0, int& y0, int& x1, int& y1) const;

    int width_, height_;
    std::vector<unsigned char> pixels_, saved_;
    GifFrame last_;  // its place and disposal; no pixels
    bool haveLast_ = false;
};

// GifDelayMs turns a GIF's delay (hundredths of a second) into milliseconds.
// Like browsers, 10ms or less means 100ms: those GIFs were made for that.
int GifDelayMs(int centiseconds);

// GifFrameAt is the frame showing ms into a looping animation.
int GifFrameAt(const std::vector<int>& delaysMs, double ms);

// IsGifUrl says whether a URL's path ends in .gif.
bool IsGifUrl(const std::string& url);

}  // namespace who
