#pragma once

#include "devices.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

enum class IndicatorStyle : int {
    RingAndGlyph = 0,
    GlyphAndBar = 1,
    RingAndNumber = 2,
    PercentageText = 3,
    ClassicBattery = 4
};

inline COLORREF GetBatteryColor(int pct, bool /*charging*/) {
    if (pct >= 50) return RGB(46, 204, 113);
    if (pct >= 25) return RGB(245, 166, 35);
    return RGB(239, 68, 68);
}

namespace render_detail {

inline constexpr float kPi = 3.14159265358979f;

inline float DistSeg(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay;
    float wx = px - ax, wy = py - ay;
    float len2 = vx * vx + vy * vy;
    float t = (len2 > 0.0f) ? std::clamp((wx * vx + wy * vy) / len2, 0.0f, 1.0f) : 0.0f;
    float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return std::sqrt(dx * dx + dy * dy);
}

inline bool InRoundRect(float px, float py, float l, float t, float r, float b, float rad) {
    float cx = std::clamp(px, l + rad, r - rad);
    float cy = std::clamp(py, t + rad, b - rad);
    float dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= rad * rad;
}

inline bool InDeviceGlyph(DeviceKind kind, float px, float py) {
    switch (kind) {
    case DeviceKind::Mouse: {
        bool body = DistSeg(px, py, 16.0f, 12.2f, 16.0f, 19.8f) <= 4.3f;
        bool hGap = (py >= 13.3f && py <= 14.4f);
        bool vGap = (py <= 14.2f && px >= 15.4f && px <= 16.6f);
        return body && !hGap && !vGap;
    }
    case DeviceKind::Controller: {
        bool top = InRoundRect(px, py, 9.3f, 11.4f, 22.7f, 17.6f, 3.0f);
        bool gripL = DistSeg(px, py, 11.2f, 14.5f, 9.5f, 20.2f) <= 2.1f;
        bool gripR = DistSeg(px, py, 20.8f, 14.5f, 22.5f, 20.2f) <= 2.1f;
        bool dpad = (std::abs(px - 12.6f) <= 1.55f && std::abs(py - 14.8f) <= 0.55f) ||
                    (std::abs(px - 12.6f) <= 0.55f && std::abs(py - 14.8f) <= 1.55f);
        auto dot = [&](float cx, float cy) {
            float dx = px - cx, dy = py - cy;
            return dx * dx + dy * dy <= 0.62f * 0.62f;
        };
        bool btns = dot(19.4f, 13.5f) || dot(19.4f, 16.1f) || dot(18.1f, 14.8f) || dot(20.7f, 14.8f);
        return (top || gripL || gripR) && !dpad && !btns;
    }
    case DeviceKind::Headset: {
        float dx = px - 16.0f, dy = py - 16.6f;
        float dist = std::sqrt(dx * dx + dy * dy);
        bool band = (py <= 17.6f && dist >= 5.15f && dist <= 7.05f);
        bool cupL = DistSeg(px, py, 10.2f, 16.0f, 10.2f, 19.8f) <= 1.65f;
        bool cupR = DistSeg(px, py, 21.8f, 16.0f, 21.8f, 19.8f) <= 1.65f;
        return band || cupL || cupR;
    }
    case DeviceKind::Keyboard: {
        bool body = InRoundRect(px, py, 8.6f, 11.8f, 23.4f, 20.2f, 1.5f);
        bool keyRow1 = (py >= 13.1f && py <= 14.1f) &&
                       ((px >= 10.2f && px <= 11.4f) || (px >= 12.7f && px <= 13.9f) ||
                        (px >= 15.2f && px <= 16.4f) || (px >= 17.7f && px <= 18.9f) || (px >= 20.2f && px <= 21.8f));
        bool keyRow2 = (py >= 15.2f && py <= 16.2f) &&
                       ((px >= 10.2f && px <= 11.9f) || (px >= 13.2f && px <= 14.5f) ||
                        (px >= 15.8f && px <= 17.1f) || (px >= 18.4f && px <= 19.7f) || (px >= 20.8f && px <= 21.8f));
        bool space = (py >= 17.4f && py <= 18.5f && px >= 12.4f && px <= 19.6f);
        return body && !keyRow1 && !keyRow2 && !space;
    }
    case DeviceKind::Generic:
    default: {
        bool nub = InRoundRect(px, py, 14.3f, 8.8f, 17.7f, 10.8f, 0.5f);
        bool body = InRoundRect(px, py, 12.2f, 10.5f, 19.8f, 22.8f, 1.3f);
        bool hollow = InRoundRect(px, py, 13.6f, 11.9f, 18.4f, 21.4f, 0.5f);
        bool bar = (px >= 14.2f && px <= 17.8f && py >= 15.6f && py <= 20.4f);
        return nub || (body && (!hollow || bar));
    }
    }
}

inline bool InRingArc(float px, float py, int pct, float outerR, float innerR, bool& isActive) {
    float dx = px - 16.0f, dy = py - 16.0f;
    float r = std::sqrt(dx * dx + dy * dy);
    if (r < innerR || r > outerR) return false;
    if (pct >= 100) {
        isActive = true;
        return true;
    }
    if (pct <= 0) {
        isActive = false;
        return true;
    }
    float ang = std::atan2(dx, -dy) * (180.0f / kPi);
    if (ang < 0.0f) ang += 360.0f;
    float maxAng = pct * 3.6f;
    isActive = (ang <= maxAng);
    if (!isActive) {
        float midR = (outerR + innerR) * 0.5f;
        float capR = (outerR - innerR) * 0.5f;
        float radEnd = maxAng * (kPi / 180.0f);
        float ex = 16.0f + midR * std::sin(radEnd), ey = 16.0f - midR * std::cos(radEnd);
        float sx = 16.0f, sy = 16.0f - midR;
        if ((px - ex) * (px - ex) + (py - ey) * (py - ey) <= capR * capR ||
            (px - sx) * (px - sx) + (py - sy) * (py - sy) <= capR * capR) {
            isActive = true;
        }
    }
    return true;
}

template <typename SampleFn>
inline void RenderSupersampled(uint32_t* bits, SampleFn&& fn) {
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            int rSum = 0, gSum = 0, bSum = 0, aSum = 0;
            for (int sy = 0; sy < 4; ++sy) {
                float py = y + (sy + 0.5f) * 0.25f;
                for (int sx = 0; sx < 4; ++sx) {
                    float px = x + (sx + 0.5f) * 0.25f;
                    COLORREF c = 0;
                    if (fn(px, py, c)) {
                        rSum += GetRValue(c);
                        gSum += GetGValue(c);
                        bSum += GetBValue(c);
                        aSum += 255;
                    }
                }
            }
            uint32_t a = static_cast<uint32_t>((aSum + 8) / 16);
            uint32_t r = static_cast<uint32_t>((rSum + 8) / 16);
            uint32_t g = static_cast<uint32_t>((gSum + 8) / 16);
            uint32_t b = static_cast<uint32_t>((bSum + 8) / 16);
            bits[y * 32 + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

inline void CompositeWhiteText(uint32_t* dstBits, const std::wstring& text, int fontSize, RECT rc, bool darkHalo = false) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 32;
    bmi.bmiHeader.biHeight = -32;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint32_t* txtBits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HBITMAP txtBmp = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&txtBits), nullptr, 0);
    HDC dc = CreateCompatibleDC(screenDc);
    ReleaseDC(nullptr, screenDc);
    if (!txtBmp || !txtBits || !dc) {
        if (dc) DeleteDC(dc);
        if (txtBmp) DeleteObject(txtBmp);
        return;
    }

