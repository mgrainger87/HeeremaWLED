// SPDX-License-Identifier: EUPL-1.2
#include "wled.h"

/*
 * Smart Buttons usermod for WLED 16.x
 *
 * Intended for QuinLED An-Penta-Deca physical Buttons 2, 3, and 4:
 *
 *   Physical Button 2 -> WLED button index 1 -> GPIO 39
 *   Physical Button 3 -> WLED button index 2 -> GPIO 34
 *   Physical Button 4 -> WLED button index 3 -> GPIO 33
 *
 * Physical Button 1 / WLED button index 0 / GPIO 36 is deliberately NOT
 * intercepted, so it can remain physically unwired and reserved for WLED's
 * built-in recovery behavior.
 *
 * Each handled button targets one configurable segment and implements:
 *   - Single press: toggle the segment
 *   - Double press: turn the segment on and set its configured white
 *   - Click, release, then hold: start a color show on the segment
 *   - Long press:
 *       * if OFF when the hold is recognized: fade smoothly upward
 *       * if ON  when the hold is recognized: fade smoothly downward
 *
 * A downward long-press turns the segment off at the minimum threshold and
 * resets its opacity to 255 for the next activation.
 */

class SmartButtonsUsermod : public Usermod {
private:
  static constexpr uint8_t FIRST_BUTTON_INDEX = 1;
  static constexpr uint8_t LAST_BUTTON_INDEX  = 3;
  static constexpr uint8_t BUTTON_COUNT       = 3;

  static constexpr uint16_t DEFAULT_LONG_PRESS_MS   = 600;
  static constexpr uint16_t DEFAULT_DOUBLE_PRESS_MS = 350;
  static constexpr uint16_t DEFAULT_DEBOUNCE_MS     = 40;
  static constexpr uint16_t DEFAULT_FULL_RAMP_MS    = 3500;
  static constexpr uint16_t DEFAULT_WHITE_K         = 4000;
  static constexpr uint16_t DEFAULT_LOWER_WHITE_K   = 2700;
  static constexpr uint16_t DEFAULT_UPPER_WHITE_K   = 6500;
  static constexpr uint8_t  DEFAULT_MIN_BRI         = 1;

  struct ButtonState {
    bool rawPressed = false;
    bool stablePressed = false;
    unsigned long rawChangedAt = 0;

    unsigned long pressedAt = 0;

    bool longActive = false;
    bool rampUp = false;
    unsigned long rampStartedAt = 0;
    uint8_t rampStartBrightness = 0;
    uint8_t lastRampBrightness = 0;

    bool waitingForSecondClick = false;
    bool secondPressCandidate = false;
    bool holdActionConsumed = false;
    unsigned long firstReleaseAt = 0;

    bool fadeActive = false;
    bool fadeTurningOn = false;
    uint8_t fadeSegmentId = 0;
    uint8_t fadeStartBrightness = 0;
    uint8_t fadeTargetBrightness = 0;
    uint8_t fadeRestoreBrightness = 255;
    unsigned long fadeStartedAt = 0;
    uint16_t fadeDurationMs = 0;
  };

  bool enabled = true;
  bool colorShowEnabled = true;
  bool buttonEnabled[BUTTON_COUNT] = {true, true, true};
  uint8_t segmentId[BUTTON_COUNT] = {0, 1, 2};
  uint16_t longPressMs = DEFAULT_LONG_PRESS_MS;
  uint16_t doublePressMs = DEFAULT_DOUBLE_PRESS_MS;
  uint16_t debounceMs = DEFAULT_DEBOUNCE_MS;
  uint16_t fullRampMs = DEFAULT_FULL_RAMP_MS;
  uint16_t whiteKelvin[BUTTON_COUNT] = {
    DEFAULT_WHITE_K, DEFAULT_WHITE_K, DEFAULT_WHITE_K
  };
  uint16_t lowerWhiteKelvin[BUTTON_COUNT] = {
    DEFAULT_LOWER_WHITE_K, DEFAULT_LOWER_WHITE_K, DEFAULT_LOWER_WHITE_K
  };
  uint16_t upperWhiteKelvin[BUTTON_COUNT] = {
    DEFAULT_UPPER_WHITE_K, DEFAULT_UPPER_WHITE_K, DEFAULT_UPPER_WHITE_K
  };
  uint8_t minBrightness = DEFAULT_MIN_BRI;

