#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"

// USB-MIDI includes
#include "pico/unique_id.h"
#include "tusb.h"

// I2C defines
// This example will use I2C0 on GPIO8 (SDA) and GPIO9 (SCL) running at 400KHz.
// Pins can be changed, see the GPIO function select table in the datasheet for information on GPIO assignments
#define I2C_PORT i2c0
#define I2C_SDA 8
#define I2C_SCL 9

#define LED7SEGADDR 0x20
#define UPPERPANEL 0x21
#define LOWERPANEL 0x22

// State definitions
#define STATE_IC_UFLET_1 0
#define STATE_IC_UFLET_2 1
#define STATE_FIXED 2
#define STATE_SETTING 9

// Button 6 for long press detection (bit 5)
#define BUTTON6_BIT 5
#define LONG_PRESS_TIME 1000  // 1 second for long press

// Long press state for all buttons
struct ButtonLongPressState {
    uint32_t press_start_time[16];  // Press start time for each bit (0-15)
    bool long_press_detected[16];   // Long press detection flag for each bit
};

ButtonLongPressState button_long_press_state = {0};

// Current code for the system (0-11, displayed using segCodeDeg)
int current_code = 0;  // Initial value is 0 (corresponds to 'C' in segCodeDeg)

// Current key for the system (0-11, same numeric range as current_code)
int current_key = 0;   // Initial value is 0 (corresponds to 'C' in segCodeDeg)

// Fixed pattern for State 2 (0-3, representing patterns 1-4)
int fixed_pattern = 0;  // Initial value is 0 (pattern 1)

// Current system state (for MIDI callback access)
int current_state = STATE_IC_UFLET_1;

// Button event structure
struct ButtonEvent {
    bool has_event;          // イベントがあったか
    bool is_long_press;      // ロングプッシュイベントか
    bool is_release;         // リリースイベントか
    int button_bit;          // イベントが発生したボタンのビット番号（複数の場合は最初の1つ）
    uint16_t long_press_bits; // ロングプッシュしたボタンのビットマスク
    uint16_t release_bits;    // リリースしたボタンのビットマスク（ロングプッシュ後除く）
};

const uint8_t segCode[10] = {
  // gfedcba order, 0=on, 1=off (for common anode)
  0b11000000, // 0
  0b11111001, // 1
  0b10100100, // 2
  0b10110000, // 3
  0b10011001, // 4
  0b10010010, // 5
  0b10000010, // 6
  0b11111000, // 7
  0b10000000, // 8
  0b10010000  // 9
};

const uint8_t segCodeDeg[] = {
  // gfedcba order, 0=on, 1=off (for common anode)
  0b11000110, // C
  0b00100001, // Db
  0b10100001, // D
  0b00000110, // Eb
  0b10000110, // E
  0b10001110, // F
  0b00010000, // Gb
  0b10010000, // G
  0b00001000, // Ab
  0b10001000, // A
  0b00000011, // Bb
  0b10000011  // B
};

// State display patterns (0:1, 1:2, 2:F, 9:9)
const uint8_t stateDisplay[] = {
  0b11111001, // 1 for state 0 (IC-UFlet-1)
  0b10100100, // 2 for state 1 (IC-UFlet-2) 
  0b10001110, // F for state 2 (Fixed)
  0b10010000  // 9 for state 9 (Setting) - index 3
};

// Function declarations
void initialize_code_and_key(int fixed_pattern, int* current_code_ptr, int* current_key_ptr);

