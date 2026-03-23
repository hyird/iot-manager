#pragma once

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

inline constexpr int S7AreaDB = 0;
inline constexpr int S7AreaMK = 0;
inline constexpr int S7AreaPA = 0;
inline constexpr int S7AreaPE = 0;
inline constexpr int S7AreaCT = 0;
inline constexpr int S7AreaTM = 0;
inline constexpr int S7WLByte = 0;
#endif
