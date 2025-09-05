//Matts Notes, to upload to the board, hold down Boot button when you see the Connecting..... text

//Using DOIT ESP32 DEVIKIT V1 as defined board
//The CYD (Cheap Yellow Display) uses 4 Pin JST 1.25mm connectors

// Define TOUCH_CS before including TFT_eSPI to suppress warning
#define TOUCH_CS 100 //nonsense assignment to prevent seeing error on upload

#include <TFT_eSPI.h>
#include <SPI.h>

// Display setup for ESP32-2432S028R (Little Yellow Display)
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320

// Encoder pins (HW-040 connections)
#define ENCODER_CLK 35  // Blue wire
#define ENCODER_DT  27  // Orange wire  
#define ENCODER_SW  22  // Yellow wire (VCC=Red, GND=Black)

// LED Strip PWM Control
#define LED_PWM_PIN 25  // Repurposed touch pin for MOSFET control
#define PWM_FREQ 1000
#define PWM_RESOLUTION 8  // 0-255 range

// --- Global Colour Palette (Base HEX values converted to RGB565) ---
#define COLOR_DUKE_BLUE         0x0113  // #090C9B -> RGB(9, 12, 155)
#define COLOR_TRUE_BLUE         0x3277  // #3066BE -> RGB(48, 102, 190)
#define COLOR_POWDER_BLUE       0xB5B7  // #B4C5E4 -> RGB(180, 197, 228)
#define COLOR_IVORY             0xFFFE  // #FBFFF1 -> RGB(251, 255, 241)
#define COLOR_NEUTRAL_GREY      0x4208  // #404040 -> RGB(64, 64, 64) - Intended for 'pressed' state
// --- End Global Colour Palette ---


// --- Colour Theme Structure ---
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

// --- Define Available Themes ---
const ColourTheme colourThemes[] = {
  { // Theme 0: Duke Blue Default
    COLOR_DUKE_BLUE,        // mainColour
    COLOR_IVORY,            // highlightColourBorder
    COLOR_TRUE_BLUE,        // activeColour
    COLOR_NEUTRAL_GREY,     // pressedColour
    COLOR_DUKE_BLUE,        // buttonBorder
    COLOR_IVORY,            // textColour
    COLOR_POWDER_BLUE,      // sliderMarkerColour
    COLOR_DUKE_BLUE         // lcdValueColour (mainColour)
  }
  // Add more themes here if needed, e.g.:
  /*
  { // Theme 1: Another Theme
    TFT_DARKGREEN,        // mainColour
    TFT_WHITE,            // highlightColourBorder
    TFT_GREEN,            // activeColour
    TFT_DARKGREY,         // pressedColour
    TFT_DARKGREEN,        // buttonBorder
    TFT_WHITE,            // textColour
    TFT_YELLOW,           // sliderMarkerColour
    TFT_DARKGREEN         // lcdValueColour
  }
  */
};

// --- Global Theme Variables ---
int activeThemeIndex = 0; // Index of the currently active theme in the array
const ColourTheme* currentTheme = &colourThemes[activeThemeIndex]; // Pointer to the active theme

// --- Debug Flag ---
bool Debug = true; // Set to 'false' to disable all debug serial output

// --- Global Constants ---
const int BUTTON_AND_SLIDER_HEIGHT = 36; // Constant height for submenu buttons and slider


TFT_eSPI tft = TFT_eSPI();

int currentSelection = 0;
int lastSelection = -1;
bool inSubmenu = false;
bool buttonPressed = false;
bool itemActivated = false;  // NEW: Track if current item is activated

// LED Brightness Control
int brightnessPercent = 50;  // Current applied brightness
int tempBrightness = 50;     // Temporary value while adjusting

unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 300;

int menuStack[10];
int stackIndex = 0;

String mainMenuItems[6] = {"Brightness", "Option 2", "Option 3", "Option 4", "Option 5", "Option 6"};

// --- Enum for Generic Button Types ---
enum ButtonType {
  MainMenuItem,
  Update,
  Back,
  Slider // Although slider is separate, this could conceptually be a 'type' if needed
};

// ---------------------------------------------------------------------------  Function Declarations  ---------------------------------------------------------------------------
void setup();
void loop();
void handleEncoder();
void handleButtonPress();
void pushMenu(int menuID);
void popMenu();
void goToMainMenu();

void drawMainMenu();
void drawBrightnessMenu();
void drawBrightnessSlider(int startY, int height);
void drawUpdateButton(int startY, int height);
void drawBackButton();
void drawBrightnessDisplay(int startY);

// New generic UI drawing functions
void drawTitle(int y_position, const String& title_text);
void drawGenericButton(int x_pos, int y_pos, int button_width, int button_height, const String& button_text, bool is_selected, ButtonType type);


// ---------------------------------------------------------------------------  Setup  ---------------------------------------------------------------------------

