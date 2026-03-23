# 编译说明

这个仓库不要在 Claude Code 的 bash 终端里跑 CMake 编译命令。MSVC 依赖 VS Developer 环境变量，bash 里通常缺少 `INCLUDE`，容易直接报标准库头文件找不到。

## 推荐方式

请在 VSCode 里使用 **CMake Tools** 扩展完成配置和编译：

1. 用 VSCode 打开仓库根目录。
2. 让 CMake Tools 选择一个可用的 MSVC kit / 编译器。
3. 先执行 **Configure**。
4. 再执行 **Build**。

如果你更习惯命令行，请在 **VS Developer Prompt / Developer PowerShell** 里执行：

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release --parallel
```

## 这个仓库的目标

- `iot-manager`：主服务器目标，来自根目录 `CMakeLists.txt`。
- `iot-edgenode`：边缘节点目标，来自 `edgenode/CMakeLists.txt`。

## 构建范围

- 默认构建 `iot-manager` 时，会连带构建 `iot-edgenode`。
- 只想验证边缘节点时，单独 build `iot-edgenode` 即可。
- 测试是独立的 CMake 构建，不属于根目录默认构建。

## Windows 编译注意事项

- 建议使用 **Release** 或 **Debug** 配置，由 CMake Tools 选择。
- 根目录 CMake 会自动尝试使用 `VCPKG_ROOT` 指向的 toolchain。
- 没有 `CMakePresets.json`，所以不要依赖 preset 流程。
- 前端构建默认开启；如果只想编译后端，可在配置时关闭 `BUILD_FRONTEND`。
- 边缘节点单独构建目标是 `iot-edgenode`，不要再找旧的 `iot-agent`。
- `l2config` 是 Windows 专用可选目标，默认关闭；只有需要时再启用 `BUILD_L2CONFIG=ON`。
- `iot-edgenode` 只复制 `config/agent.json`，不触发前端构建。

## 额外说明

- 如果你只想验证边缘节点，可以单独 build `iot-edgenode`。
- 如果你要验证整个仓库，直接 build 全部目标即可。
- `L2CONFIG` 是可选的 Windows 目标，默认关闭；只有需要 L2 配置工具时再启用。
- CI 里 Linux 会验证主后端与边缘节点，Windows 主要用于 `l2config`。
