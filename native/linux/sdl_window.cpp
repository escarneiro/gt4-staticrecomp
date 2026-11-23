// TASK-074 L2: the Linux game window, through SDL3. It implements native/window.hpp for the port's main.cpp, as
// native/window.cpp does on Windows, and keeps that host's shape: one thread, and at every guest VBlank the window
// events, the pad, the presentation and the pacing. The SDL handling of X11/Wayland handles, audio and the controller
// follows the MIT donor's host/window.cpp (ps2-web-recomp, SDL2), ported to SDL3.
//
// What it has: an X11 or Wayland window presented by the native Vulkan GS backend (16:9, GT4_PRESENT_MODE), the
// controls table (GT4_CONTROLS_FILE) for the keyboard and the first SDL3 gamepad (GT4_PAD selects one), rumble, F11
// and Alt+Enter fullscreen, and the Windows host's pacing (GT4_PACING): by default the frame clock releases every
// VBlank on a 60 Hz grid and the sound is resampled to keep its queue at the latency target (sdl_audio.hpp); F8
// prints the pacing report. The game patches: GT4_FORCE_480P is held here at every VBlank (the VU1 worker runtime holds
// it too, as on Windows); the display aspect (GT4_DISPLAY_ASPECT) gets the window size and the race view presentation
// from here; the rest act from the translated game and the runtime (linux_main.cpp configures them). What it does not
// have yet (later TASK-074 steps): RetroAchievements, the F7/F8 performance probe sections and the GS process (gt4-gs).
// Input recording and replay (GT4_INPUT_RECORD, GT4_INPUT_REPLAY) work as on Windows. The cursor hides after 3 s without movement over the picture (GT4_CURSOR_AUTOHIDE), as on Windows. The frame skip (TASK-082, GT4_FRAME_SKIP / GT4_FRAME_SKIP_FORCE) gets its pacing inputs here at every
// VBlank, as on Windows; its hook is in the guard capsule's dispatch.cpp (slice376 profile, GT4_LINUX_LICENSE_GUARD). Racing wheels (TASK-018): a wheel.toml with enabled = true drives the
// virtual Driving Force Pro (native/wheel.hpp), as on Windows; the USB capsule (GT4_LINUX_USB_RUNTIME) must be built in.

#include "window.hpp"

#include "controls.hpp"
#include "cursor_autohide.hpp"
#include "display_aspect.hpp"
#include "display_refresh.hpp"
#include "frame_clock.hpp"
#include "frame_skip.hpp"
#include "gpu.hpp"
#include "input_recording.hpp"
#include "gs/gs.hpp"
#include "keyboard_scancodes.hpp"
#include "linux/sdl_audio.hpp"
#include "native_vulkan.hpp"
#include "race_display.hpp"
#include "race_widescreen.hpp"
#include "pacing.hpp"
#include "pad_mapping.hpp"
#include "ps2/system.hpp"
#include "sdl_gamepad.hpp"
#include "wheel.hpp"
#include "window_mode.hpp"
#ifdef GT4_GS_STREAM_CLIENT
#include "gs_stream/gs_stream_client.hpp"
#endif
#include "ps2/memory.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {

// SurfaceTarget kinds of the derived gpu.hpp (enum Kind { X11, Wayland, Canvas, Win32 }).
constexpr int kSurfaceX11 = 0;
constexpr int kSurfaceWayland = 1;

struct KeyBinding {
  std::array<std::uint16_t, 2> scancodes;
  std::uint16_t button_mask;
};

SDL_Window *g_window = nullptr;
SDL_Gamepad *g_pad = nullptr;
gt4::controls::ControlMap g_controls;
std::vector<KeyBinding> g_keys;
std::uint16_t g_rumble = 0;

// Racing wheel (native/wheel.hpp, TASK-018/TASK-181), as in native/window.cpp: the host wheel, pedals and shifter drive the
// virtual Driving Force Pro on the emulated USB bus, and the game's own forces play on the wheel. Off unless a wheel.toml
// with enabled = true is found (GT4_WHEEL_FILE, beside GT4_CONTROLS_FILE, or beside the executable).
gt4::wheel::WheelConfig g_wheel_config;
std::unique_ptr<gt4::wheel::WheelInput> g_wheel;

bool owned_by_wheel(SDL_JoystickID instance) {
  if (!g_wheel)
    return false;
  const auto &owned = g_wheel->owned_device_ids();
  return std::find(owned.begin(), owned.end(), instance) != owned.end();
}

