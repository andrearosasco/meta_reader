#include "hud_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <vector>

namespace {

using Glyph = std::array<uint8_t, 7>;

constexpr Glyph kBlank{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr std::string_view kGlyphAlphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.: ";
constexpr std::array<Glyph, 40> kGlyphs{{
    Glyph{0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}, Glyph{0x04,0x0c,0x14,0x04,0x04,0x04,0x1f},
    Glyph{0x0e,0x11,0x01,0x02,0x04,0x08,0x1f}, Glyph{0x1e,0x01,0x01,0x0e,0x01,0x01,0x1e},
    Glyph{0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}, Glyph{0x1f,0x10,0x10,0x1e,0x01,0x01,0x1e},
    Glyph{0x0e,0x10,0x10,0x1e,0x11,0x11,0x0e}, Glyph{0x1f,0x01,0x02,0x04,0x08,0x08,0x08},
    Glyph{0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e}, Glyph{0x0e,0x11,0x11,0x0f,0x01,0x01,0x0e},
    Glyph{0x0e,0x11,0x11,0x1f,0x11,0x11,0x11}, Glyph{0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e},
    Glyph{0x0e,0x11,0x10,0x10,0x10,0x11,0x0e}, Glyph{0x1c,0x12,0x11,0x11,0x11,0x12,0x1c},
    Glyph{0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}, Glyph{0x1f,0x10,0x10,0x1e,0x10,0x10,0x10},
    Glyph{0x0f,0x10,0x10,0x17,0x11,0x11,0x0f}, Glyph{0x11,0x11,0x11,0x1f,0x11,0x11,0x11},
    Glyph{0x1f,0x04,0x04,0x04,0x04,0x04,0x1f}, Glyph{0x1f,0x02,0x02,0x02,0x12,0x12,0x0c},
    Glyph{0x11,0x12,0x14,0x18,0x14,0x12,0x11}, Glyph{0x10,0x10,0x10,0x10,0x10,0x10,0x1f},
    Glyph{0x11,0x1b,0x15,0x15,0x11,0x11,0x11}, Glyph{0x11,0x11,0x19,0x15,0x13,0x11,0x11},
    Glyph{0x0e,0x11,0x11,0x11,0x11,0x11,0x0e}, Glyph{0x1e,0x11,0x11,0x1e,0x10,0x10,0x10},
    Glyph{0x0e,0x11,0x11,0x11,0x15,0x12,0x0d}, Glyph{0x1e,0x11,0x11,0x1e,0x14,0x12,0x11},
    Glyph{0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e}, Glyph{0x1f,0x04,0x04,0x04,0x04,0x04,0x04},
    Glyph{0x11,0x11,0x11,0x11,0x11,0x11,0x0e}, Glyph{0x11,0x11,0x11,0x11,0x11,0x0a,0x04},
    Glyph{0x11,0x11,0x11,0x15,0x15,0x15,0x0a}, Glyph{0x11,0x11,0x0a,0x04,0x0a,0x11,0x11},
    Glyph{0x11,0x11,0x0a,0x04,0x04,0x04,0x04}, Glyph{0x1f,0x01,0x02,0x04,0x08,0x10,0x1f},
    Glyph{0x00,0x00,0x00,0x1f,0x00,0x00,0x00}, Glyph{0x00,0x00,0x00,0x00,0x00,0x0c,0x0c},
    Glyph{0x00,0x0c,0x0c,0x00,0x0c,0x0c,0x00}, Glyph{0x00,0x00,0x00,0x00,0x00,0x00,0x00},
}};

const Glyph& GlyphFor(char character) {
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    const size_t index = kGlyphAlphabet.find(upper);
    return index == std::string_view::npos ? kBlank : kGlyphs[index];
}

void BlendPixel(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, int x, int y,
                uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) {
        return;
    }
    const size_t index = (static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4;
    const float srcAlpha = static_cast<float>(alpha) / 255.0f;
    const float dstAlpha = static_cast<float>(pixels[index + 3]) / 255.0f;
    const float outAlpha = srcAlpha + dstAlpha * (1.0f - srcAlpha);
    if (outAlpha <= 0.0f) {
        return;
    }
    const auto blend = [&](uint8_t src, size_t offset) {
        const float dst = static_cast<float>(pixels[index + offset]);
        const float out = (static_cast<float>(src) * srcAlpha + dst * dstAlpha * (1.0f - srcAlpha)) / outAlpha;
        pixels[index + offset] = static_cast<uint8_t>(std::clamp(out, 0.0f, 255.0f));
    };
    blend(red, 0);
    blend(green, 1);
    blend(blue, 2);
    pixels[index + 3] = static_cast<uint8_t>(std::clamp(outAlpha * 255.0f, 0.0f, 255.0f));
}

void DrawRect(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, int x, int y, int rectWidth, int rectHeight,
              uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    for (int row = 0; row < rectHeight; ++row) {
        for (int column = 0; column < rectWidth; ++column) {
            BlendPixel(pixels, width, height, x + column, y + row, red, green, blue, alpha);
        }
    }
}

void DrawText(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, int x, int y, int scale,
              const std::string& text, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    int cursorX = x;
    for (char character : text) {
        const Glyph& glyph = GlyphFor(character);
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if ((glyph[row] & (1 << (4 - column))) == 0) {
                    continue;
                }
                for (int scaleY = 0; scaleY < scale; ++scaleY) {
                    for (int scaleX = 0; scaleX < scale; ++scaleX) {
                        BlendPixel(pixels, width, height, cursorX + column * scale + scaleX, y + row * scale + scaleY, red, green, blue, alpha);
                    }
                }
            }
        }
        cursorX += 6 * scale;
    }
}

