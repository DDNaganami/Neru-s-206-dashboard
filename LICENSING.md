# 授权分层（本文件不修改 MIT 正文）

本仓库按内容分三层授权。**本文件只是范围说明，不改动任何许可证正文**，也不缩减或扩张
根目录 `LICENSE` 里那份 MIT 全文赋予的权利；MIT 的权利与义务**一律以根目录 `LICENSE` 的
纯正文为准**（该文件不含版权行之外的任何附加说明）。

> 根目录 `LICENSE` 只放标准 MIT 全文 + 一行版权声明 `Copyright (c) 2026 DDNaganami`，
> 目的是让自动化许可证识别（GitHub 的 Licensee / SPDX 指纹匹配）能直接认出 **MIT**；
> 分层范围因此挪到本文件（原先是接在 MIT 正文后面的一段说明）。

## 分层一览

| 层 | 覆盖什么 | 适用许可证 | 详见 |
|---|---|---|---|
| 代码 / 接线表 | `src/` / `lib/` / `include/` / `test/` / `preview/` / `tools/` 下的源码，构建配置（`platformio.ini`、`partitions*.csv`），以及其中的接线 / 引脚 / 分区等事实性表格 | **MIT** | 根目录 `LICENSE`（纯 MIT 全文） |
| 文档 | 全部 `*.md`（含 `README.md`、`VAN-PROTOCOL.md` 等）的**行文与表达** | **CC BY 4.0** | `LICENSE-DOCS` |
| 美术素材与图片 / 二进制 | 图片、图标、位图、字体、音频，以及本仓库随附的默认主题数据等**外观设计载体** | **保留所有权利**（单独声明，**不在** MIT 也不在 CC BY 4.0 覆盖范围内） | `LICENSE-ARTWORK` |

## 逐层说明

1. **代码与接线 / 引脚表等 —— MIT。** 即根目录 `LICENSE` 的全文：固件与工具源码
   （`src/`、`lib/`、`include/`、`test/`、`preview/`、`tools/`）、构建配置（`platformio.ini`、
   `partitions*.csv`），以及其中的接线 / 引脚 / 分区等**事实性表格**。文档里的事实性表格
   （数据本身）同样按 MIT 使用；文档的行文与表达部分按下一层的 CC BY 4.0 使用。
2. **文档（`*.md`）—— CC BY 4.0。** 以 **Creative Commons Attribution 4.0 International**
   提供；许可全文、适用文件清单与署名要求见 `LICENSE-DOCS`。
3. **美术素材与图片 / 二进制 —— 单独声明，保留所有权利。** 既不在 MIT、也不在 CC BY 4.0
   覆盖范围内；授权口径、范围清单与「运行本仓库构建产物所必需的使用」这一项例外见
   `LICENSE-ARTWORK`。

**第三方**内容（协议 / 技术资料、硬件资料、软件依赖及其字体）不属于本项目，各自保留其
原始条款，登记见 `THIRD-PARTY.md`；本项目对它们的引用与实测复现不改变其归属。

## 三个子文件

`LICENSE-DOCS`、`LICENSE-ARTWORK`、`THIRD-PARTY.md` 各自在开头写明自己的适用范围；本文件
（`LICENSING.md`）属于文档层，与其它 `*.md` 一样按 `LICENSE-DOCS` 的 CC BY 4.0 提供。
