/* 
================================================================================================
======================================== ChannelLED ============================================
================================================================================================

LED batten controller (PWM) for the ESP32-2432S028R "Cheap Yellow Display" (2.8", 320 x 240).
2026 Disaster of Puppets
Use the code at your own risk, I mean it should be fine..but you know..free and all that.
Ie, If you break hardware / yourself, I am not liable.
TLDR :) Have fun

Board: ESP32 Dev Module (Arduino IDE, ESP32 core v3.x).

--- HARDWARE NOTE ---
GPIO 21 on the P3 header drives the LCD backlight - don't use it for anything else.
GPIO 35 (LEFT / -) is input-only with no internal pull-up - it needs the external 10k pull-up.
Two connections are soldered straight to the ESP32-WROOM-32 module (not on any header):
  - GROUND: top-right pin of the module, next to the antenna shield.
  - GPIO 25 (PWM out to the MOSFET's TRIG/PWM pin): 5th pin up from the bottom-left corner.
--------------------------------------------------------------------------------------------------

--- LOOK (change these in VARIABLES - one theme per mode) ---
  hostTheme / clientTheme / standaloneTheme, each with:
    background           screen background (RGB565)
    backgroundIntensity  0 = black ... 100 = background at full strength
    glow                 colour of frames, highlights and icons (RGB565)
    glowIntensity        0 = no glow ... 100 = strongest glow
  Host = blue, Client = green, Standalone = orange. The boot screen uses the Host theme.
  While the Switch Mode popup is open the theme stays as it was - it switches when the popup closes.
  Text: titles white (MENU_COL_TEXT), everything else grey (MENU_COL_BODY). Only the logo is bold.

--- MODES (not remembered - every power-on searches again) ---
  H  Host        H_ConnectLED          Leads the group - every Client follows it. Only one per group.
  C  Client      C_ConnectLED_<num>    Follows the Host. Reconnects automatically if the Host drops out.
                                       View-only: compact rows show the Host's values - only Switch Mode works.
  S  Standalone  SA_ConnectLED_<num>   Runs on its own, ignores the Host.
  On power-on: joins a Host if it hears one, otherwise becomes Host. Enter skips to Standalone.
  Mode changes from the Switch Mode popup last until power-off.

--- STATUS ICONS (top-right) ---
  Link:  GREEN chain = Connected   YELLOW broken chain (blinking) = Retrying   (none when Standalone)
  Role:  Coral house = Host   Cyan linked node = Client   Purple person = Standalone

--- ANIMATIONS ---
  None, Blink, Lightning, Strobe, Chase
  Chase: every linked light goes dark, then each one fades 0 -> 50% -> 0 in turn - the Host first,
         then each Client in name order (C_ConnectLED_01, _02 ...), then round again.
         Speed sets how long each pulse lasts (Speed 1 = 2s, 50 = about 1.2s, 100 = 0.3s).

--- BUTTONS ---  LEFT = (-)   RIGHT = (+)   OK = Enter
  - / +                          -> move between items, or change a value while editing
  OK (short press, on release)   -> start / confirm editing the highlighted item

--- BUTTON SHORTCUTS (HOLD) ---   (Client: only Switch Mode)
  50% Brightness (no animation)  -> hold - & + (1s), release
  All Off                        -> hold - & + & OK (1s)
  Switch Mode popup              -> hold OK (1s), release  (same again to cancel)
  LEFT (-) held on boot          -> LED starts at 50% brightness immediately
  Enter (OK) during boot search  -> skip the search, run Standalone
None of the button pins (35, 22, 27) are ESP32 boot strapping pins, so holding them at
power-on can't stop the board booting.

--- SWITCH MODE POPUP ---
  - / +  pick the New Mode   ->   OK moves to Update   ->   OK applies
  On the buttons: + moves Update -> Cancel, - moves back (Cancel -> Update -> New Mode)
  Cancel (or holding OK) returns to the main menu with nothing changed.

--- INFO BOX (under the menu) ---
  Fades between "Button Shortcuts (Hold)" and the icon "Key" every INFO_CYCLE_MS.
  Paused while the Switch Mode popup is open or a notification is showing.
  Client: one fixed, taller box instead - "Controlled by Host", Switch Mode (Hold OK) and the Key.

You may see the below warning, but can safely ignore it - the CYD is used as a display only, touch is disabled.
  #warning >>>>------>> TOUCH_CS pin not defined, TFT_eSPI touch functions will not be available!

Needs LOAD_GFXFF enabled in TFT_eSPI's User_Setup.h (for the bold FreeSansBold logo font).
*/


#include <Arduino.h>
#include <TFT_eSPI.h>     // Copy this repo's User_Setup.h into the TFT_eSPI library folder (see README).
#include <WiFi.h>         // Required for ESP-NOW - puts the radio in STA mode, no actual WiFi connection is made.
#include <esp_now.h>      // ESP-NOW: multi-device sync between battens.

//------------------------------------------  DEFINES  --------------------------------------------

#define SW tft.width()
#define SH tft.height()

// Inactivity timeout (ms)
#define INACTIVITY_TIMEOUT 60000  

// --- Button GPIO Pins ---
#define LEFT_BTN_PIN    35  // (-) Blue wire from button to ESP32 GPIO 35
#define RIGHT_BTN_PIN   22  // (+) Yellow wire from button to ESP32 GPIO 22
#define OK_BTN_PIN      27  // (OK / Enter) Purple wire from button to ESP32 GPIO 27

// --- Debounce & Ramping Configuration ---
const unsigned long BUTTON_DEBOUNCE_MS  = 75;  // Time to ignore bounces after a press
const unsigned long RAMP_START_DELAY_MS = 300; // Time button must be held before ramping starts
const unsigned long RAMP_INTERVAL_MS    = 75;  // Time between continuous ramps (controls speed)

// --- Ramp acceleration (step size grows the longer the button stays held) ---
const unsigned long RAMP_ACCEL_STAGE1_MS = 800;  // total hold time before step size first increases
const unsigned long RAMP_ACCEL_STAGE2_MS = 2000; // total hold time before step size increases again
const unsigned long RAMP_ACCEL_STAGE3_MS = 4000; // total hold time before step size increases a third time
const int RAMP_STEP_STAGE0 = 1;  // step size while held < STAGE1
const int RAMP_STEP_STAGE1 = 5;  // step size while STAGE1 <= held < STAGE2
const int RAMP_STEP_STAGE2 = 10; // step size while STAGE2 <= held < STAGE3
const int RAMP_STEP_STAGE3 = 20; // step size once held >= STAGE3

// --- ESP-NOW Multi-Device Sync ---
// Every unit in a group shares DEVICE_GROUP_NAME. ***DEVICE_NUMBER MUST BE UNIQUE PER UNIT***
// (used in Client/Standalone names, e.g. unit 1 = "01", unit 2 = "02" ...)
#define DEVICE_GROUP_NAME  "ConnectLED"
#define DEVICE_NUMBER      "01"

const unsigned long HOST_SEARCH_WINDOW_MS     = 10000; // Power-on search for an existing Host
const unsigned long HOST_CHECK_MS             = 3500;  // Quick "is there a Host?" check (Switch Mode popup)
const unsigned long HOST_QUERY_INTERVAL_MS    = 1000;  // How often a searching unit asks "any Host out there?"
const unsigned long HOST_ANNOUNCE_INTERVAL_MS = 3000;  // How often a Host broadcasts a keep-alive
const unsigned long HOST_HEARTBEAT_TIMEOUT_MS = 15000; // A Client starts reconnecting after this long with no contact
const unsigned long CLIENT_HELLO_INTERVAL_MS  = 2000;  // How often a linked Client tells the Host it's there
const unsigned long CLIENT_ROSTER_TIMEOUT_MS  = 7000;  // Host drops a Client from the Chase after this long unheard
#define MAX_CLIENTS        20                          // Most Clients the Host keeps track of

// --- PWM config (ESP32 Arduino core v3.x API) ---
#define LED_PWM_PIN     25  // Soldered direct to the module - see hardware note at top of file
#define PWM_FREQ        5000
#define PWM_RESOLUTION  8  // 0-255

// --- Chase animation (each unit pulses in turn: Host first, then each Client in name order) ---
#define CHASE_PEAK_PERCENT   50     // Brightness at the top of each pulse
#define CHASE_STEP_SLOW_MS   2000   // Pulse length at Speed 1
#define CHASE_STEP_FAST_MS   300    // Pulse length at Speed 100 (Speed 50 = about 1.2s)

// Set to 'true' to invert value adjustment direction when in sub-menus (brightness/speed).
// If SUB_DIR is 'false', LEFT (-) will DECREASE values and RIGHT (+) will INCREASE values.
#define SUB_DIR false 

// --- Colours (backgroundColor / glowColor / glowIntensity come from the active theme - see VARIABLES) ---
#define THEME_CURRENT_COLORS_NOTIFICATION_SUCCESS TFT_GREEN  // Notification text in the info box

#define MENU_COL_TEXT    TFT_WHITE        // Titles - white, regular weight
#define MENU_COL_BODY    0xAD55           // Everything else (values, selections, rows, messages) - light grey
#define MENU_COL_MUTED   0x8C71           // Darker grey - secondary hints
#define MENU_COL_LOCKED  0x52AA           // Greyed-out item
#define MENU_COL_ANIM    0xA9FF           // Purple - used in gradients
#define MENU_COL_MAGENTA 0xD81F           // Magenta - end of the Speed slider gradient
#define INFO_COL_TITLE   MENU_COL_TEXT    // Info box titles
#define INFO_COL_TEXT    MENU_COL_BODY    // Info box row text

// --- Glow frames ---
#define GLOW_W           4     // How far the outer glow spreads outside a frame (px)
#define INNER_GLOW_W     4     // How far the glow bleeds inward from a frame's edge (px)
#define FRAME_CUT        10    // Size of the angled corner cuts on the big panels

// --- Screen layout (portrait 240 x 320) ---
#define MENU_HEADER_H    30
#define MENU_ROW_X       6
#define MENU_ROW_W       228
#define MENU_ROW_H       48
#define MENU_ROW_Y0      34
#define MENU_ROW_GAP     8

// --- Info box (shortcuts <-> key) ---
#define INFO_X               6
#define INFO_Y               202
#define INFO_W               228
#define INFO_H               84
#define INFO_TITLE_H         22    // Title row height
#define INFO_ROW_H           20    // Each content row
#define INFO_ICON_COL_W      28    // Icon cell on the left of the title row
#define INFO_ICON_DX         15    // Icon column centre
#define INFO_TEXT_DX         34    // Text column
#define SHORTCUT_KEYS_DX     128   // Shortcut keys column
#define SHORTCUT_AMP_GAP     5     // Space either side of the "&" between keys
#define KEY_COL2_DX          122   // Key page: right column
#define NOTIFY_SHOW_MS       3000
#define INFO_CYCLE_MS        6000  // How long each info page shows before fading to the next
#define INFO_FADE_STEPS      8     // Steps in each fade out / fade in
#define INFO_FADE_STEP_MS    35    // Time between fade steps
#define NOTIFICATION_TEXT_Y  (INFO_Y + INFO_H / 2) // Passed to drawNotificationText() - not used for positioning

// --- Client layout (view-only: compact rows, one fixed info page) ---
#define CLIENT_ROW_H         32    // Row height - title and value on one line, no sliders
#define CLIENT_INFO_ROW_H    22    // Info box rows below the title
#define CLIENT_INFO_Y        (MENU_ROW_Y0 + 3 * CLIENT_ROW_H + 2 * MENU_ROW_GAP + 8)
#define CLIENT_INFO_H        (INFO_TITLE_H + 5 * CLIENT_INFO_ROW_H) // Title + Switch Mode + Key header + 3 key rows
#define CLIENT_LOCK_COL      0xFEA0 // Yellow padlock

// --- Footer bar (Mode on the left, Device on the right) ---
#define FOOTER_X         6
#define FOOTER_Y         294
#define FOOTER_W         228
#define FOOTER_H         22

// --- Boot screen ---
#define BOOT_FRAME_MS         33    // ~30fps
#define BOOT_TITLE_X          14    // Logo + "ChannelLED" title, top-left
#define BOOT_TITLE_Y          24
#define BOOT_TEXT_Y           242   // Top of the status strip (status, time left, progress bar)
#define BOOT_TEXT_H           50
#define BOOT_HINT_Y           304   // "Press Enter to Skip"

// --- Boot vortex animation ---
#define VORTEX_SPR_X          6      // Vortex sprite position / size on screen
#define VORTEX_SPR_Y          44
#define VORTEX_SPR_W          228
#define VORTEX_AREA_H         196
#define VORTEX_CX             (VORTEX_SPR_W / 2)  // Centre of the vortex inside the sprite
#define VORTEX_CY             98
#define VORTEX_PARTICLES      120    // Particles in the two spiral arms
#define VORTEX_STARS          30     // Background twinkle stars
#define VORTEX_RING_R         86     // Outer glowing ring radius
#define VORTEX_INNER_R        64     // Dashed inner ring radius
#define VORTEX_SPAWN_R        78     // Particles are born here and spiral inward
#define VORTEX_CORE_R         12     // Particles are absorbed into the orb here
#define VORTEX_HEAT_R         32     // Particles start glowing white-hot inside this radius
#define VORTEX_SPIN           0.05   // Rotation speed (higher = faster swirl)
#define VORTEX_ARM_TWIST      0.045  // How tightly the two arms curl
#define VORTEX_RING_DRAW_MS   900    // Ring "draws itself" in over this long on boot
#define VORTEX_CRACKLE_CHANCE 4      // % chance per frame of a lightning bolt into the orb

// --- Boot vortex: central energy orb ---
#define VORTEX_ORB_R          14     // Resting orb radius
#define VORTEX_ORB_PULSE      2      // Size of the slow pulse (px)
#define VORTEX_ORB_GROW       6      // Extra radius at full energy
#define VORTEX_ORB_FEED       0.02   // Energy each absorbed particle adds
#define VORTEX_ORB_BOLT_FEED  0.5    // Energy a lightning bolt hitting the orb adds
#define VORTEX_ORB_DECAY      0.9    // Energy kept each frame (lower = flares fade faster)

// Vortex colours (RGB565)
#define VORTEX_COL_CYAN       0x05BF
#define VORTEX_COL_PURPLE     0xA9FF
#define VORTEX_COL_MAGENTA    0xD81F
#define VORTEX_COL_ORANGE     0xFCA2
#define VORTEX_COL_GOLD       0xFE8A
#define VORTEX_COL_EMBER      0xFAC0

// --- Status icons (top-right of the header) ---
#define STATUS_LINK_X        (SW - 18)   // Chain-link connection icon
#define STATUS_ROLE_X        (SW - 46)   // Role icon (moves into the link icon's spot when Standalone)
#define STATUS_ICON_Y        16
#define STATUS_COL_CONNECTED 0x2E68      // Green  - Connected (Host / linked Client)
#define STATUS_COL_RETRY     0xFEA0      // Yellow - Retrying (Client looking for its Host)
#define STATUS_COL_HOST      0xFB10      // Coral  - Host role icon (house)
#define STATUS_COL_CLIENT    0x05BF      // Cyan   - Client role icon
#define STATUS_COL_SOLO      0xA9FF      // Purple - Standalone role icon
#define STATUS_BLINK_MS      500         // Yellow icon blink rate while retrying
#define KEY_ICON_SCALE       0.7         // Icons in the Key box are drawn at this size

// --- Switch Mode popup ---
#define POP_X            12
#define POP_Y            44
#define POP_W            216
#define POP_H            214
#define POP_COL_WARN     0xFCA2   // Orange warning text


//------------------------------------------  VARIABLES  ------------------------------------------

// ===== LOOK - one theme per mode. Change these to restyle the UI =====
struct UiTheme {
  uint16_t background;          // Screen background (RGB565)
  uint8_t  backgroundIntensity; // 0 = black ... 100 = background at full strength
  uint16_t glow;                // Frames, highlights and icons (RGB565)
  uint8_t  glowIntensity;       // 0 = no glow ... 100 = strongest glow
};
UiTheme hostTheme       = { 0x08EB, 60, 0x05BF, 40 };  // Host       - royal blue  / cyan glow
UiTheme clientTheme     = { 0x09C4, 60, 0x07EF, 40 };  // Client     - forest green / neon green glow
UiTheme standaloneTheme = { 0x38E0, 60, 0xFC60, 40 };  // Standalone - deep amber  / orange glow
// The boot screen (before the mode is decided) uses hostTheme.

// Live colours - set by applyTheme(), used by all the drawing code. Don't edit these directly.
uint16_t backgroundColor = 0x08EB;
uint16_t glowColor       = 0x05BF;
uint8_t  glowIntensity   = 40;

