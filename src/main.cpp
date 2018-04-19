/*
 
Copyright  2018 Oliver Brandmueller <ob@sysadm.in>
Copyright  2018 Klaus Wilting <verkehrsrot@arcor.de>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

NOTICE: 
Parts of the source files in this repository are made available under different licenses.
Refer to LICENSE.txt file in repository for more details.

*/

// Basic Config
#include "globals.h"

// Does nothing and avoid any compilation error with I2C
#include <Wire.h> 

// LMIC-Arduino LoRaWAN Stack
#include "loraconf.h"
#include <lmic.h>
#include <hal/hal.h>

// ESP32 lib Functions
#include <esp_event_loop.h>         // needed for Wifi event handler
#include <esp_spi_flash.h>          // needed for reading ESP32 chip attributes
#include <esp32-hal-log.h>          // needed for ESP_LOGx on arduino framework

// Initialize global variables
configData_t cfg;                   // struct holds current device configuration
osjob_t sendjob, initjob;           // LMIC jobs
uint64_t uptimecounter = 0;         // timer global for uptime counter
uint32_t currentMillis = 0;         // timer global for state machine
uint8_t DisplayState, LEDcount = 0; // globals for state machine
uint16_t LEDBlinkduration = 0, LEDInterval = 0, color=COLOR_NONE; // state machine variables
uint16_t macs_total = 0, macs_wifi = 0, macs_ble = 0; // MAC counters globals for display
uint8_t channel = 0;                // wifi channel rotation counter global for display
char display_lora[16], display_lmic[16]; // display buffers
enum states LEDState = LED_OFF;     // LED state global for state machine
bool joinstate = false;             // LoRa network joined? global flag

std::set<uint16_t> macs; // associative container holds total of unique MAC adress hashes (Wifi + BLE)

// this variable will be changed in the ISR, and read in main loop
static volatile bool ButtonTriggered = false;

// local Tag for logging
static const char *TAG = "paxcnt";
// Note: Log level control seems not working during runtime,
// so we need to switch loglevel by compiler build option in platformio.ini

#ifndef VERBOSE
int redirect_log(const char * fmt, va_list args) {
   //do nothing
   return 0;
}
#endif

void set_LED (uint16_t set_color, uint16_t set_blinkduration, uint16_t set_interval, uint8_t set_count) {
    color = set_color;                      // set color for RGB LED
    LEDBlinkduration = set_blinkduration;   // duration on
    LEDInterval = set_interval;             // duration off - on - off
    LEDcount = set_count * 2;               // number of on/off cycles before LED off
    LEDState = set_count ? LED_ON : LED_OFF;           // sets LED to off if 0 blinks
}

void reset_counters() {
    macs.clear();                           // clear all macs container
    macs_total = 0;                         // reset all counters
    macs_wifi = 0;
    macs_ble = 0;
}

/* begin LMIC specific parts ------------------------------------------------------------ */


#ifdef VERBOSE
    void printKeys(void);
#endif // VERBOSE

// LMIC callback functions
void os_getDevKey (u1_t *buf) { 
    memcpy(buf, APPKEY, 16);
}

void os_getArtEui (u1_t *buf) { 
    memcpy(buf, APPEUI, 8);
    RevBytes(buf, 8); // TTN requires it in LSB First order, so we swap bytes
}

void os_getDevEui (u1_t* buf) {
    int i=0, k=0;
    memcpy(buf, DEVEUI, 8); // get fixed DEVEUI from loraconf.h 
    for (i=0; i<8 ; i++) {
        k += buf[i];
    }
    if (k) {
        RevBytes(buf, 8); // use fixed DEVEUI and swap bytes to LSB format
    } else {
        gen_lora_deveui(buf); // generate DEVEUI from device's MAC
    }

    // Get MCP 24AA02E64 hardware DEVEUI (override default settings if found)
    #ifdef MCP_24AA02E64_I2C_ADDRESS
        get_hard_deveui(buf); 
        RevBytes(buf, 8); // swap bytes to LSB format
    #endif
}

