#pragma once

#include <cstdint>
#include <string>

#if __has_include(<snap7/snap7_libmain.h>)
#include <snap7/snap7_libmain.h>

using S7ClientHandle = S7Object;
inline constexpr S7ClientHandle kS7InvalidObject = static_cast<S7ClientHandle>(0);

inline bool s7IsValidHandle(S7Object handle) {
    return handle != kS7InvalidObject;
}

inline S7ClientHandle s7CliCreate() {
    return Cli_Create();
}

inline void s7CliDestroy(S7ClientHandle& client) {
    Cli_Destroy(client);
    client = kS7InvalidObject;
}

inline int s7CliDisconnect(S7ClientHandle client) {
    return Cli_Disconnect(client);
}

inline bool s7CliGetConnected(S7ClientHandle client) {
    int connected = 0;
    return Cli_GetConnected(client, connected) == 0 && connected != 0;
}

inline int s7CliConnectTo(S7ClientHandle client, const char* host, int rack, int slot) {
    return Cli_ConnectTo(client, host, rack, slot);
}

inline int s7CliConnect(S7ClientHandle client) {
    return Cli_Connect(client);
}

inline int s7CliSetConnectionParams(S7ClientHandle client, const char* host,
                                    std::uint16_t localTSAP, std::uint16_t remoteTSAP) {
    return Cli_SetConnectionParams(client, host, localTSAP, remoteTSAP);
}

inline int s7CliSetConnectionType(S7ClientHandle client, const char* connectionType) {
    int connectionMode = 0;
    if (connectionType && connectionType[0] != '\0' && connectionType[1] != '\0' && connectionType[2] == '\0') {
        const bool isOp = (connectionType[0] == 'O' || connectionType[0] == 'o') &&
                          (connectionType[1] == 'P' || connectionType[1] == 'p');
        if (isOp) {
            connectionMode = 1;
        }
    }
    return Cli_SetConnectionType(client, connectionMode);
}

inline int s7CliSetConnectionType(S7ClientHandle client, const std::string& connectionType) {
    return s7CliSetConnectionType(client, connectionType.c_str());
}

inline int s7CliSetConnectionType(S7ClientHandle client, int connectionType) {
    return Cli_SetConnectionType(client, static_cast<word>(connectionType));
}

inline int s7CliReadArea(S7ClientHandle client, int area, int dbNumber, int start, int amount, int wordLen, void* data) {
    return Cli_ReadArea(client, area, dbNumber, start, amount, wordLen, data);
}

inline int s7CliWriteArea(S7ClientHandle client, int area, int dbNumber, int start, int amount, int wordLen, void* data) {
    return Cli_WriteArea(client, area, dbNumber, start, amount, wordLen, data);
}

#else

using S7Object = void*;
using S7ClientHandle = S7Object;

inline constexpr S7ClientHandle kS7InvalidObject = nullptr;

inline S7ClientHandle s7CliCreate() { return nullptr; }
inline void s7CliDestroy(S7ClientHandle&) {}
inline int s7CliConnect(S7ClientHandle) { return -1; }
inline int s7CliConnectTo(S7ClientHandle, const char*, int, int) { return -1; }
inline int s7CliSetConnectionParams(S7ClientHandle, const char*, std::uint16_t, std::uint16_t) { return -1; }
inline int s7CliDisconnect(S7ClientHandle) { return 0; }
inline bool s7CliGetConnected(S7ClientHandle) { return false; }
inline int s7CliReadArea(S7ClientHandle, int, int, int, int, int, void*) { return -1; }
inline int s7CliWriteArea(S7ClientHandle, int, int, int, int, int, void*) { return -1; }
inline int s7CliSetConnectionType(S7ClientHandle, const char*) { return 0; }
inline int s7CliSetConnectionType(S7ClientHandle, const std::string&) { return 0; }
inline int s7CliSetConnectionType(S7ClientHandle, int) { return 0; }

#ifndef S7AreaDB
inline constexpr int S7AreaDB = 0;
#endif
#ifndef S7AreaMK
inline constexpr int S7AreaMK = 0;
#endif
#ifndef S7AreaPA
inline constexpr int S7AreaPA = 0;
#endif
#ifndef S7AreaPE
inline constexpr int S7AreaPE = 0;
#endif
#ifndef S7AreaCT
inline constexpr int S7AreaCT = 0;
#endif
#ifndef S7AreaTM
inline constexpr int S7AreaTM = 0;
#endif
#ifndef S7WLByte
inline constexpr int S7WLByte = 0;
#endif
#ifndef S7WLCounter
inline constexpr int S7WLCounter = 0;
#endif
#ifndef S7WLTimer
inline constexpr int S7WLTimer = 0;
#endif

#endif