// Pacing, as in native/window.cpp: GT4_PACING, the frame clock and grid, the release diagnostics and the sound output.
gt4::pacing::Mode g_pacing_mode = gt4::pacing::Mode::paced;
std::unique_ptr<FrameClock> g_clock;
std::unique_ptr<gt4::pacing::FramePacer> g_pacer;
std::unique_ptr<gt4::linux_host::SdlAudioOutput> g_audio;
gt4::pacing::Diagnostics g_diagnostics;
gt4::pacing::Counters g_counters_at_report;
std::int64_t g_last_release = 0;
std::int64_t g_ee_frame_ticks = 0; // from the last release to this VBlank, before any wait for the pacer
std::int64_t g_last_gs_wait = 0;
std::uint64_t g_vblank = 0;
bool g_report_requested = false;

// TASK-060, as native/window.cpp update_cursor: the mouse cursor hidden after 3 s without movement while it is over the
// game picture (GT4_CURSOR_AUTOHIDE, cursor_autohide.hpp). SDL gives the position relative to the window and the window
// under the mouse (SDL_GetMouseFocus), which Wayland allows where a screen position is not; SDL_HideCursor acts only over
// this window, so outside it the cursor is never touched, as on Windows.
gt4::cursor::AutoHide g_cursor;

// IN-520, as native/window.cpp: GT4_INPUT_RECORD=<file> writes the pad state the game receives at each VBlank (F9 ends
// the recording and closes the game; closing the window also leaves a complete file); GT4_INPUT_REPLAY=<file> plays one
// back at the same VBlanks instead of the keyboard and pads, so a bench repeats a real drive however fast it runs.
gt4::input_recording::Recorder g_recorder;
std::unique_ptr<gt4::input_recording::Player> g_player;

void update_cursor() {
  if (!g_cursor.policy().auto_hide)
    return;
  float x = 0.0f, y = 0.0f;
  SDL_GetMouseState(&x, &y);
  const bool over_picture = SDL_GetMouseFocus() == g_window;
  const bool was_hidden = g_cursor.hidden();
  const bool hide = g_cursor.update(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), SDL_GetTicks(), over_picture);
  if (hide != was_hidden) {
    if (hide)
      SDL_HideCursor();
    else
      SDL_ShowCursor();
  }
}

// GT4_FORCE_480P (slice164): the race display index held at 480p at every VBlank.
bool g_force_480p = false;
gt4::display::Force480pState g_force_480p_state;

// TASK-080: the race view presentation the display aspect asks for, to whichever renderer presents (window.cpp
// present_race_view): CoreGS through the GS stream seam, else the native Vulkan backend.
void present_race_view(bool fill, double ratio) {
#ifdef GT4_GS_STREAM_CLIENT
  if (gt4::gs_stream::stream_renderer()) {
    gt4::gs_stream::set_display_aspect_mode(fill ? gt4::gs_stream::DisplayAspectMode::fill
                                                 : gt4::gs_stream::DisplayAspectMode::game_aspect,
                                            fill ? gt4::gs_stream::aspect_millionths(ratio) : 0);
    return;
  }
#endif
  gt4::vulkan::set_race_view_presentation(fill, ratio);
}

void close_gamepad() {
  if (g_pad) {
    SDL_CloseGamepad(g_pad);
    g_pad = nullptr;
    g_rumble = 0;
  }
}

// The controller the launcher chose (GT4_PAD), else controller 1: the same rule as the Windows host (pad_mapping.hpp).
void open_gamepad() {
  if (g_pad)
    return;
  int count = 0;
  SDL_JoystickID *ids = SDL_GetGamepads(&count);
  if (!ids)
    return;
  std::vector<gt4::pad::PadCandidate> candidates;
  for (int index = 0; index < count; ++index) {
    // A device the wheel owns is never read as a gamepad too (as ControllerInput::set_excluded_devices on Windows).
    if (owned_by_wheel(ids[index]))
      continue;
    gt4::pad::PadCandidate candidate;
    candidate.instance = ids[index];
    char guid[33] = {};
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[index]), guid, sizeof guid);
    candidate.guid = guid;
    candidates.push_back(candidate);
  }
  SDL_free(ids);
  gt4::pad::PadChoice choice;
  const bool chosen = gt4::pad::parse_pad_choice(std::getenv("GT4_PAD"), choice);
  std::uint32_t instance = 0;
  if (!gt4::pad::choose_pad(candidates, chosen ? &choice : nullptr, instance))
    return;
  g_pad = SDL_OpenGamepad(instance);
  if (g_pad)
    std::fprintf(stderr, "[input] gamepad: %s\n", SDL_GetGamepadName(g_pad));
}

