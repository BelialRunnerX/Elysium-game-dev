#include "game/Game.hpp"

#include "core/Determinism.hpp"
#include "world/Block.hpp"
#include "world/EarlyMetallurgy.hpp"
#include "world/PlanetEnvironment.hpp"
#include "world/BiomeCatalog.hpp"
#include "world/SurfaceChunkPersistence.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace elysium {
namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr const char* kSavePath = "elysium_save.txt";
constexpr const char* kSurfaceChunkSaveRoot = "elysium_save_chunks";

Vector3 rv(Vec3 v) { return {v.x, v.y, v.z}; }
Color rc(Color4u c) { return {c.r,c.g,c.b,c.a}; }

float distance2D(Vec3 a, Vec3 b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx*dx + dz*dz);
}

// CONCEPT v0.7 Present chrome — glass panels + cyan accents (raylib approximations).
constexpr Color kGlassFill{36, 48, 62, 150};
constexpr Color kGlassEdge{170, 200, 220, 110};
constexpr Color kCyanAccent{0, 245, 255, 255};
constexpr Color kCyanFill{0, 220, 245, 230};
constexpr Color kCyanSoft{80, 210, 230, 180};
constexpr Color kBarTrack{18, 28, 38, 170};
constexpr float kVitalMax = 100.0f;
// Present-local display thresholds only — not survival authority (see CHANGE_NOTE Pass 5).
constexpr float kVitalWarn = 50.0f;
constexpr float kVitalCritical = 25.0f;
constexpr Color kVitalAmberFill{245, 175, 55, 230};
constexpr Color kVitalAmberEdge{255, 190, 70, 220};
constexpr Color kVitalRedFill{235, 70, 70, 235};
constexpr Color kVitalRedEdge{255, 90, 90, 240};

enum class VitalUrgency : std::uint8_t { Normal = 0, Warn = 1, Critical = 2 };

VitalUrgency vitalUrgencyFromSnapshot(float cur) {
    if (cur < kVitalCritical) return VitalUrgency::Critical;
    if (cur < kVitalWarn) return VitalUrgency::Warn;
    return VitalUrgency::Normal;
}

Color vitalBarFill(VitalUrgency u) {
    switch (u) {
        case VitalUrgency::Critical: return kVitalRedFill;
        case VitalUrgency::Warn: return kVitalAmberFill;
        default: return kCyanFill;
    }
}

Color vitalLabelColor(VitalUrgency u) {
    switch (u) {
        case VitalUrgency::Critical: return Color{255, 170, 170, 255};
        case VitalUrgency::Warn: return Color{255, 220, 160, 255};
        default: return RAYWHITE;
    }
}

void drawGlassPanel(int x, int y, int w, int h) {
    const Rectangle r{static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)};
    DrawRectangleRounded(r, 0.18f, 8, kGlassFill);
    DrawRectangleRoundedLines(r, 0.18f, 8, kGlassEdge);
}

void drawVitalRow(int x, int y, int rowW, int rowH, float cur, const char* label, const char* icon) {
    const float clamped = std::clamp(cur, 0.0f, kVitalMax);
    const VitalUrgency urgency = vitalUrgencyFromSnapshot(clamped);
    const int iconW = 24;
    const int labelW = 78;
    const int numW = 72;
    const int gap = 6;
    const int barX = x + iconW + labelW + gap;
    const int barW = rowW - iconW - labelW - numW - gap * 3;
    const int barH = rowH - 8;
    const int barY = y + 4;

    const Color labelCol = vitalLabelColor(urgency);
    DrawText(icon, x, y + 5, 16, labelCol);
    DrawText(label, x + iconW + 2, y + 5, 14, labelCol);

    const Rectangle rowR{static_cast<float>(x - 2), static_cast<float>(y), static_cast<float>(rowW + 4), static_cast<float>(rowH)};
    if (urgency == VitalUrgency::Critical) {
        const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(GetTime()) * 9.0f);
        const unsigned char edgeA = static_cast<unsigned char>(std::clamp(140.0f + 100.0f * pulse, 0.0f, 255.0f));
        Color edge = kVitalRedEdge;
        edge.a = edgeA;
        DrawRectangleRoundedLinesEx(rowR, 0.28f, 6, 2.5f + pulse, edge);
    } else if (urgency == VitalUrgency::Warn) {
        DrawRectangleRoundedLinesEx(rowR, 0.28f, 6, 1.5f, kVitalAmberEdge);
    }

    DrawRectangleRounded(Rectangle{static_cast<float>(barX), static_cast<float>(barY), static_cast<float>(barW), static_cast<float>(barH)}, 0.4f, 6, kBarTrack);
    const int fillW = static_cast<int>(static_cast<float>(barW) * (clamped / kVitalMax));
    if (fillW > 0) {
        Color fill = vitalBarFill(urgency);
        if (urgency == VitalUrgency::Critical) {
            const float pulse = 0.65f + 0.35f * std::sin(static_cast<float>(GetTime()) * 9.0f);
            fill.a = static_cast<unsigned char>(std::clamp(160.0f + 75.0f * pulse, 0.0f, 255.0f));
        }
        DrawRectangleRounded(Rectangle{static_cast<float>(barX), static_cast<float>(barY), static_cast<float>(fillW), static_cast<float>(barH)}, 0.4f, 6, fill);
    }

    const char* num = TextFormat("%.0f / %.0f", clamped, kVitalMax);
    const int nw = MeasureText(num, 14);
    DrawText(num, x + rowW - nw, y + 5, 14, labelCol);
}

// Present overlay only — soft screen-edge vignette when HEALTH or OXYGEN is critical.
void drawSurvivalUrgencyVignette(int sw, int sh, float health, float oxygen) {
    const VitalUrgency hu = vitalUrgencyFromSnapshot(health);
    const VitalUrgency ou = vitalUrgencyFromSnapshot(oxygen);
    if (hu != VitalUrgency::Critical && ou != VitalUrgency::Critical) return;

    const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(GetTime()) * 5.5f);
    const int band = std::max(28, std::min(sw, sh) / 14);
    const unsigned char a = static_cast<unsigned char>(std::clamp(28.0f + 42.0f * pulse, 0.0f, 90.0f));
    const Color veil{120, 18, 22, a};
    DrawRectangle(0, 0, sw, band, veil);
    DrawRectangle(0, sh - band, sw, band, veil);
    DrawRectangle(0, band, band, sh - band * 2, veil);
    DrawRectangle(sw - band, band, band, sh - band * 2, veil);
}

void drawSurvivalUrgencyChip(int x, int y, float health, float oxygen, float energy) {
    struct Candidate { VitalUrgency u; const char* text; };
    // Prefer HEALTH > OXYGEN > ENERGY when urgency ties — one stable chip, not a toast.
    const Candidate cands[] = {
        {vitalUrgencyFromSnapshot(health), health < kVitalCritical ? "CRITICAL HEALTH" : "LOW HEALTH"},
        {vitalUrgencyFromSnapshot(oxygen), oxygen < kVitalCritical ? "CRITICAL O2" : "LOW O2"},
        {vitalUrgencyFromSnapshot(energy), energy < kVitalCritical ? "CRITICAL ENERGY" : "LOW ENERGY"},
    };
    VitalUrgency best = VitalUrgency::Normal;
    const char* chip = nullptr;
    for (const auto& c : cands) {
        if (static_cast<int>(c.u) > static_cast<int>(best)) {
            best = c.u;
            chip = c.text;
        }
    }
    if (best == VitalUrgency::Normal || chip == nullptr) return;

    const int fontSize = 14;
    const int tw = MeasureText(chip, fontSize);
    const int padX = 10;
    const int padY = 5;
    const int cw = tw + padX * 2;
    const int ch = fontSize + padY * 2;
    const Rectangle r{static_cast<float>(x), static_cast<float>(y), static_cast<float>(cw), static_cast<float>(ch)};
    const Color fill = (best == VitalUrgency::Critical) ? Color{70, 22, 28, 200} : Color{62, 48, 22, 190};
    const Color edge = (best == VitalUrgency::Critical) ? kVitalRedEdge : kVitalAmberEdge;
    const Color text = (best == VitalUrgency::Critical) ? Color{255, 190, 190, 255} : Color{255, 220, 160, 255};
    DrawRectangleRounded(r, 0.35f, 6, fill);
    DrawRectangleRoundedLinesEx(r, 0.35f, 6, 1.5f, edge);
    DrawText(chip, x + padX, y + padY, fontSize, text);
}

void drawConceptVitals(int x, int y, int sw, int sh, float health, float oxygen, float energy) {
    drawSurvivalUrgencyVignette(sw, sh, health, oxygen);

    constexpr int panelW = 310;
    constexpr int panelH = 108;
    constexpr int pad = 10;
    constexpr int rowH = 28;
    drawGlassPanel(x, y, panelW, panelH);
    drawVitalRow(x + pad, y + pad, panelW - pad * 2, rowH, health, "HEALTH", "+");
    drawVitalRow(x + pad, y + pad + rowH, panelW - pad * 2, rowH, oxygen, "OXYGEN", "O");
    drawVitalRow(x + pad, y + pad + rowH * 2, panelW - pad * 2, rowH, energy, "ENERGY", "Z");

    // Chip to the right of the panel — stable while urgency holds (not a per-frame toast).
    drawSurvivalUrgencyChip(x + panelW + 8, y + pad, health, oxygen, energy);
}

struct HotbarSlotView {
    const char* abbrev;
    int count;
    bool occupied;
};

int conceptHotbarSelectedIndex(BlockType selectedBlock, bool machineMode, bool portalMode, MachineType selectedMachine) {
    if (portalMode) return -1;
    if (!machineMode) {
        switch (selectedBlock) {
            case BlockType::Dirt: return 0;
            case BlockType::Stone: return 1;
            case BlockType::Planks: return 2;
            case BlockType::SteelPlate: return 3;
            case BlockType::RegistryBeacon: return 4;
            default: return -1;
        }
    }
    switch (selectedMachine) {
        case MachineType::BurnerGenerator: return 5;
        case MachineType::BatteryBank: return 6;
        case MachineType::AtmosphereUnit: return 7;
        case MachineType::StorageCrate: return 8;
        default: return -1;
    }
}

void fillConceptHotbarSlots(HotbarSlotView out[9], const EcsWorld& ecs) {
    const BlockType blocks[5] = {
        BlockType::Dirt, BlockType::Stone, BlockType::Planks, BlockType::SteelPlate, BlockType::RegistryBeacon
    };
    const char* blockAbbrev[5] = {"DRT", "STN", "PLK", "STL", "BCN"};
    for (int i = 0; i < 5; ++i) {
        out[i].abbrev = blockAbbrev[i];
        out[i].count = ecs.inventoryCount(static_cast<int>(blocks[i]));
        out[i].occupied = true;
    }
    const MachineType machines[4] = {
        MachineType::BurnerGenerator, MachineType::BatteryBank, MachineType::AtmosphereUnit, MachineType::StorageCrate
    };
    const char* machineAbbrev[4] = {"BRN", "BAT", "ATM", "CRT"};
    for (int i = 0; i < 4; ++i) {
        out[5 + i].abbrev = machineAbbrev[i];
        out[5 + i].count = ecs.inventoryCount(machineItemId(machines[i]));
        out[5 + i].occupied = true;
    }
}

void drawConceptHotbar(int sw, int sh, int selectedSlot, const HotbarSlotView slots[9], const char* modeHint) {
    constexpr int slot = 52;
    constexpr int gap = 5;
    constexpr int count = 9;
    const int totalW = count * slot + (count - 1) * gap;
    const int panelPad = 8;
    const int panelW = totalW + panelPad * 2;
    const int panelH = slot + panelPad * 2;
    const int panelX = sw / 2 - panelW / 2;
    const int panelY = sh - panelH - 14;

    if (modeHint && modeHint[0] != '\0') {
        const int tw = MeasureText(modeHint, 14);
        DrawText(modeHint, sw / 2 - tw / 2, panelY - 20, 14, kCyanSoft);
    }

    drawGlassPanel(panelX, panelY, panelW, panelH);

    for (int i = 0; i < count; ++i) {
        const int sx = panelX + panelPad + i * (slot + gap);
        const int sy = panelY + panelPad;
        const Rectangle sr{static_cast<float>(sx), static_cast<float>(sy), static_cast<float>(slot), static_cast<float>(slot)};
        DrawRectangleRounded(sr, 0.22f, 6, Color{22, 32, 44, 160});
        DrawRectangleRoundedLines(sr, 0.22f, 6, Color{140, 170, 190, 90});

        if (i == selectedSlot) {
            DrawRectangleRoundedLinesEx(sr, 0.22f, 6, 3.0f, kCyanAccent);
        }

        DrawText(TextFormat("%d", i + 1), sx + 4, sy + 3, 12, Color{220, 230, 240, 180});

        if (slots[i].occupied) {
            const int aw = MeasureText(slots[i].abbrev, 14);
            DrawText(slots[i].abbrev, sx + (slot - aw) / 2, sy + slot / 2 - 8, 14, RAYWHITE);
            if (slots[i].count > 0) {
                const char* cq = TextFormat("%d", slots[i].count);
                const int cw = MeasureText(cq, 12);
                DrawText(cq, sx + slot - cw - 4, sy + slot - 16, 12, RAYWHITE);
            }
        }
    }
}

void drawCompassStub(int sw, float yawRadians) {
    // yaw_=0 faces +Z; treat as North. Positive yaw turns toward +X (East).
    float heading = std::fmod(yawRadians * (180.0f / kPi), 360.0f);
    if (heading < 0.0f) heading += 360.0f;

    const int cx = sw / 2;
    const int cy = 22;
    const float pxPerDeg = 2.4f;
    const float halfSpan = 150.0f;

    // Heading caret (downward triangle) — luminous cyan.
    DrawTriangle(
        Vector2{static_cast<float>(cx), static_cast<float>(cy - 2)},
        Vector2{static_cast<float>(cx - 7), static_cast<float>(cy - 12)},
        Vector2{static_cast<float>(cx + 7), static_cast<float>(cy - 12)},
        kCyanAccent);

    struct DirMark { float deg; const char* label; };
    const DirMark marks[] = {
        {0.f, "N"}, {45.f, "NE"}, {90.f, "E"}, {135.f, "SE"},
        {180.f, "S"}, {225.f, "SW"}, {270.f, "W"}, {315.f, "NW"}
    };

    for (const auto& m : marks) {
        float delta = m.deg - heading;
        while (delta > 180.0f) delta -= 360.0f;
        while (delta < -180.0f) delta += 360.0f;
        const float x = static_cast<float>(cx) + delta * pxPerDeg;
        if (x < static_cast<float>(cx) - halfSpan || x > static_cast<float>(cx) + halfSpan) continue;

        const bool cardinal = (m.label[1] == '\0');
        const int fontSize = cardinal ? 16 : 13;
        const int tw = MeasureText(m.label, fontSize);
        DrawText(m.label, static_cast<int>(x) - tw / 2, cy + 2, fontSize, RAYWHITE);
        DrawLine(static_cast<int>(x), cy + (cardinal ? 20 : 18), static_cast<int>(x), cy + 26, Color{200, 220, 230, 160});
    }

    // Subtle tick marks every 15 degrees inside the strip window.
    for (int d = 0; d < 360; d += 15) {
        float delta = static_cast<float>(d) - heading;
        while (delta > 180.0f) delta -= 360.0f;
        while (delta < -180.0f) delta += 360.0f;
        const float x = static_cast<float>(cx) + delta * pxPerDeg;
        if (x < static_cast<float>(cx) - halfSpan || x > static_cast<float>(cx) + halfSpan) continue;
        if (d % 45 == 0) continue;
        DrawLine(static_cast<int>(x), cy + 22, static_cast<int>(x), cy + 26, Color{160, 180, 200, 100});
    }
}


void drawConceptBrand(int x, int y, const char* secondaryLine) {
    constexpr int panelW = 300;
    const bool hasSecondary = secondaryLine && secondaryLine[0] != '\0';
    const int panelH = hasSecondary ? 72 : 54;
    drawGlassPanel(x, y, panelW, panelH);
    DrawText("ELYSIUM", x + 14, y + 10, 24, RAYWHITE);
    DrawText("CONCEPT->v0.20", x + 14, y + 36, 13, kCyanSoft);
    if (hasSecondary) {
        DrawText(secondaryLine, x + 14, y + 52, 12, Color{173, 196, 185, 230});
    }
}

// Present-only sky/horizon readability — env table colors, no world mutation.
Color lerpByteColor(Color4u a, Color4u b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return Color{
        static_cast<unsigned char>(a.r + (b.r - a.r) * t),
        static_cast<unsigned char>(a.g + (b.g - a.g) * t),
        static_cast<unsigned char>(a.b + (b.b - a.b) * t),
        255};
}

void drawHorizonSkyGradient(int sw, int sh, Color4u sky, Color4u horizon) {
    // Full-width bottom-half fade: horizon near ground band → sky toward zenith.
    // Drawn after ClearBackground(sky) and before BeginMode3D so world meshes sit on top.
    const int bandTop = sh / 3;
    const int bandH = sh - bandTop;
    constexpr int kSteps = 40;
    for (int i = 0; i < kSteps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSteps - 1); // 0=sky, 1=horizon
        const Color c = lerpByteColor(sky, horizon, t);
        const int y0 = bandTop + (i * bandH) / kSteps;
        const int y1 = bandTop + ((i + 1) * bandH) / kSteps;
        DrawRectangle(0, y0, sw, std::max(1, y1 - y0 + 1), c);
    }
}

unsigned minimapHash(int x, int z) {
    unsigned h = static_cast<unsigned>(x) * 374761393u ^ static_cast<unsigned>(z) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

Color shadeEnv(Color4u base, float mul, unsigned char a) {
    const auto ch = [](int v) {
        return static_cast<unsigned char>(std::clamp(v, 0, 255));
    };
    return Color{ch(static_cast<int>(base.r * mul)), ch(static_cast<int>(base.g * mul)),
                 ch(static_cast<int>(base.b * mul)), a};
}

void drawConceptMinimap(int sw, Vec3 pos, float yawRadians, const char* biomeLabel,
                        const char* worldClassLabel, Color4u sky, Color4u horizon) {
    constexpr int panelW = 196;
    constexpr int panelH = 196;
    constexpr int pad = 10;
    const int px = sw - panelW - 16;
    const int py = 16;
    drawGlassPanel(px, py, panelW, panelH);

    // Fake day clock from GetTime (nice-to-have stub — not sim authority).
    const int daySec = static_cast<int>(GetTime() * 12.0f) % 86400;
    const int hh = (daySec / 3600) % 24;
    const int mm = (daySec / 60) % 60;
    DrawText(TextFormat("* %02d:%02d", hh, mm), px + pad, py + pad, 14, RAYWHITE);

    // World-class chip (TEMPERATE/BARREN/SCORCHED) — distinct from biome name chip.
    const char* worldClass = (worldClassLabel && worldClassLabel[0] != '\0') ? worldClassLabel : "WORLD";
    {
        // Uppercase-ish short label from env.name (already title case in table).
        const int cw = MeasureText(worldClass, 11);
        const int chipW = cw + 12;
        const int chipX = px + panelW - pad - chipW;
        const int chipY = py + pad - 1;
        const Rectangle chipR{
            static_cast<float>(chipX), static_cast<float>(chipY),
            static_cast<float>(chipW), 18.0f};
        DrawRectangleRounded(chipR, 0.35f, 4, Color{sky.r, sky.g, sky.b, 200});
        DrawRectangleRoundedLines(chipR, 0.35f, 4, Color{horizon.r, horizon.g, horizon.b, 220});
        DrawText(worldClass, chipX + 6, chipY + 3, 11, RAYWHITE);
    }

    const int mapX = px + pad;
    const int mapY = py + 34;
    const int mapW = panelW - pad * 2;
    const int mapH = panelH - 70;
    const Rectangle mapR{
        static_cast<float>(mapX), static_cast<float>(mapY),
        static_cast<float>(mapW), static_cast<float>(mapH)};
    DrawRectangleRounded(mapR, 0.12f, 6, shadeEnv(sky, 0.45f, 180));

    // Local fake terrain blobs hashed from player xz — planet-tinted (Present stub).
    constexpr int cell = 8;
    const int originX = static_cast<int>(std::floor(pos.x)) - mapW / (2 * cell);
    const int originZ = static_cast<int>(std::floor(pos.z)) - mapH / (2 * cell);
    const Color land = shadeEnv(horizon, 0.85f, 200);
    const Color landDark = shadeEnv(horizon, 0.55f, 200);
    const Color water = shadeEnv(sky, 0.75f, 210);
    for (int row = 0; row < mapH / cell; ++row) {
        for (int col = 0; col < mapW / cell; ++col) {
            const unsigned h = minimapHash(originX + col, originZ + row);
            const int wx = mapX + col * cell;
            const int wy = mapY + row * cell;
            Color c;
            if ((h & 7u) == 0u) {
                c = water;
            } else if ((h & 3u) == 0u) {
                c = landDark;
            } else {
                c = land;
            }
            DrawRectangle(wx, wy, cell - 1, cell - 1, c);
        }
    }

    // Cyan player marker at center, oriented from yaw_ (0 = +Z = North/up).
    const float cx = static_cast<float>(mapX) + static_cast<float>(mapW) * 0.5f;
    const float cy = static_cast<float>(mapY) + static_cast<float>(mapH) * 0.5f;
    const float ang = -yawRadians; // screen: up = north
    const float tipX = cx + std::sin(ang) * 9.0f;
    const float tipY = cy - std::cos(ang) * 9.0f;
    const float b1X = cx + std::sin(ang + 2.5f) * 7.0f;
    const float b1Y = cy - std::cos(ang + 2.5f) * 7.0f;
    const float b2X = cx + std::sin(ang - 2.5f) * 7.0f;
    const float b2Y = cy - std::cos(ang - 2.5f) * 7.0f;
    DrawTriangle(
        Vector2{tipX, tipY},
        Vector2{b1X, b1Y},
        Vector2{b2X, b2Y},
        kCyanAccent);
    DrawCircle(static_cast<int>(cx), static_cast<int>(cy), 2.5f, kCyanAccent);

    const char* coords = TextFormat("%.0f, %.0f, %.0f", pos.x, pos.y, pos.z);
    DrawText(coords, px + pad, py + panelH - 22, 13, RAYWHITE);

    const char* biome = (biomeLabel && biomeLabel[0] != '\0') ? biomeLabel : "UNKNOWN";
    const int bw = MeasureText(biome, 12);
    const int chipW = bw + 12;
    const int chipH = 18;
    const int chipX = px + panelW - pad - chipW;
    const int chipY = py + panelH - 24;
    const Rectangle chipR{
        static_cast<float>(chipX), static_cast<float>(chipY),
        static_cast<float>(chipW), static_cast<float>(chipH)};
    DrawRectangleRounded(chipR, 0.35f, 4, Color{horizon.r, horizon.g, horizon.b, 160});
    DrawRectangleRoundedLines(chipR, 0.35f, 4, kCyanSoft);
    DrawText(biome, chipX + 6, chipY + 3, 12, kCyanAccent);
}

void drawSuspicionCompact(int sw, float suspicionValue, float claimFloorValue, float favorValue,
                          bool claimed, bool beaconIntact) {
    // Relocated below CONCEPT minimap so top-right stays map-owned.
    constexpr int panelW = 196;
    constexpr int panelH = 78;
    const int px = sw - panelW - 16;
    const int py = 16 + 196 + 8;
    drawGlassPanel(px, py, panelW, panelH);
    DrawText(TextFormat("SUSPICION  %05.1f", suspicionValue), px + 10, py + 8, 14, RAYWHITE);
    DrawRectangleRounded(
        Rectangle{static_cast<float>(px + 10), static_cast<float>(py + 30), 176.0f, 10.0f},
        0.4f, 4, kBarTrack);
    const int fill = static_cast<int>(176.0f * std::clamp(suspicionValue, 0.0f, 100.0f) / 100.0f);
    if (fill > 0) {
        DrawRectangleRounded(
            Rectangle{static_cast<float>(px + 10), static_cast<float>(py + 30), static_cast<float>(fill), 10.0f},
            0.4f, 4, Color{164, 93, 85, 230});
    }
    DrawText(TextFormat("Floor %.1f  Favor %.0f", claimFloorValue, favorValue), px + 10, py + 46, 12, Color{190, 198, 194, 230});
    // Claim line keeps standing context; distinct intact/destroyed cue lives in beacon chip below.
    const char* claimLine = "CLAIM: UNFILED";
    Color claimCol{196, 196, 196, 230};
    if (claimed && beaconIntact) {
        claimLine = "CLAIM: FILED · INTACT";
        claimCol = Color{83, 224, 151, 255};
    } else if (claimed) {
        claimLine = "CLAIM: FILED · LOST";
        claimCol = Color{255, 150, 120, 255};
    }
    DrawText(claimLine, px + 10, py + 60, 12, claimCol);
}

// Pass 6 — distinct beacon intact vs claimed Present chip (snapshot reads only).
// Sits under Suspicion compact; does not occupy minimap top-right.
void drawBeaconPresentChip(int sw, bool claimed, bool beaconIntact, std::uint64_t stableBeaconId) {
    constexpr int panelW = 196;
    constexpr int suspicionH = 78;
    const int px = sw - panelW - 16;
    const int py = 16 + 196 + 8 + suspicionH + 6;

    const char* label = "BEACON  UNFILED";
    Color fill{36, 48, 62, 175};
    Color edge = kGlassEdge;
    Color text{200, 210, 220, 255};
    if (claimed && beaconIntact) {
        label = "BEACON  CLAIMED+INTACT";
        fill = Color{22, 55, 42, 200};
        edge = Color{83, 224, 151, 210};
        text = Color{150, 245, 200, 255};
    } else if (claimed && !beaconIntact) {
        label = "BEACON  CLAIMED+DESTROYED";
        fill = Color{70, 28, 24, 205};
        edge = Color{255, 120, 90, 230};
        text = Color{255, 190, 170, 255};
    }

    const int fontSize = 12;
    const int padX = 10;
    const int padY = 6;
    const int line2 = (claimed && stableBeaconId != 0) ? (fontSize + 4) : 0;
    const int ch = fontSize + padY * 2 + line2;
    const Rectangle r{static_cast<float>(px), static_cast<float>(py),
                      static_cast<float>(panelW), static_cast<float>(ch)};
    DrawRectangleRounded(r, 0.28f, 6, fill);
    DrawRectangleRoundedLinesEx(r, 0.28f, 6, 1.5f, edge);
    DrawText(label, px + padX, py + padY, fontSize, text);
    if (claimed && stableBeaconId != 0) {
        DrawText(TextFormat("id %04llX", static_cast<unsigned long long>(stableBeaconId & 0xFFFFull)),
                 px + padX, py + padY + fontSize + 2, 11, Color{170, 190, 200, 210});
    }
}

// Pass 6 — player-facing siege banner when director.active() (not Idle/Cleared/Failed alone).
void drawSiegePresentBanner(int sw, const SurfaceSiegeState& state, bool siegeActive) {
    if (!siegeActive) return;

    const char* typeName = imperialEnforcementTypeName(state.type);
    const char* bandName = imperialAttentionBandName(state.band);
    const char* phaseName = registerActionPhaseName(state.phase);
    const char* line = TextFormat("SIEGE  %s // %s // %s // WAVE %d/%d // %.0fs",
                                  typeName, bandName, phaseName,
                                  state.waveIndex + 1, std::max(1, state.totalWaves),
                                  state.phaseSecondsRemaining);

    const int fontSize = 15;
    const int tw = MeasureText(line, fontSize);
    const int padX = 16;
    const int padY = 8;
    const int bw = tw + padX * 2;
    const int bh = fontSize + padY * 2;
    const int bx = (sw - bw) / 2;
    const int by = 18;

    const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(GetTime()) * 4.5f);
    const bool hot = state.phase == RegisterActionPhase::WaveActive;
    Color fill = hot ? Color{72, 32, 28, static_cast<unsigned char>(185 + 40 * pulse)}
                     : Color{52, 40, 28, 190};
    Color edge = hot ? Color{255, 120, 90, static_cast<unsigned char>(180 + 60 * pulse)}
                     : Color{255, 190, 90, 210};
    Color text = hot ? Color{255, 200, 185, 255} : Color{255, 230, 180, 255};

    const Rectangle r{static_cast<float>(bx), static_cast<float>(by),
                      static_cast<float>(bw), static_cast<float>(bh)};
    DrawRectangleRounded(r, 0.22f, 8, fill);
    DrawRectangleRoundedLinesEx(r, 0.22f, 8, hot ? 2.0f : 1.5f, edge);
    DrawText(line, bx + padX, by + padY, fontSize, text);
}

