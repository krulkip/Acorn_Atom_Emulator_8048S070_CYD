#include <Arduino.h>
#include "esp_system.h"
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <driver/i2s.h>
#include <SPIFFS.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include "tft_setup.h"

// ---------------------------------------------------------------------------
// Serial diagnostic switches
// 0 = suppress this diagnostic, 1 = show it.
// These affect only the repeating diagnostics; normal boot messages remain.
// ---------------------------------------------------------------------------
#define SERIAL_SHOW_ATOMMC_COMMENTS      0
#define SERIAL_SHOW_CPU_DIAGNOSTIC       0
#define SERIAL_SHOW_RX_MATRIX_DIAGNOSTIC 0


#include "fakemem_atom.h"
#include "atom_kbd.h"
#include "atom_hw_kbd.h"

extern "C" {
#include "atom_serial.h"
#include "fake6502.h"
#include "fakemem.h"

extern uint16_t *fake6502_pc;
extern uint32_t clockticks6502;
extern uint8_t *fake6502_y;



extern uint8_t last_oswrch_char;
extern uint32_t last_oswrch_count;

extern uint8_t atom_last_b000_write;

void bitboard_app_main(void);
void atom_request_break(void);
}

extern "C" void atom_dump_vram_top();
volatile uint8_t suppress_boot_prompt = 0;
volatile uint8_t user_typed_since_boot = 0;

extern "C" {
uint8_t atom_serial_show_cpu_diagnostic =
    SERIAL_SHOW_CPU_DIAGNOSTIC ? 1 : 0;
uint8_t atom_serial_show_atommc_comments =
    SERIAL_SHOW_ATOMMC_COMMENTS ? 1 : 0;
}



// ---------------------------------------------------------------------------
// ESP32-8048S070 7-inch 800x480 RGB display
// Exact LovyanGFX configuration proven by the standalone screen test.
// ---------------------------------------------------------------------------
class AtomLGFX : public lgfx::LGFX_Device
{
public:
  lgfx::Bus_RGB   _bus_instance;
  lgfx::Panel_RGB _panel_instance;
  lgfx::Light_PWM _light_instance;

