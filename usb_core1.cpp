// TRACT8 - Core 1 USB host task pump.
//
// Core 1 owns the whole USB stack and does nothing else. Core 0 runs the
// 48 kHz audio interrupt via ComputerCard::Run().
//
// This split is the opposite of what might seem natural (DSP on the second
// core, housekeeping on the first), and it is deliberate. ComputerCard
// installs its audio interrupt on whichever core calls Run(), and every
// working USB-host card on this hardware - the official ComputerCard
// midi_host example included - puts audio on Core 0 and the USB pump on
// Core 1. tuh_task() is a blocking-ish polled loop with no deadline of its
// own; the audio ISR has a hard 20.8 us one. Giving the ISR the core that
// is not also servicing USB interrupts is the safer arrangement.
//
// The MIDI host driver is rppicomidi/usb_midi_host (MIT), vendored
// unmodified in usb_midi_host.c/.h and registered through TinyUSB's
// usbh_app_driver_get_cb() hook in usb_midi_host_app_driver.c. Pico SDK
// 2.2.0 ships TinyUSB 0.18, which has no MIDI host class driver of its own.

#include "shared.h"
#include "midi8mu.h"

#include "pico/multicore.h"
#include "pico/time.h"
#include "bsp/board_api.h"
#include "tusb.h"
#include "usb_midi_host.h"

volatile VoderState g_state;

namespace {

// Address of the one MIDI device we listen to. The front jack is a single
// port; a hub could bring more, but the first device to mount wins.
uint8_t s_dev_addr = 0;

// Running-status parser state. tuh_midi_stream_read() hands back a byte
// stream, not aligned messages, so status and data bytes have to be
// reassembled here. Running status - a stream of data bytes with the
// status sent only once - is exactly what an 8mu emits when you sweep a
// fader, so this is not an optional nicety.
uint8_t s_status = 0;
uint8_t s_data1 = 0;
uint8_t s_have_data1 = 0;

void ParseByte(uint8_t b) {
  if (b >= 0xF8) {
    // System real-time (clock, start, stop) can appear anywhere, even
    // between the data bytes of another message. Ignore without
    // disturbing the running status.
    return;
  }

  if (b & 0x80) {
    if (b >= 0xF0) {
      // System common cancels running status. We do not decode sysex; the
      // bytes of one will be swallowed harmlessly as data with no status.
      s_status = 0;
      s_have_data1 = 0;
      return;
    }
    s_status = b;
    s_have_data1 = 0;
    return;
  }

  if (s_status == 0) return;  // data byte with no status, drop

  if (!s_have_data1) {
    s_data1 = b;
    s_have_data1 = 1;

    // Program change and channel aftertouch are single-data-byte messages.
    // Neither is mapped, but they must be consumed or the parser would
    // treat the next status byte as their second data byte.
    const uint8_t type = s_status & 0xF0;
    if (type == 0xC0 || type == 0xD0) {
      s_have_data1 = 0;
    }
    return;
  }

  tract8::Midi8muMessage(s_status, s_data1, b);
  s_have_data1 = 0;  // stay armed for running status
}

}  // namespace

// --- rppicomidi callbacks ----------------------------------------------

void tuh_midi_mount_cb(uint8_t dev_addr, uint8_t in_ep, uint8_t out_ep,
                       uint8_t num_cables_rx, uint16_t num_cables_tx) {
  (void)in_ep; (void)out_ep; (void)num_cables_rx; (void)num_cables_tx;

  if (s_dev_addr == 0) {
    s_dev_addr = dev_addr;
    g_state.midi_connected = 1;
  }
}

void tuh_midi_umount_cb(uint8_t dev_addr, uint8_t instance) {
  (void)instance;

  if (dev_addr == s_dev_addr) {
    s_dev_addr = 0;
    g_state.midi_connected = 0;

    // Leave band_gain where it is. Unplugging the 8mu mid-phrase should
    // not slam every formant shut - the panel knobs pick up from whatever
    // the faders last set, which is the least surprising behaviour.
    s_status = 0;
    s_have_data1 = 0;
  }
}

// Maximum bytes drained per callback.
//
// This bound is the whole point of the constant. The obvious loop here is
// "read until the stream returns nothing", and it hangs the card: an 8mu
// held in the hand streams accelerometer CCs continuously, and it can
// refill the endpoint as fast as this loop empties it. The read never
// returns 0, the callback never returns, tuh_task() is never called again
// and USB stops being serviced entirely. The card appears to lock up under
// heavy MIDI - which is exactly what was reported from hardware.
//
// 256 bytes is 85 complete channel-voice messages, far more than an 8mu
// generates between two calls of tuh_task(). Anything left over is not
// dropped: it stays in the endpoint buffer and arrives on the next
// callback, a fraction of a millisecond later. Bounding the work per
// callback is what keeps the task pump turning.
static constexpr uint32_t kMaxRxBytesPerCallback = 256;

void tuh_midi_rx_cb(uint8_t dev_addr, uint32_t num_packets) {
  if (s_dev_addr != dev_addr || num_packets == 0) return;

  uint8_t cable;
  uint8_t buf[48];
  uint32_t drained = 0;

  while (drained < kMaxRxBytesPerCallback) {
    const uint32_t n = tuh_midi_stream_read(dev_addr, &cable, buf, sizeof(buf));
    if (n == 0) return;
    for (uint32_t i = 0; i < n; i++) ParseByte(buf[i]);
    drained += n;
  }
}

void tuh_midi_tx_cb(uint8_t dev_addr) {
  (void)dev_addr;  // TRACT8 never transmits
}

// --- Core 1 entry -------------------------------------------------------

extern "C" void core1_entry(void) {
  // Settling delay before touching the USB controller. The working
  // USB-host cards on this hardware all do this; without it the controller
  // does not always come up cleanly after a reset.
  sleep_ms(150);

  board_init();
  tusb_init();

  while (true) {
    tuh_task();
  }
}