void setup() {

  pinMode(ENCODER_CLK, INPUT_PULLUP);
  pinMode(ENCODER_DT, INPUT_PULLUP);
  pinMode(ENCODER_SW, INPUT_PULLUP);

  Serial.begin(115200);
  
  // Initialize PWM for LED strip control (ESP32 Arduino Core v3.x compatible)
  ledcAttach(LED_PWM_PIN, PWM_FREQ, PWM_RESOLUTION);
  
  // Set default brightness (50%)
  setBrightness(brightnessPercent);
  
  // Initialize display in portrait mode
  tft.init();
  tft.setRotation(0); // Portrait orientation
  tft.fillScreen(TFT_BLACK);
  
  drawMainMenu();

}

// ---------------------------------------------------------------------------  Main Loop  ---------------------------------------------------------------------------

void loop() {
  handleEncoder();

  // Corrected logic: button is pressed when the pin reads LOW
  bool buttonIsPressed = digitalRead(ENCODER_SW) == LOW; 
  if (buttonIsPressed && !buttonPressed && (millis() - lastDebounceTime > debounceDelay)) {
    lastDebounceTime = millis();
    buttonPressed = true;
    handleButtonPress();
  }
  if (!buttonIsPressed) { // If button is no longer pressed (i.e., HIGH), reset buttonPressed flag
    buttonPressed = false;
  }
}

// ---------------------------------------------------------------------------  Functions  ---------------------------------------------------------------------------

// --- Generic UI Drawing Functions ---

void drawTitle(int y_position, const String& title_text) {
  tft.setTextSize(2);
  tft.setTextColor(currentTheme->textColour);
  int titleWidth = tft.textWidth(title_text); // Use tft.textWidth for accurate centering
  int titleX = (SCREEN_WIDTH - titleWidth) / 2;
  tft.setCursor(titleX, y_position);
  tft.print(title_text);
}

void drawGenericButton(int x_pos, int y_pos, int button_width, int button_height, const String& button_text, bool is_selected, ButtonType type) {
  // Clearing logic for the button area
  // Clear a slightly larger area than the button's current bounding box
  tft.fillRect(x_pos - 2, y_pos - 2, button_width + 4, button_height + 4, TFT_BLACK);

  // Determine colours based on selection state
  uint16_t bgColor = currentTheme->mainColour;
  uint16_t borderColor = currentTheme->buttonBorder;

  if (is_selected) {
    bgColor = currentTheme->activeColour;
    borderColor = currentTheme->highlightColourBorder;
  }

  // Draw button background (no rounded corners)
  tft.fillRect(x_pos, y_pos, button_width, button_height, bgColor);
  // Draw button border
  tft.drawRect(x_pos, y_pos, button_width, button_height, borderColor);

  // If selected, draw an additional highlight outline
  if (is_selected) {
    tft.drawRect(x_pos - 1, y_pos - 1, button_width + 2, button_height + 2, currentTheme->highlightColourBorder);
  }

  // Draw text
  tft.setTextSize(2); // Consistent text size for all main/sub menu buttons
  tft.setTextColor(currentTheme->textColour);
  int textHeightChar = 2 * 8; // For setTextSize(2)
  int textY = y_pos + (button_height - textHeightChar) / 2;

  String displayText = button_text;
  if (is_selected && (type == ButtonType::MainMenuItem)) { // Only add arrow for selected main menu items
    displayText = "▶ " + button_text;
  }

  int textWidth = tft.textWidth(displayText);
  int textX = x_pos + (button_width - textWidth) / 2; // Centre text horizontally

  tft.setCursor(textX, textY);
  tft.print(displayText);
}

// ---- Menu Drawing Functions ----

void drawMainMenu() {
  // Only clear screen on first draw
  if (lastSelection == -1) {
    tft.fillScreen(TFT_BLACK);
    drawTitle(20, "-= Main Menu =-");
  }

  // Calculate button dimensions to evenly fill screen with small gaps
  int numButtons = 6;
  int topPadding = 60; // Start buttons below the title
  int bottomPadding = 10; // Padding from bottom of screen
  int totalAvailableHeight = SCREEN_HEIGHT - topPadding - bottomPadding;
  
  int buttonGap = 5; // Pixels between buttons
  int totalGapHeight = (numButtons - 1) * buttonGap;

  int rowHeight = (totalAvailableHeight - totalGapHeight) / numButtons;
  if (rowHeight < 20) rowHeight = 20; // Ensure a minimum height

  int buttonX = 10; // X position for buttons (with some padding from left edge)
  int buttonW = SCREEN_WIDTH - 20; // Button width (with 10px padding on each side)
  
  for (int i = 0; i < numButtons; i++) {
    // Only redraw if selection changed or on first draw
    if (i == currentSelection || i == lastSelection || lastSelection == -1) {
      int y = topPadding + i * (rowHeight + buttonGap);
      drawGenericButton(buttonX, y, buttonW, rowHeight, mainMenuItems[i], (i == currentSelection), ButtonType::MainMenuItem);
    }
  }
  
  lastSelection = currentSelection;
}

