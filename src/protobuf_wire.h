#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pb {

enum class WireType : uint8_t {
    Varint = 0,
    Fixed64 = 1,
    LengthDelimited = 2,
    Fixed32 = 5,
};

class Writer {
public:
    const std::vector<uint8_t>& bytes() const { return data_; }
    std::vector<uint8_t>& bytes() { return data_; }

    void key(uint32_t field, WireType type) {
        varint((static_cast<uint64_t>(field) << 3U) | static_cast<uint8_t>(type));
    }

    void varint(uint64_t value) {
        while (value > 0x7fU) {
            data_.push_back(static_cast<uint8_t>((value & 0x7fU) | 0x80U));
            value >>= 7U;
        }
        data_.push_back(static_cast<uint8_t>(value));
    }

    void uint32Field(uint32_t field, uint32_t value) {
        key(field, WireType::Varint);
        varint(value);
    }

    void enumField(uint32_t field, uint32_t value) { uint32Field(field, value); }

    void sint32Field(uint32_t field, int32_t value) {
        key(field, WireType::Varint);
        const uint32_t zigzag = (static_cast<uint32_t>(value) << 1U) ^
                                static_cast<uint32_t>(value >> 31);
        varint(zigzag);
    }

    void fixed64Field(uint32_t field, uint64_t value) {
        key(field, WireType::Fixed64);
        for (unsigned i = 0; i < 8; ++i) {
            data_.push_back(static_cast<uint8_t>(value & 0xffU));
            value >>= 8U;
        }
    }

    void stringField(uint32_t field, const std::string& value) {
        key(field, WireType::LengthDelimited);
        varint(value.size());
        data_.insert(data_.end(), value.begin(), value.end());
    }

    void bytesField(uint32_t field, const uint8_t* value, size_t length) {
        key(field, WireType::LengthDelimited);
        varint(length);
        data_.insert(data_.end(), value, value + length);
    }

    void messageField(uint32_t field, const Writer& nested) {
        bytesField(field, nested.bytes().data(), nested.bytes().size());
    }

private:
    std::vector<uint8_t> data_;
};

struct Field {
    uint32_t number{0};
    WireType type{WireType::Varint};
    uint64_t varintValue{0};
    uint64_t fixed64Value{0};
    const uint8_t* data{nullptr};
    size_t length{0};
};

class Reader {
public:
    Reader(const uint8_t* data, size_t length) : data_(data), length_(length) {}

    bool next(Field& field) {
        if (position_ >= length_) {
            return false;
        }

        uint64_t rawKey = 0;
        if (!readVarint(rawKey) || rawKey == 0) {
            valid_ = false;
            return false;
        }

        field = Field{};
        field.number = static_cast<uint32_t>(rawKey >> 3U);
        field.type = static_cast<WireType>(rawKey & 0x07U);

        switch (field.type) {
            case WireType::Varint:
                if (!readVarint(field.varintValue)) {
                    valid_ = false;
                    return false;
                }
                return true;
            case WireType::Fixed64:
                if (position_ + 8 > length_) {
                    valid_ = false;
                    return false;
                }
                for (unsigned i = 0; i < 8; ++i) {
                    field.fixed64Value |= static_cast<uint64_t>(data_[position_ + i]) << (8U * i);
                }
                position_ += 8;
                return true;
            case WireType::LengthDelimited: {
                uint64_t size = 0;
                if (!readVarint(size) || size > length_ - position_) {
                    valid_ = false;
                    return false;
                }
                field.data = data_ + position_;
                field.length = static_cast<size_t>(size);
                position_ += field.length;
                return true;
            }
            case WireType::Fixed32:
                if (position_ + 4 > length_) {
                    valid_ = false;
                    return false;
                }
                position_ += 4;
                return true;
            default:
                valid_ = false;
                return false;
        }
    }

    bool valid() const { return valid_; }
    bool finished() const { return valid_ && position_ == length_; }

private:
    bool readVarint(uint64_t& result) {
        result = 0;
        for (unsigned shift = 0; shift < 64 && position_ < length_; shift += 7) {
            const uint8_t byte = data_[position_++];
            result |= static_cast<uint64_t>(byte & 0x7fU) << shift;
            if ((byte & 0x80U) == 0) {
                return true;
            }
        }
        return false;
    }

    const uint8_t* data_;
    size_t length_;
    size_t position_{0};
    bool valid_{true};
};

inline int32_t decodeZigZag32(uint64_t value) {
    const uint32_t v = static_cast<uint32_t>(value);
    return static_cast<int32_t>((v >> 1U) ^ static_cast<uint32_t>(-static_cast<int32_t>(v & 1U)));
}

}  // namespace pb