// Pass 6 — ship cargo / takeoff envelope chip (checklist snapshot; no travel authority).
// Visible near ship, or when menu closed and envelope blocked / cargo high.
void drawShipEnvelopePresentChip(int sw, int sh, const ShipTakeoffChecklist& check,
                                 bool nearShip, bool travelMenuOpen) {
    if (travelMenuOpen) return;

    const float cargoRatio = (check.maxCargoForTakeoff > 0.0f)
        ? (check.cargoMassUnits / check.maxCargoForTakeoff) : 0.0f;
    const bool cargoHigh = cargoRatio >= 0.75f;
    const bool blocked = !check.ready;
    if (!nearShip && !blocked && !cargoHigh) return;

    const char* status = "READY";
    Color fill{28, 48, 52, 195};
    Color edge = kCyanSoft;
    Color text = kCyanAccent;
    if (!check.hullOk && !check.cargoOk) {
        status = "BLOCKED · HULL+CARGO";
        fill = Color{70, 28, 24, 205};
        edge = kVitalRedEdge;
        text = Color{255, 190, 170, 255};
    } else if (!check.hullOk) {
        status = "BLOCKED · HULL";
        fill = Color{70, 28, 24, 205};
        edge = kVitalRedEdge;
        text = Color{255, 190, 170, 255};
    } else if (!check.cargoOk) {
        status = "BLOCKED · CARGO";
        fill = Color{70, 32, 22, 205};
        edge = Color{255, 140, 90, 230};
        text = Color{255, 200, 170, 255};
    } else if (cargoHigh) {
        status = "CARGO HIGH";
        fill = Color{62, 48, 22, 195};
        edge = kVitalAmberEdge;
        text = Color{255, 220, 160, 255};
    }

    const char* line = TextFormat("SHIP  hull %.0f/%.0f  cargo %.0f/%.0f  %s",
                                  check.hullIntegrity, check.minHullForTakeoff,
                                  check.cargoMassUnits, check.maxCargoForTakeoff,
                                  status);
    const int fontSize = 13;
    const int tw = MeasureText(line, fontSize);
    const int padX = 12;
    const int padY = 6;
    const int cw = tw + padX * 2;
    const int ch = fontSize + padY * 2;
    // Centered above control-hint line / hotbar mode hint — clears vitals + minimap.
    const int cx = (sw - cw) / 2;
    const int cy = sh - 175;

    const Rectangle r{static_cast<float>(cx), static_cast<float>(cy),
                      static_cast<float>(cw), static_cast<float>(ch)};
    DrawRectangleRounded(r, 0.3f, 6, fill);
    DrawRectangleRoundedLinesEx(r, 0.3f, 6, 1.5f, edge);
    DrawText(line, cx + padX, cy + padY, fontSize, text);
}


// --- CONCEPT v0.7 inventory modal (Present-only, read snapshot) -----------------
constexpr int kInvCapacityMax = 64; // Present display stub until Architecture publishes a real cap.
constexpr int kInvGridN = 16;
constexpr int kInvMiningHeadSentinel = -100; // not an inventory id; Present toolTier_ stub

enum class InvTab : int { Equipment = 0, Materials = 1, Tools = 2 };

struct InvSlotView {
    int itemId{};       // 0 = empty cell; kInvMiningHeadSentinel = tool stub
    int count{};
    char abbrev[5]{"--"};
    const char* name{"EMPTY"};
    const char* rarity{"-"};
    const char* desc{""};
    float weightStub{};
};

void writeInvAbbrev(char out[5], int itemId) {
    const char* src = "???";
    char machineBuf[5]{};
    if (itemId == kInvMiningHeadSentinel) src = "HEAD";
    else if (itemId > 0 && itemId < kBlockTypeCount) {
        switch (static_cast<BlockType>(itemId)) {
            case BlockType::Dirt: src = "DRT"; break;
            case BlockType::Stone: src = "STN"; break;
            case BlockType::Grass: src = "GRS"; break;
            case BlockType::Regolith: src = "REG"; break;
            case BlockType::Basalt: src = "BAS"; break;
            case BlockType::CoalOre: src = "COL"; break;
            case BlockType::CopperOre: src = "CPR"; break;
            case BlockType::TinOre: src = "TIN"; break;
            case BlockType::IronOre: src = "IRN"; break;
            case BlockType::Planks: src = "PLK"; break;
            case BlockType::SteelPlate: src = "STL"; break;
            case BlockType::RegistryBeacon: src = "BCN"; break;
            case BlockType::Magma: src = "MAG"; break;
            case BlockType::DoorPanel: src = "DOR"; break;
            case BlockType::AirlockPanel: src = "ALK"; break;
            default: break;
        }
    } else if (itemId >= 1000 && itemId < 1100) {
        const char* n = machineName(static_cast<MachineType>(itemId - 1000));
        machineBuf[0] = n && n[0] ? static_cast<char>(n[0] >= 'a' && n[0] <= 'z' ? n[0] - 32 : n[0]) : '?';
        machineBuf[1] = n && n[1] ? static_cast<char>(n[1] >= 'a' && n[1] <= 'z' ? n[1] - 32 : n[1]) : '?';
        machineBuf[2] = n && n[2] ? static_cast<char>(n[2] >= 'a' && n[2] <= 'z' ? n[2] - 32 : n[2]) : '?';
        machineBuf[3] = '\0';
        src = machineBuf;
    } else if (itemId >= 2000 && itemId < 2100) {
        src = (itemId == surfacePortalItemId(SurfacePortalType::Door)) ? "DOR" : "ALK";
    } else {
        switch (static_cast<IndustryItemId>(itemId)) {
            case IndustryItemId::CopperIngot: src = "CuI"; break;
            case IndustryItemId::TinIngot: src = "SnI"; break;
            case IndustryItemId::IronIngot: src = "FeI"; break;
            case IndustryItemId::Carbon: src = "CBN"; break;
            case IndustryItemId::BronzeIngot: src = "Brz"; break;
            case IndustryItemId::SteelIngot: src = "StI"; break;
            case IndustryItemId::StoneAggregate: src = "AGG"; break;
            case IndustryItemId::CopperConcentrate: src = "CuC"; break;
            case IndustryItemId::TinConcentrate: src = "SnC"; break;
            case IndustryItemId::IronConcentrate: src = "FeC"; break;
            case IndustryItemId::Sealant: src = "SEA"; break;
            case IndustryItemId::CopperWire: src = "WIR"; break;
            case IndustryItemId::SteelFrame: src = "FRM"; break;
            case IndustryItemId::Actuator: src = "ACT"; break;
            case IndustryItemId::ControlCircuit: src = "CIR"; break;
            case IndustryItemId::SensorPackage: src = "SEN"; break;
            case IndustryItemId::TurretAmmo: src = "AMM"; break;
            case IndustryItemId::RepairKit: src = "REP"; break;
            case IndustryItemId::FilterCartridge: src = "FLT"; break;
            case IndustryItemId::MachineCasing: src = "CAS"; break;
            case IndustryItemId::CompositePanel: src = "CMP"; break;
            default: break;
        }
    }
    out[0] = src[0] ? src[0] : '?';
    out[1] = src[1] ? src[1] : '\0';
    out[2] = src[2] ? src[2] : '\0';
    out[3] = src[3] ? src[3] : '\0';
    out[4] = '\0';
}

const char* invNameForId(int itemId, int toolTier) {
    if (itemId == kInvMiningHeadSentinel) {
        // EarlyMetallurgy owns the improvised→stone→bronze→steel labels.
        if (toolTier >= EarlyMetallurgy::kSteel) return "STEEL MINING HEAD";
        if (toolTier >= EarlyMetallurgy::kBronze) return "BRONZE MINING HEAD";
        if (toolTier >= EarlyMetallurgy::kStone) return "STONE MINING HEAD";
        return "IMPROVISED TOOLS";
    }
    if (itemId > 0 && itemId < kBlockTypeCount)
        return blockProperties(static_cast<BlockType>(itemId)).name.data();
    if (itemId >= 1000 && itemId < 1100)
        return machineName(static_cast<MachineType>(itemId - 1000));
    if (itemId >= 2000 && itemId < 2100)
        return surfacePortalName(static_cast<SurfacePortalType>(itemId - 2000));
    if (validIndustryItemId(itemId))
        return industryItemName(itemId);
    return "UNKNOWN";
}

const char* invRarityForId(int itemId) {
    if (itemId == kInvMiningHeadSentinel) return "TOOL";
    if (itemId >= 1000 && itemId < 1100) return "EQUIPMENT";
    if (itemId >= 2000 && itemId < 2100) return "EQUIPMENT";
    if (itemId == static_cast<int>(BlockType::RegistryBeacon)) return "UNCOMMON";
    if (itemId >= 3000) return "REFINED";
    return "COMMON";
}

const char* invDescForId(int itemId) {
    if (itemId == kInvMiningHeadSentinel)
        return "Installed mining head. Tier gates harvest hardness.";
    if (itemId > 0 && itemId < kBlockTypeCount) {
        const auto& p = blockProperties(static_cast<BlockType>(itemId));
        if (p.ore) return "Raw ore. Feed industry or craft.";
        if (p.placeable) return "Basic building material. Strong and reliable.";
        return "Field material.";
    }
    if (itemId >= 1000 && itemId < 1100) return "Machine kit. Place with machine hotbar / mode.";
    if (itemId >= 2000 && itemId < 2100) return "Portal kit. Seal rooms and airlocks.";
    switch (static_cast<IndustryItemId>(itemId)) {
        case IndustryItemId::RepairKit: return "Ship / hull repair consumable.";
        case IndustryItemId::TurretAmmo: return "Defense ammunition stock.";
        case IndustryItemId::FilterCartridge: return "Atmosphere filter spare.";
        case IndustryItemId::Actuator: return "Motion component for kits.";
        case IndustryItemId::SensorPackage: return "Sensor suite component.";
        default: return "Processed industry component.";
    }
}

float invWeightStub(int itemId, int count) {
    if (count <= 0) return 0.0f;
    float unit = 0.2f;
    if (itemId == kInvMiningHeadSentinel) unit = 4.0f;
    else if (itemId >= 1000 && itemId < 2100) unit = 2.5f;
    else if (itemId >= 3000) unit = 0.5f;
    else if (itemId == static_cast<int>(BlockType::SteelPlate)) unit = 0.8f;
    return unit * static_cast<float>(count);
}

InvTab classifyInvItem(int itemId) {
    // Tools: tool-adjacent kits / consumables (mining heads are synthetic).
    if (itemId == kInvMiningHeadSentinel) return InvTab::Tools;
    if (itemId == static_cast<int>(IndustryItemId::RepairKit) ||
        itemId == static_cast<int>(IndustryItemId::TurretAmmo) ||
        itemId == static_cast<int>(IndustryItemId::FilterCartridge) ||
        itemId == static_cast<int>(IndustryItemId::Actuator) ||
        itemId == static_cast<int>(IndustryItemId::SensorPackage))
        return InvTab::Tools;
    if (itemId == machineItemId(MachineType::Turret) ||
        itemId == machineItemId(MachineType::Extractor) ||
        itemId == machineItemId(MachineType::SensorMast))
        return InvTab::Tools;
    // Equipment: machine kits, portals, beacon.
    if (itemId >= 1000 && itemId < 1100) return InvTab::Equipment;
    if (itemId >= 2000 && itemId < 2100) return InvTab::Equipment;
    if (itemId == static_cast<int>(BlockType::RegistryBeacon) ||
        itemId == static_cast<int>(BlockType::DoorPanel) ||
        itemId == static_cast<int>(BlockType::AirlockPanel))
        return InvTab::Equipment;
    // Default Materials: ores, blocks, plates, industry intermediates.
    return InvTab::Materials;
}

void fillKnownCatalog(std::vector<int>& out, InvTab tab) {
    out.clear();
    if (tab == InvTab::Materials) {
        const BlockType blocks[] = {
            BlockType::Dirt, BlockType::Stone, BlockType::Planks, BlockType::SteelPlate,
            BlockType::CoalOre, BlockType::CopperOre, BlockType::TinOre, BlockType::IronOre,
            BlockType::Grass, BlockType::Regolith, BlockType::Basalt
        };
        for (auto b : blocks) out.push_back(static_cast<int>(b));
        const IndustryItemId mats[] = {
            IndustryItemId::CopperIngot, IndustryItemId::TinIngot, IndustryItemId::IronIngot,
            IndustryItemId::Carbon, IndustryItemId::BronzeIngot, IndustryItemId::SteelIngot,
            IndustryItemId::StoneAggregate, IndustryItemId::CopperConcentrate,
            IndustryItemId::TinConcentrate, IndustryItemId::IronConcentrate,
            IndustryItemId::Sealant, IndustryItemId::CopperWire, IndustryItemId::SteelFrame,
            IndustryItemId::ControlCircuit, IndustryItemId::MachineCasing, IndustryItemId::CompositePanel
        };
        for (auto id : mats) out.push_back(static_cast<int>(id));
    } else if (tab == InvTab::Equipment) {
        out.push_back(static_cast<int>(BlockType::RegistryBeacon));
        for (int m = 0; m <= static_cast<int>(MachineType::Fabricator); ++m) {
            const auto mt = static_cast<MachineType>(m);
            if (mt == MachineType::Turret || mt == MachineType::Extractor || mt == MachineType::SensorMast)
                continue; // tool-adjacent
            out.push_back(machineItemId(mt));
        }
        out.push_back(surfacePortalItemId(SurfacePortalType::Door));
        out.push_back(surfacePortalItemId(SurfacePortalType::Airlock));
    } else {
        out.push_back(kInvMiningHeadSentinel);
        out.push_back(static_cast<int>(IndustryItemId::RepairKit));
        out.push_back(static_cast<int>(IndustryItemId::TurretAmmo));
        out.push_back(static_cast<int>(IndustryItemId::FilterCartridge));
        out.push_back(static_cast<int>(IndustryItemId::Actuator));
        out.push_back(static_cast<int>(IndustryItemId::SensorPackage));
        out.push_back(machineItemId(MachineType::Turret));
        out.push_back(machineItemId(MachineType::Extractor));
        out.push_back(machineItemId(MachineType::SensorMast));
    }
}

void buildInvTabSlots(InvSlotView out[kInvGridN], InvTab tab, const EcsWorld& ecs, int toolTier,
                      const std::vector<std::pair<int,int>>* invSnap) {
    for (int i = 0; i < kInvGridN; ++i) out[i] = {};

    // Prefer occupied stacks from read snapshot partitioned by tab, then pad from catalog.
    std::vector<int> ordered;
    ordered.reserve(32);
    if (invSnap) {
        for (const auto& [id, count] : *invSnap) {
            if (count <= 0) continue;
            if (classifyInvItem(id) != tab) continue;
            ordered.push_back(id);
        }
    }
    std::vector<int> catalog;
    fillKnownCatalog(catalog, tab);
    for (int id : catalog) {
        if (std::find(ordered.begin(), ordered.end(), id) == ordered.end())
            ordered.push_back(id);
    }

    for (int i = 0; i < kInvGridN && i < static_cast<int>(ordered.size()); ++i) {
        const int id = ordered[static_cast<std::size_t>(i)];
        int count = 0;
        if (id == kInvMiningHeadSentinel) {
            count = 1; // installed tool stub always visible
        } else {
            count = ecs.inventoryCount(id);
        }
        // Hide empty catalog pads after occupied ones when we already filled from snap —
        // still show catalog entries with 0 so tabs feel populated; CONCEPT allows empty cells.
        out[i].itemId = id;
        out[i].count = count;
        writeInvAbbrev(out[i].abbrev, id);
        out[i].name = invNameForId(id, toolTier);
        out[i].rarity = invRarityForId(id);
        out[i].desc = invDescForId(id);
        out[i].weightStub = invWeightStub(id, std::max(count, id == kInvMiningHeadSentinel ? 1 : 0));
    }
}

int inventoryOccupiedSlots(const std::vector<std::pair<int,int>>& inv) {
    int n = 0;
    for (const auto& [id, count] : inv) {
        (void)id;
        if (count > 0) ++n;
    }
    return n;
}

struct InvLayout {
    int panelX{}, panelY{}, panelW{}, panelH{};
    int tabY{}, tabH{}, tabW{};
    int gridX{}, gridY{}, cell{}, gap{};
    int detailX{}, detailY{}, detailW{}, detailH{};
    int closeX{}, closeY{}, closeS{};
    int capX{}, capY{};
    int voxelX{}, voxelY{}, toggleW{}, toggleH{};
};

InvLayout makeInvLayout(int sw, int sh) {
    InvLayout L;
    L.panelW = 560;
    L.panelH = 520;
    L.panelX = 24;
    L.panelY = std::max(88, sh / 2 - L.panelH / 2 - 20);
    L.tabY = L.panelY + 48;
    L.tabH = 32;
    L.tabW = 150;
    L.closeS = 28;
    L.closeX = L.panelX + L.panelW - L.closeS - 14;
    L.closeY = L.panelY + 12;
    L.cell = 56;
    L.gap = 6;
    L.gridX = L.panelX + 18;
    L.gridY = L.tabY + L.tabH + 14;
    const int gridW = 4 * L.cell + 3 * L.gap;
    L.detailX = L.gridX + gridW + 16;
    L.detailY = L.gridY;
    L.detailW = L.panelX + L.panelW - L.detailX - 18;
    L.detailH = 4 * L.cell + 3 * L.gap;
    L.capX = L.panelX + 18;
    L.capY = L.panelY + L.panelH - 44;
    L.toggleW = 110;
    L.toggleH = 30;
    L.voxelX = L.panelX + L.panelW / 2 - L.toggleW;
    L.voxelY = L.capY - 2;
    (void)sw;
    return L;
}

void drawInventoryModalChrome(const InvLayout& L, int activeTab, int selected, bool sculptMode,
                              const InvSlotView slots[kInvGridN], int capacityN, int capacityMax) {
    drawGlassPanel(L.panelX, L.panelY, L.panelW, L.panelH);
    DrawText("INVENTORY", L.panelX + 18, L.panelY + 14, 22, RAYWHITE);

    // Close affordance
    const Rectangle closeR{static_cast<float>(L.closeX), static_cast<float>(L.closeY),
                           static_cast<float>(L.closeS), static_cast<float>(L.closeS)};
    DrawRectangleRounded(closeR, 0.3f, 4, Color{22, 32, 44, 180});
    DrawRectangleRoundedLines(closeR, 0.3f, 4, kGlassEdge);
    DrawText("X", L.closeX + 8, L.closeY + 5, 18, RAYWHITE);

    const char* tabNames[3] = {"Equipment", "Materials", "Tools"};
    for (int t = 0; t < 3; ++t) {
        const int tx = L.panelX + 18 + t * (L.tabW + 8);
        const Rectangle tr{static_cast<float>(tx), static_cast<float>(L.tabY),
                           static_cast<float>(L.tabW), static_cast<float>(L.tabH)};
        const bool on = (t == activeTab);
        DrawRectangleRounded(tr, 0.35f, 6, on ? kCyanAccent : Color{22, 32, 44, 160});
        DrawRectangleRoundedLines(tr, 0.35f, 6, on ? kCyanAccent : kGlassEdge);
        const int tw = MeasureText(tabNames[t], 16);
        DrawText(tabNames[t], tx + (L.tabW - tw) / 2, L.tabY + 7, 16, on ? Color{10, 18, 24, 255} : RAYWHITE);
    }

    for (int i = 0; i < kInvGridN; ++i) {
        const int col = i % 4;
        const int row = i / 4;
        const int sx = L.gridX + col * (L.cell + L.gap);
        const int sy = L.gridY + row * (L.cell + L.gap);
        const Rectangle sr{static_cast<float>(sx), static_cast<float>(sy),
                           static_cast<float>(L.cell), static_cast<float>(L.cell)};
        DrawRectangleRounded(sr, 0.22f, 6, Color{22, 32, 44, 160});
        DrawRectangleRoundedLines(sr, 0.22f, 6, Color{140, 170, 190, 90});
        if (i == selected) {
            DrawRectangleRoundedLinesEx(sr, 0.22f, 6, 3.0f, kCyanAccent);
        }
        if (slots[i].itemId != 0) {
            const int aw = MeasureText(slots[i].abbrev, 14);
            DrawText(slots[i].abbrev, sx + (L.cell - aw) / 2, sy + L.cell / 2 - 10, 14, RAYWHITE);
            if (slots[i].count > 0) {
                const char* cq = TextFormat("%d", slots[i].count);
                const int cw = MeasureText(cq, 12);
                DrawText(cq, sx + L.cell - cw - 4, sy + L.cell - 16, 12, RAYWHITE);
            }
        }
    }

    // Detail pane
    const Rectangle dr{static_cast<float>(L.detailX), static_cast<float>(L.detailY),
                       static_cast<float>(L.detailW), static_cast<float>(L.detailH)};
    DrawRectangleRounded(dr, 0.12f, 6, Color{22, 32, 44, 150});
    DrawRectangleRoundedLines(dr, 0.12f, 6, kGlassEdge);
    const InvSlotView& sel = slots[std::clamp(selected, 0, kInvGridN - 1)];
    if (sel.itemId != 0) {
        const int preview = 72;
        const Rectangle pr{static_cast<float>(L.detailX + (L.detailW - preview) / 2),
                           static_cast<float>(L.detailY + 12),
                           static_cast<float>(preview), static_cast<float>(preview)};
        DrawRectangleRounded(pr, 0.2f, 6, Color{30, 44, 58, 200});
        DrawRectangleRoundedLines(pr, 0.2f, 6, kCyanSoft);
        const int aw = MeasureText(sel.abbrev, 20);
        DrawText(sel.abbrev, L.detailX + (L.detailW - aw) / 2, L.detailY + 36, 20, kCyanAccent);

        DrawText(sel.name, L.detailX + 12, L.detailY + 96, 18, RAYWHITE);
        DrawText(sel.rarity, L.detailX + 12, L.detailY + 118, 13, kCyanSoft);

        // Wrap-ish short description (single line clipped OK for stub)
        DrawText(sel.desc, L.detailX + 12, L.detailY + 142, 13, Color{200, 210, 220, 220});

        DrawText(TextFormat("%d", sel.count), L.detailX + 18, L.detailY + L.detailH - 56, 20, RAYWHITE);
        DrawText("STACK COUNT", L.detailX + 18, L.detailY + L.detailH - 32, 11, Color{170, 190, 200, 180});
        DrawText(TextFormat("%.1f", sel.weightStub), L.detailX + L.detailW / 2, L.detailY + L.detailH - 56, 20, RAYWHITE);
        DrawText("WEIGHT", L.detailX + L.detailW / 2, L.detailY + L.detailH - 32, 11, Color{170, 190, 200, 180});
    } else {
        DrawText("EMPTY SLOT", L.detailX + 16, L.detailY + 24, 16, Color{180, 190, 200, 200});
        DrawText("Select an item in the grid.", L.detailX + 16, L.detailY + 50, 13, Color{160, 170, 180, 180});
    }

    // Capacity
    DrawText(TextFormat("#  %d / %d", capacityN, capacityMax), L.capX, L.capY + 6, 16, RAYWHITE);

    // VOXEL / SUBVOXEL — maps to sculptMode_ (false=macro VOXEL, true=16^3 SUBVOXEL)
    const Rectangle voxelR{static_cast<float>(L.voxelX), static_cast<float>(L.voxelY),
                           static_cast<float>(L.toggleW), static_cast<float>(L.toggleH)};
    const Rectangle subR{static_cast<float>(L.voxelX + L.toggleW + 6), static_cast<float>(L.voxelY),
                         static_cast<float>(L.toggleW), static_cast<float>(L.toggleH)};
    const bool voxelOn = !sculptMode;
    DrawRectangleRounded(voxelR, 0.45f, 6, voxelOn ? kCyanAccent : Color{22, 32, 44, 160});
    DrawRectangleRoundedLines(voxelR, 0.45f, 6, voxelOn ? kCyanAccent : kGlassEdge);
    DrawRectangleRounded(subR, 0.45f, 6, !voxelOn ? kCyanAccent : Color{22, 32, 44, 160});
    DrawRectangleRoundedLines(subR, 0.45f, 6, !voxelOn ? kCyanAccent : kGlassEdge);
    const int vw = MeasureText("VOXEL", 14);
    const int swt = MeasureText("SUBVOXEL", 14);
    DrawText("VOXEL", L.voxelX + (L.toggleW - vw) / 2, L.voxelY + 7, 14,
             voxelOn ? Color{10, 18, 24, 255} : RAYWHITE);
    DrawText("SUBVOXEL", L.voxelX + L.toggleW + 6 + (L.toggleW - swt) / 2, L.voxelY + 7, 14,
             !voxelOn ? Color{10, 18, 24, 255} : RAYWHITE);
}

const char* planetName(PlanetClass p) {
    switch (p) {
        case PlanetClass::Temperate: return "TEMPERATE // FRONTIER PLAINS";
        case PlanetClass::Barren: return "BARREN // REGOLITH PLAIN";
        case PlanetClass::Scorched: return "SCORCHED // BASALT PLAIN";
        case PlanetClass::Frozen: return "FROZEN // ICE SHEET";
        case PlanetClass::Toxic: return "TOXIC // SPORE DEEPS";
        case PlanetClass::Irradiated: return "IRRADIATED // GLASS SEA";
        case PlanetClass::Oceanic: return "OCEANIC // KELP SHELF";
        case PlanetClass::Anomalous: return "ANOMALOUS // FOLDED MESA";
        case PlanetClass::Count: break;
    }
    return "UNKNOWN";
}

EnemyArchetype enemyArchetypeFor(ImperialUnitRole role) {
    switch(role) {
        case ImperialUnitRole::Drone: return EnemyArchetype::Drone;
        case ImperialUnitRole::Lictor: return EnemyArchetype::Lictor;
        case ImperialUnitRole::Adept: return EnemyArchetype::Adept;
        case ImperialUnitRole::Praetor: return EnemyArchetype::Praetor;
    }
    return EnemyArchetype::Drone;
}

float enemyRenderRadius(EnemyArchetype archetype) {
    switch(archetype) {
        case EnemyArchetype::Drone: return 0.42f;
        case EnemyArchetype::Lictor: return 0.58f;
        case EnemyArchetype::Adept: return 0.46f;
        case EnemyArchetype::Praetor: return 0.82f;
    }
    return 0.42f;
}

Color enemyRenderColor(EnemyArchetype archetype) {
    switch(archetype) {
        case EnemyArchetype::Drone: return Color{28,34,31,255};
        case EnemyArchetype::Lictor: return Color{48,55,51,255};
        case EnemyArchetype::Adept: return Color{28,42,46,255};
        case EnemyArchetype::Praetor: return Color{20,24,23,255};
    }
    return Color{28,34,31,255};
}

