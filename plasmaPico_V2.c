#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/adc.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"

#include "pinsToggle.pio.h"


// Declare System Variables
// Pin "Addresses"
uint32_t S4 = 0x00000008; // 1000
uint32_t S3 = 0x00000004; // 0100
uint32_t S2 = 0x00000002; // 0010
uint32_t S1 = 0x00000001; // 0001

// Pulse States
uint32_t __not_in_flash("pwm") stop2free, free2stop, free2poss, free2neg, poss2free, neg2free;
uint32_t __not_in_flash("pwm") freeCycle, possCycle, negCycle;
uint32_t __not_in_flash("pwm") nextState, cycleCount;

// Pulse Charicteristics
uint32_t __not_in_flash("pwm") delay;
uint32_t __not_in_flash("pwm") target;

// The Number of the PIO State Machine
uint __not_in_flash("pwm") sm;


// === Status LED Initialization ===
#define LED_PIN 25 // Onboard LED

void init_led() {
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
}

typedef enum {
    STATE_WAITING_USB,    // Blink slow (waiting for USB connection)
    STATE_IDLE,           // LED off (no activity)
    STATE_RECEIVING,      // Blink fast (receiving data)
    STATE_DATA_READY,     // Double-blink (full buffer received)
    STATE_OUTPUT_SHOT     // LED on
} SystemState;

static SystemState current_state = STATE_WAITING_USB;

uint64_t last_led_update = 0;
bool led_on = false;

void update_led() {
    uint64_t now = time_us_64() / 1000; // Current time in milliseconds

    switch (current_state) {
        case STATE_WAITING_USB:
            // Short blink, long pause (100ms on, 900ms off)
            if (now - last_led_update >= (led_on ? 100 : 900)) {
                led_on = !led_on;
                gpio_put(LED_PIN, led_on);
                last_led_update = now;
            }
            break;

        case STATE_RECEIVING:
            // Rapid flicker (50ms on/off)
            if (now - last_led_update >= 50) {
                led_on = !led_on;
                gpio_put(LED_PIN, led_on);
                last_led_update = now;
            }
            break;
        
        case STATE_DATA_READY:
            // Double-blink, pause (100ms on, 100ms off, 100ms on, 700ms off)
            static uint8_t blink_phase = 0;
            if (now - last_led_update >=
                (blink_phase == 0 ? 100 :
                 blink_phase == 1 ? 100 : 
                 blink_phase == 2 ? 100 : 700)) {
                    
                gpio_put(LED_PIN, (blink_phase % 2 == 0));
                blink_phase = (blink_phase + 1) % 4;
                last_led_update = now;
            }
            break;

        case STATE_IDLE:
            gpio_put(LED_PIN, 0); // LED off
            break;

        case STATE_OUTPUT_SHOT:
            gpio_put(LED_PIN, 1); // LED on
            break;
    }
}


// === Serial Configuration ===
// Uncomment ONE of these to choose the interface:
#define USE_USB_CDC   // Use USB serial (e.g., /dev/ttyACM0)
//#define USE_UART      // Use hardware UART (GPIO pins)

#ifdef USE_UART
#define UART_ID uart0
#define BAUD_RATE 115200
#define UART_TX_PIN 0 // Change depending on GPIO configuration
#define UART_RX_PIN 1 // ^
#endif

// Serial Protocol Constants
#define START_BYTE 0xAA
#define END_BYTE 0x55
#define MAX_PULSES 16383 // Max pulses per packet - ensure agreement with the transmitter

// Type Bytes
typedef enum {
    MSG_PWM = 0x01,
    MSG_MANUAL = 0x02,
    MSG_CONFIG = 0x03
} MessageType;
MessageType current_msg_type;

// Recieved pulse buffer
uint8_t pulse_buffer[MAX_PULSES];
uint16_t __not_in_flash("pwm") recieved_pulses = 0;


