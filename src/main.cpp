#include <Arduino.h>
#include "display_ui.h"
#include "settings.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "bambu_mqtt.h"
#include "config.h"
#include "bambu_state.h"
#include "button.h"
#include "buzzer.h"
#include "tasmota.h"
#ifdef USE_XPT2046
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include "touch_buttons.h"
// NOTE: XPT2046_Touchscreen instance 'ts' is initialized by button.cpp
// when buttonType == BTN_TOUCHSCREEN. We just reference it here.
extern XPT2046_Touchscreen ts;
#endif

static unsigned long splashEnd = 0;
static unsigned long lastTouchTime = 0;
static const unsigned long TOUCH_DEBOUNCE_MS = 500;  // Increased debounce time

static unsigned long finishScreenStart = 0;
static bool finishActive = false;          // guards finishScreenStart against millis() wrap
static unsigned long idleClockStart = 0;  // when all printers became idle
static bool idleClockActive = false;      // guards idleClockStart against millis() wrap
static unsigned long clockSleepStart = 0; // when SCREEN_CLOCK became active
static bool clockSleepActive = false;     // guards clockSleepStart against millis() wrap
static bool clockTimedSleepOff = false;   // true only when SCREEN_OFF was entered from clock timeout
static unsigned long connectingScreenStart = 0;  // for stuck-state timeout
static char prevGcodeState[MAX_ACTIVE_PRINTERS][16] = {{0}};
static bool wakeStateInit = false;
static bool prevWakeConnected[MAX_ACTIVE_PRINTERS] = {false};
static bool prevWakePrinting[MAX_ACTIVE_PRINTERS] = {false};
static char prevWakeGcodeState[MAX_ACTIVE_PRINTERS][16] = {{0}};

static bool detectPrinterActivity() {
  bool activity = false;

  for (uint8_t i = 0; i < MAX_ACTIVE_PRINTERS; i++) {
    bool cfg = isPrinterConfigured(i);
    const BambuState& s = printers[i].state;

    if (wakeStateInit && cfg) {
      bool gcodeChanged = strcmp(s.gcodeState, prevWakeGcodeState[i]) != 0;
      bool wasPrinting = prevWakePrinting[i];

      // Meaningful wake events only: start print, pause/resume,
      // finish/fail, or reconnect while already printing.
      if (!wasPrinting && s.printing) {
        activity = true;
      } else if (!prevWakeConnected[i] && s.connected && s.printing) {
        activity = true;
      } else if (gcodeChanged) {
        bool pauseOrRun = (strcmp(s.gcodeState, "PAUSE") == 0 ||
                           strcmp(s.gcodeState, "RUNNING") == 0);
        bool finishedOrFailed = (strcmp(s.gcodeState, "FINISH") == 0 ||
                                 strcmp(s.gcodeState, "FAILED") == 0);
        if (pauseOrRun || finishedOrFailed) {
          activity = true;
        }
      }
    }

    if (cfg) {
      prevWakeConnected[i] = s.connected;
      prevWakePrinting[i] = s.printing;
      strlcpy(prevWakeGcodeState[i], s.gcodeState, sizeof(prevWakeGcodeState[i]));
    } else {
      prevWakeConnected[i] = false;
      prevWakePrinting[i] = false;
      prevWakeGcodeState[i][0] = '\0';
    }
  }

  wakeStateInit = true;
  return activity;
}

// ---------------------------------------------------------------------------
//  Display rotation logic (multi-printer)
// ---------------------------------------------------------------------------
static void handleRotation() {
  if (rotState.mode == ROTATE_OFF) return;
  if (getActiveConnCount() < 2) return;

  // Don't rotate when display is in clock, off, or finished state,
  // UNLESS a printer is actively printing (wake up to show it)
  ScreenState scr = getScreenState();
  if (scr == SCREEN_CLOCK || scr == SCREEN_OFF || scr == SCREEN_FINISHED) {
    bool anyPrinting = false;
    for (uint8_t i = 0; i < MAX_ACTIVE_PRINTERS; i++) {
      if (isPrinterConfigured(i) && printers[i].state.connected && printers[i].state.printing) {
        anyPrinting = true;
        break;
      }
    }
    if (!anyPrinting) return;
    // A printer started printing — wake display and let rotation proceed
    setBacklight(getEffectiveBrightness());
  }

  unsigned long now = millis();
  if (now - rotState.lastRotateMs < rotState.intervalMs) return;

  // Gather candidates
  uint8_t candidates[MAX_ACTIVE_PRINTERS];
  uint8_t candidateCount = 0;
  uint8_t printingCount = 0;
  uint8_t printingSlot = 0xFF;

  for (uint8_t i = 0; i < MAX_ACTIVE_PRINTERS; i++) {
    if (!isPrinterConfigured(i)) continue;
    if (!printers[i].state.connected) continue;
    candidates[candidateCount++] = i;
    if (printers[i].state.printing) {
      printingCount++;
      printingSlot = i;
    }
  }

  if (candidateCount == 0) return;

  if (rotState.mode == ROTATE_SMART) {
    if (printingCount == 1) {
      // Only one printing — show it, no cycling
      if (rotState.displayIndex != printingSlot) {
        rotState.displayIndex = printingSlot;
        triggerDisplayTransition();
      }
      rotState.lastRotateMs = now;
      return;
    }
    // 0 or 2 printing: fall through to cycling
  }

  // Cycle to next candidate
  uint8_t current = rotState.displayIndex;
  for (uint8_t attempt = 1; attempt <= MAX_ACTIVE_PRINTERS; attempt++) {
    uint8_t next = (current + attempt) % MAX_ACTIVE_PRINTERS;
    for (uint8_t c = 0; c < candidateCount; c++) {
      if (candidates[c] == next && next != current) {
        rotState.displayIndex = next;
        triggerDisplayTransition();
        rotState.lastRotateMs = now;
        return;
      }
    }
  }

  rotState.lastRotateMs = now;
}

