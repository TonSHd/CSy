#pragma once

#include <string>

namespace csy {

struct VoicedMessage {
    std::string text;
    std::string fix;
};

VoicedMessage voice_message(const std::string& raw);
std::string voice_text(const std::string& raw);
std::string voice_footer(std::size_t errors, std::size_t warnings);

}