// --- Button State Variables (for software debouncing and ramping logic) ---
// These track the *debounced* state of the buttons
bool upBtnDebouncedState = HIGH;      // LEFT (-)  - HIGH when not pressed (due to pull-up)
bool downBtnDebouncedState = HIGH;    // RIGHT (+) - HIGH when not pressed
bool okBtnDebouncedState = HIGH;      // OK        - HIGH when not pressed

// These track the *raw* physical state for detecting changes
bool upBtnRawState = HIGH;
bool downBtnRawState = HIGH;
bool okBtnRawState = HIGH;

// These store the timestamp of the last raw state change for debouncing
unsigned long lastUpBtnRawChangeTime = 0;
unsigned long lastDownBtnRawChangeTime = 0;
unsigned long lastOkBtnRawChangeTime = 0;

// Ramping specific timers
unsigned long upBtnHeldStartTime = 0;   // Time LEFT started being held down
unsigned long downBtnHeldStartTime = 0; // Time RIGHT started being held down
unsigned long lastUpRampTime = 0;       // Last time LEFT performed a ramp step
unsigned long lastDownRampTime = 0;     // Last time RIGHT performed a ramp step

// OK acts on release: a short press is a normal OK, a long hold opens the Switch Mode popup
const unsigned long OK_MODE_HOLD_MS = 1000; // Hold OK (on its own) this long for the Switch Mode popup
bool okPressValid = false;              // This OK press started outside a shortcut - act on release
unsigned long okHeldSince = 0;          // millis() OK went down
bool okModeArmed = false;               // OK held long enough - releasing opens (or cancels) the popup

// --- Button shortcuts ---
const unsigned long ALL_BUTTONS_HOLD_MS = 1000; // - & + & OK held this long = all off
const unsigned long COMBO_MIN_HOLD_MS   = 1000; // - & + held this long arms the 50% reset
const int SHORTCUT_RESET_BRIGHTNESS     = 50;   // 50% shortcut, and LEFT-held-on-boot level

// --- Button shortcut state ---
bool comboActive = false;               // - & + down - normal button actions ignored until all released
uint8_t comboShape = 0;                 // Which buttons are down: bit0 = LEFT, bit1 = RIGHT, bit2 = OK
unsigned long comboHeldSince = 0;       // millis() the current button combination started
bool comboUsedOk = false;               // OK was pressed at some point during this combo
bool combo50Armed = false;              // - & + held long enough - releasing fires the 50% reset
bool allButtonsFired = false;           // - & + & OK shortcut already fired during this hold

// Track last user action for screen timeout
static unsigned long lastActivity = 0;
static bool screenSleeping = false;
bool menuShown = false;                 // Main menu has been drawn at least once (footer can draw)

// --- Info box state ---
unsigned long notifyUntil = 0;          // While a notification shows: millis() it expires (0 = none)
enum InfoFade { INFO_SHOWING, INFO_FADING_OUT, INFO_FADING_IN };
uint8_t infoPage = 0;                   // 0 = Button Shortcuts (Hold), 1 = Key
InfoFade infoFade = INFO_SHOWING;
uint8_t infoFadeStep = 0;
unsigned long infoNextTime = 0;         // millis() of the next page change / fade step

// --- App/UI state ---
TFT_eSPI tft = TFT_eSPI();
bool Debug = true;// disable to remove serial prints

int brightnessPercent = 0;  // default brightness on boot(0-100)
int tempBrightness = 0;     // temporary brightness while adjusting

// Menu items: 1 = Brightness, 2 = Animation, 3 = Speed.
// (Index 0 is the status icons in the header - shown, never selected.)
int previousSelection = 1; // To track the item that was last hovered/active
int currentSelection = 1;  // index of selected item - starts on Brightness
bool itemActivated = false;  // whether the menu item is active or not


enum AnimationType { NONE, BLINK, LIGHTNING, STROBE, CHASE };
#define ANIM_COUNT 5                    // Number of entries in AnimationType - used to wrap the selector
AnimationType currentAnimation = NONE; 
int speedPercent = 50; // Default speed, though it will be greyed out initially
int tempSpeed = 50; // temp speed while adjusting
AnimationType tempAnimation = NONE; 

// --- Animation State Variables ---
bool blinkState = false; // Current state of the blink (on/off)
unsigned long lastBlinkToggleTime = 0; // Timestamp of last blink state change

bool strobeState = false; // Current state of the strobe (on/off)
unsigned long lastStrobeToggleTime = 0; // Timestamp of last strobe state change

// Lightning is a two-phase cycle: a dark WAITING gap, then a burst of random
// flickers (FLASHING), then back to WAITING with a new random gap.
enum LightningPhase { LIGHTNING_WAITING, LIGHTNING_FLASHING };
LightningPhase lightningPhase = LIGHTNING_WAITING;
unsigned long lightningNextEventTime = 0; // millis() timestamp of the next flicker/phase change
int lightningFlashesRemaining = 0;        // Flickers left in the current strike
bool lightningLedOn = false;              // Current on/off state of the LED during a strike

// Chase: the Host (or a unit on its own) runs the sequence; linked Clients wait to be told to pulse.
volatile bool chasePulseRequested = false;   // The Host told this Client to pulse - started from loop()
volatile uint16_t chasePulseRequestMs = 0;   // How long that pulse should last
bool chasePulseActive = false;               // This unit is mid-pulse
unsigned long chasePulseStart = 0;           // millis() this unit's pulse started
unsigned long chasePulseLen = 0;             // Length of this unit's pulse
unsigned long chaseNextStepTime = 0;         // Host / solo: when the next unit's turn starts
int chaseTurn = 0;                           // Host / solo: 0 = this unit, 1.. = Clients in name order

// --- Device mode (not saved - every power-on searches again) ---
enum DeviceMode : uint8_t { MODE_UNSET = 0, MODE_HOST = 1, MODE_CLIENT = 2, MODE_STANDALONE = 3 };
volatile DeviceMode deviceMode = MODE_UNSET;

String fullDeviceName;                   // H_ConnectLED / C_ConnectLED_01 / SA_ConnectLED_01
char myName[32] = {0};                   // Copy of fullDeviceName the radio callback can safely read
uint8_t broadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Client side: the Host we follow
uint8_t hostMac[6] = {0};
char hostName[32] = {0};
volatile bool hostMacKnown = false;
volatile bool clientLinked = false;               // Client: true = following a Host, false = searching
volatile unsigned long lastHostContactTime = 0;   // millis() of last message heard from our Host
unsigned long lastClientHelloTime = 0;            // millis() this Client last told the Host it's here

// Host side
unsigned long lastHostAnnounceTime = 0;           // millis() this Host last sent its keep-alive
volatile bool hostAnnounceRequested = false;      // A searching unit asked "any Host?" - reply from loop()
volatile bool hostConflictDetected = false;       // Another Host with a lower MAC exists - step down in loop()

// Host side: the Clients that have checked in recently (used for the Chase order)
struct ClientEntry {
  bool used;
  uint8_t mac[6];
  char name[32];
  unsigned long lastSeen;
};
ClientEntry clientRoster[MAX_CLIENTS];             // Written by the radio callback
ClientEntry chaseOrder[MAX_CLIENTS];               // Sorted snapshot used by the Chase
portMUX_TYPE rosterMux = portMUX_INITIALIZER_UNLOCKED;

// Set by the receive callback whenever ANY Host is heard - used by searches
volatile bool hostHeard = false;
uint8_t heardHostMac[6] = {0};
char heardHostName[32] = {0};

// Wire format for ESP-NOW packets. Must stay identical on every device since they
// all run the same firmware and decode this struct directly from the raw bytes.
enum EspNowMsgType : uint8_t {
  MSG_ANNOUNCE     = 0,  // Host keep-alive (carries state)
  MSG_STATE_SYNC   = 1,  // Host pushes a changed state
  MSG_HOST_QUERY   = 2,  // "Is there a Host?"
  MSG_CLIENT_HELLO = 3,  // Client -> Host: "I'm here" (builds the Chase order)
  MSG_CHASE_PULSE  = 4   // Host -> one Client: "your turn to pulse"
};
typedef struct {
  EspNowMsgType msgType;
  char deviceName[32];      // Sender's full name, e.g. "H_ConnectLED"
  bool isHost;
  int brightnessPercent;    // Host state messages only
  AnimationType animation;  // Host state messages only
  int speedPercent;         // Host state messages only
  char targetName[32];      // MSG_CHASE_PULSE: which Client should pulse
  uint16_t pulseMs;         // MSG_CHASE_PULSE: how long the pulse lasts
} EspNowMessage;

// The receive callback runs on the WiFi/LWIP task, not the main loop - TFT/SPI drawing isn't
// safe to call from there alongside loop()'s own SPI use (menu redraws).
// So state received from the Host is queued here and actually applied from loop().
volatile bool pendingStateUpdate = false;
EspNowMessage pendingStateMsg;

// --- Boot Vortex Animation State ---
struct VortexParticle {
  float angle;     // Radians around the centre
  float radius;    // Distance from the centre (px)
  float speed;     // Inward speed (px per frame)
  uint16_t colour;
};
VortexParticle vortex[VORTEX_PARTICLES];
uint8_t starX[VORTEX_STARS];
uint8_t starY[VORTEX_STARS];
float orbEnergy = 0; // 0..1 - rises as the orb absorbs particles/bolts, decays each frame

// --- Status icon state ---
// The ESP-NOW callback can change link state but can't draw, so loop() compares
// these against the live values and redraws the icons when they differ.
DeviceMode drawnMode = MODE_UNSET;
bool drawnLinked = false;
bool statusBlinkOn = true;
unsigned long lastStatusBlink = 0;

// --- Switch Mode popup state ---
enum PopupScreen { POP_SELECT, POP_RESULT, POP_CONFIRM_HOST, POP_HOST_EXISTS };
bool popupOpen = false;                 // While true, the popup gets every button press
PopupScreen popupScreen = POP_SELECT;
uint8_t popupSelectFocus = 0;           // Select screen: 0 = New Mode selector, 1 = Update, 2 = Cancel
DeviceMode popupNewMode = MODE_CLIENT;  // Option shown under "New Mode"
DeviceMode popupPrevMode = MODE_STANDALONE; // Mode to go back to if "No" is chosen
bool popupYesSelected = false;          // Yes/No focus on the "No Host found" question
const char *popupTitle = "";
String popupBody;
String popupWarning;

// --- Sprites ---
TFT_eSprite menuSpr = TFT_eSprite(&tft);  // One menu row (plus its glow) at a time, then pushed - no flicker
TFT_eSprite infoSpr = TFT_eSprite(&tft);  // Info box (plus its glow) - drawn here so fades don't flicker


//-------------------------------------------  SETUP  --------------------------------------------

void setup() {
  Serial.begin(115200);

  // Plain INPUT - the buttons use external 10k pull-ups (see the wiring diagram).
  // GPIO 35 has no internal pull-up, so the external resistors are required.
  pinMode(LEFT_BTN_PIN, INPUT);
  pinMode(RIGHT_BTN_PIN, INPUT);
  pinMode(OK_BTN_PIN, INPUT); 

  // --- Boot shortcut: check which buttons are held at power-on ---
  bool bootLeft  = digitalRead(LEFT_BTN_PIN);
  bool bootRight = digitalRead(RIGHT_BTN_PIN);
  bool bootOk    = digitalRead(OK_BTN_PIN);
  bool leftHeldOnBoot = (bootLeft == LOW);

  // Seed button states with what's held now, so loop() doesn't treat a boot-held button as a fresh press
  upBtnRawState   = upBtnDebouncedState   = bootLeft;
  downBtnRawState = downBtnDebouncedState = bootRight;
  okBtnRawState   = okBtnDebouncedState   = bootOk;

  if (leftHeldOnBoot) {
    brightnessPercent = SHORTCUT_RESET_BRIGHTNESS;
    tempBrightness = brightnessPercent;
  }

  // Configure ESP32 LEDC (PWM) - done first so the light is at its boot level before the slower display/radio init
  ledcAttach(LED_PWM_PIN, PWM_FREQ, PWM_RESOLUTION); 
  ledcWrite(LED_PWM_PIN, brightnessToPWM(brightnessPercent)); // 0% on boot, or 50% if LEFT held

  delay(200);
  if (Debug && leftHeldOnBoot) Serial.println("LEFT held on boot - brightness set to 50%.");

  // Display
  tft.init();
  applyTheme(MODE_UNSET); // Boot screen uses the Host theme until the mode is decided
  tft.setRotation(0); // Portrait
  if (Debug) Serial.printf("Panel init OK. w=%d, h=%d, rotation=%d\n", tft.width(), tft.height(), tft.getRotation());
  tft.fillScreen(backgroundColor);

  // Begin inactivity count
  lastActivity = millis();

  // ESP-NOW: bring up the radio, then run the boot screen (search for a Host).
  setupEspNow();
  runBootSequence();

  // Initial draw of the main menu
  drawFullMenu();
}

//--------------------------------------------  LOOP  --------------------------------------------

void loop() {

  unsigned long now = millis(); // Get current time for all timing checks

  // --- Read current raw button states ---
  bool readingUp   = digitalRead(LEFT_BTN_PIN);
  bool readingDown = digitalRead(RIGHT_BTN_PIN);
  bool readingOk   = digitalRead(OK_BTN_PIN);

  // --- Process LEFT (-) button ---
  if (readingUp != upBtnRawState) { // Raw state changed
    lastUpBtnRawChangeTime = now;   // Reset debounce timer
  }
  if ((now - lastUpBtnRawChangeTime) > BUTTON_DEBOUNCE_MS) { // Debounce window passed
    if (readingUp != upBtnDebouncedState) { // Debounced state has truly changed
      upBtnDebouncedState = readingUp;
      if (upBtnDebouncedState == LOW) { // PRESSED
        if (!comboActive) {
          if (popupOpen) popupNav(-1);
          else rotateAction(-1, 1); // Initial tap always moves by 1
        }
        upBtnHeldStartTime = now;   // Start tracking hold time for ramping
        lastUpRampTime = now;
      } else { // RELEASED
        upBtnHeldStartTime = 0;
      }
    }
  }
  // Ramping (step size grows the longer it's held) - not during a shortcut or the popup
  if (!comboActive && !popupOpen && upBtnDebouncedState == LOW && upBtnHeldStartTime != 0) {
    if ((now - upBtnHeldStartTime) > RAMP_START_DELAY_MS) {
      if ((now - lastUpRampTime) > RAMP_INTERVAL_MS) {
        rotateAction(-1, getRampStepSize(now - upBtnHeldStartTime));
        lastUpRampTime = now;
      }
    }
  }

  // --- Process RIGHT (+) button ---
  if (readingDown != downBtnRawState) {
    lastDownBtnRawChangeTime = now;
  }
  if ((now - lastDownBtnRawChangeTime) > BUTTON_DEBOUNCE_MS) {
    if (readingDown != downBtnDebouncedState) {
      downBtnDebouncedState = readingDown;
      if (downBtnDebouncedState == LOW) { // PRESSED
        if (!comboActive) {
          if (popupOpen) popupNav(1);
          else rotateAction(1, 1);
        }
        downBtnHeldStartTime = now;
        lastDownRampTime = now;
      } else { // RELEASED
        downBtnHeldStartTime = 0;
      }
    }
  }
  if (!comboActive && !popupOpen && downBtnDebouncedState == LOW && downBtnHeldStartTime != 0) {
    if ((now - downBtnHeldStartTime) > RAMP_START_DELAY_MS) {
      if ((now - lastDownRampTime) > RAMP_INTERVAL_MS) {
        rotateAction(1, getRampStepSize(now - downBtnHeldStartTime));
        lastDownRampTime = now;
      }
    }
  }

  // --- Process OK button ---
  // Acts on release, so it never misfires as part of a shortcut:
  //   short press                -> normal OK (menu / popup)
  //   held alone OK_MODE_HOLD_MS -> opens the Switch Mode popup (or cancels it if it's open)
  if (readingOk != okBtnRawState) {
    lastOkBtnRawChangeTime = now;
  }
  if ((now - lastOkBtnRawChangeTime) > BUTTON_DEBOUNCE_MS) {
    if (readingOk != okBtnDebouncedState) {
      okBtnDebouncedState = readingOk;
      if (okBtnDebouncedState == LOW) { // PRESSED
        okPressValid = !comboActive;
        okHeldSince = now;
        okModeArmed = false;
      } else { // RELEASED
        if (okPressValid && !comboActive) {
          if (okModeArmed) {
            if (popupOpen) cancelPopup();
            else openModePopup();
          } else if (popupOpen) {
            popupOk();
          } else {
            pressAction();
          }
        }
        okPressValid = false;
        okModeArmed = false;
      }
    }
  }
  if (okBtnDebouncedState == LOW && okPressValid && !comboActive && !okModeArmed &&
      (now - okHeldSince >= OK_MODE_HOLD_MS)) {
    okModeArmed = true;
    resetInactivityTimer();
    drawNotificationText(NOTIFICATION_TEXT_Y, "Release to switch mode");
  }

  // --- Button Shortcuts ---
  // - & +       held COMBO_MIN_HOLD_MS, released   -> 50% brightness, no animation
  // - & + & OK  held ALL_BUTTONS_HOLD_MS           -> all off
  // A quick accidental tap of both buttons does nothing.
  bool leftDown  = (upBtnDebouncedState == LOW);
  bool rightDown = (downBtnDebouncedState == LOW);
  bool okDown    = (okBtnDebouncedState == LOW);
  uint8_t shape  = (leftDown ? 1 : 0) | (rightDown ? 2 : 0) | (okDown ? 4 : 0);

  if (!comboActive && (shape == 3 || shape == 7)) {
    comboActive = true; // Normal button actions are ignored until every button is released
    comboShape = 0xFF;  // Forces the hold timer to start below
    comboUsedOk = false;
    combo50Armed = false;
    allButtonsFired = false;
  }
  if (comboActive) {
    if (okDown) comboUsedOk = true;
    if (shape != comboShape) { // Combination changed - restart the hold timer
      comboShape = shape;
      comboHeldSince = now;
    }
    unsigned long held = now - comboHeldSince;

    if (shape == 7 && !allButtonsFired && !popupOpen && !controlsLocked() && held >= ALL_BUTTONS_HOLD_MS) {
      applyShortcutReset(0, "All off");
      allButtonsFired = true;
    }
    if (shape == 3 && !combo50Armed && !comboUsedOk && !popupOpen && !controlsLocked() && held >= COMBO_MIN_HOLD_MS) {
      combo50Armed = true;
      resetInactivityTimer();
      drawNotificationText(NOTIFICATION_TEXT_Y, "Release for 50% brightness");
    }

    if (shape == 0) { // Everything released - combo over
      if (!allButtonsFired) {
        if (combo50Armed && !comboUsedOk) {
          applyShortcutReset(SHORTCUT_RESET_BRIGHTNESS, "50% brightness, no animation");
        } else if (combo50Armed) {
          drawNotificationText(NOTIFICATION_TEXT_Y, ""); // Armed, then changed mind - back to the info page
        }
      }
      comboActive = false;
    }
  }
  // --- End Button Shortcuts ---

  // --- Update last raw states for next loop iteration ---
  upBtnRawState = readingUp;
  downBtnRawState = readingDown;
  okBtnRawState = readingOk;

  // --- Info box: notification expiry, then the page cycle (shortcuts <-> key) ---
  if (notifyUntil != 0 && (long)(now - notifyUntil) >= 0) {
    notifyUntil = 0;
    showInfoPageNow();
  }
  infoTick(now);

  runAnimations();

  // --- ESP-NOW Sync Maintenance ---
  if (deviceMode == MODE_HOST && (hostAnnounceRequested || now - lastHostAnnounceTime >= HOST_ANNOUNCE_INTERVAL_MS)) {
    hostAnnounceRequested = false;
    broadcastAnnounce();
    lastHostAnnounceTime = now;
  }
  if (deviceMode == MODE_CLIENT && clientLinked && now - lastClientHelloTime >= CLIENT_HELLO_INTERVAL_MS) {
    sendClientHello(); // Keeps this Client in the Host's Chase order
    lastClientHelloTime = now;
  }
  if (hostConflictDetected) {
    hostConflictDetected = false;
    if (deviceMode == MODE_HOST) { // Two Hosts - the other one has the lower MAC, so it stays Host
      linkToHeardHost();
      drawNotificationText(NOTIFICATION_TEXT_Y, "Another Host found - now a Client");
      if (Debug) Serial.println("Host conflict - stepped down to Client.");
    }
  }
  if (pendingStateUpdate) {
    pendingStateUpdate = false;
    applyReceivedState(pendingStateMsg);
  }
  checkHostHeartbeat();
  // --- End ESP-NOW Sync Maintenance ---

  // Keep the status icons current (link changes from the radio, yellow blink)
  if (!screenSleeping) statusIconsTick(now);

  // Enter sleep if idle
  checkInactivity();

  delay(5); // Small delay to prevent excessive polling
}