  ButtonState states[BUTTON_COUNT];

  static const char _name[];

  bool handlesButton(uint8_t b) const {
    return enabled && b >= FIRST_BUTTON_INDEX && b <= LAST_BUTTON_INDEX &&
      buttonEnabled[b - FIRST_BUTTON_INDEX];
  }

  ButtonState& stateFor(uint8_t b) {
    return states[b - FIRST_BUTTON_INDEX];
  }

  Segment* segmentFor(uint8_t b) {
    const uint8_t index = b - FIRST_BUTTON_INDEX;
    if (segmentId[index] >= strip.getSegmentsNum()) return nullptr;
    Segment& seg = strip.getSegment(segmentId[index]);
    return seg.isActive() ? &seg : nullptr;
  }

  void ensureMasterOn() {
    if (bri > 0) return;
    bri = briLast > 0 ? briLast : 255;
    strip.restartRuntime();
    stateChanged = true;
  }

  bool readButtonPressed(uint8_t b) const {
    if (b >= buttons.size()) return false;
    if (buttons[b].pin < 0) return false;

    const int pin = buttons[b].pin;

    switch (buttons[b].type) {
      case BTN_TYPE_PUSH:
      case BTN_TYPE_SWITCH:
        return digitalRead(pin) == LOW;

      case BTN_TYPE_PUSH_ACT_HIGH:
      case BTN_TYPE_PIR_SENSOR:
        return digitalRead(pin) == HIGH;

      default:
        return false;
    }
  }

  void finishFade(ButtonState& s, Segment& seg) {
    if (s.fadeTurningOn) {
      seg.on = true;
      seg.opacity = s.fadeTargetBrightness;
    } else {
      seg.on = false;
      seg.opacity = s.fadeRestoreBrightness;
    }

    s.fadeActive = false;
    strip.trigger();
    stateChanged = true;
    stateUpdated(CALL_MODE_BUTTON);
  }

  void updateFade(ButtonState& s, unsigned long now) {
    if (!s.fadeActive) return;
    if (s.fadeSegmentId >= strip.getSegmentsNum()) {
      s.fadeActive = false;
      return;
    }

    Segment& seg = strip.getSegment(s.fadeSegmentId);
    if (!seg.isActive()) {
      s.fadeActive = false;
      return;
    }

    const unsigned long elapsed = now - s.fadeStartedAt;
    if (s.fadeDurationMs == 0 || elapsed >= s.fadeDurationMs) {
      finishFade(s, seg);
      return;
    }

    const int16_t distance =
      int16_t(s.fadeTargetBrightness) - int16_t(s.fadeStartBrightness);
    const int32_t interpolated = int32_t(s.fadeStartBrightness) +
      (int32_t(distance) * int32_t(elapsed)) / int32_t(s.fadeDurationMs);
    seg.on = true;
    seg.opacity = uint8_t(constrain(interpolated, int32_t(0), int32_t(255)));
    strip.trigger();
  }

  void startFade(ButtonState& s, Segment& seg, bool turnOn,
                 uint8_t restoreBrightness, unsigned long now) {
    ensureMasterOn();

    const uint8_t startBrightness = seg.on ? seg.opacity : 0;
    const uint8_t targetBrightness = turnOn ? restoreBrightness : 0;
    const uint16_t fullFadeMs = strip.getTransition();
    const uint16_t distance = abs(
      int16_t(targetBrightness) - int16_t(startBrightness)
    );

    s.fadeActive = true;
    s.fadeTurningOn = turnOn;
    s.fadeSegmentId = segmentId[&s - states];
    s.fadeStartBrightness = startBrightness;
    s.fadeTargetBrightness = targetBrightness;
    s.fadeRestoreBrightness = restoreBrightness;
    s.fadeStartedAt = now;
    s.fadeDurationMs = uint16_t(
      (uint32_t(fullFadeMs) * uint32_t(distance) + 254U) / 255U
    );

    // Keep the segment logically on while either direction is visibly fading.
    seg.on = true;
    seg.opacity = startBrightness;

    if (s.fadeDurationMs == 0 || startBrightness == targetBrightness) {
      finishFade(s, seg);
    } else {
      strip.trigger();
    }
  }

