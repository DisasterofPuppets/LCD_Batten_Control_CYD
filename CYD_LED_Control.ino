/* 
TO DO LIST

// 1. Display the menu elements as in example GUI_Dark.png 
2. Confirm Rotary input and button selection works to enter and exit each Menu Item
3. Ensure Text area (Item 3) displays text from funcions.
4. Update Brightness Menu Item selected functionality
5. Update Animation Menu Item selected functionality
6. Update Speed Menu Item selected functionality
7. Update Speed Menu lock / unlock and style logic based on Animation Menu value
//8. Display horizontal scrolling text placeholder// stop when screen times out
9. Set ESP32_NOW and update functions so when a menu item is applied, check for any other ESP32_Now Devices based on pre-configured names and apply the same changes


================================================================================================
=================================== LED Batten Control (CYD) ===================================
================================================================================================

ESP32-2432S028R 320 x 240 LCD Display (2.8") (Commonly known as Cheap Yellow) based LED Controller Interface with PWM
2025 Disaster of Puppets
Use the code at your own risk, I mean it should be fine..but you know..free and all that.
Ie, If you break hardware / yourself, I am not liable.
TLDR :) Have fun

I selected ESP32 Dev Module as the board.

---------------------------------------------------------------------------------------------------
*/

#include <Arduino.h>
#include <TFT_eSPI.h>   // Ensure your User_Setup matches ESP32-2432S028R see the Users_Setup.h I used.
//#include <ESP32_NOW.h>
//#include <ESP32_NOW_Serial.h>

//------------------------------------------  DEFINES  --------------------------------------------

#define SW tft.width()
#define SH tft.height()

// Inactivity timeout (ms)
#define INACTIVITY_TIMEOUT 60000  

// --- Rotary encoder pins ---
#define ENCODER_CLK 35  // Blue wire (requires external pull-up)
#define ENCODER_DT  27  // Orange wire
#define ENCODER_SW  22  // Yellow wire (active LOW, use INPUT_PULLUP)

// --- PWM config (ESP32 Arduino core v3.x API) ---
#define LED_PWM_PIN     25
#define PWM_FREQ        5000
#define PWM_RESOLUTION  8  // 0-255

// --- Themes ---
//#define COLOUR_BLUE #Hexcode

#define THEME Light // choose a theme from the above options

#define ENCODER_REVERSE False // switch from clockwise to anticlockwise


// --- Theme Colors (Dark Theme based on GUI_Dark.png) ---
#define COLOR_DARK_BACKGROUND         TFT_BLACK         // Main background
#define COLOR_DARK_BORDER_DEFAULT     0xCE79            // Light grey border for unselected/default items
#define COLOR_DARK_BORDER_HOVER       TFT_BLUE          // Blue border for hovered (not active) item
#define COLOR_DARK_BORDER_ACTIVE      TFT_BLUE          // Blue border for active item
#define COLOR_DARK_ITEM_BACKGROUND    0x2104            // Dark grey background for active items
#define COLOR_DARK_TEXT               TFT_WHITE         // General text color
#define COLOR_DARK_SLIDER_BG          0x3186            // Darker grey for slider track
#define COLOR_DARK_SLIDER_FILL        TFT_BLUE          // Blue for slider fill
#define COLOR_DARK_GREYED_OUT_BORDER  0x5AAB            // Lighter grey for greyed-out border
#define COLOR_DARK_GREYED_OUT_TEXT    0x5AAB            // Lighter grey for greyed-out text
#define COLOR_DARK_GREYED_OUT_SLIDER_BG 0x1082          // A very dark grey for the inactive slider track
#define COLOR_DARK_ARROW_COLOR        TFT_WHITE         // Color for animation arrows