void drawBrightnessMenu() {
  // Only clear screen on first draw
  if (lastSelection == -1) {
    tft.fillScreen(TFT_BLACK);
    drawTitle(20, "-= Brightness =-");
  }
  
  // --- Calculate vertical positions for components ---
  // Start below the title (Title Y + Title Height + Padding)
  int currentY = 20 + (2 * 8) + 20;

  // 1. Slider
  int sliderDisplayY = currentY; 
  drawBrightnessSlider(sliderDisplayY, BUTTON_AND_SLIDER_HEIGHT);

  // Fixed height for instructions area (always reserved to prevent layout shifts)
  String instructionStringForCalc = "Press button to return, then select the Update button";
  tft.setTextSize(1); // Set text size for accurate fontHeight() measurement
  tft.setTextWrap(true); // Temporarily enable wrap for measurement
  int textLineHeight = tft.fontHeight(); // Height of a single line (size 1)
  int availableSliderTextWidth = (SCREEN_WIDTH - 10) - (5 * 2); // Slider width minus side padding
  
  int numLines = 1;
  // Estimate number of lines needed for the instruction text
  if (tft.textWidth(instructionStringForCalc) > availableSliderTextWidth) {
      numLines = (tft.textWidth(instructionStringForCalc) + availableSliderTextWidth - 1) / availableSliderTextWidth;
  }
  // Reserve fixed height for instructions: (lines * line_height) + (inter-line_spacing) + additional_bottom_padding
  // Increased additional_bottom_padding to 15 to provide more vertical separation.
  const int FIXED_INSTRUCTIONS_AREA_HEIGHT = (numLines * textLineHeight) + (numLines > 1 ? (numLines - 1) * 2 : 0) + 15; 
  tft.setTextWrap(false); // Reset text wrap to default

  currentY = sliderDisplayY + BUTTON_AND_SLIDER_HEIGHT + FIXED_INSTRUCTIONS_AREA_HEIGHT; 
  
  // 2. Update button
  int updateButtonY = currentY;
  drawUpdateButton(updateButtonY, BUTTON_AND_SLIDER_HEIGHT);
  // Update currentY position after the update button
  currentY = updateButtonY + BUTTON_AND_SLIDER_HEIGHT + 15; // Padding after update button

  // 3. Back button (position calculated from screen bottom, so define its top Y for layout reference)
  int backButtonH = BUTTON_AND_SLIDER_HEIGHT;
  int backButtonY = SCREEN_HEIGHT - backButtonH - 10; // 10px from bottom

  // 4. LCD Display (positioned dynamically in the remaining space)
  int spaceAboveBackBtnY = backButtonY; // Top Y of the Back button
  int spaceBelowUpdateBtnY = currentY;   // Bottom Y of the area after Update button and its padding

  int availableSpaceForDisplay = spaceAboveBackBtnY - spaceBelowUpdateBtnY;
  int lcdDisplayH = BUTTON_AND_SLIDER_HEIGHT; // LCD display same height as buttons
  int lcdDisplayY = spaceBelowUpdateBtnY + (availableSpaceForDisplay - lcdDisplayH) / 2;
  
  // Call the new generic LCD drawing function
  drawLCDDisplay(lcdDisplayY, lcdDisplayH, String(brightnessPercent) + "%", currentTheme->lcdValueColour);

  // 5. Back button (drawn last to ensure it's on top if there are any tiny overlaps)
  drawBackButton(); 

  lastSelection = currentSelection; // Keep lastSelection updated for menu-level redraws
}

