#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mb {

struct TextView {
    const char* data;
    size_t size;
    TextView(const char* value, size_t length) : data(value), size(length) {}
};

template <size_t N>
inline TextView textView(const char (&value)[N]) { return TextView(value, N - 1); }

inline bool validNodeId(TextView value) {
    if (value.data == nullptr || value.size == 0 || value.size > 32) return false;
    for (size_t i = 0; i < value.size; ++i) {
        const char ch = value.data[i];
        const bool valid = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                           (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!valid) return false;
    }
    return true;
}

inline bool validMqttPort(long value) { return value > 0 && value <= 65535; }

inline bool validMacOrEmpty(TextView value) {
    if (value.size == 0) return true;
    if (value.data == nullptr || value.size != 17) return false;
    for (size_t i = 0; i < value.size; ++i) {
        if (i % 3 == 2) {
            if (value.data[i] != ':') return false;
        } else {
            const char ch = value.data[i];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                  (ch >= 'A' && ch <= 'F'))) return false;
        }
    }
    return true;
}

inline bool strongOtaPassword(TextView value) {
    if (value.data == nullptr || value.size < 12 || value.size > 128) return false;
    bool alpha = false;
    bool digit = false;
    for (size_t i = 0; i < value.size; ++i) {
        const char ch = value.data[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) alpha = true;
        if (ch >= '0' && ch <= '9') digit = true;
        if (static_cast<unsigned char>(ch) < 0x21 || static_cast<unsigned char>(ch) > 0x7e)
            return false;
    }
    return alpha && digit;
}

inline bool contains(TextView value, const char* needle) {
    if (value.data == nullptr || needle == nullptr) return false;
    const size_t needleLength = std::strlen(needle);
    if (needleLength == 0 || needleLength > value.size) return false;
    for (size_t i = 0; i + needleLength <= value.size; ++i) {
        if (std::memcmp(value.data + i, needle, needleLength) == 0) return true;
    }
    return false;
}

inline bool looksLikePemCertificate(TextView value) {
    return contains(value, "-----BEGIN CERTIFICATE-----") &&
           contains(value, "-----END CERTIFICATE-----");
}

}  // namespace mb