// Current theme colors (for easy switching later)
#define THEME_CURRENT_COLORS_BG             COLOR_DARK_BACKGROUND
#define THEME_CURRENT_COLORS_BORDER_DEFAULT COLOR_DARK_BORDER_DEFAULT
#define THEME_CURRENT_COLORS_BORDER_HOVER   COLOR_DARK_BORDER_HOVER
#define THEME_CURRENT_COLORS_BORDER_ACTIVE  COLOR_DARK_BORDER_ACTIVE
#define THEME_CURRENT_COLORS_ITEM_BACKGROUND COLOR_DARK_ITEM_BACKGROUND
#define THEME_CURRENT_COLORS_TEXT           COLOR_DARK_TEXT
#define THEME_CURRENT_COLORS_SLIDER_BG      COLOR_DARK_SLIDER_BG
#define THEME_CURRENT_COLORS_SLIDER_FILL    COLOR_DARK_SLIDER_FILL
#define THEME_CURRENT_COLORS_GREYED_OUT_BORDER COLOR_DARK_GREYED_OUT_BORDER
#define THEME_CURRENT_COLORS_GREYED_OUT_TEXT COLOR_DARK_GREYED_OUT_TEXT
#define THEME_CURRENT_COLORS_ARROW_COLOR    COLOR_DARK_ARROW_COLOR

// Layout constants (adjust as needed to match GUI_Dark.png precisely)
#define ITEM_WIDTH           240
#define ITEM_HEIGHT          50
#define ITEM_MARGIN_TOP      10
#define ITEM_MARGIN_X        ((SW - ITEM_WIDTH) / 2)
#define ITEM_SPACING         10

#define NOTIFICATION_TEXT_Y  (ITEM_MARGIN_TOP + (ITEM_HEIGHT + ITEM_SPACING) * 3 + 20) // Below Speed item + some spacing
#define FOOTER_HEIGHT        30
#define FOOTER_Y             (SH - FOOTER_HEIGHT - -5) // -5px from bottom //dodgy fix to remove the mirror corrupted pixels

// Slider specific
#define SLIDER_BAR_WIDTH     (ITEM_WIDTH - 60) // Adjust for text "75" and padding
#define SLIDER_BAR_HEIGHT    10
#define SLIDER_BAR_X         (ITEM_MARGIN_X + 20)
#define SLIDER_BAR_Y_OFFSET  20 // Offset from item top

// Animation specific
#define ANIMATION_ARROW_WIDTH 15


//------------------------------------------  VARIABLES  ------------------------------------------

static unsigned long lastRotationTime = 0;
const unsigned long ROTATION_DEBOUNCE_MS = 100; // Adjust as needed if your encoder selection of items in main menu feels off(e.g., 50-150ms).  lower if your encoder moves but selection doesn't)

// Track last user action for screen timeout
static unsigned long lastActivity = 0;
static bool screenSleeping = false;

// --- App/UI state ---
TFT_eSPI tft = TFT_eSPI();
bool Debug = true;// disable to remove serial prints

int brightnessPercent = 50;  // default brightness on boot(0-100)
int tempBrightness = 50;     // temporary brightness while adjusting

int previousSelection = 0; // To track the item that was last hovered/active
int currentSelection = 0;   // index os selected item
bool itemActivated = false;  // whether the menu item is active or not


enum AnimationType { NONE, BLINK, LIGHTNING, STROBE };
AnimationType currentAnimation = NONE; 
int speedPercent = 50; // Default speed, though it will be greyed out initially

// --- Footer Scrolling Text Variables ---
String footerText = "MODE: M | Soldiers: 192.16.1.100 / ESP32_001 user: admin We will change this later......"; // Changed to a longer placeholder
static int scrollOffset = 0;
static int textPixelWidth = 0; // Will store the width of the footerText
const int scrollGapPixels = 50; // Gap between repetitions of the text
const int scrollSpeedPixels = 1; // Pixels to move per update
const unsigned long scrollUpdateInterval = 50; // Milliseconds between scroll updates
static unsigned long lastScrollUpdateTime = 0;

// --- TFT_eSprite for Flicker-Free Footer ---
TFT_eSprite footerSprite = TFT_eSprite(&tft); // Create sprite object (pass a pointer to the main TFT object)

// Rotary encoder state
volatile unsigned long lastEncoderBtnPress = 0; // For debouncing
volatile bool encoderBtnPressed = false;
volatile int lastCLK = HIGH;
volatile int lastDT = HIGH;

//-------------------------------------------  SETUP  --------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);

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