  void doSinglePress(uint8_t b, ButtonState& s, unsigned long now) {
    Segment* seg = segmentFor(b);
    if (!seg) return;

    updateFade(s, now);

    const bool turnOn = s.fadeActive ? !s.fadeTurningOn : (!seg->on || bri == 0);
    const uint8_t restoreBrightness = s.fadeActive
      ? s.fadeRestoreBrightness
      : (seg->opacity > 0 ? seg->opacity : 255);
    startFade(s, *seg, turnOn, restoreBrightness, now);
  }

  void doDoublePress(uint8_t b) {
    ButtonState& s = stateFor(b);
    s.fadeActive = false;
    Segment* seg = segmentFor(b);
    if (!seg) return;
    ensureMasterOn();

    // WLED derives the UI's Value from stored RGB, so full Value requires
    // 255/255/255. Accurate auto-white then removes that common RGB component
    // and routes it exclusively to the physical warm/cool-white channels.
    Bus::setGlobalAWMode(AW_GLOBAL_DISABLED);
    for (size_t i = 0; i < BusManager::getNumBusses(); i++) {
      Bus* bus = BusManager::getBus(i);
      if (bus && bus->hasWhite()) bus->setAutoWhiteMode(RGBW_MODE_AUTO_ACCURATE);
    }

    const uint32_t white = RGBW32(255, 255, 255, 0);
    const uint8_t settingIndex = b - FIRST_BUTTON_INDEX;
    const uint16_t boundedKelvin = constrain(
      whiteKelvin[settingIndex],
      lowerWhiteKelvin[settingIndex],
      upperWhiteKelvin[settingIndex]
    );
    const uint32_t kelvinRange =
      upperWhiteKelvin[settingIndex] - lowerWhiteKelvin[settingIndex];
    const uint8_t cctRatio = uint8_t(
      ((uint32_t(boundedKelvin - lowerWhiteKelvin[settingIndex]) * 255U) +
       (kelvinRange / 2U)) / kelvinRange
    );

    seg->refreshLightCapabilities();
    seg->setOption(SEG_OPTION_ON, true);
    seg->setOpacity(255);
    seg->setMode(FX_MODE_STATIC);
    seg->setColor(0, white);
    seg->setCCT(cctRatio);

    strip.trigger();
    stateUpdated(CALL_MODE_BUTTON);
  }

  void doColorShow(uint8_t b) {
    ButtonState& s = stateFor(b);
    s.fadeActive = false;
    Segment* seg = segmentFor(b);
    if (!seg) return;
    ensureMasterOn();

    // Saturated Colorloop continuously sweeps the physical RGB channels. In
    // accurate auto-white mode, saturated colors have no common white
    // component, so they remain RGB while neutral whites use the CCT outputs.
    seg->setOption(SEG_OPTION_ON, true);
    seg->setOpacity(255);
    seg->setMode(FX_MODE_RAINBOW);
    seg->speed = 100;
    seg->intensity = 255;
    seg->setColor(0, RGBW32(255, 0, 255, 0));

    strip.restartRuntime();
    strip.trigger();
    stateChanged = true;
    stateUpdated(CALL_MODE_BUTTON);
  }

  void beginLongPress(uint8_t b, ButtonState& s, unsigned long now) {
    updateFade(s, now);
    s.fadeActive = false;
    Segment* seg = segmentFor(b);
    if (!seg) return;

    s.longActive = true;
    s.waitingForSecondClick = false;

    // Choose direction once, based on power state at the moment the
    // press becomes a long press.
    s.rampUp = !seg->on || bri == 0;

    if (s.rampUp) {
      ensureMasterOn();
      seg->opacity = minBrightness;
      seg->on = true;
      strip.trigger();
    }

    s.rampStartBrightness = seg->opacity;
    s.lastRampBrightness = seg->opacity;
    s.rampStartedAt = now;
  }

