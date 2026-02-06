#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

/**
 * @brief 平台工具 - 处理平台特定的功能
 */
class PlatformUtils {
public:
    /**
     * @brief 初始化平台特定设置
     */
    static void initialize() {
#ifdef _WIN32
        disableQuickEditMode();
#endif
    }

private:
#ifdef _WIN32
    /**
     * @brief 禁用 Windows 控制台快速编辑模式
     * @details 防止点击控制台导致程序暂停
     */
    static void disableQuickEditMode() {
        HANDLE hInput = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode;
        if (GetConsoleMode(hInput, &mode)) {
            mode &= ~ENABLE_QUICK_EDIT_MODE;
            mode |= ENABLE_EXTENDED_FLAGS;
            SetConsoleMode(hInput, mode);
        }
    }
#endif
};