// Initialize the footer sprite
  footerSprite.createSprite(SW, FOOTER_HEIGHT); // Create a sprite the size of the footer area
  //footerSprite.setFreeFont(&FreeSans9pt7b); // Use a smooth font for sprite text
  footerSprite.setTextFont(2); 
  footerSprite.setTextSize(1); 
  footerSprite.setTextDatum(TL_DATUM); //  Explicitly set Top-Left datum

  // Calculate textPixelWidth *after* setting font properties for the sprite.
  // This is crucial for accurate scrolling calculations.
  textPixelWidth = footerSprite.textWidth(footerText); 
  if (Debug) {
    Serial.printf("Footer Sprite created: w=%d, h=%d\n", footerSprite.width(), footerSprite.height());
    Serial.printf("Calculated footerText width (Font 2, size 1): %d pixels\n", textPixelWidth);
    Serial.printf("Calculated footerText height (Font 2, size 1): %d pixels\n", footerSprite.fontHeight());
  }

    // Begin inactivity count
  lastActivity = millis();
  
  if (Debug) {
    Serial.println("CYD ready: portrait, theme applied, PWM attached on GPIO25.");
  }
 // Initial draw of the menu as per GUI_Dark.png
  drawFullMenu();
}


//--------------------------------------------  LOOP  --------------------------------------------

void loop() {

  // Declare 'now' once at the beginning of the loop
  unsigned long now = millis();

  // Poll inputs every cycle (no ISRs)
  static const unsigned long btnDebounce = 250;
  static unsigned long lastBtnTime = 0;

  // Read encoder pins
  int clk = digitalRead(ENCODER_CLK);
  int dt = digitalRead(ENCODER_DT);
  int sw = digitalRead(ENCODER_SW);

  // Button handling (active LOW)
  if (sw == LOW && (now - lastBtnTime) > btnDebounce) {
    lastBtnTime = now;
    // Call pressAction directly when button is confirmed pressed
    pressAction(); 
  }

  // Encoder step: use falling edge of CLK; DT determines direction
  if (clk != lastCLK && clk == LOW) { // Falling edge of CLK detected
    // Implement rotation debounce here
    if (now - lastRotationTime > ROTATION_DEBOUNCE_MS) { 
      lastRotationTime = now; // Update last rotation time *after* debounce check

      int detectedDirection = (dt != lastDT) ? -1 : 1; // Raw direction from encoder

      // Apply ENCODER_REVERSE if defined as True
      #if ENCODER_REVERSE == True
          detectedDirection = -detectedDirection;
      #endif

      // Call rotateAction directly with the debounced detected direction (1:1 per debounced click)
      rotateAction(detectedDirection);
    }
  }
  lastCLK = clk;
  lastDT = dt; // Update lastDT after checking for rotation

  // --- Scrolling Footer Logic ---
  // Only update and draw the footer if the screen is NOT sleeping
  if (!screenSleeping && (now - lastScrollUpdateTime >= scrollUpdateInterval)) { 
    lastScrollUpdateTime = now;
    
    scrollOffset += scrollSpeedPixels; 

    if (scrollOffset >= (textPixelWidth + scrollGapPixels)) {
      scrollOffset = 0; 
    }

    // Redraw the content directly to the sprite with the new offset
    drawFooterContent(footerText); 
    
    // Check if screen is still awake before pushing the sprite
    if (!screenSleeping) {
        footerSprite.pushSprite(0, FOOTER_Y); 
    }
  }
  // --- End Scrolling Footer Logic ---


  // Enter sleep if idle
  checkInactivity();

  delay(5); // Small delay to prevent excessive polling
}

//--------------------------------------------  FUNCTIONS  ---------------------------------------


//------------------------------------------------------------------- SLEEP and WAKE

// Blank the entire screen and mark sleeping
void sleepScreen() {
  screenSleeping = true;
  tft.fillScreen(TFT_BLACK);
}

