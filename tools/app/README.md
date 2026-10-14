# 206Dash 表盘助手（`tools/app/`）—— 一个窗口里把"设计 → 构建 → 烧写 → 预览"做完

> 2026-09-29 新增。车主："把之前的网页端独立成一个应用程序。"
> 2026-10-15 改版。车主："the client is still not suitable for a user, too complex
> —— simplify the flow and beautify the UI." ⇒ 七个页签收成**三个屏**，
> 刷固件变成**看得见的三步**，术语换成人话（技术值收进「高级」）。

## 怎么启动

双击 `tools\app\run.bat`（或者命令行 `python tools/app/server.py`），它会：
1. 起一个**只监听 127.0.0.1** 的本地服务（默认 <http://127.0.0.1:8787>）；
2. 自动打开应用页。

想要"像原生窗口"（无地址栏）：`run.bat --app-window`。
换端口 / 不自动开浏览器：`run.bat --port 8899 --no-open`。

## 打包成可执行文件（一个 exe，双击即用）

```powershell
powershell -ExecutionPolicy Bypass -File tools\app\build-exe.ps1 -OneFile
# → tools\app\dist\206Dash-Workbench.exe   （约 8.8MB，单文件，不需要装 Python）
```

用法（双击也行，双击时自动找仓库、自动开浏览器）：

```
206Dash-Workbench.exe                       # 自动找仓库 + 打开浏览器
206Dash-Workbench.exe --app-window          # 无地址栏窗口（更像原生应用）
206Dash-Workbench.exe --repo <仓库路径>      # 明确指定仓库（找不到仓库时用）
206Dash-Workbench.exe --port 8899 --no-open
```

设计上的三条（都实测过，见 §"验过什么"）：

1. **界面文件内嵌在 exe 里**（`--add-data`）⇒ **没有仓库也能打开界面**；
   仓库只在"构建 / 烧写 / 编辑器 / 预览"这些真需要它的地方才用到。
2. **exe 自己找仓库**：`--repo` > 环境变量 `DSH_206DASH_REPO` > 从 exe 位置往上找
   （标志是同时存在 `platformio.ini` 与 `tools/app`）> 从当前目录往上找 > 本机默认路径。
   找不到会在界面右下角明说"没找到仓库"，不装作正常。
3. **用 `--console` 而不是 `--noconsole`**：日志本身就是这个工具的一部分；
   退出走界面右下角的**「退出应用」**按钮（`POST /api/quit`），不需要去找那个黑窗口。

★ 需要 `pip install pyinstaller`（这台机器上装在项目 venv 里，版本 6.22.3）。
★ `tools/app/dist/206Dash-Workbench.exe` **是仓库里的一份文件**（`.gitignore` 只忽略
  `tools/app/build/` 与 `*.spec`）—— 重打之后按正常流程提交即可。
★ Windows SmartScreen 可能对"没有签名的新 exe"弹一次警告 —— 选"仍要运行"即可。

## 三个屏，能力一项没少

车主只干四件事：① 刷固件 ② 改颜色 ③ 换图片 ④ 看效果。所以顶栏就是三个屏：

| 屏 | 收进来的东西 | 一句话 |
|---|---|---|
| ① **刷固件** | 原「无线刷写」+「开发板状态」 | 三步：选哪块表（**转速表（左）/ 速度表（右）**两张卡片）→ 固件（自动挑最新的那个）→ 一个「开始升级」按钮（上传 + 重启 + 确认成功都在它后面） |
| ② **表盘** | 原「主题配色」+「图片素材」+「预览表盘」 | 二级页签切三块；两个编辑器仍是**同源 iframe**（`tools/theme-editor/*.html` 一行没改），预览仍是 `initPreviewInline()` 那套注入 |
| ③ **工具** | 原「文件与烧写」+「总览」里的零碎 | USB 写素材、读固件文件；「构建新固件」收进「高级」（车主不需要知道 env 是什么） |

技术值一个没删，只是都收进「高级」折叠区：固件来源（本机选文件）、板子地址、板子串口、
板子返回的原始 `/status`（含 `heap_kb` 这类字段）、原始固件列表、原始日志、构建档位（env）。
车主看不懂的词（OTA / 热点 / 槽位 / running / target / env / bin / slave / master）
**只允许出现在「高级」里** —— 这一条由静态闸门看着（见下）。

