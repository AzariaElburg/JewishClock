#pragma once
#include <Arduino.h>

enum class OnScreenElementType {
    TEXT,
    STATIC_IMAGE,
    ANIMATED_SPRITE
};

struct OnScreenElement {
    OnScreenElementType type;
    String content; // Path for images, text for text
    int width;
    bool isValid = true;

    // When true, this element is rendered directly against the previous one
    // with no message separator in between (e.g. an icon glued to its label,
    // such as a Havdalah bitmap immediately followed by its time). Separators
    // still appear between distinct segments as usual.
    bool glueToPrevious = false;

    // For animated sprites
    int frame_count = 1;
    int frame_delay_ms = 100;

    OnScreenElement(OnScreenElementType t, String c, int w) : type(t), content(c), width(w) {}
};