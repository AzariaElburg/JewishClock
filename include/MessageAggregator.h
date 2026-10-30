#pragma once

#include <Arduino.h>
#include <vector>
#include "Storage.h"
#include "Calendar.h"
#include "OnScreenElement.h"

class MessageAggregator {
public:
    static void begin(const AppConfig& cfg, const EventList& events);
    static void loop(const AppConfig& cfg, bool showOverlay);

    // Forces the next loop() to push the current content to the display again
    // (new message ID) even if none of it actually changed - e.g. after a
    // live font-spacing change, so the currently-shown text re-measures and
    // re-renders immediately instead of waiting for its content to change.
    static void forceRefresh();

    // Demo (Advanced settings): one pass through samples of every kind of
    // slide, in the configured mode, then back to normal. Safe from any task
    // (only posts a request; loop() acts on it). Not saved.
    static void requestDemo(bool on);
    static bool isDemoActive();

    static const std::vector<OnScreenElement>& getMessageElements();
    static uint32_t                   getCurrentMessageId();
    static uint16_t                   getCurrentColor();

private:
    static void buildElements(const AppConfig& cfg);
};