    std::fill(txtBits, txtBits + 32 * 32, 0u);
    HGDIOBJ oldBmp = SelectObject(dc, txtBmp);
    HFONT font = CreateFontW(-fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, text.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    GdiFlush();

    auto sampleA = [&](int x, int y) -> uint32_t {
        if (x < 0 || x >= 32 || y < 0 || y >= 32) return 0u;
        uint32_t px = txtBits[y * 32 + x];
        return std::max({(px >> 16) & 0xFFu, (px >> 8) & 0xFFu, px & 0xFFu});
    };
    auto blendOver = [](uint32_t dst, uint32_t sr, uint32_t sg, uint32_t sb, uint32_t sa) -> uint32_t {
        if (sa == 0) return dst;
        uint32_t da = (dst >> 24) & 0xFFu, dr = (dst >> 16) & 0xFFu, dg = (dst >> 8) & 0xFFu, db = dst & 0xFFu;
        uint32_t inv = 255u - sa;
        uint32_t oa = sa + (da * inv + 127u) / 255u;
        uint32_t or_ = (sr * sa + 127u) / 255u + (dr * inv + 127u) / 255u;
        uint32_t og = (sg * sa + 127u) / 255u + (dg * inv + 127u) / 255u;
        uint32_t ob = (sb * sa + 127u) / 255u + (db * inv + 127u) / 255u;
        return (oa << 24) | (or_ << 16) | (og << 8) | ob;
    };

    if (darkHalo) {
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 32; ++x) {
                uint32_t maxA = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) maxA = std::max(maxA, sampleA(x + dx, y + dy));
                }
                if (maxA > 0) dstBits[y * 32 + x] = blendOver(dstBits[y * 32 + x], 16, 20, 28, (maxA * 220u) / 255u);
            }
        }
    }

    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            uint32_t ta = sampleA(x, y);
            if (ta > 0) dstBits[y * 32 + x] = blendOver(dstBits[y * 32 + x], 255, 255, 255, ta);
        }
    }

    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(font);
    DeleteDC(dc);
    DeleteObject(txtBmp);
}