// LMIC enhanced Pin mapping
const lmic_pinmap lmic_pins = {
    .mosi = PIN_SPI_MOSI,
    .miso = PIN_SPI_MISO,
    .sck = PIN_SPI_SCK,
    .nss = PIN_SPI_SS,
    .rxtx = LMIC_UNUSED_PIN,
    .rst = RST,
    .dio = {DIO0, DIO1, DIO2}
};

// LoRaWAN Initjob
static void lora_init (osjob_t* j) {
    // reset MAC state
    LMIC_reset();
    // This tells LMIC to make the receive windows bigger, in case your clock is 1% faster or slower.
    LMIC_setClockError(MAX_CLOCK_ERROR * 1 / 100); 
    // start joining
    LMIC_startJoining();
}

// LMIC FreeRTos Task
void lorawan_loop(void * pvParameters) {

    configASSERT( ( ( uint32_t ) pvParameters ) == 1 ); // FreeRTOS check

    while(1) {
        
        os_runloop_once();

        // LED indicators for viusalizing LoRaWAN state
        if ( LMIC.opmode & (OP_JOINING | OP_REJOIN) )  {
            // 5 quick blinks 20ms on each 1/5 second while joining
            set_LED(COLOR_YELLOW, 20, 200, 5);      
        // TX data pending
        } else if (LMIC.opmode & (OP_TXDATA | OP_TXRXPEND)) {
            // 3 small blink 10ms on each 1/2sec (not when joining)
            set_LED(COLOR_BLUE, 10, 500, 3);
        // This should not happen so indicate a problem
        } else  if ( LMIC.opmode & (OP_TXDATA | OP_TXRXPEND | OP_JOINING | OP_REJOIN) == 0 ) {
            // 5 heartbeat long blink 200ms on each 2 seconds
            set_LED(COLOR_RED, 200, 2000, 5);
        } else {
            // led off
            set_LED(COLOR_NONE, 0, 0, 0);
        }
        
        vTaskDelay(10/portTICK_PERIOD_MS);
        yield();
    }    
}

/* end LMIC specific parts --------------------------------------------------------------- */



/* beginn hardware specific parts -------------------------------------------------------- */

#ifdef HAS_DISPLAY
    HAS_DISPLAY u8x8(OLED_RST, OLED_SCL, OLED_SDA);
#endif

#ifdef HAS_ANTENNA_SWITCH
    // defined in antenna.cpp
    void antenna_init();
    void antenna_select(const int8_t _ant);
#endif

#ifndef BLECOUNTER
    bool btstop = btStop();
#endif

#ifdef HAS_BUTTON
    // Button Handling, board dependent -> perhaps to be moved to hal/<$board.h>
    // IRAM_ATTR necessary here, see https://github.com/espressif/arduino-esp32/issues/855
    void IRAM_ATTR isr_button_pressed(void) {
        ButtonTriggered = true; }
#endif

/* end hardware specific parts -------------------------------------------------------- */


/* begin wifi specific parts ---------------------------------------------------------- */

