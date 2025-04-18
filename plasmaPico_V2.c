#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/adc.h"

#include "pinsToggle.pio.h"

// NOTE:
// Pulse: a single PWM pulse
// CUTE_Pulse: a 200ms tokamak pulse
// I'm trying not to use the same language for both, but as of now it's still inconsistent and context-based


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


// Number of PIO State Machine
uint __not_in_flash("pwm") sm;

// Array of Pulse Lengths
unsigned char* pulseDelays;
uint16_t __not_in_flash("pwm") pulseDelays_length_uint;


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


void init_pulse() {
    /*
    Runs initialization for pulse output:
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


void run_pulse(uint16_t pulseCycles) {
    /*
    Runs pulse and then turns off PWM and PIO sm
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
    const uint LED_PIN = 25;

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    gpio_put(LED_PIN, 1);


    // Initializations
    // Initialize Serial Libraries (USB)
    stdio_init_all();

    // Initialize PWM and PIO
    init_pulse();

    // Runs pulse and then turns off PWM and PIO sm
    run_pulse(199);


    return 0;
}