  void updateRamp(uint8_t b, ButtonState& s, unsigned long now) {
    if (!s.longActive) return;
    Segment* seg = segmentFor(b);
    if (!seg) {
      s.longActive = false;
      return;
    }

    const unsigned long elapsed = now - s.rampStartedAt;
    const float unitsPerMs =
      255.0f / float(max<uint16_t>(fullRampMs, 1));

    float value = s.rampStartBrightness;

    if (s.rampUp) {
      value += unitsPerMs * float(elapsed);
    } else {
      value -= unitsPerMs * float(elapsed);
    }

    int target = int(value + 0.5f);
    target = constrain(target, 0, 255);

    // Treat the configured minimum as the effective-off threshold and store
    // full segment opacity for the next activation.
    if (!s.rampUp && target <= minBrightness) {
      seg->on = false;
      seg->opacity = 255;
      s.lastRampBrightness = 0;
      strip.trigger();
      return;
    }

    if (target != s.lastRampBrightness) {
      seg->opacity = uint8_t(target);
      s.lastRampBrightness = seg->opacity;
      strip.trigger();
    }
  }

  void finishLongPress(uint8_t b, ButtonState& s) {
    if (!s.longActive) return;

    s.longActive = false;
    Segment* seg = segmentFor(b);
    if (seg && !seg->on) seg->opacity = 255;

    stateChanged = true;
    stateUpdated(CALL_MODE_BUTTON);
  }

  void pressEdge(ButtonState& s, unsigned long now) {
    s.pressedAt = now;
    s.holdActionConsumed = false;
    s.secondPressCandidate = s.waitingForSecondClick &&
      (now - s.firstReleaseAt) <= doublePressMs;
  }

  void releaseEdge(uint8_t b, ButtonState& s, unsigned long now) {
    if (s.holdActionConsumed) {
      s.holdActionConsumed = false;
      s.waitingForSecondClick = false;
      s.secondPressCandidate = false;
      return;
    }

    if (s.longActive) {
      finishLongPress(b, s);
      s.waitingForSecondClick = false;
      s.secondPressCandidate = false;
      return;
    }

    const unsigned long heldFor = now - s.pressedAt;
    if (heldFor < debounceMs) return;

    if (s.secondPressCandidate) {
      s.waitingForSecondClick = false;
      s.secondPressCandidate = false;
      doDoublePress(b);
    } else {
      s.waitingForSecondClick = true;
      s.secondPressCandidate = false;
      s.firstReleaseAt = now;
    }
  }

public:
  void setup() override {}

  void loop() override {}

  bool handleButton(uint8_t b) override {
    if (!handlesButton(b)) return false;
    if (b >= buttons.size()) return false;

    const uint8_t type = buttons[b].type;
    if (type != BTN_TYPE_PUSH && type != BTN_TYPE_PUSH_ACT_HIGH) return false;

    ButtonState& s = stateFor(b);
    const unsigned long now = millis();
    updateFade(s, now);
    const bool pressedNow = readButtonPressed(b);

    // Debounce.
    if (pressedNow != s.rawPressed) {
      s.rawPressed = pressedNow;
      s.rawChangedAt = now;
    }

    if (s.stablePressed != s.rawPressed &&
        (now - s.rawChangedAt) >= debounceMs) {
      s.stablePressed = s.rawPressed;

      if (s.stablePressed) {
        pressEdge(s, now);
      } else {
        releaseEdge(b, s, now);
      }
    }

    // A held second press launches the optional show without delaying the
    // ordinary double-press action, which still fires on second release.
    if (s.stablePressed && !s.longActive && !s.holdActionConsumed &&
        (now - s.pressedAt) >= longPressMs) {
      if (colorShowEnabled && s.secondPressCandidate) {
        s.waitingForSecondClick = false;
        s.secondPressCandidate = false;
        s.holdActionConsumed = true;
        doColorShow(b);
      } else {
        beginLongPress(b, s, now);
      }
    }

    // Smooth ramp for the duration of the hold.
    if (s.stablePressed && s.longActive) {
      updateRamp(b, s, now);
    }

    // Emit a single click only after the double-click window expires.
    if (!s.stablePressed && s.waitingForSecondClick &&
        (now - s.firstReleaseAt) > doublePressMs) {
      s.waitingForSecondClick = false;
      doSinglePress(b, s, now);
    }

    // Fully consume indices 1, 2, and 3 so WLED's default button actions
    // do not also run.
    return true;
  }