Color enemyAccentColor(EnemyArchetype archetype) {
    switch(archetype) {
        case EnemyArchetype::Drone: return Color{49,215,142,255};
        case EnemyArchetype::Lictor: return Color{129,224,172,255};
        case EnemyArchetype::Adept: return Color{90,189,238,255};
        case EnemyArchetype::Praetor: return Color{74,255,166,255};
    }
    return Color{49,215,142,255};
}

void drawSurfaceCellWires(const PlanetSurface& planet, SurfaceCellAddress a, Color color) {
    a=planet.normalize(a);
    const Vec3 a000=planet.boundaryPosition(a.face,a.u,a.v,a.radial);
    const Vec3 a100=planet.boundaryPosition(a.face,a.u+1,a.v,a.radial);
    const Vec3 a110=planet.boundaryPosition(a.face,a.u+1,a.v+1,a.radial);
    const Vec3 a010=planet.boundaryPosition(a.face,a.u,a.v+1,a.radial);
    const Vec3 a001=planet.boundaryPosition(a.face,a.u,a.v,a.radial+1);
    const Vec3 a101=planet.boundaryPosition(a.face,a.u+1,a.v,a.radial+1);
    const Vec3 a111=planet.boundaryPosition(a.face,a.u+1,a.v+1,a.radial+1);
    const Vec3 a011=planet.boundaryPosition(a.face,a.u,a.v+1,a.radial+1);
    DrawLine3D(rv(a000),rv(a100),color); DrawLine3D(rv(a100),rv(a110),color);
    DrawLine3D(rv(a110),rv(a010),color); DrawLine3D(rv(a010),rv(a000),color);
    DrawLine3D(rv(a001),rv(a101),color); DrawLine3D(rv(a101),rv(a111),color);
    DrawLine3D(rv(a111),rv(a011),color); DrawLine3D(rv(a011),rv(a001),color);
    DrawLine3D(rv(a000),rv(a001),color); DrawLine3D(rv(a100),rv(a101),color);
    DrawLine3D(rv(a110),rv(a111),color); DrawLine3D(rv(a010),rv(a011),color);
}


// --- CONCEPT v0.7 bottom-right utility cluster (pass 7) ---
constexpr int kUtilBtnCount = 5;
constexpr int kUtilBtnSize = 44;
constexpr int kUtilGap = 6;
constexpr int kUtilPad = 8;

struct UtilityClusterLayout {
    int panelX{}, panelY{}, panelW{}, panelH{};
    int btn{kUtilBtnSize};
    int gap{kUtilGap};
    int pad{kUtilPad};
    int count{kUtilBtnCount};

    Rectangle buttonRect(int i) const {
        const int bx = panelX + pad + i * (btn + gap);
        const int by = panelY + pad;
        return Rectangle{static_cast<float>(bx), static_cast<float>(by),
                         static_cast<float>(btn), static_cast<float>(btn)};
    }
};

UtilityClusterLayout makeUtilityClusterLayout(int sw, int sh) {
    UtilityClusterLayout L;
    L.panelW = L.pad * 2 + L.count * L.btn + (L.count - 1) * L.gap;
    L.panelH = L.pad * 2 + L.btn + 14; // room for key hint under icon
    L.panelX = sw - L.panelW - 16;
    L.panelY = sh - L.panelH - 14;
    return L;
}

void drawUtilIconMenu(int cx, int cy, Color col) {
    // 3x3 grid
    for (int gy = -1; gy <= 1; ++gy) {
        for (int gx = -1; gx <= 1; ++gx) {
            DrawRectangle(cx + gx * 7 - 2, cy + gy * 7 - 2, 4, 4, col);
        }
    }
}

void drawUtilIconCraft(int cx, int cy, Color col) {
    // Simple hammer / wrench glyph
    DrawRectangle(cx - 8, cy - 2, 16, 4, col);
    DrawRectangle(cx - 2, cy - 9, 4, 18, col);
    DrawRectangle(cx + 4, cy - 8, 6, 3, col);
    DrawRectangle(cx - 10, cy + 5, 6, 3, col);
}

void drawUtilIconChart(int cx, int cy, Color col) {
    // Three vertical bars (stats / chart)
    DrawRectangle(cx - 10, cy + 2, 5, 10, col);
    DrawRectangle(cx - 2, cy - 6, 5, 18, col);
    DrawRectangle(cx + 6, cy - 2, 5, 14, col);
}

void drawUtilIconTravel(int cx, int cy, Color col) {
    // Up-chevron / ship nose
    DrawTriangle(Vector2{static_cast<float>(cx), static_cast<float>(cy - 10)},
                 Vector2{static_cast<float>(cx - 9), static_cast<float>(cy + 6)},
                 Vector2{static_cast<float>(cx + 9), static_cast<float>(cy + 6)},
                 col);
    DrawRectangle(cx - 2, cy + 4, 4, 8, col);
}

void drawUtilIconMore(int cx, int cy, Color col) {
    DrawCircle(cx - 9, cy, 3, col);
    DrawCircle(cx, cy, 3, col);
    DrawCircle(cx + 9, cy, 3, col);
}

void drawConceptUtilityCluster(int sw, int sh,
                               bool inventoryOpen, bool craftOpen, bool chartOpen, bool travelOpen,
                               bool nearShip) {
    const UtilityClusterLayout L = makeUtilityClusterLayout(sw, sh);
    drawGlassPanel(L.panelX, L.panelY, L.panelW, L.panelH);

    const bool active[kUtilBtnCount] = {inventoryOpen, craftOpen, chartOpen, travelOpen, false};
    const bool enabled[kUtilBtnCount] = {true, true, true, nearShip || travelOpen, true};
    const char* hints[kUtilBtnCount] = {"TAB", "C", "M", "T", "…"};
    using IconFn = void (*)(int, int, Color);
    IconFn icons[kUtilBtnCount] = {
        drawUtilIconMenu, drawUtilIconCraft, drawUtilIconChart, drawUtilIconTravel, drawUtilIconMore};

    for (int i = 0; i < L.count; ++i) {
        const Rectangle br = L.buttonRect(i);
        const int bx = static_cast<int>(br.x);
        const int by = static_cast<int>(br.y);
        const Color slotFill = enabled[i] ? Color{22, 32, 44, 160} : Color{18, 22, 28, 120};
        DrawRectangleRounded(br, 0.28f, 6, slotFill);
        DrawRectangleRoundedLines(br, 0.28f, 6, enabled[i] ? Color{140, 170, 190, 90} : Color{80, 90, 100, 70});
        if (active[i]) {
            DrawRectangleRoundedLinesEx(br, 0.28f, 6, 2.5f, kCyanAccent);
        }
        const Color iconCol = !enabled[i] ? Color{120, 130, 140, 140}
                             : active[i] ? kCyanAccent
                                         : RAYWHITE;
        icons[i](bx + L.btn / 2, by + L.btn / 2 - 2, iconCol);
        const int hw = MeasureText(hints[i], 10);
        DrawText(hints[i], bx + (L.btn - hw) / 2, by + L.btn - 1, 10,
                 enabled[i] ? Color{180, 200, 210, 200} : Color{100, 110, 120, 140});
    }
}

// Glass modal chrome shared by craft / chart / travel (pass 7).
void drawGlassModalFrame(int x, int y, int w, int h, const char* title) {
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{5, 8, 12, 140});
    drawGlassPanel(x, y, w, h);
    // Soft cyan top accent line
    DrawRectangle(x + 12, y + 8, w - 24, 2, Color{0, 245, 255, 160});
    DrawText(title, x + 22, y + 18, 22, RAYWHITE);
    const Rectangle closeR{static_cast<float>(x + w - 40), static_cast<float>(y + 14), 26.0f, 26.0f};
    DrawRectangleRounded(closeR, 0.3f, 4, Color{22, 32, 44, 180});
    DrawRectangleRoundedLines(closeR, 0.3f, 4, kGlassEdge);
    DrawText("X", x + w - 32, y + 18, 18, RAYWHITE);
}

bool glassModalCloseHit(int x, int y, int w, Vector2 mouse) {
    const Rectangle closeR{static_cast<float>(x + w - 40), static_cast<float>(y + 14), 26.0f, 26.0f};
    return CheckCollisionPointRec(mouse, closeR);
}

} // namespace

Game::Game() : renderer_(jobs_, graphics_), planetRenderer_(jobs_, graphics_) {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(1440, 900, "ELYSIUM // C++ EnTT Vertical Slice v0.20 Part Two");
    SetTargetFPS(60);
    DisableCursor();

    camera_.position = {0,0,0};
    camera_.target = {0,0,1};
    camera_.up = {0,1,0};
    camera_.fovy = 75.0f;
    camera_.projection = CAMERA_PERSPECTIVE;

    initWorlds();
    ecs_.createPlayer(spawnPoint(world()));

    // Survival starter kit. Resources remain scarce enough to make mining useful.
    ecs_.addItem(static_cast<int>(BlockType::Dirt), 20);
    ecs_.addItem(static_cast<int>(BlockType::Planks), 24);
    ecs_.addItem(static_cast<int>(BlockType::SteelPlate), 4);
    ecs_.addItem(static_cast<int>(BlockType::RegistryBeacon), 1);
    // Vertical-slice infrastructure test kit. Production progression will move
    // these behind the intended machine recipes/research gates.
    ecs_.addItem(machineItemId(MachineType::BurnerGenerator), 1);
    ecs_.addItem(machineItemId(MachineType::BatteryBank), 1);
    ecs_.addItem(machineItemId(MachineType::AtmosphereUnit), 1);
    ecs_.addItem(machineItemId(MachineType::StorageCrate), 1);
    ecs_.addItem(machineItemId(MachineType::AirlockController), 1);
    ecs_.addItem(machineItemId(MachineType::SensorMast), 1);
    ecs_.addItem(machineItemId(MachineType::Turret), 1);
    ecs_.addItem(machineItemId(MachineType::ShieldPylon), 1);
    ecs_.addItem(machineItemId(MachineType::LogicController), 1);
    ecs_.addItem(machineItemId(MachineType::Furnace), 1);
    ecs_.addItem(machineItemId(MachineType::AlloyCrucible), 1);
    ecs_.addItem(machineItemId(MachineType::Refinery), 1);
    ecs_.addItem(machineItemId(MachineType::NetworkStorage), 1);
    ecs_.addItem(machineItemId(MachineType::Conveyor), 2);
    ecs_.addItem(machineItemId(MachineType::Sorter), 1);
    ecs_.addItem(machineItemId(MachineType::CargoLoader), 1);
    ecs_.addItem(machineItemId(MachineType::Crusher), 1);
    ecs_.addItem(machineItemId(MachineType::ChemicalVat), 1);
    ecs_.addItem(machineItemId(MachineType::Fabricator), 1);
    ecs_.addItem(machineItemId(MachineType::Extractor), 1);
    ecs_.addItem(surfacePortalItemId(SurfacePortalType::Door), 2);
    ecs_.addItem(surfacePortalItemId(SurfacePortalType::Airlock), 2);

    if (!loadGame()) setMessage("UNFILED. Spherical surface online. Mine, build, survive.", 5.0f);
}

Game::~Game() {
    saveGame();
    renderer_.invalidate();
    planetRenderer_.invalidate();
    CloseWindow();
}

void Game::initWorlds() {
    const std::array<PlanetClass,PlanetCount> classes{PlanetClass::Temperate,PlanetClass::Barren,PlanetClass::Scorched};
    const std::array<std::uint64_t,PlanetCount> labels{0x54454D50ULL,0x42415252ULL,0x53434F52ULL};
    for (int i=0;i<PlanetCount;++i) {
        const std::uint64_t seed=mix64(galaxySeed_^labels[static_cast<std::size_t>(i)]);
        worlds_[static_cast<std::size_t>(i)] = std::make_unique<World>(seed,classes[static_cast<std::size_t>(i)]);
        planetSurfaces_[static_cast<std::size_t>(i)] = std::make_unique<PlanetSurface>(seed,classes[static_cast<std::size_t>(i)]);
        bases_[static_cast<std::size_t>(i)] = std::make_unique<BaseInfrastructure>(seed);
        surfaceBases_[static_cast<std::size_t>(i)] = std::make_unique<SurfaceInfrastructure>(seed);
        surfaceNavigation_[static_cast<std::size_t>(i)] = std::make_unique<SurfaceNavigationService>(*planetSurfaces_[static_cast<std::size_t>(i)],64,768);
        surfaceSieges_[static_cast<std::size_t>(i)] = std::make_unique<SurfaceSiegeDirector>(seed);

        // Cube-sphere player position is stable per planet. v0.6 also migrates
        // MicroBricks and base-machine anchors into this same address domain;
        // the legacy planar patch remains only as a compatibility sandbox.
        const int u=PlanetSurface::FaceResolution/2;
        const int v=PlanetSurface::FaceResolution/2;
        const Vec3 dir=faceGridCellDirection(CubeFace::PositiveZ,u,v,PlanetSurface::FaceResolution);
        const float ground=planetSurfaces_[static_cast<std::size_t>(i)]->surfaceBoundaryRadius(dir);
        sphericalPlayerPositions_[static_cast<std::size_t>(i)] = dir*(ground+0.05f);
    }
}

Vec3 Game::spawnPoint(const World& w) const {
    constexpr int x = World::Width / 2;
    constexpr int z = World::Depth / 2;
    return {x + 0.5f, static_cast<float>(w.surfaceY(x,z)) + 1.01f, z + 0.5f};
}

Vec3 Game::shipTerminalPosition(const World& w) const {
    constexpr int x = World::Width / 2 + 5;
    constexpr int z = World::Depth / 2;
    return {x + 0.5f, static_cast<float>(w.surfaceY(x,z)) + 1.0f, z + 0.5f};
}

Vec3 Game::sphericalShipTerminalPosition() const {
    const int u=PlanetSurface::FaceResolution/2 + 5;
    const int v=PlanetSurface::FaceResolution/2;
    const Vec3 dir=faceGridCellDirection(CubeFace::PositiveZ,u,v,PlanetSurface::FaceResolution);
    return dir*(surfaceRead().surfaceBoundaryRadius(dir)+0.80f);
}

Vec3 Game::sphericalArrivalPosition() const {
    const int u=PlanetSurface::FaceResolution/2 + 2;
    const int v=PlanetSurface::FaceResolution/2;
    const Vec3 dir=faceGridCellDirection(CubeFace::PositiveZ,u,v,PlanetSurface::FaceResolution);
    return dir*(surfaceRead().surfaceBoundaryRadius(dir)+0.05f);
}

Vec3 Game::cameraPosition() const {
    return ecs_.player().position + Vec3{0.0f, 1.62f, 0.0f};
}

Vec3 Game::cameraForward() const {
    const float cp = std::cos(pitch_);
    return normalize(Vec3{std::sin(yaw_) * cp, std::sin(pitch_), std::cos(yaw_) * cp});
}

Vec3 Game::sphericalCameraPosition() const {
    const Vec3 feet=sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)];
    const Vec3 up=normalize(feet);
    return feet + up*1.62f;
}

Vec3 Game::sphericalCameraForward() const {
    const Vec3 feet=sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)];
    const auto frame=planetSurface().surfaceFrame(feet);
    const Vec3 horizontal=normalize(frame.forward*std::cos(yaw_)+frame.right*std::sin(yaw_));
    return normalize(horizontal*std::cos(pitch_)+frame.up*std::sin(pitch_));
}

void Game::run() {
    while (!WindowShouldClose()) {
        const float dt = std::min(GetFrameTime(), 0.05f);
        update(dt);
        draw();
    }
}

void Game::update(float dt) {
    if (messageTimer_ > 0.0f) messageTimer_ -= dt;
    surfaceAiTelemetry_={};
    surfaceDefenseTelemetry_={};
    surfaceIndustryTelemetry_={};

    if (IsKeyPressed(KEY_F5)) saveGame();
    if (IsKeyPressed(KEY_F9)) {
        if (loadGame()) setMessage("Save reloaded.");
        else setMessage("No valid save found.");
    }

    if (IsKeyPressed(KEY_P)) {
        sphericalSurfaceMode_ = !sphericalSurfaceMode_;
        orbitalPreview_ = false;
        travelMenu_ = craftingMenu_ = mapMenu_ = inventoryMenu_ = false;
        sphericalVerticalVelocity_=0.0f;
        sphericalGrounded_=true;
        miningProgress_=0.0f;
        sphericalMiningValid_=false;
        ecs_.clearEnemies();
        setMessage(sphericalSurfaceMode_ ? "PLANET SURFACE // streamed cube-sphere traversal + combat online" : "LEGACY PATCH // local systems view",2.8f);
    }
    if (IsKeyPressed(KEY_O)) {
        orbitalPreview_ = !orbitalPreview_;
        sphericalSurfaceMode_ = false;
        travelMenu_ = craftingMenu_ = mapMenu_ = inventoryMenu_ = false;
        if (orbitalPreview_ && lengthSq(orbitWalkerPosition_)<1.0f)
            orbitWalkerPosition_=sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)]+normalize(sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)])*1.2f;
        ecs_.clearEnemies();
        setMessage(orbitalPreview_ ? "ORBITAL FIELD LOD // topology/debug view" : "LEGACY PATCH // local systems view",2.5f);
    }

    if (orbitalPreview_) {
        updateOrbitalPreview(dt);
    } else if (sphericalSurfaceMode_) {
        updateMenus();
        const Vec3 streamFocus=sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)];
        planetRenderer_.setStreamingFocus(streamFocus,7,9);
        planetRenderer_.sync(planetSurface());
        surfaceBase().update(planetSurface(),dt);
        surfaceIndustry_.update(surfaceBase(),dt);
        surfaceIndustry_.updateExtraction(planetSurface(),surfaceBase(),dt);
        surfaceIndustryTelemetry_=surfaceIndustry_.telemetry();
        if(surfaceIndustryTelemetry_.extractorSuspicionGenerated>0.0f)
            raiseSuspicion(surfaceIndustryTelemetry_.extractorSuspicionGenerated);
        if (!travelMenu_ && !craftingMenu_ && !mapMenu_ && !inventoryMenu_) {
            selectHotbar();
            updateSphericalPlayer(dt);
            updateSphericalInteraction(dt);
            const Vec3 feet=sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)];
            const Vec3 playerCenter=feet+normalize(feet)*0.9f;
            SurfaceWorldReadService read(planetSurface(),planetRenderer_.cpuChunkCache());
            auto hover=[this,&read](Vec3 candidate){
                Vec3 dir=lengthSq(candidate)>0.001f?normalize(candidate):Vec3{0,0,1};
                return dir*(read.surfaceBoundaryRadius(dir)+1.8f);
            };
            auto& navigation=surfaceNavigation();
            auto path=[&read,&navigation,playerCenter](Vec3 position,Vec3 velocity,float pathDt) {
                return navigation.advance(read,position,playerCenter,length(velocity),pathDt,1.8f,0.38f,1.6f);
            };
            auto mitigate=[this,playerCenter](Vec3 /*attacker*/,float damage) {
                return surfaceBase().absorbShieldDamage(planetSurface(),playerCenter,damage);
            };
            surfaceAiTelemetry_=ecs_.updateSurfaceEnemiesPhased(jobs_,dt,playerCenter,hover,path,mitigate);

            std::vector<SurfaceHostileContact> contacts;
            for(const auto& enemy:ecs_.enemies(ActorDomain::PlanetSurface))
                contacts.push_back({enemy.stableId,enemy.position,enemy.health});
            surfaceDefenseTelemetry_=surfaceBase().updateDefense(planetSurface(),dt,contacts);
            for(const auto& fire:surfaceBase().consumeTurretFireRequests())
                ecs_.queueDamageEnemy(fire.targetStableId,fire.damage);
            ecs_.flushCommands();
            updateEmpire(dt);
        }
    } else {
        updateMenus();
        if (!travelMenu_ && !craftingMenu_ && !mapMenu_ && !inventoryMenu_) {
            base().update(dt);
            selectHotbar();
            updatePlayer(dt);
            updateInteraction(dt);
            updateEmpire(dt);
        }
    }

    suspicion().setClaimFloor(claimFloor());
    suspicion().decay(dt);

    auto p = ecs_.player();
    const bool planarOutOfWorld=!sphericalSurfaceMode_ && !orbitalPreview_ && p.position.y < -10.0f;
    if (p.health <= 0.0f || planarOutOfWorld) {
        PlayerPersistentState state = ecs_.savePlayer();
        state.position = spawnPoint(world());
        state.health = 100.0f;
        state.oxygen = 100.0f;
        state.energy = 100.0f;
        state.hunger = 100.0f;
        ecs_.loadPlayer(state);
        const Vec3 dir=faceGridCellDirection(CubeFace::PositiveZ,PlanetSurface::FaceResolution/2,PlanetSurface::FaceResolution/2,PlanetSurface::FaceResolution);
        sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)]=dir*(surfaceRead().surfaceBoundaryRadius(dir)+0.05f);
        sphericalVerticalVelocity_=0.0f;
        sphericalGrounded_=true;
        sphericalSurfaceMode_=true;
        orbitalPreview_=false;
        sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)]=sphericalArrivalPosition();
        setMessage("FIELD FAILURE RECORDED // recovered at spherical ship", 4.0f);
    }
}

void Game::updateMenus() {
    auto openExclusive = [this](bool& target) {
        const bool next = !target;
        travelMenu_ = craftingMenu_ = mapMenu_ = inventoryMenu_ = false;
        target = next;
        if (&target == &inventoryMenu_ && inventoryMenu_) inventorySelected_ = 0;
    };

    if (IsKeyPressed(KEY_T) && nearShip()) openExclusive(travelMenu_);
    if (IsKeyPressed(KEY_C)) openExclusive(craftingMenu_);
    if (IsKeyPressed(KEY_M)) openExclusive(mapMenu_);
    // KEY_TAB opens CONCEPT inventory (KEY_I is Refinery craft; KEY_C is craft menu).
    if (IsKeyPressed(KEY_TAB)) openExclusive(inventoryMenu_);
    if (IsKeyPressed(KEY_ESCAPE)) {
        travelMenu_ = craftingMenu_ = mapMenu_ = inventoryMenu_ = false;
    }

    // Utility cluster clicks (pass 7). Cursor is freed while any modal is open;
    // when closed, FPS lock keeps GetMousePosition near center — key hints remain primary.
    {
        const int sw = GetScreenWidth();
        const int sh = GetScreenHeight();
        const UtilityClusterLayout util = makeUtilityClusterLayout(sw, sh);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            const Vector2 m = GetMousePosition();
            // Close affordances on glass mint menus
            if (craftingMenu_) {
                const int px = sw / 2 - 410, py = sh / 2 - 355, pw = 820;
                if (glassModalCloseHit(px, py, pw, m)) craftingMenu_ = false;
            }
            if (mapMenu_) {
                const int px = sw / 2 - 390, py = sh / 2 - 245, pw = 780;
                if (glassModalCloseHit(px, py, pw, m)) mapMenu_ = false;
            }
            if (travelMenu_) {
                const int px = sw / 2 - 330, py = sh / 2 - 205, pw = 660;
                if (glassModalCloseHit(px, py, pw, m)) travelMenu_ = false;
            }
            // Cluster buttons (work when cursor unlocked, e.g. modal already open, or freed)
            for (int i = 0; i < util.count; ++i) {
                if (!CheckCollisionPointRec(m, util.buttonRect(i))) continue;
                if (i == 0) openExclusive(inventoryMenu_);
                else if (i == 1) openExclusive(craftingMenu_);
                else if (i == 2) openExclusive(mapMenu_);
                else if (i == 3) {
                    if (nearShip()) openExclusive(travelMenu_);
                    else setMessage("TRAVEL // approach ship terminal [T]", 1.8f);
                } else if (i == 4) {
                    setMessage("JOURNAL // deferred Present surface", 2.0f);
                }
                break;
            }
        }
    }

    // Free cursor for modal UI clicks; restore FPS lock when chrome-only.
    if (travelMenu_ || craftingMenu_ || mapMenu_ || inventoryMenu_) EnableCursor();
    else DisableCursor();

    if (travelMenu_) {
        if (IsKeyPressed(KEY_ONE)) switchPlanet(0);
        if (IsKeyPressed(KEY_TWO)) switchPlanet(1);
        if (IsKeyPressed(KEY_THREE)) switchPlanet(2);
    }
    if (craftingMenu_) {
        if (IsKeyPressed(KEY_ONE)) tryCraft(1);
        if (IsKeyPressed(KEY_TWO)) tryCraft(2);
        if (IsKeyPressed(KEY_THREE)) tryCraft(3);
        if (IsKeyPressed(KEY_FOUR)) tryCraft(4);
        if (IsKeyPressed(KEY_FIVE)) tryCraft(5);
        if (IsKeyPressed(KEY_SIX)) tryCraft(6);
        if (IsKeyPressed(KEY_SEVEN)) tryCraft(7);
        if (IsKeyPressed(KEY_EIGHT)) tryCraft(8);
        if (IsKeyPressed(KEY_NINE)) tryCraft(9);
        if (IsKeyPressed(KEY_ZERO)) tryCraft(10);
        if (IsKeyPressed(KEY_K)) tryCraft(11);
        if (IsKeyPressed(KEY_J)) tryCraft(12);
        if (IsKeyPressed(KEY_U)) tryCraft(13);
        if (IsKeyPressed(KEY_Y)) tryCraft(14);
        if (IsKeyPressed(KEY_L)) tryCraft(15);
        if (IsKeyPressed(KEY_G)) tryCraft(16);
        if (IsKeyPressed(KEY_H)) tryCraft(17);
        if (IsKeyPressed(KEY_I)) tryCraft(18);
        if (IsKeyPressed(KEY_N)) tryCraft(19);
        if (IsKeyPressed(KEY_Q)) tryCraft(20);
        if (IsKeyPressed(KEY_R)) tryCraft(21);
        if (IsKeyPressed(KEY_X)) tryCraft(22);
        if (IsKeyPressed(KEY_Z)) tryCraft(23);
        if (IsKeyPressed(KEY_F1)) tryCraft(24);
        if (IsKeyPressed(KEY_F2)) tryCraft(25);
        if (IsKeyPressed(KEY_F3)) tryCraft(26);
    }
    if (inventoryMenu_) {
        // Tabs: 1-3 or click (handled below).
        if (IsKeyPressed(KEY_ONE)) { inventoryTab_ = 0; inventorySelected_ = 0; }
        if (IsKeyPressed(KEY_TWO)) { inventoryTab_ = 1; inventorySelected_ = 0; }
        if (IsKeyPressed(KEY_THREE)) { inventoryTab_ = 2; inventorySelected_ = 0; }

        // Arrow / WASD-style grid navigation (arrows only — WASD moves player when closed).
        if (IsKeyPressed(KEY_RIGHT)) inventorySelected_ = (inventorySelected_ % 4 == 3) ? inventorySelected_ : inventorySelected_ + 1;
        if (IsKeyPressed(KEY_LEFT)) inventorySelected_ = (inventorySelected_ % 4 == 0) ? inventorySelected_ : inventorySelected_ - 1;
        if (IsKeyPressed(KEY_DOWN)) inventorySelected_ = std::min(15, inventorySelected_ + 4);
        if (IsKeyPressed(KEY_UP)) inventorySelected_ = std::max(0, inventorySelected_ - 4);

        // [ / ] footer toggles — same semantic as key V (macro VOXEL vs 16^3 SUBVOXEL).
        if (IsKeyPressed(KEY_LEFT_BRACKET)) {
            sculptMode_ = false;
            setMessage("VOXEL // macro mining/building", 1.6f);
        }
        if (IsKeyPressed(KEY_RIGHT_BRACKET)) {
            sculptMode_ = true;
            setMessage("SUBVOXEL // 6.25 cm micro sculpt", 1.6f);
        }

        const int sw = GetScreenWidth();
        const int sh = GetScreenHeight();
        const InvLayout layout = makeInvLayout(sw, sh);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            const Vector2 m = GetMousePosition();
            const Rectangle closeR{static_cast<float>(layout.closeX), static_cast<float>(layout.closeY),
                                   static_cast<float>(layout.closeS), static_cast<float>(layout.closeS)};
            if (CheckCollisionPointRec(m, closeR)) {
                inventoryMenu_ = false;
            }
            for (int t = 0; t < 3; ++t) {
                const int tx = layout.panelX + 18 + t * (layout.tabW + 8);
                const Rectangle tr{static_cast<float>(tx), static_cast<float>(layout.tabY),
                                   static_cast<float>(layout.tabW), static_cast<float>(layout.tabH)};
                if (CheckCollisionPointRec(m, tr)) {
                    inventoryTab_ = t;
                    inventorySelected_ = 0;
                }
            }
            for (int i = 0; i < kInvGridN; ++i) {
                const int col = i % 4;
                const int row = i / 4;
                const int sx = layout.gridX + col * (layout.cell + layout.gap);
                const int sy = layout.gridY + row * (layout.cell + layout.gap);
                const Rectangle sr{static_cast<float>(sx), static_cast<float>(sy),
                                   static_cast<float>(layout.cell), static_cast<float>(layout.cell)};
                if (CheckCollisionPointRec(m, sr)) inventorySelected_ = i;
            }
            const Rectangle voxelR{static_cast<float>(layout.voxelX), static_cast<float>(layout.voxelY),
                                   static_cast<float>(layout.toggleW), static_cast<float>(layout.toggleH)};
            const Rectangle subR{static_cast<float>(layout.voxelX + layout.toggleW + 6), static_cast<float>(layout.voxelY),
                                 static_cast<float>(layout.toggleW), static_cast<float>(layout.toggleH)};
            if (CheckCollisionPointRec(m, voxelR)) {
                sculptMode_ = false;
                setMessage("VOXEL // macro mining/building", 1.6f);
            }
            if (CheckCollisionPointRec(m, subR)) {
                sculptMode_ = true;
                setMessage("SUBVOXEL // 6.25 cm micro sculpt", 1.6f);
            }
        }
    }
}

