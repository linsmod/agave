# 构建与运行：工具链、产物、常见坑

## 1. 工具链

- Windows 10/11（全部构建与调试都在 Windows 上）。
- **MSYS2 mingw64**：`C:\msys64\mingw64\bin` 进 `PATH`（`cmake --build` 直接跑时要自己加）。
- **zig 编译器包装器**：CMake 的 `CMAKE_CXX_COMPILER` 是 `.zig-toolchain\zig-cxx.cmd`
  （内部指向 WinGet 安装的真实 `zig.exe`）。代理会话的沙箱会拒绝执行 symlink 出来的 zig，
  所以包装器必须直指 `WinGet\Packages\...\zig.exe`；**zig 升级后要同步路径**。
- Generator 是 **Ninja**，构建目录 **`build-zig-native/`**（`CMAKE_BUILD_TYPE=Debug`）。
- 构建目录已经存在就**不要 clean**（全量编译代价很大，见 §3）。

## 2. 三条构建路径

```powershell
# A. 常规增量（首次 configure 慢，之后通常几秒到 1 分钟）
pwsh -File tools\build_project.ps1 *> build.log; $LASTEXITCODE
# 脚本只往流里写，由调用者重定向落盘（脚本自身不再写 build-native.log，
# 避免和调用者的重定向抢同一个文件）；输出几千行，先落盘再 grep，别直接灌进上下文

# B. 快速重链（只改了 1-2 个 .cpp/.c，跳过脚本的 toolchain/configure 检查）
$env:PATH = "C:\msys64\mingw64\bin;" + $env:PATH
cmake --build e:\mybrowsers\agave\build-zig-native --parallel 16

# C. 改了桥（ant 的 bridge/AntBridge.c）
pwsh -File tools\build_antjsb.ps1 -Force
pwsh -File tools\build_project.ps1        # 必须用它：它会把新 dll 同步进构建目录

# D. 改了 ant 引擎内核（H:\ant-v16）
pwsh -File tools\build_antjsb.ps1        # 编 libant.a + 重链 antjsb.dll
# 只有改了本仓库 C++ 才需要再 cmake --build build-zig-native
```

**构建变体（引擎层）**：默认脚本固定 `-DOPT_JS_ANT=ON`。`OFF` 时 `AntRuntime.cpp` 不进源列表、
不链 `antjsb`，脚本入口落到 `OPT_USE_KJS`（默认 ON）的 KJS 分支；这是结构裁剪/验证用的变体，
当前发布与回归矩阵只覆盖 ON。

**编译配置（别乱改，否则 attach 只剩模块+偏移）**

