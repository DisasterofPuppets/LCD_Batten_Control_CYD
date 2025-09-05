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

  bool buttonIsPressed = digitalRead(ENCODER_SW) == HIGH;
  if (buttonIsPressed && !buttonPressed && (millis() - lastDebounceTime > debounceDelay)) {
    lastDebounceTime = millis();
    buttonPressed = true;
    handleButtonPress();
  }
  if (!buttonIsPressed) {
    buttonPressed = false;
  }
}

// ---------------------------------------------------------------------------  Functions  ---------------------------------------------------------------------------

// ---- Drawing Functions ----
void drawMainMenu() {
  // Only clear screen on first draw
  if (lastSelection == -1) {
    tft.fillScreen(TFT_BLACK);
    
    // Centered title
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE);
    String title = "-= Main Menu =-";
    int titleWidth = title.length() * 12;
    int titleX = (SCREEN_WIDTH - titleWidth) / 2;
    tft.setCursor(titleX, 20);
    tft.print(title);
  }

  // Menu items with visual selection
  int rowHeight = 30;
  int startY = 60;
  int x = 20;
  
  for (int i = 0; i < 6; i++) {
    // Only redraw if selection changed
    if (i == currentSelection || i == lastSelection || lastSelection == -1) {
      int y = startY + i * (rowHeight + 5);
      
      // Clear line area
      tft.fillRect(0, y, SCREEN_WIDTH, rowHeight, TFT_BLACK);
      
      // Draw selection indicator and button background
      if (i == currentSelection) {
        tft.fillRoundRect(5, y, SCREEN_WIDTH - 10, rowHeight, 5, TFT_BLUE);
        tft.setTextColor(TFT_WHITE);
        tft.setCursor(x, y + 8);
        tft.print("▶ ");
      } else {
        tft.fillRoundRect(5, y, SCREEN_WIDTH - 10, rowHeight, 5, TFT_DARKGREY);
        tft.drawRoundRect(5, y, SCREEN_WIDTH - 10, rowHeight, 5, TFT_WHITE);
        tft.setTextColor(TFT_WHITE);
        tft.setCursor(x, y + 8);
        tft.print("  ");
      }
      
      tft.print(String(i + 2) + ": " + mainMenuItems[i]);
    }
  }
  
  lastSelection = currentSelection;
}

void drawMenuItems(String items[], int itemCount, int startY) {
  int rowHeight = 30;
  int x = 20;
  
  for (int i = 0; i < itemCount; i++) {
    // Only redraw if selection changed
    if (i == currentSelection || i == lastSelection || lastSelection == -1) {
      int y = startY + i * (rowHeight + 5);
      
      // Clear line area
      tft.fillRect(0, y, SCREEN_WIDTH, rowHeight, TFT_BLACK);
      
      // Draw selection indicator and button background
      if (i == currentSelection) {
        uint16_t bgColor = TFT_BLUE;
        if (inSubmenu && i == 0 && itemActivated) {
          bgColor = TFT_GREEN; // Show activation
        }
        tft.fillRoundRect(5, y, SCREEN_WIDTH - 10, rowHeight, 5, bgColor);
        tft.setTextColor(TFT_WHITE);
        tft.setCursor(x, y + 8);
        tft.print("▶ ");
      } else {
        tft.fillRoundRect(5, y, SCREEN_WIDTH - 10, rowHeight, 5, TFT_DARKGREY);
        tft.drawRoundRect(5, y, SCREEN_WIDTH - 10, rowHeight, 5, TFT_WHITE);
        tft.setTextColor(TFT_WHITE);
        tft.setCursor(x, y + 8);
        tft.print("  ");
      }
      
      tft.print(items[i]);
    }
  }
  
  lastSelection = currentSelection;
}

void drawBrightnessMenu() {
  // Only clear screen on first draw
  if (lastSelection == -1) {
    tft.fillScreen(TFT_BLACK);
  }
  
  // Clear home arrow area if switching away from it
  if (lastSelection == -1 && currentSelection != -1) {
    tft.fillRect(8, 18, 16, 16, TFT_BLACK);
  }
  
  // Home arrow icon (top left) - always draw
  tft.setTextSize(1);  // Smaller size for icon
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(10, 22);
  tft.print("<-");
  
  // Highlight home arrow if selected
  if (currentSelection == -1) {
    tft.drawRoundRect(8, 20, 20, 12, 2, TFT_RED);
    tft.drawRoundRect(7, 19, 22, 14, 2, TFT_RED);
  }
  
  // Title - smaller font size
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(40, 22);
  tft.print("Brightness Control");
  
  // Always show brightness slider (item 0)
  drawBrightnessSlider();
  
  // Update button (item 1) - centered, smaller, between slider and display
  drawUpdateButton();
  
  // Always show current brightness display
  drawBrightnessDisplay();
}