void Game::updateOrbitalPreview(float dt) {
    const Vector2 mouse=GetMouseDelta();
    orbitYaw_ += mouse.x*0.0030f;
    orbitPitch_ -= mouse.y*0.0030f;
    orbitPitch_=std::clamp(orbitPitch_,-1.35f,1.35f);
    orbitDistance_=std::clamp(orbitDistance_-GetMouseWheelMove()*4.0f,58.0f,180.0f);

    // A live topology probe walks the curved substrate using a local tangent
    // frame. Direction->face ownership is recomputed after every move, so this
    // visibly crosses cube seams without teleporting between coordinate charts.
    if (lengthSq(orbitWalkerPosition_)>1.0f) {
        const auto frame=planetSurface().surfaceFrame(orbitWalkerPosition_);
        float forward=(IsKeyDown(KEY_W)?1.0f:0.0f)-(IsKeyDown(KEY_S)?1.0f:0.0f);
        float right=(IsKeyDown(KEY_D)?1.0f:0.0f)-(IsKeyDown(KEY_A)?1.0f:0.0f);
        Vec3 wish=frame.forward*forward+frame.right*right;
        if (lengthSq(wish)>1.0f) wish=normalize(wish);
        if (lengthSq(wish)>0.001f) {
            const float radius=std::max(1.0f,length(orbitWalkerPosition_));
            const Vec3 dir=normalize(normalize(orbitWalkerPosition_)+wish*(8.0f*dt/radius));
            const auto cell=planetSurface().locate(dir*planetSurface().referenceRadius());
            const int surface=planetSurface().surfaceRadial(cell.face,cell.u,cell.v);
            const float targetRadius=planetSurface().referenceRadius()+static_cast<float>(surface+1-PlanetSurface::ReferenceRadial)+1.2f;
            orbitWalkerPosition_=dir*targetRadius;
        }
    }
    if (IsKeyPressed(KEY_ESCAPE)) orbitalPreview_=false;
}

void Game::updateSphericalPlayer(float dt) {
    const Vector2 mouse=GetMouseDelta();
    yaw_ += mouse.x*0.00225f;
    pitch_ -= mouse.y*0.00225f;
    pitch_=std::clamp(pitch_,-1.48f,1.48f);

    Vec3& feet=sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)];
    if (lengthSq(feet)<1.0f) {
        const Vec3 dir=faceGridCellDirection(CubeFace::PositiveZ,PlanetSurface::FaceResolution/2,PlanetSurface::FaceResolution/2,PlanetSurface::FaceResolution);
        feet=dir*(surfaceRead().surfaceBoundaryRadius(dir)+0.05f);
    }

    const auto initialFrame=planetSurface().surfaceFrame(feet);
    Vec3 forward=::elysium::normalize(initialFrame.forward*std::cos(yaw_)+initialFrame.right*std::sin(yaw_));
    Vec3 right=::elysium::normalize(cross(forward,initialFrame.up));
    const float moveForward=(IsKeyDown(KEY_W)?1.0f:0.0f)-(IsKeyDown(KEY_S)?1.0f:0.0f);
    const float moveRight=(IsKeyDown(KEY_D)?1.0f:0.0f)-(IsKeyDown(KEY_A)?1.0f:0.0f);
    Vec3 wish=forward*moveForward+right*moveRight;
    if (lengthSq(wish)>1.0f) wish=::elysium::normalize(wish);

    const bool moving=lengthSq(wish)>0.001f;
    const bool sprinting=IsKeyDown(KEY_LEFT_SHIFT) && ecs_.player().energy>0.5f;
    const float speed=sprinting?7.2f:4.6f;

    // Horizontal movement now preserves current radial depth instead of snapping
    // to the highest column. Collision samples the actual macro/micro volume, so
    // tunnels, overhangs and constructed interiors can be traversed.
    if (moving) {
        const float distance=speed*dt;
        const int steps=std::max(1,static_cast<int>(std::ceil(distance/0.20f)));
        const float stepDistance=distance/static_cast<float>(steps);
        for(int i=0;i<steps;++i) {
            const float radius=length(feet);
            const Vec3 candidateDir=::elysium::normalize(feet + wish*stepDistance);
            Vec3 candidate=candidateDir*radius;
            if (!surfaceRead().capsuleCollides(candidate)) {
                feet=candidate;
                continue;
            }
            // A bounded one-metre step-up keeps ordinary voxel terrain walkable
            // without reverting to the old height-field controller.
            if (sphericalGrounded_) {
                candidate=candidateDir*(radius+1.02f);
                if (!surfaceRead().capsuleCollides(candidate)) {
                    feet=candidate;
                    sphericalGrounded_=false;
                }
            }
        }
    }

    sphericalGrounded_=surfaceRead().groundedAt(feet,0.10f) && sphericalVerticalVelocity_<=0.0f;
    if (sphericalGrounded_ && IsKeyPressed(KEY_SPACE)) {
        sphericalGrounded_=false;
        sphericalVerticalVelocity_=6.0f;
    } else if (!sphericalGrounded_) {
        sphericalVerticalVelocity_-=14.0f*dt;
    } else {
        sphericalVerticalVelocity_=0.0f;
    }

    const float radialMove=sphericalVerticalVelocity_*dt;
    if (std::abs(radialMove)>0.00001f) {
        const int steps=std::max(1,static_cast<int>(std::ceil(std::abs(radialMove)/0.06f)));
        const float step=radialMove/static_cast<float>(steps);
        for(int i=0;i<steps;++i) {
            const Vec3 up=::elysium::normalize(feet);
            const Vec3 candidate=feet+up*step;
            if (surfaceRead().capsuleCollides(candidate)) {
                if (step<0.0f) sphericalGrounded_=true;
                sphericalVerticalVelocity_=0.0f;
                break;
            }
            feet=candidate;
            sphericalGrounded_=false;
        }
    }
    if (!sphericalGrounded_ && sphericalVerticalVelocity_<=0.0f && surfaceRead().groundedAt(feet,0.10f)) {
        sphericalGrounded_=true;
        sphericalVerticalVelocity_=0.0f;
    }

    const auto& env=planetSurface().environment();
    const Vec3 suitCenter=feet+::elysium::normalize(feet)*0.9f;
    const auto atmosphere=surfaceBase().atmosphereAt(planetSurface(),suitCenter);
    const AtmosphereSupportSample support{atmosphere.pressure,atmosphere.oxygen};
    const float oxygenDrain=effectiveOxygenDrain(env.oxygenDrainPerSecond,support);
    ecs_.updateVitals(dt,oxygenDrain,env.hazardDamagePerSecond,sprinting,moving);
    if (IsKeyPressed(KEY_ESCAPE)) sphericalSurfaceMode_=false;
}

void Game::updateSphericalInteraction(float dt) {
    if (IsKeyPressed(KEY_F)) trySphericalAttack();
    if (IsKeyPressed(KEY_V)) {
        sculptMode_=!sculptMode_;
        miningProgress_=0.0f;
        sphericalMiningValid_=false;
        setMessage(sculptMode_ ? "SPHERICAL MICRO SCULPT // 6.25 cm edits live" : "SPHERICAL MACRO TOOL // mining/building",2.2f);
    }
    if (IsKeyPressed(KEY_E)) {
        if (!trySurfacePortalInteract() && !trySurfaceMachineInteract() && nearShip()) {
            resetPlayerMetersAtShip();
            if (!tryShipRepairFromInventory()) {
                const auto check = shipTransit_.checklist();
                setMessage(TextFormat("Ship life support restored // hull %.0f (takeoff min %.0f)",
                                      check.hullIntegrity, check.minHullForTakeoff), 3.0f);
            }
        }
    }
    if (sculptMode_) {
        trySphericalMicroSculpt();
        return;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        if (portalMode_) trySphericalPlacePortal();
        else if (machineMode_) trySphericalPlaceMachine();
        else trySphericalPlaceBlock();
    }
    trySphericalMine(dt);
}

void Game::updatePlayer(float dt) {
    const Vector2 mouse = GetMouseDelta();
    yaw_ += mouse.x * 0.00225f;
    pitch_ -= mouse.y * 0.00225f;
    pitch_ = std::clamp(pitch_, -1.48f, 1.48f);

    PlayerInput input{};
    input.moveForward = (IsKeyDown(KEY_W) ? 1.0f : 0.0f) - (IsKeyDown(KEY_S) ? 1.0f : 0.0f);
    input.moveRight = (IsKeyDown(KEY_D) ? 1.0f : 0.0f) - (IsKeyDown(KEY_A) ? 1.0f : 0.0f);
    input.forward = cameraForward();
    input.jump = IsKeyPressed(KEY_SPACE);
    input.sprint = IsKeyDown(KEY_LEFT_SHIFT);

    const auto pos = ecs_.player().position;
    const float hazard = world().localHazardAt(pos);
    const IVec3 playerCell{static_cast<int>(std::floor(pos.x)),static_cast<int>(std::floor(pos.y+0.9f)),static_cast<int>(std::floor(pos.z))};
    const bool oxygenated = base().oxygenatedAt(world(),playerCell);
    const float oxygenDrain = effectiveOxygenDrain(world().environment().oxygenDrainPerSecond, oxygenated);
    ecs_.update(dt, input,
                [this](int x,int y,int z){ return world().isSolid(x,y,z); },
                oxygenDrain,
                hazard);

    if (IsKeyPressed(KEY_E)) {
        if (!tryMachineInteract() && nearShip()) {
            resetPlayerMetersAtShip();
            if (!tryShipRepairFromInventory()) {
                const auto check = shipTransit_.checklist();
                setMessage(TextFormat("Ship life support restored // hull %.0f (takeoff min %.0f)",
                                      check.hullIntegrity, check.minHullForTakeoff), 3.0f);
            }
        }
    }
}

void Game::updateInteraction(float dt) {
    if (IsKeyPressed(KEY_V)) {
        sculptMode_ = !sculptMode_;
        miningProgress_ = 0.0f;
        miningCell_ = {-999,-999,-999};
        setMessage(sculptMode_ ? "MICRO SCULPT // 6.25 cm chisel enabled" : "MACRO TOOL // mining/building enabled", 2.0f);
    }
    if (IsKeyPressed(KEY_F)) tryAttack();
    if (sculptMode_) {
        tryMicroSculpt();
        return;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        if (machineMode_) tryPlaceMachine(); else tryPlaceBlock();
    }
    tryMine(dt);
}

void Game::selectHotbar() {
    if (IsKeyPressed(KEY_ONE)) { selectedBlock_ = BlockType::Dirt; machineMode_=false; portalMode_=false; }
    if (IsKeyPressed(KEY_TWO)) { selectedBlock_ = BlockType::Stone; machineMode_=false; portalMode_=false; }
    if (IsKeyPressed(KEY_THREE)) { selectedBlock_ = BlockType::Planks; machineMode_=false; portalMode_=false; }
    if (IsKeyPressed(KEY_FOUR)) { selectedBlock_ = BlockType::SteelPlate; machineMode_=false; portalMode_=false; }
    if (IsKeyPressed(KEY_FIVE)) { selectedBlock_ = BlockType::RegistryBeacon; machineMode_=false; portalMode_=false; }
    if (IsKeyPressed(KEY_SIX)) { selectedMachine_=MachineType::BurnerGenerator; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_SEVEN)) { selectedMachine_=MachineType::BatteryBank; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_EIGHT)) { selectedMachine_=MachineType::AtmosphereUnit; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_NINE)) { selectedMachine_=MachineType::StorageCrate; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_K)) { selectedMachine_=MachineType::AirlockController; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_J)) { selectedMachine_=MachineType::SensorMast; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_U)) { selectedMachine_=MachineType::Turret; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_Y)) { selectedMachine_=MachineType::ShieldPylon; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_L)) { selectedMachine_=MachineType::LogicController; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_G)) { selectedMachine_=MachineType::Furnace; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_H)) { selectedMachine_=MachineType::AlloyCrucible; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_I)) { selectedMachine_=MachineType::Refinery; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_N)) { selectedMachine_=MachineType::NetworkStorage; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_Q)) { selectedMachine_=MachineType::Conveyor; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_R)) { selectedMachine_=MachineType::Sorter; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_X)) { selectedMachine_=MachineType::CargoLoader; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_Z)) { selectedMachine_=MachineType::Crusher; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_F1)) { selectedMachine_=MachineType::ChemicalVat; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_F2)) { selectedMachine_=MachineType::Fabricator; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_F3)) { selectedMachine_=MachineType::Extractor; machineMode_=true; portalMode_=false; }
    if (IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressed(KEY_RIGHT_BRACKET)) {
        constexpr int first=static_cast<int>(MachineType::BurnerGenerator);
        constexpr int last=static_cast<int>(MachineType::Extractor);
        int value=static_cast<int>(selectedMachine_);
        value += IsKeyPressed(KEY_RIGHT_BRACKET) ? 1 : -1;
        if(value>last) value=first;
        if(value<first) value=last;
        selectedMachine_=static_cast<MachineType>(value);
        machineMode_=true;
        portalMode_=false;
        setMessage(std::string("MACHINE SELECT // ")+machineName(selectedMachine_),1.4f);
    }
    if (IsKeyPressed(KEY_B)) { selectedPortal_=SurfacePortalType::Door; portalMode_=true; machineMode_=false; }
    if (IsKeyPressed(KEY_ZERO)) { selectedPortal_=SurfacePortalType::Airlock; portalMode_=true; machineMode_=false; }
}

void Game::tryMine(float dt) {
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        miningProgress_ = 0.0f;
        miningCell_ = {-999,-999,-999};
        return;
    }

    const auto hit = world().raycast(cameraPosition(), cameraForward(), 7.0f);
    if (!hit || !blockProperties(hit->type).mineable) {
        miningProgress_ = 0.0f;
        return;
    }

    if (!(hit->cell == miningCell_)) {
        miningCell_ = hit->cell;
        miningProgress_ = 0.0f;
    }

    const auto& props = blockProperties(hit->type);
    if (toolTier_ < props.harvestTier) {
        miningProgress_ = 0.0f;
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) setMessage("TOOL GATE // upgrade mining head at [C]");
        return;
    }

    const float mineSeconds = std::max(0.18f, props.hardness * 0.32f / (1.0f + 0.25f * static_cast<float>(toolTier_ - 1)));
    miningProgress_ += dt / mineSeconds;
    if (miningProgress_ >= 1.0f) {
        const BlockType mined = hit->type;
        const BlockType drop = miningDrop(mined);
        world().set(hit->cell.x, hit->cell.y, hit->cell.z, BlockType::Air);
        if (drop != BlockType::Air) ecs_.addItem(static_cast<int>(drop), 1);
        raiseSuspicion(props.suspicionOnMine);
        if (mined == BlockType::RegistryBeacon) {
            registryBeacons_.destroyBeacon(currentPlanet_);
            surfaceSiege().notifyBeaconDestroyed();
            setMessage("CLAIM STRUCK // structures and terrain edits remain persistent", 4.0f);
        } else {
            setMessage(std::string("Extracted ") + std::string(props.name));
        }
        miningProgress_ = 0.0f;
        miningCell_ = {-999,-999,-999};
    }
}

void Game::trySphericalMine(float dt) {
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        miningProgress_=0.0f;
        sphericalMiningValid_=false;
        return;
    }

    const auto hit=surfaceRead().raycast(sphericalCameraPosition(),sphericalCameraForward(),7.0f,0.08f);
    if (!hit.hit) {
        miningProgress_=0.0f;
        sphericalMiningValid_=false;
        return;
    }
    const BlockType type=planetSurface().get(hit.cell);
    const auto& props=blockProperties(type);
    if (!props.mineable) {
        miningProgress_=0.0f;
        sphericalMiningValid_=false;
        return;
    }
    if (!sphericalMiningValid_ || !(hit.cell==sphericalMiningCell_)) {
        sphericalMiningCell_=hit.cell;
        sphericalMiningValid_=true;
        miningProgress_=0.0f;
    }
    if (toolTier_<props.harvestTier) {
        miningProgress_=0.0f;
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) setMessage("TOOL GATE // upgrade mining head on the legacy fabrication screen [C]");
        return;
    }

    const float mineSeconds=std::max(0.18f,props.hardness*0.32f/(1.0f+0.25f*static_cast<float>(toolTier_-1)));
    miningProgress_+=dt/mineSeconds;
    if (miningProgress_<1.0f) return;

    const bool wasPlaced=planetSurface().playerPlaced(hit.cell);
    std::uint64_t minedPortalId{};
    SurfacePortalType minedPortalType{SurfacePortalType::Door};
    for(const auto& portal:surfaceBase().portals()) {
        if(portal.anchor==hit.cell) { minedPortalId=portal.stableId; minedPortalType=portal.type; break; }
    }
    if(minedPortalId!=0) {
        surfaceBase().removePortal(planetSurface(),minedPortalId,true);
        ecs_.addItem(surfacePortalItemId(minedPortalType),1);
    } else {
        planetSurface().set(hit.cell,BlockType::Air);
        const BlockType drop=miningDrop(type);
        if (drop!=BlockType::Air) ecs_.addItem(static_cast<int>(drop),1);
    }
    if (!wasPlaced) raiseSuspicion(props.suspicionOnMine);
    if (type==BlockType::RegistryBeacon) {
        registryBeacons_.destroyBeacon(currentPlanet_);
        surfaceSiege().notifyBeaconDestroyed();
        setMessage("CLAIM STRUCK ON CUBE-SPHERE // planetary edits remain",3.4f);
    } else {
        setMessage(std::string("SPHERICAL EXTRACT // ")+std::string(props.name),1.5f);
    }
    miningProgress_=0.0f;
    sphericalMiningValid_=false;
}

void Game::trySphericalMicroSculpt() {
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    const auto hit=surfaceRead().raycastMicro(sphericalCameraPosition(),sphericalCameraForward(),7.0f,0.012f);
    if (!hit.hit) return;
    const auto& props=blockProperties(hit.type);
    if (hit.type==BlockType::DoorPanel || hit.type==BlockType::AirlockPanel) {
        setMessage("PORTAL FRAME // remove with macro tool",1.4f);
        return;
    }
    if (!props.mineable) {
        setMessage("SPHERICAL MICRO // material cannot be cut",1.2f);
        return;
    }
    if (toolTier_<props.harvestTier) {
        setMessage("MICRO TOOL GATE // upgrade mining head at [C]",1.5f);
        return;
    }
    const bool wasPlaced=planetSurface().playerPlaced(hit.micro.cell);
    planetSurface().setMicro(hit.micro.cell,hit.micro.u,hit.micro.radial,hit.micro.v,BlockType::Air);
    if (!wasPlaced) raiseSuspicion(props.suspicionOnMine/static_cast<float>(MicroBrick::CellCount));
    setMessage("SPHERICAL MICRO CHISEL // persistent 6.25 cm edit",1.2f);
}

void Game::trySphericalPlaceMachine() {
    const auto hit=surfaceRead().raycast(sphericalCameraPosition(),sphericalCameraForward(),7.0f,0.06f);
    if (!hit.hit) return;
    const SurfaceCellAddress target=hit.previous;
    if (!planetSurface().radialInBounds(target.radial) || surfaceRead().solidAt(planetSurface().cellCenterPosition(target))) return;
    if (surfaceBase().occupied(target) || surfaceBase().portalOccupied(target)) { setMessage("Surface machine anchor occupied."); return; }
    const Vec3 center=planetSurface().cellCenterPosition(target);
    const Vec3 feet=sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)];
    const Vec3 suitCenter=feet+::elysium::normalize(feet)*0.9f;
    if (length(center-suitCenter)<1.05f) { setMessage("Machine placement obstructed by suit volume."); return; }
    const int item=machineItemId(selectedMachine_);
    if (!ecs_.consumeItem(item,1)) { setMessage(std::string("No ")+machineName(selectedMachine_)+" kit in inventory."); return; }
    surfaceBase().place(selectedMachine_,target);
    surfaceBase().update(planetSurface(),0.0f);
    raiseSuspicion(0.35f);
    setMessage(std::string("SURFACE MACHINE // ")+machineName(selectedMachine_),2.5f);
}

