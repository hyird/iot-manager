#include "common/protocol/s7/S7.Client.hpp"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

std::string hexDump(const Bytes& bytes) {
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0) {
            oss << ' ';
        }
        oss << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return oss.str();
}

bool expectBytes(const std::string& name, const Bytes& actual, const Bytes& expected) {
    if (actual == expected) {
        return true;
    }

    std::cerr << name << " mismatch\n"
              << "expected: " << hexDump(expected) << '\n'
              << "actual  : " << hexDump(actual) << '\n';
    return false;
}

}  // namespace

int main() {
    bool ok = true;

    s7::Client client;
    ok = ok && client.setConnectionParams("127.0.0.1", 0x0100, 0x0101) == s7::kS7Ok;

    ok = ok && expectBytes("iso.cr", client.buildConnectionRequestFrame(), Bytes{
        0x03, 0x00, 0x00, 0x16, 0x11, 0xE0, 0x00, 0x00,
        0x00, 0x01, 0x00, 0xC0, 0x01, 0x0A, 0xC1, 0x02,
        0x01, 0x00, 0xC2, 0x02, 0x01, 0x01,
    });
    ok = ok && expectBytes("iso.cr-repeat", client.buildConnectionRequestFrame(), Bytes{
        0x03, 0x00, 0x00, 0x16, 0x11, 0xE0, 0x00, 0x00,
        0x00, 0x01, 0x00, 0xC0, 0x01, 0x0A, 0xC1, 0x02,
        0x01, 0x00, 0xC2, 0x02, 0x01, 0x01,
    });

    ok = ok && expectBytes("setup-communication", client.buildSetupCommunicationRequest(), Bytes{
        0x32, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08,
        0x00, 0x00, 0xF0, 0x00, 0x00, 0x01, 0x00, 0x01,
        0x01, 0xE0,
    });

    std::uint8_t readBuffer = 0;
    std::vector<s7::DataItem> readItems{{
        .area = S7AreaPE,
        .dbNumber = 0,
        .start = 0,
        .amount = 1,
        .wordLen = S7WLByte,
        .data = &readBuffer,
        .capacity = 1,
    }};
    ok = ok && expectBytes("read-pe0-byte", client.buildReadRequestForItems(readItems, 0, 1), Bytes{
        0x32, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x0E,
        0x00, 0x00, 0x04, 0x01, 0x12, 0x0A, 0x10, 0x02,
        0x00, 0x01, 0x00, 0x00, 0x81, 0x00, 0x00, 0x00,
    });

    std::uint8_t writeBuffer = 0x01;
    std::vector<s7::DataItem> writeItems{{
        .area = S7AreaPA,
        .dbNumber = 0,
        .start = 0,
        .amount = 1,
        .wordLen = S7WLByte,
        .data = &writeBuffer,
        .capacity = 1,
    }};
    ok = ok && expectBytes("write-pa0-byte", client.buildWriteRequestForItems(writeItems, 0, 1), Bytes{
        0x32, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x0E,
        0x00, 0x05, 0x05, 0x01, 0x12, 0x0A, 0x10, 0x02,
        0x00, 0x01, 0x00, 0x00, 0x82, 0x00, 0x00, 0x00,
        0x00, 0x04, 0x00, 0x08, 0x01,
    });

    std::uint16_t counterBuffer = 0;
    std::vector<s7::DataItem> counterItems{{
        .area = S7AreaCT,
        .dbNumber = 0,
        .start = 5,
        .amount = 1,
        .wordLen = S7WLByte,
        .data = &counterBuffer,
        .capacity = sizeof(counterBuffer),
    }};
    ok = ok && expectBytes("read-ct-normalized", client.buildReadRequestForItems(counterItems, 0, 1), Bytes{
        0x32, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x0E,
        0x00, 0x00, 0x04, 0x01, 0x12, 0x0A, 0x10, 0x1C,
        0x00, 0x01, 0x00, 0x00, 0x1C, 0x00, 0x00, 0x05,
    });

    s7::Client customPduClient;
    std::uint16_t pduRequest = 960;
    ok = ok && customPduClient.setParam(p_u16_PduRequest, &pduRequest) == s7::kS7Ok;
    ok = ok && expectBytes("setup-communication-custom-pdu",
        customPduClient.buildSetupCommunicationRequest(), Bytes{
            0x32, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08,
            0x00, 0x00, 0xF0, 0x00, 0x00, 0x01, 0x00, 0x01,
            0x03, 0xC0,
        });

    return ok ? 0 : 1;
}