void drawBrightnessSlider() {
  // Slider positioning
  int sliderX = 20;
  int sliderY = 80;
  int sliderW = SCREEN_WIDTH - 40;
  int sliderH = 40;
  
  // Clear slider area
  tft.fillRect(sliderX - 5, sliderY - 5, sliderW + 10, sliderH + 25, TFT_BLACK);
  
  // Slider background and border - highlight if selected
  uint16_t borderColor = (currentSelection == 0) ? TFT_RED : TFT_WHITE;
  uint16_t bgColor = (currentSelection == 0 && itemActivated) ? TFT_NAVY : TFT_DARKGREY;
  
  tft.fillRoundRect(sliderX, sliderY, sliderW, sliderH, 8, bgColor);
  tft.drawRoundRect(sliderX, sliderY, sliderW, sliderH, 8, borderColor);
  
  // Solid red outline if selected 
  if (currentSelection == 0) {
    tft.drawRoundRect(sliderX - 1, sliderY - 1, sliderW + 2, sliderH + 2, 8, TFT_RED);
  }
  
  // Horizontal slider line
  int lineY = sliderY + sliderH/2;
  int lineStartX = sliderX + 25;
  int lineEndX = sliderX + sliderW - 25;
  tft.drawLine(lineStartX, lineY, lineEndX, lineY, TFT_WHITE);
  
  // "0" and "100" labels
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(sliderX + 8, lineY - 4);
  tft.print("0");
  tft.setCursor(lineEndX + 8, lineY - 4);
  tft.print("100");
  
  // Slider marker
  int markerX = map(tempBrightness, 0, 100, lineStartX, lineEndX);
  int markerW = 6;
  int markerH = 20;
  tft.fillRect(markerX - markerW/2, lineY - markerH/2, markerW, markerH, TFT_YELLOW);
  
  // Instructions below slider (only when activated)
  if (currentSelection == 0 && itemActivated) {
    tft.setTextSize(1);
    tft.setTextColor(TFT_RED);
    tft.setCursor(20, sliderY + sliderH + 8);
    tft.print("Rotate: Adjust | Press: Deactivate");
  }
}

void drawUpdateButton() {
  // Update button positioning - centered, between slider and display
  int buttonY = 180;
  int buttonW = 100;
  int buttonH = 25;
  int buttonX = (SCREEN_WIDTH - buttonW) / 2;
  
  // Clear button area
  tft.fillRect(buttonX - 5, buttonY - 5, buttonW + 10, buttonH + 10, TFT_BLACK);
  
  // Button background and border - highlight if selected
  uint16_t buttonColor = (currentSelection == 1) ? TFT_GREEN : TFT_DARKGREY;
  uint16_t borderColor = (currentSelection == 1) ? TFT_RED : TFT_WHITE;
  
  tft.fillRoundRect(buttonX, buttonY, buttonW, buttonH, 5, buttonColor);
  tft.drawRoundRect(buttonX, buttonY, buttonW, buttonH, 5, borderColor);
  
  // Solid red outline if selected
  if (currentSelection == 1) {
    tft.drawRoundRect(buttonX - 1, buttonY - 1, buttonW + 2, buttonH + 2, 5, TFT_RED);
  }
  
  // Button text
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(buttonX + 30, buttonY + 8);
  tft.print("Update");
}

void drawBrightnessDisplay() {
  // Clear display area - positioned lower
  int displayY = 220;
  int displayH = 50;
  int displayW = 120;
  int displayX = (SCREEN_WIDTH - displayW) / 2;
  
  tft.fillRect(displayX - 5, displayY - 5, displayW + 10, displayH + 10, TFT_BLACK);
  
  // White rectangular border
  tft.drawRect(displayX - 5, displayY - 5, displayW + 10, displayH + 10, TFT_WHITE);
  tft.drawRect(displayX - 4, displayY - 4, displayW + 8, displayH + 8, TFT_WHITE);
  
  // Large LCD-style number - show current applied brightness
  tft.setTextSize(3);
  tft.setTextColor(TFT_CYAN);
  String brightnessText = String(brightnessPercent) + "%";
  
  // Center the text
  int textW = brightnessText.length() * 18;
  int textX = displayX + (displayW - textW) / 2;
  int textY = displayY + 15;
  
  tft.setCursor(textX, textY);
  tft.print(brightnessText);
  
  // Show temp brightness if different and slider is active
  if (tempBrightness != brightnessPercent && itemActivated && currentSelection == 0) {
    tft.setTextSize(1);
    tft.setTextColor(TFT_YELLOW);
    String tempText = "Preview: " + String(tempBrightness) + "%";
    int tempTextX = displayX + (displayW - tempText.length() * 6) / 2;
    tft.setCursor(tempTextX, displayY - 15);
    tft.print(tempText);
  }
}