// === Hardware-Agnostic Serial Helpers ===
bool serial_connected() {
    #ifdef USE_USB_CDC
    return stdio_usb_connected();
    #elif defined(USE_UART)
    return true; // UART is always "connected"
    #endif
}

void init_serial() {
    #ifdef USE_USB_CDC
    stdio_init_all(); // Initializes USB CDC
    current_state = STATE_WAITING_USB;
    while (!serial_connected()) {
        update_led();
        sleep_ms(10); // Wait for USB connection
    }

    #elif defined(USE_UART)
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    #endif

    current_state = STATE_IDLE; // Connected, now idle
}

int read_serial_byte() {
    #ifdef USE_USB_CDC
    return getchar_timeout_us(0); // Non-blocking USB read
    #elif defined(USE_UART)
    return uart_is_readable(UART_ID) ? uart_getc(UART_ID) : PICO_ERROR_TIMEOUT; // Non-blocking UART read, returns -1 if no data
    #endif
}


// Serial Protocol Input Parser
void serial_input() {
    /*
    Parses incoming serial data (UART or USB) of the following protocol:
    [START_BYTE][TYPE][LENGTH][DATA...][CHECKSUM][END_BYTE]
    
    Start/End: 0xAA/0x55
    Types: 0x01 (PWM), 0x02 (Manual Mode), 0x03 (Config)
    */
    static enum { WAIT_START, WAIT_LENGTH, WAIT_TYPE, WAIT_DATA, WAIT_CHECKSUM, WAIT_END } state = WAIT_START;
    static uint8_t expected_length = 0;
    static uint8_t checksum = 0;
    static uint16_t data_index = 0;

    int byte;
    while ((byte = read_serial_byte()) != PICO_ERROR_TIMEOUT) {
        switch (state) {
            case WAIT_START:
                if (byte == START_BYTE) {
                    checksum = START_BYTE;
                    state = WAIT_LENGTH;

                    current_state = STATE_RECEIVING; // Data incoming
                }
                break;

            case WAIT_TYPE:
                if (byte >= 0x01 && byte <= 0x03) {
                    current_msg_type = (MessageType)byte;
                    checksum ^= byte;
                    state = WAIT_LENGTH;
                } else {
                    state = WAIT_START; // Reset on invalid type TODO: determine if we want to throw an error flag
                }
                break;

            
            case WAIT_LENGTH:
                expected_length = byte;
                checksum ^= byte;
                data_index = 0;
                state = (expected_length > 0) ? WAIT_DATA : WAIT_CHECKSUM;
                break;

            case WAIT_DATA:
                if (data_index < MAX_PULSES) {
                    pulse_buffer[data_index++] = byte;
                    checksum ^= byte;
                }
                if (data_index >= expected_length) {
                    state = WAIT_CHECKSUM;
                }
                break;

            case WAIT_CHECKSUM:
                if (checksum == byte) {
                    state == WAIT_END;
                } else {
                    state = WAIT_START; // Reset on checksum error TODO: function to report error
                }
                break;

            case WAIT_END:
                if (byte == END_BYTE) {
                    recieved_pulses = expected_length;
                    printf("Recieved %d pulses\n", recieved_pulses); // Disable this if not testing TODO: Add compile time testing flags

                    current_state = STATE_DATA_READY;
                }
                state = WAIT_START; // Reset
                break;
        }
    }
}

