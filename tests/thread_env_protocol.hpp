#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace thread_env_proof {

struct Record {
    std::string attempt;
    std::string caseId;
    bool present = false;
    std::string value;
    uint32_t result = 0;
    std::string executableIdentity;
};

inline char hexDigit(unsigned v) { return "0123456789abcdef"[v & 0xfu]; }

inline std::string hexEncode(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out.push_back(hexDigit(c >> 4));
        out.push_back(hexDigit(c));
    }
    return out;
}

inline int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

inline std::optional<std::string> hexDecode(std::string_view encoded) {
    if ((encoded.size() & 1u) != 0) return std::nullopt;
    std::string out;
    out.reserve(encoded.size() / 2);
    for (size_t i = 0; i < encoded.size(); i += 2) {
        const int hi = hexValue(encoded[i]);
        const int lo = hexValue(encoded[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    if (hexEncode(out) != encoded) return std::nullopt;
    return out;
}

inline std::string serialize(const Record &r) {
    return "v=1\n" +
           std::string("attempt=") + hexEncode(r.attempt) + "\n" +
           "case=" + hexEncode(r.caseId) + "\n" +
           "present=" + (r.present ? "1\n" : "0\n") +
           "value=" + hexEncode(r.value) + "\n" +
           "result=" + std::to_string(r.result) + "\n" +
           "exe=" + hexEncode(r.executableIdentity) + "\n" +
           "end=1\n";
}

inline std::optional<Record> parse(std::string_view bytes) {
    std::vector<std::string_view> lines;
    size_t pos = 0;
    while (pos < bytes.size()) {
        const size_t nl = bytes.find('\n', pos);
        if (nl == std::string_view::npos) return std::nullopt;
        lines.push_back(bytes.substr(pos, nl - pos));
        pos = nl + 1;
    }
    if (lines.size() != 8 || lines[0] != "v=1" || lines[7] != "end=1") return std::nullopt;
    auto field = [&](size_t i, std::string_view name) -> std::optional<std::string_view> {
        const std::string prefix = std::string(name) + "=";
        if (!lines[i].starts_with(prefix)) return std::nullopt;
        return lines[i].substr(prefix.size());
    };
    const auto attemptText = field(1, "attempt");
    const auto caseText = field(2, "case");
    const auto presentText = field(3, "present");
    const auto valueText = field(4, "value");
    const auto resultText = field(5, "result");
    const auto exeText = field(6, "exe");
    if (!attemptText || !caseText || !presentText || !valueText || !resultText || !exeText) return std::nullopt;
    const auto attempt = hexDecode(*attemptText);
    const auto caseId = hexDecode(*caseText);
    const auto value = hexDecode(*valueText);
    const auto exe = hexDecode(*exeText);
    if (!attempt || !caseId || !value || !exe) return std::nullopt;
    if (*presentText != "0" && *presentText != "1") return std::nullopt;
    if (resultText->empty()) return std::nullopt;
    uint64_t result = 0;
    for (char c : *resultText) {
        if (c < '0' || c > '9') return std::nullopt;
        result = result * 10 + static_cast<unsigned>(c - '0');
        if (result > UINT32_MAX) return std::nullopt;
    }
    Record r{*attempt, *caseId, *presentText == "1", *value, static_cast<uint32_t>(result), *exe};
    if (serialize(r) != bytes) return std::nullopt;
    return r;
}

} // namespace thread_env_proof