void drawBrightnessSlider(int startY, int height) {
  static int lastTempBrightnessValue = -1; // Tracks previous tempBrightness to optimise marker redraw
  static bool lastItemActivatedState = false; // Tracks previous itemActivated state for instructions redraw
  static int lastCurrentSelectionState = -1; // Tracks previous currentSelection for border redraw
  static int lastSliderHeight = -1; // Track height changes to force full redraw
  static int lastSliderWidth = -1; // Track width changes to force full redraw

  // Slider positioning
  int sliderX = 5; // 5px padding from sides for 100% width look
  int sliderY = startY; // Use passed startY
  int sliderW = SCREEN_WIDTH - 10; // Full width with 5px padding on each side
  int sliderH = height; // Use passed height
  int lineY = sliderY + sliderH / 2;
  int lineStartX = sliderX + 25; // Adjusted start X for the line
  int lineEndX = sliderX + sliderW - 25; // Adjusted end X for the line

  bool selectionChanged = (currentSelection != lastCurrentSelectionState);
  bool itemActivationChanged = (itemActivated != lastItemActivatedState);
  bool heightChanged = (height != lastSliderHeight);
  bool widthChanged = (sliderW != lastSliderWidth);

  // Redraw static parts (background, border, labels) only if state changes, size changes, or on first draw
  if (lastCurrentSelectionState == -1 || selectionChanged || heightChanged || widthChanged) {
    tft.fillRect(sliderX - 2, sliderY - 2, sliderW + 4, sliderH + 4, TFT_BLACK); 
    
    uint16_t bgColor = currentTheme->mainColour; 
    if (currentSelection == 0 && itemActivated) { 
      bgColor = currentTheme->activeColour; 
    }
    
    tft.fillRect(sliderX, sliderY, sliderW, sliderH, bgColor);
    
    uint16_t borderColor = (currentSelection == 0) ? currentTheme->highlightColourBorder : currentTheme->buttonBorder;
    tft.drawRect(sliderX, sliderY, sliderW, sliderH, borderColor);
    
    if (currentSelection == 0) {
      tft.drawRect(sliderX - 1, sliderY - 1, sliderW + 2, sliderH + 2, currentTheme->highlightColourBorder);
    }
    
    tft.drawLine(lineStartX, lineY, lineEndX, lineY, currentTheme->textColour );
    
    tft.setTextSize(1);
    tft.setTextColor(currentTheme->textColour);
    tft.setCursor(sliderX + 8, lineY - 4);
    tft.print("0");
    tft.setCursor(lineEndX + 8, lineY - 4);
    tft.print("100");
    
    lastCurrentSelectionState = currentSelection;
    lastSliderHeight = height;
    lastSliderWidth = sliderW;
    lastTempBrightnessValue = -1; // Force marker redraw if static parts changed
  }

  // --- Helper Text (Instructions) ---
  int instructionsY = sliderY + sliderH + 8; // Y position for the start of the instructions text
  
  String instructionsText = "Press button to return, then select the Update button";
  tft.setTextSize(1); 
  int textLineHeight = tft.fontHeight();
  int availableSliderTextWidth = sliderW - (5 * 2); 
  
  int numLines = 1;
  if (tft.textWidth(instructionsText) > availableSliderTextWidth) {
      numLines = (tft.textWidth(instructionsText) + availableSliderTextWidth - 1) / availableSliderTextWidth;
  }
  // The FIXED_INSTRUCTIONS_AREA_HEIGHT matches definition in drawBrightnessMenu for consistent layout reservation.
  const int EXPECTED_SLIDER_INSTRUCTIONS_HEIGHT = (numLines * textLineHeight) + (numLines > 1 ? (numLines - 1) * 2 : 0) + 15; // Matches value from drawBrightnessMenu
  
  // Clear area for instructions (always clear the full expected area)
  tft.fillRect(sliderX - 5, instructionsY - 2, sliderW + 10, EXPECTED_SLIDER_INSTRUCTIONS_HEIGHT + 4, TFT_BLACK); 

  if (currentSelection == 0 && itemActivated) {
    tft.setTextSize(1);
    tft.setTextColor(currentTheme->highlightColourBorder); 
    
    tft.setTextWrap(true); 
    
    // Calculate initial X position for the text block to be visually centered
    int textBlockWidth = tft.textWidth(instructionsText); 
    int instructionsTextX = sliderX + (sliderW - textBlockWidth) / 2; 
    instructionsTextX = max(sliderX + 5, instructionsTextX); // Ensure minimum left padding
    
    tft.setCursor(instructionsTextX, instructionsY);
    tft.print(instructionsText);
    tft.setTextWrap(false); 
  }
  lastItemActivatedState = itemActivated;
  
  // Only redraw slider marker if tempBrightness changes or on first draw or size changes
  if (tempBrightness != lastTempBrightnessValue || lastTempBrightnessValue == -1 || heightChanged || widthChanged) { 
    // Erase old marker
    if (lastTempBrightnessValue != -1) { 
      int oldMarkerX = map(lastTempBrightnessValue, 0, 100, lineStartX, lineEndX);
      uint16_t clearColour = (currentSelection == 0 && itemActivated) ? currentTheme->activeColour : currentTheme->mainColour;
      tft.fillRect(oldMarkerX - 6/2, lineY - 20/2, 6, 20, clearColour);
    }
    
    // Draw new marker
    int markerX = map(tempBrightness, 0, 100, lineStartX, lineEndX);
    int markerW = 6;
    int markerH = 20;
    tft.fillRect(markerX - markerW/2, lineY - markerH/2, markerW, markerH, currentTheme->sliderMarkerColour);
    
    lastTempBrightnessValue = tempBrightness;
  }
}

void drawUpdateButton(int startY, int height) {
  static int lastCurrentSelectionState = -1; 
  static int lastButtonHeight = -1; 
  static int lastCalculatedButtonWidth = -1; 
  static int lastButtonX = -1; 

  String updateText = "Update";
  tft.setTextSize(2); 
  int updateTextW = tft.textWidth(updateText); 

  int textPaddingX = 20; 

  int calculatedButtonW = updateTextW + (textPaddingX * 2);
  calculatedButtonW = max(calculatedButtonW, SCREEN_WIDTH / 4); 

  int buttonX = (SCREEN_WIDTH - calculatedButtonW) / 2;
  
  int buttonH = height; 
  int buttonY = startY; 
  
  bool selectionChanged = (currentSelection != lastCurrentSelectionState);
  bool sizeOrPositionChanged = (buttonH != lastButtonHeight || calculatedButtonW != lastCalculatedButtonWidth || buttonX != lastButtonX);

  if (lastCurrentSelectionState == -1 || selectionChanged || sizeOrPositionChanged) {
    drawGenericButton(buttonX, buttonY, calculatedButtonW, buttonH, updateText, (currentSelection == 1), ButtonType::Update);

    lastCurrentSelectionState = currentSelection;
    lastButtonHeight = buttonH;
    lastCalculatedButtonWidth = calculatedButtonW;
    lastButtonX = buttonX;
  }
}