// Pulse Functions
void __time_critical_func(on_pwm_wrap)() {
    /*
    Runs
    */
   // Clears interrupt flag
   pwm_clear_irq(0);


   // Pushes the stored next pulse state to the FIFO
   //pio_sm_put(pio0, sm, nextState);
   pio0->txf[sm] = nextState; // Same as pio_sm_put without checking

   
   // Calculates the following nextState for next cycle
   //target = 100; // Target values from 0 - 99 are negative pulses. Values from 100 - 199 are positive pulses.

   // Finds nextState from target
   if (target < 100) { // Negative pulses
    nextState = negCycle;
    delay = (100-target) * 5; // Delay in PIO cycles @ 25 MHz
    } else { // Positive pulses
        nextState = possCycle;
        delay = (target-99) * 5; // Delay in PIO cycles @ 25 MHz
    }

    // Sets Lower bound on DCP (1 us + switching time)/20 us ~7.5%
    if (delay < 25) {nextState = freeCycle;} // TODO: add back in subseviding dead zone
    // Sets Upper bound on DCP (18 us + switching time)/20 us ~92.5%
    if (delay > 450) {delay = 450;}              


    // Sets nextState for the next cycle with the new delay
    nextState = nextState | ( delay << 8);

    cycleCount++;
}


void init_shot() {
    /*
    Runs initialization for shot pulse output:
        Computes output state definitions
        Sets up PIO
        Sets up PWM wrapping
    */
    static const uint startPin = 10;

    set_sys_clock_khz(125000, true); //125000


    // Computes state definitions
    stop2free = (S2 | S4) << 4;  // Turn on S2 and S4
    free2stop = 0;  // Turn all off
    freeCycle = ((S2 | S4) << 28) | ((S2 | S4) << 24) | ((S2 | S4) << 4) | (S2 | S4);
 
    free2poss = ((S2 | S3) << 4) | S2;  // S2 only then S2 and S3
    poss2free = ((S2 | S4) << 4) | S2;  // S2 only then S2 and S4
    possCycle = (poss2free << 24) | free2poss;
 
    free2neg = ((S1 | S4) << 4) | S4;  // S4 only then S1 and S4
    neg2free = ((S2 | S4) << 4) | S4;  // S4 only then S2 and S4
    negCycle = (neg2free << 24) | free2neg;


    // Set Up PIO
    // Choose PIO instance (0 or 1)
    PIO pio = pio0;

    // Get first free state machine in PIO 0
    uint sm = pio_claim_unused_sm(pio, true);

    // Add PIO program to PIO instruction memory. SDK will find location and return with the memory offset of the program.
    uint offset = pio_add_program(pio, &pinsToggle_program);

    // PIO clock divider
    float div = 5.f; //(float)clock_get_hz(clk_sys) / pio_freq;

    // Initialize the program using the helper function in our .pio file
    pinsToggle_program_init(pio, sm, offset, startPin, div);


    // Set up PWM Wrapping
    pwm_clear_irq(0);
    pwm_set_irq_enabled(0, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, on_pwm_wrap);
    irq_set_enabled(PWM_IRQ_WRAP, true);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, div); // Use system clock frequency (25 MHz)
    pwm_config_set_wrap(&config, 499);   // Wrap every 20 us (0-499)
    pwm_init(0, &config, false);


    return;
}


void run_shot(uint16_t pulseCycles) {
    /*
    Runs shot pulses and then turns off PWM and PIO sm
    */

    // Loads freewheeling as first PWM pulse
    delay = 250;
    nextState = (stop2free << 24) | (( delay << 8) | free2stop);

    // Start PWM
    pwm_set_enabled(0, true);
    //busy_wait_ms(10);

    // Pulse Loop
    for (uint16_t cycle = 0; cycle <= pulseCycles; cycle++) {
        sleep_ms(1);
        target = cycle; // Sets the target variable which is pulled by the PWM function
    }

    // Turn off PWM
    pwm_set_enabled(0, false);
    irq_set_enabled(PWM_IRQ_WRAP, false);
     
    // Return to off state
    pio_sm_put(pio0, sm, stop2free);
     
    //Turn off pio
    pio_sm_set_enabled(pio0, sm, false);

    return;
}


int main() {
    // Status indicator
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    gpio_put(LED_PIN, 1);


    // Initialize Serial Configuration
    init_serial();


    // Initialize PWM and PIO
    init_shot();


    // Runs pulse and then turns off PWM and PIO sm
    run_shot(199);


    return 0;
}