// Wake the screen by redrawing the current menu
void wakeScreen() {
    screenSleeping = false;
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

//------------------------------------------------------------------- ROTARY FUNCTIONS

// Rotary encoder rotation action
void rotateAction(int direction) {
  resetInactivityTimer(); // Reset inactivity timer on any encoder movement

  if (itemActivated) {
    // --- SCENARIO 1: Adjusting values within an active item ---
    if (currentSelection == 0) { // Brightness
      tempBrightness += direction;
      tempBrightness = constrain(tempBrightness, 0, 100);
      // Update ONLY the active item's display
      updateMenuItemDisplay(currentSelection); 
      if (Debug) Serial.printf("Adjusting Brightness (temp): %d\n", tempBrightness);
    } else if (currentSelection == 1) { // Animation
      // For AnimationType, ensure wrap-around (0=NONE, 1=BLINK, 2=LIGHTNING, 3=STROBE)
      currentAnimation = (AnimationType)((currentAnimation + direction + 4) % 4); // +4 for proper wrap-around with negative direction
      // Update ONLY the active item's display
      updateMenuItemDisplay(currentSelection); 
      if (Debug) Serial.printf("Adjusting Animation (temp): %d\n", currentAnimation);
    } else if (currentSelection == 2) { // Speed
      // Adjust speed value (only if not greyed out)
      if (currentAnimation != NONE) { // Only adjust if Animation is not "None"
        speedPercent += direction;
        speedPercent = constrain(speedPercent, 1, 100);
        // Update ONLY the active item's display
        updateMenuItemDisplay(currentSelection); 
        if (Debug) Serial.printf("Adjusting Speed (temp): %d\n", speedPercent);
      } else {
        if (Debug) Serial.println("Cannot adjust Speed, Animation is None.");
      }
    }
  } else {
    // --- SCENARIO 2: Navigating between menu items (when no item is active) ---
    previousSelection = currentSelection; // Store the currently hovered item BEFORE changing

    currentSelection += direction; // Now 'direction' will be a filtered +1 or -1

    // Wrap-around logic for menu items (0, 1, 2)
    if (currentSelection < 0) currentSelection = 2;
    if (currentSelection > 2) currentSelection = 0;

    // "Speed" Item Lockout: If trying to select Speed while Animation is "None", skip it
    if (currentSelection == 2 && currentAnimation == NONE) {
        currentSelection += direction; // Try to move to the next item in the same direction
        if (currentSelection < 0) currentSelection = 2; // Re-wrap if needed
        if (currentSelection > 2) currentSelection = 0; // Re-wrap if needed
        if (Debug) Serial.println("Skipped Speed item (Animation is None).");
    }

    // --- PARTIAL UPDATE LOGIC ---
    // 1. Redraw the PREVIOUSLY selected item to its default (un-hovered) state
    updateMenuItemDisplay(previousSelection);
    // 2. Redraw the NEWLY selected item to its hovered state
    updateMenuItemDisplay(currentSelection);
    // --- END PARTIAL UPDATE LOGIC ---

    if (Debug) Serial.printf("Menu Selection: %d\n", currentSelection);
  }

}

// Button press action (active LOW on ENCODER_SW)
void pressAction() {
  resetInactivityTimer();

  // If the speed item is greyed out and currently selected,
  // a button press when NOT already active should do nothing.
  // This prevents activating a disabled item.
  if (currentSelection == 2 && currentAnimation == NONE && !itemActivated) {
      if (Debug) Serial.println("Cannot activate Speed, Animation is None.");
      return; // Do nothing if trying to activate a greyed-out item
  }

  itemActivated = !itemActivated; // Toggle the active state

  if (!itemActivated) { // If itemActivated just became false (meaning item was deactivated/confirmed)
    handleSelection(currentSelection); // Call handleSelection to finalize settings
  }
  
  // Redraw only the current item to reflect its new active/hovered state
  updateMenuItemDisplay(currentSelection); 

  if (Debug) {
    Serial.print("Button Pressed. Item ");
    Serial.print(currentSelection);
    Serial.print(" ");
    Serial.println(itemActivated ? "ACTIVATED" : "DEACTIVATED");
  }
}


  // This function is called when an item is *deactivated* (button pressed to confirm)
void handleSelection(int selectedItemIndex) { 

  if (selectedItemIndex == 0){
    if (Debug){
        Serial.println("Brightness menu confirmed");
    }
    // Placeholder for Step 4: Update actual PWM output for brightnessPercent
    // Example: ledcWrite(0, map(brightnessPercent, 0, 100, 0, 255));
    drawNotificationText(NOTIFICATION_TEXT_Y, "Brightness set to " + String(brightnessPercent) + "%");
  }
  else if (selectedItemIndex == 1){
    if (Debug){
        Serial.println("Animation menu confirmed");
    }
    // Placeholder for Step 5: Trigger selected animation logic
    String animName;
    switch (currentAnimation) {
        case NONE:      animName = "None";      break;
        case BLINK:     animName = "Blink";     break;
        case LIGHTNING: animName = "Lightning"; break;
        case STROBE:    animName = "Strobe";    break;
    }
    drawNotificationText(NOTIFICATION_TEXT_Y, "Animation set to " + animName);
  }
  else if (selectedItemIndex == 2){
    if (Debug){
        Serial.println("Speed menu confirmed");
    }
    // Placeholder for Step 6: Update animation speed logic
    drawNotificationText(NOTIFICATION_TEXT_Y, "Speed set to " + String(speedPercent) + "");
  }

}

 



//--------------------------------------------------------------------- MAIN MENU

//------------------------------------------------------------------- DRAW BRIGHTNESS
// Boxes around Items
void drawItemBox(int x, int y, int w, int h, uint16_t borderColor, uint16_t bgColor, bool filled) {
    tft.drawRoundRect(x, y, w, h, 5, borderColor); // Rounded corners with radius 5
    if (filled) {
        tft.fillRoundRect(x + 1, y + 1, w - 2, h - 2, 4, bgColor); // Smaller radius for fill
    } else {
        // If not filled, ensure background is cleared within the border if it was previously filled
        tft.fillRoundRect(x + 1, y + 1, w - 2, h - 2, 4, THEME_CURRENT_COLORS_BG);
    }
}

//Brightness Slider
void drawBrightness(int yPos, int value, bool isActive, bool isHovered) {
    uint16_t borderColor = THEME_CURRENT_COLORS_BORDER_DEFAULT;
    uint16_t bgColor = THEME_CURRENT_COLORS_BG;

    if (isActive) {
        borderColor = THEME_CURRENT_COLORS_BORDER_ACTIVE;
        bgColor = THEME_CURRENT_COLORS_ITEM_BACKGROUND;
    } else if (isHovered) {
        borderColor = THEME_CURRENT_COLORS_BORDER_HOVER;
    }
    
    drawItemBox(ITEM_MARGIN_X, yPos, ITEM_WIDTH, ITEM_HEIGHT, borderColor, bgColor, isActive);

    tft.setTextColor(THEME_CURRENT_COLORS_TEXT);
    tft.setTextFont(2); // Medium font
    tft.setTextSize(1);
    tft.setCursor(ITEM_MARGIN_X + 10, yPos + 10);
    tft.print("BRIGHTNESS");

    // Draw slider background
    int sliderY = yPos + SLIDER_BAR_Y_OFFSET + 10; // Adjust for text height
    tft.fillRect(SLIDER_BAR_X, sliderY, SLIDER_BAR_WIDTH, SLIDER_BAR_HEIGHT, THEME_CURRENT_COLORS_SLIDER_BG);

    // Draw slider fill
    int fillWidth = map(value, 0, 100, 0, SLIDER_BAR_WIDTH);
    tft.fillRect(SLIDER_BAR_X, sliderY, fillWidth, SLIDER_BAR_HEIGHT, THEME_CURRENT_COLORS_SLIDER_FILL);

    // Draw current value
    tft.setCursor(ITEM_MARGIN_X + ITEM_WIDTH - 40, yPos + 10); // Position "75" to the right
    tft.print(value);
}

//------------------------------------------------------------------- DRAW ANIMATION
void drawAnimation(int yPos, AnimationType animType, bool isActive, bool isHovered) {
    uint16_t borderColor = THEME_CURRENT_COLORS_BORDER_DEFAULT;
    uint16_t bgColor = THEME_CURRENT_COLORS_BG;

    if (isActive) {
        borderColor = THEME_CURRENT_COLORS_BORDER_ACTIVE;
        bgColor = THEME_CURRENT_COLORS_ITEM_BACKGROUND;
    } else if (isHovered) {
        borderColor = THEME_CURRENT_COLORS_BORDER_HOVER;
    }

    drawItemBox(ITEM_MARGIN_X, yPos, ITEM_WIDTH, ITEM_HEIGHT, borderColor, bgColor, isActive);

    tft.setTextColor(THEME_CURRENT_COLORS_TEXT);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setCursor(ITEM_MARGIN_X + 10, yPos + 10);
    tft.print("ANIMATION");

    String animName;
    switch (animType) {
        case NONE:      animName = "None";      break;
        case BLINK:     animName = "Blink";     break;
        case LIGHTNING: animName = "Lightning"; break;
        case STROBE:    animName = "Strobe";    break;
    }

    // Draw arrows
    tft.setTextColor(THEME_CURRENT_COLORS_ARROW_COLOR); // Arrows typically white
    tft.setCursor(ITEM_MARGIN_X + 20, yPos + (ITEM_HEIGHT / 2) - (tft.fontHeight() / 2) + 5);
    tft.print("<");
    tft.setCursor(ITEM_MARGIN_X + ITEM_WIDTH - 30, yPos + (ITEM_HEIGHT / 2) - (tft.fontHeight() / 2) + 5);
    tft.print(">");

    // Draw animation name
    tft.setTextColor(THEME_CURRENT_COLORS_TEXT);
    int textWidth = tft.textWidth(animName);
    tft.setCursor(ITEM_MARGIN_X + (ITEM_WIDTH / 2) - (textWidth / 2), yPos + (ITEM_HEIGHT / 2) - (tft.fontHeight() / 2) + 5);
    tft.print(animName);
}

//------------------------------------------------------------------- DRAW SPEED
void drawSpeed(int yPos, int value, bool isActive, bool isHovered, bool isGreyedOut) {
    uint16_t borderColor = THEME_CURRENT_COLORS_BORDER_DEFAULT; // Default to greyed out if unselectable
    uint16_t textColor = THEME_CURRENT_COLORS_TEXT;
    uint16_t bgColor = THEME_CURRENT_COLORS_BG;
    uint16_t sliderBgColor = THEME_CURRENT_COLORS_SLIDER_BG;
    uint16_t sliderFillColor = THEME_CURRENT_COLORS_SLIDER_FILL;

    if (isGreyedOut) {
        borderColor = THEME_CURRENT_COLORS_GREYED_OUT_BORDER;
        textColor = THEME_CURRENT_COLORS_GREYED_OUT_TEXT;
        sliderBgColor = COLOR_DARK_GREYED_OUT_SLIDER_BG;
        sliderFillColor = THEME_CURRENT_COLORS_GREYED_OUT_TEXT;
        // Also ensure background is cleared if it was previously active
        tft.fillRoundRect(ITEM_MARGIN_X + 1, yPos + 1, ITEM_WIDTH - 2, ITEM_HEIGHT - 2, 4, THEME_CURRENT_COLORS_BG);
    } else {
        if (isActive) {
            borderColor = THEME_CURRENT_COLORS_BORDER_ACTIVE;
            bgColor = THEME_CURRENT_COLORS_ITEM_BACKGROUND;
        } else if (isHovered) {
            borderColor = THEME_CURRENT_COLORS_BORDER_HOVER;
        }
    }

    drawItemBox(ITEM_MARGIN_X, yPos, ITEM_WIDTH, ITEM_HEIGHT, borderColor, bgColor, isActive && !isGreyedOut);

    tft.setTextColor(textColor);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setCursor(ITEM_MARGIN_X + 10, yPos + 10);
    tft.print("SPEED");

    // Draw slider background
    int sliderY = yPos + SLIDER_BAR_Y_OFFSET + 10;
    tft.fillRect(SLIDER_BAR_X, sliderY, SLIDER_BAR_WIDTH, SLIDER_BAR_HEIGHT, sliderBgColor);

    if (!isGreyedOut) {
        // Draw slider fill if not greyed out
        int fillWidth = map(value, 0, 100, 0, SLIDER_BAR_WIDTH);
        tft.fillRect(SLIDER_BAR_X, sliderY, fillWidth, SLIDER_BAR_HEIGHT, sliderFillColor);
        tft.setCursor(ITEM_MARGIN_X + ITEM_WIDTH - 40, yPos + 10);
        tft.print(value);
    } else {
        tft.setCursor(ITEM_MARGIN_X + ITEM_WIDTH - 40, yPos + 10);
        tft.print("--"); // Display "--" when greyed out
    }
}

//------------------------------------------------------------------- DRAW NOTIFICATION TEXT
void drawNotificationText(int yPos, const String& text) {
    // Clear previous text area
    tft.fillRect(ITEM_MARGIN_X, yPos - 10, ITEM_WIDTH, tft.fontHeight() + 20, THEME_CURRENT_COLORS_BG); // Clear a generous area
    
    tft.setTextColor(THEME_CURRENT_COLORS_TEXT);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM); // Center datum
    tft.drawString(text, SW / 2, yPos);
    tft.setTextDatum(TL_DATUM); // Reset to top-left
}

