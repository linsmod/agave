# 下载管理器（DownloadManager）设计

> 状态：P0 已实现并验证（引擎 + API + 控制层 + 探针）；P1（GUI 面板）待做。
> 相关经验：[lessons.md](lessons.md) R47；宿主 API 见 `include/macross.h` 的 `cb_download`；
> 回归服务器 `tools/download_server.py`。

## 1. 目标

1. 顶层导航遇到**不能显示**的内容（二进制、附件）时，**存下来**，而不是报一条加载错误。
2. 下载是**独立于页面**的任务：页面跳转、标签页关闭都不影响它。
3. 宿主/GUI 能拿到**完整状态**（开始 / 进度 / 完成 / 失败 / 取消），而不是"收到第一块字节"。
4. 探针能无头验证，且**绝不**写用户目录。

## 2. 为什么以前是报错

| 层 | 原来的样子 | 结果 |
|---|---|---|
| 决策 | `FrameLoaderClientMC::dispatchDecidePolicyForMIMEType()` 永远返回 `PolicyUse` | `PolicyDownload` 分支永远走不到 |
| 兜底 | `MainResourceLoader::continueAfterContentPolicy()` 的 `PolicyUse` 分支：`!canShowMIMEType()` → `cannotShowMIMEType()` + `stopLoadingForPolicyChange()` | R46 之后表现为 `on_error(ERR_NET_UNSUPPORTED_MIME)` + 空白页 |
| 钩子 | `FrameLoaderClientMC::download()` = `notImplemented()` | 即使走到也没人接 |
| 宿主 API | `cb_write_file_data` 已接进 `WebCallback.cpp`，引擎从不调用 | — |
| 控制层 | `agwWriteFileData()`：在**引擎线程** `fopen`，写死 `%USERPROFILE%\Downloads\<URL尾段>`，首字节就回调 `on_download`，忽略 `item->downloadDir` | 半成品：无状态、无失败、污染用户目录 |

## 3. 决策点（唯一）

`FrameLoaderClientMC::dispatchDecidePolicyForMIMEType()`。响应头从
`m_frame->loader()->activeDocumentLoader()->response()` 取。

| 顺序 | 条件 | 结果 |
|---|---|---|
| 1 | `Content-Disposition` 的类型是 `attachment` | `PolicyDownload` |
| 2 | MIME **非空** 且 `!canShowMIMEType(mime)` | `PolicyDownload` |
| 3 | 其它（**含 MIME 为空**） | `PolicyUse`，交给内容嗅探 |

MIME 为空必须放行：`MIMETypeRegistry::isSupportedNonImageMIMEType()` 对空串返回
false，不挡的话"服务器没给 `Content-Type` 的 HTML"会变成下载。

只有**主资源**（`MainResourceLoader`）走这里；`<img>`/`<script>` 等子资源不受影响。

## 4. 所有权交接

`ResourceLoader::releaseResources()` 会把 handle 的 client 清成 0 但**不 cancel**
（`src/webcore/loader/ResourceLoader.cpp:102`）。所以不能直接
`download(m_handle.get(), …)` —— 随后的 `receivedError()` 会把刚接管的下载掐死。

新增一处 WebCore 小改：

```cpp
// ResourceLoader
PassRefPtr<ResourceHandle> detachHandle();   // m_handle 归调用方，loader 不再持有
```

`MainResourceLoader::continueAfterContentPolicy()`：

```cpp
case PolicyDownload: {
    RefPtr<ResourceHandle> handle = detachHandle();
    frameLoader()->client()->download(handle.get(), request(), handleRequest, r);
    receivedError(ResourceError());   // 下载不是加载失败：null 错误 = 结束但没出错
    return;
}
```

**为什么是 null 错误**：传 `interruptionForPolicyChangeError()` 会让每次下载都弹一条
`on_error`（R46 的 `ERR_NET_INTERRUPTED_BY_POLICY`）。null 在 FrameLoader 里正是
"结束了但没出错"，与 cancel 路径同一语义。

## 5. 任务状态机

```
STARTED ──► PROGRESS* ──► FINISHED
   │            │
   └────────────┴──────► FAILED / CANCELLED
```

| 细节 | 决定 |
|---|---|
| 写盘 | 先写 `<name>.part`，`FINISHED` 时 rename —— 失败/取消天然不留半截文件 |
| 文件名 | `filename*=`（RFC 5987，百分号解码）→ `filename=` → URL 尾段（解码）→ `download.bin`；复用 `filenameFromHTTPContentDisposition()` |
| 清洗 | 去 `\ / : * ? " < > \|` 与控制字符、去尾部点/空格、挡 `CON/PRN/AUX/NUL/COM1..9/LPT1..9`、长度上限 120（保扩展名） |
| 去重 | `name (1).ext`、`name (2).ext` |
| 进度节流 | 每 200 ms 或每 64 KiB 一次，状态变化立即发 |
| 并发 | v1 不限（各自独立 handle）；上限/排队留 P2 |
| 目录 | `DownloadManager::setDirectory()`；默认 `<AGB_USERDATA>/downloads`，GUI 由 shell 设成 `%USERPROFILE%\Downloads` |
| 生命周期 | **进程级单例**，不挂在 frame 上 |