// Sniffer Task
void sniffer_loop(void * pvParameters) {

    configASSERT( ( ( uint32_t ) pvParameters ) == 1 ); // FreeRTOS check

    char buff[16];
    int nloop=0, lorawait=0;
   
  	while (1) {

        nloop++; // actual number of wifi loops, controls cycle when data is sent

        channel = (channel % WIFI_CHANNEL_MAX) + 1;     // rotates variable channel 1..WIFI_CHANNEL_MAX
        wifi_sniffer_set_channel(channel);
        ESP_LOGD(TAG, "Wifi set channel %d", channel);

        // duration of one wifi scan loop reached? then send data and begin new scan cycle
        if ( nloop >= ( (100 / cfg.wifichancycle) * (cfg.wifiscancycle * 2)) +1 ) {

            nloop=0; channel=0;                         // reset wifi scan + channel loop counter           
            do_send(&sendjob);                          // Prepare and execute LoRaWAN data upload
            
            //vTaskDelay(500/portTICK_PERIOD_MS);       // tbd - is this delay really needed here?
            //yield();

            // clear counter if not in cumulative counter mode
            if (cfg.countermode != 1) {
                reset_counters();                       // clear macs container and reset all counters
                reset_salt();                           // get new salt for salting hashes
            }      

            // check if payload is sent
            lorawait = 0;
            while(LMIC.opmode & OP_TXRXPEND) {
                if(!lorawait) 
                    sprintf(display_lora, "LoRa wait");
                lorawait++;
                // in case sending really fails: reset and rejoin network
                if( (lorawait % MAXLORARETRY ) == 0) {
                    ESP_LOGI(TAG, "Payload not sent, trying reset and rejoin");
                    esp_restart();
                };
                vTaskDelay(1000/portTICK_PERIOD_MS);
                yield();
            }
            sprintf(display_lora, ""); // clear LoRa wait message fromd display
                    
        } // end of send data cycle
        
        vTaskDelay(cfg.wifichancycle*10 / portTICK_PERIOD_MS);
        yield();

    } // end of infinite wifi channel rotation loop
}

/* end wifi specific parts ------------------------------------------------------------ */

// uptime counter 64bit to prevent millis() rollover after 49 days
uint64_t uptime() {
    static uint32_t low32, high32;
    uint32_t new_low32 = millis();
    if (new_low32 < low32) high32++;
    low32 = new_low32;
    return (uint64_t) high32 << 32 | low32;
}

