/* 
================================================================================================
=================================== LED Batten Control (CYD) ===================================
================================================================================================

ESP32-2432S028R 320 x 240 LCD Display (2.8") (Commonly known as Cheap Yellow) based LED Controller Interface with PWM
2025 Disaster of Puppets
Use the code at your own risk, I mean it should be fine..but you know..free and all that.
Ie, If you break hardware / yourself, I am not liable.
TLDR :) Have fun

---------------------------------------------------------------------------------------------------
*/

#include <Arduino.h>
#include <TFT_eSPI.h>   // Ensure your User_Setup matches ESP32-2432S028R see the Users_Setup.h I used.

// --- Display constants (portrait) ---
#define SW tft.width()
#define SH tft.height()

// Inactivity timeout (ms)
#define INACTIVITY_TIMEOUT 60000  


// --- Rotary encoder pins ---
// Note: GPIO35 has no internal pull-up; add a 10k pull-up to 3.3V for CLK if you use 35.
#define ENCODER_CLK 35  // Blue wire (requires external pull-up)
#define ENCODER_DT  27  // Orange wire
#define ENCODER_SW  22  // Yellow wire (active LOW, use INPUT_PULLUP)

// --- PWM config (ESP32 Arduino core v3.x API) ---
#define LED_PWM_PIN     25
#define PWM_FREQ        5000
#define PWM_RESOLUTION  8  // 0-255

// --- Global Colour Palette (RGB565 from your base code) ---
#define COLOR_DUKE_BLUE     0x0113  // #090C9B
#define COLOR_TRUE_BLUE     0x3277  // #3066BE
#define COLOR_POWDER_BLUE   0xB5B7  // #B4C5E4
#define COLOR_IVORY         0xFFFE  // #FBFFF1
#define COLOR_NEUTRAL_GREY  0x4208  // #404040

// For differential updates in the Brightness submenu:
int prevSubSel = 0;  

String mainMenuItems[6] = {
  "Brightness","Option 2","Option 3","Option 4","Option 5","Option 6"
};


// Track last user action
static unsigned long lastActivity = 0;
static bool screenSleeping       = false;

// --- Theme structure (original) ---
struct ColourTheme {
  uint16_t mainColour;
  uint16_t highlightColourBorder;
  uint16_t activeColour;
  uint16_t pressedColour;
  uint16_t buttonBorder;
  uint16_t textColour;
  uint16_t sliderMarkerColour;
  uint16_t lcdValueColour;
};

const ColourTheme colourThemes[] = {
  { // Theme 0: Duke Blue Default
    COLOR_DUKE_BLUE,     // mainColour
    COLOR_IVORY,         // highlightColourBorder
    COLOR_TRUE_BLUE,     // activeColour
    COLOR_NEUTRAL_GREY,  // pressedColour
    COLOR_DUKE_BLUE,     // buttonBorder
    COLOR_IVORY,         // textColour
    COLOR_POWDER_BLUE,   // sliderMarkerColour
    COLOR_DUKE_BLUE      // lcdValueColour
  }
};

int activeThemeIndex = 0;
const ColourTheme* currentTheme = &colourThemes[activeThemeIndex];

// --- UI constants ---
const int BUTTON_AND_SLIDER_HEIGHT = 36;
const int BUTTON_GAP = 6;

// --- App/UI state ---
TFT_eSPI tft = TFT_eSPI();
bool Debug = true;

int currentSelection = 0;   // index of the highlighted item
int prevSelection    = 0;   // same, for differential updates


bool inSubmenu = false;      // false = main menu, true = brightness menu
bool itemActivated = false;  // whether the slider is active (adjusting)

int brightnessPercent = 50;  // applied brightness (0-100)
int tempBrightness = 50;     // temporary brightness while adjusting

// =========================
// === Button styling API ===
// =========================
struct ButtonStyle {
  uint16_t border;
  uint16_t highlightBorder;
  uint16_t background;
  uint16_t pressedBackground;
  uint16_t text;
};

ButtonStyle Button_Style_FromTheme(const ColourTheme* th) {
  ButtonStyle s;
  s.border            = th->buttonBorder;
  s.highlightBorder   = th->highlightColourBorder;
  s.background        = th->mainColour;
  s.pressedBackground = th->pressedColour;
  s.text              = th->textColour;
  return s;
}