  AtomLGFX(void)
  {
    {
      auto cfg = _panel_instance.config();
      cfg.memory_width  = 800;
      cfg.memory_height = 480;
      cfg.panel_width   = 800;
      cfg.panel_height  = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _panel_instance.config_detail();
      cfg.use_psram = 1;
      _panel_instance.config_detail(cfg);
    }
    {
      auto cfg = _bus_instance.config();
      cfg.panel = &_panel_instance;

      cfg.pin_d0  = GPIO_NUM_15;
      cfg.pin_d1  = GPIO_NUM_7;
      cfg.pin_d2  = GPIO_NUM_6;
      cfg.pin_d3  = GPIO_NUM_5;
      cfg.pin_d4  = GPIO_NUM_4;
      cfg.pin_d5  = GPIO_NUM_9;
      cfg.pin_d6  = GPIO_NUM_46;
      cfg.pin_d7  = GPIO_NUM_3;
      cfg.pin_d8  = GPIO_NUM_8;
      cfg.pin_d9  = GPIO_NUM_16;
      cfg.pin_d10 = GPIO_NUM_1;
      cfg.pin_d11 = GPIO_NUM_14;
      cfg.pin_d12 = GPIO_NUM_21;
      cfg.pin_d13 = GPIO_NUM_47;
      cfg.pin_d14 = GPIO_NUM_48;
      cfg.pin_d15 = GPIO_NUM_45;

      cfg.pin_henable = GPIO_NUM_41;
      cfg.pin_vsync   = GPIO_NUM_40;
      cfg.pin_hsync   = GPIO_NUM_39;
      cfg.pin_pclk    = GPIO_NUM_42;

      cfg.freq_write = 14000000;
      cfg.hsync_polarity    = 0;
      cfg.hsync_front_porch = 80;
      cfg.hsync_pulse_width = 4;
      cfg.hsync_back_porch  = 16;
      cfg.vsync_polarity    = 0;
      cfg.vsync_front_porch = 22;
      cfg.vsync_pulse_width = 13;
      cfg.vsync_back_porch  = 10;
      cfg.pclk_idle_high = 1;

      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl      = GPIO_NUM_2;
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    setPanel(&_panel_instance);
  }
};

static AtomLGFX atomTft;
lgfx::LGFX_Device& tft = atomTft;

// Onboard microSD: SPI SCK=12, MISO=13, MOSI=11, CS=10.
SPIClass atomSdSPI(FSPI);
static bool atomSdMounted = false;

// GPIO0 is I2S BCLK on this board revision. The physical BOOT button is
// therefore not usable as a runtime screenshot input while audio is active.
// V111j restores screenshot capture through the host command ~SHOT.
// V111k moves MAX98357 PCM feeding to a dedicated task so I2S never slows PC2 edges.
#define ATOM_SCREENSHOT_BUTTON_ENABLED 0
static volatile bool atomScreenshotRequested = false;
static const uint32_t ATOM_SCREENSHOT_DEBOUNCE_MS = 40u;

// Onboard MAX98357 I2S amplifier (verified on this board).
#define ATOM_I2S_PORT  I2S_NUM_0
#define ATOM_I2S_BCLK  0
#define ATOM_I2S_LRCLK 18
#define ATOM_I2S_DOUT  17

// Debug hook called from atom_kbd.c when a key is latched/released
extern "C" void atom_kbd_dbg_event(uint8_t event, uint8_t packed, uint8_t row_raw) {
  uint8_t row = (packed >> 3) & 0x1F;
  uint8_t col = packed & 0x07;
  if (event == 1) {
    Serial.printf("[KBD] latch row=%u col=%u row_raw=0x%02X\n", row, col, row_raw);
  } else if (event == 2) {
    Serial.printf("[KBD] release row=%u col=%u row_raw=0x%02X\n", row, col, row_raw);
  }
}

// Debug hook called from atom_kbd.c on each B001 read when a key is being asserted
extern "C" void atom_kbd_dbg_b001(uint8_t row_raw, uint8_t ret, uint8_t state) {
  if (ret != 0xFF) {
    Serial.printf("[B001] row_raw=0x%02X ret=0x%02X state=%u\n", row_raw, ret, state);
  }
}

// ---------------------------------------------------------------------------
// Real Atom speaker output
// ---------------------------------------------------------------------------
// The Atom speaker is 8255 Port-C bit 2. fakemem.c owns the emulated 8255
// latch and calls this hook whenever PC2 changes, including the BSR commands
// written to $B003 by the Kernel BEL routine.
// V086: real-time GPIO26 transition window around the original Invaders FIRE.
//
// We keep a small rolling history of actual physical GPIO26 edges, timestamped
// with micros(). When the firing routine writes $B002, a 150 ms observation
// window is armed: 50 ms before FIRE through 100 ms after FIRE.
//
// The report is printed later from loop(), never from the GPIO hot path.

struct V086AudioEdge {
  uint32_t us;
  uint16_t pc;
  uint8_t before;
  uint8_t after;
};

static const uint16_t V086_EDGE_HISTORY = 192;
static V086AudioEdge v086Edges[V086_EDGE_HISTORY];
static volatile uint16_t v086EdgeHead = 0;
static volatile uint32_t v086FireUs = 0;
static volatile bool v086FireWindowActive = false;
static volatile bool v086FireWindowReady = false;
static portMUX_TYPE v086AudioMux = portMUX_INITIALIZER_UNLOCKED;

static void v086RecordPhysicalEdge(uint16_t pc, uint8_t before, uint8_t after) {
  if (before == after)
    return;

  uint32_t nowUs = micros();

  portENTER_CRITICAL(&v086AudioMux);
  uint16_t i = v086EdgeHead;
  v086Edges[i].us = nowUs;
  v086Edges[i].pc = pc;
  v086Edges[i].before = before;
  v086Edges[i].after = after;
  v086EdgeHead = (uint16_t)((i + 1u) % V086_EDGE_HISTORY);
  portEXIT_CRITICAL(&v086AudioMux);
}

// Current generic Atom PC2 audio state. Declared before debug hooks that read it.
static bool atomAudioHavePc2 = false;
static uint8_t atomAudioPrevPc2 = 0;

extern "C" void atom_v085_fire_b002_read(uint16_t pc, uint8_t value) {
  Serial.printf("[FIRE-READ]  PC=$%04X B002=%02X PC2=%u I2S-PC2=%u\n",
                pc, value, (value & 0x04u) ? 1u : 0u,
                atomAudioPrevPc2 ? 1u : 0u);
}

extern "C" void atom_v085_fire_b002_write(uint16_t pc,
                                           uint8_t oldValue,
                                           uint8_t newValue) {
  Serial.printf("[FIRE-WRITE] PC=$%04X B002 %02X->%02X PC2 %u->%u\n",
                pc, oldValue, newValue,
                (oldValue & 0x04u) ? 1u : 0u,
                (newValue & 0x04u) ? 1u : 0u);

  // Arm only one report at a time. The actual FIRE edge occurs immediately
  // after this hook when atom_sync_speaker_from_portc() calls the audio hook.
  portENTER_CRITICAL(&v086AudioMux);
  if (!v086FireWindowActive && !v086FireWindowReady) {
    v086FireUs = micros();
    v086FireWindowActive = true;
  }
  portEXIT_CRITICAL(&v086AudioMux);
}

// ---------------------------------------------------------------------------
// V120 raw Acorn Atom PC2 -> I2S speaker model
// ---------------------------------------------------------------------------
// V110b/CYD drove the physical audio output from the actual 8255 PC2 waveform.
// Old traces show dense Invaders sound routines toggling PC2 only 18-19 6502
// cycles apart.  The V117 synthetic edge-decay renderer blocked the 6502 for
// hundreds of microseconds per edge and therefore changed those waveforms.
//
// V120 removes that synthetic kick/decay completely.  Each real PC2 transition
// is timestamped in 6502 cycles (1 cycle ~= 1 us at the Atom's 1 MHz clock).
// A separate I2S task reconstructs the resulting 0/1 waveform at 80 kHz and
// passes it through a simple one-pole high-pass (AC-coupling) model.  This is
// deliberately generic: Invaders' lower alien, upper alien, mothership, fire,
// march and player-death sounds remain whatever waveforms the original program
// itself generates.

static volatile bool atomI2sAudioRunning = false;
static const uint32_t ATOM_AUDIO_SAMPLE_RATE = 80000;
static const int16_t ATOM_AUDIO_RAW_LEVEL = 12000;  // conservative MAX98357 level

struct AtomAudioEdgeEvent {
  uint32_t cycle;        // absolute 6502 cycle of this PC2 transition
  uint8_t newLevel;      // PC2 state after this transition
};

// Single producer (6502 task), single consumer (I2S task).
static const uint16_t ATOM_AUDIO_EVENT_RING = 512;
static AtomAudioEdgeEvent atomAudioEventRing[ATOM_AUDIO_EVENT_RING];
static volatile uint16_t atomAudioEventHead = 0;
static volatile uint16_t atomAudioEventTail = 0;
static volatile uint32_t atomAudioEventDrops = 0;
static uint32_t atomAudioLastEdgeCycle = 0;

static inline void atomAudioQueueEdge(uint32_t cycle, uint8_t newLevel) {
  uint16_t head = atomAudioEventHead;
  uint16_t next = (uint16_t)((head + 1u) % ATOM_AUDIO_EVENT_RING);
  if (next == atomAudioEventTail) {
    ++atomAudioEventDrops;
    return;
  }
  atomAudioEventRing[head].cycle = cycle;
  atomAudioEventRing[head].newLevel = newLevel ? 1u : 0u;
  atomAudioEventHead = next;
}

static inline int16_t atomAudioHighPassSample(int16_t input,
                                               int32_t &hpY,
                                               int16_t &hpPrevX) {
  // AC-coupling / small-speaker approximation:
  // y[n] = (63/64) * (y[n-1] + x[n] - x[n-1])
  // At 40 kHz this is roughly a 100 Hz high-pass corner.  Unlike V117 there
  // is no invented per-edge envelope; the filter merely removes DC and gives
  // each genuine PC2 transition the analogue 'floep' character.
  int32_t y = hpY + (int32_t)input - (int32_t)hpPrevX;
  y = (y * 63) / 64;
  if (y >  26000) y =  26000;
  if (y < -26000) y = -26000;
  hpY = y;
  hpPrevX = input;
  return (int16_t)y;
}

static void atomAudioI2sTask(void *pvParameters) {
  (void)pvParameters;

  static const uint16_t FRAMES_PER_BLOCK = 64;
  int16_t frames[FRAMES_PER_BLOCK * 2];
  uint16_t frameCount = 0;

  uint8_t currentLevel = atomAudioPrevPc2 ? 1u : 0u;
  int16_t hpPrevX = currentLevel ? ATOM_AUDIO_RAW_LEVEL : -ATOM_AUDIO_RAW_LEVEL;
  int32_t hpY = 0;

  // Continuous render cursor in absolute 6502 cycles, with sub-sample phase.
  uint32_t renderCycle = clockticks6502;
  uint32_t samplePhase = 0;  // cycles * sample_rate modulo 1,000,000

  auto flushFrames = [&]() {
    if (frameCount == 0) return;
    size_t written = 0;
    i2s_write(ATOM_I2S_PORT, frames,
              (size_t)frameCount * 2u * sizeof(int16_t),
              &written, portMAX_DELAY);
    frameCount = 0;
  };

  auto emitSample = [&](int16_t sample) {
    frames[frameCount * 2] = sample;
    frames[frameCount * 2 + 1] = sample;
    ++frameCount;
    if (frameCount >= FRAMES_PER_BLOCK)
      flushFrames();
  };

  auto renderToCycle = [&](uint32_t targetCycle) {
    uint32_t cycles = targetCycle - renderCycle;

    // For very long idle gaps, don't replay seconds of settled DC later.
    if (cycles > 20000u && hpY <= 2 && hpY >= -2) {
      renderCycle = targetCycle;
      samplePhase = 0;
      hpY = 0;
      hpPrevX = currentLevel ? ATOM_AUDIO_RAW_LEVEL : -ATOM_AUDIO_RAW_LEVEL;
      return;
    }

    samplePhase += cycles * ATOM_AUDIO_SAMPLE_RATE;
    uint32_t samples = samplePhase / 1000000u;
    samplePhase %= 1000000u;

    const int16_t x =
        currentLevel ? ATOM_AUDIO_RAW_LEVEL : -ATOM_AUDIO_RAW_LEVEL;

    for (uint32_t i = 0; i < samples; ++i)
      emitSample(atomAudioHighPassSample(x, hpY, hpPrevX));

    renderCycle = targetCycle;
  };

  while (atomI2sAudioRunning) {
    bool consumedEvent = false;

    // Consume all queued edges in strict 6502-cycle order.
    while (atomAudioEventTail != atomAudioEventHead) {
      consumedEvent = true;
      uint16_t tail = atomAudioEventTail;
      AtomAudioEdgeEvent ev = atomAudioEventRing[tail];
      atomAudioEventTail =
          (uint16_t)((tail + 1u) % ATOM_AUDIO_EVENT_RING);

      renderToCycle(ev.cycle);
      currentLevel = ev.newLevel;
    }

    // Crucial V120 behavior:
    // continue rendering the CURRENT PC2 level up to the current emulated
    // 6502 time, even if no following edge exists.  This makes isolated fire
    // pulses audible without inventing an extra scheduler-dependent sample,
    // and keeps continuous tones independent of queue-drain timing.
    uint32_t nowCycle = clockticks6502;
    renderToCycle(nowCycle);

    if (frameCount >= 32 || (!consumedEvent && frameCount > 0))
      flushFrames();

    if (!consumedEvent) {
      if (hpY <= 2 && hpY >= -2) {
        hpY = 0;
        vTaskDelay(1);
      } else {
        // Let the analogue tail settle using real emulated time, not an
        // arbitrary fixed pulse inserted at a queue boundary.
        taskYIELD();
      }
    }
  }

  flushFrames();
  vTaskDelete(NULL);
}

extern "C" void atom_audio_set_level(uint8_t level) {
  level = level ? 1u : 0u;

  if (!atomAudioHavePc2) {
    atomAudioHavePc2 = true;
    atomAudioPrevPc2 = level;
    atomAudioLastEdgeCycle = clockticks6502;
    return;
  }

  if (level == atomAudioPrevPc2)
    return;

  uint32_t now = clockticks6502;
  atomAudioLastEdgeCycle = now;
  atomAudioPrevPc2 = level;

  if (atomI2sAudioRunning)
    atomAudioQueueEdge(now, level);
}


// ---------------------------------------------------------------------------
// V066 real Atom cassette input
// ---------------------------------------------------------------------------
//
// The Atom Kernel reads cassette data from 8255 Port-C bit 5 and uses the
// continuously-running 2.4 kHz clock on Port-C bit 4.  We therefore do not
// copy .atm payloads directly into emulated RAM.  Instead, a standard .atm
// file is converted at boot into genuine Atom cassette block bytes, and this
// code synthesizes the 1200/2400 Hz waveform that the ROM's own OSBGET
// routine decodes.
//
// .atm format used here:
//   0..15  filename, NUL padded
//   16..17 load address, little endian
//   18..19 execution address, little endian
//   20..21 data length, little endian
//   22..   data
//
// The real cassette block stream is:
//   2 s high-tone leader
//   ****
//   filename + CR
//   status, block number (MSB,LSB), length-1,
//   execution address (MSB,LSB), block load address (MSB,LSB)
//   0.5 s high tone
//   data
//   arithmetic checksum
//
// Each byte is framed as start=0, eight data bits LSB first, stop=1.
// At 300 baud a zero is represented by 1200 Hz and a one by 2400 Hz.

static uint8_t *atomTapeStream = nullptr;
static size_t atomTapeStreamLen = 0;

struct AtomTapeBlockDesc {
  uint32_t start;
  uint32_t headerEnd;  // first data byte
  uint32_t end;        // byte after checksum
};

static AtomTapeBlockDesc atomTapeBlocks[256];
static uint16_t atomTapeBlockCount = 0;
static bool atomCatVirtualActive = false;
static uint16_t atomCatFileCount = 0;
static String atomMountedPath;
static String atomMountedName;

// V109: virtual cassette working directory. All CAT/LOAD/SAVE/MOUNT file
// operations are rooted here and navigation is never allowed above /ATOM/TAPES.
static const char *ATOM_TAPE_ROOT = "/ATOM/TAPES";
static String atomTapeCurrentDir = ATOM_TAPE_ROOT;
static volatile bool atomCatRestorePending = false;
static String atomCatRestorePath;

static bool atomTapeAvailable = false;
static bool atomTapeActive = false;
static bool atomTapeFinished = false;
static bool atomTapeNeedHeaderGap = false;
static uint16_t atomTapePlayBlock = 0;
static uint32_t atomTapeBytePos = 0;
static uint8_t atomTapeBitPos = 0;  // 0=start, 1..8=data, 9=stop
static uint32_t atomTapePhaseStart = 0;

enum AtomTapePhase : uint8_t {
  ATOM_TAPE_IDLE = 0,
  ATOM_TAPE_LEADER,
  ATOM_TAPE_BYTE,
  ATOM_TAPE_HEADER_GAP,
  ATOM_TAPE_DONE
};
static AtomTapePhase atomTapePhase = ATOM_TAPE_IDLE;

// V104 fast SAVE state. Declared here so the CPU governor can accelerate
// the genuine ROM SAVE control flow while OSBPUT physical modulation is bypassed.
static volatile bool atomSaveFastActive = false;

// V072: tell the CPU task when the *input* virtual cassette is genuinely
// being consumed by the Atom ROM. Timing of the cassette waveform remains
// expressed entirely in emulated 6502 cycles.
extern "C" uint8_t atom_tape_turbo_active(void)
{
  bool loadTurbo = (atomTapeActive &&
                    !atomTapeFinished &&
                    atomTapePhase != ATOM_TAPE_IDLE &&
                    atomTapePhase != ATOM_TAPE_DONE);
  return loadTurbo ? 1u : 0u;
}


// ---------------------------------------------------------------------------
// V073 fast SD-backed cassette path
// ---------------------------------------------------------------------------
//
// The genuine Atom ROM still performs:
//   * header/preamble validation
//   * filename reading/comparison
//   * 8-byte cassette header handling
//   * block sequencing/load-address logic
//   * checksum comparison
//   * BASIC LOAD / *LOAD return paths
//
// Only the physical layer is bypassed:
//   OSBGET at $FBEE receives the next already-decoded cassette byte rather
//   than spending ~33 ms measuring 1200/2400-Hz pulses for that byte.
//
// The full waveform synthesizer remains below as a reference/fallback.

extern "C" uint8_t atom_tape_fast_available(void)
{
  return (atomTapeAvailable && !atomTapeFinished &&
          atomTapeBlockCount != 0 && atomTapeStream != nullptr) ? 1u : 0u;
}

extern "C" uint8_t atom_cat_fast_end_pending(void)
{
  return (atomCatVirtualActive &&
          atomTapeActive &&
          atomTapePlayBlock >= atomTapeBlockCount) ? 1u : 0u;
}

static void atomTapeFastStartIfNeeded()
{
  if (atomTapeActive || !atom_tape_fast_available())
    return;

  atomTapeActive = true;
  atomTapeFinished = false;
  atomTapePlayBlock = 0;
  atomTapeBytePos = atomTapeBlocks[0].start;
  atomTapeBitPos = 0;
  atomTapePhase = ATOM_TAPE_BYTE;
  atomTapePhaseStart = clockticks6502;

  Serial.println("[TAPE-FAST] Direct ROM OSBGET byte delivery active");
}

extern "C" uint8_t atom_tape_fast_get_byte(uint8_t *out)
{
  if (!out || !atom_tape_fast_available())
    return 0;

  atomTapeFastStartIfNeeded();

  if (!atomTapeActive || atomTapePlayBlock >= atomTapeBlockCount ||
      atomTapeBytePos >= atomTapeStreamLen)
    return 0;

  *out = atomTapeStream[atomTapeBytePos];

  // Advance exactly one genuine cassette byte.
  atomTapeEnterNextByteOrPhase(clockticks6502);

  /*
   * V073b: ROM-native *CAT consumes only the cassette HEADER of each file.
   * It never calls OSBGET for that block's data/checksum; on a real moving
   * tape those bytes simply pass before the next leader/header arrives.
   *
   * In the V073 direct-byte path there is no independently moving tape, so
   * after the last header byte atomTapeBytePos would otherwise remain parked
   * on the dummy data byte. The next *CAT header search would then read:
   *   dummy data, checksum, '*', '*', ...
   * as though they were a new header.
   *
   * Therefore, only for the synthetic catalogue cassette, jump over the
   * unused data/checksum immediately after delivering a complete header.
   */
  if (atomCatVirtualActive &&
      atomTapeActive &&
      atomTapePhase == ATOM_TAPE_HEADER_GAP) {

    uint16_t finishedBlock = atomTapePlayBlock;
    atomTapePlayBlock++;

    if (atomTapePlayBlock >= atomTapeBlockCount) {
      atomTapeBytePos = atomTapeBlocks[finishedBlock].end;

      /*
       * V073c:
       * Keep the virtual cassette logically active for one final ROM header
       * search. The next pass through $FB8E/$FB9D must be able to see our
       * synthetic CTRL on B001 and return through the genuine CTRL-abort path.
       *
       * V073b marked the tape DONE/inactive here, which also disabled the
       * fast-cassette hooks before the ROM could observe CTRL.
       */
      atomTapePhase = ATOM_TAPE_LEADER;
      atomTapeFinished = false;
      atomTapeActive = true;

      Serial.println("[TAPE] End of virtual catalogue data");
      Serial.println("[CAT] End reached - final ROM header search will return as CTRL-abort");
    } else {
      atomTapeBytePos = atomTapeBlocks[atomTapePlayBlock].start;
      atomTapeBitPos = 0;
      atomTapePhase = ATOM_TAPE_LEADER;
      atomTapePhaseStart = clockticks6502;
    }
  }

  return 1;
}

static const uint32_t ATOM_TAPE_BIT_CYCLES = 3333u;       // ~1/300 s at 1 MHz
static const uint32_t ATOM_TAPE_LEADER_CYCLES = 2100000u; // safely > ROM leader requirement
static const uint32_t ATOM_TAPE_HEADER_GAP_CYCLES = 500000u;
static const uint32_t ATOM_TAPE_HALF_2400 = 208u;
static const uint32_t ATOM_TAPE_HALF_1200 = 416u;

static uint16_t atomTapeReadLe16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static bool atomTapeNameEndsAtm(const char *name) {
  size_t n = strlen(name);
  if (n < 4) return false;
  const char *p = name + n - 4;
  return (p[0] == '.') &&
         (p[1] == 'a' || p[1] == 'A') &&
         (p[2] == 't' || p[2] == 'T') &&
         (p[3] == 'm' || p[3] == 'M');
}

static void atomTapeRewindInternal(bool announce) {
  if (!atomTapeAvailable || atomTapeBlockCount == 0) {
    atomTapeActive = false;
    atomTapeFinished = false;
    atomTapePhase = ATOM_TAPE_IDLE;
    return;
  }

  atomTapeActive = false;     // starts when ROM begins reading tape input
  atomTapeFinished = false;
  atomTapePlayBlock = 0;
  atomTapeBytePos = atomTapeBlocks[0].start;
  atomTapeBitPos = 0;
  atomTapeNeedHeaderGap = false;
  atomTapePhase = ATOM_TAPE_IDLE;
  atomTapePhaseStart = clockticks6502;

  if (announce)
    Serial.println("[TAPE] Rewound; waiting for Atom PLAY TAPE / cassette read");
}

// Called from fakemem.c when the CPU fetches the first instruction of *LOAD.
// This is equivalent to rewinding our single virtual cassette before playback.
extern "C" void atom_tape_rewind(void) {
  atomTapeRewindInternal(false);
}

// V074: clean transient host/emulator state for an Atom BREAK without
// remounting SPIFFS/SD or changing the currently mounted .atm file.
extern "C" void atom_host_break_cleanup(void) {
  atom_kbd_force_ctrl(0);

  // Abort any in-progress virtual tape/CAT operation and rewind the mounted
  // cassette so it remains ready for a later LOAD.
  atomCatVirtualActive = false;
  atomCatRestorePending = false;
  atomCatRestorePath = "";
  atomTapeRewindInternal(false);

  Serial.println("[BREAK] Transient keyboard/tape state cleared");
}

static bool atomTapeBuildFromAtm(const char *path) {
  fs::File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.printf("[TAPE] Cannot open %s\n", path);
    return false;
  }

