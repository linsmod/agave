[![Windows](https://github.com/onecoolx/agave/actions/workflows/windows.yml/badge.svg)](https://github.com/onecoolx/agave/actions/workflows/windows.yml)
[![Linux](https://github.com/onecoolx/agave/actions/workflows/linux.yml/badge.svg)](https://github.com/onecoolx/agave/actions/workflows/linux.yml)
# agave
A fast, lightweight and full featured web browser

> 开发入口：[AGENTS.md](AGENTS.md)（铁律 + 最常用命令）；文档索引与当前架构总图见
> [docs/README.md](docs/README.md)、[docs/arch-overview.md](docs/arch-overview.md)。

# 编译输出
[text](build-zig-native/agave_view_host.exe) 
[text](build-zig-native/agave_shell.exe)
agave_shell 浏览器外壳，用户界面
agave_view_host 方便截图

# Tools
[text](tools)
用于编译外部JS引擎依赖（Ant）和编译本项目（Agave）

# 开发准则
默认认为项目开发进行中，默认认为输出是最新，delta改动编译很快，没大问题不准全量重建。
处理错误的方案应该契合浏览器架构，确认设计合理性。

---

Copyright(C) 2009 ~ 2024 Zhang Ji Peng (onecoolx@gmail.com)