void drawLCDDisplay(int startY, int display_height, const String& value_text, uint16_t value_colour) { 
  // All static variables are now within this function for its state tracking
  static String lastDisplayedValueText = "";
  static int lastStartY = -1; 
  static int lastDisplayH = -1; 

  // Display positioning
  int displayY = startY; 
  int displayH = display_height; // Use passed height
  int displayW = 120;
  int displayX = (SCREEN_WIDTH - displayW) / 2;
  
  // Check if position, size, or text value has changed to optimize redraws
  bool startYChanged = (startY != lastStartY);
  bool heightChanged = (displayH != lastDisplayH);
  bool valueTextChanged = (value_text != lastDisplayedValueText); 

  // --- Robust Clearing Logic ---
  // Clear a rectangle that covers both the current and last known position/size.
  // This ensures any old content (including borders or text) from previous positions/states is removed, preventing ghosting.
  int clearAreaYStart = min(displayY, (lastStartY != -1 ? lastStartY : displayY)) - 6;
  int clearAreaYEnd = max(displayY + displayH, (lastStartY != -1 ? lastStartY + lastDisplayH : displayY + displayH)) + 6;
  tft.fillRect(displayX - 6, clearAreaYStart, displayW + 12, clearAreaYEnd - clearAreaYStart, TFT_BLACK);

  // --- Draw Borders ---
  // Always redraw borders to ensure they are consistent after clearing
  tft.drawRect(displayX - 5, displayY - 5, displayW + 10, displayH + 10, currentTheme->buttonBorder); 
  
  // Update static tracking variables AFTER clearing and before drawing content
  lastStartY = displayY;
  lastDisplayH = displayH;
  
  // --- Draw Value Text ---
  // If value text changed, OR position/size changed, OR it's the first time drawing after reset
  // The crucial part for fixing re-entry and selector-move disappearance is `lastDisplayedValueText == ""`
  // This ensures it draws on initial entry to the menu, and after pushMenu() resets this static.
  if (valueTextChanged || startYChanged || heightChanged || lastDisplayedValueText == "") {
    tft.setTextSize(3); // LCD font (size 3)
    tft.setTextColor(value_colour); // Use passed colour

    int newTextW = tft.textWidth(value_text); 
    int textHeightForSize3 = 3 * 8; // Approx height for size 3 font
    int textY = displayY + (displayH - textHeightForSize3) / 2; // Vertically center within the displayH
    
    // Clear only the text area within the display widget's bounds,
    // before drawing new text. Use max width for clearing to avoid artifacts if old text was longer.
    // Chaining max calls to correctly find the maximum of three values, casting to int for consistency.
    int clearTextWidth = max(max((int)tft.textWidth(lastDisplayedValueText), newTextW), (int)tft.textWidth("100%")); // Corrected max() call with chaining and explicit casts
    int clearTextX = displayX + (displayW - clearTextWidth) / 2;
    tft.fillRect(clearTextX, textY, clearTextWidth, textHeightForSize3, TFT_BLACK);

    int newTextX = displayX + (displayW - newTextW) / 2;
    tft.setCursor(newTextX, textY);
    tft.print(value_text);
    lastDisplayedValueText = value_text; // Update displayed text for next comparison
  }
}

void drawBackButton() {
  static int lastCurrentSelectionState = -1; 
  static int lastButtonHeight = -1; 

  int backButtonH = BUTTON_AND_SLIDER_HEIGHT; 
  int backButtonY = SCREEN_HEIGHT - backButtonH - 10;
  int backButtonX = 5; 
  int backButtonW = SCREEN_WIDTH - 10;

  bool selectionChanged = (currentSelection != lastCurrentSelectionState);
  bool heightChanged = (backButtonH != lastButtonHeight);

  if (lastCurrentSelectionState == -1 || selectionChanged || heightChanged) {
    drawGenericButton(backButtonX, backButtonY, backButtonW, backButtonH, "Back", (currentSelection == 2), ButtonType::Back);

    lastCurrentSelectionState = currentSelection;
    lastButtonHeight = backButtonH;
  }
}