  size_t fileSize = f.size();
  if (fileSize < 22) {
    Serial.printf("[TAPE] %s too small for ATM header (%u bytes)\n",
                  path, (unsigned)fileSize);
    f.close();
    return false;
  }

  uint8_t hdr[22];
  if (f.read(hdr, sizeof(hdr)) != sizeof(hdr)) {
    Serial.println("[TAPE] ATM header read failed");
    f.close();
    return false;
  }

  char tapeName[14];
  size_t nameLen = 0;
  while (nameLen < 13 && hdr[nameLen] != 0) {
    tapeName[nameLen] = (char)hdr[nameLen];
    nameLen++;
  }
  tapeName[nameLen] = 0;

  if (nameLen == 0) {
    Serial.println("[TAPE] ATM filename is empty");
    f.close();
    return false;
  }

  uint16_t loadAddr = atomTapeReadLe16(hdr + 16);
  uint16_t execAddr = atomTapeReadLe16(hdr + 18);
  uint16_t dataLen  = atomTapeReadLe16(hdr + 20);

  if ((size_t)dataLen + 22u > fileSize) {
    Serial.printf("[TAPE] ATM length mismatch: header=%u file=%u\n",
                  (unsigned)dataLen, (unsigned)fileSize);
    f.close();
    return false;
  }

  uint8_t *payload = (uint8_t *)malloc(dataLen ? dataLen : 1);
  if (!payload) {
    Serial.println("[TAPE] Not enough RAM for ATM payload");
    f.close();
    return false;
  }

  if (dataLen && f.read(payload, dataLen) != dataLen) {
    Serial.println("[TAPE] ATM payload read failed");
    free(payload);
    f.close();
    return false;
  }
  f.close();

  uint16_t blocks = (uint16_t)((dataLen + 255u) / 256u);
  if (blocks == 0) blocks = 1;
  if (blocks > 256) {
    Serial.println("[TAPE] ATM file has too many blocks");
    free(payload);
    return false;
  }

  // 4 '*' + filename+CR + 8 header + data + checksum per block.
  size_t capacity = (size_t)dataLen +
                    (size_t)blocks * (4u + nameLen + 1u + 8u + 1u) + 16u;

  uint8_t *stream = (uint8_t *)malloc(capacity);
  if (!stream) {
    Serial.println("[TAPE] Not enough RAM for cassette stream");
    free(payload);
    return false;
  }

  size_t pos = 0;
  size_t payloadPos = 0;

  for (uint16_t b = 0; b < blocks; ++b) {
    size_t remaining = dataLen - payloadPos;
    uint16_t blockLen = (remaining > 256u) ? 256u : (uint16_t)remaining;
    if (dataLen == 0) blockLen = 1; // defensive; normal ATM files contain data

    bool first = (b == 0);
    bool last  = (b == blocks - 1);

    atomTapeBlocks[b].start = (uint32_t)pos;

    stream[pos++] = 0x2A;
    stream[pos++] = 0x2A;
    stream[pos++] = 0x2A;
    stream[pos++] = 0x2A;

    for (size_t i = 0; i < nameLen; ++i)
      stream[pos++] = (uint8_t)tapeName[i];
    stream[pos++] = 0x0D;

    // Atom block flag: b7=not last, b6=data present, b5=not first.
    uint8_t status = 0x40u;
    if (!last)  status |= 0x80u;
    if (!first) status |= 0x20u;
    stream[pos++] = status;

    // Block number is transmitted MSB first.
    stream[pos++] = (uint8_t)(b >> 8);
    stream[pos++] = (uint8_t)(b & 0xFF);

    // The tape field is the highest zero-based data index: N-1.
    stream[pos++] = (uint8_t)(blockLen - 1u);

    stream[pos++] = (uint8_t)(execAddr >> 8);
    stream[pos++] = (uint8_t)(execAddr & 0xFF);

    uint16_t blockLoad = (uint16_t)(loadAddr + payloadPos);
    stream[pos++] = (uint8_t)(blockLoad >> 8);
    stream[pos++] = (uint8_t)(blockLoad & 0xFF);

    atomTapeBlocks[b].headerEnd = (uint32_t)pos;

    for (uint16_t i = 0; i < blockLen && payloadPos < dataLen; ++i)
      stream[pos++] = payload[payloadPos++];

    // Kernel checksum is the modulo-256 arithmetic sum from the four '*'
    // preamble bytes through the final data byte.
    uint8_t sum = 0;
    for (size_t i = atomTapeBlocks[b].start; i < pos; ++i)
      sum = (uint8_t)(sum + stream[i]);
    stream[pos++] = sum;

    atomTapeBlocks[b].end = (uint32_t)pos;
  }

  free(payload);

  if (atomTapeStream) free(atomTapeStream);
  atomTapeStream = stream;
  atomTapeStreamLen = pos;
  atomTapeBlockCount = blocks;
  atomTapeAvailable = true;

  Serial.printf("[TAPE] Mounted ATM: %s\n", path);
  Serial.printf("[TAPE] Name=\"%s\" load=$%04X exec=$%04X length=%u blocks=%u\n",
                tapeName, loadAddr, execAddr, (unsigned)dataLen, (unsigned)blocks);
  Serial.printf("[TAPE] Real cassette stream: %u bytes before FSK framing\n",
                (unsigned)atomTapeStreamLen);

  atomMountedPath = String(path);
  atomMountedName = String(tapeName);

  atomTapeRewindInternal(true);
  return true;
}



static String atomCosCommandBuffer();

// V109 path helpers ---------------------------------------------------------
static String atomTapeJoinCurrent(const String &name) {
  if (atomTapeCurrentDir.endsWith("/")) return atomTapeCurrentDir + name;
  return atomTapeCurrentDir + "/" + name;
}

static String atomTapeEntryBaseName(const String &rawName) {
  String n = rawName;
  while (n.endsWith("/")) n.remove(n.length() - 1);
  int slash = n.lastIndexOf('/');
  if (slash >= 0) n = n.substring(slash + 1);
  return n;
}

static bool atomTapeSelectChildDir(const String &wantedRaw) {
  String wanted = wantedRaw;
  wanted.trim();
  if (wanted.length() >= 2 && wanted[0] == '"' && wanted[wanted.length()-1] == '"') {
    wanted = wanted.substring(1, wanted.length()-1);
    wanted.trim();
  }
  if (!wanted.length()) return true;

  if (wanted == "..") {
    if (!atomTapeCurrentDir.equalsIgnoreCase(ATOM_TAPE_ROOT)) {
      int slash = atomTapeCurrentDir.lastIndexOf('/');
      if (slash >= (int)strlen(ATOM_TAPE_ROOT)) atomTapeCurrentDir.remove(slash);
      if (atomTapeCurrentDir.length() < strlen(ATOM_TAPE_ROOT)) atomTapeCurrentDir = ATOM_TAPE_ROOT;
    }
    Serial.printf("[CAT] Directory: %s\n", atomTapeCurrentDir.c_str());
    return true;
  }

  fs::File dir = SD.open(atomTapeCurrentDir.c_str());
  if (!dir || !dir.isDirectory()) return false;
  fs::File entry = dir.openNextFile();
  while (entry) {
    if (entry.isDirectory()) {
      String raw(entry.name());
      String base = atomTapeEntryBaseName(raw);
      if (base.equalsIgnoreCase(wanted)) {
        String path = atomTapeJoinCurrent(base);
        entry.close(); dir.close();
        // Canonicalise trailing slash only; child traversal means path is always under root.
        while (path.endsWith("/")) path.remove(path.length()-1);
        atomTapeCurrentDir = path;
        Serial.printf("[CAT] Directory: %s\n", atomTapeCurrentDir.c_str());
        return true;
      }
    }
    entry = dir.openNextFile();
  }
  dir.close();
  Serial.printf("[CAT] Directory not found: %s (staying in %s)\n",
                wanted.c_str(), atomTapeCurrentDir.c_str());
  return false;
}

static void atomCatApplyDirectoryArgument(void) {
  String cmd = atomCosCommandBuffer();
  String upper = cmd; upper.toUpperCase();
  if (!upper.startsWith("CAT")) return;
  if (cmd.length() <= 3) return;
  String arg = cmd.substring(3); arg.trim();
  if (!arg.length()) return;
  atomTapeSelectChildDir(arg);
}

// V105b: automatic named LOAD mounting at the genuine OSLOAD interface.
// X points to the documented zero-page OSLOAD control block; X+0 is a
// pointer to the CR-terminated Atom filename.  A matching ATM is selected
// from /ATOM/TAPES before the unmodified Kernel LOAD routine continues.
// If no matching SD file exists, the currently mounted virtual cassette is
// left untouched so normal/manual *MOUNT behaviour remains available.
extern "C" uint8_t atom_tape_auto_mount_load(uint8_t x) {
  uint8_t x1 = (uint8_t)(x + 1u);
  uint16_t namePtr = (uint16_t)fakemem[x] | ((uint16_t)fakemem[x1] << 8);

  char requested[17] = {0};
  size_t n = 0;
  while (n < 16) {
    uint8_t c = fakemem[(uint16_t)(namePtr + n)];
    if (c == 0x0D || c == 0x00) break;
    if (c < 0x20 || c > 0x7E) {
      // Not a sensible named OSLOAD request; retain mounted-tape behaviour.
      return 0u;
    }
    requested[n++] = (char)c;
  }
  requested[n] = 0;
  if (n == 0) return 0u;

  // If the requested Atom filename is already mounted, no SD scan is needed.
  String currentPrefix = atomTapeCurrentDir + "/";
  if (atomMountedName.length() && atomMountedName.equalsIgnoreCase(requested) &&
      atomMountedPath.startsWith(currentPrefix)) {
    Serial.printf("[AUTO-MOUNT] %s already mounted in current directory\n", requested);
    return 1u;
  }

  fs::File dir = SD.open(atomTapeCurrentDir.c_str());
  if (!dir || !dir.isDirectory()) {
    Serial.printf("[AUTO-MOUNT] %s unavailable; using current cassette\n", atomTapeCurrentDir.c_str());
    return 0u;
  }

  String wanted(requested);
  fs::File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory() && atomTapeNameEndsAtm(entry.name())) {
      String rawName(entry.name());
      String baseName = rawName;
      int slash = baseName.lastIndexOf('/');
      if (slash >= 0) baseName = baseName.substring(slash + 1);
      if (baseName.length() >= 4) baseName.remove(baseName.length() - 4);

      bool match = baseName.equalsIgnoreCase(wanted);

      // Prefer the Atom filename stored in the ATM header.  This keeps LOAD
      // consistent with *CAT even if the host SD filename has been renamed.
      if (!match) {
        uint8_t hdr[16] = {0};
        size_t oldPos = entry.position();
        entry.seek(0);
        if (entry.read(hdr, sizeof(hdr)) == sizeof(hdr)) {
          char atmName[17] = {0};
          size_t m = 0;
          while (m < 16 && hdr[m] != 0) {
            atmName[m] = (char)hdr[m];
            m++;
          }
          atmName[m] = 0;
          match = String(atmName).equalsIgnoreCase(wanted);
        }
        entry.seek(oldPos);
      }

      if (match) {
        String fullPath = atomTapeJoinCurrent(atomTapeEntryBaseName(rawName));
        entry.close();
        dir.close();

        atomCatVirtualActive = false;
        atom_kbd_force_ctrl(0);
        if (atomTapeBuildFromAtm(fullPath.c_str())) {
          Serial.printf("[AUTO-MOUNT] LOAD \"%s\" -> %s\n",
                        requested, fullPath.c_str());
          return 1u;
        }

        Serial.printf("[AUTO-MOUNT] Found %s but mount failed; using current cassette\n",
                      requested);
        return 0u;
      }
    }
    entry = dir.openNextFile();
  }
  dir.close();

  Serial.printf("[AUTO-MOUNT] No SD match for LOAD \"%s\"; using current cassette\n",
                requested);
  return 0u;
}