// The refresh rate of the display that shows the window, for the CoreGS performance HUD (native/display_refresh.hpp).
void publish_refresh_rate() {
  const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window));
  const std::uint32_t hz = mode != nullptr && mode->refresh_rate > 1.0f ? static_cast<std::uint32_t>(mode->refresh_rate + 0.5f) : 0;
  gt4::display::g_refresh_hz.store(hz, std::memory_order_relaxed);
}

void toggle_fullscreen() {
  const bool fullscreen = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
  SDL_SetWindowFullscreen(g_window, !fullscreen);
  if (fullscreen)
    SDL_ShowCursor();
  else
    SDL_HideCursor();
}

void poll_events() {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      g_recorder.finish(g_vblank); // the window's X or the desktop's close also leave a complete recording
      ps2::halt("window closed");
      break;
    case SDL_EVENT_KEY_DOWN:
      if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_F9 && g_recorder.active()) {
        const std::uint64_t changes = g_recorder.finish(g_vblank);
        std::fprintf(stderr, "[input] recording ended by F9 at VBlank %llu, %llu changes; closing the game\n",
                     static_cast<unsigned long long>(g_vblank), static_cast<unsigned long long>(changes));
        ps2::halt("input recording ended");
        break;
      }
      if (!event.key.repeat &&
          (event.key.scancode == SDL_SCANCODE_F11 ||
           (event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT))))
        toggle_fullscreen();
      if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_F8)
        g_report_requested = true;
      break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      ps2::gs::resize_window(static_cast<std::uint32_t>(event.window.data1),
                             static_cast<std::uint32_t>(event.window.data2));
      gt4::display_aspect::set_window_client_size(static_cast<std::uint32_t>(event.window.data1),
                                                  static_cast<std::uint32_t>(event.window.data2));
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      // As WM_KILLFOCUS on Windows: the wheel's input goes neutral and its forces stop.
      if (g_wheel)
        g_wheel->stop();
      break;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
      publish_refresh_rate();
      break;
    case SDL_EVENT_GAMEPAD_ADDED:
      open_gamepad();
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      if (g_pad && event.gdevice.which == SDL_GetGamepadID(g_pad)) {
        std::fprintf(stderr, "[input] gamepad disconnected\n");
        close_gamepad();
        open_gamepad();
      }
      break;
    default:
      break;
    }
  }
}

gt4::pad::PadState read_pad() {
  gt4::pad::PadState pad;
  if (g_pad)
    pad = gt4::pad::map_reading(read_sdl_gamepad(g_pad), g_controls);
  if (SDL_GetKeyboardFocus() != g_window)
    return pad;
  const bool *keys = SDL_GetKeyboardState(nullptr);
  const bool alt_held = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
  std::uint16_t buttons = 0;
  for (const KeyBinding &binding : g_keys)
    for (const std::uint16_t scancode : binding.scancodes) {
      // Alt+Enter is fullscreen, so Enter is not a pad button while Alt is held (as on Windows).
      if (!scancode || (scancode == SDL_SCANCODE_RETURN && alt_held))
        continue;
      if (keys[scancode])
        buttons |= binding.button_mask;
    }
  // Keyboard keys are digital: full pressure (host_io.cpp merge_keyboard).
  pad.buttons |= buttons;
  for (int bit = 0; bit < 16; ++bit)
    if (buttons & (1u << bit))
      pad.pressures[bit] = 255;
  return pad;
}

void apply_pad(const gt4::pad::PadState &pad) {
  ps2::set_pad_buttons(pad.buttons);
  for (int axis = 0; axis < 4; ++axis)
    ps2::set_pad_axis(axis, pad.axes[axis]);
  for (int button = 0; button < 16; ++button)
    ps2::set_pad_pressure(button, pad.pressures[button]);
}

void update_rumble() {
  const std::uint16_t value = ps2::pad_rumble();
  if (!g_pad || value == g_rumble)
    return;
  g_rumble = value;
  const auto motors = gt4::pad::rumble_motors(value);
  // Renewed at every change; a long duration keeps a steady value running between changes.
  SDL_RumbleGamepad(g_pad, motors.low_frequency, motors.high_frequency, value ? 60000 : 0);
}