inline HICON BitsToIcon(uint32_t* bits, HBITMAP colorBmp) {
    (void)bits;
    HBITMAP maskBmp = CreateBitmap(32, 32, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, maskBmp, colorBmp};
    HICON icon = CreateIconIndirect(&ii);
    if (maskBmp) DeleteObject(maskBmp);
    return icon;
}

} // namespace render_detail

inline HICON CreateDeviceTrayIcon(const DeviceInfo* dev, IndicatorStyle style, int iconSize = 2) {
    iconSize = std::clamp(iconSize, 0, 3);
    constexpr float kOuterR[] = {13.0f, 14.4f, 15.5f, 15.9f};
    constexpr float kRingThick[] = {2.4f, 2.8f, 3.2f, 3.5f};
    constexpr float kGlyphScale[] = {0.85f, 1.02f, 1.20f, 1.34f};
    constexpr int kRingFont2[] = {13, 15, 18, 21};
    constexpr int kRingFont3[] = {10, 12, 14, 17};
    constexpr int kBadgeFont2[] = {16, 19, 22, 25};
    constexpr int kBadgeFont3[] = {13, 15, 18, 20};

    const float outerR = kOuterR[iconSize];
    const float innerR = outerR - kRingThick[iconSize];
    const float glyphScale = kGlyphScale[iconSize];

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 32;
    bmi.bmiHeader.biHeight = -32;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint32_t* bits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HBITMAP colorBmp = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
    ReleaseDC(nullptr, screenDc);
    if (!colorBmp || !bits) {
        if (colorBmp) DeleteObject(colorBmp);
        return nullptr;
    }

    constexpr COLORREF kTrack = RGB(92, 98, 108);
    constexpr COLORREF kGlyph = RGB(246, 248, 252);
    constexpr COLORREF kDarkBg = RGB(24, 28, 36);

    if (!dev) {
        render_detail::RenderSupersampled(bits, [&](float px, float py, COLORREF& out) {
            bool active = false;
            if (render_detail::InRingArc(px, py, 0, outerR, innerR, active)) {
                out = kTrack;
                return true;
            }
            return false;
        });
        constexpr int kDashFont[] = {12, 14, 17, 19};
        render_detail::CompositeWhiteText(bits, L"--", kDashFont[iconSize], {1, 1, 31, 31}, true);
    } else {
        const int pct = dev->percent;
        const COLORREF accent = GetBatteryColor(pct, dev->charging);
        const DeviceKind kind = dev->kind;

        switch (style) {
        case IndicatorStyle::RingAndGlyph:
            render_detail::RenderSupersampled(bits, [&](float px, float py, COLORREF& out) {
                bool active = false;
                if (render_detail::InRingArc(px, py, pct, outerR, innerR, active)) {
                    out = active ? accent : kTrack;
                    return true;
                }
                float gx = 16.0f + (px - 16.0f) / glyphScale;
                float gy = 16.0f + (py - 16.0f) / glyphScale;
                if (render_detail::InDeviceGlyph(kind, gx, gy)) {
                    out = kGlyph;
                    return true;
                }
                return false;
            });
            break;

        case IndicatorStyle::GlyphAndBar:
            render_detail::RenderSupersampled(bits, [&](float px, float py, COLORREF& out) {
                float barTop = (iconSize >= 2) ? 23.5f : 24.5f;
                float barBot = (iconSize >= 2) ? 30.0f : 29.5f;
                float barL = (iconSize >= 2) ? 1.5f : 3.0f;
                float barR = (iconSize >= 2) ? 30.5f : 29.0f;
                if (render_detail::InRoundRect(px, py, barL, barTop, barR, barBot, 2.0f)) {
                    float fillRight = barL + ((barR - barL) * pct) / 100.0f;
                    out = (px <= fillRight && pct > 0) ? accent : kTrack;
                    return true;
                }
                float gx = 16.0f + (px - 16.0f) / glyphScale;
                float gy = 16.0f + (py - 13.5f) / glyphScale;
                if (render_detail::InDeviceGlyph(kind, gx, gy)) {
                    out = kGlyph;
                    return true;
                }
                return false;
            });
            break;

        case IndicatorStyle::RingAndNumber:
            render_detail::RenderSupersampled(bits, [&](float px, float py, COLORREF& out) {
                bool active = false;
                if (render_detail::InRingArc(px, py, pct, outerR, innerR, active)) {
                    out = active ? accent : kTrack;
                    return true;
                }
                return false;
            });
            render_detail::CompositeWhiteText(bits, std::to_wstring(pct),
                                              (pct >= 100) ? kRingFont3[iconSize] : kRingFont2[iconSize],
                                              {1, 1, 31, 31}, true);
            break;

        case IndicatorStyle::PercentageText:
            render_detail::RenderSupersampled(bits, [&](float px, float py, COLORREF& out) {
                float pad = (iconSize >= 2) ? 0.5f : 1.5f;
                if (!render_detail::InRoundRect(px, py, pad, pad, 32.0f - pad, 32.0f - pad, 7.0f)) return false;
                if (!render_detail::InRoundRect(px, py, pad + 1.5f, pad + 1.5f, 30.5f - pad, 30.5f - pad, 5.5f)) {
                    out = RGB(74, 82, 98);
                    return true;
                }
                if (py >= 24.0f && py <= 28.0f && px >= 4.0f && px <= 28.0f) {
                    float fillR = 4.0f + (24.0f * pct) / 100.0f;
                    out = (px <= fillR && pct > 0) ? accent : RGB(48, 54, 68);
                    return true;
                }
                out = kDarkBg;
                return true;
            });
            render_detail::CompositeWhiteText(bits, std::to_wstring(pct),
                                              (pct >= 100) ? kBadgeFont3[iconSize] : kBadgeFont2[iconSize],
                                              {1, 1, 31, 24}, false);
            break;

        case IndicatorStyle::ClassicBattery: {
            constexpr float kBatL[] = {3.5f, 2.2f, 1.4f, 0.8f};
            constexpr float kBatR[] = {25.5f, 26.8f, 27.6f, 28.2f};
            constexpr float kBatT[] = {8.5f, 7.0f, 5.2f, 3.8f};
            constexpr float kBatB[] = {23.5f, 25.0f, 26.8f, 28.2f};
            const float l = kBatL[iconSize], r = kBatR[iconSize], t = kBatT[iconSize], b = kBatB[iconSize];
            render_detail::RenderSupersampled(bits, [&](float px, float py, COLORREF& out) {
                bool nub = render_detail::InRoundRect(px, py, r - 0.5f, 11.5f, r + 3.0f, 20.5f, 1.0f);
                bool outer = render_detail::InRoundRect(px, py, l, t, r, b, 2.2f);
                bool inner = render_detail::InRoundRect(px, py, l + 2.0f, t + 2.0f, r - 2.0f, b - 2.0f, 0.8f);
                if (inner) {
                    float fillL = l + 2.8f, fillMax = r - 2.8f;
                    float fillR = fillL + ((fillMax - fillL) * pct) / 100.0f;
                    if (px >= fillL && px <= fillR && py >= t + 2.8f && py <= b - 2.8f && pct > 0) {
                        out = accent;
                        return true;
                    }
                    out = kDarkBg;
                    return true;
                }
                if (outer || nub) {
                    out = RGB(226, 232, 240);
                    return true;
                }
                return false;
            });
            if (iconSize >= 2) {
                int fsz = (pct >= 100) ? (iconSize == 3 ? 13 : 11) : (iconSize == 3 ? 16 : 14);
                render_detail::CompositeWhiteText(bits, std::to_wstring(pct), fsz,
                                                  {static_cast<LONG>(l + 2), static_cast<LONG>(t),
                                                   static_cast<LONG>(r - 2), static_cast<LONG>(b)}, true);
            }
            break;
        }
        }
    }

    HICON icon = render_detail::BitsToIcon(bits, colorBmp);
    DeleteObject(colorBmp);
    return icon;
}

inline HICON CreateBatteryTrayIcon(const std::vector<DeviceInfo>& devices, IndicatorStyle style, int iconSize = 2) {
    if (devices.empty()) return CreateDeviceTrayIcon(nullptr, style, iconSize);
    auto lowest = std::min_element(devices.begin(), devices.end(),
                                   [](const DeviceInfo& a, const DeviceInfo& b) { return a.percent < b.percent; });
    return CreateDeviceTrayIcon(&(*lowest), style, iconSize);
}