//------------------------------------------------------------------- DRAW FOOTER
// This function now draws *to the sprite*, not directly to the TFT.
// The sprite will be pushed to the TFT from the loop().
void drawFooterContent(const String& text) { // Renamed to clearly indicate it draws content to sprite
   footerSprite.fillSprite(THEME_CURRENT_COLORS_BG); 
    
    footerSprite.setTextColor(THEME_CURRENT_COLORS_TEXT);

    // Calculate vertical offset for text centering
    // For Font 2 (16 pixels high) and FOOTER_HEIGHT 25: (25 - 16) / 2 = 4
    int yTextOffset = (FOOTER_HEIGHT - footerSprite.fontHeight()) / 2; 

    // Determine the starting X position for the first text instance relative to the sprite
    // This will be negative as the text scrolls left
    int currentDrawX = 0 - scrollOffset;

    // The total width of one full text segment including the gap for repetition
    int segmentTotalWidth = textPixelWidth + scrollGapPixels;

    // Draw enough repetitions of the text to cover the entire sprite width seamlessly
    // We typically need to draw at least two, possibly three, segments to ensure smooth looping.
    // The loop handles this dynamically.
    int i = 0;
    while (currentDrawX + (i * segmentTotalWidth) < SW + segmentTotalWidth) { // Draw until we're past the right edge + one segment
        footerSprite.setCursor(currentDrawX + (i * segmentTotalWidth), yTextOffset); 
        footerSprite.print(text);
        i++;
    }
}