// V068d: build a synthetic cassette consisting of one valid first-block
// header per .atm file. The genuine ROM *CAT at $FA2A reads this through
// the existing PC5 cassette waveform and prints the names itself.
// V109a: catalogue scratch table is static rather than living on the
// 4 KB cpu_task stack.  V109's local CatItem[96] overflowed the task stack
// as soon as *CAT was executed.
struct AtomCatItem {
  String name;
  uint16_t loadAddr;
  uint16_t execAddr;
  uint16_t dataLen;
  bool isDir;
};

static const int ATOM_CAT_MAX = 96;
static AtomCatItem atomCatItems[ATOM_CAT_MAX];

static bool atomCatBuildVirtualTape() {
  AtomCatItem *items = atomCatItems;
  int count = 0;

  fs::File dir = SD.open(atomTapeCurrentDir.c_str());
  if (!dir || !dir.isDirectory()) {
    Serial.printf("[CAT] %s unavailable\n", atomTapeCurrentDir.c_str());
    return false;
  }

  fs::File entry = dir.openNextFile();
  while (entry && count < ATOM_CAT_MAX) {
    String raw(entry.name());
    if (entry.isDirectory()) {
      String base = atomTapeEntryBaseName(raw);
      if (base.length()) {
        String shown = base;
        if (shown.length() > 11) shown.remove(11); // Atom cassette name field <=13 incl <>
        items[count].name = String("<") + shown + ">";
        items[count].loadAddr = 0;
        items[count].execAddr = 0;
        items[count].dataLen = 1;
        items[count].isDir = true;
        count++;
      }
    } else if (atomTapeNameEndsAtm(entry.name())) {
      String path = atomTapeJoinCurrent(atomTapeEntryBaseName(raw));
      fs::File f = SD.open(path.c_str(), FILE_READ);
      if (f) {
        uint8_t hdr[22];
        if (f.read(hdr, sizeof(hdr)) == sizeof(hdr)) {
          char nm[14]; size_t nl=0;
          while (nl < 13 && hdr[nl] != 0) { nm[nl]=(char)hdr[nl]; nl++; }
          nm[nl]=0;
          if (nl) {
            items[count].name=String(nm);
            items[count].loadAddr=atomTapeReadLe16(hdr+16);
            items[count].execAddr=atomTapeReadLe16(hdr+18);
            items[count].dataLen=atomTapeReadLe16(hdr+20);
            items[count].isDir=false;
            count++;
          }
        }
        f.close();
      }
    }
    entry = dir.openNextFile();
  }
  dir.close();

  if (!count) {
    Serial.printf("[CAT] No ATM files or directories in %s\n", atomTapeCurrentDir.c_str());
    return false;
  }

  // Alphabetical order, ignoring the visual '<' on directories.
  for (int i=1;i<count;i++) {
    AtomCatItem x=items[i]; int j=i-1;
    String xn=x.name; if (x.isDir && xn.length()>2) xn=xn.substring(1,xn.length()-1);
    xn.toUpperCase();
    while (j>=0) {
      String jn=items[j].name;
      if (items[j].isDir && jn.length()>2) jn=jn.substring(1,jn.length()-1);
      jn.toUpperCase();
      if (jn.compareTo(xn)<=0) break;
      items[j+1]=items[j]; j--;
    }
    items[j+1]=x;
  }

  size_t cap=0;
  for (int i=0;i<count;i++) cap += 4u+items[i].name.length()+1u+8u+1u+1u;
  uint8_t *stream=(uint8_t*)malloc(cap);
  if (!stream) { Serial.println("[CAT] Not enough RAM for virtual catalogue"); return false; }

  size_t pos=0;
  for (int i=0;i<count;i++) {
    atomTapeBlocks[i].start=(uint32_t)pos;
    for (int k=0;k<4;k++) stream[pos++]=0x2A;
    for (size_t k=0;k<items[i].name.length();k++) stream[pos++]=(uint8_t)items[i].name[k];
    stream[pos++]=0x0D;
    stream[pos++]=0x40; stream[pos++]=0; stream[pos++]=0;
    uint16_t firstBlockLen=items[i].dataLen>256u?256u:items[i].dataLen;
    if (!firstBlockLen) firstBlockLen=1;
    stream[pos++]=(uint8_t)(firstBlockLen-1u);
    stream[pos++]=(uint8_t)(items[i].execAddr>>8); stream[pos++]=(uint8_t)items[i].execAddr;
    stream[pos++]=(uint8_t)(items[i].loadAddr>>8); stream[pos++]=(uint8_t)items[i].loadAddr;
    atomTapeBlocks[i].headerEnd=(uint32_t)pos;
    stream[pos++]=0;
    uint8_t sum=0; for (size_t k=atomTapeBlocks[i].start;k<pos;k++) sum=(uint8_t)(sum+stream[k]);
    stream[pos++]=sum; atomTapeBlocks[i].end=(uint32_t)pos;
    if (items[i].isDir)
      Serial.printf("[CAT] %2d %s directory\n",i+1,items[i].name.c_str());
    else
      Serial.printf("[CAT] %2d %s load=$%04X exec=$%04X len=%u\n",i+1,items[i].name.c_str(),items[i].loadAddr,items[i].execAddr,(unsigned)items[i].dataLen);
  }

  if (atomTapeStream) free(atomTapeStream);
  atomTapeStream=stream; atomTapeStreamLen=pos; atomTapeBlockCount=(uint16_t)count;
  atomTapeAvailable=true; atomCatFileCount=(uint16_t)count; atomCatVirtualActive=true;
  atomTapeRewindInternal(false);
  Serial.printf("[CAT] %s: %u entries, %u bytes virtual cassette\n",
                atomTapeCurrentDir.c_str(),(unsigned)count,(unsigned)atomTapeStreamLen);
  return true;
}

extern "C" void atom_kbd_force_ctrl(uint8_t on);

static String atomCosCommandBuffer() {
  String s;
  for (int i = 0; i < 80; ++i) {
    uint8_t c = fakemem[0x0100 + i];
    if (c == 0x0D || c == 0x00) break;
    if (c >= 0x20 && c <= 0x7E) s += (char)c;
  }
  s.trim();
  return s;
}

// V109d: preprocess only the emulator extension "*CAT <directory>" at the
// documented OSCLI entry ($FFF7), before the genuine Atom ROM parses it.
//
// The ROM itself understands plain *CAT but rejects trailing arguments.  We
// therefore:
//   1. inspect the complete OSCLI command in the normal $0100 buffer,
//   2. change the emulator's current tape directory,
//   3. rewrite the buffer to plain "CAT" + CR,
//   4. let the unmodified ROM OSCLI/CAT code continue normally.
//
// No PC redirection is used and LOAD/SAVE paths are untouched.
extern "C" void atom_cat_preprocess_oscli(void) {
  // V116: AtoMMC2 now owns *CAT path/wildcard parsing.  The old V109
  // cassette-directory extension must not rewrite the OSCLI command buffer.
  return;
  String cmd = atomCosCommandBuffer();
  String upper = cmd;
  upper.toUpperCase();

  if (!upper.startsWith("CAT"))
    return;

  // It must be exactly CAT followed by whitespace and an argument.
  if (cmd.length() <= 3)
    return;

  char sep = cmd[3];
  if (sep != ' ' && sep != '\t')
    return;

  String arg = cmd.substring(4);
  arg.trim();
  if (!arg.length())
    return;

  bool changed = atomTapeSelectChildDir(arg);
  if (!changed) {
    Serial.printf("[CAT] Directory unchanged: %s\n",
                  atomTapeCurrentDir.c_str());
  }

  // V109e: do NOT shorten the OSCLI command buffer.  The Atom command
  // interpreter retains its parse position and expects the statement
  // terminator at the original location.  V109d moved CR to offset 3, which
  // allowed CAT to run but caused ERROR 94 after the catalogue completed.
  //
  // Preserve the original CR position and simply blank the argument to
  // spaces.  The genuine ROM therefore sees "CAT        <CR>" with exactly
  // the same command-buffer geometry it was given on entry.
  int crPos = -1;
  for (int i = 3; i < 80; ++i) {
    if (fakemem[0x0100 + i] == 0x0D || fakemem[0x0100 + i] == 0x00) {
      crPos = i;
      break;
    }
  }
  if (crPos < 0) crPos = 79;

  for (int i = 3; i < crPos; ++i)
    fakemem[0x0100 + i] = ' ';
  fakemem[0x0100 + crPos] = 0x0D;

  Serial.printf("[CAT] OSCLI directory catalogue: %s\n",
                atomTapeCurrentDir.c_str());
}

// Emulator-only command. It is intentionally silent on the Atom screen.
// Success/failure is reported on Serial; LOAD/SAVE remain genuine Atom BASIC.
extern "C" uint8_t atom_mount_try_command(void) {
  String cmd = atomCosCommandBuffer();
  String upper = cmd;
  upper.toUpperCase();

  if (!upper.startsWith("MOUNT"))
    return 0;

  String wanted = cmd.substring(5);
  wanted.trim();
  if (wanted.length() >= 2 && wanted[0] == '"' &&
      wanted[wanted.length() - 1] == '"') {
    wanted = wanted.substring(1, wanted.length() - 1);
    wanted.trim();
  }
  if (wanted.length() >= 4 &&
      wanted.substring(wanted.length() - 4).equalsIgnoreCase(".atm"))
    wanted.remove(wanted.length() - 4);

  if (wanted.length() == 0) {
    Serial.println("[MOUNT] Usage: *MOUNT filename");
    return 1;
  }

  fs::File dir = SD.open(atomTapeCurrentDir.c_str());
  if (!dir || !dir.isDirectory()) {
    Serial.printf("[MOUNT] %s unavailable\n", atomTapeCurrentDir.c_str());
    return 1;
  }

  fs::File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory() && atomTapeNameEndsAtm(entry.name())) {
      String entryName(entry.name());
      int slash = entryName.lastIndexOf('/');
      if (slash >= 0) entryName = entryName.substring(slash + 1);
      if (entryName.length() >= 4)
        entryName.remove(entryName.length() - 4);

      if (entryName.equalsIgnoreCase(wanted)) {
        String rawName(entry.name());
        String fullPath = atomTapeJoinCurrent(atomTapeEntryBaseName(rawName));
        entry.close();
        dir.close();

        atomCatVirtualActive = false;
        atom_kbd_force_ctrl(0);

        if (atomTapeBuildFromAtm(fullPath.c_str())) {
          Serial.printf("[MOUNT] Mounted %s\\n", entryName.c_str());
        } else {
          Serial.printf("[MOUNT] Failed to mount %s\\n", entryName.c_str());
        }
        return 1;
      }
    }
    entry = dir.openNextFile();
  }
  dir.close();

  Serial.printf("[MOUNT] Not found: %s\\n", wanted.c_str());
  return 1;
}

// Called when the genuine ROM *CAT takes its CTRL-abort return path.
extern "C" void atom_cat_rom_exit(void) {
  if (!atomCatVirtualActive) return;

  atomCatVirtualActive = false;
  atom_kbd_force_ctrl(0);  // harmless if no synthetic CTRL was asserted

  // V073d: finalise the direct-byte catalogue tape only after the ROM has
  // actually taken its CTRL-abort return path.
  atomTapePhase = ATOM_TAPE_DONE;
  atomTapeFinished = true;
  atomTapeActive = false;
  Serial.println("[CAT] Genuine ROM CTRL exit reached; catalogue tape closed");

  /*
   * V068f: DO NOT rebuild/free atomTapeStream here.
   *
   * We arrive here while the genuine ROM *CAT / cassette read call chain is
   * still unwinding. V068e replaced atomTapeStream at this point, causing a
   * use-after-free and heap poisoning. Merely remember what needs restoring.
   * loop() performs the restoration later, once the CPU is safely outside
   * the cassette ROM code.
   */
  atomCatRestorePending = false;
  atomCatRestorePath = atomMountedPath;
  if (atomCatRestorePath.length())
    atomCatRestorePending = true;

  Serial.println("[CAT] End of SD catalogue - automatic CTRL released");
  if (atomCatRestorePending)
    Serial.printf("[CAT] Cassette restore deferred: %s\n",
                  atomCatRestorePath.c_str());
}

