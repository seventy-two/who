package main

import (
	"bytes"
	"image"
	"image/jpeg"
	_ "image/png"

	_ "golang.org/x/image/webp"
)

// imageThumb gives an image's size and a small JPEG preview, as WhatsApp
// shows before the full image is downloaded.
func imageThumb(data []byte) (w, h int, thumb []byte) {
	img, _, err := image.Decode(bytes.NewReader(data))
	if err != nil {
		return 0, 0, nil
	}
	b := img.Bounds()
	w, h = b.Dx(), b.Dy()
	const side = 72
	tw, th := side, side*h/max(w, 1)
	if h > w {
		th, tw = side, side*w/max(h, 1)
	}
	tw, th = max(tw, 1), max(th, 1)
	small := image.NewRGBA(image.Rect(0, 0, tw, th))
	for y := 0; y < th; y++ {
		for x := 0; x < tw; x++ {
			small.Set(x, y, img.At(b.Min.X+x*w/tw, b.Min.Y+y*h/th))
		}
	}
	var out bytes.Buffer
	if jpeg.Encode(&out, small, &jpeg.Options{Quality: 60}) != nil {
		return w, h, nil
	}
	return w, h, out.Bytes()
}