//--------------------------------------------------------------------- MAIN MENU

void drawFullMenu(){
  tft.fillScreen(THEME_CURRENT_COLORS_BG); // Still clears the screen for a full redraw

  // Redraw all menu items using the new helper function
  updateMenuItemDisplay(0); // Brightness
  updateMenuItemDisplay(1); // Animation
  updateMenuItemDisplay(2); // Speed

  // Draw Notification Text (This still clears its own area, which is fine)
  drawNotificationText(NOTIFICATION_TEXT_Y, "TEMPORARY NOTIFICATION TEXT");

  // Draw Footer (using the sprite, so it's efficient)
  drawFooterContent(footerText); 
  if (!screenSleeping) { // Only push sprite if screen is awake
    footerSprite.pushSprite(0, FOOTER_Y); 
  }
}

// Helper function to draw or update a single menu item
void updateMenuItemDisplay(int itemIndex) {
  // Calculate Y positions for menu items
  int brightnessY = ITEM_MARGIN_TOP;
  int animationY = brightnessY + ITEM_HEIGHT + ITEM_SPACING;
  int speedY = animationY + ITEM_HEIGHT + ITEM_SPACING;

  // Determine the Y position for the specific item based on its index
  int yPos;
  switch (itemIndex) {
    case 0: yPos = brightnessY; break; // Brightness
    case 1: yPos = animationY; break; // Animation
    case 2: yPos = speedY; break;   // Speed
    default:
      if (Debug) Serial.printf("ERROR: Invalid itemIndex %d in updateMenuItemDisplay.\n", itemIndex);
      return; // Should not happen if logic is correct
  }

  // Determine the current state for this specific item
  bool isCurrentActive = (currentSelection == itemIndex && itemActivated);
  bool isCurrentHovered = (currentSelection == itemIndex && !itemActivated);
  bool isCurrentGreyedOut = (itemIndex == 2 && currentAnimation == NONE); // Greyed out logic only applies to Speed

  // Call the appropriate item drawing function with its calculated state
  if (itemIndex == 0) { // Brightness
    drawBrightness(yPos, brightnessPercent, isCurrentActive, isCurrentHovered);
  } else if (itemIndex == 1) { // Animation
    drawAnimation(yPos, currentAnimation, isCurrentActive, isCurrentHovered);
  } else if (itemIndex == 2) { // Speed
    drawSpeed(yPos, speedPercent, isCurrentActive, isCurrentHovered, isCurrentGreyedOut);
  }
  // Note: Notification text and footer are handled separately as they are not "menu items"
}