static void atomCatProcessDeferredRestore() {
  if (!atomCatRestorePending) return;

  // Do not touch the cassette buffer while the ROM is still anywhere in
  // cassette/COS code. Wait until execution has returned to BASIC/direct mode.
  uint16_t pc = *fake6502_pc;
  if ((pc >= 0xF8E0u && pc <= 0xFCE9u) ||
      (pc >= 0xFA00u && pc <= 0xFCFFu))
    return;

  String restorePath = atomCatRestorePath;
  atomCatRestorePending = false;
  atomCatRestorePath = "";

  if (atomTapeBuildFromAtm(restorePath.c_str())) {
    Serial.printf("[CAT] Restored mounted cassette %s\n",
                  atomMountedName.c_str());
  } else {
    Serial.printf("[CAT] Failed to restore cassette %s\n",
                  restorePath.c_str());
  }
}

extern "C" void atom_cat_rom_enter(void) {
  if (!atomCatVirtualActive) {
    atomCatApplyDirectoryArgument();
    if (atomCatBuildVirtualTape())
      Serial.println("[CAT] Genuine Atom ROM *CAT running");
  }
}

static void atomTapeFindFirstAtm() {
  atomTapeAvailable = false;

  fs::File dir = SD.open(atomTapeCurrentDir.c_str());
  if (!dir || !dir.isDirectory()) {
    Serial.printf("[TAPE] %s unavailable\n", atomTapeCurrentDir.c_str());
    return;
  }

  fs::File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      const char *name = entry.name();
      if (atomTapeNameEndsAtm(name)) {
        String fullPath;
        fullPath = atomTapeJoinCurrent(atomTapeEntryBaseName(String(name)));
        entry.close();
        atomTapeBuildFromAtm(fullPath.c_str());
        return;
      }
    }
    entry = dir.openNextFile();
  }

  Serial.printf("[TAPE] No .atm file found in %s\n", atomTapeCurrentDir.c_str());
}

static uint8_t atomTapeCurrentBitValue() {
  if (atomTapeBitPos == 0) return 0; // start bit
  if (atomTapeBitPos == 9) return 1; // stop bit

  uint8_t value = atomTapeStream[atomTapeBytePos];
  return (value >> (atomTapeBitPos - 1u)) & 1u;
}

static void atomTapeEnterNextByteOrPhase(uint32_t nowCycles) {
  AtomTapeBlockDesc &d = atomTapeBlocks[atomTapePlayBlock];

  atomTapeBytePos++;

  // The real SAVE routine leaves 0.5 s of high tone between header and data.
  if (atomTapeBytePos == d.headerEnd) {
    atomTapePhase = ATOM_TAPE_HEADER_GAP;
    atomTapePhaseStart = nowCycles;
    return;
  }

  if (atomTapeBytePos >= d.end) {
    atomTapePlayBlock++;
    if (atomTapePlayBlock >= atomTapeBlockCount) {
      atomTapePhase = ATOM_TAPE_DONE;
      atomTapeFinished = true;
      atomTapeActive = false;
      Serial.println("[TAPE] End of virtual cassette");

      // V068e: the genuine Atom *CAT otherwise searches forever for another
      // header. Assert the real active-low CTRL input so FB8E returns through
      // the ROM's normal CTRL-abort path.
      if (atomCatVirtualActive) {
        atom_kbd_force_ctrl(1);
        Serial.println("[CAT] End reached - asserting automatic CTRL");
      }
      return;
    }

    atomTapeBytePos = atomTapeBlocks[atomTapePlayBlock].start;
    atomTapeBitPos = 0;
    atomTapePhase = ATOM_TAPE_LEADER;
    atomTapePhaseStart = nowCycles;
    return;
  }

  atomTapeBitPos = 0;
  atomTapePhase = ATOM_TAPE_BYTE;
  atomTapePhaseStart = nowCycles;
}

static void atomTapeAdvance(uint32_t nowCycles) {
  // Advance whole completed phases. Usually zero or one iteration per B002 read.
  for (int guard = 0; guard < 8; ++guard) {
    uint32_t elapsed = nowCycles - atomTapePhaseStart;

    if (atomTapePhase == ATOM_TAPE_LEADER) {
      if (elapsed < ATOM_TAPE_LEADER_CYCLES) return;
      atomTapePhaseStart += ATOM_TAPE_LEADER_CYCLES;
      atomTapePhase = ATOM_TAPE_BYTE;
      atomTapeBitPos = 0;
      continue;
    }

    if (atomTapePhase == ATOM_TAPE_HEADER_GAP) {
      if (elapsed < ATOM_TAPE_HEADER_GAP_CYCLES) return;
      atomTapePhaseStart += ATOM_TAPE_HEADER_GAP_CYCLES;
      atomTapePhase = ATOM_TAPE_BYTE;
      atomTapeBitPos = 0;
      continue;
    }

    if (atomTapePhase == ATOM_TAPE_BYTE) {
      if (elapsed < ATOM_TAPE_BIT_CYCLES) return;
      atomTapePhaseStart += ATOM_TAPE_BIT_CYCLES;

      if (atomTapeBitPos < 9) {
        atomTapeBitPos++;
      } else {
        atomTapeEnterNextByteOrPhase(atomTapePhaseStart);
      }
      continue;
    }

    return;
  }
}

// Port-C bit 4: the Atom's free-running ~2.4 kHz clock input.
extern "C" uint8_t atom_tape_clock_level(void) {
  return (uint8_t)((clockticks6502 / ATOM_TAPE_HALF_2400) & 1u);
}

// Port-C bit 5: cassette input waveform.
extern "C" uint8_t atom_tape_input_level(void) {
  if (!atomTapeAvailable || atomTapeFinished)
    return 0;

  // Do not let ordinary VDG/keyboard reads consume the tape. Start only once
  // the Kernel is executing its cassette receive/leader routines.
  uint16_t pc = *fake6502_pc;
  bool inCassetteReader = (pc >= 0xFB8Eu && pc <= 0xFCD7u);

  if (!atomTapeActive) {
    if (!inCassetteReader)
      return 0;

    atomTapeActive = true;
    atomTapePhase = ATOM_TAPE_LEADER;
    atomTapePlayBlock = 0;
    atomTapeBytePos = atomTapeBlocks[0].start;
    atomTapeBitPos = 0;
    atomTapePhaseStart = clockticks6502;
    Serial.println("[TAPE] PLAY: synthesizing real 300-baud Atom cassette signal");
  }

  atomTapeAdvance(clockticks6502);

  if (atomTapePhase == ATOM_TAPE_DONE || atomTapePhase == ATOM_TAPE_IDLE)
    return 0;

  uint32_t elapsed = clockticks6502 - atomTapePhaseStart;

  if (atomTapePhase == ATOM_TAPE_LEADER ||
      atomTapePhase == ATOM_TAPE_HEADER_GAP) {
    return (uint8_t)((elapsed / ATOM_TAPE_HALF_2400) & 1u);
  }

  uint8_t bit = atomTapeCurrentBitValue();
  uint32_t halfPeriod = bit ? ATOM_TAPE_HALF_2400 : ATOM_TAPE_HALF_1200;
  return (uint8_t)((elapsed / halfPeriod) & 1u);
}



// ---------------------------------------------------------------------------
// V067 real Atom cassette output capture -> .atm on SD
// ---------------------------------------------------------------------------
//
// This deliberately captures the signal at the emulated 8255 cassette
// interface, not at BASIC or OSSAVE.  The Kernel's own OSBPUT routine still
// performs all serial framing and toggles Port C exactly as on a real Atom.
//
// In OSBPUT:
//   PC1=1 enables the 2.4 kHz clock -> a logical 1 bit
//   PC1=0 and PC0 is toggled at half rate -> a logical 0 bit
//
// The hook in fakemem.c reports the writes generated by those two paths.
// We recognize the first port write of each 300-baud bit and reconstruct the
// byte stream.  Once the final cassette block checksum is received, the main
// Arduino loop converts the captured genuine Atom block stream to .atm.

static const size_t ATOM_SAVE_CAPTURE_MAX = 65536u;
static uint8_t *atomSaveBytes = nullptr;
static size_t atomSaveByteCount = 0;
static bool atomSaveCapturing = false;
static bool atomSaveReady = false;
static bool atomSaveFrameActive = false;
static uint8_t atomSaveFrameBit = 0;
static uint8_t atomSaveFrameByte = 0;
static uint32_t atomSaveLastAcceptedBitCycle = 0;
static bool atomSaveHaveBitTime = false;
static uint32_t atomSaveFrameErrors = 0;
static uint32_t atomSaveAcceptedBits = 0;
static uint32_t atomSaveCandidateOnes = 0;
static uint32_t atomSaveCandidateZeros = 0;

// Incremental cassette-block parser, used only to know when the ROM has sent
// the checksum of the final block.  The complete captured bytes are retained
// for the final ATM conversion and validation.
enum AtomSaveParseState : uint8_t {
  AS_WAIT_STAR1 = 0, AS_WAIT_STAR2, AS_WAIT_STAR3, AS_WAIT_STAR4,
  AS_NAME, AS_HEADER, AS_DATA, AS_CHECKSUM
};
static AtomSaveParseState atomSaveParseState = AS_WAIT_STAR1;
static uint8_t atomSaveHeader[8];
static uint8_t atomSaveHeaderPos = 0;
static uint16_t atomSaveDataRemaining = 0;
static bool atomSaveLastBlock = false;

static void atomSaveResetParser() {
  atomSaveParseState = AS_WAIT_STAR1;
  atomSaveHeaderPos = 0;
  atomSaveDataRemaining = 0;
  atomSaveLastBlock = false;
}

extern "C" void atom_tape_save_begin(void) {
  if (!atomSaveBytes) {
    atomSaveBytes = (uint8_t *)malloc(ATOM_SAVE_CAPTURE_MAX);
    if (!atomSaveBytes) {
      Serial.println("[TAPE-SAVE] Cannot allocate capture buffer");
      atomSaveCapturing = false;
      atomSaveFastActive = false;
      return;
    }
  }

  atomSaveByteCount = 0;
  atomSaveCapturing = true;
  atomSaveReady = false;
  atomSaveFrameActive = false;
  atomSaveFrameBit = 0;
  atomSaveFrameByte = 0;
  atomSaveHaveBitTime = false;
  atomSaveFrameErrors = 0;
  atomSaveAcceptedBits = 0;
  atomSaveCandidateOnes = 0;
  atomSaveCandidateZeros = 0;
  atomSaveResetParser();
  atomSaveFastActive = true;
  Serial.println("[TAPE-SAVE] OSSAVE started; Turbo SAVE direct OSBPUT active");
}

static void atomSaveParseByte(uint8_t b) {
  switch (atomSaveParseState) {
    case AS_WAIT_STAR1:
      if (b == 0x2A) atomSaveParseState = AS_WAIT_STAR2;
      break;
    case AS_WAIT_STAR2:
      atomSaveParseState = (b == 0x2A) ? AS_WAIT_STAR3 : AS_WAIT_STAR1;
      break;
    case AS_WAIT_STAR3:
      atomSaveParseState = (b == 0x2A) ? AS_WAIT_STAR4 : AS_WAIT_STAR1;
      break;
    case AS_WAIT_STAR4:
      if (b == 0x2A) atomSaveParseState = AS_NAME;
      else atomSaveParseState = AS_WAIT_STAR1;
      break;
    case AS_NAME:
      if (b == 0x0D) {
        atomSaveHeaderPos = 0;
        atomSaveParseState = AS_HEADER;
      }
      break;
    case AS_HEADER:
      atomSaveHeader[atomSaveHeaderPos++] = b;
      if (atomSaveHeaderPos == 8) {
        atomSaveLastBlock = (atomSaveHeader[0] & 0x80u) == 0;
        atomSaveDataRemaining = (uint16_t)atomSaveHeader[3] + 1u;
        atomSaveParseState = atomSaveDataRemaining ? AS_DATA : AS_CHECKSUM;
      }
      break;
    case AS_DATA:
      if (atomSaveDataRemaining) atomSaveDataRemaining--;
      if (atomSaveDataRemaining == 0) atomSaveParseState = AS_CHECKSUM;
      break;
    case AS_CHECKSUM:
      if (atomSaveLastBlock) {
        atomSaveCapturing = false;
        atomSaveFastActive = false;
        atomSaveReady = true;
        Serial.printf("[TAPE-SAVE] Captured final block: bytes=%u bits=%u ones=%u zeros=%u frameErrors=%u\n",
                      (unsigned)atomSaveByteCount,
                      (unsigned)atomSaveAcceptedBits,
                      (unsigned)atomSaveCandidateOnes,
                      (unsigned)atomSaveCandidateZeros,
                      (unsigned)atomSaveFrameErrors);
      } else {
        atomSaveResetParser();
      }
      break;
  }
}