//--------------------------------------------  FUNCTIONS  ---------------------------------------


//------------------------------------------------------------------- SLEEP and WAKE

// Blank the entire screen and mark sleeping. An open popup is cancelled
// (an unanswered "set as new Host?" goes back to the previous mode).
void sleepScreen() {
  if (popupOpen && popupScreen == POP_CONFIRM_HOST) setMode(popupPrevMode);
  screenSleeping = true;
  popupOpen = false;
  notifyUntil = 0;
  infoFade = INFO_SHOWING;
  tft.fillScreen(TFT_BLACK);
  // IMPORTANT: Do NOT stop PWM or animations here. They should continue running.
  if (Debug) Serial.println("Screen is now sleeping. Animations (if any) continue running.");
}

// Wake the screen by redrawing the current menu (in the current mode's theme)
void wakeScreen() {
  screenSleeping = false;
  applyTheme(deviceMode);
  drawFullMenu();
}

// Reset timer and wake if sleeping
void resetInactivityTimer() {
  lastActivity = millis();
  if (screenSleeping) wakeScreen();
}

// Called each loop to enter sleep when timed out
void checkInactivity() {
  if (!screenSleeping && millis() - lastActivity >= INACTIVITY_TIMEOUT) {
    sleepScreen();
  }
}

//------------------------------------------------------------------- THEMES

// Switches the live colours to the theme for this mode (Host theme before a mode is decided).
// Everything else - panels, glow, dividers, fades - is mixed from these three values.
void applyTheme(DeviceMode mode) {
  const UiTheme &t = (mode == MODE_CLIENT)     ? clientTheme
                   : (mode == MODE_STANDALONE) ? standaloneTheme
                   : hostTheme;
  uint8_t pct = (t.backgroundIntensity > 100) ? 100 : t.backgroundIntensity;
  backgroundColor = tft.alphaBlend((uint8_t)((pct * 255) / 100), t.background, TFT_BLACK);
  glowColor       = t.glow;
  glowIntensity   = t.glowIntensity;
}

//------------------------------------------------------------------- MODE + NAMES

String getModeName(DeviceMode m) {
  switch (m) {
    case MODE_HOST:       return "Host";
    case MODE_CLIENT:     return "Client";
    case MODE_STANDALONE: return "Standalone";
    default:              return "";
  }
}

// H / C / S - used in the footer
String getModeAbbrev(DeviceMode m) {
  switch (m) {
    case MODE_HOST:       return "H";
    case MODE_CLIENT:     return "C";
    case MODE_STANDALONE: return "S";
    default:              return "-";
  }
}

// H_ConnectLED (only one Host, so no number) / C_ConnectLED_01 / SA_ConnectLED_01
String deviceNameFor(DeviceMode m) {
  if (m == MODE_HOST) return String("H_") + DEVICE_GROUP_NAME;
  if (m == MODE_STANDALONE) return String("SA_") + DEVICE_GROUP_NAME + "_" + DEVICE_NUMBER;
  return String("C_") + DEVICE_GROUP_NAME + "_" + DEVICE_NUMBER; // Client (also used before a mode is decided)
}

// Switches mode, renames the device, clears any Host link / Client list, and switches to that
// mode's theme. Lasts until power-off.
void setMode(DeviceMode mode) {
  deviceMode = mode;
  fullDeviceName = deviceNameFor(mode);
  strncpy(myName, fullDeviceName.c_str(), sizeof(myName) - 1);
  hostMacKnown = false;
  clientLinked = false;
  hostName[0] = '\0';
  lastClientHelloTime = 0;      // A new Client checks in with its Host straight away
  clearClientRoster();
  if (mode == MODE_HOST) lastHostAnnounceTime = millis() - HOST_ANNOUNCE_INTERVAL_MS; // Announce straight away

  if (mode == MODE_CLIENT && itemActivated) { // Drop any edit in progress - the Host is in charge now
    itemActivated = false;
    if (currentAnimation == NONE) ledcWrite(LED_PWM_PIN, brightnessToPWM(brightnessPercent)); // Undo live preview
  }
  if (mode != MODE_CLIENT && !isItemSelectable(currentSelection)) currentSelection = 1;

  // While the popup is open the old theme stays, so the popup and the screen behind it match.
  // closePopup() switches the theme and repaints everything.
  if (!popupOpen) {
    applyTheme(mode);
    if (menuShown && !screenSleeping) drawFullMenu();
  }

  if (Debug) Serial.printf("Mode: %s (%s)\n", getModeName(mode).c_str(), fullDeviceName.c_str());
}

// Becomes a Client following the Host most recently heard by the radio.
void linkToHeardHost() {
  setMode(MODE_CLIENT);
  memcpy(hostMac, heardHostMac, 6);
  strncpy(hostName, heardHostName, sizeof(hostName) - 1);
  hostMacKnown = true;
  clientLinked = true;
  lastHostContactTime = millis();
}

//------------------------------------------------------------------- ESP-NOW MULTI-DEVICE SYNC
//
// One Host, any number of Clients, plus Standalone units - all running this exact same firmware.
// Every power-on searches for a Host; the Switch Mode popup can change the mode until power-off.
//
// Searching units broadcast MSG_HOST_QUERY; a Host replies with an announce straight away, so
// searches finish in a fraction of a second when a Host is around.
// Linked Clients send MSG_CLIENT_HELLO every CLIENT_HELLO_INTERVAL_MS so the Host knows who's
// there (that list is the Chase order).
//
// Safety net: if two Hosts ever hear each other, the one with the lower MAC address stays Host
// and the other becomes a Client.
//
// NOTE ON CORE VERSION: the receive-callback signature below (esp_now_recv_info_t) matches the
// ESP32 Arduino core v3.x API already used elsewhere in this file for ledcAttach/ledcWrite.

// Sends one ESP-NOW message and logs on failure.
bool sendEspNowMessage(const EspNowMessage &msg) {
  esp_err_t result = esp_now_send(broadcastAddress, (const uint8_t *)&msg, sizeof(msg));
  if (result != ESP_OK) {
    if (Debug) Serial.printf("ESP-NOW send failed (err %d)\n", result);
    return false;
  }
  return true;
}

// Host keep-alive. Carries the current state so a Client that joins mid-session catches up.
void broadcastAnnounce() {
  EspNowMessage msg = {};
  msg.msgType = MSG_ANNOUNCE;
  strncpy(msg.deviceName, myName, sizeof(msg.deviceName) - 1);
  msg.isHost = true;
  msg.brightnessPercent = brightnessPercent;
  msg.animation = currentAnimation;
  msg.speedPercent = speedPercent;
  sendEspNowMessage(msg);
}

// Host only: pushes the current Brightness/Animation/Speed to every Client.
// Call this any time one of those three values is confirmed.
void broadcastStateIfHost() {
  if (deviceMode != MODE_HOST) return;
  EspNowMessage msg = {};
  msg.msgType = MSG_STATE_SYNC;
  strncpy(msg.deviceName, myName, sizeof(msg.deviceName) - 1);
  msg.isHost = true;
  msg.brightnessPercent = brightnessPercent;
  msg.animation = currentAnimation;
  msg.speedPercent = speedPercent;
  sendEspNowMessage(msg);
}

// "Is there a Host out there?" - any Host replies with an announce.
void sendHostQuery() {
  EspNowMessage msg = {};
  msg.msgType = MSG_HOST_QUERY;
  strncpy(msg.deviceName, myName, sizeof(msg.deviceName) - 1);
  msg.isHost = false;
  sendEspNowMessage(msg);
}

// Client -> Host: "I'm here" - keeps this Client in the Host's Chase order.
void sendClientHello() {
  EspNowMessage msg = {};
  msg.msgType = MSG_CLIENT_HELLO;
  strncpy(msg.deviceName, myName, sizeof(msg.deviceName) - 1);
  msg.isHost = false;
  sendEspNowMessage(msg);
}

// Host -> one Client: "your turn - pulse for pulseMs".
void sendChasePulse(const char *target, unsigned long pulseMs) {
  EspNowMessage msg = {};
  msg.msgType = MSG_CHASE_PULSE;
  strncpy(msg.deviceName, myName, sizeof(msg.deviceName) - 1);
  msg.isHost = true;
  strncpy(msg.targetName, target, sizeof(msg.targetName) - 1);
  msg.pulseMs = (uint16_t)pulseMs;
  sendEspNowMessage(msg);
}

// Listens for up to windowMs for any Host, asking every HOST_QUERY_INTERVAL_MS.
// Keeps the LED animation running while it waits.
bool searchForHost(unsigned long windowMs) {
  hostHeard = false;
  unsigned long start = millis();
  unsigned long lastQuery = 0;
  bool firstQuery = true;
  while (millis() - start < windowMs) {
    if (hostHeard) return true;
    if (firstQuery || millis() - lastQuery >= HOST_QUERY_INTERVAL_MS) {
      sendHostQuery();
      lastQuery = millis();
      firstQuery = false;
    }
    runAnimations();
    delay(10);
  }
  return hostHeard;
}

// --- Host's Client list (written by the radio callback, read by the Chase) ---

void clearClientRoster() {
  portENTER_CRITICAL(&rosterMux);
  for (int i = 0; i < MAX_CLIENTS; i++) clientRoster[i].used = false;
  portEXIT_CRITICAL(&rosterMux);
}

// Adds a Client to the list, or refreshes it if it's already there. Called from the radio callback.
void rosterTouch(const uint8_t *mac, const char *name) {
  portENTER_CRITICAL(&rosterMux);
  int freeSlot = -1;
  bool found = false;
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clientRoster[i].used && memcmp(clientRoster[i].mac, mac, 6) == 0) {
      strncpy(clientRoster[i].name, name, sizeof(clientRoster[i].name) - 1);
      clientRoster[i].lastSeen = millis();
      found = true;
      break;
    }
    if (!clientRoster[i].used && freeSlot < 0) freeSlot = i;
  }
  if (!found && freeSlot >= 0) {
    clientRoster[freeSlot].used = true;
    memcpy(clientRoster[freeSlot].mac, mac, 6);
    memset(clientRoster[freeSlot].name, 0, sizeof(clientRoster[freeSlot].name));
    strncpy(clientRoster[freeSlot].name, name, sizeof(clientRoster[freeSlot].name) - 1);
    clientRoster[freeSlot].lastSeen = millis();
  }
  portEXIT_CRITICAL(&rosterMux);
}

// Fills chaseOrder with the Clients heard from recently, sorted by name (C_..._01, _02 ...).
// Drops any that have gone quiet. Returns how many there are.
int buildChaseOrder() {
  int n = 0;
  unsigned long now = millis();
  portENTER_CRITICAL(&rosterMux);
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!clientRoster[i].used) continue;
    if (now - clientRoster[i].lastSeen > CLIENT_ROSTER_TIMEOUT_MS) {
      clientRoster[i].used = false; // Gone quiet - out of the Chase
      continue;
    }
    chaseOrder[n++] = clientRoster[i];
  }
  portEXIT_CRITICAL(&rosterMux);

  // Insertion sort by name - the list is tiny
  for (int i = 1; i < n; i++) {
    ClientEntry key = chaseOrder[i];
    int j = i - 1;
    while (j >= 0 && strcmp(chaseOrder[j].name, key.name) > 0) {
      chaseOrder[j + 1] = chaseOrder[j];
      j--;
    }
    chaseOrder[j + 1] = key;
  }
  return n;
}

// Client only: applies a state update received from our Host and mirrors it onto the
// hardware, reusing the same start/stop functions the local menu uses.
void applyReceivedState(const EspNowMessage &msg) {
  brightnessPercent = msg.brightnessPercent;
  tempBrightness = brightnessPercent;
  AnimationType oldAnimation = currentAnimation;
  currentAnimation = msg.animation;
  speedPercent = msg.speedPercent;
  tempSpeed = speedPercent;

  if (currentAnimation == NONE) {
    stopCurrentAnimation();
  } else if (currentAnimation != oldAnimation) {
    startAnimation(currentAnimation);
  }
  // If the animation type didn't change, its own runner already picks up the new
  // brightnessPercent/speedPercent on its next toggle - nothing else to push here.

  updateMenuItemDisplay(1); // Brightness
  updateMenuItemDisplay(2); // Animation
  updateMenuItemDisplay(3); // Speed
}