#ifdef HAS_DISPLAY

    // Print a key on display
    void DisplayKey(const uint8_t * key, uint8_t len, bool lsb) {
        uint8_t start=lsb?len:0;
        uint8_t end = lsb?0:len;
        const uint8_t * p ;
        for (uint8_t i=0; i<len ; i++) {
            p = lsb ? key+len-i-1 : key+i;
            u8x8.printf("%02X", *p);
        }
        u8x8.printf("\n");
    }

    void init_display(const char *Productname, const char *Version) {
        uint8_t buf[32];
        u8x8.begin();
        u8x8.setFont(u8x8_font_chroma48medium8_r);
        u8x8.clear();
        u8x8.setFlipMode(0);
        u8x8.setInverseFont(1);
        u8x8.draw2x2String(0, 0, Productname);
        u8x8.setInverseFont(0);
        u8x8.draw2x2String(2, 2, Productname);
        delay(1500);
        u8x8.clear();
        u8x8.setFlipMode(1);
        u8x8.setInverseFont(1);
        u8x8.draw2x2String(0, 0, Productname);
        u8x8.setInverseFont(0);
        u8x8.draw2x2String(2, 2, Productname);
        delay(1500);

        u8x8.setFlipMode(0);
        u8x8.clear();

        #ifdef DISPLAY_FLIP
            u8x8.setFlipMode(1);
        #endif

        // Display chip information
        #ifdef VERBOSE
            esp_chip_info_t chip_info;
            esp_chip_info(&chip_info);
            u8x8.printf("ESP32 %d cores\nWiFi%s%s\n",
                chip_info.cores,
                (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
                (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");
            u8x8.printf("ESP Rev.%d\n", chip_info.revision);
            u8x8.printf("%dMB %s Flash\n", spi_flash_get_chip_size() / (1024 * 1024),
                (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "int." : "ext.");
        #endif // VERBOSE

        u8x8.print(Productname);
        u8x8.print(" v");
        u8x8.println(PROGVERSION);
        u8x8.println("DEVEUI:");
        os_getDevEui((u1_t*) buf);
        DisplayKey(buf, 8, true);
        delay(5000);
        u8x8.clear();
    }

    void refreshDisplay() {
        // update counter display (lines 0-4)
        char buff[16];
        snprintf(buff, sizeof(buff), "PAX:%-4d", (int) macs.size()); // convert 16-bit MAC counter to decimal counter value
        u8x8.draw2x2String(0, 0, buff);          // display number on unique macs total Wifi + BLE
        u8x8.setCursor(0,4);
        u8x8.printf("WIFI: %-4d", macs_wifi);

        #ifdef BLECOUNTER
            u8x8.setCursor(0,3);
            if (cfg.blescan)
                u8x8.printf("BLTH: %-4d", macs_ble);
            else
                u8x8.printf("%-16s", "BLTH: off");
        #endif

        // update wifi channel display (line 4)
        u8x8.setCursor(11,4);
        u8x8.printf("ch:%02i", channel);

        // update RSSI limiter status display (line 5)
        u8x8.setCursor(0,5);
        u8x8.printf(!cfg.rssilimit ? "RLIM: off" : "RLIM: %-4d", cfg.rssilimit);

        // update LoRa status display (line 6)
        u8x8.setCursor(0,6);
        u8x8.printf("%-16s", display_lora);

        // update LMiC event display (line 7)
        u8x8.setCursor(0,7);
        u8x8.printf("%-16s", display_lmic);
    }    

    void updateDisplay() {
        // timed display refresh according to refresh cycle setting
        uint32_t previousDisplaymillis;

        if (currentMillis - previousDisplaymillis >= DISPLAYREFRESH_MS) {
            refreshDisplay();
            previousDisplaymillis += DISPLAYREFRESH_MS;
        }
        // set display on/off according to current device configuration
        if (DisplayState != cfg.screenon) {
            DisplayState = cfg.screenon;
            u8x8.setPowerSave(!cfg.screenon);
        }
    } // updateDisplay()


#endif // HAS_DISPLAY

#ifdef HAS_BUTTON
    void readButton() {
        if (ButtonTriggered) {
            ButtonTriggered = false;
            ESP_LOGI(TAG, "Button pressed, resetting device to factory defaults");
            eraseConfig();
            esp_restart();
        }
    }
#endif

#ifdef HAS_LED
    void switchLED() {
        enum states previousLEDState;
        // led need to change state? avoid digitalWrite() for nothing
        if (LEDState != previousLEDState) {
            #ifdef LED_ACTIVE_LOW
                digitalWrite(HAS_LED, !LEDState);
            #else
                digitalWrite(HAS_LED, LEDState);
            #endif

            #ifdef HAS_RGB_LED
                rgb_set_color(LEDState ? color : COLOR_NONE);
            #endif

            previousLEDState = LEDState;
            LEDcount--;        // decrement blink counter
        }
    }; // switchLED()

    void switchLEDstate() {
        if (!LEDcount)         // no more blinks? -> switch off LED
            LEDState = LED_OFF;
        else if (LEDInterval)   // blinks left? -> toggle LED and decrement blinks
            LEDState = ((currentMillis % LEDInterval) < LEDBlinkduration) ? LED_ON : LED_OFF;
    } // switchLEDstate()
#endif


/* begin Aruino SETUP ------------------------------------------------------------ */

void setup() {

    // disable brownout detection
#ifdef DISABLE_BROWNOUT
    // register with brownout is at address DR_REG_RTCCNTL_BASE + 0xd4
  (*((volatile uint32_t *)ETS_UNCACHED_ADDR((DR_REG_RTCCNTL_BASE+0xd4)))) = 0;
#endif

    // setup debug output or silence device
#ifdef VERBOSE
    Serial.begin(115200);
    esp_log_level_set("*", ESP_LOG_VERBOSE);
#else
    // mute logs completely by redirecting them to silence function
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_log_set_vprintf(redirect_log);
#endif
    
    ESP_LOGI(TAG, "Starting %s %s", PROGNAME, PROGVERSION);
    
    // initialize system event handler for wifi task, needed for wifi_sniffer_init()
    esp_event_loop_init(NULL, NULL);

    // print chip information on startup if in verbose mode
#ifdef VERBOSE
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "This is ESP32 chip with %d CPU cores, WiFi%s%s, silicon revision %d, %dMB %s Flash",
            chip_info.cores,
            (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
            (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "", 
            chip_info.revision, spi_flash_get_chip_size() / (1024 * 1024),
            (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    ESP_LOGI(TAG, "ESP32 SDK: %s", ESP.getSdkVersion());
#endif

    // read settings from NVRAM
    loadConfig(); // includes initialize if necessary

    // initialize led if needed
#ifdef HAS_LED
    pinMode(HAS_LED, OUTPUT);
    set_LED(COLOR_NONE, 0, 0, 0); // LED off
#endif

    // initialize button handling if needed
#ifdef HAS_BUTTON
    #ifdef BUTTON_PULLUP
        // install button interrupt (pullup mode)
        pinMode(HAS_BUTTON, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(HAS_BUTTON), isr_button_pressed, RISING); 
    #else
        // install button interrupt (pulldown mode)
        pinMode(HAS_BUTTON, INPUT_PULLDOWN);
        attachInterrupt(digitalPinToInterrupt(HAS_BUTTON), isr_button_pressed, FALLING); 
    #endif
#endif

    // initialize wifi antenna if needed
#ifdef HAS_ANTENNA_SWITCH
    antenna_init();
#endif

#ifdef HAS_DISPLAY
// initialize display  
    init_display(PROGNAME, PROGVERSION);  
    DisplayState = cfg.screenon;   
    u8x8.setPowerSave(!cfg.screenon); // set display off if disabled
    u8x8.draw2x2String(0, 0, "PAX:0");
    u8x8.setCursor(0,4);
    u8x8.printf("WIFI: 0");
    #ifdef BLECOUNTER
        u8x8.setCursor(0,3);
        u8x8.printf("BLTH: 0");
    #endif
    u8x8.setCursor(0,5);
    u8x8.printf(!cfg.rssilimit ? "RLIM: off" : "RLIM: %d", cfg.rssilimit);
    sprintf(display_lora, "Join wait");
#endif

// output LoRaWAN keys to console
#ifdef VERBOSE
    printKeys();
#endif

os_init(); // setup LMIC
os_setCallback(&initjob, lora_init); // setup initial job & join network 
wifi_sniffer_init(); // setup wifi in monitor mode and start MAC counting

// initialize salt value using esp_random() called by random() in arduino-esp32 core
// note: do this *after* wifi has started, since gets it's seed from RF noise
reset_salt(); // get new 16bit for salting hashes
 
// run wifi task on core 0 and lora task on core 1 and bt task on core 0
ESP_LOGI(TAG, "Starting Lora task on core 1");
xTaskCreatePinnedToCore(lorawan_loop, "loratask", 2048, ( void * ) 1,  ( 5 | portPRIVILEGE_BIT ), NULL, 1); 

ESP_LOGI(TAG, "Starting Wifi task on core 0");
xTaskCreatePinnedToCore(sniffer_loop, "wifisniffer", 16384, ( void * ) 1, 1, NULL, 0);

#ifdef BLECOUNTER
    if (cfg.blescan) { // start BLE task only if BLE function is enabled in NVRAM configuration
        ESP_LOGI(TAG, "Starting Bluetooth task on core 0");
        xTaskCreatePinnedToCore(bt_loop, "btscan", 16384, ( void * ) 1, 1, NULL, 0);
    }
#endif
    
// Finally: kickoff first sendjob and join, then send initial payload "0000"
uint8_t mydata[] = "0000";
do_send(&sendjob);
}

/* end Aruino SETUP ------------------------------------------------------------ */


/* begin Aruino LOOP ------------------------------------------------------------ */

// Arduino main moop, runs on core 1
// https://techtutorialsx.com/2017/05/09/esp32-get-task-execution-core/
void loop() {

    uptimecounter = uptime() / 1000; // counts uptime in seconds (64bit)
    currentMillis = millis(); // timebase for state machine in milliseconds (32bit)

    // simple state machine for controlling display, LED, button, etc.
    
    #ifdef HAS_BUTTON
        readButton();
    #endif

    #ifdef HAS_DISPLAY
        updateDisplay();
    #endif

    #ifdef HAS_LED
        switchLEDstate();
        switchLED();
    #endif

    //sendPayload();  

}

/* end Aruino LOOP ------------------------------------------------------------ */
