// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// TASK-074 (Linux): the hw_hud_system.hpp interface without the Windows APIs behind hw_hud_system.cpp (GDI for the
// font, PDH for the graphics card, the monitor's refresh rate). The Linux backend keeps the HUD off
// (hw_gs_backend.cpp, hud_wanted), so these only let the HUD layer link: an empty font (Windows' own result when it
// cannot draw the glyphs) and a sampler that publishes nothing.
#include "hw_hud_system.hpp"

namespace gt4::hw::hud {

GlyphAtlas make_glyph_atlas(int) { return {}; }

SystemSampler::SystemSampler(void *window, std::function<void(const SystemReading &)> publish)
    : window_(window), publish_(std::move(publish)) {}

SystemSampler::~SystemSampler() = default;

void SystemSampler::run() {}

}  // namespace gt4::hw::hud
