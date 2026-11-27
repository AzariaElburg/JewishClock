#pragma once
// PC stand-in for the display library (simulator only - see sim/README.md):
// the panel itself does nothing; the canvas is a real RGB565 framebuffer
// with the same drawing methods Display.cpp uses, so the simulator's frame
// is exactly what the clock would send to its panel.

#include <Arduino.h>
#include <vector>

#define GFX_NOT_DEFINED -1

class Arduino_DataBus {
public:
    virtual ~Arduino_DataBus() {}
};

class Arduino_ESP32SPI : public Arduino_DataBus {
public:
    Arduino_ESP32SPI(int8_t dc, int8_t cs, int8_t sck, int8_t mosi, int8_t miso, uint8_t spi_num, bool shared = false) {}
    void writePixels(uint16_t* data, uint32_t len) {}
    void writeCommand(uint8_t c) {}
    void write(uint8_t d) {}
};

class Arduino_ESP32SPIDMA : public Arduino_ESP32SPI {
public:
    using Arduino_ESP32SPI::Arduino_ESP32SPI;
};

class Arduino_GFX {
public:
    Arduino_GFX(int16_t w = 0, int16_t h = 0) : w_(w), h_(h) {}
    virtual ~Arduino_GFX() {}
    virtual bool begin(int32_t speed = GFX_NOT_DEFINED) { return true; }
    virtual void drawPixel(int16_t x, int16_t y, uint16_t color) {}
    virtual void fillScreen(uint16_t color) {}
    virtual void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {}
    virtual void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    virtual void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
    virtual void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t* bitmap, int16_t w, int16_t h) {}
    virtual void startWrite() {}
    virtual void endWrite() {}
    int16_t width() const { return w_; }
    int16_t height() const { return h_; }
protected:
    int16_t w_, h_;
};

// The window the last frame was sent to (x, y, w, h) - what the panel was told, kept like
// the ledc stand-in keeps its duty, so a test can follow Calibration's window to the panel
// (/state "panelWindow", 2026-11-15). All -1 = nothing sent yet.
inline int16_t g_simPanelWindow[4] = { -1, -1, -1, -1 };

class Arduino_NV3041A : public Arduino_GFX {
public:
    Arduino_NV3041A(Arduino_DataBus* bus, int8_t rst = GFX_NOT_DEFINED, uint8_t r = 0, bool ips = false) : Arduino_GFX(480, 128) {}
    void writeAddrWindow(int16_t x, int16_t y, uint16_t w, uint16_t h) {
        g_simPanelWindow[0] = x; g_simPanelWindow[1] = y; g_simPanelWindow[2] = (int16_t)w; g_simPanelWindow[3] = (int16_t)h;
    }
};

class Arduino_Canvas : public Arduino_GFX {
public:
    Arduino_Canvas(int16_t w, int16_t h, Arduino_GFX* output) : Arduino_GFX(w, h), fb_((size_t)w * h, 0) {}
    bool begin(int32_t speed = GFX_NOT_DEFINED) override { return true; }
    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        if (x >= 0 && y >= 0 && x < w_ && y < h_) fb_[(size_t)y * w_ + x] = color;
    }
    void fillScreen(uint16_t color) override { std::fill(fb_.begin(), fb_.end(), color); }
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        for (int yy = y; yy < y + h; yy++) for (int xx = x; xx < x + w; xx++) drawPixel((int16_t)xx, (int16_t)yy, color);
    }
    uint16_t* getFramebuffer() { return fb_.data(); }
private:
    std::vector<uint16_t> fb_;
};