  void addToConfig(JsonObject& root) override {
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) top = root.createNestedObject(FPSTR(_name));

    JsonObject global = top["global"];
    if (global.isNull()) global = top.createNestedObject("global");
    global["enabled"] = enabled;
    global["colorShowEnabled"] = colorShowEnabled;
    global["longPressMs"] = longPressMs;
    global["doublePressMs"] = doublePressMs;
    global["debounceMs"] = debounceMs;
    global["fullRampMs"] = fullRampMs;
    global["minBrightness"] = minBrightness;

    static const char* const buttonKeys[BUTTON_COUNT] = {
      "button-2", "button-3", "button-4"
    };
    for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
      JsonObject button = top[buttonKeys[i]];
      if (button.isNull()) button = top.createNestedObject(buttonKeys[i]);
      button["enabled"] = buttonEnabled[i];
      button["segmentId"] = segmentId[i];
      button["targetKelvin"] = whiteKelvin[i];
      button["warmEndpointKelvin"] = lowerWhiteKelvin[i];
      button["coolEndpointKelvin"] = upperWhiteKelvin[i];
    }
  }

  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root[FPSTR(_name)];
    bool configComplete = !top.isNull();

    JsonObject global = top["global"];
    configComplete &= !global.isNull();
    enabled = global["enabled"] | true;
    colorShowEnabled = global["colorShowEnabled"] | true;
    longPressMs = global["longPressMs"] | DEFAULT_LONG_PRESS_MS;
    doublePressMs = global["doublePressMs"] | DEFAULT_DOUBLE_PRESS_MS;
    debounceMs = global["debounceMs"] | DEFAULT_DEBOUNCE_MS;
    fullRampMs = global["fullRampMs"] | DEFAULT_FULL_RAMP_MS;
    minBrightness = global["minBrightness"] | DEFAULT_MIN_BRI;

    static const char* const buttonKeys[BUTTON_COUNT] = {
      "button-2", "button-3", "button-4"
    };
    for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
      JsonObject button = top[buttonKeys[i]];
      configComplete &= !button.isNull();
      buttonEnabled[i] = button["enabled"] | true;
      segmentId[i] = button["segmentId"] | i;
      whiteKelvin[i] = button["targetKelvin"] | DEFAULT_WHITE_K;
      lowerWhiteKelvin[i] =
        button["warmEndpointKelvin"] | DEFAULT_LOWER_WHITE_K;
      upperWhiteKelvin[i] =
        button["coolEndpointKelvin"] | DEFAULT_UPPER_WHITE_K;
    }

    longPressMs = constrain(longPressMs, uint16_t(200), uint16_t(3000));
    doublePressMs = constrain(doublePressMs, uint16_t(150), uint16_t(1000));
    debounceMs = constrain(debounceMs, uint16_t(10), uint16_t(150));
    fullRampMs = constrain(fullRampMs, uint16_t(500), uint16_t(15000));
    for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
      lowerWhiteKelvin[i] = constrain(
        lowerWhiteKelvin[i], uint16_t(1000), uint16_t(9999)
      );
      upperWhiteKelvin[i] = constrain(
        upperWhiteKelvin[i], uint16_t(1001), uint16_t(10000)
      );
      if (upperWhiteKelvin[i] <= lowerWhiteKelvin[i]) {
        lowerWhiteKelvin[i] = DEFAULT_LOWER_WHITE_K;
        upperWhiteKelvin[i] = DEFAULT_UPPER_WHITE_K;
      }
      whiteKelvin[i] = constrain(
        whiteKelvin[i], lowerWhiteKelvin[i], upperWhiteKelvin[i]
      );
    }
    minBrightness = constrain(minBrightness, uint8_t(1), uint8_t(64));

    return configComplete;
  }

  void appendConfigData(Print& settingsScript) override {
    settingsScript.print(F(
      "addInfo('SmartButtons:global:enabled',1,'Master switch for smart handling of physical Buttons 2, 3, and 4.');"
      "addInfo('SmartButtons:global:colorShowEnabled',1,'Enables click-release-then-hold to start a Colorloop show.');"
      "addInfo('SmartButtons:global:fullRampMs',1,'Time for a full minimum-to-255 brightness sweep.');"
      "addInfo('SmartButtons:global:minBrightness',1,'Effective-off threshold for a downward hold.');"
      "addInfo('SmartButtons:button-2:enabled',1,'Enables smart handling for physical Button 2 (GPIO 39).');"
      "addInfo('SmartButtons:button-2:segmentId',1,'Zero-based WLED segment controlled by Button 2.');"
      "addInfo('SmartButtons:button-2:targetKelvin',1,'Button 2 double-click target in Kelvin.');"
      "addInfo('SmartButtons:button-2:warmEndpointKelvin',1,'Kelvin rating of Button 2 warm-white channel.');"
      "addInfo('SmartButtons:button-2:coolEndpointKelvin',1,'Kelvin rating of Button 2 cool-white channel.');"
      "addInfo('SmartButtons:button-3:enabled',1,'Enables smart handling for physical Button 3 (GPIO 34).');"
      "addInfo('SmartButtons:button-3:segmentId',1,'Zero-based WLED segment controlled by Button 3.');"
      "addInfo('SmartButtons:button-3:targetKelvin',1,'Button 3 double-click target in Kelvin.');"
      "addInfo('SmartButtons:button-3:warmEndpointKelvin',1,'Kelvin rating of Button 3 warm-white channel.');"
      "addInfo('SmartButtons:button-3:coolEndpointKelvin',1,'Kelvin rating of Button 3 cool-white channel.');"
      "addInfo('SmartButtons:button-4:enabled',1,'Enables smart handling for physical Button 4 (GPIO 33).');"
      "addInfo('SmartButtons:button-4:segmentId',1,'Zero-based WLED segment controlled by Button 4.');"
      "addInfo('SmartButtons:button-4:targetKelvin',1,'Button 4 double-click target in Kelvin.');"
      "addInfo('SmartButtons:button-4:warmEndpointKelvin',1,'Kelvin rating of Button 4 warm-white channel.');"
      "addInfo('SmartButtons:button-4:coolEndpointKelvin',1,'Kelvin rating of Button 4 cool-white channel.');"
    ));
  }
};

