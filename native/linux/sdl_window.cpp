// TASK-074 L2: the Linux game window, through SDL3. It implements native/window.hpp for the port's main.cpp, as
// native/window.cpp does on Windows, and keeps that host's shape: one thread, and at every guest VBlank the window
// events, the pad, the presentation and the pacing. The SDL handling of X11/Wayland handles, audio and the controller
// follows the MIT donor's host/window.cpp (ps2-web-recomp, SDL2), ported to SDL3.
//
// What it has: an X11 or Wayland window presented by the native Vulkan GS backend (16:9, FIFO), the controls table
// (GT4_CONTROLS_FILE) for the keyboard and the first SDL3 gamepad (GT4_PAD selects one), rumble, 48 kHz stereo audio
// that paces the game, F11 and Alt+Enter fullscreen. What it does not have yet (later TASK-074 steps): the Windows
// host's frame clock and audio rate control, the game patches (widescreen hooks, 360 camera, FoV), racing wheels,
// RetroAchievements, the F7/F8 performance probe, input recording and the GS process (gt4-gs).

#include "window.hpp"

#include "controls.hpp"
#include "gpu.hpp"
#include "gs/gs.hpp"
#include "keyboard_scancodes.hpp"
#include "pad_mapping.hpp"
#include "ps2/system.hpp"
#include "sdl_gamepad.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace {

// The guest's audio: 48 kHz, 16-bit, stereo (ps2::set_audio_sink).
constexpr int kAudioRate = 48000;
constexpr int kAudioChannels = 2;
constexpr int kAudioBytesPerMs = kAudioRate / 1000 * kAudioChannels * 2;
// The audio device clock paces the game: past this much queued sound the VBlank waits (the donor's value).
constexpr int kPaceAheadMs = 80;
// Sound beyond this much is dropped instead of queued, so a stall cannot build a long delay.
constexpr int kDropAboveMs = 500;
// SurfaceTarget kinds of the derived gpu.hpp (enum Kind { X11, Wayland, Canvas, Win32 }).
constexpr int kSurfaceX11 = 0;
constexpr int kSurfaceWayland = 1;

struct KeyBinding {
  std::array<std::uint16_t, 2> scancodes;
  std::uint16_t button_mask;
};

SDL_Window *g_window = nullptr;
SDL_AudioStream *g_audio = nullptr;
SDL_Gamepad *g_pad = nullptr;
gt4::controls::ControlMap g_controls;
std::vector<KeyBinding> g_keys;
std::uint16_t g_rumble = 0;
std::uint64_t g_next_vblank_ns = 0;

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
      ps2::halt("window closed");
      break;
    case SDL_EVENT_KEY_DOWN:
      if (!event.key.repeat &&
          (event.key.scancode == SDL_SCANCODE_F11 ||
           (event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT))))
        toggle_fullscreen();
      break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      ps2::gs::resize_window(static_cast<std::uint32_t>(event.window.data1),
                             static_cast<std::uint32_t>(event.window.data2));
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

int queued_audio_ms() {
  return g_audio ? SDL_GetAudioStreamQueued(g_audio) / kAudioBytesPerMs : 0;
}

void pace() {
  if (g_audio) {
    while (queued_audio_ms() > kPaceAheadMs)
      SDL_Delay(1);
    return;
  }
  // Without audio: the NTSC field rate, 60000/1001 Hz (PAL 50 Hz).
  const std::uint64_t period_ns = ps2::g_options.pal ? 20000000ull : 16683333ull;
  const std::uint64_t now = SDL_GetTicksNS();
  if (g_next_vblank_ns && now < g_next_vblank_ns) {
    SDL_DelayPrecise(g_next_vblank_ns - now);
    g_next_vblank_ns += period_ns;
  } else {
    g_next_vblank_ns = now + period_ns;
  }
}

void on_vblank() {
  poll_events();
  // Scripted presses (--press) drive the pad on their own, as in the headless runner.
  if (ps2::g_options.presses.empty())
    apply_pad(read_pad());
  update_rumble();
  ps2::gs::present();
  pace();
}

void queue_audio(const std::int16_t *samples, std::size_t frames) {
  if (queued_audio_ms() > kDropAboveMs)
    return;
  SDL_PutAudioStreamData(g_audio, samples, static_cast<int>(frames * kAudioChannels * 2));
}

// Closes the sound and the controller. The window and SDL stay open until the process ends: the native Vulkan context
// is a global of the port (g_vulkan) whose destructor, at exit, releases the swapchain and surface built on this
// window, so destroying the window or the X11/Wayland connection first would crash that destructor.
void close_all() {
  close_gamepad();
  if (g_audio) {
    SDL_DestroyAudioStream(g_audio);
    g_audio = nullptr;
  }
}

} // namespace

bool open_window(bool fullscreen) {
  try {
    g_controls = gt4::controls::load_controls_from_environment();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "[input] %s\n", error.what());
    return false;
  }
  for (const auto &binding : gt4::controls::keyboard_bindings(g_controls))
    g_keys.push_back({gt4::keyboard::scancodes_for_virtual_key(binding.virtual_key), binding.button_mask});
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
    std::fprintf(stderr, "[window] SDL: %s\n", SDL_GetError());
    return false;
  }
  // SDL_WINDOW_VULKAN keeps SDL from putting its own GL/EGL surface on the window the Vulkan backend presents to.
  const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                (fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
  g_window = SDL_CreateWindow("GT4Recomp (Linux preview)", 1280, 720, flags);
  if (!g_window) {
    std::fprintf(stderr, "[window] SDL: %s\n", SDL_GetError());
    SDL_Quit();
    return false;
  }
  if (fullscreen)
    SDL_HideCursor();
  const char *audio_choice = std::getenv("GT4_AUDIO");
  if (!(audio_choice && std::strcmp(audio_choice, "none") == 0)) {
    const SDL_AudioSpec spec{SDL_AUDIO_S16, kAudioChannels, kAudioRate};
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (g_audio)
      SDL_ResumeAudioStreamDevice(g_audio);
    else
      std::fprintf(stderr, "[audio] no audio device (%s); paced by the clock\n", SDL_GetError());
  }
  open_gamepad();
  std::fprintf(stderr, "[window] SDL %d.%d.%d, video driver %s\n", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
               SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
               SDL_GetCurrentVideoDriver());
  return true;
}

bool install_input_replay_without_window() {
  if (std::getenv("GT4_INPUT_RECORD") || std::getenv("GT4_INPUT_REPLAY")) {
    std::fprintf(stderr, "[input] input recording and replay are not on Linux yet; game not started\n");
    return false;
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
  // The presentation is 16:9 pillarbox, as on Windows.
  ps2::gs::gpu::set_widescreen(true);
  std::fprintf(stderr, "[window] guest GPU output through native Vulkan (%s, %dx%d)\n", driver, width, height);
  ps2::g_options.on_vblank = on_vblank;
  if (g_audio)
    ps2::set_audio_sink(queue_audio);
  int result = 1;
  try {
    result = emulate();
  } catch (...) {
    ps2::set_audio_sink({});
    ps2::g_options.on_vblank = {};
    close_all();
    throw;
  }
  ps2::set_audio_sink({});
  ps2::g_options.on_vblank = {};
  close_all();
  return result;
}
