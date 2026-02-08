# 项目规范

- 使用中文回答
- 不要在 Claude Code 终端中执行 CMake 编译命令（MSVC 需要 VS Developer 环境变量，bash shell 中缺少 INCLUDE 路径会导致标准库头文件找不到）。代码修改完成后提示用户在 VSCode CMake Tools 中自行编译验证。
