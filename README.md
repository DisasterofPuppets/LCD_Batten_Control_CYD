# LCD_Batten_Control_CYD
ESP32 Light Controller for customized LED battens using Cheap Yellow Display

Hardware
- Cheap Yellow Display (ESP32-2432S028R)
- 24V 2835 SMD 240LED/M Color 6000L IP20 
- 6A 220V PSU
- High-Power Dual MOSFET Switch (Two parallel N-Channel logic compatible MOSFETS with lod RDs(on)) I'm using one labelled 'XY-Mos'
- 24V to 5V compatible step-down module
- 3 x temporary switches
- 3 x 10k Resistors
- 3 x 100nf ceramic capacitor
- Power switch with inbuilt light (24v compatible)

Total amount of LED lights per Batten

SMD 240LED/M @ 24V
- 22 LEDS Per Row, 
- 2 Rows per Batten
- 44 LEDS per Batten
-6 Battens
Grand total of 264 LEDs per Completed Light. 
( I Have 2, but the maximum will be 264 LEDs per PSU)

*SCHEMATIC

![Schematic](https://github.com/DisasterofPuppets/LCD_Batten_Control_CYD/blob/main/CYD%20-%20Batten%20Lights%20with%20Switches_bb.png)