bool Game::trySurfaceMachineInteract() {
    const Vec3 feet=sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)];
    const Vec3 suitCenter=feet+::elysium::normalize(feet)*0.9f;
    if (auto* gen=surfaceBase().nearest(planetSurface(),MachineType::BurnerGenerator,suitCenter,2.8f)) {
        if (ecs_.consumeItem(static_cast<int>(BlockType::CoalOre),1)) {
            gen->fuelSeconds+=60.0f;
            setMessage("Surface Burner fueled // +60 s coal burn",2.5f);
        } else setMessage("Surface Burner requires Coal Ore.",2.0f);
        return true;
    }
    // Prototype route-authoring path for the standalone client. The persistent
    // contract remains stable-ID + owner-address; this convenience interaction
    // merely chooses nearby endpoints and calls the same validated industry API.
    auto tryConfigureNearbyLogistics=[&]()->bool {
        SurfaceMachineObject* transport=nullptr;
        for(const auto type:{MachineType::Conveyor,MachineType::Sorter,MachineType::CargoLoader}) {
            if(auto* candidate=surfaceBase().nearest(planetSurface(),type,suitCenter,2.8f)) { transport=candidate; break; }
        }
        if(!transport) return false;
        if(!IsKeyDown(KEY_LEFT_CONTROL) && !IsKeyDown(KEY_RIGHT_CONTROL)) return false;

        std::vector<const SurfaceMachineObject*> candidates;
        for(const auto& object:surfaceBase().objects()) {
            if(object.stableId==transport->stableId) continue;
            switch(object.type) {
                case MachineType::StorageCrate:
                case MachineType::Furnace:
                case MachineType::AlloyCrucible:
                case MachineType::Refinery:
                case MachineType::NetworkStorage:
                case MachineType::Conveyor:
                case MachineType::Sorter:
                case MachineType::CargoLoader:
                case MachineType::Crusher:
                case MachineType::ChemicalVat:
                case MachineType::Fabricator:
                case MachineType::Extractor:
                    break;
                default: continue;
            }
            const float d=lengthSq(planetSurface().cellCenterPosition(object.anchor)-planetSurface().cellCenterPosition(transport->anchor));
            if(d<=SurfaceIndustrySystem::MaxLogisticsLinkDistance*SurfaceIndustrySystem::MaxLogisticsLinkDistance)
                candidates.push_back(&object);
        }
        std::sort(candidates.begin(),candidates.end(),[&](const auto* a,const auto* b){
            const Vec3 t=planetSurface().cellCenterPosition(transport->anchor);
            const float da=lengthSq(planetSurface().cellCenterPosition(a->anchor)-t);
            const float db=lengthSq(planetSurface().cellCenterPosition(b->anchor)-t);
            if(std::abs(da-db)>1e-5f) return da<db;
            return a->stableId<b->stableId;
        });
        const std::size_t needed=transport->type==MachineType::Sorter?3U:2U;
        if(candidates.size()<needed) {
            setMessage("LOGISTICS LINK // place nearby source/target inventories first",2.8f);
            return true;
        }
        int filter=0;
        std::uint64_t alternate=0;
        if(transport->type==MachineType::Sorter) {
            alternate=candidates[2]->stableId;
            filter=!candidates[0]->inventory.empty()?candidates[0]->inventory.front().itemId:static_cast<int>(BlockType::CopperOre);
        }
        if(surfaceIndustry_.configureLogisticsLink(surfaceBase(),planetSurface(),transport->stableId,
                                                   candidates[0]->stableId,candidates[1]->stableId,alternate,filter)) {
            setMessage(std::string("LOGISTICS ROUTE // ")+machineName(transport->type)+" linked by stable IDs",3.0f);
        } else setMessage("LOGISTICS ROUTE // endpoint validation rejected",2.5f);
        return true;
    };
    if(tryConfigureNearbyLogistics()) return true;

    auto industryInteract=[&](MachineType type,const std::vector<int>& acceptedItems,const char* label)->bool {
        auto* machine=surfaceBase().nearest(planetSurface(),type,suitCenter,2.8f);
        if(!machine) return false;
        if(IsKeyDown(KEY_LEFT_SHIFT)) {
            if(machine->inventory.empty()) {
                setMessage(std::string(label)+" inventory empty.",1.8f);
                return true;
            }
            const int itemId=machine->inventory.front().itemId;
            if(surfaceIndustry_.extract(surfaceBase(),machine->stableId,itemId,1)==1) {
                ecs_.addItem(itemId,1);
                setMessage(std::string(label)+" // withdrew 1 "+industryItemName(itemId),2.0f);
            }
            return true;
        }
        for(const int itemId:acceptedItems) {
            if(ecs_.inventoryCount(itemId)<=0) continue;
            if(!ecs_.consumeItem(itemId,1)) continue;
            if(surfaceIndustry_.insert(surfaceBase(),machine->stableId,itemId,1)) {
                setMessage(std::string(label)+" // loaded 1 "+industryItemName(itemId),2.0f);
            } else {
                ecs_.addItem(itemId,1);
                setMessage(std::string(label)+" inventory full.",1.8f);
            }
            return true;
        }
        setMessage(std::string(label)+" // no compatible material; hold Shift+E to withdraw",2.4f);
        return true;
    };
    if(industryInteract(MachineType::Furnace,{static_cast<int>(BlockType::CopperOre),static_cast<int>(BlockType::TinOre),static_cast<int>(BlockType::IronOre),static_cast<int>(BlockType::CoalOre)},"FURNACE")) return true;
    if(industryInteract(MachineType::AlloyCrucible,{static_cast<int>(IndustryItemId::CopperIngot),static_cast<int>(IndustryItemId::TinIngot),static_cast<int>(IndustryItemId::IronIngot),static_cast<int>(IndustryItemId::Carbon)},"ALLOY CRUCIBLE")) return true;
    if(industryInteract(MachineType::Crusher,{static_cast<int>(BlockType::Stone),static_cast<int>(BlockType::CopperOre),static_cast<int>(BlockType::TinOre),static_cast<int>(BlockType::IronOre)},"CRUSHER")) return true;
    if(industryInteract(MachineType::Refinery,{static_cast<int>(BlockType::CopperOre),static_cast<int>(BlockType::TinOre),static_cast<int>(BlockType::IronOre),static_cast<int>(IndustryItemId::CopperConcentrate),static_cast<int>(IndustryItemId::TinConcentrate),static_cast<int>(IndustryItemId::IronConcentrate)},"REFINERY")) return true;
    if(industryInteract(MachineType::ChemicalVat,{static_cast<int>(IndustryItemId::StoneAggregate),static_cast<int>(IndustryItemId::Carbon)},"CHEMICAL VAT")) return true;
    if(industryInteract(MachineType::Fabricator,{static_cast<int>(IndustryItemId::CopperIngot),static_cast<int>(IndustryItemId::SteelIngot),static_cast<int>(IndustryItemId::BronzeIngot),static_cast<int>(IndustryItemId::Carbon),static_cast<int>(IndustryItemId::StoneAggregate),static_cast<int>(IndustryItemId::Sealant),static_cast<int>(IndustryItemId::CopperWire),static_cast<int>(IndustryItemId::SteelFrame),static_cast<int>(IndustryItemId::Actuator),static_cast<int>(IndustryItemId::ControlCircuit),static_cast<int>(BlockType::Planks)},"FABRICATOR")) return true;
    if(industryInteract(MachineType::Extractor,{},"EXTRACTOR")) return true;
    if(industryInteract(MachineType::Conveyor,{static_cast<int>(BlockType::CopperOre),static_cast<int>(BlockType::TinOre),static_cast<int>(BlockType::IronOre),static_cast<int>(IndustryItemId::CopperConcentrate),static_cast<int>(IndustryItemId::TinConcentrate),static_cast<int>(IndustryItemId::IronConcentrate)},"CONVEYOR")) return true;
    if(industryInteract(MachineType::Sorter,{static_cast<int>(BlockType::CopperOre),static_cast<int>(BlockType::TinOre),static_cast<int>(BlockType::IronOre),static_cast<int>(IndustryItemId::CopperIngot),static_cast<int>(IndustryItemId::SteelIngot)},"SORTER")) return true;
    if(industryInteract(MachineType::CargoLoader,{static_cast<int>(BlockType::SteelPlate),static_cast<int>(IndustryItemId::SteelIngot),static_cast<int>(IndustryItemId::RepairKit),static_cast<int>(IndustryItemId::TurretAmmo)},"CARGO LOADER")) return true;
    if(industryInteract(MachineType::NetworkStorage,{static_cast<int>(BlockType::CoalOre),static_cast<int>(BlockType::CopperOre),static_cast<int>(BlockType::TinOre),static_cast<int>(BlockType::IronOre),static_cast<int>(BlockType::Stone),static_cast<int>(BlockType::Planks),static_cast<int>(BlockType::SteelPlate),static_cast<int>(IndustryItemId::CopperIngot),static_cast<int>(IndustryItemId::TinIngot),static_cast<int>(IndustryItemId::IronIngot),static_cast<int>(IndustryItemId::Carbon),static_cast<int>(IndustryItemId::BronzeIngot),static_cast<int>(IndustryItemId::SteelIngot),static_cast<int>(IndustryItemId::StoneAggregate),static_cast<int>(IndustryItemId::CopperConcentrate),static_cast<int>(IndustryItemId::TinConcentrate),static_cast<int>(IndustryItemId::IronConcentrate),static_cast<int>(IndustryItemId::Sealant),static_cast<int>(IndustryItemId::CopperWire),static_cast<int>(IndustryItemId::SteelFrame),static_cast<int>(IndustryItemId::Actuator),static_cast<int>(IndustryItemId::ControlCircuit),static_cast<int>(IndustryItemId::SensorPackage),static_cast<int>(IndustryItemId::TurretAmmo),static_cast<int>(IndustryItemId::RepairKit),static_cast<int>(IndustryItemId::FilterCartridge),static_cast<int>(IndustryItemId::MachineCasing),static_cast<int>(IndustryItemId::CompositePanel)},"NETWORK STORAGE")) return true;
    if(auto* controller=surfaceBase().nearest(planetSurface(),MachineType::AirlockController,suitCenter,3.0f)) {
        auto* assembly=surfaceBase().airlockForController(controller->stableId);
        if(!assembly) {
            std::vector<const SurfacePortalObject*> candidates;
            for(const auto& portal:surfaceBase().portals()) {
                if(portal.type!=SurfacePortalType::Airlock || surfaceBase().airlockForPortal(portal.stableId)) continue;
                if(length(planetSurface().cellCenterPosition(portal.anchor)-planetSurface().cellCenterPosition(controller->anchor))<=6.0f)
                    candidates.push_back(&portal);
            }
            std::sort(candidates.begin(),candidates.end(),[this](const auto* a,const auto* b){
                if(a->anchor.radial!=b->anchor.radial) return a->anchor.radial<b->anchor.radial;
                return a->stableId<b->stableId;
            });
            if(candidates.size()<2) {
                setMessage("Airlock Controller needs two nearby unpaired Airlock portals.",3.0f);
                return true;
            }
            const auto id=surfaceBase().createAirlockAssembly(planetSurface(),controller->stableId,candidates[0]->stableId,candidates[1]->stableId,controller->anchor,1.0f,1.0f);
            if(id==0) setMessage("Airlock pairing rejected.",2.5f);
            else setMessage("AIRLOCK INTERLOCK PAIRED // E cycles exterior/interior",3.2f);
            return true;
        }
        if(assembly->state==SurfaceAirlockState::Depressurizing || assembly->state==SurfaceAirlockState::Pressurizing) {
            setMessage(std::string("AIRLOCK // ")+surfaceAirlockStateName(assembly->state),1.8f);
            return true;
        }
        const auto destination=assembly->state==SurfaceAirlockState::ExteriorOpen
            ? SurfaceAirlockDestination::Interior
            : SurfaceAirlockDestination::Exterior;
        if(surfaceBase().requestAirlockCycle(planetSurface(),assembly->stableId,destination))
            setMessage(destination==SurfaceAirlockDestination::Exterior?"AIRLOCK CYCLE // depressurizing":"AIRLOCK CYCLE // pressurizing",2.3f);
        else setMessage("AIRLOCK CONTROLLER FAULT",2.3f);
        return true;
    }
    if(auto* turret=surfaceBase().nearest(planetSurface(),MachineType::Turret,suitCenter,2.8f)) {
        if(ecs_.consumeItem(static_cast<int>(BlockType::SteelPlate),1)) {
            turret->ammo+=12;
            setMessage("TURRET MAGAZINE // +12 rounds from 1 Steel Plate",2.5f);
        } else setMessage("Turret reload requires 1 Steel Plate.",2.0f);
        return true;
    }
    if(auto* logic=surfaceBase().nearest(planetSurface(),MachineType::LogicController,suitCenter,3.0f)) {
        int existing=0;
        for(const auto& rule:surfaceBase().automationRules()) if(rule.controllerMachineId==logic->stableId) ++existing;
        if(existing>0) {
            setMessage("LOGIC CONTROLLER // automation rules already configured",2.5f);
            return true;
        }
        const Vec3 logicPos=planetSurface().cellCenterPosition(logic->anchor);
        SurfaceMachineObject* sensor=surfaceBase().nearest(planetSurface(),MachineType::SensorMast,logicPos,12.0f);
        SurfaceMachineObject* turret=surfaceBase().nearest(planetSurface(),MachineType::Turret,logicPos,12.0f);
        if(!sensor || !turret) {
            setMessage("Logic Controller needs a Sensor Mast + Turret within 12 m.",3.0f);
            return true;
        }
        turret->enabled=false;
        const auto ruleId=surfaceBase().createAutomationRule(logic->stableId,SurfaceAutomationTrigger::HostilesDetected,
                                                              sensor->stableId,0.0f,SurfaceAutomationAction::EnableMachine,turret->stableId);
        if(ruleId==0) { setMessage("Automation rule creation rejected.",2.5f); return true; }
        if(auto* portal=surfaceBase().nearestPortal(planetSurface(),logicPos,12.0f))
            surfaceBase().createAutomationRule(logic->stableId,SurfaceAutomationTrigger::HostilesDetected,
                                               sensor->stableId,0.0f,SurfaceAutomationAction::ClosePortal,portal->stableId);
        setMessage("AUTOMATION // hostiles enable turret + close nearest portal",3.2f);
        return true;
    }
    return false;
}

void Game::trySphericalPlacePortal() {
    const auto hit=surfaceRead().raycast(sphericalCameraPosition(),sphericalCameraForward(),7.0f,0.06f);
    if(!hit.hit) return;
    const SurfaceCellAddress target=hit.previous;
    if(!planetSurface().radialInBounds(target.radial) || planetSurface().get(target)!=BlockType::Air) return;
    if(surfaceBase().occupied(target) || surfaceBase().portalOccupied(target)) { setMessage("Surface anchor occupied."); return; }
    const Vec3 center=planetSurface().cellCenterPosition(target);
    const Vec3 feet=sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)];
    const Vec3 suitCenter=feet+normalize(feet)*0.9f;
    if(length(center-suitCenter)<1.05f) { setMessage("Portal placement obstructed by suit volume."); return; }
    const int item=surfacePortalItemId(selectedPortal_);
    if(!ecs_.consumeItem(item,1)) { setMessage(std::string("No ")+surfacePortalName(selectedPortal_)+" kit in inventory."); return; }
    const auto id=surfaceBase().placePortal(planetSurface(),selectedPortal_,target,false);
    if(id==0) { ecs_.addItem(item,1); setMessage("Portal placement rejected."); return; }
    raiseSuspicion(selectedPortal_==SurfacePortalType::Airlock?0.28f:0.18f);
    setMessage(std::string("SURFACE ")+surfacePortalName(selectedPortal_)+" // closed + sealed",2.5f);
}

bool Game::trySurfacePortalInteract() {
    const Vec3 feet=sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)];
    const Vec3 suitCenter=feet+normalize(feet)*0.9f;
    auto* portal=surfaceBase().nearestPortal(planetSurface(),suitCenter,2.6f);
    if(!portal) return false;
    const bool opening=!portal->open;
    if(!surfaceBase().setPortalOpen(planetSurface(),portal->stableId,opening)) {
        if(surfaceBase().airlockForPortal(portal->stableId)) setMessage("INTERLOCKED AIRLOCK // use its controller",2.1f);
        return true;
    }
    setMessage(std::string(surfacePortalName(portal->type))+(opening?" OPEN // atmosphere breach possible":" CLOSED // seal restored"),2.2f);
    return true;
}

void Game::trySphericalPlaceBlock() {
    const auto hit=surfaceRead().raycast(sphericalCameraPosition(),sphericalCameraForward(),7.0f,0.08f);
    if (!hit.hit) return;
    const auto& props=blockProperties(selectedBlock_);
    if (!props.placeable) return;
    const SurfaceCellAddress target=hit.previous;
    if (!planetSurface().radialInBounds(target.radial) || planetSurface().get(target)!=BlockType::Air || target==hit.cell) return;

    const Vec3 center=planetSurface().cellCenterPosition(target);
    const Vec3 suitCenter=sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)]+normalize(sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)])*0.9f;
    if (length(center-suitCenter)<1.15f) {
        setMessage("Placement obstructed by suit volume.");
        return;
    }
    const int id=static_cast<int>(selectedBlock_);
    if (!ecs_.consumeItem(id,1)) {
        setMessage(std::string("No ")+std::string(props.name)+" in inventory.");
        return;
    }
    planetSurface().set(target,selectedBlock_,true);
    raiseSuspicion(0.15f);
    if (selectedBlock_==BlockType::RegistryBeacon) {
        registryBeacons_.fileClaim(currentPlanet_, RegistryBeaconDomain::Spherical);
        suspicion().setClaimFloor(claimFloor());
        setMessage("REGISTRY BEACON ONLINE // cube-sphere claim filed",4.0f);
    }
}

void Game::tryMicroSculpt() {
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    const auto hit = world().raycastMicro(cameraPosition(), cameraForward(), 7.0f);
    if (!hit) return;
    const auto& props = blockProperties(hit->type);
    if (!props.mineable) {
        setMessage("MICRO SCULPT // material cannot be cut", 1.2f);
        return;
    }
    if (toolTier_ < props.harvestTier) {
        setMessage("MICRO TOOL GATE // upgrade mining head at [C]", 1.5f);
        return;
    }

    world().setMicroGlobal(hit->globalMicro.x, hit->globalMicro.y, hit->globalMicro.z, BlockType::Air);
    // Micro-chiseling deliberately yields no item: a 1/4096 m^3 chip must not
    // become a full block and create a duplication exploit.
    raiseSuspicion(props.suspicionOnMine / static_cast<float>(MicroBrick::CellCount));
    setMessage("MICRO CHISEL // persistent 6.25 cm edit (no full-block drop)", 1.2f);
}

void Game::tryPlaceBlock() {
    const auto hit = world().raycast(cameraPosition(), cameraForward(), 7.0f);
    if (!hit) return;
    const auto& props = blockProperties(selectedBlock_);
    if (!props.placeable) return;

    const IVec3 c{hit->cell.x + hit->normal.x, hit->cell.y + hit->normal.y, hit->cell.z + hit->normal.z};
    if (!world().inBounds(c.x,c.y,c.z) || world().get(c.x,c.y,c.z) != BlockType::Air) return;

    const Vec3 p = ecs_.player().position;
    const Vec3 blockCenter{c.x + 0.5f, c.y + 0.5f, c.z + 0.5f};
    if (length(blockCenter - (p + Vec3{0,0.9f,0})) < 1.05f) {
        setMessage("Placement obstructed by suit volume.");
        return;
    }

    const int id = static_cast<int>(selectedBlock_);
    if (!ecs_.consumeItem(id, 1)) {
        setMessage(std::string("No ") + std::string(props.name) + " in inventory.");
        return;
    }

    world().set(c.x,c.y,c.z, selectedBlock_);
    raiseSuspicion(0.15f); // proposed activity pressure: tuning value for this prototype
    if (selectedBlock_ == BlockType::RegistryBeacon) {
        registryBeacons_.fileClaim(currentPlanet_, RegistryBeaconDomain::Planar);
        suspicion().setClaimFloor(claimFloor());
        setMessage("REGISTRY BEACON ONLINE // planetary claim filed", 4.0f);
    }
}


void Game::tryPlaceMachine() {
    const auto hit=world().raycast(cameraPosition(),cameraForward(),7.0f);
    if(!hit) return;
    const IVec3 c{hit->cell.x+hit->normal.x,hit->cell.y+hit->normal.y,hit->cell.z+hit->normal.z};
    if(!world().inBounds(c.x,c.y,c.z) || world().isSolid(c.x,c.y,c.z)) return;
    for(const auto& o:base().objects()) if(o.anchor==c) { setMessage("Machine anchor occupied."); return; }
    const Vec3 p=ecs_.player().position;
    const Vec3 center{c.x+0.5f,c.y+0.5f,c.z+0.5f};
    if(length(center-(p+Vec3{0,0.9f,0}))<1.0f){setMessage("Machine placement obstructed by suit volume.");return;}
    const int item=machineItemId(selectedMachine_);
    if(!ecs_.consumeItem(item,1)){setMessage(std::string("No ")+machineName(selectedMachine_)+" kit in inventory.");return;}
    base().place(selectedMachine_,c);
    raiseSuspicion(0.35f);
    setMessage(std::string("PLACED // ")+machineName(selectedMachine_),2.5f);
}

bool Game::tryMachineInteract() {
    if(auto* gen=base().nearest(MachineType::BurnerGenerator,ecs_.player().position,2.8f)) {
        if(ecs_.consumeItem(static_cast<int>(BlockType::CoalOre),1)) {
            gen->fuelSeconds += 60.0f;
            setMessage("Burner Generator fueled // +60 s coal burn",2.5f);
        } else setMessage("Burner Generator requires Coal Ore.",2.0f);
        return true;
    }
    return false;
}

void Game::tryAttack() {
    const auto result = ecs_.attackRay(cameraPosition(), cameraForward(), 18.0f, 18.0f);
    if (result.hit) {
        if (result.killed) {
            ecs_.addItem(static_cast<int>(BlockType::SteelPlate), 1);
            setMessage("Imperial drone neutralized // salvage +1 Steel Plate");
        } else setMessage("Drone hit.", 1.0f);
    }
}

void Game::trySphericalAttack() {
    const auto result=ecs_.attackRay(sphericalCameraPosition(),sphericalCameraForward(),22.0f,18.0f,ActorDomain::PlanetSurface);
    if (!result.hit) return;
    if (result.killed) {
        ecs_.addItem(static_cast<int>(BlockType::SteelPlate),1);
        setMessage("SURFACE DRONE NEUTRALIZED // salvage +1 Steel Plate",2.0f);
    } else setMessage("Surface drone hit.",1.0f);
}

void Game::tryCraft(int recipe) {
    auto have = [this](BlockType t, int n){ return ecs_.inventoryCount(static_cast<int>(t)) >= n; };
    auto use = [this](BlockType t, int n){ return ecs_.consumeItem(static_cast<int>(t), n); };
    auto haveItem = [this](IndustryItemId id, int n){ return ecs_.inventoryCount(static_cast<int>(id)) >= n; };
    auto useItem = [this](IndustryItemId id, int n){ return ecs_.consumeItem(static_cast<int>(id), n); };

    auto applyHead = [&](EarlyMetallurgy::HeadKind kind) {
        const auto eval = EarlyMetallurgy::evaluate(
            toolTier_, kind,
            ecs_.inventoryCount(static_cast<int>(BlockType::Stone)),
            ecs_.inventoryCount(static_cast<int>(BlockType::CopperOre)),
            ecs_.inventoryCount(static_cast<int>(BlockType::TinOre)),
            ecs_.inventoryCount(static_cast<int>(IndustryItemId::BronzeIngot)),
            ecs_.inventoryCount(static_cast<int>(BlockType::IronOre)),
            ecs_.inventoryCount(static_cast<int>(BlockType::CoalOre)),
            ecs_.inventoryCount(static_cast<int>(IndustryItemId::SteelIngot)));
        if (!eval.canApply) { setMessage(std::string(eval.message)); return; }
        using CM = EarlyMetallurgy::CostMode;
        switch (eval.cost.mode) {
        case CM::StoneBlocks:
            use(BlockType::Stone, eval.cost.primaryCount); break;
        case CM::BronzeIngots:
            useItem(IndustryItemId::BronzeIngot, eval.cost.primaryCount); break;
        case CM::BronzeOres:
            use(BlockType::CopperOre, eval.cost.primaryCount);
            use(BlockType::TinOre, eval.cost.secondaryCount); break;
        case CM::SteelIngots:
            useItem(IndustryItemId::SteelIngot, eval.cost.primaryCount); break;
        case CM::SteelOres:
            use(BlockType::IronOre, eval.cost.primaryCount);
            use(BlockType::CoalOre, eval.cost.secondaryCount); break;
        case CM::None:
            break;
        }
        toolTier_ = eval.nextTier;
        setMessage(std::string(eval.message));
    };

    if (recipe == 1) {
        // Progressive [1]: stone head from improvised, then bronze head.
        if (toolTier_ < EarlyMetallurgy::kStone) applyHead(EarlyMetallurgy::HeadKind::Stone);
        else applyHead(EarlyMetallurgy::HeadKind::Bronze);
    } else if (recipe == 2) {
        applyHead(EarlyMetallurgy::HeadKind::Steel);
    } else if (recipe == 3) {
        if (have(BlockType::IronOre,2) && have(BlockType::CoalOre,1)) {
            use(BlockType::IronOre,2); use(BlockType::CoalOre,1);
            ecs_.addItem(static_cast<int>(BlockType::SteelPlate),1);
            setMessage("Fabricated 1 Steel Plate.");
        } else setMessage("Need 2 Iron Ore + 1 Coal Ore.");
    } else if (recipe == 4) {
        if (have(BlockType::SteelPlate,4) && have(BlockType::CopperOre,2)) {
            use(BlockType::SteelPlate,4); use(BlockType::CopperOre,2);
            ecs_.addItem(static_cast<int>(BlockType::RegistryBeacon),1);
            setMessage("Fabricated Registry Beacon.");
        } else setMessage("Need 4 Steel Plate + 2 Copper Ore.");
    } else if (recipe == 5) {
        if (have(BlockType::SteelPlate,2) && have(BlockType::CopperOre,2)) {
            use(BlockType::SteelPlate,2); use(BlockType::CopperOre,2);
            ecs_.addItem(machineItemId(MachineType::BurnerGenerator),1);
            setMessage("Fabricated Burner Generator kit.");
        } else setMessage("Need 2 Steel Plate + 2 Copper Ore.");
    } else if (recipe == 6) {
        if (have(BlockType::SteelPlate,2) && have(BlockType::CopperOre,3)) {
            use(BlockType::SteelPlate,2); use(BlockType::CopperOre,3);
            ecs_.addItem(machineItemId(MachineType::BatteryBank),1);
            setMessage("Fabricated Battery Bank kit.");
        } else setMessage("Need 2 Steel Plate + 3 Copper Ore.");
    } else if (recipe == 7) {
        if (have(BlockType::SteelPlate,3) && have(BlockType::CopperOre,2)) {
            use(BlockType::SteelPlate,3); use(BlockType::CopperOre,2);
            ecs_.addItem(machineItemId(MachineType::AtmosphereUnit),1);
            setMessage("Fabricated Atmosphere Unit kit.");
        } else setMessage("Need 3 Steel Plate + 2 Copper Ore.");
    } else if (recipe == 8) {
        if (have(BlockType::SteelPlate,1) && have(BlockType::Planks,4)) {
            use(BlockType::SteelPlate,1); use(BlockType::Planks,4);
            ecs_.addItem(machineItemId(MachineType::StorageCrate),1);
            setMessage("Fabricated Storage Crate kit.");
        } else setMessage("Need 1 Steel Plate + 4 Planks.");
    } else if (recipe == 9) {
        if (have(BlockType::SteelPlate,1) && have(BlockType::Planks,2)) {
            use(BlockType::SteelPlate,1); use(BlockType::Planks,2);
            ecs_.addItem(surfacePortalItemId(SurfacePortalType::Door),1);
            setMessage("Fabricated Door kit.");
        } else setMessage("Need 1 Steel Plate + 2 Planks.");
    } else if (recipe == 10) {
        if (have(BlockType::SteelPlate,3) && have(BlockType::CopperOre,1)) {
            use(BlockType::SteelPlate,3); use(BlockType::CopperOre,1);
            ecs_.addItem(surfacePortalItemId(SurfacePortalType::Airlock),1);
            setMessage("Fabricated Airlock kit.");
        } else setMessage("Need 3 Steel Plate + 1 Copper Ore.");
    } else if (recipe == 11) {
        if (have(BlockType::SteelPlate,2) && have(BlockType::CopperOre,2)) {
            use(BlockType::SteelPlate,2); use(BlockType::CopperOre,2);
            ecs_.addItem(machineItemId(MachineType::AirlockController),1);
            setMessage("Fabricated Airlock Controller kit.");
        } else setMessage("Need 2 Steel Plate + 2 Copper Ore.");
    } else if (recipe == 12) {
        if (have(BlockType::SteelPlate,2) && have(BlockType::CopperOre,2)) {
            use(BlockType::SteelPlate,2); use(BlockType::CopperOre,2);
            ecs_.addItem(machineItemId(MachineType::SensorMast),1);
            setMessage("Fabricated Sensor Mast kit.");
        } else setMessage("Need 2 Steel Plate + 2 Copper Ore.");
    } else if (recipe == 13) {
        if (have(BlockType::SteelPlate,4) && have(BlockType::CopperOre,2)) {
            use(BlockType::SteelPlate,4); use(BlockType::CopperOre,2);
            ecs_.addItem(machineItemId(MachineType::Turret),1);
            setMessage("Fabricated Turret kit.");
        } else setMessage("Need 4 Steel Plate + 2 Copper Ore.");
    } else if (recipe == 14) {
        if (have(BlockType::SteelPlate,6) && have(BlockType::CopperOre,3)) {
            use(BlockType::SteelPlate,6); use(BlockType::CopperOre,3);
            ecs_.addItem(machineItemId(MachineType::ShieldPylon),1);
            setMessage("Fabricated Shield Pylon kit.");
        } else setMessage("Need 6 Steel Plate + 3 Copper Ore.");
    } else if (recipe == 15) {
        if (have(BlockType::SteelPlate,2) && have(BlockType::CopperOre,3)) {
            use(BlockType::SteelPlate,2); use(BlockType::CopperOre,3);
            ecs_.addItem(machineItemId(MachineType::LogicController),1);
            setMessage("Fabricated Logic Controller kit.");
        } else setMessage("Need 2 Steel Plate + 3 Copper Ore.");
    } else if (recipe == 16) {
        if (have(BlockType::SteelPlate,2) && have(BlockType::CoalOre,1)) {
            use(BlockType::SteelPlate,2); use(BlockType::CoalOre,1);
            ecs_.addItem(machineItemId(MachineType::Furnace),1);
            setMessage("Fabricated Furnace kit.");
        } else setMessage("Need 2 Steel Plate + 1 Coal Ore.");
    } else if (recipe == 17) {
        if (have(BlockType::SteelPlate,2) && have(BlockType::CopperOre,2)) {
            use(BlockType::SteelPlate,2); use(BlockType::CopperOre,2);
            ecs_.addItem(machineItemId(MachineType::AlloyCrucible),1);
            setMessage("Fabricated Alloy Crucible kit.");
        } else setMessage("Need 2 Steel Plate + 2 Copper Ore.");
    } else if (recipe == 18) {
        if (have(BlockType::SteelPlate,4) && have(BlockType::CopperOre,3)) {
            use(BlockType::SteelPlate,4); use(BlockType::CopperOre,3);
            ecs_.addItem(machineItemId(MachineType::Refinery),1);
            setMessage("Fabricated Refinery kit.");
        } else setMessage("Need 4 Steel Plate + 3 Copper Ore.");
    } else if (recipe == 19) {
        if (have(BlockType::SteelPlate,2) && have(BlockType::CopperOre,2)) {
            use(BlockType::SteelPlate,2); use(BlockType::CopperOre,2);
            ecs_.addItem(machineItemId(MachineType::NetworkStorage),1);
            setMessage("Fabricated Network Storage kit.");
        } else setMessage("Need 2 Steel Plate + 2 Copper Ore.");
    } else if (recipe == 20) {
        if (haveItem(IndustryItemId::SteelFrame,1) && haveItem(IndustryItemId::CopperWire,1)) {
            useItem(IndustryItemId::SteelFrame,1); useItem(IndustryItemId::CopperWire,1);
            ecs_.addItem(machineItemId(MachineType::Conveyor),2);
            setMessage("Fabricated 2 Conveyor kits.");
        } else setMessage("Need 1 Steel Frame + 1 Copper Wire.");
    } else if (recipe == 21) {
        if (haveItem(IndustryItemId::SteelFrame,1) && haveItem(IndustryItemId::ControlCircuit,1)) {
            useItem(IndustryItemId::SteelFrame,1); useItem(IndustryItemId::ControlCircuit,1);
            ecs_.addItem(machineItemId(MachineType::Sorter),1);
            setMessage("Fabricated Sorter kit.");
        } else setMessage("Need 1 Steel Frame + 1 Control Circuit.");
    } else if (recipe == 22) {
        if (haveItem(IndustryItemId::SteelFrame,2) && haveItem(IndustryItemId::Actuator,1)) {
            useItem(IndustryItemId::SteelFrame,2); useItem(IndustryItemId::Actuator,1);
            ecs_.addItem(machineItemId(MachineType::CargoLoader),1);
            setMessage("Fabricated Cargo Loader kit.");
        } else setMessage("Need 2 Steel Frames + 1 Actuator.");
    } else if (recipe == 23) {
        if (haveItem(IndustryItemId::SteelFrame,1) && haveItem(IndustryItemId::Actuator,1)) {
            useItem(IndustryItemId::SteelFrame,1); useItem(IndustryItemId::Actuator,1);
            ecs_.addItem(machineItemId(MachineType::Crusher),1);
            setMessage("Fabricated Crusher kit.");
        } else setMessage("Need 1 Steel Frame + 1 Actuator.");
    } else if (recipe == 24) {
        if (haveItem(IndustryItemId::SteelFrame,1) && haveItem(IndustryItemId::Sealant,2)) {
            useItem(IndustryItemId::SteelFrame,1); useItem(IndustryItemId::Sealant,2);
            ecs_.addItem(machineItemId(MachineType::ChemicalVat),1);
            setMessage("Fabricated Chemical Vat kit.");
        } else setMessage("Need 1 Steel Frame + 2 Sealant.");
    } else if (recipe == 25) {
        if (haveItem(IndustryItemId::MachineCasing,1) && haveItem(IndustryItemId::ControlCircuit,1) && haveItem(IndustryItemId::Actuator,1)) {
            useItem(IndustryItemId::MachineCasing,1); useItem(IndustryItemId::ControlCircuit,1); useItem(IndustryItemId::Actuator,1);
            ecs_.addItem(machineItemId(MachineType::Fabricator),1);
            setMessage("Fabricated Fabricator kit.");
        } else setMessage("Need Machine Casing + Control Circuit + Actuator.");
    } else if (recipe == 26) {
        if (haveItem(IndustryItemId::MachineCasing,1) && haveItem(IndustryItemId::Actuator,2) && haveItem(IndustryItemId::SensorPackage,1)) {
            useItem(IndustryItemId::MachineCasing,1); useItem(IndustryItemId::Actuator,2); useItem(IndustryItemId::SensorPackage,1);
            ecs_.addItem(machineItemId(MachineType::Extractor),1);
            setMessage("Fabricated Extractor kit.");
        } else setMessage("Need Machine Casing + 2 Actuators + Sensor Package.");
    }
}