后端一行没改：所有 `/api/*` 路由与白名单照旧（界面可以藏，能力不许丢）。

## 为什么是"本地服务 + 网页应用"，而不是 Electron / 安装包

- **零安装**：只用 Python 标准库 + 一个浏览器。Electron/Tauri 要 Node/Rust 工具链
  和几百 MB 依赖，而这个仓库的机器上已经有 Python 与 PlatformIO。
- **同源**（关键）：把仓库根目录挂在一个 `http://127.0.0.1` 源上之后，
  四个页面就是**同源**的 ⇒ 应用外壳可以 iframe 收进来、将来也能跨页取数据；
  `file://` 下每个文件是独立源，这些都做不了。
- **安全上下文**：`http://127.0.0.1` 算安全上下文 —— WebSerial / 直连板子热点都需要它。
- **该在后端的在后端**：列串口、跑 esptool、跑 platformio 一律由 `server.py` 执行，
  而且**只认白名单**（env / 分区偏移 / 操作都写死在文件里）；页面传来的字符串只当"键"用。
- 想再"像应用"一点：`--app-window` 就是无地址栏窗口；将来真要做安装包，
  这一层原样保留、外面套个壳即可。

## 后端做了什么（`server.py`）

| 接口 | 作用 | 边界 |
|---|---|---|
| `GET /api/health` | 服务自检（仓库路径 / pio 解释器 / 代理） | 只读 |
| `GET /api/ports` | 列串口（`pio device list`） | 只读 |
| `GET /api/bins` | 列各 env 的 `firmware.bin`（大小/时间/是否 `-ota`） | 只读 |
| `GET /api/bin?env=…` | 把某个 env 的固件吐给页面（OTA 上传用） | env 白名单 |
| `POST /api/upload` | 页面选的文件落盘到 `C:\206dash-scratch\app-upload\`（≤16MB） | 只接单文件 |
| `POST /api/flash` | esptool 写素材分区 | 端口必须 `COM\d+`；分区只允许 `theme/assets/image`；文件必须在仓库或 scratch 内 |
| `POST /api/build` | `platformio run -e <env>` | env 白名单 |
| `POST /api/preview` | 跑 40 秒 `pcpreview` 落帧 | 固定 env |

★ 边界就这些：**没有任意 shell、不能越出仓库、不能指定任意偏移**。
  要加能力就照这张表的风格加 —— 新增的每一行都是一次"要不要开放"的决定。

## 静态闸门

```powershell
node tools/app/check-app.js
```
它钉的是"我在这台机器上点不了浏览器"时最容易漏的几件事：
`app.js` 用到的每个 `#id` 在 `index.html` 里存在、顶栏就是三个屏且每屏有对应 `<section>`、
iframe 的 `src` 文件真实存在、`data-go` 指向存在的屏、用到的 `/api/*` 在 `server.py` 里真有路由、
**32 个能力控件一个都没丢**（构建/烧写/列串口/固件列表/板子状态/确认升级成功/预览/两个编辑器/退出），
以及**三步流程与去术语**（两张板卡 + 一个「开始升级」按钮；「高级」以外不许出现那批术语）。

> 改版前它断言"页签 ≥ 6 个"。七页签收成三屏之后这条必然红 —— 所以是**改成"恰好 3 个屏"并补上
> 上面那几条**，不是把门槛调低。静态检查的覆盖面只许变宽。

## 已知注意

- **编辑器的旧保存看不到**：编辑器用 `localStorage`，而旧入口是 `file://`（另一个源）。
  第一次从新入口打开时，配色/图片页里的"上次保存"会是空的 —— 旧入口仍然可用，
  需要旧数据就从旧入口导出一次再导进来。
- **无线刷写要求板子在 OTA 模式**：那是板子自己起的热点，你连上它之后本应用才能
  `POST http://192.168.4.1/update`；板子自带的上传页（手机浏览器开 `192.168.4.1`）
  仍然是备份路径。
- **构建/烧写会占用串口**：别同时开着别的串口监视器。