// ── Touch Button Functions ──────────────────────────────────────────────
#ifdef USE_XPT2046

bool touchButtonsDirty = false;

void drawTouchButtons() {
  // Print control buttons are disabled.
  touchButtonsDirty = false;
}

void handleTouchButton(int x, int y) {
  (void)x;
  (void)y;
}

#endif

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.printf("\n=== BambuHelper %s Starting ===\n", FW_VERSION);

  loadSettings();
  initDisplay();
  splashEnd = millis() + 2000;
  setBacklight(brightness);
  // Touchscreen will be initialized by initButton() via button.cpp
}

void loop() {
  // Hold splash for 2s
  if (splashEnd > 0 && millis() > splashEnd) {
    splashEnd = 0;
    Serial.printf("[INIT] Button type: %d (0=disabled, 1=push, 2=touch, 3=touchscreen)\n", buttonType);
    initWiFi();
    initWebServer();
    initBambuMqtt();
    initButton();
    Serial.println("[INIT] initButton() completed");
    initBuzzer();
    tasmotaInit();
  }

  if (splashEnd > 0) {
    delay(10);
    return;
  }

  handleWiFi();
  handleWebServer();

  if (isWiFiConnected() && !isAPMode()) {
    bool printerActivity = false;

    if (isAnyPrinterConfigured()) {
      handleBambuMqtt();
      handleRotation();
      printerActivity = detectPrinterActivity();
    }

    // Wake from timed clock sleep on printer state/activity changes.
    if (clockTimedSleepOff && getScreenState() == SCREEN_OFF && printerActivity) {
      setBacklight(getEffectiveBrightness());
      finishActive = false;
      idleClockActive = false;
      clockSleepActive = false;
      clockTimedSleepOff = false;
      resetMqttBackoff();
      setScreenState(SCREEN_IDLE);
    }

    // Handle physical button press
    if (wasButtonPressed()) {
      ScreenState cur = getScreenState();
      if (cur == SCREEN_OFF || cur == SCREEN_CLOCK) {
        // Wake from sleep + reset backoff for immediate reconnect
        setBacklight(getEffectiveBrightness());
        finishActive = false;
        idleClockActive = false;
        clockSleepActive = false;
        clockTimedSleepOff = false;
        resetMqttBackoff();
        setScreenState(SCREEN_IDLE);  // state machine will correct on next loop
      } else if (getActiveConnCount() >= 2) {
        // Cycle to next configured printer
        uint8_t idx = rotState.displayIndex;
        for (uint8_t a = 1; a <= MAX_ACTIVE_PRINTERS; a++) {
          uint8_t next = (idx + a) % MAX_ACTIVE_PRINTERS;
          if (isPrinterConfigured(next) && next != idx) {
            rotState.displayIndex = next;
            triggerDisplayTransition();
            rotState.lastRotateMs = millis();  // reset auto-rotate timer
            finishActive = false;
            // If switching to a cloud printer in UNKNOWN state, try a refresh
            requestCloudRefresh(next);
            break;
          }
        }
      } else if (cur == SCREEN_IDLE &&
                 isCloudMode(displayedPrinter().config.mode) &&
                 strcmp(displayedPrinter().state.gcodeState, "UNKNOWN") == 0) {
        // Single printer, cloud, UNKNOWN - manual refresh
        requestCloudRefresh(rotState.displayIndex);
      }
    }

#ifdef USE_XPT2046
    // Handle touchscreen input
    if (ts.touched()) {
      TS_Point p = ts.getPoint();
      
      // Filter out stuck corner readings (baseline junk values for each rotation)
      bool isJunkReading = (
        (p.x == 0 && p.y == 0 && p.z == 4095) ||
        (p.x == 4095 && p.y == 0 && p.z == 4095) ||
        (p.x == 0 && p.y == 4095 && p.z == 4095) ||
        (p.x == 4095 && p.y == 4095 && p.z == 4095)
      );
      
      if (!isJunkReading && millis() - lastTouchTime > TOUCH_DEBOUNCE_MS) {
        int x = map(p.x, 0, 4095, 239, 0);
        int y = map(p.y, 0, 4095, 319, 0);
        x = constrain(x + 2, 0, 239);
        y = constrain(y, 0, 319);
        (void)x;
        (void)y;

        ScreenState cur = getScreenState();
        if (cur == SCREEN_OFF || cur == SCREEN_CLOCK || cur == SCREEN_FINISHED) {
          setBacklight(getEffectiveBrightness());
          finishActive = false;
          idleClockActive = false;
          clockSleepActive = false;
          clockTimedSleepOff = false;
          resetMqttBackoff();
          setScreenState(SCREEN_IDLE);
          lastTouchTime = millis();
          return;
        }
      }
    }
#endif

    // Auto-select screen based on displayed printer state
    BambuState& s = displayedPrinter().state;
    ScreenState current = getScreenState();

    if (!isAnyPrinterConfigured()) {
      if (current != SCREEN_IDLE && current != SCREEN_OFF) {
        setScreenState(SCREEN_IDLE);
        finishActive = false;
      }
    } else if (isOtaAutoInProgress()) {
      if (current != SCREEN_OTA_UPDATE) setScreenState(SCREEN_OTA_UPDATE);
    } else if (!s.connected && current != SCREEN_CONNECTING_MQTT &&
               current != SCREEN_OFF && current != SCREEN_CLOCK) {
      setScreenState(SCREEN_CONNECTING_MQTT);
      finishActive = false;
      connectingScreenStart = millis();
    } else if (!s.connected && (current == SCREEN_OFF || current == SCREEN_CLOCK)) {
      // Stay off/clock when printer is disconnected/off
    } else if (s.connected && s.printing) {
      if (current != SCREEN_PRINTING) {
        setScreenState(SCREEN_PRINTING);
        finishActive = false;
        if (tasmotaSettings.assignedSlot == 255 ||
            tasmotaSettings.assignedSlot == rotState.displayIndex)
          tasmotaMarkPrintStart();
      }
      s.finishBuzzerPlayed = false;  // reset for next finish event
      s.doorAcknowledged = false;    // reset door ack for next finish
    } else if (s.connected && !s.printing &&
               strcmp(s.gcodeState, "FINISH") == 0) {
      if (current != SCREEN_FINISHED && current != SCREEN_OFF && current != SCREEN_CLOCK) {
        if (tasmotaSettings.enabled &&
            (tasmotaSettings.assignedSlot == 255 ||
             tasmotaSettings.assignedSlot == rotState.displayIndex))
          tasmotaMarkPrintEnd();
        setScreenState(SCREEN_FINISHED);
        finishScreenStart = millis();
        finishActive = true;
        if (!s.finishBuzzerPlayed) {
          buzzerPlay(BUZZ_PRINT_FINISHED);
          s.finishBuzzerPlayed = true;
        }
      }

      // Door acknowledge: wait for door open before starting timeout
      bool waitingForDoor = dpSettings.doorAckEnabled && s.doorSensorPresent
                            && !s.doorAcknowledged;
      if (waitingForDoor && s.doorOpen) {
        s.doorAcknowledged = true;
        finishScreenStart = millis();  // restart timeout from door open moment
        finishActive = true;
        Serial.println("Door opened - print removal acknowledged, starting timeout");
      }

      // Transition from finish screen to clock/off
      // If door ack is enabled and door not yet opened, block the transition
      if (current == SCREEN_FINISHED && !dpSettings.keepDisplayOn &&
          !waitingForDoor && finishActive) {
        // finishDisplayMins==0: go to clock immediately if enabled, otherwise stay on finish
        // finishDisplayMins>0: wait for timeout before transitioning
        bool timeoutReached = (dpSettings.finishDisplayMins > 0) &&
            (millis() - finishScreenStart > (unsigned long)dpSettings.finishDisplayMins * 60000UL);
        bool immediateClockTransition = (dpSettings.finishDisplayMins == 0) &&
            (dpSettings.showClockAfterFinish || buttonType == BTN_DISABLED);

        if (timeoutReached || immediateClockTransition) {
          bool anyPrinting = false;
          for (uint8_t i = 0; i < MAX_ACTIVE_PRINTERS; i++) {
            if (isPrinterConfigured(i) && printers[i].state.connected && printers[i].state.printing) {
              anyPrinting = true;
              break;
            }
          }
          if (!anyPrinting) {
            if (dpSettings.showClockAfterFinish || buttonType == BTN_DISABLED) {
              setScreenState(SCREEN_CLOCK);
            } else {
              setScreenState(SCREEN_OFF);
            }
          }
        }
      }
    } else if (s.connected && !s.printing &&
               strcmp(s.gcodeState, "FINISH") != 0) {
      // SCREEN_CLOCK and SCREEN_OFF are sticky — only button press or
      // new print (s.printing → SCREEN_PRINTING) exits them
      if (current == SCREEN_CLOCK || current == SCREEN_OFF) {
        // nothing — stay asleep while printer is idle
      } else if (current != SCREEN_IDLE) {
        if (current == SCREEN_CONNECTING_MQTT) buzzerPlay(BUZZ_CONNECTED);
        setScreenState(SCREEN_IDLE);
        finishActive = false;
        idleClockActive = false;
      }
    }
  }

  // Idle/Connecting → Clock/Off: if all printers are idle or disconnected,
  // transition to clock or off after finishDisplayMins timeout.
  // Covers both SCREEN_IDLE (printer connected but not printing) and
  // SCREEN_CONNECTING_MQTT (printer offline/unreachable at startup).
  ScreenState cur = getScreenState();
  if ((cur == SCREEN_IDLE || cur == SCREEN_CONNECTING_MQTT) &&
      !dpSettings.keepDisplayOn && dpSettings.finishDisplayMins > 0) {
    bool anyBusy = false;
    for (uint8_t i = 0; i < MAX_ACTIVE_PRINTERS; i++) {
      if (isPrinterConfigured(i) && printers[i].state.connected && printers[i].state.printing) {
        anyBusy = true;
        break;
      }
    }
    if (!anyBusy) {
      if (!idleClockActive) { idleClockStart = millis(); idleClockActive = true; }
      if (millis() - idleClockStart > (unsigned long)dpSettings.finishDisplayMins * 60000UL) {
        if (dpSettings.showClockAfterFinish || buttonType == BTN_DISABLED) {
          setScreenState(SCREEN_CLOCK);
        } else {
          setScreenState(SCREEN_OFF);
        }
      }
    } else {
      idleClockActive = false;
    }
  } else if (cur != SCREEN_IDLE && cur != SCREEN_CONNECTING_MQTT) {
    idleClockActive = false;
  }

  // Clock -> Off: after configured clock auto-off timeout.
  ScreenState curSleep = getScreenState();
  if (curSleep == SCREEN_CLOCK) {
    if (!clockSleepActive) {
      clockSleepStart = millis();
      clockSleepActive = true;
    }
    unsigned long offMs = (unsigned long)dpSettings.clockAutoOffMins * 60000UL;
    if (offMs > 0 && millis() - clockSleepStart > offMs) {
      setScreenState(SCREEN_OFF);
      clockTimedSleepOff = true;
      clockSleepActive = false;
    }
  } else {
    clockSleepActive = false;
    if (curSleep != SCREEN_OFF) {
      clockTimedSleepOff = false;
    }
  }

  // Stuck-state timeout: recover if stuck in a connecting screen too long
  {
    ScreenState curConn = getScreenState();
    if (curConn == SCREEN_CONNECTING_WIFI || curConn == SCREEN_CONNECTING_MQTT) {
      if (connectingScreenStart == 0) connectingScreenStart = millis();
      if (millis() - connectingScreenStart > DISPLAY_STATE_TIMEOUT_MS) {
        Serial.println("[MAIN] State timeout, recovering from connecting screen");
        connectingScreenStart = 0;
        if (dpSettings.showClockAfterFinish) {
          setScreenState(SCREEN_CLOCK);
        } else {
          setScreenState(SCREEN_IDLE);
        }
      }
    } else {
      connectingScreenStart = 0;
    }
  }

  // Check for error state transition on any printer
  for (uint8_t i = 0; i < MAX_ACTIVE_PRINTERS; i++) {
    if (!isPrinterConfigured(i)) continue;
    BambuState& ps = printers[i].state;
    if (strcmp(ps.gcodeState, "FAILED") == 0 &&
        strcmp(prevGcodeState[i], "FAILED") != 0 &&
        prevGcodeState[i][0] != '\0') {
      buzzerPlay(BUZZ_ERROR);
    }
    strlcpy(prevGcodeState[i], ps.gcodeState, sizeof(prevGcodeState[i]));
  }

  buzzerTick();
  checkNightMode();
  updateDisplay();
}