void Game::updateEmpire(float dt) {
    auto& director=surfaceSiege();

    // Stable-ID reconciliation keeps the enforcement director independent of
    // EnTT entity values and combat implementation details.
    std::vector<std::uint64_t> liveIds;
    const ActorDomain domain=sphericalSurfaceMode_?ActorDomain::PlanetSurface:ActorDomain::Planar;
    for(const auto& enemy:ecs_.enemies(domain)) liveIds.push_back(enemy.stableId);
    const auto phaseBeforeReconcile=director.state().phase;
    director.reconcileLiveEnemies(liveIds);

    if(phaseBeforeReconcile!=director.state().phase && director.state().phase==RegisterActionPhase::Cleared) {
        const bool wasRegister=director.state().type==ImperialEnforcementType::RegisterAction;
        if(wasRegister) {
            suspicion().setClaimFloor(claimFloor());
            suspicion().reduceTowardFloor(25.0f); // prototype tuning: "toward structural floor"
            favor_=std::min(100.0f,favor_+4.0f);
            ecs_.addItem(static_cast<int>(BlockType::SteelPlate),6);
            setMessage("REGISTER ACTION CLEARED // salvage secured // file pressure receding",6.0f);
        } else setMessage("IMPERIAL PATROL CLEARED",3.5f);
        director.clearTerminalState();
    }

    if(director.state().phase==RegisterActionPhase::Failed) {
        setMessage("REGISTER ACTION FAILED // claim struck // structures remain",6.0f);
        director.clearTerminalState();
    }

    // The normal Suspicion dispatch roll remains the trigger. The director owns
    // what that successful roll means at Noted/Marked/Hunted attention bands.
    dispatchTimer_ += dt;
    if(!director.active() && !director.terminal() && suspicion().value()>=25.0f && dispatchTimer_>=8.0f) {
        dispatchTimer_=0.0f;
        const float chance=std::min(0.40f,(suspicion().value()-25.0f)/200.0f);
        const float roll=hash01(galaxySeed_,static_cast<int>(dispatchRollCounter_++),currentPlanet_,0,0x44495350ULL);
        if(roll<chance && director.requestEnforcement(suspicion().value(),registryBeacons_.claimed(currentPlanet_))) {
            if(director.state().type==ImperialEnforcementType::RegisterAction) {
                setMessage(std::string("REGISTER ACTION ANNOUNCED // ")+imperialAttentionBandName(director.state().band)+
                           " // ~10 MINUTES TO CONTACT",6.0f);
            } else setMessage("IMPERIAL PATROL DISPATCHED // signature acquired",4.0f);
        }
    }

    const auto phaseBeforeUpdate=director.state().phase;
    const int waveBefore=director.state().waveIndex;
    auto spawns=director.update(dt,registryBeacons_.claimed(currentPlanet_),
                               registryBeacons_.beaconIntact(currentPlanet_));
    if(director.state().phase==RegisterActionPhase::Failed && phaseBeforeUpdate!=RegisterActionPhase::Failed) {
        setMessage("REGISTRY BEACON LOST // REGISTER ACTION FAILURE // claim released",6.0f);
        director.clearTerminalState();
        return;
    }

    if(spawns.empty()) return;

    if(sphericalSurfaceMode_) {
        const Vec3 feet=sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)];
        const auto frame=planetSurface().surfaceFrame(feet);
        const Vec3 baseDir=normalize(feet);
        const float baseRadius=length(feet);
        for(const auto& request:spawns) {
            const Vec3 tangent=frame.right*std::cos(request.azimuthRadians)+frame.forward*std::sin(request.azimuthRadians);
            const Vec3 dir=normalize(baseDir+tangent*(request.distanceMeters/std::max(1.0f,baseRadius)));
            const Vec3 pos=dir*(surfaceRead().surfaceBoundaryRadius(dir)+1.8f);
            ecs_.queueSpawnEnemy(pos,enemyArchetypeFor(request.role),request.stableEnemyId,ActorDomain::PlanetSurface);
        }
    } else {
        const auto player=ecs_.player().position;
        for(const auto& request:spawns) {
            Vec3 pos=player+Vec3{std::sin(request.azimuthRadians)*request.distanceMeters,2.0f,
                                std::cos(request.azimuthRadians)*request.distanceMeters};
            pos.y=static_cast<float>(world().surfaceY(std::clamp(static_cast<int>(pos.x),1,World::Width-2),
                                                      std::clamp(static_cast<int>(pos.z),1,World::Depth-2)))+2.2f;
            ecs_.queueSpawnEnemy(pos,enemyArchetypeFor(request.role),request.stableEnemyId,ActorDomain::Planar);
        }
    }
    ecs_.flushCommands();

    const auto& state=director.state();
    if(state.type==ImperialEnforcementType::RegisterAction) {
        setMessage(std::string("REGISTER ACTION // WAVE ")+std::to_string(state.waveIndex+1)+"/"+
                   std::to_string(state.totalWaves)+" // "+imperialAttentionBandName(state.band),5.0f);
    } else setMessage("IMPERIAL PATROL // CONTACT",3.5f);
    (void)waveBefore;
}

bool Game::nearShip() const {
    if (sphericalSurfaceMode_) {
        const Vec3 feet=sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)];
        return length(feet-sphericalShipTerminalPosition()) < 4.5f;
    }
    return distance2D(ecs_.player().position, shipTerminalPosition(world())) < 4.2f;
}

void Game::switchPlanet(int index) {
    if (index < 0 || index >= PlanetCount) return;
    syncShipCargoFromInventory();
    if (index == currentPlanet_) {
        setMessage("SHIP NAV // already on selected world", 2.0f);
        return;
    }

    // Part 24 Ship: repair-gated phase travel. Authority is ShipTransit (no planar
    // World). Keep the player on the spherical surface path after arrival.
    const PlanetClass destClass = ShipTransit::classForVerticalSliceIndex(index);
    if (!shipTransit_.travelTo(index, destClass)) {
        const auto reject = shipTransit_.lastReject();
        if (reject == ShipTravelReject::HullBelowTakeoff) {
            const auto check = shipTransit_.checklist();
            setMessage(TextFormat("TAKEOFF BLOCKED // hull %.0f < %.0f — repair at ship (E + RepairKit)",
                                  check.hullIntegrity, check.minHullForTakeoff), 4.5f);
        } else if (reject == ShipTravelReject::CargoOverloaded) {
            setMessage("TAKEOFF BLOCKED // cargo over takeoff envelope", 4.0f);
        } else {
            setMessage(std::string("TRAVEL REJECTED // ") + std::string(ShipTransit::rejectName(reject)), 3.5f);
        }
        return;
    }

    currentPlanet_ = shipTransit_.currentPlanetIndex();
    sphericalSurfaceMode_ = true;
    orbitalPreview_ = false;
    renderer_.invalidate();
    planetRenderer_.invalidate();
    orbitWalkerPosition_={};
    sphericalVerticalVelocity_=0.0f;
    sphericalGrounded_=true;
    ecs_.clearEnemies();
    ecs_.setPlayerPosition(spawnPoint(world()));
    sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)] = sphericalArrivalPosition();
    resetPlayerMetersAtShip();
    travelMenu_ = false;
    setMessage(std::string("ARRIVAL // ") + planetName(world().planetClass()) +
               " // transit " + std::string(ShipTransit::phaseName(shipTransit_.phase())), 4.0f);
}

bool Game::tryShipRepairFromInventory() {
    const int kitId = static_cast<int>(IndustryItemId::RepairKit);
    const int available = ecs_.inventoryCount(kitId);
    if (available <= 0) return false;
    if (shipTransit_.hullIntegrity() >= ShipTransit::kHullMax - 1e-5f) return false;
    const int used = shipTransit_.applyRepairKits(available);
    if (used <= 0) return false;
    ecs_.consumeItem(kitId, used);
    setMessage(TextFormat("SHIP REPAIR // used %d RepairKit // hull %.0f / %.0f",
                          used, shipTransit_.hullIntegrity(), ShipTransit::kHullMax), 3.5f);
    return true;
}

void Game::syncShipCargoFromInventory() {
    // Takeoff envelope mass = carried blocks + industry stacks only.
    // Machine/portal kits (1000+) are ship/base equipment, not cargo tonnage —
    // otherwise the starter kit would permanently fail takeoff.
    float mass = 0.0f;
    for (const auto& [id, count] : ecs_.savePlayer().inventory) {
        if (count <= 0) continue;
        const bool isBlock = id > 0 && id < kBlockTypeCount;
        const bool isIndustry = validIndustryItemId(id);
        if (isBlock || isIndustry) mass += static_cast<float>(count);
    }
    shipTransit_.setCargoMassUnits(mass);
}


void Game::resetPlayerMetersAtShip() {
    auto s = ecs_.savePlayer();
    s.health = std::max(75.0f, s.health);
    s.oxygen = 100.0f;
    s.energy = 100.0f;
    s.hunger = std::max(70.0f, s.hunger);
    ecs_.loadPlayer(s);
}

int Game::placedStructureCount(const World& w) const {
    int count = 0;
    for (const auto& [idx, type] : w.edits()) {
        const IVec3 c = w.cellFromFlatIndex(idx);
        if (type != BlockType::Air && type != w.baseline(c.x,c.y,c.z) && blockProperties(type).placeable) ++count;
    }
    return count;
}

float Game::claimFloor() const {
    int totalStructures = 0;
    bool anyClaim = false;
    for (int i = 0; i < PlanetCount; ++i) {
        if (!registryBeacons_.claimed(i)) continue;
        anyClaim = true;
        totalStructures += placedStructureCount(*worlds_[static_cast<std::size_t>(i)]);
        totalStructures += static_cast<int>(planetSurfaces_[static_cast<std::size_t>(i)]->placedMarkerCount());
        totalStructures += static_cast<int>(bases_[static_cast<std::size_t>(i)]->objects().size());
        totalStructures += static_cast<int>(surfaceBases_[static_cast<std::size_t>(i)]->objects().size());
    }
    if (!anyClaim) return 0.0f;
    return std::min(70.0f, 8.0f + 2.0f * std::sqrt(static_cast<float>(totalStructures) / 100.0f));
}

SuspicionState& Game::suspicion() {
    // Alpha vertical slice: one star system keyed by galaxy seed. Future travel
    // to other systems selects a different stable id without sharing standing.
    return suspicionLedger_.system(galaxySeed_);
}

void Game::raiseSuspicion(float amount) {
    suspicion().setClaimFloor(claimFloor());
    suspicion().raise(amount);
}

void Game::setMessage(std::string text, float seconds) {
    message_ = std::move(text);
    messageTimer_ = seconds;
}

void Game::draw() {
    const auto env = world().environment();
    if (orbitalPreview_) {
        drawOrbitalPreview();
        return;
    }
    if (sphericalSurfaceMode_) {
        drawSphericalSurface();
        return;
    }
    const Vec3 cp = cameraPosition();
    const Vec3 cf = cameraForward();
    camera_.position = rv(cp);
    camera_.target = rv(cp + cf);
    camera_.up = {0,1,0};
    camera_.fovy = 75.0f;

    BeginDrawing();
    ClearBackground(rc(env.skyColor));
    drawHorizonSkyGradient(GetScreenWidth(), GetScreenHeight(), env.skyColor, env.horizonColor);
    BeginMode3D(camera_);
    drawWorld3D();
    EndMode3D();
    drawHud();
    if (travelMenu_) drawTravelMenu();
    if (craftingMenu_) drawCraftingMenu();
    if (mapMenu_) drawMapMenu();
    if (inventoryMenu_) drawInventoryMenu();
    EndDrawing();
}

void Game::drawOrbitalPreview() {
    const auto env=world().environment();
    planetRenderer_.setOrbitalShellOnly();
    planetRenderer_.sync(planetSurface());
    const float cp=std::cos(orbitPitch_);
    const Vec3 eye{std::sin(orbitYaw_)*cp*orbitDistance_,std::sin(orbitPitch_)*orbitDistance_,std::cos(orbitYaw_)*cp*orbitDistance_};
    camera_.position=rv(eye);
    camera_.target={0,0,0};
    camera_.up={0,1,0};
    camera_.fovy=58.0f;
    BeginDrawing();
    ClearBackground(Color{static_cast<unsigned char>(env.skyColor.r/4),static_cast<unsigned char>(env.skyColor.g/4),static_cast<unsigned char>(env.skyColor.b/4),255});
    BeginMode3D(camera_);
    planetRenderer_.drawOrbitalShell();
    if (lengthSq(orbitWalkerPosition_)>1.0f) {
        DrawSphere(rv(orbitWalkerPosition_),0.75f,Color{78,239,158,255});
        DrawSphere(rv(orbitWalkerPosition_),0.28f,RAYWHITE);
    }
    EndMode3D();
    drawOrbitalHud();
    EndDrawing();
}

void Game::drawSphericalSurface() {
    const auto env=world().environment();
    planetRenderer_.setStreamingFocus(sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)],7,9);
    planetRenderer_.sync(planetSurface());
    const Vec3 cp=sphericalCameraPosition();
    const Vec3 cf=sphericalCameraForward();
    const Vec3 up=normalize(sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)]);
    camera_.position=rv(cp);
    camera_.target=rv(cp+cf);
    camera_.up=rv(up);
    camera_.fovy=75.0f;

    const int sw = std::max(1, GetScreenWidth());
    const int sh = std::max(1, GetScreenHeight());
    planetRenderer_.setView(makePlanetSurfaceView(
        cp, cf, up, camera_.fovy,
        static_cast<float>(sw) / static_cast<float>(sh),
        planetSurface().referenceRadius(),
        PlanetSurface::ReferenceRadial));

    BeginDrawing();
    ClearBackground(rc(env.skyColor));
    drawHorizonSkyGradient(sw, sh, env.skyColor, env.horizonColor);
    BeginMode3D(camera_);
    planetRenderer_.draw();
    drawSurfaceShipTerminal3D();
    drawSurfaceMachines3D();
    drawSurfacePortals3D();
    for (const auto& e:ecs_.enemies(ActorDomain::PlanetSurface)) {
        const Vec3 up=lengthSq(e.position)>0.001f?normalize(e.position):Vec3{0,1,0};
        const float radius=enemyRenderRadius(e.archetype);
        const Vec3 center=e.position+up*(radius+0.05f);
        DrawSphere(rv(center),radius,enemyRenderColor(e.archetype));
        DrawSphere(rv(center+up*(radius*0.42f)),std::max(0.10f,radius*0.24f),enemyAccentColor(e.archetype));
    }
    const auto hit=surfaceRead().raycast(cp,cf,7.0f,0.08f);
    if (hit.hit) drawSurfaceCellWires(planetSurface(),hit.cell,RAYWHITE);
    EndMode3D();
    drawSphericalHud();
    if (travelMenu_) drawTravelMenu();
    if (craftingMenu_) drawCraftingMenu();
    if (mapMenu_) drawMapMenu();
    if (inventoryMenu_) drawInventoryMenu();
    EndDrawing();
}

void Game::drawSphericalHud() {
    const int sw=GetScreenWidth();
    const int sh=GetScreenHeight();
    const auto p=ecs_.player();
    const Vec3 feet=sphericalPlayerPositions_[static_cast<std::size_t>(currentPlanet_)];
    const auto cell=planetSurface().locate(feet);
    const auto& bp=blockProperties(selectedBlock_);
    const auto ps=surfaceBase().summary();
    const auto navStats=surfaceNavigation().stats();
    const auto& env=world().environment();
    // Local biome chip from deterministic sampleBiome (Present-safe; no EnTT).
    const BiomeId localBiome = sampleBiome(
        planetSurface().seed(), planetSurface().planetClass(),
        lengthSq(feet) > 1e-6f ? normalize(feet) : Vec3{0.0f, 1.0f, 0.0f});
    const char* biomeLabel = env.biome.data();
    if (const BiomeDef* def = biomeDef(localBiome)) biomeLabel = def->name.data();

    drawConceptBrand(16, 16, planetName(world().planetClass()));
    drawConceptMinimap(sw, feet, yaw_, biomeLabel, env.name.data(), env.skyColor, env.horizonColor);
    drawSuspicionCompact(sw, suspicion().value(), claimFloor(), favor_,
                         registryBeacons_.claimed(currentPlanet_),
                         registryBeacons_.beaconIntact(currentPlanet_));
    drawBeaconPresentChip(sw,
                          registryBeacons_.claimed(currentPlanet_),
                          registryBeacons_.beaconIntact(currentPlanet_),
                          registryBeacons_.stableBeaconId(currentPlanet_));
    drawSiegePresentBanner(sw, surfaceSiege().state(), surfaceSiege().active());
    syncShipCargoFromInventory();
    drawShipEnvelopePresentChip(sw, sh, shipTransit_.checklist(), nearShip(), travelMenu_);

    // Engineering telemetry remains debug chrome beneath the CONCEPT brand.
    constexpr int telX = 16;
    constexpr int telY = 96;
    constexpr int telW = 640;
    constexpr int telH = 250;
    DrawRectangle(telX,telY,telW,telH,Color{8,12,14,220});
    DrawRectangleLines(telX,telY,telW,telH,Color{72,165,131,255});
    DrawText(TextFormat("%s  U%02d V%02d R%02d // %s",toString(cell.face).c_str(),cell.u,cell.v,cell.radial,sphericalGrounded_?"GROUNDED":"RADIAL FALL"),28,telY+10,16,Color{114,245,176,255});
    DrawText(TextFormat("sparse macro %d  micro %d  baseline resident %d  jobs %d dirty %d",static_cast<int>(planetSurface().macroEditCount()),static_cast<int>(planetSurface().microOverrideCount()),static_cast<int>(planetSurface().materializedBaselineCellCount()),planetRenderer_.pendingJobs(),planetRenderer_.dirtyChunks()),28,telY+33,14,RAYWHITE);
    DrawText(TextFormat("LOD0 %d / near %d / far %d // %d quads // hostiles %d",planetRenderer_.fullDetailChunks(),planetRenderer_.nearFieldChunks(),planetRenderer_.farFieldChunks(),planetRenderer_.quads(),static_cast<int>(ecs_.enemies(ActorDomain::PlanetSurface).size())),28,telY+55,14,GRAY);
    DrawText(TextFormat("power %.0f gen / %.0f demand / %.0f supplied // battery %.0f/%.0f",ps.generation,ps.demand,ps.supplied,ps.batteryStored,ps.batteryCapacity),28,telY+76,14,Color{190,205,198,255});
    DrawText(TextFormat("AI sense %d think %d path %d/%d commit %d support %d // nav %llu/%llu",surfaceAiTelemetry_.sensed,surfaceAiTelemetry_.thought,surfaceAiTelemetry_.pathResolved,surfaceAiTelemetry_.pathRequested,surfaceAiTelemetry_.committed,surfaceAiTelemetry_.supportActions,static_cast<unsigned long long>(navStats.cacheHits),static_cast<unsigned long long>(navStats.requests)),28,telY+97,13,Color{190,205,198,255});
    const auto cacheStats=planetRenderer_.cpuCacheStats();
    DrawText(TextFormat("CPU chunks %d resident / %d pending // %d KB // evict %llu cancel %llu",cacheStats.resident,cacheStats.pending,static_cast<int>(cacheStats.residentBytes/1024U),static_cast<unsigned long long>(cacheStats.evictions),static_cast<unsigned long long>(cacheStats.cancellations)),28,telY+115,13,GRAY);
    const SurfaceMachineObject* nearestAtmosphere=nullptr;
    float nearestAtmosphereSq=20.0f*20.0f;
    for(const auto& machine:surfaceBase().objects()) {
        if(machine.type!=MachineType::AtmosphereUnit) continue;
        const float d=lengthSq(planetSurface().cellCenterPosition(machine.anchor)-feet);
        if(d<=nearestAtmosphereSq){nearestAtmosphereSq=d;nearestAtmosphere=&machine;}
    }
    DrawText(TextFormat("DEF sensor %d detect %d // turret %d fire %d // shield %.0f/%.0f // auto %d/%d",
                        surfaceDefenseTelemetry_.poweredSensors,surfaceDefenseTelemetry_.hostilesDetected,
                        surfaceDefenseTelemetry_.activeTurrets,surfaceDefenseTelemetry_.fireRequests,
                        surfaceDefenseTelemetry_.shieldCharge,surfaceDefenseTelemetry_.shieldCapacity,
                        surfaceDefenseTelemetry_.automationActionsApplied,surfaceDefenseTelemetry_.automationRulesEvaluated),28,telY+134,13,Color{221,177,135,255});
    DrawText(TextFormat("IND proc %d active %d done %d // net pull %d push %d // stall in/pwr/out %d/%d/%d",
                        surfaceIndustryTelemetry_.processMachines,surfaceIndustryTelemetry_.activeProcesses,
                        surfaceIndustryTelemetry_.completedProcesses,surfaceIndustryTelemetry_.networkPulls,
                        surfaceIndustryTelemetry_.networkPushes,surfaceIndustryTelemetry_.stalledForInput,
                        surfaceIndustryTelemetry_.stalledForPower,surfaceIndustryTelemetry_.stalledForOutput),28,telY+172,13,Color{190,180,130,255});
    DrawText(TextFormat("LOG xfer %d items %d jam %d // EXT ore %d +Susp %.1f // ammo +%d",
                        surfaceIndustryTelemetry_.logisticsTransfers,surfaceIndustryTelemetry_.logisticsItemsMoved,
                        surfaceIndustryTelemetry_.logisticsJams,surfaceIndustryTelemetry_.extractorOreMined,
                        surfaceIndustryTelemetry_.extractorSuspicionGenerated,surfaceIndustryTelemetry_.turretRoundsLoaded),28,telY+191,13,Color{188,168,122,255});
    const auto& siegeState=surfaceSiege().state();
    if(surfaceSiege().active())
        DrawText(TextFormat("EMPIRE %s // %s // wave %d/%d // %.0fs",imperialAttentionBandName(siegeState.band),
                            registerActionPhaseName(siegeState.phase),siegeState.waveIndex+1,siegeState.totalWaves,
                            siegeState.phaseSecondsRemaining),28,telY+209,13,Color{236,150,119,255});
    if(nearestAtmosphere)
        DrawText(TextFormat("ATM P %.0f%% O2 %.0f%% // %s %s",nearestAtmosphere->roomPressure*100.0f,nearestAtmosphere->roomOxygen*100.0f,nearestAtmosphere->roomSealed?"SEALED":"VENTING",nearestAtmosphere->powered?"POWERED":"UNPOWERED"),28,telY+153,13,Color{154,211,205,255});
    else
        DrawText("ATM P -- O2 -- // no atmosphere unit within 20 m",28,telY+153,13,GRAY);

    drawCompassStub(sw, yaw_);
    drawConceptVitals(16, sh - 132, sw, sh, p.health, p.oxygen, p.energy);

    HotbarSlotView hotSlots[9]{};
    fillConceptHotbarSlots(hotSlots, ecs_);
    const int hotSel = conceptHotbarSelectedIndex(selectedBlock_, machineMode_, portalMode_, selectedMachine_);
    const char* modeHint = nullptr;
    if (sculptMode_) modeHint = "V: SUBVOXEL // LMB cuts one persistent 6.25 cm cell";
    else if (portalMode_) modeHint = TextFormat("[B/0] PORTAL %s x%d", surfacePortalName(selectedPortal_), ecs_.inventoryCount(surfacePortalItemId(selectedPortal_)));
    else if (machineMode_ && hotSel < 0) modeHint = TextFormat("MACHINE %s x%d // tool T%d", machineName(selectedMachine_), ecs_.inventoryCount(machineItemId(selectedMachine_)), toolTier_);
    else if (!machineMode_ && !portalMode_) modeHint = TextFormat("BUILD %s // tool T%d // V: %s", bp.name.data(), toolTier_, sculptMode_ ? "SUBVOXEL" : "VOXEL");
    else modeHint = TextFormat("tool T%d // V: %s", toolTier_, sculptMode_ ? "SUBVOXEL" : "VOXEL");
    drawConceptHotbar(sw, sh, hotSel, hotSlots, modeHint);
    drawConceptUtilityCluster(sw, sh, inventoryMenu_, craftingMenu_, mapMenu_, travelMenu_, nearShip());

    DrawLine(sw/2-8,sh/2,sw/2+8,sh/2,RAYWHITE);
    DrawLine(sw/2,sh/2-8,sw/2,sh/2+8,RAYWHITE);
    if (sculptMode_) {
        const auto mh=surfaceRead().raycastMicro(sphericalCameraPosition(),sphericalCameraForward(),7.0f,0.012f);
        if (mh.hit) DrawText(TextFormat("MICRO %s // %s U%d V%d R%d : %d,%d,%d",blockProperties(mh.type).name.data(),toString(mh.micro.cell.face).c_str(),mh.micro.cell.u,mh.micro.cell.v,mh.micro.cell.radial,mh.micro.u,mh.micro.radial,mh.micro.v),sw/2+18,sh/2+16,14,Color{114,245,176,255});
    } else {
        const auto hit=surfaceRead().raycast(sphericalCameraPosition(),sphericalCameraForward(),7.0f,0.06f);
        if (hit.hit) {
            const BlockType type=planetSurface().get(hit.cell);
            DrawText(TextFormat("%s // %s U%d V%d R%d",blockProperties(type).name.data(),toString(hit.cell.face).c_str(),hit.cell.u,hit.cell.v,hit.cell.radial),sw/2+18,sh/2+16,14,RAYWHITE);
            if (miningProgress_>0.0f) {
                DrawRectangle(sw/2-80,sh/2+42,160,8,Color{25,25,25,220});
                DrawRectangle(sw/2-80,sh/2+42,static_cast<int>(160*std::clamp(miningProgress_,0.0f,1.0f)),8,RAYWHITE);
            }
        }
    }

    const Vec3 suitCenter=feet+::elysium::normalize(feet)*0.9f;
    if (nearShip()) DrawText("[T] SHIP NAVIGATION   [E] LIFE SUPPORT",sw/2-190,sh-92,17,Color{102,238,168,255});
    else if (auto* portal=surfaceBase().nearestPortal(planetSurface(),suitCenter,2.6f))
        DrawText(TextFormat("[E] %s %s",portal->open?"CLOSE":"OPEN",surfacePortalName(portal->type)),sw/2-150,sh-92,17,Color{114,225,177,255});
    else if (surfaceBase().nearest(planetSurface(),MachineType::BurnerGenerator,suitCenter,2.8f))
        DrawText("[E] FEED 1 COAL ORE TO SURFACE BURNER",sw/2-205,sh-92,17,Color{235,160,100,255});
    else if (surfaceBase().nearest(planetSurface(),MachineType::Turret,suitCenter,2.8f))
        DrawText("[E] LOAD TURRET // 1 STEEL PLATE = 12 ROUNDS",sw/2-220,sh-92,17,Color{235,160,100,255});
    else if (surfaceBase().nearest(planetSurface(),MachineType::LogicController,suitCenter,3.0f))
        DrawText("[E] CONFIGURE SENSOR -> TURRET AUTOMATION",sw/2-220,sh-92,17,Color{114,225,177,255});
    else if (surfaceBase().nearest(planetSurface(),MachineType::Furnace,suitCenter,2.8f))
        DrawText("[E] LOAD FURNACE  [SHIFT+E] WITHDRAW",sw/2-210,sh-92,17,Color{235,160,100,255});
    else if (surfaceBase().nearest(planetSurface(),MachineType::AlloyCrucible,suitCenter,2.8f))
        DrawText("[E] LOAD ALLOY INPUT  [SHIFT+E] WITHDRAW",sw/2-230,sh-92,17,Color{200,155,235,255});
    else if (surfaceBase().nearest(planetSurface(),MachineType::Refinery,suitCenter,2.8f))
        DrawText("[E] LOAD REFINERY ORE  [SHIFT+E] WITHDRAW",sw/2-235,sh-92,17,Color{120,210,225,255});
    else if (surfaceBase().nearest(planetSurface(),MachineType::Crusher,suitCenter,2.8f))
        DrawText("[E] LOAD CRUSHER  [SHIFT+E] WITHDRAW",sw/2-210,sh-92,17,Color{225,190,126,255});
    else if (surfaceBase().nearest(planetSurface(),MachineType::ChemicalVat,suitCenter,2.8f))
        DrawText("[E] LOAD CHEMICAL VAT  [SHIFT+E] WITHDRAW",sw/2-225,sh-92,17,Color{145,215,153,255});
    else if (surfaceBase().nearest(planetSurface(),MachineType::Fabricator,suitCenter,2.8f))
        DrawText("[E] LOAD FABRICATOR  [SHIFT+E] WITHDRAW",sw/2-220,sh-92,17,Color{158,184,235,255});
    else if (surfaceBase().nearest(planetSurface(),MachineType::Extractor,suitCenter,2.8f))
        DrawText("EXTRACTOR // AUTO-MINES NATURAL ORE  [SHIFT+E] WITHDRAW",sw/2-270,sh-92,17,Color{231,173,125,255});
    else if (surfaceBase().nearest(planetSurface(),MachineType::Conveyor,suitCenter,2.8f) ||
             surfaceBase().nearest(planetSurface(),MachineType::Sorter,suitCenter,2.8f) ||
             surfaceBase().nearest(planetSurface(),MachineType::CargoLoader,suitCenter,2.8f))
        DrawText("[CTRL+E] AUTO-LINK NEAREST ROUTE  [SHIFT+E] WITHDRAW",sw/2-265,sh-92,17,Color{202,190,136,255});
    else if (surfaceBase().nearest(planetSurface(),MachineType::NetworkStorage,suitCenter,2.8f))
        DrawText("[E] DEPOSIT NETWORK STORAGE  [SHIFT+E] WITHDRAW",sw/2-250,sh-92,17,Color{120,225,170,255});
    DrawText("WASD tangent walk  Space jump  LMB mine/chisel  RMB build  F attack  TAB inv  C craft  M chart  [/] machines",20,sh-154,15,Color{222,226,223,230});

    if (messageTimer_>0.0f) {
        const int tw=MeasureText(message_.c_str(),18);
        DrawRectangle(sw/2-tw/2-16,166,tw+32,34,Color{9,13,15,225});
        DrawRectangleLines(sw/2-tw/2-16,166,tw+32,34,Color{108,154,135,230});
        DrawText(message_.c_str(),sw/2-tw/2,175,18,RAYWHITE);
    }
}