gt4::pacing::Counters pacing_counters() {
  gt4::pacing::Counters counters;
  if (g_pacer)
    counters.late_frames = g_pacer->late_frames();
  if (g_audio) {
    counters.audio_underruns = g_audio->underruns();
    counters.ring_overflow_frames = g_audio->overflow_frames();
  }
  return counters;
}

// The pacing report of the Windows F8 (pacing.hpp Diagnostics) for the frames since the last one.
void report_pacing() {
  const gt4::pacing::Counters session = pacing_counters();
  std::fprintf(stderr, "[pacing] report (%s pacing):\n", gt4::pacing::mode_name(g_pacing_mode));
  g_diagnostics.format(stderr, g_pacing_mode, session.since(g_counters_at_report), session);
  g_counters_at_report = session;
  g_diagnostics.reset();
}

// TASK-082, as native/window.cpp note_frame_skip_vblank: what the frame skip decides from (the EE's slack against the grid,
// the GS worker's lateness and cost, the EE's own frame time without its waits for the GS worker), and what the presenter
// and the pacer get back from it.
void note_frame_skip_vblank() {
  if (!gt4::frame_skip::g_enabled)
    return;
  gt4::frame_skip::Controller &skip = gt4::frame_skip::controller();
  const std::int64_t gs_wait = gt4::pacing::ee_gs_wait_us.load(std::memory_order_relaxed);
  const std::int64_t frequency = g_clock ? static_cast<std::int64_t>(g_clock->frequency()) : 1;
  const std::int32_t ee_frame =
      static_cast<std::int32_t>(std::max<std::int64_t>(g_ee_frame_ticks * 1000000 / frequency - (gs_wait - g_last_gs_wait), 0));
  g_last_gs_wait = gs_wait;
  skip.note_vblank(g_vblank, gt4::pacing::ee_slack_us, gt4::pacing::gs_late_us.load(std::memory_order_relaxed),
                   gt4::pacing::gs_cost_us.load(std::memory_order_relaxed), ee_frame);
  gt4::pacing::frame_skip_engaged.store(skip.gate_open(), std::memory_order_relaxed);
  gt4::pacing::frame_skip_recent.store(skip.skipping_recently(), std::memory_order_relaxed);
  if (g_vblank % 600 == 0)
    gt4::frame_skip::print_report(stderr);
}

// The VBlank release, as native/window.cpp release_frame. Paced: wait for the grid deadline, but at least
// GT4_PACING_MIN_INTERVAL_US (15700) after the previous release, so time lost in a slow stretch comes back at about 1 ms
// per frame instead of in a burst; a full period behind, the grid restarts. With the frame skip engaged (TASK-082/TASK-146) up to 12 periods are owed and paid back,
// and while frames are being left out the minimum interval is off: the lost time comes back at once. Audio: the sound
// device paces instead, and the frame skip, without a slack, leaves nothing out.
void release_frame() {
  if (g_pacing_mode == gt4::pacing::Mode::paced) {
    if (!g_pacer->anchored())
      g_pacer->anchor(g_clock->now());
    // IN-521: the display queue control of CoreGS 2.0 (hw_present_latency.hpp, GT4_HW_LATENCY_CONTROL) moves the grid
    // when frames keep missing the flip; as in window.cpp, the shift is taken once and applied here.
    if (const std::int32_t shift = gt4::pacing::grid_shift_us.exchange(0, std::memory_order_relaxed))
      g_pacer->shift(static_cast<std::int64_t>(shift) * static_cast<std::int64_t>(g_clock->frequency()) / 1000000);
    static const std::int64_t min_interval = [] {
      const char *text = std::getenv("GT4_PACING_MIN_INTERVAL_US");
      const std::int64_t microseconds = text != nullptr ? std::atoll(text) : 15700;
      return microseconds * static_cast<std::int64_t>(g_clock->frequency()) / 1000000;
    }();
    const std::int64_t frequency = static_cast<std::int64_t>(g_clock->frequency());
    g_ee_frame_ticks = g_last_release != 0 ? g_clock->now() - g_last_release : 0;
    gt4::pacing::ee_slack_us = static_cast<std::int32_t>(
        std::clamp<std::int64_t>((g_pacer->deadline() - g_clock->now()) * 1000000 / frequency, -100000, 100000));
    g_pacer->set_resync_periods(gt4::pacing::frame_skip_engaged.load(std::memory_order_relaxed) ? 12 : 1);
    const std::int64_t earliest =
        g_last_release != 0 && !gt4::pacing::frame_skip_recent.load(std::memory_order_relaxed) ? g_last_release + min_interval
                                                                                                : 0;
    g_clock->wait_until(std::max(g_pacer->deadline(), earliest));
  } else if (g_audio) {
    g_audio->wait_for_room();
  }
  const std::int64_t released = g_clock->now();
  g_last_release = released;
  if (g_pacing_mode == gt4::pacing::Mode::paced)
    g_pacer->release(released);
  g_diagnostics.record_release(released, g_clock->frequency());
}