// ESP-NOW receive callback. Runs in the WiFi/LWIP task, not the main loop - keep it quick.
void onEspNowReceive(const esp_now_recv_info_t *recvInfo, const uint8_t *incomingData, int len) {
  if (len != sizeof(EspNowMessage)) return; // Not our packet shape - ignore

  EspNowMessage msg;
  memcpy(&msg, incomingData, sizeof(msg));
  msg.deviceName[sizeof(msg.deviceName) - 1] = '\0';
  msg.targetName[sizeof(msg.targetName) - 1] = '\0';

  // Ignore anything outside our group
  if (strstr(msg.deviceName, DEVICE_GROUP_NAME) == nullptr) return;

  if (msg.msgType == MSG_HOST_QUERY) {
    if (deviceMode == MODE_HOST) hostAnnounceRequested = true; // Reply from loop()
    return;
  }
  if (msg.msgType == MSG_CLIENT_HELLO) {
    if (deviceMode == MODE_HOST) rosterTouch(recvInfo->src_addr, msg.deviceName);
    return;
  }
  if (msg.msgType == MSG_CHASE_PULSE) {
    if (deviceMode == MODE_CLIENT && hostMacKnown && memcmp(recvInfo->src_addr, hostMac, 6) == 0) {
      lastHostContactTime = millis();
      if (strcmp(msg.targetName, myName) == 0) { // Our turn - pulse from loop()
        chasePulseRequestMs = msg.pulseMs;
        chasePulseRequested = true;
      }
    }
    return;
  }
  if (!msg.isHost) return;

  // Remember the most recent Host heard - used by searches and the popup
  memcpy(heardHostMac, recvInfo->src_addr, 6);
  strncpy(heardHostName, msg.deviceName, sizeof(heardHostName) - 1);
  hostHeard = true;

  if (deviceMode == MODE_CLIENT) {
    if (!hostMacKnown) { // Searching / just switched to Client - follow this Host
      memcpy(hostMac, recvInfo->src_addr, 6);
      strncpy(hostName, msg.deviceName, sizeof(hostName) - 1);
      hostMacKnown = true;
    }
    if (memcmp(recvInfo->src_addr, hostMac, 6) == 0) {
      lastHostContactTime = millis();
      clientLinked = true;
      memcpy(&pendingStateMsg, &msg, sizeof(msg)); // Applied from loop()
      pendingStateUpdate = true;
    }
  } else if (deviceMode == MODE_HOST) {
    // Two Hosts at once (shouldn't happen) - lower MAC address wins, the other becomes a Client
    uint8_t ourMac[6];
    WiFi.macAddress(ourMac);
    if (memcmp(recvInfo->src_addr, ourMac, 6) < 0) hostConflictDetected = true;
  }
}

void setupEspNow() {
  fullDeviceName = deviceNameFor(MODE_UNSET);
  strncpy(myName, fullDeviceName.c_str(), sizeof(myName) - 1);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(); // STA mode only for the radio - not actually joining a network

  if (esp_now_init() != ESP_OK) {
    if (Debug) Serial.println("ESP-NOW init failed! Sync will not work on this device.");
    return;
  }
  esp_now_register_recv_cb(onEspNowReceive);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    if (Debug) Serial.println("Failed to add ESP-NOW broadcast peer.");
  }

  if (Debug) Serial.println("ESP-NOW ready.");
}

// Called every loop(): a Client that's stopped hearing its Host starts searching again.
// It rejoins automatically as soon as a Host is heard.
void checkHostHeartbeat() {
  if (deviceMode != MODE_CLIENT || !clientLinked) return;
  unsigned long last = lastHostContactTime;
  if ((long)(millis() - last) > (long)HOST_HEARTBEAT_TIMEOUT_MS) {
    clientLinked = false;
    hostMacKnown = false;
    drawNotificationText(NOTIFICATION_TEXT_Y, "Host not found, reconnecting...");
    if (Debug) Serial.println("Lost contact with Host - reconnecting.");
  }
}

//------------------------------------------------------------------- GLOW / THEME HELPERS
// Everything is built from backgroundColor, glowColor and glowIntensity (set by applyTheme).

// Scales a glow strength (0-255) by glowIntensity (0-100)
uint8_t glowLevel(uint16_t base) {
  uint8_t pct = (glowIntensity > 100) ? 100 : glowIntensity;
  return (uint8_t)((base * pct) / 100);
}

uint16_t restEdge()   { return tft.alphaBlend(200, glowColor, backgroundColor); }   // Frame edge, not selected
uint16_t hotEdge()    { return tft.alphaBlend(70, TFT_WHITE, glowColor); }          // Frame edge, selected (near-white hot core)
uint16_t panelFill()  { return tft.alphaBlend(18,  glowColor, backgroundColor); }   // Inside of a panel
uint16_t activeFill() { return tft.alphaBlend(42,  glowColor, backgroundColor); }   // Inside of a panel being edited
uint16_t dividerCol() { return tft.alphaBlend(70, glowColor, panelFill()); }        // Thin lines inside panels

// Blends a colour toward the panel fill - level 255 = full colour, 0 = invisible (used for fades)
uint16_t fadeCol(TFT_eSPI &g, uint16_t col, uint8_t level) {
  return (level >= 255) ? col : g.alphaBlend(level, col, panelFill());
}

// Outline of a box with its top-left and bottom-right corners cut at 45 degrees
void drawChamferOutline(TFT_eSPI &g, int x, int y, int w, int h, int c, uint16_t col) {
  int r = x + w - 1, b = y + h - 1;
  g.drawLine(x + c, y, r, y, col);
  g.drawLine(r, y, r, b - c, col);
  g.drawLine(r, b - c, r - c, b, col);
  g.drawLine(r - c, b, x, b, col);
  g.drawLine(x, b, x, y + c, col);
  g.drawLine(x, y + c, x + c, y, col);
}

// Filled angled-corner panel with a glowing edge.
//   edge    = edge colour        fill    = inside colour
//   glow    = glow strength 0-255 (0 = none) - usually from glowLevel()
//   outerBg = colour around the panel (corners and outer glow blend into this)
// The outer glow spreads GLOW_W px outside the panel, so leave that much room around it.
void drawGlowFrame(TFT_eSPI &g, int x, int y, int w, int h, int c, uint16_t edge, uint16_t fill, uint8_t glow, uint16_t outerBg) {
  // Body, with the two corners cut back to the outside colour
  g.fillRect(x, y, w, h, fill);
  g.fillTriangle(x, y, x + c, y, x, y + c, outerBg);
  g.fillTriangle(x + w - 1, y + h - 1, x + w - 1 - c, y + h - 1, x + w - 1, y + h - 1 - c, outerBg);

  if (glow > 0) {
    // Outer glow - outlines stepping outward, fading away (squared falloff = soft edge)
    for (int i = GLOW_W; i >= 1; i--) {
      float f = (float)(GLOW_W + 1 - i) / (GLOW_W + 1); // 1 next to the edge -> 0 far away
      uint8_t a = (uint8_t)(glow * f * f);
      if (a == 0) continue;
      drawChamferOutline(g, x - i, y - i, w + 2 * i, h + 2 * i, c + i / 2, g.alphaBlend(a, edge, outerBg));
    }
    // Inner glow - the edge colour bleeding inward into the panel
    for (int i = INNER_GLOW_W + 1; i >= 2; i--) {
      float f = (float)(INNER_GLOW_W + 2 - i) / (INNER_GLOW_W + 1);
      uint8_t a = (uint8_t)(glow * 0.6f * f * f);
      if (a == 0) continue;
      drawChamferOutline(g, x + i, y + i, w - 2 * i, h - 2 * i, c, g.alphaBlend(a, edge, fill));
    }
  }

  // Crisp 2px edge
  drawChamferOutline(g, x, y, w, h, c, edge);
  drawChamferOutline(g, x + 1, y + 1, w - 2, h - 2, c, edge);
}

// Thin angled frame around the whole screen (drawn last so nothing covers it)
void drawScreenFrame() {
  drawChamferOutline(tft, 0, 0, SW, SH, 14, restEdge());
}

// Small purple/cyan diamond logo
void drawDiamond(TFT_eSPI &g, int cx, int cy, int r) {
  g.fillTriangle(cx, cy - r, cx - r, cy, cx + r, cy, VORTEX_COL_PURPLE); // Top half
  g.fillTriangle(cx - r, cy, cx + r, cy, cx, cy + r, VORTEX_COL_CYAN);   // Bottom half
  g.drawLine(cx - r, cy, cx, cy - r, TFT_WHITE);                         // Highlight edge
}

// Diamond logo + bold "ChannelLED" (L=red, E=blue, D=green), left-aligned at x, centred on y.
// The only bold text on the screen. large = boot screen size, otherwise header size.
void drawLogoTitle(TFT_eSPI &g, int x, int y, bool large) {
  const char *parts[4]   = { "Channel", "L", "E", "D" };
  const uint16_t cols[4] = { TFT_WHITE, TFT_RED, TFT_BLUE, TFT_GREEN };
  int r = large ? 9 : 6;
  drawDiamond(g, x + r, y, r);
  x += r * 2 + (large ? 8 : 6);

  if (large) g.setFreeFont(&FreeSansBold12pt7b);
  else g.setFreeFont(&FreeSansBold9pt7b);
  g.setTextSize(1);
  g.setTextDatum(ML_DATUM);
  for (int i = 0; i < 4; i++) {
    g.setTextColor(cols[i]);
    g.drawString(parts[i], x, y);
    x += g.textWidth(parts[i]);
  }
  g.setTextFont(2);
  g.setTextDatum(TL_DATUM);
}

// Horizontal gradient slider with a white knob. colA = left end, colB = right end of the fill.
void drawGradientSlider(TFT_eSPI &g, int x, int y, int w, int value, uint16_t colA, uint16_t colB,
                        bool active, bool locked, uint16_t bg) {
  int h = 6;
  g.fillRoundRect(x, y, w, h, h / 2, g.alphaBlend(70, MENU_COL_MUTED, bg)); // Track
  if (locked) return;

  int fw = map(value, 0, 100, 0, w);
  for (int i = 0; i < fw; i++) {
    uint16_t c = g.alphaBlend((uint8_t)((i * 255) / (w > 1 ? w - 1 : 1)), colB, colA);
    g.drawFastVLine(x + i, y, h, c);
  }

  int kx = constrain(x + fw, x + 6, x + w - 6);
  int ky = y + h / 2;
  if (active) g.fillCircle(kx, ky, 9, g.alphaBlend(glowLevel(200), glowColor, bg)); // Glow while editing
  g.fillCircle(kx, ky, 6, TFT_WHITE);
  g.drawCircle(kx, ky, 7, active ? glowColor : g.alphaBlend(140, colA, bg));
}

// Small chevron arrow, dir = -1 points left, +1 points right
void drawChevron(TFT_eSPI &g, int x, int y, int dir, uint16_t col) {
  for (int k = 0; k < 2; k++) {
    g.drawLine(x - 3 * dir + k * dir, y - 5, x + 2 * dir + k * dir, y, col);
    g.drawLine(x + 2 * dir + k * dir, y, x - 3 * dir + k * dir, y + 5, col);
  }
}


//------------------------------------------------------------------- BOOT VORTEX ANIMATION

// (Re)spawns one particle on the outer edge, on one of the two spiral arms.
// randomRadius = true scatters it anywhere along the arm (used on the very first frame).
void spawnVortexParticle(int i, bool randomRadius) {
  const uint16_t cool[3] = { VORTEX_COL_CYAN, VORTEX_COL_PURPLE, VORTEX_COL_MAGENTA };
  const uint16_t warm[3] = { VORTEX_COL_ORANGE, VORTEX_COL_GOLD, VORTEX_COL_EMBER };
  VortexParticle &p = vortex[i];

  int arm = random(2); // 0 = cool arm, 1 = warm arm
  p.radius = randomRadius ? random(VORTEX_CORE_R, VORTEX_SPAWN_R) : VORTEX_SPAWN_R - random(0, 10);
  // Arms sit opposite each other; the angle offset grows with radius so they curl into a spiral
  p.angle  = arm * PI + p.radius * VORTEX_ARM_TWIST + random(-30, 31) / 100.0;
  p.speed  = random(5, 15) / 10.0;
  p.colour = (arm == 0) ? cool[random(3)] : warm[random(3)];
}

void initVortex() {
  for (int i = 0; i < VORTEX_PARTICLES; i++) spawnVortexParticle(i, true);
  for (int i = 0; i < VORTEX_STARS; i++) {
    starX[i] = random(0, VORTEX_SPR_W);
    starY[i] = random(0, VORTEX_AREA_H);
  }
  orbEnergy = 0;
}

// Angular speed for a given radius - spins faster near the centre, like water down a drain.
float vortexOmega(float radius) {
  return VORTEX_SPIN * (VORTEX_SPAWN_R / (radius + 10.0));
}

// Adds energy to the orb (capped at 1.0).
void feedOrb(float amount) {
  orbEnergy += amount;
  if (orbEnergy > 1.0) orbEnergy = 1.0;
}

// Moves every particle one frame: round and inward. When one reaches the core it's absorbed
// into the orb (feeding it) and respawns at the outer edge.
void updateVortex() {
  orbEnergy *= VORTEX_ORB_DECAY; // Orb settles back down between feeds

  for (int i = 0; i < VORTEX_PARTICLES; i++) {
    VortexParticle &p = vortex[i];
    p.angle  += vortexOmega(p.radius);
    p.radius -= p.speed;
    if (p.radius < VORTEX_CORE_R) {
      feedOrb(VORTEX_ORB_FEED);
      spawnVortexParticle(i, false);
    }
  }
}

// Ring colour at angle a: cyan -> purple -> orange and back, with no seam
uint16_t vortexRingColour(TFT_eSPI &g, float a, float spin) {
  float t = (1.0 + cos(a + spin)) * 0.5; // 0..1
  if (t < 0.5) return g.alphaBlend((uint8_t)(t * 2 * 255), VORTEX_COL_PURPLE, VORTEX_COL_CYAN);
  return g.alphaBlend((uint8_t)((t - 0.5) * 2 * 255), VORTEX_COL_ORANGE, VORTEX_COL_PURPLE);
}

// Outer gradient ring with a glow halo (draws itself in on boot), plus a dashed inner ring
// turning the other way.
void drawVortexRing(TFT_eSPI &gfx, unsigned long elapsedMs) {
  int cx = VORTEX_CX;
  float sweep = elapsedMs / (float)VORTEX_RING_DRAW_MS;
  if (sweep > 1.0) sweep = 1.0;
  int steps = (int)(720 * sweep); // Half-degree steps - no gaps at this radius
  float spin = elapsedMs * 0.0004;
  uint8_t haloBase = glowLevel(180);

  for (int i = 0; i < steps; i++) {
    float a = i * 0.5 * DEG_TO_RAD - HALF_PI; // Start drawing from the top
    float ca = cos(a), sa = sin(a);
    uint16_t c = vortexRingColour(gfx, a, spin);

    for (int r = VORTEX_RING_R - 1; r <= VORTEX_RING_R + 1; r++) {
      gfx.drawPixel(cx + r * ca, VORTEX_CY + r * sa, c);
    }
    for (int k = 1; k <= 4; k++) { // Halo fading out on both sides of the ring
      uint8_t al = (haloBase * (5 - k)) / 5;
      if (al == 0) break;
      uint16_t gc = gfx.alphaBlend(al, c, backgroundColor);
      gfx.drawPixel(cx + (VORTEX_RING_R + 1 + k) * ca, VORTEX_CY + (VORTEX_RING_R + 1 + k) * sa, gc);
      gfx.drawPixel(cx + (VORTEX_RING_R - 1 - k) * ca, VORTEX_CY + (VORTEX_RING_R - 1 - k) * sa, gc);
    }
  }

  // Dashed inner ring
  float spin2 = -elapsedMs * 0.0008;
  uint16_t dash = gfx.alphaBlend(150, glowColor, backgroundColor);
  for (int d = 0; d < 360; d++) {
    if ((d % 24) >= 14) continue; // Gap between dashes
    float a = d * DEG_TO_RAD + spin2;
    gfx.drawPixel(cx + VORTEX_INNER_R * cos(a), VORTEX_CY + VORTEX_INNER_R * sin(a), dash);
  }
}

// Central energy orb: white-hot centre fading out to a colour that drifts between purple and
// orange. Pulses gently on its own, and swells/brightens with orbEnergy as it's fed.
void drawVortexOrb(TFT_eSPI &gfx, unsigned long elapsedMs) {
  int cx = VORTEX_CX;
  float pulse = VORTEX_ORB_PULSE * sin(elapsedMs * 0.006);
  int outerR = (int)(VORTEX_ORB_R + pulse + orbEnergy * VORTEX_ORB_GROW);

  // Edge colour slowly drifts between the two arm colours - the orb is fed by both
  uint8_t hueMix = (uint8_t)(127.5 + 127.5 * sin(elapsedMs * 0.0015));
  uint16_t edge = gfx.alphaBlend(hueMix, VORTEX_COL_ORANGE, VORTEX_COL_PURPLE);

  // Draw from the outside in, each layer a little hotter (whiter) than the last
  for (int r = outerR; r > 0; r--) {
    float f = r / (float)outerR;               // 1.0 = outer edge, ~0 = centre
    float hot = (1.0 - f) * (1.0 - f) + orbEnergy * 0.4; // More energy = whiter overall
    if (hot > 1.0) hot = 1.0;
    uint16_t c = gfx.alphaBlend((uint8_t)(hot * 255), TFT_WHITE, edge);

    // Soft outer edge - fades into the background over the outer half
    uint8_t vis = (f < 0.5) ? 255 : (uint8_t)(255 * (1.0 - (f - 0.5) * 1.6));
    gfx.fillCircle(cx, VORTEX_CY, r, gfx.alphaBlend(vis, c, backgroundColor));
  }
}