// PWM Control Functions
void setBrightness(int percent) {
  percent = constrain(percent, 0, 100);
  int pwmValue = map(percent, 0, 100, 0, (1 << PWM_RESOLUTION) - 1); // Use (1 << PWM_RESOLUTION) - 1 for max value
  ledcWrite(LED_PWM_PIN, pwmValue);
  
  if (Debug) { // Debug output conditional
    float maxVoltage = 24.0; // Assuming 24V supply for the LED strips
    float averageVoltage = (float)pwmValue / ((1 << PWM_RESOLUTION) - 1) * maxVoltage;
    
    Serial.print("Brightness set to: ");
    Serial.print(percent);
    Serial.print("% (PWM: ");
    Serial.print(pwmValue);
    Serial.print(", [debug] Expected Multimeter Reading (DC Avg): ");
    Serial.print(averageVoltage, 2); // Print with 2 decimal places
    Serial.println("V)");
  }
}

// ---- Input Handling ----
void handleEncoder() {
  static uint8_t lastState = 0;
  static int encoderCounter = 0; // Add counter for sensitivity reduction

  // Read current CLK and DT
  uint8_t clk = digitalRead(ENCODER_CLK);
  uint8_t dt  = digitalRead(ENCODER_DT);
  uint8_t state = (clk << 1) | dt; // Combine CLK and DT into a 2-bit state

  // Only process if state has changed
  if (state == lastState) return;

  // Determine direction based on state transitions
  int direction = 0;
  // Clockwise transitions (assuming common encoder wiring for typical CW output)
  if ((lastState == 0b00 && state == 0b10) || // 00 -> 10
      (lastState == 0b10 && state == 0b11) || // 10 -> 11
      (lastState == 0b11 && state == 0b01) || // 11 -> 01
      (lastState == 0b01 && state == 0b00)) { // 01 -> 00
    direction = 1; // Clockwise
  } 
  // Counter-clockwise transitions
  else if ((lastState == 0b00 && state == 0b01) || // 00 -> 01
           (lastState == 0b01 && state == 0b11) || // 01 -> 11
           (lastState == 0b11 && state == 0b10) || // 11 -> 10
           (lastState == 0b10 && state == 0b00)) { // 10 -> 00
    direction = -1; // Counter-clockwise
  }
  // If direction is 0, it might be an invalid transition (e.g., noise or skipped state)
  if (direction == 0) { 
      lastState = state; // Still update lastState to reflect the current physical state
      return; 
  }

  // Handle encoder based on current state
  if (inSubmenu && itemActivated && currentSelection == 0) {
    // Brightness slider is activated - adjust brightness (no sensitivity reduction for slider)
    tempBrightness += direction;
    tempBrightness = constrain(tempBrightness, 0, 100);
    
    if (Debug) { // Debug output conditional
      Serial.print("Brightness adjusted to: ");
      Serial.println(tempBrightness);
    }
    
    // Call the full brightness menu redraw to ensure correct layout and optimised component updates
    drawBrightnessMenu();
    
  } else {
    // Normal menu navigation with reduced sensitivity
    encoderCounter += direction;
    
    // Only move selection every 2 encoder steps for reduced sensitivity
    if (abs(encoderCounter) >= 2) {
      int moveDirection = (encoderCounter > 0) ? 1 : -1;
      encoderCounter = 0; // Reset counter
      
      currentSelection += moveDirection;

      // Handle different menu lengths
      int maxItems;
      if (inSubmenu) {
        maxItems = 3; // Brightness menu: Slider (0), Update button (1), Back button (2)
        if (currentSelection >= maxItems) currentSelection = 0;
        if (currentSelection < 0) currentSelection = maxItems - 1; // Go to Back button
      } else {
        maxItems = 6; // Main menu
        if (currentSelection >= maxItems) currentSelection = 0;
        if (currentSelection < 0) currentSelection = maxItems - 1;
      }

      if (Debug) { // Debug output conditional
        Serial.print("Menu navigation → Selection: ");
        Serial.println(currentSelection);
      }

      // Redraw appropriate menu (optimised)
      if (inSubmenu) {
        drawBrightnessMenu();
      } else {
        drawMainMenu();
      }
    }
  }

  lastState = state; // Update lastState only after valid processing
}