void on_vblank() {
  release_frame();
  ++g_vblank;
  note_frame_skip_vblank();
  poll_events();
  update_cursor();
  if (g_force_480p)
    gt4::display::force_race_480p_at_vblank(ps2::g_ram, ps2::kRamSize, g_force_480p_state);
  // A replay drives the pad on its own (IN-520); so do scripted presses (--press), as in the headless runner.
  if (g_player) {
    apply_pad(g_player->at(g_vblank));
  } else if (ps2::g_options.presses.empty()) {
    // The wheel resolves which devices it owns first, so the gamepad path never reads them too.
    if (g_wheel) {
      g_wheel->prepare(SDL_GetKeyboardFocus() == g_window);
      if (g_pad && owned_by_wheel(SDL_GetGamepadID(g_pad))) {
        close_gamepad();
        open_gamepad();
      }
    }
    const gt4::pad::PadState pad = read_pad();
    apply_pad(pad);
    g_recorder.vblank(g_vblank, pad);
  }
  update_rumble();
  if (g_audio) {
    // Paced mode: rate control and moving ring data to the device, without waiting.
    g_audio->pump();
    g_diagnostics.record_queue_frames(g_audio->queued_frames());
    if (g_audio->mode() == gt4::pacing::Mode::paced)
      g_diagnostics.record_ratio(g_audio->ratio());
  }
  if (g_report_requested) {
    g_report_requested = false;
    report_pacing();
  }
  ps2::gs::present();
}

// Closes the sound and the controller. The window and SDL stay open until the process ends: the native Vulkan context
// is a global of the port (g_vulkan) whose destructor, at exit, releases the swapchain and surface built on this
// window, so destroying the window or the X11/Wayland connection first would crash that destructor.
void close_all() {
  g_wheel.reset(); // stops the forces and releases the haptic device before its joystick closes
  close_gamepad();
  g_audio.reset();
}

} // namespace

#ifdef GT4_MEMCARD_FLUSH_OVERLAY
namespace ps2::iop::sio2 {
void flush_cards_now(); // memcard capsule sio2.cpp (scripts/prepare_memcard_flush.py)
}
#endif

// IN-520: opens the recording or the replay; false stops the start, since a bench must not run with half its input.
bool open_input_files() {
  const char *record = std::getenv("GT4_INPUT_RECORD");
  const char *replay = std::getenv("GT4_INPUT_REPLAY");
  std::string error;
  if (record != nullptr && replay != nullptr) {
    std::fprintf(stderr, "[input] GT4_INPUT_RECORD and GT4_INPUT_REPLAY together, game not started\n");
    return false;
  }
  if (record != nullptr && !g_recorder.open(record, error)) {
    std::fprintf(stderr, "[input] %s, game not started\n", error.c_str());
    return false;
  }
  if (record != nullptr)
    std::fprintf(stderr, "[input] recording to %s; F9 ends it and closes the game\n", record);
  if (replay != nullptr) {
    g_player = std::make_unique<gt4::input_recording::Player>();
    if (!g_player->load(replay, error)) {
      std::fprintf(stderr, "[input] %s: %s, game not started\n", replay, error.c_str());
      return false;
    }
    std::fprintf(stderr, "[input] replaying %s: %zu changes up to VBlank %llu\n", replay, g_player->changes(),
                 static_cast<unsigned long long>(g_player->end()));
  }
  return true;
}

