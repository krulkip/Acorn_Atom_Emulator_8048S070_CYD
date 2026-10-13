#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_sntp.h"
#include "esp_timer.h"

extern uint8_t atom_serial_show_cpu_diagnostic;
#include "driver/gpio.h"

#include "fake6502.h"
extern uint32_t clockticks6502;
#include "fakemem.h"
#include "info_display.h"
#include "command_handler.h"
#include "p_slip.h"
#include "atom_kbd.h"           // legacy matrix kbd (optional)
#include "atom_serial.h"        // UART FIFO
#include "atom_osrdch_hook.h"   // installs OSRDCH vector hook

void console_task(void *pvParameters);
static void cpu_task(void *pvParameters);
static int atom_atommc2_banner_at_8000(void);
static void atom_scan_task(void *pvParameters);   // <-- add this line
extern void fakemem_init_atom();
extern uint8_t atom_reload_asbk_s(void);
extern void atom_scan_for_banner(void);

//-----------------------------------------------------------------------------

// Memory map
// 0x0000 - 0x00FF: Zero Page
// 0x0100 - 0x01FF: Stack
// V166: 6522 Peripheral (Fake 6522) is mapped at $B800-$B80F in fakemem.c
// 0x7000 - 0x70FF: Callable Memory (Custom emulator functions)
// 0x8000 - 0xFFFF: ROM

// Custom Emulator Functions
// 0x7000 : -w : led
// 0x7001 : -w : Delay 

//-----------------------------------------------------------------------------
const uint16_t EXEC_START = 0x8000;
uint8_t fake6502_running_status;

//-----------------------------------------------------------------------------
void io_init(){
  // Initialize the IO Buttons and Leds
  gpio_config_t config_input = {
    // GPIO 0 and 13 Buttons
    .pin_bit_mask = (1ULL << GPIO_NUM_0), 
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_ENABLE,
    .intr_type = GPIO_INTR_DISABLE
  };
  // Configure GPIO 0 and 13 as input with pull-up enabled
  gpio_config(&config_input); 

}
//-----------------------------------------------------------------------------
void io_task(void *pvParameters) {
  while(1) {
    // Check if GPIO 0 button is pressed
    if(!gpio_get_level(GPIO_NUM_0)) {
      //printf("Resetting 6502 CPU...\n");
      reset6502(); // Reset the CPU state
      *fake6502_status = FLAG_CONSTANT;
      while(!gpio_get_level(GPIO_NUM_0)){
        vTaskDelay(pdMS_TO_TICKS(250)); 
      }
    }
    vTaskDelay(pdMS_TO_TICKS(150)); // Adjust delay as needed
  }
}
//-----------------------------------------------------------------------------
void io_write(uint16_t addr, uint8_t byte) {
  // Handle IO write operations
  if(byte == 0x01) {
    // gpio_set_level(GPIO_NUM_45, 1); // disabled on classic ESP32
  } else if(byte == 0x00) {
    // gpio_set_level(GPIO_NUM_45, 0); // disabled on classic ESP32
  }
}
void delay_write(uint16_t addr, uint8_t byte){
  vTaskDelay(pdMS_TO_TICKS(byte));
}

//-----------------------------------------------------------------------------
void log_perf_task(void *pvParameters) {
  static uint32_t old_instructions = 0;
  while(1) {
    // Log performance metrics every second
    char text[32];
    sprintf(text, "6502 CPU Speed: %dKips\n", 
                (int)((instructions - old_instructions)/1000));
    serial_send_slip_byte(CMD_LOG);
    serial_send_slip_bytes((uint8_t *)text, strlen(text)); // Send log message
    serial_send_slip_end();
    old_instructions = instructions; // Update old instruction count
    vTaskDelay(pdMS_TO_TICKS(1000)); // Log every second
  }
}

extern uint8_t atom_tape_turbo_active(void);

extern void atom_host_break_cleanup(void);

static volatile uint8_t atom_break_requested = 0;

// V171: SHIFT+BREAK BBC boot handshake owned by the Arduino loop.
extern volatile uint8_t v171_bbc_boot_pending;
extern volatile uint8_t v171_bbc_atom_ready;
extern volatile uint32_t v173_bbc_osrdch_epoch;