static void atomSaveAcceptBit(uint8_t bit) {
  atomSaveAcceptedBits++;
  // Search for a valid start bit when idle.
  if (!atomSaveFrameActive) {
    if (bit != 0) return;
    atomSaveFrameActive = true;
    atomSaveFrameBit = 0;
    atomSaveFrameByte = 0;
    return;
  }

  // frameBit 0..7 = eight data bits, LSB first; frameBit 8 = stop bit.
  if (atomSaveFrameBit < 8) {
    if (bit) atomSaveFrameByte |= (uint8_t)(1u << atomSaveFrameBit);
    atomSaveFrameBit++;
    return;
  }

  // Stop bit must be 1. If not, drop framing and reacquire a start bit.
  if (bit != 1) {
    atomSaveFrameErrors++;
    atomSaveFrameActive = false;
    if (bit == 0) {
      atomSaveFrameActive = true;
      atomSaveFrameBit = 0;
      atomSaveFrameByte = 0;
    }
    return;
  }

  atomSaveFrameActive = false;
  if (atomSaveByteCount >= ATOM_SAVE_CAPTURE_MAX) {
    atomSaveCapturing = false;
    atomSaveFastActive = false;
    Serial.println("[TAPE-SAVE] Capture overflow; SAVE aborted");
    return;
  }

  atomSaveBytes[atomSaveByteCount++] = atomSaveFrameByte;
  atomSaveParseByte(atomSaveFrameByte);
}

// V104: fast SAVE counterpart of V073 direct OSBGET delivery.
// Called only when the CPU fetches the genuine cassette OSBPUT entry at $FC7C.
// The higher-level ROM has already constructed the exact byte stream (preamble,
// filename, header, data and checksum), so feed that byte to the same validated
// parser used by the physical Port-C capture and skip only the modulation layer.
extern "C" uint8_t atom_tape_fast_save_put(uint8_t b) {
  if (!atomSaveCapturing || !atomSaveFastActive) return 0u;

  if (atomSaveByteCount >= ATOM_SAVE_CAPTURE_MAX) {
    atomSaveCapturing = false;
    atomSaveFastActive = false;
    Serial.println("[TAPE-SAVE] Capture overflow; SAVE aborted");
    return 0u;
  }

  atomSaveBytes[atomSaveByteCount++] = b;
  atomSaveParseByte(b);
  return 1u;
}

extern "C" void atom_tape_output_portc(uint8_t value, uint16_t nextPc) {
  // V104 direct OSBPUT normally bypasses this physical path. Keep the V067
  // decoder intact as a fallback/reference path.
  if (!atomSaveCapturing || atomSaveFastActive) return;

  /*
   * Deterministic decoding of the ROM's real cassette-output hardware writes.
   *
   * Logical 1:
   *   FC94 STA $B002 with value $07, then PC=$FC97.
   *   There is exactly one such write per serial bit.
   *
   * Logical 0:
   *   FCA0 STA $B002 with value $04, then PC=$FCA3.
   *   The ROM repeats this four times to make four 1.2-kHz cycles.
   *   Y starts at 4 and is decremented after each cycle, so Y==4 marks
   *   precisely the FIRST write of the serial bit.  No timing heuristic
   *   is required.
   */
  if (nextPc == 0xFC97u && (value & 0x03u) == 0x03u) {
    atomSaveCandidateOnes++;
    atomSaveAcceptBit(1);
    return;
  }

  if (nextPc == 0xFCA3u && (value & 0x03u) == 0x00u &&
      fake6502_y && *fake6502_y == 4u) {
    atomSaveCandidateZeros++;
    atomSaveAcceptBit(0);
    return;
  }
}

static bool atomSaveParseCapturedToAtm(char *nameOut, size_t nameOutSize,
                                       uint16_t &loadOut, uint16_t &execOut,
                                       uint8_t *&payloadOut, uint16_t &lenOut) {
  payloadOut = nullptr;
  lenOut = 0;
  if (!atomSaveBytes || atomSaveByteCount < 16) return false;

  size_t p = 0;
  size_t totalData = 0;
  bool firstBlock = true;
  char firstName[14] = {0};
  uint16_t firstLoad = 0;
  uint16_t firstExec = 0;

  // First pass: validate blocks and total payload length.
  while (p < atomSaveByteCount) {
    if (p + 4 > atomSaveByteCount ||
        atomSaveBytes[p] != 0x2A || atomSaveBytes[p+1] != 0x2A ||
        atomSaveBytes[p+2] != 0x2A || atomSaveBytes[p+3] != 0x2A) {
      Serial.printf("[TAPE-SAVE] Bad preamble at captured byte %u\n", (unsigned)p);
      return false;
    }
    size_t blockStart = p;
    p += 4;

    char nm[14] = {0};
    size_t ni = 0;
    while (p < atomSaveByteCount && atomSaveBytes[p] != 0x0D && ni < 13)
      nm[ni++] = (char)atomSaveBytes[p++];
    if (p >= atomSaveByteCount || atomSaveBytes[p] != 0x0D) {
      Serial.println("[TAPE-SAVE] Bad/long filename in captured tape");
      return false;
    }
    p++;

    if (p + 8 > atomSaveByteCount) return false;
    uint8_t status = atomSaveBytes[p+0];
    uint8_t blockLenMinus1 = atomSaveBytes[p+3];
    uint16_t run = ((uint16_t)atomSaveBytes[p+4] << 8) | atomSaveBytes[p+5];
    uint16_t load = ((uint16_t)atomSaveBytes[p+6] << 8) | atomSaveBytes[p+7];
    p += 8;

    uint16_t blockLen = (uint16_t)blockLenMinus1 + 1u;
    if (p + blockLen + 1u > atomSaveByteCount) return false;

    uint8_t sum = 0;
    for (size_t i = blockStart; i < p + blockLen; ++i)
      sum = (uint8_t)(sum + atomSaveBytes[i]);
    uint8_t receivedSum = atomSaveBytes[p + blockLen];
    if (sum != receivedSum) {
      Serial.printf("[TAPE-SAVE] Checksum mismatch: got %02X expected %02X\n",
                    receivedSum, sum);
      return false;
    }

    if (firstBlock) {
      strncpy(firstName, nm, sizeof(firstName)-1);
      firstLoad = load;
      firstExec = run;
      firstBlock = false;
    } else if (strcmp(firstName, nm) != 0) {
      Serial.println("[TAPE-SAVE] Filename changed between blocks");
      return false;
    }

    totalData += blockLen;
    p += blockLen + 1u; // data + checksum

    if ((status & 0x80u) == 0) break;
  }

  if (firstBlock || totalData > 65535u) return false;

  uint8_t *payload = (uint8_t *)malloc(totalData ? totalData : 1);
  if (!payload) return false;

  // Second pass: concatenate data bytes.
  p = 0;
  size_t out = 0;
  while (p < atomSaveByteCount && out < totalData) {
    size_t blockStart = p;
    p += 4;
    while (p < atomSaveByteCount && atomSaveBytes[p] != 0x0D) p++;
    p++;
    uint8_t status = atomSaveBytes[p+0];
    uint16_t blockLen = (uint16_t)atomSaveBytes[p+3] + 1u;
    p += 8;
    memcpy(payload + out, atomSaveBytes + p, blockLen);
    out += blockLen;
    p += blockLen + 1u;
    (void)blockStart;
    if ((status & 0x80u) == 0) break;
  }

  strncpy(nameOut, firstName, nameOutSize - 1);
  nameOut[nameOutSize - 1] = 0;
  loadOut = firstLoad;
  execOut = firstExec;
  payloadOut = payload;
  lenOut = (uint16_t)totalData;
  return true;
}