void UploadTexture(GLuint texture, uint32_t width, uint32_t height, const std::vector<uint8_t>& pixels, bool flipY) {
    std::vector<uint8_t> upload(pixels.size());
    if (flipY) {
        const size_t rowBytes = static_cast<size_t>(width) * 4;
        for (uint32_t row = 0; row < height; ++row) {
            std::memcpy(upload.data() + static_cast<size_t>(height - 1 - row) * rowBytes, pixels.data() + static_cast<size_t>(row) * rowBytes, rowBytes);
        }
    } else {
        upload = pixels;
    }

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, upload.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace

std::string BuildHudSnapshot(const std::string& connectionState, bool trackingValid, const std::string& localIp, const std::string& targetHost) {
    std::ostringstream stream;
    stream << connectionState << '|' << (trackingValid ? '1' : '0') << '|' << localIp << '|' << targetHost;
    return stream.str();
}

void RenderHudTexture(GLuint texture, uint32_t width, uint32_t height, const std::string& connectionState, bool trackingValid, const std::string& localIp, const std::string& targetHost) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4, 0);
    const bool connected = connectionState.find("CONNECTED") != std::string::npos;

    DrawRect(pixels, width, height, 16, 16, static_cast<int>(width) - 32, static_cast<int>(height) - 32, 8, 10, 18, 196);
    DrawRect(pixels, width, height, 16, 16, static_cast<int>(width) - 32, 4, 0, 176, 255, 220);
    DrawText(pixels, width, height, 40, 34, 2, "META READER", 255, 255, 255, 255);
    DrawText(pixels, width, height, 40, 90, 2, "CONNECTION:", 130, 180, 255, 255);
    DrawText(pixels, width, height, 280, 90, 2, connectionState, connected ? 64 : 255, connected ? 255 : 88, connected ? 128 : 88, 255);
    DrawText(pixels, width, height, 40, 126, 2, "STREAM:", 130, 180, 255, 255);
    DrawText(pixels, width, height, 280, 126, 2, connected ? "LIVE" : "IDLE", 255, 255, 255, 255);
    DrawText(pixels, width, height, 40, 162, 2, "TRACKING:", 130, 180, 255, 255);
    DrawText(pixels, width, height, 280, 162, 2, trackingValid ? "VALID" : "INVALID", trackingValid ? 64 : 255, trackingValid ? 255 : 88, trackingValid ? 128 : 88, 255);
    DrawText(pixels, width, height, 560, 90, 2, "LOCAL IP:", 130, 180, 255, 255);
    DrawText(pixels, width, height, 760, 90, 2, localIp, 255, 255, 255, 255);
    DrawText(pixels, width, height, 560, 126, 2, "TARGET HOST:", 130, 180, 255, 255);
    DrawText(pixels, width, height, 760, 126, 2, targetHost, 255, 255, 255, 255);
    UploadTexture(texture, width, height, pixels, true);
}

void RenderHandOverlayTexture(GLuint texture, uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4, 0);
    const float centerX = static_cast<float>(width) * 0.5f;
    const float centerY = static_cast<float>(height) * 0.5f;
    const float outerRadius = static_cast<float>(width) * 0.42f;
    const float innerRadius = static_cast<float>(width) * 0.24f;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const float dx = static_cast<float>(x) - centerX;
            const float dy = static_cast<float>(y) - centerY;
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (distance > outerRadius) {
                continue;
            }
            const size_t index = (static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4;
            pixels[index + 0] = 0;
            pixels[index + 1] = distance >= innerRadius ? 220 : 120;
            pixels[index + 2] = 255;
            pixels[index + 3] = distance >= innerRadius ? 224 : 72;
        }
    }
    UploadTexture(texture, width, height, pixels, false);
}