## 6. API（引擎 → 控制层 → GUI）

```c
/* include/macross.h —— MC_CALLBACKS 末尾追加，不动既有字段 */
typedef enum _mc_download_state {
    MC_DL_STARTED = 0, MC_DL_PROGRESS, MC_DL_FINISHED, MC_DL_FAILED, MC_DL_CANCELLED
} MC_DOWNLOAD_STATE;

typedef struct _mc_download_info {
    int                id;
    int                state;      /* MC_DOWNLOAD_STATE */
    int                error;      /* _mc_error，仅 FAILED 有效 */
    const char*        utf8_url;
    const char*        utf8_path;
    const char*        utf8_mime;
    unsigned long long received;
    unsigned long long total;      /* 0 = 未知 */
} MC_DOWNLOAD_INFO;

MC_BOOL (*cb_download)(int id, const MC_DOWNLOAD_INFO* info);  /* 返回 FALSE = 取消 */
```

| 层 | 接口 |
|---|---|
| 引擎 | `cb_download`（引擎线程） |
| 控制层 | `on_download(AgaveWebView*, const MC_DOWNLOAD_INFO*, void* user)`（UI 线程） |
| 控制层 | `agave_webview_cancel_download(view, id)`、`agave_webview_set_download_dir(view, dir)` |

旧的 `cb_write_file_data` / `agwWriteFileData()` **不再使用**：它和"引擎拥有文件"互斥，
留着就是两套真相。控制层的实现与其注册已删除；`macross.h` 的字段保留（ABI 不变），
引擎不再调用。

## 7. 线程模型（对接 GUI 的关键）

```
引擎线程                            UI 线程
DownloadManager                     AgaveWebView 下载表(32 槽, 加锁)
  │ cb_download(id, info)              │
  ├── 深拷贝进定长槽 ──────────────────► PostMessageA(AGW_MSG_DOWNLOAD)
  │                                    └─ on_download(view, &info, user)
  │ ◄── agwPost(AGW_CMD_DL_CANCEL) ───── agave_webview_cancel_download()
```

三条硬规则：

1. `cb_download` **跑在引擎线程**：不许建窗口、不许碰 HWND、不许弹对话框。
2. 控制层只做**深拷贝 + PostMessage**，回调在 UI 线程派发（沿用 `AGW_MSG_STATE` 那套）。
3. 取消/改目录走 `agwPost` 命令队列，UI 线程不直接碰 `DownloadManager`。

## 8. 面板 = 内部页（`agave://downloads`）

面板不是一个 Win32 列表窗口，而是一张 **HTML 页面 + 标签页**：浏览器自己就是渲染器，
列表样式改起来不用重编。

| 元素 | 行为 |
|---|---|
| 状态栏 | `Downloading name 45% (1.2/2.7 MB)` / `Downloaded: path` / `Download failed (code) url` |
| `Ctrl+J` | 新标签页导航到 `agave://downloads` |
| 面板 | `src/webview/internal/downloads.html`：文件 / 大小 / 状态（进度条）/ 取消 · 打开 |

### 8.1 为什么面板要自己的 runtime

`agave.downloads()` 会暴露用户的下载路径，`agave.cancelDownload()` 能取消传输 —— 这是
**能力**，不是 DOM 特性。挂在每个页面的 `window` 上，任何站点一行 `if (window.agave)`
就能枚举、取消。所以：

```
agave://downloads
   │ WebView::loadUrl → loadInternalPage() 读 <exe>/downloads.html
   │                  → loadHtml(html, "agave://downloads")   (SubstituteData.responseURL)
   ▼
Document（自己的 realm，document.URL = agave://downloads）
   │ AntRuntime::bindDocument：
   │    isInternalDocument() ? 注册 __agbDownloadList/Cancel/Open + eval internal.js
   │    否则                    只有 prelude.js / polyfills.js
   ▼
internal.js → window.agave = { downloads(), cancelDownload(id), openDownload(id), onchange }
事件：DownloadManager::emit → g_agbDownloadEventSink → 仅内部 realm 派发 agavedownloadchange
```

| 文件 | 作用 |
|---|---|
| `src/javascript/ant_backend/internal.js` | 内部页 JS 门面（运行时加载，`AGB_INTERNAL` 可覆盖） |
| `src/webview/internal/downloads.html` | 面板页面（运行时资产，拷到 exe 旁） |
| `src/webview/DownloadEvents.h` | `DownloadManager` → JS 的单一 hook（函数指针，避免 webview 依赖 JS 后端） |

**不给内部页再开一个 VM**：realm 本来就是"一份文档一个 engine"，内部页天然独立；真正要
独立的是**绑定面**。两个 VM 同时活跃会把 `AntRuntime` 的进程级 `g_engine`/`g_document`
全量改成实例化，收益为零。

面板只读 `DownloadManager` 的快照，不缓存状态；引擎销毁某个 job 后列表自然变短。

## 9. 探针