| 产物 | 配置 | 说明 |
|---|---|---|
| `libant.a`（ant 引擎） | meson `buildtype=release` + `-Ddebug=true` → `-O3 -g` | 2026-11-04 起打开；`meson configure H:\ant-v16\build-zig -Ddebug=true`，改完 `ninja -C H:\ant-v16\build-zig libant.a` |
| `antjsb.dll`（桥 + 引擎） | ant Meson `-O2 -g` | 产出 `antjsb.pdb`；`build_antjsb.ps1` 装到 `3rdparty/ant/bin`，`build_project.ps1` 每次构建同步进 `build-zig-native\` |
| `agave_shell` / `libagave` | CMake `CMAKE_BUILD_TYPE=Debug`（`-O0 -g -D_DEBUG`） | 带完整 PDB（`libagave.pdb`），不是 release |

## 3. 改什么 → 重编什么（先估时间再动手）

| 改动 | 需要做 | 经验耗时 |
|---|---|---|
| `prelude.js` / `polyfills.js` | 拷到 `build-zig-native\`（或 `cmake --build` 会顺带拷） | 0（下次绑文档生效） |
| `inspector.js` / `vconsole.min.js` | 拷到 `build-zig-native\`（或 `cmake --build` 会顺带拷） | 0（菜单每次点击重新读文件；`AGB_INSPECTOR` 可换 inspector.js 路径） |
| `fonts/*.ttf` | `cmake --build`（CMake `fonts` 目标 `copy_if_different` 到 `build-zig-native\fonts\`；运行时 `FontCachePS` 用 `AddFontResourceExA(FR_PRIVATE)` 注册） | 0（下次启动生效） |
| 一个 `webcore/*.cpp` / `webview/*.cpp` | `cmake --build build-zig-native` | 10 秒 ~ 1 分钟 |
| `AntRuntime.cpp` / `AntBridge.c` | `build_antjsb.ps1` + `cmake --build` | 1 ~ 3 分钟 |
| ant 的 1-2 个 `.c` | `build_antjsb.ps1`（内含 libant + 桥） | ~1 分钟 |
| ant 的**头文件** | 同上，但 libant 会重编 200+ 个对象 | ~10 分钟 |
| 改 `OPT_JS_ANT` / `OPT_USE_KJS` | 重新 configure + 全量重编（开关改变整个源列表） | 全量 |
| 加/删源文件、改 CMakeLists | `src/` 下的源文件由 `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` 收集，ninja 会自己重跑 configure；改 `CMakeLists.txt` 仍要手动 configure | 秒级 ~ 数分钟 |

### 生成物：SVG 名字表/工厂（ksvg2）

- `ENABLE_SVG` 由 `OPT_SVG` 控制（默认 ON）。不开时 ksvg2 的源码仍参与编译，但全部被
  `#if ENABLE(SVG)` 编译成空。
- `src/webcore/buildKJS/{SVGNames,XLinkNames,SVGElementFactory}.{h,cpp}` 与
  `CSSPropertyNames.*`/`CSSValueKeywords.*` 是**签入的生成物**（历史上 SVG 那几份是禁用分支的空桩）。
  改了 `ksvg2/svg/svgtags.in`、`svgattrs.in`、`xlinkattrs.in`、`ksvg2/css/SVGCSS*.in` 之后重跑：

  ```powershell
  pwsh -File tools\gen_svg_names.ps1              # SVG/XLink 名字 + 工厂
  pwsh -File tools\gen_svg_names.ps1 -WithCSS     # 连 CSS 属性/关键字表一起（需要 gperf）
  ```

  脚本会自己找 perl（Git/msys）和 gperf（vcpkg），并统一去掉 `.in` 的 CRLF（`gcc -E` 管道会把 CR
  带进生成物，曾生成出 `a<CR>Tag` 这种断行）。

### 生成物：CSS 语法（`CSSGrammar.cpp`）与属性/关键字表

`src/webcore/buildKJS/CSSGrammar.cpp`/`.h`、`CSSPropertyNames.*`、`CSSValueKeywords.h` 都是**签入的生成物**，
CMake 不跑 bison。只有改了 `src/webcore/css/CSSGrammar.y` 才需要手工重新生成：

```powershell
bison -d -p cssyy src\webcore\css\CSSGrammar.y -o debug\CSSGrammar.tab.c
# 然后按 src\webcore\css\makegrammar.pl 的做法拼：.h = "#ifndef CSSGRAMMAR_H/#define CSSGRAMMAR_H" + tab.h + "#endif"，
# .cpp = tab.c 的内容
```

- **本机没有 bison 时（2026-11-06 就是这样）优先不碰语法**：`calc()`、`var()`、`:is()`/`:where()`、`@page`
  都是在 yacc 之前改写文本（见 `CSSParser::parseSheet` 的 rewrite 链），比改语法便宜得多，也不动签入的生成物。
- ⚠ 用 bison 3.8 重新生成会把整个文件换掉（原来那份是 bison 2.5 的骨架），diff 约 4k 行，必须整体回归
  （`run_layout_tests.ps1` + 站点冒烟），不适合夹在别的改动里做。

## 4. 产物与运行拓扑

`build-zig-native\` 是**唯一可以直接运行的目录**（所有 DLL 和运行时 JS 都在这里）：

| 产物 | 用途 |
|---|---|
| `agave_shell.exe` | 桌面壳：GUI 浏览 + `--probe` 无头探针；`--backend local\|process` 选渲染后端（默认 local） |
| `agave_render_host.exe` | `process` 后端的引擎进程：每个 view 一个，由控件拉起（`--backend process` 时需要它在 exe 旁） |
| `agave_multiview_probe.exe` | 两个 view 的验收宿主：A 卡死时 B 的命令延迟（`pong_ms` / `blocked`），S3 验收 1 |
| `agave_view_host.exe` | **无窗口**渲染：`agave_view_host.exe <url> [out.bmp] [--eval js] [--click x y] [--type text] [--enter] [--wheel n] [--wheelup n] [--repaint]`，像素/交互校验用 |
| `agave_embed.exe` | 嵌入 demo（同一套控件） |
| `libagave.dll` | 引擎 + Win32 控件（`macross_*` 和 `agave_webview_*`） |
| `antjsb.dll` (+ `antjsb.pdb`) | JS 桥 + 引擎（ant 侧构建，见 [ref-ant-engine.md](ref-ant-engine.md)） |
| `prelude.js` / `polyfills.js` | 运行时从 exe 旁加载的 JS shim（可热改） |
| `fonts/NotoSansSC-*.ttf` | 默认字体与 CJK 回退（Noto Sans SC）；构建时从仓库 `fonts/` 拷过来，启动时私有注册（R168） |
| `inspector.js` / `vconsole.min.js` | 外壳菜单注入的调试脚本：从 exe 旁读取后 eval（可热改，见 `examples/browser_shell/README.md`） |
| `%LOCALAPPDATA%\Agave\probe\` / `...\view-host\` | 无头工具自己的 profile（cookies/cache），由 `AGB_USERDATA` 决定，见 [dev-debugging.md](dev-debugging.md) |

- `3rdparty/ant/bin/antjsb.dll` 是**预编译回落副本**（plain clone 也能构建/运行）；运行时仍读 `build-zig-native/` 里的那份。
  两份由 `build_project.ps1` 的 `== ant runtime` 段同步（大小+时间戳相同就跳过）。
  **裸 `cmake --build` 不同步**：`antjsb.dll` 只是 link 期的 import lib 输入，ant 重链但导出表没变时
  `antjsb.dll.a` 不变 → libagave 不重链 → `src/src.cmake` 的 POST_BUILD 拷贝不触发，
  构建会报成功而跑的还是旧引擎。改过 ant 一律走 `build_project.ps1`（或手动拷 dll+pdb）。
- **picasso（图形后端）源码在 `3rdparty/picasso/`，由 `add_subdirectory` 直接编译**，
  产物是 `build-zig-native/picasso/libpicasso2_sw.a`（静态）。它以前是 `ExternalProject`：
  从 `packages/picasso-2.8.0.tar.gz` 解压到构建目录、再用 `packages/patchs/` 覆盖几个文件，
  所以**任何图形修复都写在仓库外**，清构建目录就被还原。现在改图形代码直接改 `3rdparty/picasso/`。
  两个坑：①`BUILD_SHARED_LIBS` 与主工程共用，picasso 只在 `add_subdirectory` 期间用普通变量切成
  静态、之后立刻还原（强制成 cache 会把 libagave 也变静态）；
  ②主工程给每个 C++ TU 强制 `-include src/wtf/Platform.h`（它拉 `mconfig.h`），picasso 没有
  `mconfig.h`，所以 `3rdparty/picasso/CMakeLists.txt` 里把这一条从 `CMAKE_CXX_FLAGS` 剥掉。
- 把 exe 单独拷到别处跑会缺 DLL（`debug/` 里直接跑拷过去的 exe 会失败）。
- **跑 GUI**：`pwsh -File tools/agave.ps1 [url]`（默认 `process` 后端，每个 tab 一个引擎进程；
  `-Local` 回到单引擎线程，`-DryRun` 只打印命令行）。壳本身默认 local：
  `agave_shell.exe [url]` 与 `agave_shell.exe --backend process [url]` 都可用，`--backend` 也给 `--probe` 用。
- 可选宿主：`test/`（`OPT_EXAMPLES=ON` 时构建 `agave_test`）、`examples/touchweb`（手机 UI，单独构建）。
- 单元测试：`unit_tests/` 存在，但默认 `OPT_UNITTEST=OFF` 不构建；打开它会在 configure 阶段
  从网络拉 googletest（`FetchContent`），离线环境会失败。

## 5. 拿 ant 单独跑（脱离浏览器复现 JS 语义）

```powershell
H:\ant-v16\build-zig\ant.exe script.js      # 直接跑一段 JS
H:\ant-v16\build-zig\ant.exe                # REPL
```
用途：怀疑"是引擎语义问题还是浏览器/桥接问题"时，先用它把纯 JS 行为钉死（ant 自带的
`tests/test_wasm_*.cjs` 等也是这样跑的）。

## 6. 环境坑（都实际踩过）

- **`Start-Process -ArgumentList` 不会给含空格的参数加引号**：`--eval` 里带空格/单引号会被拆成多个
  argv，JS 就变成语法错误。要么整串给 `-ArgumentList '--probe "url" --eval "..."'`，要么写 `.cmd`。
- **PowerShell 会把 `&&` 搅成 `;`**（传给 `--eval` 时）；脚本里用嵌套三元或逗号表达式。
- **旧进程占着 `antjsb.dll` 时链接会静默失败**——跑的还是旧 dll。重编前先
  `Get-Process agave_shell | Stop-Process -Force`。
- 命令执行器可能是 bash（`$env:` 会炸）：本仓库脚本都是 PowerShell，用 `pwsh -File` 调用。
- 输出是 UTF-8；把日志重定向到文件再 grep，不要依赖控制台编码。
- 网络偶发长滞（WinHTTP `SendRequest`）通常是 WPAD/PAC 抖动：同刻 GUI 正常就不是代码回归，先重试；
  `AGB_NO_PROXY=1` 可以绕开代理解析。
- **`--shot` 写到 `debug/` 会静默失败**：探针在沙箱里跑，只有**构建目录**
  （`build-zig-native\`）保证可写；写别处 `probeCapture` 打一行 `shot failed` 就继续，
  依赖像素的回归组会报 "probe failed"。所有截图/scratch 都放构建目录
  （`run_media_tests.ps1` / `run_layout_tests.ps1` 已按此约定）。
  格式只认 `.png` / `.bmp`（其它扩展名会明确报错，不再默默写错格式）；`--shot-rect x,y,w,h`
  直接裁图，坐标是 CSS 像素（`getBoundingClientRect()` 的那套，输出按 `--zoom` 变大）。
- **构建日志别直接进上下文**（几千行）：`*> build.log` 然后 `grep`（脚本自身不落盘，落盘靠调用者重定向）。