void Game::drawWorld3D() {
    renderer_.sync(world());
    renderer_.draw();
    drawShipTerminal3D();
    drawMachines3D();

    for (const auto& e : ecs_.enemies(ActorDomain::Planar)) {
        const float scale=enemyRenderRadius(e.archetype)/0.42f;
        const Vector3 p=rv(e.position+Vec3{0,0.45f*scale,0});
        DrawCube(p,0.82f*scale,0.55f*scale,0.82f*scale,enemyRenderColor(e.archetype));
        DrawCubeWires(p,0.84f*scale,0.57f*scale,0.84f*scale,enemyAccentColor(e.archetype));
        DrawSphere(rv(e.position+Vec3{0,0.62f*scale,0}),0.11f*scale,enemyAccentColor(e.archetype));
    }

    if (sculptMode_) {
        const auto hit = world().raycastMicro(cameraPosition(), cameraForward(), 7.0f);
        if (hit) {
            constexpr float s = 1.0f / static_cast<float>(MicroBrick::Resolution);
            DrawCubeWires(Vector3{(hit->globalMicro.x + 0.5f)*s, (hit->globalMicro.y + 0.5f)*s, (hit->globalMicro.z + 0.5f)*s},
                          s*1.04f, s*1.04f, s*1.04f, Color{100,255,184,255});
        }
    } else {
        const auto hit = world().raycast(cameraPosition(), cameraForward(), 7.0f);
        if (hit) {
            DrawCubeWires(Vector3{hit->cell.x + 0.5f, hit->cell.y + 0.5f, hit->cell.z + 0.5f}, 1.01f,1.01f,1.01f, RAYWHITE);
        }
    }
}

void Game::drawShipTerminal3D() {
    const Vec3 t = shipTerminalPosition(world());
    DrawCube(rv(t + Vec3{0,-0.35f,0}), 5.0f, 0.3f, 4.0f, Color{54,61,66,255});
    DrawCubeWires(rv(t + Vec3{0,-0.35f,0}), 5.0f, 0.3f, 4.0f, Color{121,139,144,255});
    DrawCube(rv(t + Vec3{0,0.75f,0}), 0.8f, 1.6f, 0.8f, Color{16,33,29,255});
    DrawCubeWires(rv(t + Vec3{0,0.75f,0}), 0.84f, 1.64f, 0.84f, Color{52,218,144,255});
    DrawSphere(rv(t + Vec3{0,1.7f,0}), 0.18f, Color{72,236,163,255});
}


void Game::drawSurfaceShipTerminal3D() {
    const Vec3 t=sphericalShipTerminalPosition();
    const Vec3 up=::elysium::normalize(t);
    const auto frame=planetSurface().surfaceFrame(t);
    // Radially aligned proxy assembled from spheres/lines so no global-Y
    // assumption leaks into the spherical gameplay path.
    DrawSphere(rv(t),0.62f,Color{46,54,58,255});
    DrawSphere(rv(t+up*0.72f),0.20f,Color{21,41,35,255});
    DrawSphere(rv(t+up*0.98f),0.10f,Color{72,236,163,255});
    DrawLine3D(rv(t-frame.right*1.35f),rv(t+frame.right*1.35f),Color{105,124,128,255});
    DrawLine3D(rv(t-frame.forward*1.15f),rv(t+frame.forward*1.15f),Color{105,124,128,255});
}

void Game::drawSurfaceMachines3D() {
    for (const auto& o:surfaceBase().objects()) {
        const Vec3 center=planetSurface().cellCenterPosition(o.anchor);
        const Vec3 up=::elysium::normalize(center);
        Color body{76,82,84,255};
        Color accent=o.powered?Color{69,225,147,255}:Color{126,89,74,255};
        switch(o.type) {
            case MachineType::BurnerGenerator: body=Color{89,72,61,255}; break;
            case MachineType::BatteryBank: body=Color{69,78,91,255}; break;
            case MachineType::AtmosphereUnit: body=Color{58,89,87,255}; break;
            case MachineType::StorageCrate: body=Color{95,83,63,255}; break;
            case MachineType::AirlockController: body=Color{54,100,104,255}; break;
            case MachineType::SensorMast: body=Color{60,88,104,255}; break;
            case MachineType::Turret: body=Color{104,66,58,255}; break;
            case MachineType::ShieldPylon: body=Color{61,78,118,255}; break;
            case MachineType::LogicController: body=Color{73,96,78,255}; break;
            case MachineType::Furnace: body=Color{112,76,56,255}; break;
            case MachineType::AlloyCrucible: body=Color{103,72,90,255}; break;
            case MachineType::Refinery: body=Color{74,96,103,255}; break;
            case MachineType::NetworkStorage: body=Color{67,103,84,255}; break;
            case MachineType::Conveyor: body=Color{91,91,76,255}; break;
            case MachineType::Sorter: body=Color{78,93,108,255}; break;
            case MachineType::CargoLoader: body=Color{95,84,104,255}; break;
            case MachineType::Crusher: body=Color{105,91,66,255}; break;
            case MachineType::ChemicalVat: body=Color{77,112,82,255}; break;
            case MachineType::Fabricator: body=Color{74,88,117,255}; break;
            case MachineType::Extractor: body=Color{114,82,63,255}; break;
        }
        // Sphere-based proxy avoids assuming world Y is up; the radial marker
        // makes the machine's local orientation readable on every cube face.
        DrawSphere(rv(center),0.38f,body);
        DrawSphere(rv(center+up*0.38f),0.08f,accent);
        if(o.type==MachineType::BurnerGenerator && o.fuelSeconds>0.0f)
            DrawSphere(rv(center+up*0.52f),0.07f,Color{235,133,73,255});
    }
}

void Game::drawSurfacePortals3D() {
    for(const auto& portal:surfaceBase().portals()) {
        const Vec3 center=planetSurface().cellCenterPosition(portal.anchor);
        const Vec3 up=normalize(center);
        const auto frame=planetSurface().surfaceFrame(center);
        const Color edge=portal.type==SurfacePortalType::Airlock?Color{78,214,164,255}:Color{166,174,170,255};
        const Color state=portal.open?Color{230,143,88,255}:Color{71,223,151,255};
        if(!portal.open) DrawSphere(rv(center),0.30f,portal.type==SurfacePortalType::Airlock?Color{42,70,67,255}:Color{75,82,84,255});
        DrawLine3D(rv(center-frame.right*0.42f),rv(center+frame.right*0.42f),edge);
        DrawLine3D(rv(center-frame.forward*0.42f),rv(center+frame.forward*0.42f),edge);
        DrawSphere(rv(center+up*0.42f),0.07f,state);
    }
}

void Game::drawMachines3D() {
    for(const auto& o:base().objects()) {
        const Vector3 p{static_cast<float>(o.anchor.x)+0.5f,static_cast<float>(o.anchor.y)+0.5f,static_cast<float>(o.anchor.z)+0.5f};
        Color body{76,82,84,255};
        Color accent=o.powered?Color{69,225,147,255}:Color{126,89,74,255};
        switch(o.type) {
            case MachineType::BurnerGenerator: body=Color{89,72,61,255}; break;
            case MachineType::BatteryBank: body=Color{69,78,91,255}; break;
            case MachineType::AtmosphereUnit: body=Color{58,89,87,255}; break;
            case MachineType::StorageCrate: body=Color{95,83,63,255}; break;
            case MachineType::AirlockController: body=Color{54,100,104,255}; break;
            case MachineType::SensorMast: body=Color{60,88,104,255}; break;
            case MachineType::Turret: body=Color{104,66,58,255}; break;
            case MachineType::ShieldPylon: body=Color{61,78,118,255}; break;
            case MachineType::LogicController: body=Color{73,96,78,255}; break;
            case MachineType::Furnace: body=Color{112,76,56,255}; break;
            case MachineType::AlloyCrucible: body=Color{103,72,90,255}; break;
            case MachineType::Refinery: body=Color{74,96,103,255}; break;
            case MachineType::NetworkStorage: body=Color{67,103,84,255}; break;
            case MachineType::Conveyor: body=Color{91,91,76,255}; break;
            case MachineType::Sorter: body=Color{78,93,108,255}; break;
            case MachineType::CargoLoader: body=Color{95,84,104,255}; break;
            case MachineType::Crusher: body=Color{105,91,66,255}; break;
            case MachineType::ChemicalVat: body=Color{77,112,82,255}; break;
            case MachineType::Fabricator: body=Color{74,88,117,255}; break;
            case MachineType::Extractor: body=Color{114,82,63,255}; break;
        }
        DrawCube(p,0.72f,0.72f,0.72f,body);
        DrawCubeWires(p,0.75f,0.75f,0.75f,accent);
        if(o.type==MachineType::BurnerGenerator && o.fuelSeconds>0.0f)
            DrawSphere(Vector3{p.x,p.y+0.48f,p.z},0.08f,Color{235,133,73,255});
    }
}

void Game::drawOrbitalHud() {
    const int sw=GetScreenWidth();
    DrawRectangle(18,18,620,140,Color{5,9,12,225});
    DrawRectangleLines(18,18,620,140,Color{72,165,131,255});
    DrawText("CUBE-SPHERE ORBITAL DEBUG",34,30,24,RAYWHITE);
    DrawText(TextFormat("6 faces x %d x %d columns // %d radial layers",PlanetSurface::FaceResolution,PlanetSurface::FaceResolution,PlanetSurface::RadialLayers),34,62,16,Color{183,208,196,255});
    DrawText(TextFormat("ORBIT SHELL climate %d quads / clouds %d // voxel LOD0 not requested",planetRenderer_.orbitalClimateQuads(),planetRenderer_.orbitalCloudQuads()),34,86,16,RAYWHITE);
    const auto probe=planetSurface().locate(orbitWalkerPosition_);
    DrawText(TextFormat("probe %s U%02d V%02d R%02d // WASD walks tangent frame",toString(probe.face).c_str(),probe.u,probe.v,probe.radial),34,108,14,Color{114,245,176,255});
    DrawText("Mouse orbit  Wheel zoom  O/Esc return // P enters playable spherical surface",34,128,14,GRAY);
    DrawText(TextFormat("%s // %s",planetName(world().planetClass()), world().environment().biome.data()),sw-420,28,16,Color{114,245,176,255});
}

void Game::drawHud() {
    const int sw = GetScreenWidth();
    const int sh = GetScreenHeight();
    const auto p = ecs_.player();
    const auto& env = world().environment();

    drawConceptBrand(16, 16, planetName(world().planetClass()));
    drawCompassStub(sw, yaw_);
    drawConceptVitals(16, sh - 132, sw, sh, p.health, p.oxygen, p.energy);

    drawConceptMinimap(sw, p.position, yaw_, env.biome.data(), env.name.data(), env.skyColor, env.horizonColor);
    drawSuspicionCompact(sw, suspicion().value(), claimFloor(), favor_,
                         registryBeacons_.claimed(currentPlanet_),
                         registryBeacons_.beaconIntact(currentPlanet_));
    drawBeaconPresentChip(sw,
                          registryBeacons_.claimed(currentPlanet_),
                          registryBeacons_.beaconIntact(currentPlanet_),
                          registryBeacons_.stableBeaconId(currentPlanet_));
    drawSiegePresentBanner(sw, surfaceSiege().state(), surfaceSiege().active());
    syncShipCargoFromInventory();
    drawShipEnvelopePresentChip(sw, sh, shipTransit_.checklist(), nearShip(), travelMenu_);

    HotbarSlotView hotSlots[9]{};
    fillConceptHotbarSlots(hotSlots, ecs_);
    const int hotSel = conceptHotbarSelectedIndex(selectedBlock_, machineMode_, portalMode_, selectedMachine_);
    const char* modeHint = nullptr;
    if (sculptMode_) modeHint = TextFormat("V: SUBVOXEL // tool T%d", toolTier_);
    else if (portalMode_) modeHint = TextFormat("[B/0] PORTAL %s x%d", surfacePortalName(selectedPortal_), ecs_.inventoryCount(surfacePortalItemId(selectedPortal_)));
    else if (machineMode_ && hotSel < 0) modeHint = TextFormat("MACHINE %s x%d // tool T%d", machineName(selectedMachine_), ecs_.inventoryCount(machineItemId(selectedMachine_)), toolTier_);
    else modeHint = TextFormat("tool T%d // V: %s", toolTier_, sculptMode_ ? "SUBVOXEL" : "VOXEL");
    drawConceptHotbar(sw, sh, hotSel, hotSlots, modeHint);
    drawConceptUtilityCluster(sw, sh, inventoryMenu_, craftingMenu_, mapMenu_, travelMenu_, nearShip());

    DrawLine(sw/2 - 8, sh/2, sw/2 + 8, sh/2, RAYWHITE);
    DrawLine(sw/2, sh/2 - 8, sw/2, sh/2 + 8, RAYWHITE);

    if (sculptMode_) {
        const auto hit = world().raycastMicro(cameraPosition(), cameraForward(), 7.0f);
        if (hit) DrawText(TextFormat("MICRO // %s  %d,%d,%d", blockProperties(hit->type).name.data(), hit->micro.x, hit->micro.y, hit->micro.z), sw/2 + 18, sh/2 + 16, 14, Color{114,245,176,255});
    } else {
        const auto hit = world().raycast(cameraPosition(), cameraForward(), 7.0f);
        if (hit) {
            DrawText(TextFormat("%s  T%d", blockProperties(hit->type).name.data(), blockProperties(hit->type).harvestTier), sw/2 + 18, sh/2 + 16, 14, RAYWHITE);
            if (miningProgress_ > 0.0f) {
                DrawRectangle(sw/2 - 80, sh/2 + 42, 160, 8, Color{25,25,25,220});
                DrawRectangle(sw/2 - 80, sh/2 + 42, static_cast<int>(160 * std::clamp(miningProgress_,0.0f,1.0f)), 8, RAYWHITE);
            }
        }
    }

    DrawText("LMB mine/chisel  RMB build/machine  1-9 hotbar  V micro  TAB inv  F attack  C craft  M chart  T travel  P spherical  O orbit", 20, sh - 154, 15, Color{222,226,223,230});
    DrawText(TextFormat("Greedy quads %d (macro %d / micro %d)  tris %d  mesh jobs %d/%zu  dirty %d rebuilt %d", renderer_.meshQuads(), renderer_.macroQuads(), renderer_.microQuads(), renderer_.triangleCount(), renderer_.pendingJobs(), jobs_.workerCount(),renderer_.dirtyChunks(),renderer_.rebuiltChunksLastSync()),
             20, 96, 14, Color{220,225,222,210});
    const auto ps=base().summary();
    DrawText(TextFormat("Base power %.0f gen / %.0f demand / %.0f supplied  battery %.0f/%.0f  machines %d",ps.generation,ps.demand,ps.supplied,ps.batteryStored,ps.batteryCapacity,static_cast<int>(base().objects().size())),
             20, 114, 14, Color{220,225,222,210});

    if (nearShip()) DrawText("[T] NAVIGATION   [E] SHIP LIFE SUPPORT", sw/2 - 175, sh - 92, 17, Color{102,238,168,255});
    else if (base().nearest(MachineType::BurnerGenerator,ecs_.player().position,2.8f)) DrawText("[E] FEED 1 COAL ORE TO BURNER GENERATOR",sw/2-205,sh-92,17,Color{235,160,100,255});

    if (messageTimer_ > 0.0f) {
        const int tw = MeasureText(message_.c_str(), 18);
        DrawRectangle(sw/2 - tw/2 - 16, 122, tw + 32, 34, Color{9,13,15,225});
        DrawRectangleLines(sw/2 - tw/2 - 16, 122, tw + 32, 34, Color{108,154,135,230});
        DrawText(message_.c_str(), sw/2 - tw/2, 131, 18, RAYWHITE);
    }
}

void Game::drawInventoryMenu() {
    const int sw = GetScreenWidth();
    const int sh = GetScreenHeight();
    // Dim world slightly behind modal (not full-screen opaque craft overlay).
    DrawRectangle(0, 0, sw, sh, Color{5, 8, 12, 90});

    const PlayerPersistentState snap = ecs_.savePlayer(); // read-only listing snapshot
    InvSlotView slots[kInvGridN]{};
    buildInvTabSlots(slots, static_cast<InvTab>(inventoryTab_), ecs_, toolTier_, &snap.inventory);
    inventorySelected_ = std::clamp(inventorySelected_, 0, kInvGridN - 1);

    const int capN = inventoryOccupiedSlots(snap.inventory);
    const InvLayout layout = makeInvLayout(sw, sh);
    drawInventoryModalChrome(layout, inventoryTab_, inventorySelected_, sculptMode_, slots, capN, kInvCapacityMax);

    DrawText("TAB / Esc close   1-3 tabs   arrows select   [ ] VOXEL/SUBVOXEL", layout.panelX + 18, layout.panelY + layout.panelH + 8, 13, Color{180, 200, 210, 200});
}

void Game::drawTravelMenu() {
    const int sw = GetScreenWidth(), sh = GetScreenHeight();
    syncShipCargoFromInventory();
    const auto check = shipTransit_.checklist();
    const int px = sw / 2 - 330, py = sh / 2 - 205, pw = 660, ph = 410;
    drawGlassModalFrame(px, py, pw, ph, "SHIP NAVIGATION");
    DrawText("THREE-PLANET TEST SYSTEM", px + 22, py + 48, 14, kCyanSoft);

    const Rectangle row1{static_cast<float>(px + 28), static_cast<float>(py + 88), 604.0f, 36.0f};
    const Rectangle row2{static_cast<float>(px + 28), static_cast<float>(py + 132), 604.0f, 36.0f};
    const Rectangle row3{static_cast<float>(px + 28), static_cast<float>(py + 176), 604.0f, 36.0f};
    DrawRectangleRounded(row1, 0.2f, 6, Color{22, 32, 44, 160});
    DrawRectangleRounded(row2, 0.2f, 6, Color{22, 32, 44, 160});
    DrawRectangleRounded(row3, 0.2f, 6, Color{22, 32, 44, 160});
    DrawText("[1] TEMPERATE  — breathable homestead / broad resources", px + 40, py + 96, 18, Color{200, 220, 230, 255});
    DrawText("[2] BARREN     — vacuum / exposed geology", px + 40, py + 140, 18, Color{205, 196, 190, 255});
    DrawText("[3] SCORCHED   — thermal pressure / basalt + magma", px + 40, py + 184, 18, Color{226, 164, 135, 255});

    const Rectangle envR{static_cast<float>(px + 28), static_cast<float>(py + 240), 604.0f, 56.0f};
    DrawRectangleRounded(envR, 0.18f, 6, Color{22, 32, 44, 170});
    DrawRectangleRoundedLines(envR, 0.18f, 6, check.ready ? kCyanAccent : Color{226, 164, 135, 200});
    DrawText(TextFormat("TAKEOFF ENVELOPE  hull %.0f / min %.0f   cargo %.0f / max %.0f",
                        check.hullIntegrity, check.minHullForTakeoff,
                        check.cargoMassUnits, check.maxCargoForTakeoff),
             px + 40, py + 250, 15, RAYWHITE);
    DrawText(check.ready ? "READY" : "BLOCKED",
             px + 40, py + 272, 16, check.ready ? kCyanAccent : Color{226, 164, 135, 255});

    DrawText("Repair kits at the ship (E). Transit: surface → atmo → orbit → transit.",
             px + 28, py + 316, 14, Color{160, 180, 190, 200});
    DrawText("T / Esc / X closes", px + 28, py + 370, 14, Color{160, 180, 190, 180});
}

void Game::drawCraftingMenu() {
    const int sw = GetScreenWidth(), sh = GetScreenHeight();
    const int px = sw / 2 - 410, py = sh / 2 - 355, pw = 820, ph = 710;
    drawGlassModalFrame(px, py, pw, ph, "FIELD FABRICATION");
    DrawText("EARLY INFRASTRUCTURE", px + 22, py + 48, 14, kCyanSoft);
    DrawText(TextFormat("Tool // %s (T%d)", EarlyMetallurgy::headLabel(toolTier_).data(), toolTier_),
             px + 22, py + 72, 14, kCyanSoft);

    const int lx = px + 28;
    int ly = py + 100;
    auto recipe = [&](const char* line, Color col) {
        DrawText(line, lx, ly, 16, col);
        ly += 26;
    };
    recipe("[1] Stone head (4 Stone) then Bronze (4 Bronze Ingot / 3Cu+1Sn)", RAYWHITE);
    recipe("[2] Steel mining head  = 2 Steel Ingot (or 4 Iron + 2 Coal)", RAYWHITE);
    recipe("[3] Steel Plate        = 2 Iron + 1 Coal", RAYWHITE);
    recipe("[4] Registry Beacon    = 4 Steel Plate + 2 Copper", RAYWHITE);
    ly += 6;
    recipe("[5] Burner Generator   = 2 Steel Plate + 2 Copper", Color{215, 191, 167, 255});
    recipe("[6] Battery Bank       = 2 Steel Plate + 3 Copper", Color{215, 191, 167, 255});
    recipe("[7] Atmosphere Unit    = 3 Steel Plate + 2 Copper", Color{215, 191, 167, 255});
    recipe("[8] Storage Crate      = 1 Steel Plate + 4 Planks", Color{215, 191, 167, 255});
    recipe("[9] Door kit           = 1 Steel Plate + 2 Planks", Color{196, 206, 202, 255});
    recipe("[0] Airlock kit        = 3 Steel Plate + 1 Copper", Color{114, 225, 177, 255});
    recipe("[K] Airlock Controller = 2 Steel Plate + 2 Copper", Color{114, 225, 177, 255});
    recipe("[J] Sensor Mast        = 2 Steel Plate + 2 Copper", Color{221, 177, 135, 255});
    recipe("[U] Turret             = 4 Steel Plate + 2 Copper", Color{221, 177, 135, 255});
    recipe("[Y] Shield Pylon       = 6 Steel Plate + 3 Copper", Color{152, 180, 231, 255});
    recipe("[L] Logic Controller   = 2 Steel Plate + 3 Copper", Color{141, 211, 168, 255});
    ly += 4;
    recipe("[G] Furnace  [H] Alloy Crucible  [I] Refinery  [N] Network Storage", Color{226, 183, 143, 255});
    recipe("[Q] x2 Conveyor  [R] Sorter  [X] Cargo Loader  [Z] Crusher", Color{208, 194, 142, 255});
    recipe("[F1] Chemical Vat  [F2] Fabricator  [F3] Extractor", Color{159, 205, 183, 255});
    ly += 8;
    DrawText(TextFormat("ORE // Coal %d  Copper %d  Tin %d  Iron %d",
                        ecs_.inventoryCount(static_cast<int>(BlockType::CoalOre)),
                        ecs_.inventoryCount(static_cast<int>(BlockType::CopperOre)),
                        ecs_.inventoryCount(static_cast<int>(BlockType::TinOre)),
                        ecs_.inventoryCount(static_cast<int>(BlockType::IronOre))),
             lx, ly, 14, kCyanSoft);
    DrawText("C / Esc / X closes   (craft keys unchanged)", lx, py + ph - 36, 14, Color{160, 180, 190, 180});
}

