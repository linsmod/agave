# antbridge.dll

`antbridge.dll`（约 43 MB）是把 Ant JS 引擎包成扁平 C ABI 的桥接 DLL，
agave 通过 `AntBridge.h` 的 9+ 个 `antb_*` 函数使用它。

**DLL 本身不进版本库**（体积原因，见 `.gitignore`），但 `include/AntBridge.h` 和
`lib/antbridge.dll.a`（导入库，40 KB）在库里，所以源码不需要改动即可编译。

## 重新生成

前置：`H:\ant` 源码树、Zig 0.16、MSYS2（`ar`/`nasm`）、已编译好的 `H:\ant\build-zig-dev`。

```powershell
$ws = "C:\Users\linswin\Documents\deepseek-harness\default-workspace"
$bridge = "E:\mybrowsers\agave\src\javascript\ant_backend"

# 1) 编译桥接（C23；这是唯一包含 ant 头文件的 TU）
zig cc -target x86_64-windows-gnu -std=gnu23 -DAGB_BUILD_DLL `
    -I H:\ant\include -I H:\ant\vendor\uthash-2.3.0\src -I $bridge `
    -c "$bridge\AntBridge.c" -o AntBridge.obj

# 2) 链接（必须用 zig c++，ant 里有 C++ 目标；并且要带全部 vendor 静态库）
$libs = Get-ChildItem 'H:\ant\build-zig-dev\vendor' -Recurse -Filter '*.a' | ForEach-Object { $_.FullName }
$ml = 'C:/msys64/mingw64/lib'
zig c++ -target x86_64-windows-gnu -shared -Wl,--stack,134217728 `
    -Wl,--out-implib,antbridge.dll.a `
    -Wl,--start-group AntBridge.obj H:\ant\build-zig-dev\libant.a H:\ant\build-zig-dev\libpkg.a $libs -Wl,--end-group `
    -lpsapi -luser32 -ladvapi32 -liphlpapi -luserenv -lws2_32 -ldbghelp -lole32 `
    -luuid -lshell32 -lm -lsecur32 -lntdll -lcrypt32 -loleaut32 -lwininet -lwinspool -lncrypt `
    "$ml/libpathcch.a" "$ml/libtaskschd.a" -o antbridge.dll

# 3) 放回仓库（DLL 被 .gitignore 忽略，导入库和头文件要提交）
Copy-Item antbridge.dll bin\ -Force
Copy-Item antbridge.dll.a lib\ -Force
Copy-Item "$bridge\AntBridge.h" include\ -Force
```

## 为什么单独一个 DLL

ant 的 vendor 目录里有 boringssl、zlib-ng、pcre2，直接静态链进 `libagave.dll`
会和 `dist/` 里的 zlib / polarssl 撞符号。DLL 边界把冲突隔开，ant 升级时也只换这一个文件。

## ant 侧需要的补丁

`H:\ant` 需要 `js_call_value()`（`src/ant.c` + `include/internal.h`），
见工作区的 `ant-js_call_value.md`。没有它，C 侧无法调用 JS 闭包。

## 构建 Ant 时必须关掉 UBSan

`zig cc` **默认开启 `-fsanitize=undefined`（含 alignment 检查）**。Ant 里有非对齐访问，
一旦编译进去，某些脚本（例如百度首页的 `es6-polyfill`）在运行时会直接
`panic: load of misaligned address ...` 把宿主进程带走 —— 表现就是"页面崩了"。

重编 `H:\ant` 时必须带上：

```powershell
$env:PATH = "C:\msys64\usr\bin;C:\msys64\mingw64\bin;" + $env:PATH   # meson 需要 date
$env:CC  = "zig cc -target x86_64-windows-gnu"
$env:CXX = "zig c++ -target x86_64-windows-gnu"
meson configure build-zig-dev `
  -Dc_args="-fno-sanitize=undefined -fno-sanitize=alignment" `
  -Dcpp_args="-fno-sanitize=undefined -fno-sanitize=alignment"
meson compile -C build-zig-dev
```

副作用：`antbridge.dll` 从 43.7 MB 降到 **13.5 MB**（少了 UBSan 运行库）。
`C:\msys64\usr\bin` 必须在 PATH 里，否则 meson 的 `version/meson.build` 找不到 `date`
而让 `build.ninja` 重新生成失败。
## 上游来源与补丁清单（2026-11 更新）

当前使用的 ant 源码：`https://github.com/theMackabu/ant`
提交 **`5fb9ddb3`**（tag `v14.0.ff84a70d.0-10-g5fb9ddb3`），构建目录 `H:\ant-upstream\build-zig-up`。

选择 v14 而不是最新的 `v16.5.c41757b4.0`：`C:\Users\linswin\Documents\antjs-windows-build-patch`
里的 Windows 构建补丁（`ant-master-buildfix.patch` + MIR 固定提交 + `mir-c2mir.patch`）
是针对这个提交验证过的，直接套在 `master` 上会 `patch does not apply`。

### 应用的补丁

1. `ant-master-buildfix.patch`（Windows 构建修复，3 个文件）
2. `vendor/mir` 固定到 `d89ca1d079576924c0c7e2d38e5845a7e75b1f30`，并应用 `mir-c2mir.patch`
3. `include/internal.h` + `src/ant.c`：导出 `js_call_value()`（宿主从 C 调用 JS 闭包）
4. `src/modules/tls.c`：`default_tls_context(ca,len)` → `default_tls_context()`
   —— 该提交的源码期望新版 tlsuv，而仓库里 vendor 的 tlsuv 是旧签名
5. `src/silver/lexer.c`：`?.` 后面紧跟数字时不能再当作可选链
   （`e?.88*e.offsetHeight` 是三元 + 数字字面量；上游 master 已修，这里回移一行）

### 构建

```powershell
cd H:\ant-upstream
$env:PATH = "C:\msys64\usr\bin;C:\msys64\mingw64\bin;" + $env:PATH
$env:CC  = "zig cc -target x86_64-windows-gnu"
$env:CXX = "zig c++ -target x86_64-windows-gnu"
meson setup build-zig-up --wipe --buildtype=release -Db_lto=false -Dtemporal=disabled `
    "-Dzlib-ng:b_lto=false" `
    "-Dc_args=-fno-sanitize=undefined -fno-sanitize=alignment" `
    "-Dcpp_args=-fno-sanitize=undefined -fno-sanitize=alignment"
meson compile -C build-zig-up
```

产物：`build-zig-up/libant.a`（约 40 MB）、`libpkg.a`、`vendor/**/*.a`。
`-fno-sanitize=*` 不能省：zig cc 默认开 UBSan，会在这套代码的非对齐访问上直接 panic。

### 重新链接 antbridge.dll

见上文「重新生成」，把 `H:\ant\build-zig-dev` 换成 `H:\ant-upstream\build-zig-up`、
头文件路径换成 `H:\ant-upstream\include` 即可。