bool open_window(bool fullscreen) {
  if (!open_input_files())
    return false;
  try {
    g_controls = gt4::controls::load_controls_from_environment();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "[input] %s\n", error.what());
    return false;
  }
  // The launcher's GT4_FULLSCREEN (its Display setting) and GT4_OUTPUT_SIZE (the window size, WxH), as window.cpp reads
  // them; an invalid value stops the start. Without GT4_OUTPUT_SIZE the window opens at 1280x720.
  gt4::display_aspect::OutputSize output;
  try {
    fullscreen = fullscreen || gt4::window_mode::parse_start_fullscreen(std::getenv("GT4_FULLSCREEN"));
    output = gt4::display_aspect::configured_output_size();
    g_cursor = gt4::cursor::AutoHide(gt4::cursor::parse_policy(std::getenv("GT4_CURSOR_AUTOHIDE")));
  } catch (const std::exception &error) {
    std::fprintf(stderr, "[window] %s, game not started\n", error.what());
    return false;
  }
  std::fprintf(stderr, "[window] cursor auto-hide %s (GT4_CURSOR_AUTOHIDE; %u ms without movement over the picture)\n",
               g_cursor.policy().auto_hide ? "on" : "off", g_cursor.policy().hide_delay_ms);
  // A wheel file the parser refuses never stops the game: the error is logged and the game runs without the wheel.
  const gt4::wheel::WheelFileResult wheel_file = gt4::wheel::load_wheel_from_environment();
  g_wheel_config = wheel_file.config;
  if (!wheel_file.error.empty())
    std::fprintf(stderr, "[wheel] wheel.toml refused, the game runs without the wheel: %s\n", wheel_file.error.c_str());
  else
    std::fprintf(stderr, "[wheel] %s (enabled=%s, preset=%s)\n",
                 wheel_file.path.empty() ? "no wheel.toml" : "wheel.toml found", g_wheel_config.enabled ? "true" : "false",
                 g_wheel_config.preset.c_str());
  for (const auto &binding : gt4::controls::keyboard_bindings(g_controls))
    g_keys.push_back({gt4::keyboard::scancodes_for_virtual_key(binding.virtual_key), binding.button_mask});
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
    std::fprintf(stderr, "[window] SDL: %s\n", SDL_GetError());
    return false;
  }
  // SDL_WINDOW_VULKAN keeps SDL from putting its own GL/EGL surface on the window the Vulkan backend presents to.
  const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                (fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
  g_window = SDL_CreateWindow("GT4Recomp (Linux preview)", output.known() ? static_cast<int>(output.width) : 1280,
                              output.known() ? static_cast<int>(output.height) : 720, flags);
  if (!g_window) {
    std::fprintf(stderr, "[window] SDL: %s\n", SDL_GetError());
    SDL_Quit();
    return false;
  }
  if (fullscreen)
    SDL_HideCursor();
  publish_refresh_rate();
  try {
    g_pacing_mode = gt4::pacing::parse_mode(std::getenv("GT4_PACING"));
    // The display switches are read here as on Windows (window.cpp open_window), so an invalid value stops the start.
    gt4::display_aspect::configured_setting();
    g_force_480p = gt4::display::parse_force_480p(std::getenv("GT4_FORCE_480P"));
    gt4::display::widescreen_hold_enabled();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "[window] %s\n", error.what());
    return false;
  }
  if (g_force_480p)
    std::fprintf(stderr, "[display] GT4_FORCE_480P: races held at 480p\n");
  g_clock = std::make_unique<FrameClock>();
  g_pacer = std::make_unique<gt4::pacing::FramePacer>(g_clock->frequency(), gt4::pacing::guest_vblank_rate);
  const char *audio_choice = std::getenv("GT4_AUDIO");
  if (!(audio_choice && std::strcmp(audio_choice, "none") == 0)) {
    g_audio = std::make_unique<gt4::linux_host::SdlAudioOutput>(g_pacing_mode);
    if (g_audio->silent())
      g_audio.reset();
  }
  // Without sound the audio mode has no clock: the frame clock paces instead (as on Windows).
  if (!g_audio && g_pacing_mode == gt4::pacing::Mode::audio) {
    std::fprintf(stderr, "[pacing] no sound device: paced by the frame clock\n");
    g_pacing_mode = gt4::pacing::Mode::paced;
  }
  std::fprintf(stderr, "[pacing] %s pacing, %llu Hz grid, monotonic clock with nanosleep\n",
               gt4::pacing::mode_name(g_pacing_mode),
               static_cast<unsigned long long>(gt4::pacing::guest_vblank_rate));
  open_gamepad();
  std::fprintf(stderr, "[window] SDL %d.%d.%d, video driver %s\n", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
               SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
               SDL_GetCurrentVideoDriver());
  return true;
}