void handleButtonPress() {
  if (Debug) { // Debug output conditional
    Serial.println("Encoder button pressed.");
  }
  
  if (inSubmenu) {
    if (currentSelection == 0) { // Slider
      if (itemActivated) {
        // Deactivate slider
        itemActivated = false;
        if (Debug) { // Debug output conditional
          Serial.println("Brightness slider deactivated");
        }
        drawBrightnessMenu(); // Redraw to remove instructions
      } else {
        // Activate slider
        itemActivated = true;
        tempBrightness = brightnessPercent; // Initialize temp with current brightness
        if (Debug) { // Debug output conditional
          Serial.println("Brightness slider activated");
        }
        drawBrightnessMenu(); // Redraw to show instructions and preview
      }
    } else if (currentSelection == 1) { // Update button
      // Apply the temp brightness value
      if (tempBrightness != brightnessPercent) {
        brightnessPercent = tempBrightness;
        setBrightness(brightnessPercent);      
        // Removed the "Brightness Updated to X %" confirmation text display as requested
        delay(1000);
        drawBrightnessMenu(); // Redraw after update
      }
    } else if (currentSelection == 2) { // Back button (new position)
      if (Debug) { // Debug output conditional
        Serial.println("Going to main menu via Back button.");
      }
      // Momentarily show "pressed" state for Back button
      int backButtonH = BUTTON_AND_SLIDER_HEIGHT;
      int backButtonY = SCREEN_HEIGHT - backButtonH - 10;
      int backButtonX = 5;
      int backButtonW = SCREEN_WIDTH - 10;

      // Draw the pressed state explicitly using drawGenericButton
      // Note: drawGenericButton clears its own area, so we can directly call it for pressed state
      drawGenericButton(backButtonX, backButtonY, backButtonW, backButtonH, "Back", true, ButtonType::Back); // Draw selected state
      tft.fillRect(backButtonX, backButtonY, backButtonW, backButtonH, currentTheme->pressedColour); // Overlay with pressed colour
      // Redraw text and borders for the pressed state
      tft.setTextSize(2);
      tft.setTextColor(currentTheme->textColour);
      String backText = "Back";
      int textHeightChar = 2 * 8;
      int backTextW = tft.textWidth(backText);
      int textX = backButtonX + (backButtonW - backTextW) / 2;
      int textY = backButtonY + (backButtonH - textHeightChar) / 2;
      tft.setCursor(textX, textY);
      tft.print(backText);
      tft.drawRect(backButtonX - 1, backButtonY - 1, backButtonW + 2, backButtonH + 2, currentTheme->highlightColourBorder); // Highlight border

      delay(150); // Short delay to show pressed state

      goToMainMenu();
      drawMainMenu();
    }
  } else {
    // Main menu selection - all options go to brightness submenu (as per current logic)
    if (Debug) { // Debug output conditional
      Serial.print("Entering submenu from Option ");
      Serial.println(currentSelection + 1);
    }
    
    // Momentarily show "pressed" state for main menu item
    int numButtons = 6;
    int topPadding = 60;
    int buttonGap = 5;
    int totalAvailableHeight = SCREEN_HEIGHT - topPadding - 10; 
    int totalGapHeight = (numButtons - 1) * buttonGap;
    int rowHeight = (totalAvailableHeight - totalGapHeight) / numButtons;
    if (rowHeight < 20) rowHeight = 20;

    int buttonX = 10;
    int buttonW = SCREEN_WIDTH - 20;
    int buttonY = topPadding + currentSelection * (rowHeight + buttonGap);

    // Draw the pressed state explicitly using drawGenericButton
    drawGenericButton(buttonX, buttonY, buttonW, rowHeight, mainMenuItems[currentSelection], true, ButtonType::MainMenuItem); // Draw selected state
    tft.fillRect(buttonX, buttonY, buttonW, rowHeight, currentTheme->pressedColour); // Overlay with pressed colour
    // Redraw text and borders for the pressed state
    tft.setTextSize(2);
    tft.setTextColor(currentTheme->textColour);
    int textHeightChar = 2 * 8;
    String displayText = "▶ " + mainMenuItems[currentSelection]; // Assuming selected state
    int textWidth = tft.textWidth(displayText);
    int textX = buttonX + (buttonW - textWidth) / 2;
    int textY = buttonY + (rowHeight - textHeightChar) / 2;
    tft.setCursor(textX, textY);
    tft.print(displayText);
    tft.drawRect(buttonX - 1, buttonY - 1, buttonW + 2, rowHeight + 2, currentTheme->highlightColourBorder); // Highlight border

    delay(150); // Short delay

    pushMenu(currentSelection); // Push current main menu selection
    currentSelection = 0;      // Start at slider in brightness menu
    itemActivated = false;     // Ensure slider is not active initially
    tempBrightness = brightnessPercent; // Sync temp with actual brightness
    drawBrightnessMenu();
  }
}

void pushMenu(int menuID) {
  menuStack[stackIndex++] = menuID;
  inSubmenu = true;
  lastSelection = -1; // Force full screen redraw when entering a new menu
  itemActivated = false;
  
  // Reset static variables in drawing functions to force full redraws when menu changes
  // This is important for ensuring the new menu draws correctly without artifacts from the previous state.
  static int brightnessSlider_lastTempBrightnessValue = -1;
  static bool brightnessSlider_lastItemActivatedState = false;
  static int brightnessSlider_lastCurrentSelectionState = -1;
  static String drawLCDDisplay_lastDisplayedValueText = ""; // <--- Reset for LCD text
  static int drawLCDDisplay_lastStartY = -1;               // <--- Reset for LCD position
  static int drawLCDDisplay_lastDisplayH = -1;             // <--- Reset for LCD height
  static int updateButton_lastCurrentSelectionState = -1;
  static int backButton_lastCurrentSelectionState = -1;
  static int brightnessSlider_lastSliderHeight = -1;
  static int brightnessSlider_lastSliderWidth = -1;
  static int updateButton_lastButtonHeight = -1;
  static int updateButton_lastCalculatedButtonWidth = -1;
  static int updateButton_lastButtonX = -1;
  static int backButton_lastButtonHeight = -1;


  brightnessSlider_lastTempBrightnessValue = -1;
  brightnessSlider_lastItemActivatedState = false;
  brightnessSlider_lastCurrentSelectionState = -1;
  drawLCDDisplay_lastDisplayedValueText = ""; // ACTUAL RESET
  drawLCDDisplay_lastStartY = -1;             // ACTUAL RESET
  drawLCDDisplay_lastDisplayH = -1;           // ACTUAL RESET
  updateButton_lastCurrentSelectionState = -1;
  backButton_lastCurrentSelectionState = -1;
  brightnessSlider_lastSliderHeight = -1;
  brightnessSlider_lastSliderWidth = -1;
  updateButton_lastButtonHeight = -1;
  updateButton_lastCalculatedButtonWidth = -1;
  updateButton_lastButtonX = -1;
  backButton_lastButtonHeight = -1;

  tempBrightness = brightnessPercent; // Synchronize tempBrightness with current actual brightness
}