// Draw_Button: draw a styled button centered text; selected shows highlight border; pressed uses pressed background.
void Draw_Button(int x, int y, int w, int h, const String& text,
                 const ButtonStyle& style, bool selected, bool pressed) {
  uint16_t bg     = pressed ? style.pressedBackground : style.background;
  // for non-selected, draw the border in the same colour as the background
  uint16_t border = selected ? style.highlightBorder : bg;

  tft.fillRect(x, y, w, h, bg);
  tft.drawRect(x, y, w, h, border);
  if (selected) {
    tft.drawRect(x - 1, y - 1, w + 2, h + 2, style.highlightBorder);
  }

  tft.setTextSize(2);
  tft.setTextColor(style.text, bg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(text, x + w / 2, y + h / 2);
}

// Draw exactly one main-menu row (selected or not)
void drawMainMenuRow(int row, bool selected) {
  ButtonStyle bs = Button_Style_FromTheme(currentTheme);

  const int numButtons  = 6;
  const int topPad      = 60;
  const int botPad      = 10;
  const int availH      = SH - topPad - botPad;
  const int gapTotal    = (numButtons - 1) * BUTTON_GAP;
  const int rowH        = max((availH - gapTotal) / numButtons, 24);

  const int x           = 10;
  const int w           = SW - 20;
  const int y           = topPad + row * (rowH + BUTTON_GAP);

  // Grab the correct label from your global array
  String label = mainMenuItems[row];

  // Draw that one button in its selected/unselected state
  Draw_Button(x, y, w, rowH, label, bs, selected, false);
}

// =========================
// === PWM helpers (v3.x) ===
// =========================
static inline int dutyFromPercent(int percent) {
  percent = constrain(percent, 0, 100);
  return map(percent, 0, 100, 0, (1 << PWM_RESOLUTION) - 1);
}

void setBrightnessPercent(int percent) {
  percent = constrain(percent, 0, 100);
  brightnessPercent = percent;
  int duty = dutyFromPercent(percent);
  ledcWrite(LED_PWM_PIN, duty);
  if (Debug) {
    Serial.printf("Applied Brightness: %d%% (duty=%d)\n", percent, duty);
  }
}

// =========================
// === Drawing helpers ===
// =========================
void drawTitleCentered(int y, const String& title) {
  tft.setTextSize(2);
  tft.setTextColor(currentTheme->textColour, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(title, SW / 2, y);
}

// Simple LCD value display (styled frame + large text)
void drawLCDValue(int x, int y, int w, int h, const String& valText) {
  tft.drawRect(x - 5, y - 5, w + 10, h + 10, currentTheme->buttonBorder);
  tft.setTextSize(3);
  tft.setTextColor(currentTheme->lcdValueColour, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(valText, x + w / 2, y + h / 2);
}

// Brightness slider with marker (0-100)
void drawBrightnessSlider(int x, int y, int w, int h, int percent, bool selected, bool activated) {
  uint16_t bg = (activated && selected) ? currentTheme->activeColour : currentTheme->mainColour;
  uint16_t border = selected ? currentTheme->highlightColourBorder : currentTheme->buttonBorder;

  tft.fillRect(x, y, w, h, bg);
  tft.drawRect(x, y, w, h, border);
  if (selected) {
    tft.drawRect(x - 1, y - 1, w + 2, h + 2, currentTheme->highlightColourBorder);
  }

  int lineY = y + h / 2;
  int lineStart = x + 25;
  int lineEnd = x + w - 25;
  tft.drawLine(lineStart, lineY, lineEnd, lineY, currentTheme->textColour);

  // End labels
  tft.setTextSize(1);
  tft.setTextColor(currentTheme->textColour, bg);
  tft.setTextDatum(MR_DATUM);
  tft.drawString("0", lineStart - 6, lineY);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("100", lineEnd + 4, lineY);

  // Marker
  int markerX = map(percent, 0, 100, lineStart, lineEnd);
  int markerW = 6;
  int markerH = 20;
  tft.fillRect(markerX - markerW / 2, lineY - markerH / 2, markerW, markerH, currentTheme->sliderMarkerColour);

  // Instruction (only shown when slider is selected and activated)
  if (selected && activated) {
    String instr = "Rotate to adjust. Press to deactivate, then Update.";
    tft.setTextColor(currentTheme->highlightColourBorder, TFT_BLACK);
    tft.setTextDatum(TL_DATUM); // left-top for precise x placement

    int instrW = tft.textWidth(instr);
    int instrX = x + (w - instrW) / 2;        // center within slider [x, x+w]
    int instrY = y + h + 14;
    int instrH = tft.fontHeight();
    tft.fillRect(x, instrY - (instrH / 2), w, instrH + 2, TFT_BLACK);

    tft.drawString(instr, instrX, instrY);
  }

}

// =========================
// === Menus ===
// =========================
void drawMainMenu() {
  tft.fillScreen(TFT_BLACK);
  drawTitleCentered(20, "-= Main Menu =-");

  ButtonStyle style = Button_Style_FromTheme(currentTheme);

  // Layout evenly
  int numButtons = 6;
  int topPadding = 60;
  int bottomPadding = 10;
  int totalAvailableHeight = SH - topPadding - bottomPadding;
  int totalGapHeight = (numButtons - 1) * BUTTON_GAP;
  int rowHeight = (totalAvailableHeight - totalGapHeight) / numButtons;
  rowHeight = max(rowHeight, 24);

  int buttonX = 10;
  int buttonW = SW - 20;

  for (int i = 0; i < numButtons; i++) {
    int y = topPadding + i * (rowHeight + BUTTON_GAP);
    bool selected = (i == currentSelection);
    Draw_Button(buttonX, y, buttonW, rowHeight, mainMenuItems[i], style, selected, false);
  }
  prevSelection = currentSelection;
}

void drawBrightnessMenu() {
  tft.fillScreen(TFT_BLACK);
  drawTitleCentered(20, "-= Brightness =-");

  // Slider row (row 0)
  drawBrightnessRow(0, currentSelection == 0);

  // “Update” button (row 1)
  drawBrightnessRow(1, currentSelection == 1);

  // Large LCD value remains static here
  int yVal = 20 + (2*8) + 18 + BUTTON_AND_SLIDER_HEIGHT + 36 + BUTTON_AND_SLIDER_HEIGHT + 16;
  drawLCDValue((SW - 120)/2, yVal, 120, BUTTON_AND_SLIDER_HEIGHT,
               String(brightnessPercent) + "%");

  // “Back” button (row 2)
  drawBrightnessRow(2, currentSelection == 2);

  // Remember which row was last drawn selected
  prevSubSel = currentSelection;
}

// redraw exactly one of the three Brightness submenu rows
// row = 0 → slider, 1 → Update button, 2 → Back button
void drawBrightnessRow(int row, bool selected) {
  ButtonStyle bs = Button_Style_FromTheme(currentTheme);
  int y; 
  switch(row) {
    case 0: { // slider row
      // same geometry as drawBrightnessMenu()
      int x = 5;
      int w = SW - 10;
      int h = BUTTON_AND_SLIDER_HEIGHT;
      int sliderY = 20 + (2 * 8) + 18;
      // redraw with selection highlight, but keep 'activated' flag as before
      drawBrightnessSlider(x, sliderY, w, h,
                           tempBrightness,
                           selected,
                           itemActivated);
      return;
    }
    case 1: { // Update button
      // same geometry as drawBrightnessMenu()
      int updW = SW/2 + 10;
      int updX = (SW - updW)/2;
      y = 20 + (2 * 8) + 18 + BUTTON_AND_SLIDER_HEIGHT + 36;
      Draw_Button(updX, y, updW, BUTTON_AND_SLIDER_HEIGHT,
                  "Update", bs, selected, false);
      return;
    }
    case 2: { // Back button
      y = SH - BUTTON_AND_SLIDER_HEIGHT - 10;
      Draw_Button(5, y, SW - 10, BUTTON_AND_SLIDER_HEIGHT,
                  "Back", bs, selected, false);
      return;
    }
  }
}


// =========================
// === Navigation/actions ===
// =========================
void goToMainMenu() {
  inSubmenu = false;
  currentSelection = 0;
  itemActivated = false;
  drawMainMenu();
}

void enterBrightnessMenu() {
  inSubmenu      = true;
  itemActivated  = false;
  tempBrightness = brightnessPercent;
  currentSelection = 0;
  drawBrightnessMenu();   // this will set prevSubSel = 0
}

// Button press action (active LOW on ENCODER_SW)
void pressAction() {
  resetInactivityTimer();
  if (!inSubmenu) {
    // Main menu: only Brightness implemented
    if (currentSelection == 0) {
      enterBrightnessMenu();
    }
    return;
  }

  // In brightness submenu
  if (currentSelection == 0) {
    // Toggle slider activation
    itemActivated = !itemActivated;
    drawBrightnessMenu();
  } else if (currentSelection == 1) {
    // Update
    if (tempBrightness != brightnessPercent) {
      setBrightnessPercent(tempBrightness);
    }
    drawBrightnessMenu();
  } else if (currentSelection == 2) {
    // Back
    goToMainMenu();
  }
}

void rotateAction(int dir) {
  // Wake screen and reset inactivity timer
  resetInactivityTimer();

  // ——— Active slider case: only move the little marker ——
  if (inSubmenu && currentSelection == 0 && itemActivated) {
    int oldPct = tempBrightness;
    tempBrightness = constrain(oldPct + dir, 0, 100);
    if (tempBrightness == oldPct) return;  // no change

    // Slider geometry must match drawBrightnessRow(0)
    const int x       = 5;
    const int y       = 20 + (2*8) + 18;
    const int w       = SW - 10;
    const int h       = BUTTON_AND_SLIDER_HEIGHT;
    const int lineY   = y + h/2;
    const int startX  = x + 25;
    const int endX    = x + w - 25;
    const int mW      = 6;
    const int mH      = 20;

    // Compute old & new marker X
    int oldX = map(oldPct,         0, 100, startX, endX);
    int newX = map(tempBrightness, 0, 100, startX, endX);

    // Erase old marker and draw new one
    tft.fillRect(oldX - mW/2, lineY - mH/2, mW, mH, currentTheme->mainColour);
    tft.fillRect(newX - mW/2, lineY - mH/2, mW, mH, currentTheme->sliderMarkerColour);

    // Update PWM only on real change
    static int lastDuty = -1;
    int duty = dutyFromPercent(tempBrightness);
    if (duty != lastDuty) {
      ledcWrite(LED_PWM_PIN, duty);
      lastDuty = duty;
      if (Debug) Serial.printf("Preview: %d%% (duty=%d)\n", tempBrightness, duty);
    }
    return;
  }

  // ——— Brightness submenu, slider NOT active: only repaint two rows ——
  if (inSubmenu) {
    int oldSel = prevSubSel;
    const int N = 3;  // slider, Update, Back
    currentSelection = (oldSel + (dir > 0 ? 1 : -1) + N) % N;

    // Erase old highlight, draw new highlight
    drawBrightnessRow(oldSel, false);
    drawBrightnessRow(currentSelection, true);

    // Remember for next turn
    prevSubSel = currentSelection;
    return;
  }

  // ——— Main menu: unchanged differential update ——
  {
    int oldSel = prevSelection;
    const int N = 6;
    currentSelection = (oldSel + (dir > 0 ? 1 : -1) + N) % N;

    drawMainMenuRow(oldSel, false);
    drawMainMenuRow(currentSelection, true);

    prevSelection = currentSelection;
  }
}



// ========================================================================================================================
// ===================================================== Setup & Loop =====================================================
// ========================================================================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  // Encoder inputs
  // GPIO35 has no internal pull-up. Use INPUT and add external 10k pull-up to 3.3V.
  pinMode(ENCODER_CLK, INPUT);
  pinMode(ENCODER_DT,  INPUT_PULLUP);
  pinMode(ENCODER_SW,  INPUT_PULLUP);


  // Display
  tft.init();
  Serial.printf("Panel init OK. w=%d, h=%d, rotation=%d\n",
              tft.width(), tft.height(), tft.getRotation());
  tft.setRotation(0); // Portrait
  Serial.printf("Panel init OK. w=%d, h=%d, rotation=%d\n",
              tft.width(), tft.height(), tft.getRotation());
  tft.fillScreen(TFT_BLACK);

  currentSelection = 0;      // highlight the very first button
  prevSelection    = 0;      // make sure diff-update thinks nothing moved yet
  drawMainMenu();           // draws with button[0] selected

  // PWM init (new API)
  ledcAttach(LED_PWM_PIN, PWM_FREQ, PWM_RESOLUTION);
  setBrightnessPercent(brightnessPercent);

  // Draw initial screen
  drawMainMenu();

  // Begin inactivity count
  lastActivity = millis();

  if (Debug) {
    Serial.println("CYD ready: portrait, theme applied, PWM attached on GPIO25.");
  }
}

void loop() {
  // Poll inputs every cycle (no ISRs)
  static int lastCLK = HIGH;
  static const unsigned long btnDebounce = 250;
  static unsigned long lastBtnTime = 0;

  int clk = digitalRead(ENCODER_CLK);
  int dt  = digitalRead(ENCODER_DT);
  int sw  = digitalRead(ENCODER_SW);

  // Button handling (active LOW)
  unsigned long now = millis();
  if (sw == LOW && (now - lastBtnTime) > btnDebounce) {
    lastBtnTime = now;
    pressAction();
  }

  // Encoder step: use falling edge of CLK; DT determines direction
  if (clk != lastCLK && clk == LOW) {
    int direction = (dt != clk) ? +1 : -1;
    rotateAction(direction);
  }
  lastCLK = clk;
  
  // Enter sleep if idle
  checkInactivity();
  // No blocking delays; UI updates triggered by input only
}


// Blank the entire screen and mark sleeping
void sleepScreen() {
  screenSleeping = true;
  tft.fillScreen(TFT_BLACK);
}

// Wake the screen by redrawing the current menu
void wakeScreen() {
  screenSleeping = false;
  if (inSubmenu) {
    drawBrightnessMenu();
  } else {
    drawMainMenu();
  }
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