// I2C scan function
void i2c_scan() {
    printf("\nStarting I2C device scan...\n");
    printf("   0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    
    for (int addr = 0; addr < (1 << 7); ++addr) {
        if (addr % 16 == 0) {
            printf("%02x ", addr);
        }
        
        // Try dummy read to I2C address
        uint8_t rxdata;
        int ret = i2c_read_blocking(I2C_PORT, addr, &rxdata, 1, false);
        
        if (ret >= 0) {
            printf("%02X ", addr);
        } else {
            printf("-- ");
        }
        
        if (addr % 16 == 15) {
            printf("\n");
        }
    }
    printf("Scan completed\n\n");
}

// State transition processing function
int process_state_transition(int current_state, const ButtonEvent& event, int* setting_exit_target, int* current_key_ptr, int* current_code_ptr, int* fixed_pattern_ptr) {
    // Only process if there's a valid event
    if (!event.has_event) {
        return current_state;  // No state change
    }
    
    // Convert bit position to button number (1-based)
    int button_number = 0;
    if (event.button_bit >= 0 && event.button_bit <= 5) {
        button_number = event.button_bit + 1;  // Buttons 1-6 (bits 0-5)
    } else if (event.button_bit >= 8 && event.button_bit <= 12) {
        button_number = event.button_bit - 1;  // Buttons 7-11 (bits 8-12)
    }
    
    printf("State transition check - Current state: %d, Button: %d, Long press: %s, Release: %s\n", 
           current_state, button_number, 
           event.is_long_press ? "Yes" : "No", 
           event.is_release ? "Yes" : "No");
    
    // State machine logic
    switch (current_state) {
        case STATE_IC_UFLET_1:  // IC-UFlet-1 (0)
            if (event.is_long_press && button_number == 6) {
                printf("State transition: %d -> Setting (9)\n", current_state);
                return STATE_SETTING;
            }
            if (event.is_release && button_number == 6) {
                // 短押し：カレントキーを+1（0-11の範囲でループ）
                *current_key_ptr = (*current_key_ptr + 1) % 12;
                printf("Current key changed to: %d\n", *current_key_ptr);
                // 状態は変わらない
                return current_state;
            }
            if (event.is_release && button_number != 6) {
                // ボタン1-5,7-11の短押し：カレントコードを変更
                int code_change = 0;
                switch (button_number) {
                    case 1: code_change = -(*current_code_ptr); break;  // 0に戻す
                    case 2: code_change = 2; break;    // +2
                    case 3: code_change = 4; break;    // +4
                    case 4: code_change = -4; break;   // -4
                    case 5: code_change = -2; break;   // -2
                    case 7: code_change = 1; break;    // +1
                    case 8: code_change = 3; break;    // +3
                    case 9: code_change = 6; break;    // +6
                    case 10: code_change = -3; break;  // -3
                    case 11: code_change = -1; break;  // -1
                }
                
                if (code_change != 0 || button_number == 1) {  // button 1 always processes (reset to 0)
                    int old_code = *current_code_ptr;
                    *current_code_ptr = (*current_code_ptr + code_change) % 12;
                    if (*current_code_ptr < 0) *current_code_ptr += 12;  // Handle negative values
                    printf("Button %d pressed: Current code %d -> %d (change: %d)\n", 
                           button_number, old_code, *current_code_ptr, code_change);
                }
                // 状態は変わらない
                return current_state;
            }
            break;
            
        case STATE_IC_UFLET_2:  // IC-UFlet-2 (1)
            if (event.is_long_press && button_number == 6) {
                printf("State transition: %d -> Setting (9)\n", current_state);
                return STATE_SETTING;
            }
            if (event.is_release && button_number == 6) {
                // 短押し：カレントキーを+1（0-11の範囲でループ）
                *current_key_ptr = (*current_key_ptr + 1) % 12;
                printf("Current key changed to: %d\n", *current_key_ptr);
                // 状態は変わらない
                return current_state;
            }
            if (event.is_release && button_number != 6) {
                // ボタン1-5,7-11の短押し：カレントコードを固定値に設定
                int new_code = -1;
                switch (button_number) {
                    case 1: new_code = 0; break;   // 固定値0
                    case 2: new_code = 2; break;   // 固定値2
                    case 3: new_code = 4; break;   // 固定値4
                    case 4: new_code = 8; break;   // 固定値8
                    case 5: new_code = 10; break;  // 固定値10
                    case 7: new_code = 1; break;   // 固定値1
                    case 8: new_code = 3; break;   // 固定値3
                    case 9: new_code = 6; break;   // 固定値6
                    case 10: new_code = 9; break;  // 固定値9
                    case 11: new_code = 11; break; // 固定値11
                }
                
                if (new_code >= 0) {
                    int old_code = *current_code_ptr;
                    *current_code_ptr = new_code;
                    printf("Button %d pressed: Current code %d -> %d (fixed value)\n", 
                           button_number, old_code, *current_code_ptr);
                }
                // 状態は変わらない
                return current_state;
            }
            break;
            
        case STATE_FIXED:       // Fixed (2)
            if (event.is_long_press && button_number == 6) {
                printf("State transition: %d -> Setting (9)\n", current_state);
                return STATE_SETTING;
            }
            if (event.is_release && button_number == 6) {
                // 短押し：パターンを切り替え（0-3の範囲でループ）
                *fixed_pattern_ptr = (*fixed_pattern_ptr + 1) % 4;
                printf("Fixed pattern changed to: %d (pattern %d)\n", *fixed_pattern_ptr, *fixed_pattern_ptr + 1);
                
                // パターン変更時にカレントキーとカレントコードを初期化
                initialize_code_and_key(*fixed_pattern_ptr, current_code_ptr, current_key_ptr);
                
                // 状態は変わらない
                return current_state;
            }
            if (event.is_release && button_number != 6) {
                // ボタン1-5,7-11の短押し：パターンに応じてカレントコードを設定
                int new_code = -1;
                
                // パターン別のコード設定テーブル
                const int pattern_codes[4][11] = {
                    // パターン1 (内部0): ボタン1,2,3,4,5,7,8,9,10,11
                    {0, 2, 4, 8, 10, 1, 3, 6, 9, 11},
                    // パターン2 (内部1): ボタン1,2,3,4,5,7,8,9,10,11  
                    {0, 2, 6, 8, 10, 1, 5, 7, 9, 11},
                    // パターン3 (内部2): ボタン1,2,3,4,5,7,8,9,10,11
                    {2, 4, 6, 8, 10, 3, 5, 7, 9, 11},
                    // パターン4 (内部3): ボタン1,2,3,4,5,7,8,9,10,11
                    {0, 2, 4, 6, 8, 1, 3, 5, 7, 9}
                };
                
                // ボタン番号をテーブルインデックスに変換
                int table_index = -1;
                if (button_number >= 1 && button_number <= 5) {
                    table_index = button_number - 1;  // ボタン1-5 -> インデックス0-4
                } else if (button_number >= 7 && button_number <= 11) {
                    table_index = button_number - 2;  // ボタン7-11 -> インデックス5-9
                }
                
                if (table_index >= 0 && table_index < 10) {
                    new_code = pattern_codes[*fixed_pattern_ptr][table_index];
                    int old_code = *current_code_ptr;
                    *current_code_ptr = new_code;
                    printf("Button %d pressed (Pattern %d): Current code %d -> %d\n", 
                           button_number, *fixed_pattern_ptr + 1, old_code, *current_code_ptr);
                }
                // 状態は変わらない
                return current_state;
            }
            // 他のボタンの短押しは今回実装しない
            break;
            
        case STATE_SETTING:  // Setting (9)
            if (event.is_release && button_number == 6) {
                // 単押し：遷移先をトグル
                switch (*setting_exit_target) {
                    case STATE_IC_UFLET_1:
                        *setting_exit_target = STATE_IC_UFLET_2;
                        break;
                    case STATE_IC_UFLET_2:
                        *setting_exit_target = STATE_FIXED;
                        break;
                    case STATE_FIXED:
                        *setting_exit_target = STATE_IC_UFLET_1;
                        break;
                }
                printf("Setting exit target toggled to: %d\n", *setting_exit_target);
                // 状態は変わらない
                return current_state;
            }
            if (event.is_long_press && button_number == 6) {
                // 長押し：設定した遷移先に移動
                printf("State transition: Setting (9) -> %d\n", *setting_exit_target);
                return *setting_exit_target;
            }
            break;
            
        default:
            printf("Unknown state: %d\n", current_state);
            break;
    }
    
    // No state transition occurred
    return current_state;
}

// Initialize current_code and current_key based on fixed_pattern
void initialize_code_and_key(int fixed_pattern, int* current_code_ptr, int* current_key_ptr) {
    *current_key_ptr = 0;  // Always reset to 0
    
    if (fixed_pattern == 2) {  // Pattern 3 (internal value 2)
        *current_code_ptr = 2;
        printf("Code and key initialized: current_code=%d, current_key=%d (pattern 3)\n", *current_code_ptr, *current_key_ptr);
    } else {
        *current_code_ptr = 0;
        printf("Code and key initialized: current_code=%d, current_key=%d (pattern %d)\n", *current_code_ptr, *current_key_ptr, fixed_pattern + 1);
    }
}

// Button state reading function
uint16_t read_button_states() {
    uint8_t upper_data, lower_data;
    uint8_t init_data = 0xFF;
    
    // Prepare UPPERPANEL for reading (write 0xFF)
    i2c_write_blocking(I2C_PORT, UPPERPANEL, &init_data, 1, false);
    // Read UPPERPANEL button state
    int upper_result = i2c_read_blocking(I2C_PORT, UPPERPANEL, &upper_data, 1, false);
    
    // Prepare LOWERPANEL for reading (write 0xFF)
    i2c_write_blocking(I2C_PORT, LOWERPANEL, &init_data, 1, false);
    // Read LOWERPANEL button state
    int lower_result = i2c_read_blocking(I2C_PORT, LOWERPANEL, &lower_data, 1, false);

    return (static_cast<uint16_t>(upper_data) << 8) | lower_data;
}

// Check which button was newly pressed by comparing current and previous state
int get_newly_pressed_button(uint16_t current_state, uint16_t previous_state) {
    // Calculate which bits changed from 1 to 0 (newly pressed)
    uint16_t newly_pressed = previous_state & (~current_state);
    
    // Check lower panel buttons 1-6 (bits 0-5)
    for (int i = 0; i <= 5; i++) {
        if (newly_pressed & (1 << i)) {  // This bit was newly pressed
            return i + 1;  // Return button 1-6
        }
    }
    
    // Check upper panel buttons 7-11 (bits 8-12)
    for (int i = 0; i <= 4; i++) {
        if (newly_pressed & (1 << (i + 8))) {  // This bit was newly pressed
            return i + 7;  // Return button 7-11
        }
    }
    
    return 0;  // No new button pressed
}

// Display number on 7-segment
void display_number(int number) {
    if (number >= 0 && number <= 9) {
        uint8_t segment_data = segCode[number];
        int result = i2c_write_blocking(I2C_PORT, LED7SEGADDR, &segment_data, 1, false);
        if (result == 1) {
            printf("Displaying number %d on 7-segment (code: 0x%02X)\n", number, segment_data);
        }
    }
}

// Blink state display 3 times with 0.4s interval
void blink_state_display(int state) {
    uint8_t display_code;
    uint8_t off_code = 0xFF;  // All segments off
    
    if (state == STATE_SETTING) {
        display_code = stateDisplay[3]; // 9
    } else {
        display_code = stateDisplay[state]; // 0->1, 1->2, 2->F
    }
    
    printf("Blinking state %d display\n", state);
    for (int i = 0; i < 3; i++) {
        // Turn on
        i2c_write_blocking(I2C_PORT, LED7SEGADDR, &display_code, 1, false);
        sleep_ms(200);
        // Turn off
        i2c_write_blocking(I2C_PORT, LED7SEGADDR, &off_code, 1, false);
        sleep_ms(200);
    }
}

// Check if button 6 is being pressed
bool is_button6_pressed(uint16_t button_state) {
    return !(button_state & (1 << BUTTON6_BIT));
}

// Check for long press detection for any button and return true if any long press is triggered
bool check_long_press(uint16_t current_button, uint16_t previous_button) {
    bool any_long_press_triggered = false;
    uint32_t current_time = to_ms_since_boot(get_absolute_time());
    
    // Check all 16 bits (buttons)
    for (int bit = 0; bit < 16; bit++) {
        bool currently_pressed = !(current_button & (1 << bit));  // 0 = pressed
        bool previously_pressed = !(previous_button & (1 << bit)); // 0 = pressed
        
        // Button just pressed
        if (currently_pressed && !previously_pressed) {
            button_long_press_state.press_start_time[bit] = current_time;
            button_long_press_state.long_press_detected[bit] = false;
        }
        // Button still pressed - check for long press
        else if (currently_pressed) {
            uint32_t press_duration = current_time - button_long_press_state.press_start_time[bit];
            if (press_duration >= LONG_PRESS_TIME && !button_long_press_state.long_press_detected[bit]) {
                button_long_press_state.long_press_detected[bit] = true;
                printf("Long press detected on bit %d\n", bit);
                any_long_press_triggered = true;
            }
        }
        // Button released - DON'T reset long_press_detected flag here
        // It will be used for filtering releases and reset later
    }
    
    return any_long_press_triggered;
}

// Check for button events (long press and valid releases)
ButtonEvent check_button_events(uint16_t current_button, uint16_t previous_button) {
    ButtonEvent event = {false, false, false, -1, 0, 0};
    
    // === Long Press Detection ===
    bool any_long_press_triggered = false;
    uint32_t current_time = to_ms_since_boot(get_absolute_time());
    
    // Check all 16 bits (buttons) for long press
    for (int bit = 0; bit < 16; bit++) {
        bool currently_pressed = !(current_button & (1 << bit));  // 0 = pressed
        bool previously_pressed = !(previous_button & (1 << bit)); // 0 = pressed
        
        // Button just pressed
        if (currently_pressed && !previously_pressed) {
            button_long_press_state.press_start_time[bit] = current_time;
            button_long_press_state.long_press_detected[bit] = false;
        }
        // Button still pressed - check for long press
        else if (currently_pressed) {
            uint32_t press_duration = current_time - button_long_press_state.press_start_time[bit];
            if (press_duration >= LONG_PRESS_TIME && !button_long_press_state.long_press_detected[bit]) {
                button_long_press_state.long_press_detected[bit] = true;
                printf("Long press detected on bit %d\n", bit);
                any_long_press_triggered = true;
                event.long_press_bits |= (1 << bit);
                if (event.button_bit == -1) event.button_bit = bit; // 最初のボタンを記録
            }
        }
    }
    
    // === Valid Release Detection ===
    bool has_valid_release = false;
    if (current_button != previous_button) {
        // Check each bit to see if any button was released
        uint16_t released_buttons = (~previous_button) & current_button;
        
        // Filter out releases that had long press detected
        for (int bit = 0; bit < 16; bit++) {
            if ((released_buttons & (1 << bit)) && button_long_press_state.long_press_detected[bit]) {
                // This button had long press, so ignore its release
                released_buttons &= ~(1 << bit);
                printf("Ignoring release of bit %d (had long press)\n", bit);
            }
        }
        
        // Reset long_press_detected flags for all released buttons after processing
        uint16_t all_released_buttons = (~previous_button) & current_button;
        for (int bit = 0; bit < 16; bit++) {
            if (all_released_buttons & (1 << bit)) {
                button_long_press_state.long_press_detected[bit] = false;
            }
        }
        
        // Check if there are any valid releases left after filtering
        if (released_buttons != 0) {
            has_valid_release = true;
            event.release_bits = released_buttons;
            // Find first released button bit
            for (int bit = 0; bit < 16; bit++) {
                if (released_buttons & (1 << bit)) {
                    if (event.button_bit == -1) event.button_bit = bit;
                    break;
                }
            }
        }
    }
    
    // Set event flags
    event.has_event = any_long_press_triggered || has_valid_release;
    event.is_long_press = any_long_press_triggered;
    event.is_release = has_valid_release;
    
    return event;
}

// Check if there are valid button releases (excluding long press releases)
bool check_valid_releases(uint16_t current_button, uint16_t previous_button) {
    if (current_button == previous_button) {
        return false;  // No button state change
    }
    
    // Check each bit to see if any button was released (1->0 transition in inverted logic)
    uint16_t released_buttons = (~previous_button) & current_button;
    
    // Filter out releases that had long press detected
    for (int bit = 0; bit < 16; bit++) {
        if ((released_buttons & (1 << bit)) && button_long_press_state.long_press_detected[bit]) {
            // This button had long press, so ignore its release
            released_buttons &= ~(1 << bit);
            printf("Ignoring release of bit %d (had long press)\n", bit);
        }
    }
    
    // Reset long_press_detected flags for all released buttons after processing
    uint16_t all_released_buttons = (~previous_button) & current_button;
    for (int bit = 0; bit < 16; bit++) {
        if (all_released_buttons & (1 << bit)) {
            button_long_press_state.long_press_detected[bit] = false;
        }
    }
    
    // Return true if there are any valid releases left after filtering
    return (released_buttons != 0);
}

// Display degree on 7-segment
void display_degree(int number) {
    if (number >= 0 && number <= 11) {
        uint8_t segment_data = segCodeDeg[number];
        int result = i2c_write_blocking(I2C_PORT, LED7SEGADDR, &segment_data, 1, false);
        if (result == 1) {
            printf("Displaying degree %d on 7-segment (code: 0x%02X)\n", number, segment_data);
        }
    }
}

// USB-MIDI callback functions
void tud_midi_rx_cb(uint8_t itf)
{
    (void) itf;
    
    uint8_t packet[4];
    while (tud_midi_available()) {
        if (tud_midi_packet_read(packet)) {
            printf("MIDI RX: %02X %02X %02X %02X\n", packet[0], packet[1], packet[2], packet[3]);
            
            // Check if this is a Control Change message
            uint8_t message_type = packet[1] & 0xF0;
            uint8_t channel = packet[1] & 0x0F;
            
            if (message_type == 0xB0) {  // Control Change
                uint8_t controller = packet[2];
                uint8_t value = packet[3];
                
                // Check for channel 15 (internal 14), controller 15
                if (channel == 14 && controller == 15) {
                    printf("Received CC: Ch%d CC%d Val%d\n", channel + 1, controller, value);
                    
                    // Ignore if current state is FIXED (2)
                    if (current_state == STATE_FIXED) {
                        printf("Ignoring CC in FIXED state\n");
                        return;
                    }
                    
                    // Set current_key to received value (clamped to 0-11)
                    current_key = value % 12;
                    current_code = 0;  // Reset current_code to 0
                    
                    printf("CC received: current_key set to %d, current_code reset to 0\n", current_key);
                    
                    // Update LED display
                    int combined_value = (current_code + current_key) % 12;
                    display_degree(combined_value);
                    printf("Display updated to show combined value: (%d + %d) mod 12 = %d\n", 
                           current_code, current_key, combined_value);
                }
            }
        }
    }
}

// Send a simple MIDI note on message
void send_midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
    uint8_t packet[4] = {0x09, (uint8_t)(0x90 | channel), note, velocity};
    if (tud_midi_mounted()) {
        tud_midi_packet_write(packet);
        printf("MIDI TX: Note On Ch%d Note%d Vel%d\n", channel, note, velocity);
    }
}

