# antjsb.dll

`antjsb.dll` 是 Ant JS 引擎 + 桥接层，导出 `AntBridge.h` 里的 `antb_*` C ABI。
桥的源码在 ant 仓库的 `bridge/`，由 ant 的 Meson target `antjsb` 构建；agave 只消费
`lib/antjsb.dll.a` 和 `include/AntBridge.h`。

## 重新生成

```powershell
# 在 agave 仓库根目录
pwsh -File tools\build_antjsb.ps1

# 可选：自定义 ant checkout / 构建目录
pwsh -File tools\build_antjsb.ps1 -AntRoot D:\src\ant -BuildDir D:\src\ant\build-zig
```

脚本内部：

```powershell
meson compile -C H:\ant-v16\build-zig antjsb
```

然后把产物发布到本目录：

```text
bin/antjsb.dll
bin/antjsb.pdb
lib/antjsb.dll.a
include/AntBridge.h
```

DLL/PDB 被 `.gitignore` 忽略，导入库和头文件在库里，所以 agave 源码不需要改动即可编译。

## 构建选项

`build-zig` 当前使用：

```text
b_lto=false
zlib-ng:b_lto=false
buildtype=release
debug=true
```

Windows + MinGW 下不要打开 LTO：zig/lld 的 `.refptr` 段和 gcc 的 `--export-all-symbols`
都会出问题（V14 时代的结论，V16 仍然适用）。关 LTO 只影响体积/峰值性能，不影响功能。

## 为什么单独一个 DLL

ant 的 vendor 目录里有 BoringSSL、zlib-ng、PCRE2 等静态库；直接链进 `libagave.dll`
会和 agave 自带的 zlib/PolarSSL 撞符号。DLL 边界把冲突隔开，ant 升级时只换这一个文件。

## 桥的边界

- `antjsb.dll` 只导出 `antb_*`（当前 lld 还会带出一些 vendor 符号，暂不影响 agave）；
- 桥不 include agave 的日志头；诊断通过 `antb_set_logger` 回调给宿主；
- `AntBridge.c` 仍直接使用 ant 的内部头（`internal.h`/`gc/roots.h` 等），所以桥和
  ant 必须同一个 commit、同一次构建。