void Game::drawMapMenu() {
    const int sw = GetScreenWidth(), sh = GetScreenHeight();
    const int px = sw / 2 - 390, py = sh / 2 - 245, pw = 780, ph = 490;
    drawGlassModalFrame(px, py, pw, ph, "SYSTEM FILE");
    DrawText("LOCAL CHART", px + 22, py + 48, 14, kCyanSoft);

    DrawText(TextFormat("System Suspicion %.1f / 100    Claim floor %.1f",
                        suspicion().value(), claimFloor()),
             px + 28, py + 84, 18, RAYWHITE);

    for (int i = 0; i < PlanetCount; ++i) {
        const auto& w = *worlds_[static_cast<std::size_t>(i)];
        const int rowY = py + 130 + i * 52;
        const Rectangle row{static_cast<float>(px + 28), static_cast<float>(rowY), 724.0f, 44.0f};
        DrawRectangleRounded(row, 0.18f, 6, Color{22, 32, 44, 160});
        if (i == currentPlanet_) {
            DrawRectangleRoundedLinesEx(row, 0.18f, 6, 2.0f, kCyanAccent);
        } else {
            DrawRectangleRoundedLines(row, 0.18f, 6, kGlassEdge);
        }
        const Color rowCol = i == currentPlanet_ ? kCyanAccent : RAYWHITE;
        DrawText(TextFormat("%d  %-10s  |  patch %d  sphere %d+%d micro  |  machines %d/%d  |  claim %s",
                            i + 1, w.environment().name.data(),
                            static_cast<int>(w.edits().size()),
                            static_cast<int>(planetSurfaces_[static_cast<std::size_t>(i)]->macroEditCount()),
                            static_cast<int>(planetSurfaces_[static_cast<std::size_t>(i)]->microOverrideCount()),
                            static_cast<int>(bases_[static_cast<std::size_t>(i)]->objects().size()),
                            static_cast<int>(surfaceBases_[static_cast<std::size_t>(i)]->objects().size()
                                             + surfaceBases_[static_cast<std::size_t>(i)]->portals().size()),
                            registryBeacons_.claimed(i) ? "FILED" : "-"),
                 px + 40, rowY + 12, 16, rowCol);
    }

    DrawText("Mining creates territorial attention. Claims preserve a Suspicion floor.",
             px + 28, py + ph - 78, 15, Color{160, 180, 190, 200});
    DrawText("M / Esc / X closes", px + 28, py + ph - 44, 14, Color{160, 180, 190, 180});
}

void Game::saveGame() {
    if (!IsWindowReady()) return;
    std::ostringstream out;

    const auto previousManifestForCleanup=readCheckedAtomicGeneration(kSavePath,true);
    const auto previousSurfaceRefs=previousManifestForCleanup.loaded
        ? extractSurfaceManifestChunkRefs(previousManifestForCleanup.data)
        : std::vector<SurfaceManifestChunkRef>{};
    std::vector<SurfaceManifestChunkRef> currentSurfaceRefs;
    const std::uint64_t nextSaveGeneration=saveGeneration_+1;

    const auto p = ecs_.savePlayer();
    // Save schema 8 binds every spherical sidecar to the same whole-save
    // generation as the global manifest. If a crash occurs before manifest
    // publication, the old manifest selects the retained previous chunk
    // generations instead of accidentally mixing campaign states.
    out << "ELYSIUM_SAVE 8 1\n";
    out << "save_generation " << nextSaveGeneration << "\n";
    out << "seed " << galaxySeed_ << "\n";
    out << "planet " << currentPlanet_ << "\n";
    out << std::setprecision(9);
    out << "state " << suspicion().value() << ' ' << favor_ << ' ' << toolTier_ << ' '
        << (registryBeacons_.claimed(0) ? 1 : 0) << ' '
        << (registryBeacons_.claimed(1) ? 1 : 0) << ' '
        << (registryBeacons_.claimed(2) ? 1 : 0) << "\n";
    out << "player " << p.position.x << ' ' << p.position.y << ' ' << p.position.z << ' '
        << p.health << ' ' << p.oxygen << ' ' << p.energy << ' ' << p.hunger << "\n";
    out << "inventory " << p.inventory.size() << "\n";
    for (const auto& [id,count] : p.inventory) out << id << ' ' << count << "\n";

    for (int i = 0; i < PlanetCount; ++i) {
        const auto& w = *worlds_[static_cast<std::size_t>(i)];
        const auto& edits = w.edits();
        out << "world " << i << ' ' << edits.size() << "\n";
        std::vector<std::pair<int, BlockType>> ordered(edits.begin(), edits.end());
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
        for (const auto& [idx,type] : ordered) out << idx << ' ' << static_cast<int>(type) << "\n";
    }

    for (int i = 0; i < PlanetCount; ++i) {
        const auto& bricks = worlds_[static_cast<std::size_t>(i)]->microBricks();
        std::vector<int> cellIndices;
        cellIndices.reserve(bricks.size());
        for (const auto& [idx, brick] : bricks) {
            if (brick.overrideCount() > 0) cellIndices.push_back(idx);
        }
        std::sort(cellIndices.begin(), cellIndices.end());
        out << "micro_world " << i << ' ' << cellIndices.size() << "\n";
        for (const int idx : cellIndices) {
            const auto it = bricks.find(idx);
            if (it == bricks.end()) continue;
            const auto overrides = it->second.overrides();
            out << "cell " << idx << ' ' << overrides.size() << "\n";
            for (const auto& [microIndex,type] : overrides)
                out << microIndex << ' ' << static_cast<int>(type) << "\n";
        }
    }

    for (int i=0;i<PlanetCount;++i) {
        const auto& objects=bases_[static_cast<std::size_t>(i)]->objects();
        out << "objects " << i << ' ' << objects.size() << "\n";
        for(const auto& o:objects)
            out << "obj " << o.stableId << ' ' << static_cast<int>(o.type) << ' ' << o.anchor.x << ' ' << o.anchor.y << ' ' << o.anchor.z << ' ' << o.enabled << ' ' << o.fuelSeconds << ' ' << o.storedEnergy << "\n";
    }

    for (int i=0;i<PlanetCount;++i) {
        const Vec3 sp=sphericalPlayerPositions_[static_cast<std::size_t>(i)];
        out << "surface_player " << i << ' ' << sp.x << ' ' << sp.y << ' ' << sp.z << "\n";

        const auto& surface=*planetSurfaces_[static_cast<std::size_t>(i)];
        const auto& infrastructure=*surfaceBases_[static_cast<std::size_t>(i)];
        std::vector<PlanetChunkAddress> addresses;
        addresses.reserve(surface.journals().size()+infrastructure.objects().size());
        auto addAddress=[&](const PlanetChunkAddress& a) {
            const auto key=stableChunkKey(a);
            if(std::none_of(addresses.begin(),addresses.end(),[&](const auto& e){return stableChunkKey(e)==key;})) addresses.push_back(a);
        };
        for(const auto& [_,journal]:surface.journals()) if(!journal.empty()) addAddress(journal.address);
        for(const auto& object:infrastructure.objects()) addAddress(surface.chunkOf(object.anchor));
        for(const auto& portal:infrastructure.portals()) addAddress(surface.chunkOf(portal.anchor));
        std::sort(addresses.begin(),addresses.end(),[](const auto& a,const auto& b){return stableChunkKey(a)<stableChunkKey(b);});

        SurfaceChunkStore store(kSurfaceChunkSaveRoot,i,surface.seed(),kSurfaceGeneratorVersion,kSurfaceGeneratorFingerprint);
        for(const auto& address:addresses) {
            std::string error;
            const auto record=makeSurfaceChunkRecord(i,surface,infrastructure,address,nextSaveGeneration);
            if(!store.save(record,&error)) {
                setMessage("SAVE FAILED // surface chunk transaction: "+error,5.0f);
                return;
            }
        }

        out << "surface_manifest " << i << ' ' << addresses.size() << "\n";
        for(const auto& address:addresses) {
            out << "surface_chunk " << static_cast<int>(address.face) << ' ' << address.u << ' ' << address.v << ' ' << address.radial << "\n";
            currentSurfaceRefs.push_back({i,address});
        }
    }

    // Append-only Empire siege embed (schema 8, no v9). SAVE lane owns chunks;
    // this section is siege-specific ownership before END.
    {
        const SurfaceSiegeDirector* directors[PlanetCount]{};
        for (int i = 0; i < PlanetCount; ++i)
            directors[i] = surfaceSieges_[static_cast<std::size_t>(i)].get();
        appendCampaignSiegeEmbed(out, directors, PlanetCount);
    }

    out << "END\n";
    if (!out.good()) { setMessage("SAVE FAILED // could not serialize global manifest",5.0f); return; }
    std::string manifestError;
    if (!writeCheckedAtomicGeneration(kSavePath,out.str(),&manifestError)) {
        setMessage("SAVE FAILED // manifest transaction: "+manifestError,5.0f);
        return;
    }
    saveGeneration_=nextSaveGeneration;

    std::string pruneError;
    if(!pruneSurfaceChunkStore(kSurfaceChunkSaveRoot,currentSurfaceRefs,previousSurfaceRefs,&pruneError)) {
        setMessage("SAVE COMMITTED // sidecar cleanup deferred: "+pruneError,5.0f);
        return;
    }
    setMessage("Save generation committed across manifest + spherical chunks.");
}

bool Game::loadGame() {
    const auto manifest=readCheckedAtomicGeneration(kSavePath,true);
    if (!manifest.loaded) return false;
    std::istringstream in(manifest.data);

    std::string tag;
    int saveVersion{}, generatorVersion{};
    if (!(in >> tag >> saveVersion >> generatorVersion) || tag != "ELYSIUM_SAVE") return false;
    if ((saveVersion < 1 || saveVersion > 8) || generatorVersion != 1) return false;

    std::uint64_t fileSaveGeneration{};
    if(saveVersion>=8) {
        if(!(in>>tag>>fileSaveGeneration) || tag!="save_generation") return false;
        if(fileSaveGeneration==0) return false;
    }

    std::uint64_t seed{};
    int planet{};
    float suspicion{}, favor{};
    int tool{};
    int c0{},c1{},c2{};
    PlayerPersistentState p{};

    if (!(in >> tag >> seed) || tag != "seed") return false;
    if (!(in >> tag >> planet) || tag != "planet") return false;
    if (!(in >> tag >> suspicion >> favor >> tool >> c0 >> c1 >> c2) || tag != "state") return false;
    if (!(in >> tag >> p.position.x >> p.position.y >> p.position.z >> p.health >> p.oxygen >> p.energy >> p.hunger) || tag != "player") return false;

    std::size_t invCount{};
    if (!(in >> tag >> invCount) || tag != "inventory") return false;
    p.inventory.clear();
    for (std::size_t i = 0; i < invCount; ++i) {
        int id{}, count{};
        if (!(in >> id >> count)) return false;
        const bool legacyItem=id>=0 && id<3000;
        const bool industryItem=id>=static_cast<int>(IndustryItemId::CopperIngot) && id<=static_cast<int>(IndustryItemId::CompositePanel);
        if ((legacyItem || industryItem) && count > 0) p.inventory.emplace_back(id,count);
    }

    galaxySeed_ = seed;
    initWorlds();
    for (int wi = 0; wi < PlanetCount; ++wi) {
        int fileWorld{};
        std::size_t editCount{};
        if (!(in >> tag >> fileWorld >> editCount) || tag != "world" || fileWorld < 0 || fileWorld >= PlanetCount) return false;
        for (std::size_t e = 0; e < editCount; ++e) {
            int idx{}, type{};
            if (!(in >> idx >> type)) return false;
            if (type >= 0 && type < kBlockTypeCount)
                worlds_[static_cast<std::size_t>(fileWorld)]->applySavedEdit(idx, static_cast<BlockType>(type));
        }
    }

    if (saveVersion >= 2) {
        for (int wi = 0; wi < PlanetCount; ++wi) {
            int fileWorld{};
            std::size_t brickCount{};
            if (!(in >> tag >> fileWorld >> brickCount) || tag != "micro_world" || fileWorld < 0 || fileWorld >= PlanetCount) return false;
            for (std::size_t b = 0; b < brickCount; ++b) {
                int flatCell{};
                std::size_t overrideCount{};
                if (!(in >> tag >> flatCell >> overrideCount) || tag != "cell") return false;
                for (std::size_t m = 0; m < overrideCount; ++m) {
                    int microIndex{}, type{};
                    if (!(in >> microIndex >> type)) return false;
                    if (microIndex >= 0 && microIndex < MicroBrick::CellCount && type >= 0 && type < kBlockTypeCount)
                        worlds_[static_cast<std::size_t>(fileWorld)]->applySavedMicroEdit(flatCell,microIndex,static_cast<BlockType>(type));
                }
            }
        }
    }

    if (saveVersion >= 3) {
        for(int wi=0;wi<PlanetCount;++wi) {
            int fileWorld{}; std::size_t objectCount{};
            if(!(in>>tag>>fileWorld>>objectCount) || tag!="objects" || fileWorld<0 || fileWorld>=PlanetCount) return false;
            for(std::size_t oi=0;oi<objectCount;++oi) {
                MachineObject o{}; int type{}; int enabled{};
                if(!(in>>tag>>o.stableId>>type>>o.anchor.x>>o.anchor.y>>o.anchor.z>>enabled>>o.fuelSeconds>>o.storedEnergy) || tag!="obj") return false;
                if(type<0 || type>static_cast<int>(MachineType::Extractor)) return false;
                o.type=static_cast<MachineType>(type); o.enabled=enabled!=0;
                if(!bases_[static_cast<std::size_t>(fileWorld)]->restore(o)) return false;
            }
            bases_[static_cast<std::size_t>(fileWorld)]->update(0.0f);
        }
    }

    if (saveVersion >= 4 && saveVersion <= 5) {
        for (int wi=0;wi<PlanetCount;++wi) {
            int fileWorld{};
            Vec3 pos{};
            if (!(in>>tag>>fileWorld>>pos.x>>pos.y>>pos.z) || tag!="surface_player" || fileWorld<0 || fileWorld>=PlanetCount) return false;
            if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) || lengthSq(pos)<1.0f) return false;
            sphericalPlayerPositions_[static_cast<std::size_t>(fileWorld)]=pos;

            std::size_t editCount{};
            if (!(in>>tag>>fileWorld>>editCount) || tag!="surface_world" || fileWorld<0 || fileWorld>=PlanetCount) return false;
            for (std::size_t e=0;e<editCount;++e) {
                int idx{},type{};
                if (!(in>>idx>>type)) return false;
                if (type>=0 && type<kBlockTypeCount)
                    planetSurfaces_[static_cast<std::size_t>(fileWorld)]->applySavedEdit(idx,static_cast<BlockType>(type));
            }

            std::size_t placedCount{};
            if (!(in>>tag>>fileWorld>>placedCount) || tag!="surface_placed" || fileWorld<0 || fileWorld>=PlanetCount) return false;
            for (std::size_t m=0;m<placedCount;++m) {
                int idx{};
                if (!(in>>idx)) return false;
                planetSurfaces_[static_cast<std::size_t>(fileWorld)]->applySavedPlacedMarker(idx);
            }
        }
    }

    if (saveVersion == 5) {
        for (int wi=0;wi<PlanetCount;++wi) {
            int fileWorld{}; std::size_t brickCount{};
            if (!(in>>tag>>fileWorld>>brickCount) || tag!="surface_micro" || fileWorld<0 || fileWorld>=PlanetCount) return false;
            for (std::size_t b=0;b<brickCount;++b) {
                int flatCell{}; std::size_t overrideCount{};
                if (!(in>>tag>>flatCell>>overrideCount) || tag!="surface_cell") return false;
                for (std::size_t m=0;m<overrideCount;++m) {
                    int microIndex{},type{};
                    if (!(in>>microIndex>>type)) return false;
                    if (microIndex>=0 && microIndex<MicroBrick::CellCount && type>=0 && type<kBlockTypeCount)
                        planetSurfaces_[static_cast<std::size_t>(fileWorld)]->applySavedMicroEdit(flatCell,microIndex,static_cast<BlockType>(type));
                }
            }
        }
        for (int wi=0;wi<PlanetCount;++wi) {
            int fileWorld{}; std::size_t objectCount{};
            if (!(in>>tag>>fileWorld>>objectCount) || tag!="surface_objects" || fileWorld<0 || fileWorld>=PlanetCount) return false;
            for (std::size_t oi=0;oi<objectCount;++oi) {
                SurfaceMachineObject o{}; int type{},face{},enabled{};
                if (!(in>>tag>>o.stableId>>type>>face>>o.anchor.u>>o.anchor.v>>o.anchor.radial>>enabled>>o.fuelSeconds>>o.storedEnergy) || tag!="surface_obj") return false;
                if (type<0 || type>static_cast<int>(MachineType::Extractor) || face<0 || face>=PlanetSurface::FaceCount) return false;
                o.type=static_cast<MachineType>(type); o.anchor.face=static_cast<CubeFace>(face); o.enabled=enabled!=0;
                o.anchor=planetSurfaces_[static_cast<std::size_t>(fileWorld)]->normalize(o.anchor);
                if (!planetSurfaces_[static_cast<std::size_t>(fileWorld)]->radialInBounds(o.anchor.radial)) return false;
                if (!surfaceBases_[static_cast<std::size_t>(fileWorld)]->restore(o)) return false;
            }
            surfaceBases_[static_cast<std::size_t>(fileWorld)]->update(*planetSurfaces_[static_cast<std::size_t>(fileWorld)],0.0f);
        }
    }

    if (saveVersion == 6) {
        for (int wi=0;wi<PlanetCount;++wi) {
            int fileWorld{};
            Vec3 pos{};
            if (!(in>>tag>>fileWorld>>pos.x>>pos.y>>pos.z) || tag!="surface_player" || fileWorld<0 || fileWorld>=PlanetCount) return false;
            if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) || lengthSq(pos)<1.0f) return false;
            sphericalPlayerPositions_[static_cast<std::size_t>(fileWorld)]=pos;

            std::size_t journalCount{};
            if (!(in>>tag>>fileWorld>>journalCount) || tag!="surface_journals" || fileWorld<0 || fileWorld>=PlanetCount) return false;
            auto& surface=*planetSurfaces_[static_cast<std::size_t>(fileWorld)];
            for (std::size_t ji=0;ji<journalCount;++ji) {
                int face{},cu{},cv{},cr{};
                std::size_t macroCount{},placedCount{},microCellCount{};
                if (!(in>>tag>>face>>cu>>cv>>cr>>macroCount>>placedCount>>microCellCount) || tag!="journal") return false;
                if (face<0 || face>=PlanetSurface::FaceCount) return false;
                const PlanetChunkAddress expected{static_cast<CubeFace>(face),cu,cv,cr};
                for (std::size_t e=0;e<macroCount;++e) {
                    int idx{},type{};
                    if (!(in>>tag>>idx>>type) || tag!="macro") return false;
                    if (idx<0 || idx>=PlanetSurface::TotalCells || type<0 || type>=kBlockTypeCount) return false;
                    if (!(surface.chunkOf(surface.cellFromFlatIndex(idx))==expected)) return false;
                    surface.applySavedEdit(idx,static_cast<BlockType>(type));
                }
                for (std::size_t m=0;m<placedCount;++m) {
                    int idx{};
                    if (!(in>>tag>>idx) || tag!="placed") return false;
                    if (idx<0 || idx>=PlanetSurface::TotalCells || !(surface.chunkOf(surface.cellFromFlatIndex(idx))==expected)) return false;
                    surface.applySavedPlacedMarker(idx);
                }
                for (std::size_t mc=0;mc<microCellCount;++mc) {
                    int idx{}; std::size_t overrideCount{};
                    if (!(in>>tag>>idx>>overrideCount) || tag!="micro") return false;
                    if (idx<0 || idx>=PlanetSurface::TotalCells || !(surface.chunkOf(surface.cellFromFlatIndex(idx))==expected)) return false;
                    for (std::size_t oi=0;oi<overrideCount;++oi) {
                        int microIndex{},type{};
                        if (!(in>>tag>>microIndex>>type) || tag!="microedit") return false;
                        if (microIndex<0 || microIndex>=MicroBrick::CellCount || type<0 || type>=kBlockTypeCount) return false;
                        surface.applySavedMicroEdit(idx,microIndex,static_cast<BlockType>(type));
                    }
                }
            }
        }
        for (int wi=0;wi<PlanetCount;++wi) {
            int fileWorld{}; std::size_t objectCount{};
            if (!(in>>tag>>fileWorld>>objectCount) || tag!="surface_objects" || fileWorld<0 || fileWorld>=PlanetCount) return false;
            for (std::size_t oi=0;oi<objectCount;++oi) {
                SurfaceMachineObject o{}; int type{},face{},enabled{};
                if (!(in>>tag>>o.stableId>>type>>face>>o.anchor.u>>o.anchor.v>>o.anchor.radial>>enabled>>o.fuelSeconds>>o.storedEnergy) || tag!="surface_obj") return false;
                if (type<0 || type>static_cast<int>(MachineType::Extractor) || face<0 || face>=PlanetSurface::FaceCount) return false;
                o.type=static_cast<MachineType>(type); o.anchor.face=static_cast<CubeFace>(face); o.enabled=enabled!=0;
                o.anchor=planetSurfaces_[static_cast<std::size_t>(fileWorld)]->normalize(o.anchor);
                if (!planetSurfaces_[static_cast<std::size_t>(fileWorld)]->radialInBounds(o.anchor.radial)) return false;
                if (!surfaceBases_[static_cast<std::size_t>(fileWorld)]->restore(o)) return false;
            }
            surfaceBases_[static_cast<std::size_t>(fileWorld)]->update(*planetSurfaces_[static_cast<std::size_t>(fileWorld)],0.0f);
        }
    }

    if (saveVersion >= 7) {
        for (int wi=0;wi<PlanetCount;++wi) {
            int fileWorld{};
            Vec3 pos{};
            if (!(in>>tag>>fileWorld>>pos.x>>pos.y>>pos.z) || tag!="surface_player" || fileWorld<0 || fileWorld>=PlanetCount) return false;
            if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) || lengthSq(pos)<1.0f) return false;
            sphericalPlayerPositions_[static_cast<std::size_t>(fileWorld)]=pos;

            std::size_t recordCount{};
            if (!(in>>tag>>fileWorld>>recordCount) || tag!="surface_manifest" || fileWorld<0 || fileWorld>=PlanetCount) return false;
            auto& surface=*planetSurfaces_[static_cast<std::size_t>(fileWorld)];
            auto& infrastructure=*surfaceBases_[static_cast<std::size_t>(fileWorld)];
            SurfaceChunkStore store(kSurfaceChunkSaveRoot,fileWorld,surface.seed(),kSurfaceGeneratorVersion,kSurfaceGeneratorFingerprint);
            for(std::size_t ri=0;ri<recordCount;++ri) {
                int face{},cu{},cv{},cr{};
                if(!(in>>tag>>face>>cu>>cv>>cr) || tag!="surface_chunk" || face<0 || face>=PlanetSurface::FaceCount) return false;
                const PlanetChunkAddress address{static_cast<CubeFace>(face),cu,cv,cr};
                const auto loaded=saveVersion>=8
                    ? store.load(address,fileSaveGeneration)
                    : store.load(address,std::uint64_t{0});
                if(!loaded.loaded) return false;
                const auto& record=loaded.record;
                for(const auto& [idx,type]:record.journal.macroEdits) surface.applySavedEdit(idx,type);
                for(const int idx:record.journal.placedMarkers) surface.applySavedPlacedMarker(idx);
                for(const auto& [idx,brick]:record.journal.microBricks)
                    for(const auto& [microIndex,type]:brick.overrides())
                        surface.applySavedMicroEdit(idx,static_cast<int>(microIndex),type);
                for(const auto& object:record.machines) if(!infrastructure.restore(object)) return false;
                for(const auto& portal:record.portals) if(!infrastructure.restorePortal(portal)) return false;
                for(const auto& airlock:record.airlocks) if(!infrastructure.restoreAirlockAssembly(airlock)) return false;
                for(const auto& rule:record.automationRules) if(!infrastructure.restoreAutomationRule(rule)) return false;
            }
            infrastructure.update(surface,0.0f);
        }
    }

    if (!(in >> tag)) return false;
    if (tag == "siege_count") {
        // Campaign siege embed (append-only schema 8). Absent section => idle directors.
        std::size_t siegeCount{};
        if (!(in >> siegeCount)) return false;
        SurfaceSiegeDirector* directors[PlanetCount]{};
        for (int i = 0; i < PlanetCount; ++i)
            directors[i] = surfaceSieges_[static_cast<std::size_t>(i)].get();
        std::string siegeError;
        if (!restoreCampaignSiegeEmbed(in, siegeCount, directors, PlanetCount, &siegeError))
            return false;
        if (!(in >> tag) || tag != "END") return false;
    } else if (tag != "END") {
        return false;
    }

    currentPlanet_ = std::clamp(planet, 0, PlanetCount-1);
    // Reseat transit authority on the restored world (hull not yet in save schema).
    shipTransit_ = ShipTransit(currentPlanet_, ShipTransit::classForVerticalSliceIndex(currentPlanet_));
    saveGeneration_=saveVersion>=8 ? fileSaveGeneration : 0;
    suspicion().setValue(std::clamp(suspicion, 0.0f, 100.0f));
    favor_ = std::clamp(favor, 0.0f, 100.0f);
    toolTier_ = EarlyMetallurgy::clampTier(tool);
    // Reseat with restored galaxy seed so migrated stable beacon ids match campaign.
    registryBeacons_ = RegistryBeaconAuthority(galaxySeed_);
    registryBeacons_.restoreClaimedFlags(c0 != 0, c1 != 0, c2 != 0);
    suspicion().setClaimFloor(claimFloor());
    ecs_.loadPlayer(p);
    renderer_.invalidate();
    planetRenderer_.invalidate();
    orbitWalkerPosition_={};
    sphericalSurfaceMode_=true;
    orbitalPreview_=false;
    sphericalVerticalVelocity_=0.0f;
    sphericalGrounded_=true;
    sphericalMiningValid_=false;
    ecs_.clearEnemies();
    // WaveActive blobs may list stable IDs that no longer exist in ECS after
    // clearEnemies. Reconcile without changing the siege codec (known v1 limit).
    for (int i = 0; i < PlanetCount; ++i)
        reconcileCampaignSiegeAfterRestore(*surfaceSieges_[static_cast<std::size_t>(i)]);
    return true;
}

} // namespace elysium