// Renders one full vortex frame and pushes it to the screen in one go (no flicker).
// Layer order: stars -> swirl particles -> lightning -> rings -> orb.
void drawVortexFrame(TFT_eSprite &spr, unsigned long elapsedMs) {
  int cx = VORTEX_CX;
  spr.fillSprite(backgroundColor);

  // Twinkling stars
  for (int i = 0; i < VORTEX_STARS; i++) {
    uint8_t level = (uint8_t)(128 + 127 * sin(elapsedMs * 0.003 + i * 1.7));
    spr.drawPixel(starX[i], starY[i], spr.alphaBlend(level, TFT_WHITE, backgroundColor));
  }

  // Swirl particles - a dim streak behind a bright head, heating up white as they near the orb
  for (int i = 0; i < VORTEX_PARTICLES; i++) {
    VortexParticle &p = vortex[i];
    float omega = vortexOmega(p.radius);

    // Fade in near the outer edge so particles don't pop into existence
    float fadeIn = (VORTEX_SPAWN_R - p.radius) * 20.0;
    uint8_t alpha = fadeIn > 255 ? 255 : (fadeIn < 0 ? 0 : (uint8_t)fadeIn);

    // Heat up toward white as they're drawn into the orb
    float heat = (p.radius < VORTEX_HEAT_R) ? (VORTEX_HEAT_R - p.radius) / (float)(VORTEX_HEAT_R - VORTEX_CORE_R) : 0;
    if (heat > 1.0) heat = 1.0;
    uint16_t base = spr.alphaBlend((uint8_t)(heat * 200), TFT_WHITE, p.colour);

    uint16_t head = spr.alphaBlend(alpha, base, backgroundColor);
    uint16_t tail = spr.alphaBlend(alpha / 2, base, backgroundColor);

    float hx = cx + p.radius * cos(p.angle);
    float hy = VORTEX_CY + p.radius * sin(p.angle);
    float tr = p.radius + p.speed * 4;
    float ta = p.angle - omega * 4;
    float tx = cx + tr * cos(ta);
    float ty = VORTEX_CY + tr * sin(ta);

    spr.drawLine(tx, ty, hx, hy, tail);
    spr.fillRect(hx - 1, hy - 1, 2, 2, head);
  }

  // Occasional lightning bolt - jagged path from the edge into the orb, one frame long.
  // A bolt that reaches the core makes the orb flare.
  if (random(100) < VORTEX_CRACKLE_CHANCE) {
    float a = random(0, 628) / 100.0;
    float r = VORTEX_SPAWN_R;
    int px = cx + r * cos(a);
    int py = VORTEX_CY + r * sin(a);
    for (int s = 0; s < 8; s++) {
      r -= random(8, 16);
      a += random(-30, 31) / 100.0 + 0.15;
      bool hitCore = (r <= VORTEX_CORE_R);
      if (hitCore) r = VORTEX_CORE_R;
      int nx = cx + r * cos(a);
      int ny = VORTEX_CY + r * sin(a);
      spr.drawLine(px, py, nx, ny, TFT_WHITE);
      px = nx;
      py = ny;
      if (hitCore) {
        feedOrb(VORTEX_ORB_BOLT_FEED);
        break;
      }
    }
  }

  drawVortexRing(spr, elapsedMs);
  drawVortexOrb(spr, elapsedMs); // Drawn over the particles so they vanish into it
  spr.pushSprite(VORTEX_SPR_X, VORTEX_SPR_Y);
}

// Status strip under the vortex: status line (white), time remaining (grey), glowing progress bar.
void drawBootText(TFT_eSprite &txt, const char *line1, const String &line2, float progress) {
  int w = VORTEX_SPR_W;
  txt.fillSprite(backgroundColor);
  txt.setTextFont(2);
  txt.setTextSize(1);
  txt.setTextDatum(MC_DATUM);
  txt.setTextColor(MENU_COL_TEXT);
  txt.drawString(line1, w / 2, 9);
  txt.setTextColor(MENU_COL_BODY);
  txt.drawString(line2, w / 2, 26);

  // Progress bar - gradient glow colour -> purple with a soft glow above and below
  int bw = 160, bx = (w - bw) / 2, by = 40, bh = 4;
  if (progress < 0) progress = 0;
  if (progress > 1) progress = 1;
  txt.fillRoundRect(bx, by, bw, bh, 2, txt.alphaBlend(50, glowColor, backgroundColor));
  int fw = (int)(bw * progress);
  for (int i = 0; i < fw; i++) {
    uint16_t c = txt.alphaBlend((uint8_t)((i * 255) / bw), VORTEX_COL_PURPLE, glowColor);
    txt.drawFastVLine(bx + i, by, bh, c);
    txt.drawPixel(bx + i, by + bh, txt.alphaBlend(glowLevel(140), c, backgroundColor)); // Glow below
    txt.drawPixel(bx + i, by - 1, txt.alphaBlend(glowLevel(140), c, backgroundColor));  // Glow above
  }
  txt.setTextDatum(TL_DATUM);
  txt.pushSprite(VORTEX_SPR_X, BOOT_TEXT_Y);
}

// Boot screen: searches for a Host for up to HOST_SEARCH_WINDOW_MS.
//   Host heard    -> becomes a Client of it
//   none heard    -> becomes Host
//   Enter pressed -> skips the search and runs Standalone
// Everything animated is drawn into sprites first and pushed whole, so nothing flickers.
void runBootSequence() {
  hostHeard = false;

  // Static parts - drawn once
  tft.fillScreen(backgroundColor);
  drawScreenFrame();
  drawLogoTitle(tft, BOOT_TITLE_X, BOOT_TITLE_Y, true);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(MENU_COL_TEXT);
  tft.drawString("Press Enter to Skip", SW / 2, BOOT_HINT_Y);
  tft.setTextDatum(TL_DATUM);

  // Try full 16-bit colour first (~89KB) for smooth gradients; fall back to 8-bit (~45KB)
  // if RAM is short. If both fail, the boot still runs without the vortex.
  TFT_eSprite vortexSpr = TFT_eSprite(&tft);
  vortexSpr.setColorDepth(16);
  bool vortexOk = vortexSpr.createSprite(VORTEX_SPR_W, VORTEX_AREA_H) != nullptr;
  if (!vortexOk) {
    vortexSpr.setColorDepth(8);
    vortexOk = vortexSpr.createSprite(VORTEX_SPR_W, VORTEX_AREA_H) != nullptr;
    if (Debug && vortexOk) Serial.println("Vortex sprite: 16-bit failed, using 8-bit.");
  }
  TFT_eSprite txt = TFT_eSprite(&tft);
  txt.setColorDepth(16);
  bool txtOk = txt.createSprite(VORTEX_SPR_W, BOOT_TEXT_H) != nullptr;
  if (Debug && !vortexOk) Serial.println("Vortex sprite allocation failed - no animation.");

  initVortex();

  unsigned long start = millis();
  unsigned long lastFrame = 0;
  unsigned long lastText = 0;
  unsigned long lastQuery = 0;
  bool firstQuery = true;
  bool firstText = true;
  bool hostFound = false;
  bool skipped = false;

  while (millis() - start < HOST_SEARCH_WINDOW_MS) {
    if (hostHeard) { hostFound = true; break; }
    if (digitalRead(OK_BTN_PIN) == LOW) { skipped = true; break; }

    if (firstQuery || millis() - lastQuery >= HOST_QUERY_INTERVAL_MS) {
      sendHostQuery();
      lastQuery = millis();
      firstQuery = false;
    }

    unsigned long elapsed = millis() - start;
    if (vortexOk && elapsed - lastFrame >= BOOT_FRAME_MS) {
      lastFrame = elapsed;
      updateVortex();
      drawVortexFrame(vortexSpr, elapsed);
    }

    if (txtOk && (firstText || elapsed - lastText >= 100)) { // Every 100ms so the bar moves smoothly
      firstText = false;
      lastText = elapsed;
      long remainingSec = (HOST_SEARCH_WINDOW_MS - elapsed) / 1000 + 1;
      drawBootText(txt, "Searching for Host...", String(remainingSec) + "s remaining",
                   elapsed / (float)HOST_SEARCH_WINDOW_MS);
    }

    delay(5); // The receive callback still fires during this - it runs on its own task
  }

  // Free the sprite RAM - only needed for the boot screen
  vortexSpr.deleteSprite();
  txt.deleteSprite();

  if (skipped) {
    // OK is still physically down - mark it as already pressed so loop() doesn't treat it as a fresh press
    okBtnRawState = okBtnDebouncedState = LOW;
    setMode(MODE_STANDALONE);
    if (Debug) Serial.println("Enter pressed - Standalone.");
  } else if (hostFound) {
    linkToHeardHost();
  } else {
    setMode(MODE_HOST);
  }
}

//------------------------------------------------------------------- BUTTON SHORTCUTS

// Shared by the button shortcuts: kills any animation, sets a fixed brightness, and backs out
// of whatever menu item was being edited (its temp value is discarded).
void applyShortcutReset(int newBrightness, const String &message) {
  resetInactivityTimer(); // Wakes the screen if it was asleep

  itemActivated = false;
  brightnessPercent = newBrightness;
  tempBrightness = newBrightness;
  tempAnimation = NONE;
  stopCurrentAnimation(); // Sets currentAnimation = NONE and writes brightnessPercent to the LED

  // Speed just became locked - if it was hovered, move to Brightness
  if (!isItemSelectable(currentSelection)) currentSelection = 1;

  for (int i = 1; i <= 3; i++) updateMenuItemDisplay(i);
  drawNotificationText(NOTIFICATION_TEXT_Y, message);
  broadcastStateIfHost();
  if (Debug) Serial.println("Shortcut: " + message);
}

//------------------------------------------------------------------- SWITCH MODE POPUP
//
// Opened by holding OK (1s, release). While open it takes every button press.
// Select screen focus order: New Mode selector -> Update -> Cancel
//   selector: - / + pick the New Mode, OK moves to Update
//   Update:   OK applies, + moves to Cancel, - goes back to the selector
//   Cancel:   OK returns to the main menu, - moves back to Update
// Holding OK again also cancels.
// The theme doesn't change while the popup is open - the new mode's colours appear when it closes.
//
//   -> Standalone          : always allowed. Leaving Host warns that Clients are left without one.
//   -> Client              : searches for a Host and links to it. If none is found:
//                              from Host       -> waits as a Client until a Host appears (with warning)
//                              from Standalone -> asks "Set this device as the new Host?"
//   -> Host                : refused if a Host already exists ("set it to Client or Standalone first")

// Next New Mode option in direction dir, never offering the current mode.
DeviceMode nextModeOption(DeviceMode from, int dir) {
  DeviceMode m = from;
  do {
    m = (DeviceMode)((((int)m - 1 + dir + 3) % 3) + 1); // Cycles Host -> Client -> Standalone
  } while (m == deviceMode);
  return m;
}

void openModePopup() {
  resetInactivityTimer();
  // Abandon any menu edit in progress - the popup takes over all input
  if (itemActivated) {
    itemActivated = false;
    if (currentAnimation == NONE) ledcWrite(LED_PWM_PIN, brightnessToPWM(brightnessPercent)); // Undo live preview
    updateMenuItemDisplay(currentSelection);
  }
  drawNotificationText(NOTIFICATION_TEXT_Y, ""); // Clear the "Release to..." prompt
  popupOpen = true;
  popupNewMode = nextModeOption(deviceMode, 1);
  showPopupSelect();
}

// Holding OK while the popup is open. A mode already applied (result screen) is kept;
// an unanswered "set as new Host?" goes back to the previous mode.
void cancelPopup() {
  if (popupScreen == POP_RESULT) {
    closePopup("");
    return;
  }
  if (popupScreen == POP_CONFIRM_HOST) setMode(popupPrevMode);
  closePopup("Mode unchanged");
}

// Back to the main menu, switched to the current mode's theme. Shows message in the info box if given.
void closePopup(const String &message) {
  popupOpen = false;
  applyTheme(deviceMode);
  drawFullMenu();
  if (message.length()) drawNotificationText(NOTIFICATION_TEXT_Y, message);
}

// Word-wraps text centred on cx, starting at y. Returns the y just below the last line.
int drawWrapped(const String &text, int cx, int y, int maxW, int lineH, uint16_t col) {
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(col);
  tft.setTextDatum(TC_DATUM);
  String line = "";
  String word = "";
  for (unsigned int i = 0; i <= text.length(); i++) {
    char ch = (i < text.length()) ? text[i] : ' ';
    if (ch == ' ') {
      String trial = line.length() ? line + " " + word : word;
      if (line.length() && tft.textWidth(trial) > maxW) {
        tft.drawString(line, cx, y);
        y += lineH;
        line = word;
      } else {
        line = trial;
      }
      word = "";
    } else {
      word += ch;
    }
  }
  if (line.length()) {
    tft.drawString(line, cx, y);
    y += lineH;
  }
  tft.setTextDatum(TL_DATUM);
  return y;
}

// Popup panel with its title (white, regular weight) and a divider under it
void drawPopupFrame(const char *title) {
  tft.fillRect(POP_X - GLOW_W, POP_Y - GLOW_W, POP_W + 2 * GLOW_W, POP_H + 2 * GLOW_W, backgroundColor);
  drawGlowFrame(tft, POP_X, POP_Y, POP_W, POP_H, 12, hotEdge(), panelFill(), glowLevel(255), backgroundColor);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(MENU_COL_TEXT);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(title, SW / 2, POP_Y + 20);
  tft.drawFastHLine(POP_X + 14, POP_Y + 36, POP_W - 28, dividerCol());
  tft.setTextDatum(TL_DATUM);
}

// Angled-corner button: filled glow colour when selected, outlined with grey text otherwise
void drawPopupButton(int x, int y, int w, const char *label, bool selected) {
  int h = 30;
  drawGlowFrame(tft, x, y, w, h, 6, selected ? glowColor : restEdge(),
                selected ? glowColor : panelFill(), 0, panelFill());
  tft.setTextFont(2);
  tft.setTextColor(selected ? backgroundColor : MENU_COL_BODY);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + w / 2, y + h / 2);
  tft.setTextDatum(TL_DATUM);
}