/*
 * V127 ASBK-S resident initialization.
 *
 * The ASBK-S image is already present at $1000.  We must not call LL97
 * immediately after reset6502(), because the real Atom Kernel RESET routine
 * has not yet run and would subsequently overwrite the page-2 vectors that
 * LL97 installs.
 *
 * Instead, wait until the Kernel reaches its first OSRDCH call at $FE94.
 * At that point RESET is complete and the Atom is waiting for input.
 * Inject an exact 6502 JSR $1005 by pushing FE93 and changing PC to $1005.
 * LL97 finishes with RTS, which returns to $FE94 and normal input continues.
 */
static volatile uint8_t atom_asbk_init_pending = 1;
// V155: clean BBC test. ASBK-S is completely bypassed.
// No LL97 call and no zero-page/vector restoration. Diagnostics only.
static volatile uint8_t atom_bbc_entry_pending = 1;
static uint8_t v173_fe94_seen = 0;
// V132: host BREAK clears $1000 RAM; cold boot does not need a reload.
static volatile uint8_t atom_asbk_reload_pending = 0;
static volatile uint8_t atom_break_banner_pending = 0;
static int64_t atom_break_banner_due_us = 0;

void atom_request_break(void)
{
    atom_break_requested = 1;
}

static void cpu_task(void *pvParameters)
{
    (void)pvParameters;
    uint8_t last_tape_turbo = 0;

    // V075 CPU-speed diagnostic. clockticks6502 counts emulated CPU cycles.
    uint32_t cpu_diag_last_cycles = clockticks6502;
    int64_t cpu_diag_last_us = esp_timer_get_time();

    // V076 1 MHz governor reference point.
    // 1 emulated 6502 cycle should consume 1 real microsecond.
    uint32_t cpu_gov_base_cycles = clockticks6502;
    int64_t cpu_gov_base_us = esp_timer_get_time();

    // This is your old while(1) loop, moved into its own task,
    // with the time_t stuff removed.
    for (;;) {
        /*
         * V074 host ~BREAK.
         * Handle it here, between 6502 instructions, so the Arduino/Serial
         * task never races reset6502() against step6502().
         */
        if (atom_break_requested) {
            atom_break_requested = 0;

            // Stop normal execution while reset state is rebuilt.
            fake6502_running_status = 1;

            // Clear queued/direct keyboard state before rebuilding reset state.
            atom_kbd_flush();
            atom_serial_init();

            // Clean transient virtual cassette/CAT state but keep the mounted
            // file and all ROM/SD/SPIFFS configuration intact.
            atom_host_break_cleanup();

            // Perform an Atom CPU reset. The real Kernel RESET routine at the
            // vector then reinitializes display/system state and returns to >.
            reset6502();
            *fake6502_status = FLAG_CONSTANT;
            atom_asbk_init_pending = 1;
            atom_bbc_entry_pending = 1;
            atom_asbk_reload_pending = 1;
            atom_break_banner_pending = 1;
            atom_break_banner_due_us = esp_timer_get_time() + 500000;

            printf("[BREAK] Atom reset vector PC=$%04X\n", *fake6502_pc);

            // Rebase timing after BREAK; do not try to "catch up" for the
            // wall-clock time spent paused/resetting.
            cpu_gov_base_cycles = clockticks6502;
            cpu_gov_base_us = esp_timer_get_time();
            cpu_diag_last_cycles = clockticks6502;
            cpu_diag_last_us = cpu_gov_base_us;

            fake6502_running_status = 0;
        }

        // 1 = BREAK: pause CPU, let other tasks run
        if (fake6502_running_status == 1) {
            vTaskDelay(1);
            continue;
        }

        // 2 = STEP: execute one instruction, then go back to BREAK
        if (fake6502_running_status == 2) {
            fake6502_running_status = 1;
        }

        /*
         * V178 mode split at the proven post-RESET OSRDCH point.
         *
         * Ordinary Atom boot/BREAK: restore the exact V141/V139 ASBK-S V24
         * behaviour.  After BREAK the Kernel has cleared $1000 RAM, so reload
         * V24, then emulate JSR $1005.  V24 itself contains the proven static
         * AtoMMC V3.02E vector table (COMVEC $E0DE plus filing vectors).
         *
         * SHIFT+BREAK: do NOT reload or run ASBK-S.  The real Atom reset has
         * already cleared its RAM workspace; leave the natural vectors alone
         * and hand the ready event to the existing V177 LINK #4000 path.
         */
        if (atom_asbk_init_pending && *fake6502_pc == 0xFE94) {
            if (v171_bbc_boot_pending) {
                atom_asbk_init_pending = 0;
                atom_asbk_reload_pending = 0;
                v171_bbc_atom_ready = 1;
                ++v173_bbc_osrdch_epoch;
                printf("[BBC] ASBK-S V24 bypassed - natural Atom vectors retained\n");
                printf("[BBC] natural vectors: WRCVEC=$%02X%02X RDCVEC=$%02X%02X COMVEC=$%02X%02X\n",
                       fakemem[0x0209], fakemem[0x0208], fakemem[0x020B], fakemem[0x020A],
                       fakemem[0x0207], fakemem[0x0206]);
                printf("[BREAK] Post-reset Atom OSRDCH reached at $FE94 - BBC command enabled\n");
            } else {
                if (atom_asbk_reload_pending) {
                    atom_asbk_reload_pending = 0;
                    if (atom_reload_asbk_s())
                        printf("[ASBK-S] Reloaded V24 after BREAK\n");
                }
                if (fakemem[0x1000] == 0x40 && fakemem[0x1001] == 0xBF) {
                    atom_asbk_init_pending = 0;
                    push16((uint16_t)(*fake6502_pc - 1));
                    *fake6502_pc = 0x1005;
                    printf("[ASBK-S] Atom mode - auto-init V24 LL97 at $1005\n");
                } else {
                    atom_asbk_init_pending = 0;
                    printf("[ASBK-S] WARNING: V24 image signature missing at $1000\n");
                }
            }
        }

        // V173: after the first ready point, each later entry to FE94 means
        // Atom BASIC is requesting the next command character. Count the edge
        // only once while PC remains at FE94.
        if (!atom_asbk_init_pending && v171_bbc_boot_pending &&
            v171_bbc_atom_ready && *fake6502_pc == 0xFE94) {
            if (!v173_fe94_seen) {
                ++v173_bbc_osrdch_epoch;
                v173_fe94_seen = 1;
            }
        } else {
            v173_fe94_seen = 0;
        }

        // V132: BREAK causes AtoMMC to redraw its original startup banner.
        // Once it has had time to appear, apply the same ACORN ATOM+ cleanup
        // used at cold boot.
        if (atom_break_banner_pending && atom_atommc2_banner_at_8000()) {
            atom_break_banner_pending = 0;
            atom_scan_for_banner();
        }

        // V164: first entry into BBC BASIC. No state is restored or patched here.
        // The V164 fix lives only in fakemem.c: $6000-$60FF is BBC BASIC RAM
        // while the CPU is executing ATBASIC3 at $4000-$7FFF.
        if (atom_bbc_entry_pending && *fake6502_pc >= 0x4000 && *fake6502_pc < 0x8000) {
            atom_bbc_entry_pending = 0;

            // V177: LINK #4000 has now transferred control to BBC BASIC.
            // Clear the Mode-0 text RAM and home the genuine Atom VDU cursor
            // before BBC emits its first startup character.  Atom COS keeps
            // the cursor line address in $DE/$DF and column in $E0.
            for (uint16_t a = 0x8000; a <= 0x81FF; ++a)
                fakemem[a] = 0x40;          // Mode-0 blank
            fakemem[0x00DE] = 0x00;         // line base = $8000
            fakemem[0x00DF] = 0x80;
            fakemem[0x00E0] = 0x00;         // column 0
            // Deliberately leave $E1 (cursor on/off state) unchanged.
            printf("[BBC] LINK #4000 entered - screen cleared, Atom cursor homed to $8000\n");
        }

        // 0 (or anything else you use) = RUN: keep stepping
        step6502();

        /*
         * V072 automatic virtual-tape turbo.
         *
         * Normal operation remains exactly as before: yield for one RTOS tick
         * every 256 instructions.
         *
         * While the genuine Atom ROM is reading our virtual cassette, the
         * cassette waveform is still timed from clockticks6502 (300-baud
         * logical timing), so CPU and tape can safely run much faster together
         * in wall-clock time. We only yield every 16384 instructions to keep
         * the ESP32 idle task/watchdog and Arduino loop healthy.
         */
        uint8_t tape_turbo = atom_tape_turbo_active();

        if (tape_turbo != last_tape_turbo) {
            printf("[TAPE-TURBO] %s\n", tape_turbo ? "ON" : "OFF");
            last_tape_turbo = tape_turbo;

            // Rebase when entering/leaving tape turbo so the normal governor
            // never tries to compensate for deliberately accelerated tape time.
            cpu_gov_base_cycles = clockticks6502;
            cpu_gov_base_us = esp_timer_get_time();
        }

        /*
         * V076 1 MHz governor.
         *
         * Target:
         *   1,000,000 emulated cycles / real second
         * => one emulated cycle per real microsecond.
         *
         * While fast virtual cassette is active we intentionally do not
         * govern to 1 MHz, preserving the near-instant V073+ load behavior.
         */
        if (tape_turbo) {
            // Yield occasionally for RTOS/watchdog health only.
            if ((instructions & 0x3FFF) == 0)
                vTaskDelay(1);
        } else {
            uint32_t gov_cycles = clockticks6502 - cpu_gov_base_cycles;
            int64_t gov_now_us = esp_timer_get_time();
            int64_t gov_target_us = cpu_gov_base_us + (int64_t)gov_cycles;
            int64_t gov_ahead_us = gov_target_us - gov_now_us;

            if (gov_ahead_us > 0) {
                /*
                 * For larger waits, yield most of the excess time to FreeRTOS.
                 * Leave the final <1 tick region to a short microsecond spin
                 * so the average clock does not quantize to the RTOS tick.
                 */
                if (gov_ahead_us >= 2000) {
                    TickType_t ticks =
                        (TickType_t)(gov_ahead_us / 1000);
                    if (ticks > 1)
                        vTaskDelay(ticks - 1);
                }

                while ((gov_target_us - esp_timer_get_time()) > 20) {
                    taskYIELD();
                }

                while (esp_timer_get_time() < gov_target_us) {
                    // bounded fine wait, normally only a few microseconds
                }
            }

            /*
             * Periodically rebase to keep 32-bit cycle deltas small and avoid
             * long-term accumulation of timer arithmetic error.
             */
            if ((clockticks6502 - cpu_gov_base_cycles) >= 1000000u) {
                cpu_gov_base_cycles = clockticks6502;
                cpu_gov_base_us = esp_timer_get_time();
            }
        }

        /*
         * V108 low-overhead CPU diagnostic.
         *
         * V075-V105b called esp_timer_get_time() after EVERY 6502 instruction
         * merely to decide whether one second had elapsed.  That diagnostic
         * measurement itself was permanently in the CPU hot path.
         *
         * Keep the same [CPU] information, but inspect the real-time clock
         * only every 2048 emulated instructions.  The 1 MHz governor above is
         * deliberately left completely unchanged in V108.
         */
        if (atom_serial_show_cpu_diagnostic && (instructions & 0x07FFu) == 0u) {
            int64_t cpu_diag_now_us = esp_timer_get_time();
            int64_t cpu_diag_elapsed_us = cpu_diag_now_us - cpu_diag_last_us;

            if (cpu_diag_elapsed_us >= 1000000LL) {
                uint32_t cpu_diag_now_cycles = clockticks6502;
                uint32_t cpu_diag_delta_cycles =
                    cpu_diag_now_cycles - cpu_diag_last_cycles;

                double mhz = (double)cpu_diag_delta_cycles /
                             (double)cpu_diag_elapsed_us;

                printf("[CPU] %lu cycles in %.3f s = %.3f MHz  PC=$%04X\n",
                       (unsigned long)cpu_diag_delta_cycles,
                       (double)cpu_diag_elapsed_us / 1000000.0,
                       mhz,
                       *fake6502_pc);

                cpu_diag_last_cycles = cpu_diag_now_cycles;
                cpu_diag_last_us = cpu_diag_now_us;
            }
        }
    }
}