constexpr uint8_t SmartButtonsUsermod::FIRST_BUTTON_INDEX;
constexpr uint8_t SmartButtonsUsermod::LAST_BUTTON_INDEX;
constexpr uint8_t SmartButtonsUsermod::BUTTON_COUNT;
constexpr uint16_t SmartButtonsUsermod::DEFAULT_LONG_PRESS_MS;
constexpr uint16_t SmartButtonsUsermod::DEFAULT_DOUBLE_PRESS_MS;
constexpr uint16_t SmartButtonsUsermod::DEFAULT_DEBOUNCE_MS;
constexpr uint16_t SmartButtonsUsermod::DEFAULT_FULL_RAMP_MS;
constexpr uint16_t SmartButtonsUsermod::DEFAULT_WHITE_K;
constexpr uint16_t SmartButtonsUsermod::DEFAULT_LOWER_WHITE_K;
constexpr uint16_t SmartButtonsUsermod::DEFAULT_UPPER_WHITE_K;
constexpr uint8_t SmartButtonsUsermod::DEFAULT_MIN_BRI;

const char SmartButtonsUsermod::_name[] PROGMEM = "SmartButtons";

static SmartButtonsUsermod smart_buttons_usermod;
REGISTER_USERMOD(smart_buttons_usermod);