// PWM Control Functions
void setBrightness(int percent) {
  percent = constrain(percent, 0, 100);
  int pwmValue = map(percent, 0, 100, 0, 255);
  ledcWrite(LED_PWM_PIN, pwmValue);
  
  Serial.print("Brightness set to: ");
  Serial.print(percent);
  Serial.print("% (PWM: ");
  Serial.print(pwmValue);
  Serial.println(")");
}

// ---- Input Handling ----
void handleEncoder() {
  static uint8_t lastState = 0;
  static int encoderCounter = 0; // Add counter for sensitivity reduction
  static const uint8_t validStates[] = {0b00, 0b01, 0b11, 0b10};

  // Read current CLK and DT
  uint8_t clk = digitalRead(ENCODER_CLK);
  uint8_t dt  = digitalRead(ENCODER_DT);
  uint8_t state = (clk << 1) | dt;

  // Ignore invalid states
  bool valid = false;
  for (int i = 0; i < 4; i++) {
    if (state == validStates[i]) {
      valid = true;
      break;
    }
  }
  if (!valid || state == lastState) return;

  // Determine direction
  int direction = 0;
  if ((lastState == 0b00 && state == 0b01) ||
      (lastState == 0b01 && state == 0b11) ||
      (lastState == 0b11 && state == 0b10) ||
      (lastState == 0b10 && state == 0b00)) {
    direction = -1; // Now counter-clockwise (reversed)
  } else {
    direction = 1; // Now clockwise (reversed)
  }

  // Handle encoder based on current state
  if (inSubmenu && itemActivated && currentSelection == 0) {
    // Brightness slider is activated - adjust brightness (no sensitivity reduction for slider)
    tempBrightness += direction;
    tempBrightness = constrain(tempBrightness, 0, 100);
    
    Serial.print("Brightness adjusted to: ");
    Serial.println(tempBrightness);
    
    // Only redraw slider and display areas
    drawBrightnessSlider();
    drawBrightnessDisplay();
    
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
        maxItems = 2; // Brightness menu: Slider, Update button
        
        // Special case: allow selection of home arrow (-1)
        if (currentSelection < -1) currentSelection = maxItems - 1;
        if (currentSelection >= maxItems) currentSelection = -1; // Go to home arrow
      } else {
        maxItems = 6; // Main menu
        if (currentSelection >= maxItems) currentSelection = 0;
        if (currentSelection < 0) currentSelection = maxItems - 1;
      }

      Serial.print("Menu navigation → Selection: ");
      Serial.println(currentSelection);

      // Redraw appropriate menu (optimized)
      if (inSubmenu) {
        drawBrightnessMenu();
      } else {
        drawMainMenu();
      }
    }
  }

  lastState = state;
}

void handleButtonPress() {
  Serial.println("Encoder button pressed.");
  
  if (inSubmenu) {
    // Check if home arrow was clicked (top-left area)
    // For now, we'll use a simple method: if on slider and rotate counter-clockwise past 0, select home
    
    if (currentSelection == 0) { // Slider
      if (itemActivated) {
        // Deactivate slider
        itemActivated = false;
        Serial.println("Brightness slider deactivated");
        drawBrightnessMenu();
      } else {
        // Activate slider
        itemActivated = true;
        tempBrightness = brightnessPercent;
        Serial.println("Brightness slider activated");
        drawBrightnessMenu();
      }
    } else if (currentSelection == 1) { // Update button
      // Apply the temp brightness value
      if (tempBrightness != brightnessPercent) {
        brightnessPercent = tempBrightness;
        setBrightness(brightnessPercent);
        
        // Show confirmation
        tft.fillRect(0, 150, SCREEN_WIDTH, 20, TFT_BLACK);
        tft.setTextSize(1);
        tft.setTextColor(TFT_RED);
        tft.setCursor(20, 155);
        tft.print("Brightness Updated to " + String(brightnessPercent) + "%");
        
        delay(1000);
        drawBrightnessMenu();
      }
    } else if (currentSelection == -1) { // Home arrow (special case)
      Serial.println("Going to main menu via Home arrow.");
      goToMainMenu();
      drawMainMenu();
    }
  } else {
    // Main menu selection - all options go to brightness submenu
    Serial.print("Entering brightness submenu from Option ");
    Serial.println(currentSelection + 1);
    
    pushMenu(currentSelection);
    currentSelection = 0;
    itemActivated = false;
    tempBrightness = brightnessPercent;
    drawBrightnessMenu();
  }
}

void pushMenu(int menuID) {
  menuStack[stackIndex++] = menuID;
  inSubmenu = true;
  lastSelection = -1;
  itemActivated = false;
}

void popMenu() {
  if (stackIndex > 0) stackIndex--;
  inSubmenu = false;
  currentSelection = 0;
  lastSelection = -1;
  itemActivated = false;
  tempBrightness = brightnessPercent;
}

void goToMainMenu() {
  // Reset everything to main menu
  stackIndex = 0;
  inSubmenu = false;
  currentSelection = 0;
  lastSelection = -1;
  itemActivated = false;
  tempBrightness = brightnessPercent;
}
