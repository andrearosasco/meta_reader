#pragma once

#include <GLES3/gl3.h>

#include <cstdint>
#include <string>

std::string BuildHudSnapshot(
    const std::string& connectionState,
    bool trackingValid,
    const std::string& localIp,
    const std::string& targetHost);

void RenderHudTexture(
    GLuint texture,
    uint32_t width,
    uint32_t height,
    const std::string& connectionState,
    bool trackingValid,
    const std::string& localIp,
    const std::string& targetHost);

void RenderHandOverlayTexture(GLuint texture, uint32_t width, uint32_t height);