static String atomSaveSafeFilename(const char *atomName) {
  String s;
  for (size_t i = 0; atomName[i] && i < 13; ++i) {
    char c = atomName[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_')
      s += c;
    else
      s += '_';
  }
  if (s.length() == 0) s = "NONAME";
  s += ".atm";
  return s;
}


// V104b: true Turbo SAVE at the documented OSSAVE interface.
// On entry to the genuine Kernel SAVVEC target ($FAE5), X points to a
// 10-byte zero-page control block:
//   X+0 filename pointer (CR terminated)
//   X+2 reload address
//   X+4 execution address
//   X+6 start address in memory
//   X+8 end address + 1
// The emulator writes the corresponding ATM file directly and returns from
// OSSAVE. No cassette leader/modulation/checksum stream is generated.
extern "C" uint8_t atom_tape_fast_ossave(uint8_t x) {
  auto zp16 = [x](uint8_t off) -> uint16_t {
    uint8_t a = (uint8_t)(x + off);
    uint8_t b = (uint8_t)(a + 1u);
    return (uint16_t)fakemem[a] | ((uint16_t)fakemem[b] << 8);
  };

  uint16_t namePtr  = zp16(0);
  uint16_t load     = zp16(2);
  uint16_t execAddr = zp16(4);
  uint16_t start    = zp16(6);
  uint16_t endp1    = zp16(8);
  uint16_t length   = (uint16_t)(endp1 - start);

  char atomName[17] = {0};
  size_t n = 0;
  while (n < 16) {
    uint8_t c = fakemem[(uint16_t)(namePtr + n)];
    if (c == 0x0D || c == 0x00) break;
    atomName[n++] = (char)c;
  }
  atomName[n] = 0;
  if (n == 0) {
    Serial.println("[TAPE-SAVE] Turbo OSSAVE: empty filename; falling back");
    return 0u;
  }

  String path = atomTapeJoinCurrent(atomSaveSafeFilename(atomName));
  if (SD.exists(path)) {
    SD.remove(path);
    Serial.printf("[TAPE-SAVE] Replacing existing %s\n", path.c_str());
  }

  fs::File f = SD.open(path, FILE_WRITE);
  if (!f) {
    Serial.printf("[TAPE-SAVE] Cannot create %s; falling back\n", path.c_str());
    return 0u;
  }

  uint8_t hdr[22] = {0};
  memcpy(hdr, atomName, n);
  hdr[16] = (uint8_t)(load & 0xFFu);
  hdr[17] = (uint8_t)(load >> 8);
  hdr[18] = (uint8_t)(execAddr & 0xFFu);
  hdr[19] = (uint8_t)(execAddr >> 8);
  hdr[20] = (uint8_t)(length & 0xFFu);
  hdr[21] = (uint8_t)(length >> 8);

  size_t wh = f.write(hdr, sizeof(hdr));
  size_t wd = 0;
  uint16_t p = start;
  uint16_t remaining = length;
  while (remaining) {
    uint8_t buf[256];
    uint16_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
    for (uint16_t i = 0; i < chunk; ++i)
      buf[i] = fakemem[(uint16_t)(p + i)];
    size_t w = f.write(buf, chunk);
    wd += w;
    if (w != chunk) break;
    p = (uint16_t)(p + chunk);
    remaining = (uint16_t)(remaining - chunk);
    yield();
  }
  f.close();

  if (wh != sizeof(hdr) || wd != length) {
    Serial.printf("[TAPE-SAVE] Turbo OSSAVE incomplete for %s (%u/%u)\n",
                  path.c_str(), (unsigned)wd, (unsigned)length);
    return 1u; // OSSAVE was handled; do not start a second physical SAVE.
  }

  Serial.printf("[TAPE-SAVE] Turbo wrote %s\n", path.c_str());
  Serial.printf("[TAPE-SAVE] Name=\"%s\" load=$%04X exec=$%04X start=$%04X end=$%04X length=%u\n",
                atomName, load, execAddr, start, endp1, (unsigned)length);

  atomTapeBuildFromAtm(path.c_str());
  return 1u;
}

static void atomSaveFlushToSd() {
  if (!atomSaveReady) return;
  atomSaveReady = false;

  char atomName[14] = {0};
  uint16_t load = 0, execAddr = 0, length = 0;
  uint8_t *payload = nullptr;

  if (!atomSaveParseCapturedToAtm(atomName, sizeof(atomName), load, execAddr,
                                  payload, length)) {
    Serial.printf("[TAPE-SAVE] Could not convert capture to ATM: bytes=%u bits=%u ones=%u zeros=%u frameErrors=%u\n",
                  (unsigned)atomSaveByteCount,
                  (unsigned)atomSaveAcceptedBits,
                  (unsigned)atomSaveCandidateOnes,
                  (unsigned)atomSaveCandidateZeros,
                  (unsigned)atomSaveFrameErrors);
    return;
  }

  String path = atomTapeJoinCurrent(atomSaveSafeFilename(atomName));
  if (SD.exists(path)) {
    SD.remove(path);
    Serial.printf("[TAPE-SAVE] Replacing existing %s\n", path.c_str());
  }

  fs::File f = SD.open(path, FILE_WRITE);
  if (!f) {
    Serial.printf("[TAPE-SAVE] Cannot create %s\n", path.c_str());
    free(payload);
    return;
  }

  uint8_t hdr[22] = {0};
  size_t n = strlen(atomName);
  if (n > 16) n = 16;
  memcpy(hdr, atomName, n);
  hdr[16] = (uint8_t)(load & 0xFF);
  hdr[17] = (uint8_t)(load >> 8);
  hdr[18] = (uint8_t)(execAddr & 0xFF);
  hdr[19] = (uint8_t)(execAddr >> 8);
  hdr[20] = (uint8_t)(length & 0xFF);
  hdr[21] = (uint8_t)(length >> 8);

  size_t wh = f.write(hdr, sizeof(hdr));
  size_t wd = length ? f.write(payload, length) : 0;
  f.close();
  free(payload);

  if (wh != sizeof(hdr) || (length && wd != length)) {
    Serial.printf("[TAPE-SAVE] SD write incomplete for %s\n", path.c_str());
    return;
  }

  Serial.printf("[TAPE-SAVE] Wrote %s\n", path.c_str());
  Serial.printf("[TAPE-SAVE] Name=\"%s\" load=$%04X exec=$%04X length=%u frameErrors=%u\n",
                atomName, load, execAddr, (unsigned)length,
                (unsigned)atomSaveFrameErrors);

  // Make the freshly saved file the current virtual cassette immediately.
  // This allows SAVE -> modify program -> LOAD round-trip testing without a
  // reboot or touching the SD card.
  atomTapeBuildFromAtm(path.c_str());
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// V109 generic automatic direct-scanner keyboard path
// ---------------------------------------------------------------------------
// Any serial character that maps to an Atom physical key begins as ordinary
// FIFO input. If software directly scans that key's 8255 matrix row/FE71,
// atom_kbd.c transfers it to a synthetic
// held state.  Host auto-repeat then refreshes that state instead of queueing
// future moves/shots.  Genuine ROM OSRDCH typing remains unchanged.
// No GAMEKEYS mode switch is required.

static void deliverAtomSerialByte(uint8_t c)
{
  user_typed_since_boot = 1;

  // Preserve V069's special immediate physical ESC behaviour.
  if (c == 0x1B) {
    atom_kbd_press_escape();
    return;
  }

  atom_kbd_push_ascii(c);
}


// ---------------------------------------------------------------------------
// V074 host ~BREAK escape
// ---------------------------------------------------------------------------
// Works at any time, including while machine-code software is polling the
// keyboard directly. It is consumed by the ESP32 and never reaches the Atom.
//
// Matching is case-insensitive because incoming a-z are already normalized
// to upper case by loop(). No Return is required: the reset is requested as
// soon as the final 'K' of "~BREAK" arrives.
static String hostEscapeCandidate;
static bool hostEscapeSwallowLineEnd = false;

static bool handleHostEscapeByte(uint8_t c)
{
  if (hostEscapeSwallowLineEnd) {
    if (c == '\r') {
      hostEscapeSwallowLineEnd = false;
      return true;
    }
    hostEscapeSwallowLineEnd = false;
  }

  const char *cmdBreak = "~BREAK";
  const char *cmdShot  = "~SHOT";

  if (hostEscapeCandidate.length() == 0) {
    if (c != '~')
      return false;
    hostEscapeCandidate = "~";
    return true;
  }

  hostEscapeCandidate += (char)c;
  size_t n = hostEscapeCandidate.length();
  const char *cand = hostEscapeCandidate.c_str();

  bool breakPrefix = (n <= strlen(cmdBreak) && strncmp(cand, cmdBreak, n) == 0);
  bool shotPrefix  = (n <= strlen(cmdShot)  && strncmp(cand, cmdShot,  n) == 0);

  if (breakPrefix || shotPrefix) {
    if (n == strlen(cmdBreak) && strcmp(cand, cmdBreak) == 0) {
      hostEscapeCandidate = "";
      hostEscapeSwallowLineEnd = true;
      Serial.println("[BREAK] ~BREAK received - requesting Atom reset");
      atom_request_break();
    } else if (n == strlen(cmdShot) && strcmp(cand, cmdShot) == 0) {
      hostEscapeCandidate = "";
      hostEscapeSwallowLineEnd = true;
      atomScreenshotRequested = true;
      Serial.println("[SCREENSHOT] ~SHOT received");
    }
    return true;
  }

  // Not a recognized host escape: replay it to the Atom unchanged.
  String replay = hostEscapeCandidate;
  hostEscapeCandidate = "";
  for (size_t i = 0; i < replay.length(); ++i)
    deliverAtomSerialByte((uint8_t)replay[i]);
  return true;
}

// ---------------------------------------------------------------------------
// V072 boot-screen version helper (from V071)
// ---------------------------------------------------------------------------
// Derive e.g. "V072" from the actual .ino filename.
static String atomVersionFromFilename()
{
  String f = String(__FILE__);
  int s1 = f.lastIndexOf('/');
  int s2 = f.lastIndexOf('\\');
  int slash = (s1 > s2) ? s1 : s2;
  if (slash >= 0) f = f.substring(slash + 1);

  int v = f.lastIndexOf("_V");
  int dot = f.lastIndexOf('.');
  if (v >= 0) {
    if (dot < 0 || dot <= v) dot = f.length();
    return f.substring(v + 1, dot);
  }
  return f;
}

static const uint32_t ATOM_BOOT_SCREEN_MS = 2500u;


static const char *atomResetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXTERNAL";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}

// ---------------------------------------------------------------------------
// V171 physical Atom keyboard task
// ---------------------------------------------------------------------------
// Based directly on V170. Ordinary BREAK is unchanged.
// SHIFT+BREAK is latched and both keys must be released before the normal Atom
// BREAK is requested.  Once the reset Kernel reaches FE94 (its first OSRDCH),
// bitboard_6502.c tells loop() that Atom is genuinely ready for a command.
// loop() then feeds exactly LINK #4000 + CR through the proven ASCII FIFO.
static volatile bool v171_bbc_break_wait_release = false;
extern "C" volatile uint8_t v171_bbc_boot_pending = 0;
extern "C" volatile uint8_t v171_bbc_atom_ready = 0;
extern "C" volatile uint32_t v173_bbc_osrdch_epoch = 0;


static void atomHwKeyboardTask(void *pvParameters)
{
  (void)pvParameters;
  bool breakWasDown = false;

  for (;;) {
    atom_hw_kbd_scan();

    bool breakDown = atom_hw_kbd_break_pressed() != 0;
    // PB7 is SHIFT, active low, and is common to every selected row.
    bool shiftDown = (atom_hw_kbd_get_port_b(0) & 0x80u) == 0;

    if (breakDown && !breakWasDown) {
      if (shiftDown) {
        Serial.println("[BREAK] Physical SHIFT+BREAK - waiting for key release");
        v171_bbc_break_wait_release = true;
      } else {
        Serial.println("[BREAK] Physical BREAK - requesting Atom reset");
        atom_request_break();
      }
    }

    if (v171_bbc_break_wait_release && !breakDown && !shiftDown) {
      v171_bbc_break_wait_release = false;
      v171_bbc_atom_ready = 0;
      v171_bbc_boot_pending = 1;
      Serial.println("[BREAK] SHIFT+BREAK released - requesting normal Atom reset");
      atom_request_break();
    }

    breakWasDown = breakDown;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  esp_reset_reason_t rr = esp_reset_reason();
  Serial.printf("[RESET-CAUSE] ESP32 %d (%s)\n", (int)rr, atomResetReasonName(rr));
  delay(400);
  Serial.println();
  {
    String ver = atomVersionFromFilename();
    Serial.printf("[ATOM 6502] 8048S070 %s automatic direct-scan keyboard starting...\n",
                  ver.c_str());
  }

  // ---- SPIFFS ----
  if (!SPIFFS.begin(true)) {
    Serial.println("[SPIFFS] Mount failed!");
  } else {
    Serial.println("[SPIFFS] Mounted OK");
    fs::File root = SPIFFS.open("/");
    fs::File f = root.openNextFile();
    while (f) {
      Serial.printf("[SPIFFS] %s (%u bytes)\n", f.name(), (unsigned)f.size());
      f = root.openNextFile();
    }
  }

  // ---- TFT / RGB framebuffer ----
  // Initialize this first so LovyanGFX can reserve the 800x480 RGB framebuffer
  // before SD and I2S allocate additional DMA/memory resources.
  tft.init();
  tft.setBrightness(255);
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  String bootVersion = atomVersionFromFilename();
  tft.drawString("ACORN ATOM (6502)", 10, 8, 4);
  tft.drawString("Version " + bootVersion, 10, 38, 2);
  tft.drawString("Serial @115200",    10, 58, 2);

  // ---- 8048S070 onboard SD card ----
  // SPI SCK=12, MISO=13, MOSI=11, CS=10.
#if defined(ATOM_HAS_SD)
  Serial.printf("[SD] SPI SCK=%d MISO=%d MOSI=%d CS=%d\n",
                ATOM_SD_SCLK, ATOM_SD_MISO, ATOM_SD_MOSI, ATOM_SD_CS);

  pinMode(ATOM_SD_CS, OUTPUT);
  digitalWrite(ATOM_SD_CS, HIGH);
  atomSdSPI.begin(ATOM_SD_SCLK, ATOM_SD_MISO, ATOM_SD_MOSI, ATOM_SD_CS);

  // Start conservatively at 4 MHz for maximum card compatibility.
  if (!SD.begin(ATOM_SD_CS, atomSdSPI, 4000000)) {
    atomSdMounted = false;
    Serial.println("[SD] No card / mount failed (continuing without SD)");
  } else {
    atomSdMounted = true;
    uint8_t cardType = SD.cardType();
    const char *typeName =
        (cardType == CARD_MMC)  ? "MMC" :
        (cardType == CARD_SD)   ? "SDSC" :
        (cardType == CARD_SDHC) ? "SDHC/SDXC" : "UNKNOWN";

    Serial.printf("[SD] Mounted OK, type=%s, size=%llu MB\n",
                  typeName, (unsigned long long)(SD.cardSize() / (1024ULL * 1024ULL)));

    if (!SD.exists("/ATOM")) {
      if (SD.mkdir("/ATOM")) Serial.println("[SD] Created /ATOM");
      else Serial.println("[SD] Could not create /ATOM");
    }
    if (!SD.exists("/ATOM/TAPES")) {
      if (SD.mkdir("/ATOM/TAPES")) Serial.println("[SD] Created /ATOM/TAPES");
      else Serial.println("[SD] Could not create /ATOM/TAPES");
    }
    if (!SD.exists("/ATOM/SCREENSHOTS")) {
      if (SD.mkdir("/ATOM/SCREENSHOTS")) Serial.println("[SD] Created /ATOM/SCREENSHOTS");
      else Serial.println("[SD] Could not create /ATOM/SCREENSHOTS");
    }

    atomTapeCurrentDir = ATOM_TAPE_ROOT;
    Serial.printf("[SD] %s contents:\n", atomTapeCurrentDir.c_str());
    fs::File dir = SD.open(atomTapeCurrentDir.c_str());
    if (dir && dir.isDirectory()) {
      fs::File entry = dir.openNextFile();
      bool any = false;
      while (entry) {
        any = true;
        Serial.printf("[SD]   %s%s (%u bytes)\n",
                      entry.name(), entry.isDirectory() ? "/" : "",
                      (unsigned)entry.size());
        entry = dir.openNextFile();
      }
      if (!any) Serial.println("[SD]   <empty>");
    } else {
      Serial.println("[SD]   Could not open directory");
    }

    // V066: mount the first .atm file as the virtual cassette.
    atomTapeFindFirstAtm();
  }
#else
  Serial.println("[SD] Not configured for selected board; continuing without SD");
#endif

  // ---- V120 raw PC2 speaker / onboard MAX98357 ----
  // I2S receives the original program-generated PC2 waveform reconstructed
  // from 6502-cycle timestamps, followed only by a simple AC-coupling filter.
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = ATOM_AUDIO_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 6,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  i2s_pin_config_t i2s_pins = {
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = ATOM_I2S_BCLK,
    .ws_io_num = ATOM_I2S_LRCLK,
    .data_out_num = ATOM_I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  esp_err_t audioErr = i2s_driver_install(ATOM_I2S_PORT, &i2s_config, 0, NULL);
  if (audioErr == ESP_OK) audioErr = i2s_set_pin(ATOM_I2S_PORT, &i2s_pins);
  Serial.printf("[AUDIO] MAX98357 I2S BCLK=%d WS=%d DATA=%d : %s\n",
                ATOM_I2S_BCLK, ATOM_I2S_LRCLK, ATOM_I2S_DOUT,
                audioErr == ESP_OK ? "OK" : "FAILED");

  if (audioErr == ESP_OK) {
    atomAudioEventHead = atomAudioEventTail = 0;
    atomAudioEventDrops = 0;
    atomAudioLastEdgeCycle = clockticks6502;
    atomI2sAudioRunning = true;
    BaseType_t taskOk = xTaskCreatePinnedToCore(
        atomAudioI2sTask,
        "atom_audio",
        3072,
        NULL,
        2,
        NULL,
        0);
    Serial.printf("[AUDIO] raw-PC2 AC-coupled renderer: %s (40 kHz)\n",
                  taskOk == pdPASS ? "OK" : "FAILED");
    if (taskOk != pdPASS) atomI2sAudioRunning = false;
  }

  uint32_t bootScreenShownAt = millis();

  // ---- Start 6502 + Atom memory/keyboard via C file ----
  bitboard_app_main();

  // Give the Atom reset code time to copy vectors. V041 deliberately
  // leaves OSRDCH pointing at the original Atom ROM keyboard routine.
  delay(500);

  Serial.printf("OSRDCH vector: %02X %02X\n", fakemem[0x020A], fakemem[0x020B]);
  Serial.printf("OSWRCH vector: %02X %02X\n", fakemem[0x020E], fakemem[0x020F]);

  // V166: restore the proven V141 MCP23017 keyboard only after the existing
  // boot/RESET vector setup has completed. BBC BASIC V164 memory mapping is untouched.
  Serial.printf("[V166] COMVEC before HW-KBD: %02X %02X ($%04X)\n",
                fakemem[0x0206], fakemem[0x0207],
                (unsigned)(fakemem[0x0206] | ((uint16_t)fakemem[0x0207] << 8)));

  int hwKbdOk = atom_hw_kbd_init();
  if (hwKbdOk) {
    BaseType_t hwTaskOk = xTaskCreatePinnedToCore(
        atomHwKeyboardTask,
        "atom_hw_kbd",
        3072,
        NULL,
        1,
        NULL,
        0);
    Serial.printf("[HW-KBD] background scanner: %s (core 0, priority 1)\n",
                  hwTaskOk == pdPASS ? "OK" : "FAILED");
  }

  Serial.printf("[V166] COMVEC after HW-KBD:  %02X %02X ($%04X)\n",
                fakemem[0x0206], fakemem[0x0207],
                (unsigned)(fakemem[0x0206] | ((uint16_t)fakemem[0x0207] << 8)));

  uint32_t shownFor = millis() - bootScreenShownAt;
  if (shownFor < ATOM_BOOT_SCREEN_MS)
    delay(ATOM_BOOT_SCREEN_MS - shownFor);

  tft.fillScreen(TFT_BLACK);
}

// ---------------------------------------------------------------------------
// V102b CYD screenshot support
// ---------------------------------------------------------------------------
// This JC2432W328/ST7789 board returns the screenshot readback bytes in R,B,G
// order for the working display configuration.  Convert explicitly to BMP
// B,G,R order below.  No startup calibration or test pixels are required.

#if defined(ATOM_HAS_SD)
static void atomBmpWrite16(fs::File &f, uint16_t v)
{
  uint8_t b[2] = { (uint8_t)(v & 0xFFu), (uint8_t)((v >> 8) & 0xFFu) };
  f.write(b, sizeof(b));
}

static void atomBmpWrite32(fs::File &f, uint32_t v)
{
  uint8_t b[4] = {
    (uint8_t)(v & 0xFFu),
    (uint8_t)((v >> 8) & 0xFFu),
    (uint8_t)((v >> 16) & 0xFFu),
    (uint8_t)((v >> 24) & 0xFFu)
  };
  f.write(b, sizeof(b));
}

static bool atomScreenshotFilename(char *out, size_t outLen)
{
  for (unsigned n = 1; n <= 9999; ++n) {
    snprintf(out, outLen, "/ATOM/SCREENSHOTS/ATOM%04u.BMP", n);
    if (!SD.exists(out)) return true;
    if ((n & 0x3Fu) == 0) yield();
  }
  return false;
}

static bool atomSaveCydScreenshotBmp()
{
  if (!atomSdMounted) {
    Serial.println("[SCREENSHOT] SD card is not mounted");
    return false;
  }

  if (!SD.exists("/ATOM/SCREENSHOTS") && !SD.mkdir("/ATOM/SCREENSHOTS")) {
    Serial.println("[SCREENSHOT] Could not create /ATOM/SCREENSHOTS");
    return false;
  }

  char path[48];
  if (!atomScreenshotFilename(path, sizeof(path))) {
    Serial.println("[SCREENSHOT] No free ATOM0001..ATOM9999 filename");
    return false;
  }

  fs::File f = SD.open(path, FILE_WRITE);
  if (!f) {
    Serial.printf("[SCREENSHOT] Could not open %s for writing\n", path);
    return false;
  }

  const uint32_t w = (uint32_t)tft.width();
  const uint32_t h = (uint32_t)tft.height();
  const uint32_t rowBytes = w * 3u;
  const uint32_t rowPadded = (rowBytes + 3u) & ~3u;
  const uint32_t pixelBytes = rowPadded * h;
  const uint32_t fileBytes = 54u + pixelBytes;

  // Standard uncompressed 24-bit BMP, positive height = bottom-up rows.
  f.write((const uint8_t *)"BM", 2);
  atomBmpWrite32(f, fileBytes);
  atomBmpWrite16(f, 0);
  atomBmpWrite16(f, 0);
  atomBmpWrite32(f, 54u);

  atomBmpWrite32(f, 40u);           // BITMAPINFOHEADER size
  atomBmpWrite32(f, w);
  atomBmpWrite32(f, h);
  atomBmpWrite16(f, 1u);            // planes
  atomBmpWrite16(f, 24u);           // bits/pixel
  atomBmpWrite32(f, 0u);            // BI_RGB
  atomBmpWrite32(f, pixelBytes);
  atomBmpWrite32(f, 2835u);         // 72 DPI approx.
  atomBmpWrite32(f, 2835u);
  atomBmpWrite32(f, 0u);
  atomBmpWrite32(f, 0u);

  // One scanline at a time keeps RAM use small (~1 kB) and gives the
  // Arduino/FreeRTOS watchdog a service point on every row.  V102b uses the confirmed JC2432W328/ST7789 readback order R,B,G;
  // BMP stores B,G,R.
  static uint8_t rgb[800 * 3];
  static uint8_t bgr[800 * 3 + 3];
  if (w > 800u) {
    f.close();
    SD.remove(path);
    Serial.printf("[SCREENSHOT] Unsupported TFT width %u\n", (unsigned)w);
    return false;
  }

  for (int32_t y = (int32_t)h - 1; y >= 0; --y) {
    tft.readRectRGB(0, y, (int32_t)w, 1, rgb);

    uint32_t o = 0;
    for (uint32_t x = 0; x < w; ++x) {
      uint32_t i = x * 3u;
      // LovyanGFX readRectRGB returns R,G,B. BMP stores B,G,R.
      bgr[o++] = rgb[i + 2];   // B
      bgr[o++] = rgb[i + 1];   // G
      bgr[o++] = rgb[i + 0];   // R
    }
    while (o < rowPadded) bgr[o++] = 0;

    if (f.write(bgr, rowPadded) != rowPadded) {
      f.close();
      SD.remove(path);
      Serial.printf("[SCREENSHOT] Write failed at row %ld\n", (long)y);
      return false;
    }

    // Explicitly yield every scanline so a slow SD card or TFT read cannot
    // starve the loop task/watchdog during a complete 320x240 capture.
    yield();
  }

  f.flush();
  f.close();
  Serial.printf("[SCREENSHOT] Saved %s (%ux%u, 24-bit BMP)\n",
                path, (unsigned)w, (unsigned)h);
  return true;
}
#endif

static void atomScreenshotButtonPoll()
{
#if ATOM_SCREENSHOT_BUTTON_ENABLED
  static bool initialized = false;
  static int stableState = HIGH;
  static int lastRaw = HIGH;
  static uint32_t changedAt = 0;

  int raw = digitalRead(ATOM_SCREENSHOT_PIN);
  uint32_t now = millis();

  if (!initialized) {
    initialized = true;
    stableState = raw;
    lastRaw = raw;
    changedAt = now;
    return;
  }

  if (raw != lastRaw) {
    lastRaw = raw;
    changedAt = now;
  }

  if (raw != stableState && (now - changedAt) >= ATOM_SCREENSHOT_DEBOUNCE_MS) {
    stableState = raw;
    if (stableState == LOW) {
#if defined(ATOM_HAS_SD)
      atomSaveCydScreenshotBmp();
#else
      Serial.println("[SCREENSHOT] SD not configured for this board");
#endif
    }
  }
#endif
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void loop() {
  
  atomScreenshotButtonPoll();
  atomCatProcessDeferredRestore();

  // V124 clean: optional Atom character-output mirror to USB Serial.
  while (atom_debug_out_avail()) {
    Serial.write(atom_debug_out_pop());
  }
static uint32_t lastVideo = 0;
  static uint32_t lastStats = 0;

  static bool wakeSent = false;
  static bool promptSeen = false;
  static bool autoWakeArmed = false;
  static uint32_t startMs = 0;
  static uint32_t seenPromptCount = 0;

  uint32_t now = millis();

  // V067: SD writes are deliberately performed from the Arduino loop, never
  // from inside a 6502 memory-write callback.
  atomSaveFlushToSd();

  // Arm startup timer once.
  if (!autoWakeArmed) {
    autoWakeArmed = true;
    startMs = now;
  }

  // Preserve V038 startup behaviour for this first matrix test: inject one
  // physical RETURN after the same settle delay. Interactive input after that
  // uses the real 8255 matrix path as well.
  if (!wakeSent && (now - startMs) >= 143) {
    atom_kbd_inject_boot_return(2);
    wakeSent = true;
    suppress_boot_prompt = 1;
    user_typed_since_boot = 0;
  }

  // Detect prompts for diagnostics only. Do not flush the keyboard queue:
  // that would destroy legitimate type-ahead now that matrix input is live.
  bool sawPrompt =
      (last_oswrch_count != seenPromptCount) &&
      (last_oswrch_char == '>');

  if (sawPrompt) {
    seenPromptCount = last_oswrch_count;
    promptSeen = true;


  }

  // V173: feed exactly one character for each genuine OSRDCH request.
  // V172 queued the whole command at once; the Atom keyboard FIFO could then
  // consume/drop a character (observed LINK 1684).  The CPU task increments
  // v173_bbc_osrdch_epoch every time command input re-enters $FE94.
  if (v171_bbc_boot_pending && v171_bbc_atom_ready) {
    static const char bbcCmd[] = "LINK #4000\r";
    static size_t bbcCmdPos = 0;
    static uint32_t lastEpoch = 0;

    if (bbcCmdPos == 0 && lastEpoch == 0)
      Serial.println("[BREAK] Atom ready at OSRDCH - feeding LINK #4000 one character per OSRDCH");

    uint32_t epoch = v173_bbc_osrdch_epoch;
    if (epoch != lastEpoch && bbcCmdPos < sizeof(bbcCmd) - 1) {
      lastEpoch = epoch;
      uint8_t c = (uint8_t)bbcCmd[bbcCmdPos++];
      deliverAtomSerialByte(c);

      if (bbcCmdPos >= sizeof(bbcCmd) - 1) {
        v171_bbc_atom_ready = 0;
        v171_bbc_boot_pending = 0;
        bbcCmdPos = 0;
        lastEpoch = 0;
        Serial.println("[BREAK] BBC command complete");
      }
    }
  }

  while (Serial.available() > 0) {
    uint8_t c = (uint8_t)Serial.read();

    // normalize CR/LF -> CR
    static bool lastWasCR = false;
    if (c == 13) {
      lastWasCR = true;
    } else if (c == 10) {
      if (lastWasCR) {
        lastWasCR = false;
        continue;
      }
      c = 13;
    } else {
      lastWasCR = false;
    }

    // Serial terminals often send DEL for Backspace. atom_kbd maps both.
    if (c == 0x7F) c = 0x08;

    // Preserve V038's convenient lower->upper host normalization.
    if (c >= 97 && c <= 122) c = (uint8_t)(c - 32);

    static uint32_t pushCount = 0;
    if (SERIAL_SHOW_RX_MATRIX_DIAGNOSTIC && pushCount < 200) {
      Serial.printf("[RX-MATRIX] %u '%c' (0x%02X)\n",
                    (unsigned)pushCount,
                    (c >= 32 && c < 127) ? c : '.',
                    c);
      pushCount++;
    }

    // V074 host escape works even while a game is directly scanning the
    // keyboard matrix. It is consumed before any Atom keyboard processing.
    if (handleHostEscapeByte(c))
      continue;

    deliverAtomSerialByte(c);
  }

#if defined(ATOM_HAS_SD)
  if (atomScreenshotRequested) {
    atomScreenshotRequested = false;
    atomSaveCydScreenshotBmp();
  }
#endif

  if (now - lastVideo > 50) {
    lastVideo = now;
    atom_video_update();
  }

  // V180: while SHIFT+BREAK is feeding LINK #4000, service each genuine
  // OSRDCH edge promptly.  Keep one-character-per-OSRDCH synchronization,
  // but remove the old 5 ms loop polling penalty.
  delay((v171_bbc_boot_pending && v171_bbc_atom_ready) ? 1 : 5);
}
