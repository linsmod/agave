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