// Send a simple MIDI note off message  
void send_midi_note_off(uint8_t channel, uint8_t note, uint8_t velocity)
{
    uint8_t packet[4] = {0x08, (uint8_t)(0x80 | channel), note, velocity};
    if (tud_midi_mounted()) {
        tud_midi_packet_write(packet);
        printf("MIDI TX: Note Off Ch%d Note%d Vel%d\n", channel, note, velocity);
    }
}

// Send MIDI Control Change message
void send_midi_cc(uint8_t channel, uint8_t controller, uint8_t value)
{
    uint8_t packet[4] = {0x0B, (uint8_t)(0xB0 | channel), controller, value};
    if (tud_midi_mounted()) {
        tud_midi_packet_write(packet);
        printf("MIDI TX: CC Ch%d CC%d Val%d\n", channel, controller, value);
    }
}



int main()
{
    stdio_init_all();

    // Startup message
    printf("Raspberry Pi Pico USB-MIDI Device Starting\n");
    sleep_ms(2000); // Wait for serial communication stability

    // Initialize USB-MIDI
    tusb_init();
    printf("USB-MIDI initialized\n");

    // I2C Initialisation. Using it at 400Khz.
    i2c_init(I2C_PORT, 400*1000);
    
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    
    printf("I2C initialization completed (GPIO%d: SDA, GPIO%d: SCL, 400kHz)\n", I2C_SDA, I2C_SCL);
    
    // Execute I2C scan at startup
    i2c_scan();
    
    printf("7-segment LED display started (PCF8574 address: 0x%02X)\n", LED7SEGADDR);
    
    // Initialize state variables
    // current_state is now global variable, initialized at declaration
    int default_pattern = STATE_IC_UFLET_1;  // Default pattern
    int setting_exit_target = STATE_IC_UFLET_1;  // Setting mode exit target (toggles between 0,1,2)
    
    // Initialize 7-segment display with current code
    if (current_state == STATE_IC_UFLET_1 || current_state == STATE_IC_UFLET_2) {
        // State 0 or 1: Display current_code + current_key (mod 12)
        int combined_value = (current_code + current_key) % 12;
        display_degree(combined_value);
        printf("Initial display: combined value (%d + %d) mod 12 = %d\n", current_code, current_key, combined_value);
    } else {
        // Other states: Display current_code
        display_degree(current_code);  // Display initial current_code (0 = 'C')
    }
    
    printf("System initialized with current_code: %d, current_key: %d, fixed_pattern: %d\n", current_code, current_key, fixed_pattern);
    
    // Button state and timing variables
    uint16_t previous_button = 0xFFFF;
    
    printf("Starting in state %d (IC-UFlet-1)\n", current_state);
    
    while (true) {
        // Read current button state
        uint16_t current_button = read_button_states();
        
        // Check for button events (long press and valid releases)
        ButtonEvent event = check_button_events(current_button, previous_button);
        
        // Execute processing only if there's a valid event
        if (event.has_event) {
            
            // Log button state changes
            if (current_button != previous_button) {
                printf("Button state changed: 0x%04X -> 0x%04X", previous_button, current_button);
                if (event.is_release) {
                    printf(" (includes valid release on bits: 0x%04X)", event.release_bits);
                }
                printf("\n");
            }
            
            if (event.is_long_press) {
                printf("Long press triggered on bits: 0x%04X - processing\n", event.long_press_bits);
            }
            
            if (event.is_release) {
                printf("Valid release detected on bits: 0x%04X - processing\n", event.release_bits);
            }
            
            // Process state transition
            int new_state = process_state_transition(current_state, event, &setting_exit_target, &current_key, &current_code, &fixed_pattern);
            if (new_state != current_state) {
                int previous_state = current_state;
                current_state = new_state;
                
                // Handle state transition display logic
                if (current_state == STATE_SETTING) {
                    // Transitioning TO Setting state (9)
                    // Blink the previous state, then display setting_exit_target
                    printf("Transitioning to Setting - blinking previous state: %d\n", previous_state);
                    blink_state_display(previous_state);
                    
                    // Display the setting exit target state
                    uint8_t pattern = stateDisplay[setting_exit_target == STATE_SETTING ? 3 : setting_exit_target];
                    i2c_write_blocking(I2C_PORT, LED7SEGADDR, &pattern, 1, false);
                    printf("Display updated to show setting exit target: %d\n", setting_exit_target);
                    
                } else if (previous_state == STATE_SETTING) {
                    // Transitioning FROM Setting state (9) to other states
                    // Blink the new state, then display appropriate content
                    printf("Transitioning from Setting - blinking new state: %d\n", current_state);
                    blink_state_display(current_state);
                    initialize_code_and_key(fixed_pattern, &current_code, &current_key);
                    
                    int combined_value = (current_code + current_key) % 12;
                    display_degree(combined_value);
                    printf("Display updated to show combined value: (%d + %d) mod 12 = %d\n", 
                            current_code, current_key, combined_value);    
                }               
            } else if (current_state == STATE_FIXED && event.is_release && 
                      event.button_bit >= 0 && event.button_bit <= 5 && 
                      (event.button_bit + 1) == 6) {
                // State 2 and button 6 short press (fixed_pattern changed, no state change)
                // Display pattern number (1-4)
                display_number(fixed_pattern + 1);  // Show pattern 1-4
                printf("State 2: Display updated to show pattern number: %d\n", fixed_pattern + 1);
            } else if ((current_state == STATE_IC_UFLET_1 || current_state == STATE_IC_UFLET_2) && 
                      event.is_release && event.button_bit >= 0 && event.button_bit <= 5 && 
                      (event.button_bit + 1) == 6) {
                // State 0 or 1 and button 6 short press (current_key changed, no state change)
                // Update display to show the new current_key only
                display_degree(current_key);
                printf("State %d: Display updated to show current_key: %d\n", current_state, current_key);
            } else if (current_state == STATE_SETTING && event.is_release && 
                      event.button_bit >= 0 && event.button_bit <= 5 && 
                      (event.button_bit + 1) == 6) {
                // Setting state and button 6 short press (toggle occurred, no state change)
                // Update display to show the new setting_exit_target
                uint8_t pattern = stateDisplay[setting_exit_target == STATE_SETTING ? 3 : setting_exit_target];
                i2c_write_blocking(I2C_PORT, LED7SEGADDR, &pattern, 1, false);
                printf("Setting mode: Display updated to show new exit target: %d\n", setting_exit_target);
            } else {
                int combined_value = (current_code + current_key) % 12;
                display_degree(combined_value);
                printf("State 0: Display updated to show combined value: (%d + %d) mod 12 = %d\n", 
                       current_code, current_key, combined_value);
                // Send MIDI CC message
                send_midi_cc(14, 15, combined_value);
            }
            
        } else {
            // Button state changed but no valid trigger - skip processing
            if (current_button != previous_button) {
                printf("Button state changed: 0x%04X -> 0x%04X (no valid trigger - skipping)\n", 
                       previous_button, current_button);
            }
        }
        
        // Update previous button state
        previous_button = current_button;
        
        // Handle USB-MIDI tasks
        tud_task();
        
        uint8_t upper_led_data = 0xFF;
        uint8_t lower_led_data = 0xEE;
        // ledtest
        // i2c_write_blocking(I2C_PORT, UPPERPANEL, &upper_led_data, 1, false);
        // i2c_write_blocking(I2C_PORT, LOWERPANEL, &lower_led_data, 1, false);
    
        sleep_ms(10);
    }
}