bool install_input_replay_without_window() {
  // As native/window.cpp: recording needs the window (its keyboard and pads); a replay runs without one.
  if (std::getenv("GT4_INPUT_RECORD") != nullptr) {
    std::fprintf(stderr, "[input] GT4_INPUT_RECORD needs --window, game not started\n");
    return false;
  }
  if (std::getenv("GT4_INPUT_REPLAY") != nullptr) {
    if (!open_input_files())
      return false;
    ps2::g_options.on_vblank = [] {
      apply_pad(g_player->at(++g_vblank));
      note_frame_skip_vblank();
    };
    return true;
  }
  // As native/window.cpp: without a window the frame skip still gets its VBlanks (no pacer: it leaves nothing out unless
  // GT4_FRAME_SKIP_FORCE sets a fixed pattern).
  if (gt4::frame_skip::g_enabled) {
    ps2::g_options.on_vblank = [] {
      ++g_vblank;
      note_frame_skip_vblank();
    };
  }
  return true;
}

int run_window(const std::function<int()> &emulate) {
  ps2::init_graphics();
  const SDL_PropertiesID properties = SDL_GetWindowProperties(g_window);
  const char *driver = SDL_GetCurrentVideoDriver();
  int kind = -1;
  void *display = nullptr;
  void *surface = nullptr;
  std::uint64_t window = 0;
  if (driver && std::strcmp(driver, "x11") == 0) {
    kind = kSurfaceX11;
    display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    window = static_cast<std::uint64_t>(SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  } else if (driver && std::strcmp(driver, "wayland") == 0) {
    kind = kSurfaceWayland;
    display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
    surface = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
  } else {
    std::fprintf(stderr, "[window] video driver '%s' has no Vulkan surface here; use X11 or Wayland\n",
                 driver ? driver : "none");
    close_all();
    return 1;
  }
  int width = 0, height = 0;
  SDL_GetWindowSizeInPixels(g_window, &width, &height);
  if (!ps2::gs::attach_window(kind, display, window, surface, nullptr, static_cast<std::uint32_t>(width),
                              static_cast<std::uint32_t>(height))) {
    std::fprintf(stderr, "[window] the native Vulkan backend refused the window\n");
    close_all();
    return 1;
  }
  // The presentation is 16:9 pillarbox, as on Windows; the display aspect widens the race view from the window size.
  ps2::gs::gpu::set_widescreen(true);
  gt4::display_aspect::set_window_client_size(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  gt4::display_aspect::set_presentation_sink(present_race_view);
  // GT4_RENDERER=hw: the port's GS stream hook gave the window to CoreGS 2.0 (native/hw_gs); otherwise the donor-derived
  // native Vulkan backend (CoreGS 1.1) presents.
  const char *renderer = std::getenv("GT4_RENDERER");
  const bool coregs = renderer != nullptr && std::strcmp(renderer, "hw") == 0;
  std::fprintf(stderr, "[window] guest output through %s (%s, %dx%d)\n",
               coregs ? "CoreGS 2.0 (GT4_RENDERER=hw)" : "the native Vulkan backend (CoreGS 1.1)", driver, width, height);
  // Before the guest runs: an enabled wheel plugs the virtual DFP in before the IOP's USB driver first looks.
  g_wheel = std::make_unique<gt4::wheel::WheelInput>(g_wheel_config);
  ps2::g_options.on_vblank = on_vblank;
  if (g_audio)
    ps2::set_audio_sink([](const std::int16_t *samples, std::size_t frames) { g_audio->push(samples, frames); });
  int result = 1;
  try {
    result = emulate();
  } catch (...) {
    ps2::set_audio_sink({});
    ps2::g_options.on_vblank = {};
    close_all();
    throw;
  }
  // TASK-058, as native/window.cpp: a save made less than 50 VBlanks before the window closed is written now (atomically,
  // native/memcard_store.cpp). emulate() returns normally after halt(), so this runs on every normal exit.
#ifdef GT4_MEMCARD_FLUSH_OVERLAY
  ps2::iop::sio2::flush_cards_now();
#endif
  g_recorder.finish(g_vblank);
  ps2::set_audio_sink({});
  ps2::g_options.on_vblank = {};
  report_pacing();
  close_all();
  return result;
}