// The "< Client >" selector under New Mode - glow colour while it has focus, grey otherwise
void drawPopupModePill() {
  int w = 160, h = 28;
  int x = SW / 2 - w / 2;
  int y = POP_Y + 112;
  bool focused = (popupSelectFocus == 0);
  uint16_t edge = focused ? glowColor : restEdge();

  drawGlowFrame(tft, x, y, w, h, 6, edge, focused ? activeFill() : panelFill(), 0, panelFill());
  drawChevron(tft, x + 14, y + h / 2, -1, focused ? glowColor : MENU_COL_MUTED);
  drawChevron(tft, x + w - 14, y + h / 2, 1, focused ? glowColor : MENU_COL_MUTED);
  tft.setTextFont(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(focused ? glowColor : MENU_COL_BODY);
  tft.drawString(getModeName(popupNewMode), SW / 2, y + h / 2);
  tft.setTextDatum(TL_DATUM);
}

// Update + Cancel buttons - the focused one is filled
void drawPopupSelectButtons() {
  drawPopupButton(SW / 2 - 96, POP_Y + 152, 90, "Update", popupSelectFocus == 1);
  drawPopupButton(SW / 2 + 6,  POP_Y + 152, 90, "Cancel", popupSelectFocus == 2);
}

void showPopupSelect() {
  popupScreen = POP_SELECT;
  popupSelectFocus = 0; // Start on the New Mode selector
  drawPopupFrame("Toggle Mode");

  // "Current Mode: Host" - label in white, value in grey, centred as one line
  String label = "Current Mode: ";
  String value = getModeName(deviceMode);
  tft.setTextFont(2);
  int labelW = tft.textWidth(label);
  int valueW = tft.textWidth(value);
  int x = SW / 2 - (labelW + valueW) / 2;
  int y = POP_Y + 62;
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(MENU_COL_TEXT);
  tft.drawString(label, x, y);
  tft.setTextColor(MENU_COL_BODY);
  tft.drawString(value, x + labelW, y);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(MENU_COL_TEXT);
  tft.drawString("New Mode:", SW / 2, POP_Y + 98);
  drawPopupModePill();
  drawPopupSelectButtons();
  tft.setTextDatum(TL_DATUM);
}

// "Please wait" screen shown during a Host search
void showPopupWorking(const String &message) {
  drawPopupFrame("Toggle Mode");
  drawWrapped(message, SW / 2, POP_Y + 96, POP_W - 28, 16, MENU_COL_BODY);
}

void drawPopupMessageButtons() {
  int y = POP_Y + POP_H - 44;
  if (popupScreen == POP_CONFIRM_HOST) {
    drawPopupButton(SW / 2 - 88, y, 80, "No", !popupYesSelected);
    drawPopupButton(SW / 2 + 8, y, 80, "Yes", popupYesSelected);
  } else {
    drawPopupButton(SW / 2 - 45, y, 90, "OK", true);
  }
}

// Result / question screen: wrapped grey body text, optional orange warning, then OK or No/Yes
void showPopupMessage(PopupScreen screen, const char *title, const String &body, const String &warning) {
  popupScreen = screen;
  popupTitle = title;
  popupBody = body;
  popupWarning = warning;
  drawPopupFrame(title);
  int y = drawWrapped(body, SW / 2, POP_Y + 46, POP_W - 28, 15, MENU_COL_BODY);
  if (warning.length()) drawWrapped(warning, SW / 2, y + 6, POP_W - 28, 15, POP_COL_WARN);
  drawPopupMessageButtons();
}

// - / + while the popup is open
// Select screen focus order: New Mode selector -> Update -> Cancel
void popupNav(int dir) {
  resetInactivityTimer();
  if (popupScreen == POP_SELECT) {
    if (popupSelectFocus == 0) {                                   // On the selector - change the New Mode
      popupNewMode = nextModeOption(popupNewMode, dir);
      drawPopupModePill();
      return;
    }
    if (dir > 0 && popupSelectFocus == 1) popupSelectFocus = 2;      // Update -> Cancel
    else if (dir < 0 && popupSelectFocus == 2) popupSelectFocus = 1; // Cancel -> Update
    else if (dir < 0 && popupSelectFocus == 1) popupSelectFocus = 0; // Update -> back to the selector
    else return;                                                     // + on Cancel - already at the end
    drawPopupModePill();
    drawPopupSelectButtons();
  } else if (popupScreen == POP_CONFIRM_HOST) {
    popupYesSelected = !popupYesSelected;
    drawPopupMessageButtons();
  }
}

// OK (short press) while the popup is open
void popupOk() {
  resetInactivityTimer();
  switch (popupScreen) {
    case POP_SELECT:
      if (popupSelectFocus == 0) {        // New Mode accepted - move to the Update button
        popupSelectFocus = 1;
        drawPopupModePill();
        drawPopupSelectButtons();
      } else if (popupSelectFocus == 1) { // Update
        performModeChange();
      } else {                            // Cancel - straight back to the main menu
        closePopup("Mode unchanged");
      }
      break;
    case POP_CONFIRM_HOST: // "No Host found - set this device as the new Host?"
      if (popupYesSelected) {
        setMode(MODE_HOST);
        showPopupMessage(POP_RESULT, "Mode Updated",
          String("This device has now been set as Host (") + fullDeviceName + ").", String(""));
      } else {
        setMode(popupPrevMode); // Back to what it was
        popupNewMode = nextModeOption(deviceMode, 1);
        showPopupSelect();
      }
      break;
    case POP_HOST_EXISTS:
      showPopupSelect();
      break;
    case POP_RESULT:
      closePopup(""); // Mode + device name are shown in the footer
      break;
  }
}

// "Update" pressed - apply popupNewMode and show the outcome
void performModeChange() {
  DeviceMode target = popupNewMode;
  bool wasHost = (deviceMode == MODE_HOST);
  String noHostWarning = "Clients now have no Host. Promote another device to Host.";

  if (target == MODE_STANDALONE) {
    setMode(MODE_STANDALONE);
    showPopupMessage(POP_RESULT, "Mode Updated",
      String("This device is now Standalone and has been renamed to ") + fullDeviceName + ".",
      wasHost ? noHostWarning : String(""));
  }
  else if (target == MODE_CLIENT) {
    popupPrevMode = deviceMode;
    showPopupWorking("Searching for Host...");
    setMode(MODE_CLIENT);         // Client mode follows the first Host it hears
    searchForHost(HOST_CHECK_MS);

    if (hostMacKnown) {
      showPopupMessage(POP_RESULT, "Mode Updated",
        String("This device is now linked to Host ") + hostName + " and has been renamed to " + fullDeviceName + ".",
        String(""));
    } else if (wasHost) {
      showPopupMessage(POP_RESULT, "Mode Updated",
        "No other Host found. Now a Client - it will link when a Host appears.",
        noHostWarning);
    } else {
      popupYesSelected = false;
      showPopupMessage(POP_CONFIRM_HOST, "No Host Found",
        "Error - No Host found. Do you want to set this device as the new Host?", String(""));
    }
  }
  else if (target == MODE_HOST) {
    showPopupWorking("Checking for an existing Host...");
    bool alreadyLinked = (deviceMode == MODE_CLIENT && clientLinked);
    bool exists = alreadyLinked || searchForHost(HOST_CHECK_MS);

    if (exists) {
      String existingName = alreadyLinked ? String(hostName) : String(heardHostName);
      showPopupMessage(POP_HOST_EXISTS, "Host Already Exists",
        existingName + " is already the Host. Set it to Client or Standalone first, then try again.",
        String(""));
    } else {
      setMode(MODE_HOST);
      showPopupMessage(POP_RESULT, "Mode Updated",
        String("This device has now been set as Host (") + fullDeviceName + ").", String(""));
    }
  }
}

//------------------------------------------------------------------- MENU BUTTON FUNCTIONS

// Returns the increment size to use for a ramped repeat, based on how long the
// button has been held in total. A quick tap always moves by 1 (handled at the
// call site); once ramping kicks in, this grows in stages so holding sweeps
// across the 0-100 range far faster the longer it's held.
int getRampStepSize(unsigned long heldDurationMs) {
  if (heldDurationMs >= RAMP_ACCEL_STAGE3_MS) return RAMP_STEP_STAGE3;
  if (heldDurationMs >= RAMP_ACCEL_STAGE2_MS) return RAMP_STEP_STAGE2;
  if (heldDurationMs >= RAMP_ACCEL_STAGE1_MS) return RAMP_STEP_STAGE1;
  return RAMP_STEP_STAGE0;
}

// A Client follows its Host - Brightness, Animation, Speed and the 50% / All Off shortcuts are locked.
// Switch Mode (hold OK) still works.
bool controlsLocked() {
  return deviceMode == MODE_CLIENT;
}

// True if a main-menu item can currently be hovered/selected
bool isItemSelectable(int index) {
  if (index == 0) return false;                              // Status icons - shown, never selected
  if (controlsLocked()) return false;                        // Client - the Host sets everything
  if (index == 3 && currentAnimation == NONE) return false;  // Speed locked with no animation
  return true;
}

// - / + in the main menu (presses and ramps)
// amount: step size to move by - 1 for a single tap, larger while ramping.
void rotateAction(int direction, int amount) {
  resetInactivityTimer(); // Reset inactivity timer on any button movement

  if (controlsLocked()) return; // Client - nothing to change (the info box says "Controlled by Host")

  if (itemActivated) {
    // We are editing a value
    int effectiveDirection = SUB_DIR ? -direction : direction;

    if (currentSelection == 1) { // Brightness
      tempBrightness += (effectiveDirection * amount);
      tempBrightness = constrain(tempBrightness, 0, 100);

      // Live preview: with no animation running, push straight to the LED so
      // a steady light dims/brightens as you scroll, not just after OK.
      // Skipped during an animation - its own runner reads the confirmed
      // value each toggle, so a push here could land mid-off-phase and flicker.
      if (currentAnimation == NONE) {
        ledcWrite(LED_PWM_PIN, brightnessToPWM(tempBrightness));
      }
      updateMenuItemDisplay(currentSelection); 

    } else if (currentSelection == 2) { // Animation - (-) back, (+) forward, wraps around
      tempAnimation = (AnimationType)((tempAnimation + direction + ANIM_COUNT) % ANIM_COUNT); 
      updateMenuItemDisplay(currentSelection); 

    } else if (currentSelection == 3) { // Speed
      if (currentAnimation != NONE) {
        tempSpeed += (effectiveDirection * amount); 
        tempSpeed = constrain(tempSpeed, 1, 100);
        updateMenuItemDisplay(currentSelection); 
      }
    }
  } else {
    // Moving between items - skip anything that can't be selected
    previousSelection = currentSelection;
    for (int i = 0; i < 4; i++) {
      currentSelection = (currentSelection + direction + 4) % 4; // Wraps 0-3 both ways
      if (isItemSelectable(currentSelection)) break;
    }
    updateMenuItemDisplay(previousSelection);
    updateMenuItemDisplay(currentSelection);
  }
}

// OK short press in the main menu: enter / confirm the highlighted item
void pressAction() {
  resetInactivityTimer();

  if (controlsLocked()) return; // Client - nothing to edit

  if (!itemActivated && !isItemSelectable(currentSelection)) return; // Locked item

  itemActivated = !itemActivated;

  if (itemActivated) { // Start editing from the confirmed value
    if (currentSelection == 1) tempBrightness = brightnessPercent;
    else if (currentSelection == 2) tempAnimation = currentAnimation;
    else if (currentSelection == 3) tempSpeed = speedPercent;
  } else {
    handleSelection(currentSelection);
  }
  
  updateMenuItemDisplay(currentSelection); 
}

// Called when an item is confirmed (OK pressed while editing)
void handleSelection(int selectedItemIndex) { 
  if (selectedItemIndex == 1) { // Brightness
    brightnessPercent = tempBrightness;
    if (currentAnimation == NONE) ledcWrite(LED_PWM_PIN, brightnessToPWM(brightnessPercent));
    drawNotificationText(NOTIFICATION_TEXT_Y, "Brightness set to " + String(brightnessPercent) + "%");
    broadcastStateIfHost();
  }
  else if (selectedItemIndex == 2) { // Animation
    AnimationType oldAnimation = currentAnimation; 
    drawNotificationText(NOTIFICATION_TEXT_Y, "Animation set to " + getAnimName(tempAnimation));

    if (tempAnimation == NONE) stopCurrentAnimation();
    else startAnimation(tempAnimation);

    // Animation on/off changes whether Speed is greyed out
    if (oldAnimation != currentAnimation) updateMenuItemDisplay(3);
    broadcastStateIfHost();
  }
  else if (selectedItemIndex == 3) { // Speed
    speedPercent = tempSpeed;
    drawNotificationText(NOTIFICATION_TEXT_Y, "Speed set to " + String(speedPercent));
    broadcastStateIfHost();
  }
}


//------------------------------------------------------------------- TEXT HELPERS

String getAnimName(AnimationType anim) {
  switch (anim) {
    case NONE:      return "None";
    case BLINK:     return "Blink";
    case LIGHTNING: return "Lightning";
    case STROBE:    return "Strobe";
    case CHASE:     return "Chase";
  }
  return "";
}


//------------------------------------------------------------------- FOOTER BAR

// Framed bar: "Mode:" (white) + "H" (glow colour) on the left, "Device:" (white) + name (grey)
// on the right. Drawn with the menu.
// If both don't fit on one line, the "Device:" label is dropped.
void drawFooter() {
  if (screenSleeping || !menuShown) return;

  tft.fillRect(FOOTER_X - GLOW_W, FOOTER_Y - GLOW_W, FOOTER_W + 2 * GLOW_W, FOOTER_H + GLOW_W, backgroundColor);
  drawGlowFrame(tft, FOOTER_X, FOOTER_Y, FOOTER_W, FOOTER_H, 6, restEdge(), panelFill(), glowLevel(150), backgroundColor);

  int y = FOOTER_Y + FOOTER_H / 2;
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextDatum(ML_DATUM);

  String modeLabel = "Mode: ";
  String modeValue = getModeAbbrev(deviceMode);
  String devLabel  = "Device: ";
  String devValue  = fullDeviceName;

  int inner = FOOTER_W - 20;
  int leftW  = tft.textWidth(modeLabel) + tft.textWidth(modeValue);
  int rightW = tft.textWidth(devLabel) + tft.textWidth(devValue);
  if (leftW + 12 + rightW > inner) {
    devLabel = "";
    rightW = tft.textWidth(devValue);
  }

  // Left - Mode
  int x = FOOTER_X + 10;
  tft.setTextColor(MENU_COL_TEXT);
  tft.drawString(modeLabel, x, y);
  x += tft.textWidth(modeLabel);
  tft.setTextColor(glowColor);
  tft.drawString(modeValue, x, y);

  // Right - Device
  x = FOOTER_X + FOOTER_W - 10 - rightW;
  if (devLabel.length()) {
    tft.setTextColor(MENU_COL_TEXT);
    tft.drawString(devLabel, x, y);
    x += tft.textWidth(devLabel);
  }
  tft.setTextColor(MENU_COL_BODY);
  tft.drawString(devValue, x, y);

  tft.setTextDatum(TL_DATUM);
}


//------------------------------------------------------------------- STATUS ICONS (header, top-right)
// The icon functions take a TFT_eSPI& so the same icons draw in the header and in the Key box.
// s = size (1.0 in the header, KEY_ICON_SCALE in the Key box).

// Colour for each role: Host coral, Client cyan, Standalone purple
uint16_t roleColour(DeviceMode mode) {
  if (mode == MODE_HOST) return STATUS_COL_HOST;
  if (mode == MODE_STANDALONE) return STATUS_COL_SOLO;
  return STATUS_COL_CLIENT;
}

// Chain-link connection icon. Two interlocked links = Connected; the links pulled apart with
// small break marks = Retrying. (Standalone shows no link icon at all - it isn't meant to link.)
void drawLinkIcon(TFT_eSPI &g, int cx, int cy, uint16_t col, bool broken, float s) {
  auto rs = [s](int v) { return (int)lroundf(v * s); };
  int w = rs(13), h = rs(9), r = rs(4);
  int top    = cy - rs(4);
  int leftX  = broken ? cx - rs(14) : cx - rs(11);
  int rightX = broken ? cx + rs(1)  : cx - rs(2);

  // Each link is two nested rounded rectangles - a 2px-thick ring
  g.drawRoundRect(leftX, top, w, h, r, col);
  g.drawRoundRect(leftX + 1, top + 1, w - 2, h - 2, r - 1, col);
  g.drawRoundRect(rightX, top, w, h, r, col);
  g.drawRoundRect(rightX + 1, top + 1, w - 2, h - 2, r - 1, col);

  if (broken) { // Little break marks above and below the gap
    g.drawLine(cx - 1, cy - rs(8), cx + 1, cy - rs(5), col);
    g.drawLine(cx - 1, cy + rs(5), cx + 1, cy + rs(8), col);
  }
}

// Role icon: coral house = Host, cyan linked node = Client, purple person = Standalone.
// level fades it (255 = full colour), s scales it.
void drawRoleIconFor(TFT_eSPI &g, DeviceMode mode, int cx, int cy, uint8_t level, float s) {
  auto rs = [s](int v) { return (int)lroundf(v * s); };
  uint16_t c = fadeCol(g, roleColour(mode), level);

  if (mode == MODE_HOST) {
    // House: roof, walls, and a door cut out of the walls
    g.fillTriangle(cx - rs(8), cy - rs(1), cx, cy - rs(8), cx + rs(8), cy - rs(1), c);
    g.fillRect(cx - rs(6), cy - rs(1), rs(13), rs(8), c);
    g.fillRect(cx - rs(1), cy + rs(3), max(rs(3), 2), rs(4), backgroundColor);
  } else if (mode == MODE_STANDALONE) {
    // Single person
    g.fillCircle(cx, cy - rs(4), max(rs(3), 2), c);
    g.fillRoundRect(cx - rs(6), cy + rs(1), rs(13), rs(7), max(rs(3), 1), c);
  } else {
    // Client: small node linked up to a hollow node (its Host)
    g.drawCircle(cx, cy - rs(6), max(rs(3), 2), c);
    g.drawLine(cx, cy - rs(3), cx, cy + rs(3), c);
    g.fillCircle(cx, cy + rs(6), max(rs(3), 2), c);
  }
}

void drawStatusIcons() {
  if (screenSleeping) return;
  tft.fillRect(SW - 62, 2, 59, MENU_HEADER_H - 3, backgroundColor);

  if (deviceMode == MODE_STANDALONE) {
    // No link icon - the person sits at the right edge on its own
    drawRoleIconFor(tft, deviceMode, STATUS_LINK_X, STATUS_ICON_Y, 255, 1.0);
  } else {
    bool retrying = (deviceMode == MODE_CLIENT && !clientLinked);
    uint16_t linkCol = !retrying ? STATUS_COL_CONNECTED
                     : (statusBlinkOn ? STATUS_COL_RETRY : tft.alphaBlend(80, STATUS_COL_RETRY, backgroundColor));
    drawRoleIconFor(tft, deviceMode, STATUS_ROLE_X, STATUS_ICON_Y, 255, 1.0);
    drawLinkIcon(tft, STATUS_LINK_X, STATUS_ICON_Y, linkCol, retrying, 1.0);
  }

  drawnMode = deviceMode;
  drawnLinked = clientLinked;
}

// Called from loop(): redraws the icons if the radio changed our link state, and blinks yellow.
void statusIconsTick(unsigned long now) {
  if (popupOpen) return; // The popup covers the menu - icons catch up when it closes
  if (deviceMode != drawnMode || clientLinked != drawnLinked) {
    bool justRelinked = (deviceMode == MODE_CLIENT && drawnMode == MODE_CLIENT && clientLinked && !drawnLinked);
    statusBlinkOn = true;
    drawStatusIcons();
    if (justRelinked) drawNotificationText(NOTIFICATION_TEXT_Y, String("Linked to ") + hostName);
  } else if (deviceMode == MODE_CLIENT && !clientLinked && now - lastStatusBlink >= STATUS_BLINK_MS) {
    lastStatusBlink = now;
    statusBlinkOn = !statusBlinkOn;
    drawStatusIcons();
  }
}


//------------------------------------------------------------------- INFO BOX (shortcuts <-> key)

// Info box position / height - a Client gets a taller box higher up (its rows are compact)
int infoY() { return controlsLocked() ? CLIENT_INFO_Y : INFO_Y; }
int infoH() { return controlsLocked() ? CLIENT_INFO_H : INFO_H; }

// The sprite is rebuilt when the box height changes (Client <-> other modes)
bool infoSprReady() {
  int h = infoH() + 2 * GLOW_W;
  if (infoSpr.created() && infoSpr.height() == h) return true;
  if (infoSpr.created()) infoSpr.deleteSprite();
  infoSpr.setColorDepth(16);
  return infoSpr.createSprite(INFO_W + 2 * GLOW_W, h) != nullptr;
}

// Small line icons for the info box, centred on (x, y).
// 0 = sun, 1 = power, 2 = toggle switch, 3 = curved arrow, 4 = info "i"
void drawSmallIcon(TFT_eSPI &g, int which, int x, int y, uint16_t col, uint16_t bg) {
  if (which == 0) {        // Sun
    g.fillCircle(x, y, 3, col);
    for (int i = 0; i < 8; i++) {
      float a = i * PI / 4;
      g.drawLine(x + 5 * cos(a), y + 5 * sin(a), x + 7 * cos(a), y + 7 * sin(a), col);
    }
  } else if (which == 1) { // Power symbol - ring open at the top, line through the gap
    g.drawCircle(x, y, 6, col);
    g.drawCircle(x, y, 5, col);
    g.fillRect(x - 2, y - 7, 5, 5, bg);
    g.drawFastVLine(x, y - 7, 7, col);
    g.drawFastVLine(x + 1, y - 7, 7, col);
  } else if (which == 2) { // Toggle switch - filled pill, knob on the right (switched on)
    g.fillRoundRect(x - 9, y - 5, 18, 10, 5, col);
    g.fillCircle(x + 4, y, 3, bg);
  } else if (which == 3) { // Curved arrow - head pointing right, tail sweeping down-left and tapering
    g.fillTriangle(x + 1, y - 7, x + 1, y + 3, x + 7, y - 2, col);
    for (int i = 0; i <= 12; i++) {
      float t = i / 12.0;
      float u = 1.0 - t;
      // Curve from the arrow's base, out to the left, then down to the tail tip
      float px = u * u * (x + 1) + 2 * u * t * (x - 8) + t * t * (x - 5);
      float py = u * u * (y - 2) + 2 * u * t * (y - 2) + t * t * (y + 7);
      g.fillCircle((int)px, (int)py, (t < 0.45) ? 2 : 1, col);
    }
  } else if (which == 4) { // Info "i" in a circle
    g.drawCircle(x, y, 6, col);
    g.drawFastVLine(x, y - 1, 4, col);
    g.drawPixel(x, y - 3, col);
  } else {                 // Padlock - shackle over a body with a keyhole
    g.drawCircle(x, y - 2, 4, col);
    g.drawCircle(x, y - 2, 3, col);
    g.fillRect(x - 5, y - 2, 11, 8, col);
    g.drawFastVLine(x, y + 1, 3, bg);
  }
}

// The box itself - glowing frame, always full brightness (only the contents fade)
void drawInfoFrame(TFT_eSPI &g, int ox, int oy) {
  drawGlowFrame(g, ox, oy, INFO_W, infoH(), FRAME_CUT, restEdge(), panelFill(), glowLevel(150), backgroundColor);
}

// Dividers: icon cell in the title row, then a line under the title and between each row
void drawInfoLines(TFT_eSPI &g, int ox, int oy) {
  uint16_t line = dividerCol();
  g.drawFastVLine(ox + INFO_ICON_COL_W, oy + 4, INFO_TITLE_H - 4, line);
  for (int i = 0; i < 3; i++) {
    g.drawFastHLine(ox + 6, oy + INFO_TITLE_H + i * INFO_ROW_H, INFO_W - 12, line);
  }
}

// Centre line of content row i (0-2)
int infoRowY(int oy, int i) {
  return oy + INFO_TITLE_H + i * INFO_ROW_H + INFO_ROW_H / 2 + 1;
}

// Icon in its cell + white title (regular weight) along the top of the box
void drawInfoTitle(TFT_eSPI &g, int iconWhich, const char *title, int ox, int oy, uint8_t level) {
  int y = oy + INFO_TITLE_H / 2 + 1;
  drawSmallIcon(g, iconWhich, ox + INFO_ICON_COL_W / 2, y, fadeCol(g, glowColor, level), panelFill());
  g.setTextFont(2);
  g.setTextColor(fadeCol(g, INFO_COL_TITLE, level));
  g.setTextDatum(ML_DATUM);
  g.drawString(title, ox + INFO_ICON_COL_W + 8, y);
}

// One shortcut key in the glow colour. "-" and "+" are drawn as solid shapes (the font's own
// - and + are tiny). Returns the width used.
int drawKeyGlyph(TFT_eSPI &g, const char *key, int x, int y, uint16_t col) {
  if (strcmp(key, "-") == 0) {
    g.fillRect(x, y - 1, 9, 2, col);
    return 9;
  }
  if (strcmp(key, "+") == 0) {
    g.fillRect(x, y - 1, 9, 2, col);
    g.fillRect(x + 4, y - 5, 2, 9, col);
    return 9;
  }
  g.setTextFont(2);
  g.setTextColor(col);
  g.setTextDatum(ML_DATUM);
  g.drawString(key, x, y);
  return g.textWidth(key);
}

// Page 0: "Button Shortcuts (Hold)" - each row: icon (glow colour), name (grey), then the keys.
void drawShortcutPage(TFT_eSPI &g, int ox, int oy, uint8_t level) {
  const int icons[3]   = { 0, 1, 2 };   // Sun, power, toggle
  const char *names[3] = { "50% Brightness", "All Off", "Switch Mode" };
  const char *keys[3][3] = {          // "" = no more keys on that line
    { "-",  "+", ""   },
    { "-",  "+", "OK" },
    { "OK", "",  ""   }
  };
  uint16_t textCol = fadeCol(g, INFO_COL_TEXT, level);   // Shortcut names - grey
  uint16_t ampCol  = fadeCol(g, MENU_COL_TEXT, level);   // "&" between keys - white
  uint16_t keyCol  = fadeCol(g, glowColor, level);       // Keys and row icons

  drawInfoLines(g, ox, oy);
  drawInfoTitle(g, 3, "Button Shortcuts (Hold)", ox, oy, level);

  for (int i = 0; i < 3; i++) {
    int y = infoRowY(oy, i);

    drawSmallIcon(g, icons[i], ox + INFO_ICON_DX, y, keyCol, panelFill());
    g.setTextFont(2);
    g.setTextColor(textCol);
    g.setTextDatum(ML_DATUM);
    g.drawString(names[i], ox + INFO_TEXT_DX, y);

    // Keys, joined by a white "&"
    int x = ox + SHORTCUT_KEYS_DX;
    for (int k = 0; k < 3 && keys[i][k][0] != '\0'; k++) {
      if (k > 0) {
        x += SHORTCUT_AMP_GAP;
        g.setTextFont(2);
        g.setTextColor(ampCol);
        g.setTextDatum(ML_DATUM);
        g.drawString("&", x, y);
        x += g.textWidth("&") + SHORTCUT_AMP_GAP;
      }
      x += drawKeyGlyph(g, keys[i][k], x, y, keyCol);
    }
  }
}

// The three Key rows, starting at rowTop: left column = role icon + letter (in the role's colour)
// + grey text, right column = chain-link states, with a divider between the columns.
void drawKeyRows(TFT_eSPI &g, int ox, int rowTop, int rowH, uint8_t level) {
  const DeviceMode roles[3]  = { MODE_HOST, MODE_CLIENT, MODE_STANDALONE };
  const char *abbrev[3]      = { "H", "C", "S" };
  const char *roleText[3]    = { " = Host", " = Client", " = Standalone" };
  const uint16_t linkCols[2] = { STATUS_COL_CONNECTED, STATUS_COL_RETRY };
  const char *linkText[2]    = { "Connected", "Retrying" };
  uint16_t textCol = fadeCol(g, INFO_COL_TEXT, level);

  g.drawFastVLine(ox + KEY_COL2_DX - 4, rowTop + 3, rowH * 3 - 6, dividerCol()); // Between columns

  for (int i = 0; i < 3; i++) {
    int y = rowTop + i * rowH + rowH / 2 + 1;

    // Left column: role icon, then "H = Host" (letter in the role's colour)
    drawRoleIconFor(g, roles[i], ox + INFO_ICON_DX, y, level, KEY_ICON_SCALE);
    int x = ox + INFO_TEXT_DX - 6;
    g.setTextFont(2);
    g.setTextDatum(ML_DATUM);
    g.setTextColor(fadeCol(g, roleColour(roles[i]), level));
    g.drawString(abbrev[i], x, y);
    x += g.textWidth(abbrev[i]);
    g.setTextColor(textCol);
    g.drawString(roleText[i], x, y);

    // Right column: chain-link states (only two - Standalone has no link icon)
    if (i < 2) {
      drawLinkIcon(g, ox + KEY_COL2_DX + 12, y, fadeCol(g, linkCols[i], level), i == 1, KEY_ICON_SCALE);
      g.setTextFont(2);
      g.setTextColor(textCol);
      g.setTextDatum(ML_DATUM);
      g.drawString(linkText[i], ox + KEY_COL2_DX + 26, y);
    }
  }
}

// Page 1: "Key"
void drawKeyPage(TFT_eSPI &g, int ox, int oy, uint8_t level) {
  drawInfoLines(g, ox, oy);
  drawInfoTitle(g, 4, "Key", ox, oy, level);
  drawKeyRows(g, ox, oy + INFO_TITLE_H, INFO_ROW_H, level);
}

// Client's only page (no cycling): yellow padlock + "Controlled by Host", the Switch Mode
// shortcut, then a "Key" header and the Key rows.
void drawClientPage(TFT_eSPI &g, int ox, int oy) {
  uint16_t line = dividerCol();
  int top = oy + INFO_TITLE_H;
  int R = CLIENT_INFO_ROW_H;
  auto rowY = [&](int i) { return top + i * R + R / 2 + 1; };

  // Dividers: icon cell in the title row and the "Key" row, a line above each row
  g.drawFastVLine(ox + INFO_ICON_COL_W, oy + 4, INFO_TITLE_H - 4, line);
  g.drawFastVLine(ox + INFO_ICON_COL_W, top + R + 4, R - 4, line);
  for (int i = 0; i < 5; i++) g.drawFastHLine(ox + 6, top + i * R, INFO_W - 12, line);

  // Title - yellow padlock (drawInfoTitle uses the glow colour, so drawn here)
  int ty = oy + INFO_TITLE_H / 2 + 1;
  drawSmallIcon(g, 5, ox + INFO_ICON_COL_W / 2, ty, CLIENT_LOCK_COL, panelFill());
  g.setTextFont(2);
  g.setTextDatum(ML_DATUM);
  g.setTextColor(INFO_COL_TITLE);
  g.drawString("Controlled by Host", ox + INFO_ICON_COL_W + 8, ty);

  // Switch Mode - Hold OK
  int y = rowY(0);
  drawSmallIcon(g, 2, ox + INFO_ICON_DX, y, glowColor, panelFill());
  g.setTextColor(INFO_COL_TEXT);
  g.drawString("Switch Mode", ox + INFO_TEXT_DX, y);
  int x = ox + SHORTCUT_KEYS_DX;
  g.drawString("Hold ", x, y);
  drawKeyGlyph(g, "OK", x + g.textWidth("Hold "), y, glowColor);

  // "Key" header row, then the Key rows
  y = rowY(1);
  drawSmallIcon(g, 4, ox + INFO_ICON_COL_W / 2, y, glowColor, panelFill());
  g.setTextFont(2);
  g.setTextDatum(ML_DATUM);
  g.setTextColor(INFO_COL_TITLE);
  g.drawString("Key", ox + INFO_ICON_COL_W + 8, y);
  drawKeyRows(g, ox, top + 2 * R, R, 255);
}

// Draws the info box with the current page at the given brightness (255 = full, 0 = faded out).
// Drawn into a sprite first and pushed whole, so fades don't flicker.
void drawInfoPage(uint8_t level) {
  if (screenSleeping || popupOpen) return;
  bool useSpr = infoSprReady();
  TFT_eSPI &g = useSpr ? static_cast<TFT_eSPI&>(infoSpr) : tft;
  int ox = useSpr ? GLOW_W : INFO_X;
  int oy = useSpr ? GLOW_W : infoY();

  g.fillRect(ox - GLOW_W, oy - GLOW_W, INFO_W + 2 * GLOW_W, infoH() + 2 * GLOW_W, backgroundColor);
  drawInfoFrame(g, ox, oy);
  g.setTextSize(1);
  if (controlsLocked()) drawClientPage(g, ox, oy);
  else if (infoPage == 0) drawShortcutPage(g, ox, oy, level);
  else drawKeyPage(g, ox, oy, level);
  g.setTextFont(2);
  g.setTextDatum(TL_DATUM);

  if (useSpr) infoSpr.pushSprite(INFO_X - GLOW_W, infoY() - GLOW_W);
}

// Shows the current page at full brightness and restarts the cycle timer
void showInfoPageNow() {
  infoFade = INFO_SHOWING;
  drawInfoPage(255);
  infoNextTime = millis() + INFO_CYCLE_MS;
}

// Called from loop(): after INFO_CYCLE_MS, fades the page out, switches page, fades back in.
// Paused while asleep, while the Switch Mode popup is open, or while a notification is showing.
// A Client has one fixed page, so it never cycles.
void infoTick(unsigned long now) {
  if (screenSleeping || popupOpen || notifyUntil != 0 || controlsLocked()) return;
  if ((long)(now - infoNextTime) < 0) return;

  if (infoFade == INFO_SHOWING) {
    infoFade = INFO_FADING_OUT;
    infoFadeStep = 0;
  }
  infoFadeStep++;

  if (infoFade == INFO_FADING_OUT) {
    drawInfoPage((uint8_t)(255 - (255 * infoFadeStep) / INFO_FADE_STEPS));
    if (infoFadeStep >= INFO_FADE_STEPS) { // Fully faded - switch page and fade the new one in
      infoPage = (infoPage + 1) % 2;
      infoFade = INFO_FADING_IN;
      infoFadeStep = 0;
    }
    infoNextTime = now + INFO_FADE_STEP_MS;
  } else { // INFO_FADING_IN
    drawInfoPage((uint8_t)((255 * infoFadeStep) / INFO_FADE_STEPS));
    if (infoFadeStep >= INFO_FADE_STEPS) {
      infoFade = INFO_SHOWING;
      infoNextTime = now + INFO_CYCLE_MS;
    } else {
      infoNextTime = now + INFO_FADE_STEP_MS;
    }
  }
}

// Shows a notification in the info box for NOTIFY_SHOW_MS, then the info page comes back.
// An empty string goes straight back to the info page.
// (Hidden while asleep or while the popup is open.)
void drawNotificationText(int yPos, const String& text) {
  if (screenSleeping || popupOpen) return;
  if (text.length() == 0) {
    notifyUntil = 0;
    showInfoPageNow();
    return;
  }
  infoFade = INFO_SHOWING; // Cancel any fade in progress

  bool useSpr = infoSprReady();
  TFT_eSPI &g = useSpr ? static_cast<TFT_eSPI&>(infoSpr) : tft;
  int ox = useSpr ? GLOW_W : INFO_X;
  int oy = useSpr ? GLOW_W : infoY();

  g.fillRect(ox - GLOW_W, oy - GLOW_W, INFO_W + 2 * GLOW_W, infoH() + 2 * GLOW_W, backgroundColor);
  drawInfoFrame(g, ox, oy);
  g.setTextFont(2);
  g.setTextSize(1);
  g.setTextColor(THEME_CURRENT_COLORS_NOTIFICATION_SUCCESS);
  g.setTextDatum(MC_DATUM);
  g.drawString(text, ox + INFO_W / 2, oy + infoH() / 2);
  g.setTextDatum(TL_DATUM);
  if (useSpr) infoSpr.pushSprite(INFO_X - GLOW_W, infoY() - GLOW_W);

  notifyUntil = millis() + NOTIFY_SHOW_MS;
  if (notifyUntil == 0) notifyUntil = 1; // 0 means "no notification showing"
}


//------------------------------------------------------------------- MAIN MENU (glowing panels)

bool menuSprReady() {
  if (menuSpr.created()) return true;
  menuSpr.setColorDepth(16);
  return menuSpr.createSprite(MENU_ROW_W + 2 * GLOW_W, MENU_ROW_H + 2 * GLOW_W) != nullptr;
}

// Row icons, centred on (x, y). index: 1=Brightness (sun), 2=Animation (bolt), 3=Speed (chevrons)
void drawMenuIcon(TFT_eSPI &g, int index, int x, int y, uint16_t col) {
  if (index == 1) {        // Sun
    g.fillCircle(x, y, 4, col);
    for (int i = 0; i < 8; i++) {
      float a = i * PI / 4;
      g.drawLine(x + 6 * cos(a), y + 6 * sin(a), x + 9 * cos(a), y + 9 * sin(a), col);
    }
  } else if (index == 2) { // Lightning bolt
    g.fillTriangle(x + 2, y - 9, x - 5, y + 1, x + 1, y + 1, col);
    g.fillTriangle(x - 1, y - 1, x + 5, y - 1, x - 2, y + 9, col);
  } else {                 // Speed chevrons
    for (int i = 0; i < 2; i++) {
      int cx = x - 6 + i * 6;
      g.drawLine(cx, y - 6, cx + 5, y, col);
      g.drawLine(cx + 5, y, cx, y + 6, col);
      g.drawLine(cx + 1, y - 6, cx + 6, y, col);
      g.drawLine(cx + 6, y, cx + 1, y + 6, col);
    }
  }
}

void drawMenuHeader() {
  tft.fillRect(2, 2, SW - 4, MENU_HEADER_H - 2, backgroundColor);
  drawLogoTitle(tft, 12, 16, false);
  drawStatusIcons();
}

// Client row (view-only): compact panel - icon, white title, the Host's value right-aligned.
void drawClientRow(int index) {
  int y = MENU_ROW_Y0 + (index - 1) * (CLIENT_ROW_H + MENU_ROW_GAP);
  bool useSpr = menuSprReady();
  TFT_eSPI &g = useSpr ? static_cast<TFT_eSPI&>(menuSpr) : tft;
  int ox = useSpr ? GLOW_W : MENU_ROW_X;
  int oy = useSpr ? GLOW_W : y;
  int W = MENU_ROW_W, H = CLIENT_ROW_H, cy = oy + H / 2;
  bool locked = (index == 3 && currentAnimation == NONE); // Speed greyed out with no animation

  g.fillRect(ox - GLOW_W, oy - GLOW_W, W + 2 * GLOW_W, H + 2 * GLOW_W, backgroundColor);
  drawGlowFrame(g, ox, oy, W, H, FRAME_CUT, restEdge(), panelFill(), glowLevel(150), backgroundColor);

  const char *labels[4] = { "", "BRIGHTNESS", "ANIMATION", "SPEED" };
  String value = (index == 1) ? String(brightnessPercent) + "%"
               : (index == 2) ? getAnimName(currentAnimation)
               : locked       ? String("--") : String(speedPercent);

  drawMenuIcon(g, index, ox + 20, cy, locked ? MENU_COL_LOCKED : glowColor);
  g.setTextFont(2);
  g.setTextSize(1);
  g.setTextDatum(ML_DATUM);
  g.setTextColor(locked ? MENU_COL_LOCKED : MENU_COL_TEXT);
  g.drawString(labels[index], ox + 36, cy);
  g.setTextDatum(MR_DATUM);
  g.setTextColor(locked ? MENU_COL_LOCKED : MENU_COL_BODY);
  g.drawString(value, ox + W - 12, cy);
  g.setTextDatum(TL_DATUM);

  // The sprite is sized for a full row - push only the compact part
  if (useSpr) menuSpr.pushSprite(MENU_ROW_X - GLOW_W, y - GLOW_W, 0, 0, W + 2 * GLOW_W, H + 2 * GLOW_W);
}

// Rows: 1 = Brightness, 2 = Animation, 3 = Speed - each an angled-corner glowing panel.
// Label (title) white, value grey, glow colour while editing. All regular weight.
void drawMenuRow(int index) {
  if (controlsLocked()) { drawClientRow(index); return; }
  int y = MENU_ROW_Y0 + (index - 1) * (MENU_ROW_H + MENU_ROW_GAP);
  bool useSpr = menuSprReady();
  TFT_eSPI &g = useSpr ? static_cast<TFT_eSPI&>(menuSpr) : tft;
  int ox = useSpr ? GLOW_W : MENU_ROW_X;
  int oy = useSpr ? GLOW_W : y;
  int W = MENU_ROW_W, H = MENU_ROW_H;

  bool active  = (currentSelection == index && itemActivated);
  bool hovered = (currentSelection == index && !itemActivated);
  bool locked  = !isItemSelectable(index);

  // Panel - glowing edge at rest, brighter near-white edge and stronger glow when selected
  uint16_t edge = locked ? g.alphaBlend(70, MENU_COL_MUTED, backgroundColor)
                : (hovered || active) ? hotEdge() : restEdge();
  uint8_t glow  = locked ? 0 : (hovered || active) ? glowLevel(255) : glowLevel(150);
  uint16_t fill = active ? activeFill() : panelFill();

  g.fillRect(ox - GLOW_W, oy - GLOW_W, W + 2 * GLOW_W, H + 2 * GLOW_W, backgroundColor);
  drawGlowFrame(g, ox, oy, W, H, FRAME_CUT, edge, fill, glow, backgroundColor);

  const char *labels[4] = { "", "BRIGHTNESS", "ANIMATION", "SPEED" };
  uint16_t iconCol  = locked ? MENU_COL_LOCKED : glowColor;
  uint16_t titleCol = locked ? MENU_COL_LOCKED : MENU_COL_TEXT;
  uint16_t valCol   = locked ? MENU_COL_LOCKED : (active ? glowColor : MENU_COL_BODY);

  // Icon + label
  drawMenuIcon(g, index, ox + 20, oy + 14, iconCol);
  g.setTextFont(2);
  g.setTextSize(1);
  g.setTextDatum(ML_DATUM);
  g.setTextColor(titleCol);
  g.drawString(labels[index], ox + 36, oy + 14);

  if (index == 1) { // Brightness - value top-right, glow colour -> purple slider
    int v = active ? tempBrightness : brightnessPercent;
    g.setTextDatum(MR_DATUM);
    g.setTextColor(valCol);
    g.drawString(String(v) + "%", ox + W - 12, oy + 14);
    drawGradientSlider(g, ox + 14, oy + 32, W - 28, v, glowColor, MENU_COL_ANIM, active, false, fill);

  } else if (index == 2) { // Animation - value in a boxed selector with chevrons
    int bx = ox + 36, by = oy + 25, bw = W - 48, bh = 19;
    drawGlowFrame(g, bx, by, bw, bh, 5, active ? glowColor : restEdge(), fill, 0, fill);
    drawChevron(g, bx + 10, by + bh / 2, -1, active ? glowColor : MENU_COL_MUTED);
    drawChevron(g, bx + bw - 10, by + bh / 2, 1, active ? glowColor : MENU_COL_MUTED);
    g.setTextDatum(MC_DATUM);
    g.setTextColor(valCol);
    g.drawString(getAnimName(active ? tempAnimation : currentAnimation), bx + bw / 2, by + bh / 2);

  } else { // Speed - value top-right, purple -> magenta slider (greyed out with no animation)
    int v = active ? tempSpeed : speedPercent;
    g.setTextDatum(MR_DATUM);
    g.setTextColor(valCol);
    g.drawString(locked ? String("--") : String(v), ox + W - 12, oy + 14);
    drawGradientSlider(g, ox + 14, oy + 32, W - 28, v, MENU_COL_ANIM, MENU_COL_MAGENTA, active, locked, fill);
  }
  g.setTextFont(2);
  g.setTextDatum(TL_DATUM);

  if (useSpr) menuSpr.pushSprite(MENU_ROW_X - GLOW_W, y - GLOW_W);
}

void drawFullMenu() {
  menuShown = true;
  tft.fillScreen(backgroundColor);
  drawMenuHeader();
  for (int i = 1; i <= 3; i++) drawMenuRow(i);
  notifyUntil = 0;
  showInfoPageNow();
  drawFooter();
  drawScreenFrame();
}

// Draws or updates one item: 0 = status icons, 1 = Brightness, 2 = Animation, 3 = Speed.
// Menu rows aren't drawn while the popup is open (they'd draw over it).
void updateMenuItemDisplay(int itemIndex) {
  if (itemIndex == 0) { drawStatusIcons(); return; }
  if (screenSleeping || popupOpen) return;
  if (itemIndex >= 1 && itemIndex <= 3) drawMenuRow(itemIndex);
}


//------------------------------------------------------------------- ANIMATIONS

// Runs whichever LED animation is active. Called from loop() and during blocking searches.
void runAnimations() {
  if (currentAnimation == BLINK) runBlinkAnimation();
  else if (currentAnimation == LIGHTNING) runLightningAnimation();
  else if (currentAnimation == STROBE) runStrobeAnimation();
  else if (currentAnimation == CHASE) runChaseAnimation();
}

// Starts the given animation (anything except NONE - use stopCurrentAnimation() for that)
void startAnimation(AnimationType anim) {
  if (anim == BLINK) startBlinkAnimation();
  else if (anim == LIGHTNING) startLightningAnimation();
  else if (anim == STROBE) startStrobeAnimation();
  else if (anim == CHASE) startChaseAnimation();
}

// Converts 0-100% brightness to 0-255 PWM value
int brightnessToPWM(int percent) {
  return map(percent, 0, 100, 0, (1 << PWM_RESOLUTION) - 1); // Max value for 8-bit is 255
}

// Stops any currently running animation and sets the LED to a solid brightness
void stopCurrentAnimation() {
  currentAnimation = NONE;
  blinkState = false;
  strobeState = false;
  lightningPhase = LIGHTNING_WAITING;
  chasePulseActive = false;
  ledcWrite(LED_PWM_PIN, brightnessToPWM(brightnessPercent));
}

// Starts the blink animation
void startBlinkAnimation() {
  currentAnimation = BLINK;
  blinkState = true; // Start with LED ON
  lastBlinkToggleTime = millis();
  ledcWrite(LED_PWM_PIN, brightnessToPWM(brightnessPercent));
}

// Live preview: while the Speed item (index 3) is actively being adjusted, use the
// in-progress value so a running animation's rate updates as you scroll,
// instead of waiting for OK to confirm.
int getEffectiveSpeed() {
  return (itemActivated && currentSelection == 3) ? tempSpeed : speedPercent;
}

// Handles the logic for the Blink animation
void runBlinkAnimation() {
  // Higher speed % must give a SHORTER interval = faster blink.
  unsigned long blinkInterval = map(getEffectiveSpeed(), 1, 100, 1000, 100); // 1000ms slow to 100ms fast

  if (millis() - lastBlinkToggleTime >= blinkInterval) {
    lastBlinkToggleTime = millis();
    blinkState = !blinkState;
    ledcWrite(LED_PWM_PIN, blinkState ? brightnessToPWM(brightnessPercent) : 0);
  }
}

// Starts the strobe animation
void startStrobeAnimation() {
  currentAnimation = STROBE;
  strobeState = true;
  lastStrobeToggleTime = millis();
  ledcWrite(LED_PWM_PIN, brightnessToPWM(brightnessPercent));
}

// Same on/off toggle pattern as Blink, but a much faster interval range so it
// reads as a distinct, punchier effect rather than a fast blink.
void runStrobeAnimation() {
  unsigned long strobeInterval = map(getEffectiveSpeed(), 1, 100, 200, 20); // 200ms slow to 20ms fast

  if (millis() - lastStrobeToggleTime >= strobeInterval) {
    lastStrobeToggleTime = millis();
    strobeState = !strobeState;
    ledcWrite(LED_PWM_PIN, strobeState ? brightnessToPWM(brightnessPercent) : 0);
  }
}

// Starts the lightning animation
void startLightningAnimation() {
  currentAnimation = LIGHTNING;
  lightningPhase = LIGHTNING_WAITING;
  lightningLedOn = false;
  ledcWrite(LED_PWM_PIN, 0); // Start dark, waiting for the first strike
  lightningNextEventTime = millis() + random(500, 3000);
}

// Random dark gaps (WAITING) followed by a burst of irregular on/off flickers (FLASHING) to
// mimic a real lightning strike. Non-blocking - uses millis() timestamps, no delay().
// ESP32's random() is seeded from the hardware RNG automatically.
void runLightningAnimation() {
  unsigned long now = millis();
  if (now < lightningNextEventTime) return;

  if (lightningPhase == LIGHTNING_WAITING) {
    lightningPhase = LIGHTNING_FLASHING;
    lightningFlashesRemaining = random(3, 8); // 3-7 flickers per strike
    lightningLedOn = true;
    ledcWrite(LED_PWM_PIN, brightnessToPWM(brightnessPercent));
    lightningNextEventTime = now + random(20, 80);
  } else { // LIGHTNING_FLASHING
    lightningLedOn = !lightningLedOn;
    ledcWrite(LED_PWM_PIN, lightningLedOn ? brightnessToPWM(brightnessPercent) : 0);
    lightningFlashesRemaining--;

    if (lightningFlashesRemaining <= 0) {
      lightningPhase = LIGHTNING_WAITING;
      ledcWrite(LED_PWM_PIN, 0);
      lightningNextEventTime = now + random(500, 3000);
    } else {
      lightningNextEventTime = now + random(20, 80);
    }
  }
}

// --- Chase ---
// The Host (or a unit on its own) runs the sequence: each step it either pulses itself or tells
// the next Client to pulse, in order Host -> C_..._01 -> C_..._02 ... -> back to the Host.
// A linked Client stays dark and pulses only when the Host tells it to.

// Pulse length for the current Speed (higher speed = shorter pulse)
unsigned long chaseStepMs() {
  return map(getEffectiveSpeed(), 1, 100, CHASE_STEP_SLOW_MS, CHASE_STEP_FAST_MS);
}

void startChaseAnimation() {
  currentAnimation = CHASE;
  chasePulseActive = false;
  chasePulseRequested = false;
  chaseTurn = 0;
  chaseNextStepTime = millis();   // Host / solo starts the first turn straight away
  ledcWrite(LED_PWM_PIN, 0);      // Every light starts dark
}

// Starts this unit's own fade 0 -> CHASE_PEAK_PERCENT -> 0 over lenMs
void startLocalPulse(unsigned long lenMs) {
  chasePulseActive = true;
  chasePulseStart = millis();
  chasePulseLen = (lenMs > 0) ? lenMs : 1;
}

void runChaseAnimation() {
  unsigned long now = millis();
  bool follower = (deviceMode == MODE_CLIENT && clientLinked);

  if (follower) {
    // Linked Client: wait for the Host to say it's our turn
    if (chasePulseRequested) {
      chasePulseRequested = false;
      startLocalPulse(chasePulseRequestMs);
    }
  } else if ((long)(now - chaseNextStepTime) >= 0) {
    // Host, Standalone, or a Client that's lost its Host: run the sequence
    unsigned long step = chaseStepMs();
    int clients = (deviceMode == MODE_HOST) ? buildChaseOrder() : 0;
    if (chaseTurn > clients) chaseTurn = 0;           // A Client dropped out - start again from the top

    if (chaseTurn == 0) startLocalPulse(step);         // Our turn
    else sendChasePulse(chaseOrder[chaseTurn - 1].name, step);

    chaseTurn = (chaseTurn + 1) % (clients + 1);
    chaseNextStepTime = now + step;
  }

  // Fade this unit's own pulse: half a sine wave, 0 -> peak -> 0
  if (chasePulseActive) {
    unsigned long t = now - chasePulseStart;
    if (t >= chasePulseLen) {
      chasePulseActive = false;
      ledcWrite(LED_PWM_PIN, 0);
    } else {
      float level = sin(PI * t / (float)chasePulseLen);
      int peakPwm = (CHASE_PEAK_PERCENT * ((1 << PWM_RESOLUTION) - 1)) / 100;
      ledcWrite(LED_PWM_PIN, (int)(peakPwm * level));
    }
  }
}