void popMenu() {
  if (stackIndex > 0) stackIndex--;
  inSubmenu = false;
  if (stackIndex > 0) {
      currentSelection = menuStack[stackIndex - 1]; 
  } else {
      currentSelection = 0; 
  }
  lastSelection = -1; 
  itemActivated = false;
  tempBrightness = brightnessPercent;

  // Reset static variables in drawing functions
  static int brightnessSlider_lastTempBrightnessValue = -1;
  static bool brightnessSlider_lastItemActivatedState = false;
  static int brightnessSlider_lastCurrentSelectionState = -1;
  static String brightnessDisplay_lastDisplayedBrightnessText = "";
  static int updateButton_lastCurrentSelectionState = -1;
  static int backButton_lastCurrentSelectionState = -1;
  static int brightnessSlider_lastSliderHeight = -1;
  static int brightnessSlider_lastSliderWidth = -1;
  static int updateButton_lastButtonHeight = -1;
  static int updateButton_lastCalculatedButtonWidth = -1;
  static int updateButton_lastButtonX = -1;
  static int brightnessDisplay_lastStartY = -1;
  static int brightnessDisplay_lastDisplayH = -1; 
  static int backButton_lastButtonHeight = -1;


  brightnessSlider_lastTempBrightnessValue = -1;
  brightnessSlider_lastItemActivatedState = false;
  brightnessSlider_lastCurrentSelectionState = -1;
  brightnessDisplay_lastDisplayedBrightnessText = "";
  updateButton_lastCurrentSelectionState = -1;
  backButton_lastCurrentSelectionState = -1;
  brightnessSlider_lastSliderHeight = -1;
  brightnessSlider_lastSliderWidth = -1;
  updateButton_lastButtonHeight = -1;
  updateButton_lastCalculatedButtonWidth = -1;
  updateButton_lastButtonX = -1;
  brightnessDisplay_lastStartY = -1;
  brightnessDisplay_lastDisplayH = -1; 
  backButton_lastButtonHeight = -1;
}

void goToMainMenu() {
  stackIndex = 0;
  inSubmenu = false;
  currentSelection = 0;
  lastSelection = -1; 
  itemActivated = false;
  tempBrightness = brightnessPercent;

  // Reset static variables in drawing functions
  static int brightnessSlider_lastTempBrightnessValue = -1;
  static bool brightnessSlider_lastItemActivatedState = false;
  static int brightnessSlider_lastCurrentSelectionState = -1;
  static String brightnessDisplay_lastDisplayedBrightnessText = "";
  static int updateButton_lastCurrentSelectionState = -1;
  static int backButton_lastCurrentSelectionState = -1;
  static int brightnessSlider_lastSliderHeight = -1;
  static int brightnessSlider_lastSliderWidth = -1;
  static int updateButton_lastButtonHeight = -1;
  static int updateButton_lastCalculatedButtonWidth = -1;
  static int updateButton_lastButtonX = -1;
  static int brightnessDisplay_lastStartY = -1;
  static int brightnessDisplay_lastDisplayH = -1; 
  static int backButton_lastButtonHeight = -1;


  brightnessSlider_lastTempBrightnessValue = -1;
  brightnessSlider_lastItemActivatedState = false;
  brightnessSlider_lastCurrentSelectionState = -1;
  brightnessDisplay_lastDisplayedBrightnessText = "";
  updateButton_lastCurrentSelectionState = -1;
  backButton_lastCurrentSelectionState = -1;
  brightnessSlider_lastSliderHeight = -1;
  brightnessSlider_lastSliderWidth = -1;
  updateButton_lastButtonHeight = -1;
  updateButton_lastCalculatedButtonWidth = -1;
  updateButton_lastButtonX = -1;
  brightnessDisplay_lastStartY = -1;
  brightnessDisplay_lastDisplayH = -1; 
  backButton_lastButtonHeight = -1;
}