static int atom_atommc2_banner_at_8000(void)
{
    // Atom screen codes for the complete temporary startup banner:
    // "ACORN ATOM + ATOMMC2".  Wait for the suffix as well so the
    // cleanup cannot run before AtoMMC2 has finished drawing it.
    static const uint8_t pattern[] = {
        0x01, 0x03, 0x0F, 0x12, 0x0E, 0x20, 0x01, 0x14, 0x0F, 0x0D,
        0x20, 0x2B, 0x20, 0x01, 0x14, 0x0F, 0x0D, 0x0D, 0x03, 0x32
    };
    for (unsigned i = 0; i < sizeof(pattern); ++i)
        if (fakemem[0x8000u + i] != pattern[i]) return 0;
    return 1;
}

static int atom_banner_at_8000(void)
{
    static const uint8_t pattern[] = {
        0x01, 0x03, 0x0F, 0x12, 0x0E, 0x20, 0x01, 0x14, 0x0F, 0x0D
    };
    for (unsigned i = 0; i < sizeof(pattern); ++i)
        if (fakemem[0x8000u + i] != pattern[i]) return 0;
    return 1;
}

static void atom_scan_task(void *pvParameters)
{
    (void)pvParameters;

    // V180: wait until AtoMMC2 has finished writing the complete temporary
    // "ACORN ATOM + ATOMMC2" banner, then immediately apply the existing
    // ACORN ATOM+ cleanup. This avoids V180 firing on the earlier base banner.
    for (unsigned n = 0; n < 500; ++n) {
        if (atom_atommc2_banner_at_8000()) {
            atom_scan_for_banner();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    vTaskDelete(NULL);
}

// The Atom kernel RESET routine ($FF3F) copies default OS vectors into
// page 2 RAM ($0204..$021B), which overwrites the OSRDCH vector at
// $020A/$020B. So we must patch OSRDCH *after* the ROM has run RESET.
static void atom_install_hook_task(void *pvParameters)
{
    (void)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(250));

    extern void atom_install_osrdch_serial_hook(void);
    atom_install_osrdch_serial_hook();

    vTaskDelete(NULL);
}

//-----------------------------------------------------------------------------
void bitboard_app_main(void)
{
    // ... keep ALL your existing init here ...
    // V111k: GPIO0 is I2S BCLK on the ESP32-8048S070.
    // Do not run the old CYD io_init(), which reconfigures GPIO0 as input.
    fakemem_init_atom();   // <-- Load Atom ROMs + RAM
    atom_kbd_init();       // legacy matrix keyboard (still safe)
    atom_serial_init();    // UART FIFO for OSRDCH hook
    atom_debug_out_init(); // V123 output mirror FIFO
    fakemem[0xBFFE] = 0;   // mirror OFF by default
    reset6502();           // <-- Reset vector comes from Atom Kernel at FFFC/FFFD
    atom_asbk_init_pending = 1;
    printf("[ATOM] After reset PC=$%04X\n", *fake6502_pc);
    fake6502_running_status = 0;  // optional: start in BREAK
    // or 0 if you want it to run immediately
    //command_init();

    xTaskCreatePinnedToCore(
        cpu_task,          // Task function
        "cpu_task",        // Name
        4096,              // Stack size (tune if needed)
        NULL,              // Parameters
        1,                 // Priority
        NULL,              // Task handle
        1                  // Core (0 or 1; use same as before, or 1)
    );
        // Run a one-shot task to scan RAM for "ACORN ATOM"
    xTaskCreatePinnedToCore(
        atom_scan_task,
        "atom_scan_task",
        4096,
        NULL,
        1,
        NULL,
        1
    );

    // V041: do NOT install the OSRDCH serial hook.
    // Input is now presented through the real Atom 8255 keyboard matrix,
    // so the ROM's original OSRDCH vector remains active.

    // IMPORTANT: NO while(1) here anymore.
    // Just return so Arduino's setup() can finish and loop() can run.
}
