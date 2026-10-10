# 206Dash 开发台（`tools/app/`）—— 一个窗口里把"设计 → 构建 → 烧写 → 预览"做完

> 2026-09-29 新增。车主："把之前的网页端独立成一个应用程序。"
> 它把原来散在四个页面 + 一堆命令行里的活收进**一个本地应用**：
> **① 无线刷写开发板 ② 主题配色 ③ 图片素材 ④ 导出/读取 bin ⑤ 预览表盘 ⑥ 预留开发功能**。

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
★ exe **默认不进仓库**（`.gitignore` 里忽略了 `tools/app/dist/`）：它是构建产物，
  换一台机器重打一次更干净。真要同步到笔记本再双击：
  `git add -f tools/app/dist/206Dash-Workbench.exe`。
★ Windows SmartScreen 可能对"没有签名的新 exe"弹一次警告 —— 选"仍要运行"即可。

## 六项能力对应到哪里

| 能力 | 在哪 | 说明 |
|---|---|---|
| ① **WiFi OTA 开发板** | 「无线刷写」页 | 电脑 WiFi 切到板子的热点 → 选固件 → 上传。**前提**：板子刷的是 `-ota` 档并已进 OTA 模式（串口敲 `u`）。见 `docs/OTA.md` |
| ② **主题颜色设计** | 「主题配色」页 | 原来的 `tools/theme-editor/index.html` 原样收进来（同源 iframe），功能一行没改 |
| ③ **图片上传/编辑** | 「图片素材」页 | 同上，`image-editor.html`：导入图片、生成 `image.bin` / `assets.bin` |
| ④ **导出/读取 bin** | 「文件与烧写」页 | 读本地 `assets.bin` / `theme.json` / `image.bin`（大小 + 头部十六进制），并用 USB 写进对应分区 |
| ⑤ **预览表盘** | 「预览表盘」页 | 一键跑宿主机预览（`pcpreview`，约 40 秒落帧），然后在这里看 |
| ⑥ **预留开发功能** | 「总览」页 + `app.js` 顶部 `TABS` | 加一个页签 = 加一条 `TABS` 记录 + 一个 `<section id="tab-<id>">`；后端加动作按 `server.py` 里那套白名单写法 |

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
`app.js` 用到的每个 `#id` 在 `index.html` 里存在、每个页签有对应 `<section>`、
iframe 的 `src` 文件真实存在、`data-go` 指向存在的页签、用到的 `/api/*` 在 `server.py` 里真有路由。

## 已知注意

- **编辑器的旧保存看不到**：编辑器用 `localStorage`，而旧入口是 `file://`（另一个源）。
  第一次从新入口打开时，配色/图片页里的"上次保存"会是空的 —— 旧入口仍然可用，
  需要旧数据就从旧入口导出一次再导进来。
- **无线刷写要求板子在 OTA 模式**：那是板子自己起的热点，你连上它之后本应用才能
  `POST http://192.168.4.1/update`；板子自带的上传页（手机浏览器开 `192.168.4.1`）
  仍然是备份路径。
- **构建/烧写会占用串口**：别同时开着别的串口监视器。