- 顶层导航变成下载时**没有文档提交**，`on_progress(finished)` 不会来。新增
  `probeOnDownload()`：`MC_DL_FINISHED` 且**尚无文档** → 打印 `[probe] download path=… bytes=…`
  → **exit 0**（下载是成功的导航结果，不是 exit 5）。已有文档时（页面自己触发下载）只记日志，
  退出码仍由 `--eval`/`probeExit` 决定。
- 探针模式下载目录强制 `<AGB_USERDATA>/downloads`，绝不写 `%USERPROFILE%\Downloads`（同 R25）。

## 10. 已知限制

无断点续传/Range；无"另存为"对话框；无 `<a download>` 与 `blob:` 下载；`fwrite` 在
引擎线程（大文件会顶一下页面，P2 改异步写线程）；401/重定向中途只做 FAILED。
内部页目前仍是普通 realm（没有独立生命周期），面板每次打开重新读快照。

## 11. 分期

| 期 | 内容 | 验收 |
|---|---|---|
| P0 ✅ | 决策 + `detachHandle` + DownloadManager + API + 控制层下载表 + 探针 | 见下 §11.1 |
| P1 ✅ | `agave://` 内部页 runtime + 面板 + `Ctrl+J` + 状态栏 | 见下 §11.2 |
| P2 | 异步写、并发上限、断点续传、另存为、`<a download>`、`agave://history` | — |

### 11.1 P0 验证（2026-11-05）

```bash
python tools/download_server.py 8975 &            # 起测试服务器
AGB_USERDATA=<临时目录> agave_shell.exe --probe http://127.0.0.1:8975/blob.bin \
    --wait 1500 --timeout 20000
```

| 用例 | 期望 | 实测 |
|---|---|---|
| `/blob.bin`（512 KiB octet-stream） | exit 0，文件 524288 字节、md5 与源一致 | ✅ `09cc21fef48e9ad67608d530df98cbf5` |
| `/report`（`filename="report.txt"`） | 落 `report.txt`，35 字节 | ✅ |
| `/cjk`（`filename*=UTF-8''…`） | 落 `报告 v2.bin` | ✅ |
| `/attach`（HTML + attachment） | 下载而不是渲染 | ✅ `attached.html` |
| 同 URL 再下一次 | `blob (1).bin` | ✅ |
| `/plain`（普通 HTML） | 仍然渲染，exit 0 | ✅ `[exit] dl plain` |
| `mailto:` | 仍然 exit 5（R46 不回退） | ✅ 406004 |
| 回归（四套全绿） | nav 8/8、typing 8/8、worker 9/9、deepseek `rs=complete inputs=2` | ✅ |

`--probe` 的下载落盘目录跟随 `AGB_USERDATA`（`<userdata>/downloads`），不碰
`%USERPROFILE%\Downloads`。

### 11.2 P1 验证（2026-11-05）

| 用例 | 命令 | 实测 |
|---|---|---|
| 内部页能开 | `--probe agave://downloads --eval "probeExit(typeof agave + ' url=' + document.URL)"` | `object url=agave://downloads`，exit 0 |
| **隔离**：网页拿不到 | `--probe tools/pages/cap_dom.html --eval "probeExit('agave=' + typeof agave)"` | `agave=undefined`（deepseek 上同样） |
| 面板渲染真实任务 | 面板里 `appendChild(iframe)` 指向 `…/report`，1.2 s 后读 DOM | `rows=1 state=2 cells=4` |
| 下载矩阵不回退 | `/blob.bin`、`/cjk` | exit 0，md5/文件名不变 |
| 回归 | nav 8/8、typing 8/8、worker 9/9、deepseek `rs=complete inputs=2` | 全绿 |

## 12. 代码索引

| 位置 | 作用 |
|---|---|
| `src/webview/DownloadManager.{h,cpp}` | 任务状态机、文件名、写盘 |
| `src/webview/FrameLoaderClientMC.cpp` | 决策（`dispatchDecidePolicyForMIMEType`）与入口（`download`） |
| `src/webcore/loader/ResourceLoader.cpp` | `detachHandle()` |
| `src/webcore/loader/MainResourceLoader.cpp` | `PolicyDownload` 分支 |
| `src/webview/WebCallback.cpp` | `cb_download` 全局指针 |
| `src/webview_control/AgaveWebView.c` | 下载表 + `AGW_MSG_DOWNLOAD` + `AGW_CMD_DL_CANCEL` |
| `examples/browser_shell/main.c` | 状态栏 / 探针退出码 / `Ctrl+J` |
| `src/webview/SchemeExtension.cpp` | `agave` 加进内置 scheme（否则导航策略以 406002 拒掉） |
| `src/webview/WebView.cpp` | `agave://` → 读 `downloads.html` → `loadHtml(html, "agave://…")` |
| `src/javascript/ant_backend/AntRuntime.cpp` | `isInternalDocument()` 分叉 + `__agbDownload*` |
| `src/javascript/ant_backend/internal.js` | `window.agave` 门面 |
| `src/webview/internal/downloads.html` | 面板页面 |
