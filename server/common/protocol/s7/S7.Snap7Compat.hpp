#pragma once

#include <string>

#if __has_include(<snap7/lib/snap7_libmain.h>)
#include <snap7/lib/snap7_libmain.h>
#elif __has_include(<snap7.h>)
#include <snap7.h>
#else
using S7Object = void*;

inline S7Object Cli_Create() { return nullptr; }
inline void Cli_Destroy(S7Object) {}
inline int Cli_ConnectTo(S7Object, const char*, int, int) { return -1; }
inline int Cli_Disconnect(S7Object) { return 0; }
inline int Cli_GetConnected(S7Object) { return 0; }
inline int Cli_ReadArea(S7Object, int, int, int, int, int, void*) { return -1; }
inline int Cli_WriteArea(S7Object, int, int, int, int, int, void*) { return -1; }
inline int Cli_SetConnectionType(S7Object, int) { return 0; }
#endif

using S7ClientHandle = S7Object;

inline constexpr S7ClientHandle kS7InvalidObject = nullptr;

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

inline bool s7IsValidHandle(S7Object handle) {
    return handle != kS7InvalidObject;
}

inline S7ClientHandle s7CliCreate() {
    return Cli_Create();
}

inline void s7CliDestroy(S7ClientHandle client) {
    Cli_Destroy(client);
}

inline int s7CliDisconnect(S7ClientHandle client) {
    return Cli_Disconnect(client);
}

inline bool s7CliGetConnected(S7ClientHandle client) {
    return Cli_GetConnected(client) != 0;
}

inline int s7CliConnectTo(S7ClientHandle client, const char* host, int rack, int slot) {
    return Cli_ConnectTo(client, host, rack, slot);
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
    return Cli_SetConnectionType(client, connectionType);
}

inline int s7CliReadArea(S7ClientHandle client, int area, int dbNumber, int start, int amount, int wordLen, void* data) {
    return Cli_ReadArea(client, area, dbNumber, start, amount, wordLen, data);
}

inline int s7CliWriteArea(S7ClientHandle client, int area, int dbNumber, int start, int amount, int wordLen, void* data) {
    return Cli_WriteArea(client, area, dbNumber, start, amount, wordLen, data);
}
