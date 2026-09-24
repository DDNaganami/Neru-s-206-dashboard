/* ============================================================
 * asset-spec.js —— **素材规格**（唯一权威）+ 简单转换
 *
 * ★ 为什么要有这个文件（车主 2026-09-24 的明确要求）：
 *   在这之前，仓库里只有"内部二进制格式（image.bin）+ 打包脚本"，
 *   **上传端的规格一条都没写下来、也没有任何转换** —— 于是每个人都要
 *   自己去读 image-blob-build.js 或问人，而"做错了"往往**不报错**
 *   （图大了盖住弧、没有 alpha 变成黑边、比例不对被拉扁…全都静默）。
 *
 *   所以这个文件干两件事，而且只有这两件：
 *     ① `SPEC` —— 可接受格式 / 产物格式 / 尺寸上限 / alpha 要求 / 命名与角色映射
 *        / 体积上限，全部**集中在这里**，网页与文档都引用它（文档里的表就是它）。
 *     ② `prepareAsset()` —— 常见格式的 RGBA → 目标槽位尺寸的**自动转换**：
 *        保持长宽比缩小、可选裁掉透明边、以及**人能看懂的拒绝/警告**。
 *
 * ★ 这里的数字**一个都不是新发明的**：分区大小来自分区表（由
 *   image-blob-build.js 读 csv 对账）、画布上限来自 ui_theme.h 的弧几何、
 *   颜色格式来自 image_blob.h —— 本文件只是把它们收成一份**给上传端看的**规格。
 *   ★ 所以 `test-image-blob-build.js` 有一条用例拿这些常量与 ImageBlob 逐条对账：
 *     两边分叉会当场红（"文档里写的上限"与"打包器实际用的上限"不一致，
 *     正是最坑的一类错）。
 *
 * ★ 与浏览器/Node 都能用（UMD，和 image-blob-build.js 同一套写法）：
 *   转换部分是**纯函数**（进出都是 RGBA 数组），所以 Node 里能直接测，
 *   不需要 canvas；页面那边只负责把 <img> 画成 RGBA 再交给这里。
 * ============================================================ */
(function (root, factory) {
  "use strict";
  var api = factory(
    (typeof module === "object" && module && module.exports)
      ? require("./image-blob-build.js") : root.ImageBlob
  );
  if (typeof module === "object" && module && module.exports) module.exports = api;
  root.AssetSpec = api;
})(typeof globalThis !== "undefined" ? globalThis : this, function (ImageBlob) {
  "use strict";

  // ------------------------------------------------------------
  // 一、可接受的输入格式
  //
  // ★ "支持到什么程度"逐条写死，因为三者的**alpha 能力不同**，而 alpha
  //   正是表情图的关键（没有它就会在弧线上压出一块不透明方块）：
  //     PNG  —— 无损 + **完整 alpha 通道**（8 位）⇒ 唯一"天生合格"的输入
  //     WebP —— 也可以带 alpha（无损/有损都行），但**有损模式下边缘会有
  //             半透明脏边**，所以只警告不拒绝
  //     JPEG —— **没有 alpha**：只能当背景用；当表情用会被拒（见 ROLE 的 alpha 要求）
  //   ★ 其余格式（GIF/BMP/TIFF/SVG…）一律拒：SVG 是矢量、要另一套渲染路径；
  //     GIF 的调色板透明在缩放时会碎；BMP/TIFF 体积与位深都不受控。
  // ------------------------------------------------------------
  var FORMATS = {
    png:  { id: "png",  label: "PNG",  ext: [".png"],           alpha: "full",
            note: "无损 + 8 位 alpha，唯一天生合格的输入" },
    webp: { id: "webp", label: "WebP", ext: [".webp"],          alpha: "full",
            note: "可以带 alpha；有损模式下边缘可能有半透明脏边（只警告）" },
    jpg:  { id: "jpg",  label: "JPEG", ext: [".jpg", ".jpeg"],  alpha: "none",
            note: "**没有 alpha**：只能当背景；当表情用会被拒" }
  };
  // MIME → 格式 id。★ 以**文件后缀**为准（浏览器给的 type 在拖拽/粘贴时
  //   经常是空串），两者不一致时以后缀为准并给一条警告。
  var MIME_TO_FORMAT = {
    "image/png": "png", "image/webp": "webp",
    "image/jpeg": "jpg", "image/jpg": "jpg"
  };

  function formatOf(fileName, mime) {
    var n = String(fileName || "").toLowerCase();
    for (var k in FORMATS) {
      var exts = FORMATS[k].ext;
      for (var i = 0; i < exts.length; i++) {
        if (n.length >= exts[i].length && n.slice(-exts[i].length) === exts[i]) {
          return FORMATS[k];
        }
      }
    }
    if (mime && MIME_TO_FORMAT[mime]) return FORMATS[MIME_TO_FORMAT[mime]];
    return null;
  }

  // ------------------------------------------------------------
  // 二、产物格式（说什么就写什么，别只说"内部二进制格式"）
  //
  //   颜色格式：背景 = RGB565（2 字节/像素，不透明没意义）
  //             表情 = RGB565A8（**3 字节/像素**：上半部 565 + 下半部独立的 A8 平面）
  //   ★ RGB565A8 的 3 字节/像素是"含 alpha 平面"的**打包**口径，
  //     不是"每像素 2 字节"—— 用错会把合法镜像算小 1/3 然后拒收（踩过）。
  // ------------------------------------------------------------
  var OUTPUT = {
    container: "image.bin（自定义镜像：1420 字节头 + 每项 44 字节索引 + 紧凑像素）",
    headerBytes: ImageBlob.HEADER_SIZE,
    entryBytes: ImageBlob.ENTRY_SIZE,
    maxCount: ImageBlob.MAX_COUNT,
    nameMaxChars: ImageBlob.NAME_MAX - 1,   // 名字是 24 字节 C 串 ⇒ 最多 23 个字符
    cfByRole: {
      background: ImageBlob.CF.RGB565,
      face:       ImageBlob.CF.RGB565A8
    },
    bytesPerPixelByRole: {
      background: ImageBlob.bytesPerPixel(ImageBlob.CF.RGB565),           // 2
      face:       ImageBlob.packedBytesPerPixel(ImageBlob.CF.RGB565A8)    // 3
    },
    // 亮屏上"图像像素尺寸 = 屏上尺寸"（固件里没有 lv_image_set_scale）
    note: "固件按原尺寸居中绘制（没有缩放）⇒ 素材尺寸就是屏上尺寸"
  };

  // ------------------------------------------------------------
  // 三、角色映射（编号来自 lib/themetool/image_blob.h 的 ImageRole）
  //
  // ★ 这张表**必须与固件逐条一致**：编号错了不会报错，只会"右屏显示左屏的表情"。
  //   `ImageBlob.ROLE` 是 JS 侧的同一份表，打包器与固件由 native 用例对账，
  //   这里再复述一次是给**上传端**（网页/文档）用的，并由测试与 ImageBlob 对账。
  // ------------------------------------------------------------
  var ROLES = [
    { id: "background", role: 1,  key: "Background",   label: "表盘背景（两屏共用）", alpha: "either", sizeByTier: "background" },
    { id: "face_idle",       role: 3,  key: "FaceIdle",       label: "左屏·怠速/常态", alpha: "required", sizeByTier: "face" },
    { id: "face_cruise",     role: 12, key: "FaceCruise",     label: "左屏·巡航",      alpha: "required", sizeByTier: "face" },
    { id: "face_sport",      role: 13, key: "FaceSport",      label: "左屏·运动",      alpha: "required", sizeByTier: "face" },
    { id: "face_high",       role: 21, key: "FaceHigh",       label: "左屏·高转",      alpha: "required", sizeByTier: "face" },
    { id: "face_redline",    role: 4,  key: "FaceRedline",    label: "左屏·红区",      alpha: "required", sizeByTier: "face" },
    { id: "face_idle_r",     role: 6,  key: "FaceIdleR",      label: "右屏·静止",      alpha: "required", sizeByTier: "face" },
    { id: "face_city_r",     role: 22, key: "FaceCityR",      label: "右屏·市区",      alpha: "required", sizeByTier: "face" },
    { id: "face_cruise_r",   role: 17, key: "FaceCruiseR",    label: "右屏·快速路",    alpha: "required", sizeByTier: "face" },
    { id: "face_sport_r",    role: 18, key: "FaceSportR",     label: "右屏·高速",      alpha: "required", sizeByTier: "face" },
    { id: "face_overspeed_r",role: 8,  key: "FaceOverspeedR", label: "右屏·超速",      alpha: "required", sizeByTier: "face" }
  ];
  // 保留编号：**一律不复用**（复用会让别人已导出的 image.bin 静默换脸）
  var RESERVED_ROLES = [2, 5, 7, 9, 10, 11, 14, 15, 16, 19, 20];
  var NEXT_FREE_ROLE = 23;   // 新角色从这里接（21/22 已被"高转/市区"用掉）

  // 建议文件名（只是**建议**：名字只进索引表的 name 字段，固件按 role 取图）
  var SUGGESTED_NAME = {
    background: "bg", face_idle: "L_idle", face_cruise: "L_cruise",
    face_sport: "L_sport", face_high: "L_high", face_redline: "L_redline",
    face_idle_r: "R_idle", face_city_r: "R_city", face_cruise_r: "R_cruise",
    face_sport_r: "R_sport", face_overspeed_r: "R_overspeed"
  };
  function suggestedName(roleId) {
    return SUGGESTED_NAME[roleId] || ("asset_" + roleId);
  }

  function roleById(id) {
    for (var i = 0; i < ROLES.length; i++) if (ROLES[i].id === id) return ROLES[i];
    return null;
  }
  function roleByNumber(n) {
    for (var i = 0; i < ROLES.length; i++) if (ROLES[i].role === n) return ROLES[i];
    return null;
  }

  // ------------------------------------------------------------
  // 四、尺寸口径（**按分辨率档**）
  //
  //   ★ 三个不同的数，别混（这是这套规格里最容易搞错的地方）：
  //     · 分区上限   —— "装不装得下"（跟着**板子**走：1MB / 8MB）
  //     · 画布上限   —— "表情的四角会不会伸进内圈副弧"（跟着**屏**走：480 档 320 / 240 档 160）
  //     · 推荐尺寸   —— 画质与空间的甜点（480 档 300 / 240 档 152）
  //
  //   ★★ 2026-09-24 层序定稿之后的注明（**口径变了，数字没变**）：
  //     层序是「背景 → 表情 → **弧** → 灯 → 读数」= **弧在表情之上**
  //     （车主原话："表盘弧应该永远是最上层"）。所以表情图伸进弧带的部分
  //     **会被弧裁掉** —— **设计如此，不是 bug**。
  //     ⇒ 上面这三个数**一个都没放宽**：弧压在上面不是"可以把画布做大"的理由，
  //       做大了只会让四个角被弧切掉（480 档：方块画布的对角线到半径 226，
  //       而主弧带在半径 181..205 ⇒ 四角必然被咬掉一块）。
  //     ⇒ 这份规格里"会不会盖住弧"的说法一律改成"会不会**被弧裁到**"。
  //
  //   ★ 背景另有口径：必须**铺满整屏**（480 档 480×480、240 档 240×240），
  //     否则弧带那一圈会露出主题底色 —— 这条在 pcpreview 里逐像素验过。
  //
  //   ★ 圆形可视区约束：两块屏都是**圆屏**，能看见的只有内切圆。
  //     480 档：D=480 ⇒ 内切正方形边长 floor(480/√2) = **339** → 取 **336**（4 的倍数）
  //     240 档：D=240 ⇒ floor(240/√2) = 169 → 取 **168**
  //     ⇒ 素材（矩形内容）**四角必须落在内切圆内**。表情的画布上限
  //       （320/160）本来就在这个数以内，所以正常做图不会撞上；
  //       但"背景/自定义槽位 + 手动改大尺寸"时它会兜住。
  // ------------------------------------------------------------
  function circleSafeSide(displayRes) {
    var s = Math.floor(displayRes / Math.SQRT2);       // 内切正方形边长
    return Math.floor(s / 4) * 4;                      // 向下对齐到 4 的倍数
  }

  // ------------------------------------------------------------
  // 四之二、**两块圆屏的物理口径**（2026-09-24 新增）
  //
  // ★ 为什么要有这张表：`image-editor.html` 会把"这块屏是圆的"讲给用户，
  //   而那句话里的数字（可视直径 / 像素↔毫米）**只能有一份**。
  //   写进页面 HTML 就成了第三份（固件 panel_view.h、本文件、页面各一份），
  //   所以页面从 `AssetSpec.ROUND_PANEL` 取，`test-asset-spec.js` 逐条钉住，
  //   而 native 侧（`lib/dashcore/panel_view.h` 的 panelInscribedSquareSide）
  //   用**同一套算式**算内切正方形 —— 两条链在 336 / 168 上对齐。
  //
  // ★ 数字的出处（**别自己发明**）：
  //   · 2.8C 有效区 Ø70.13 mm —— `PURCHASE.md` 第六节表格「Ø 有效区」那一列
  //     （Dwin / 微雪 2.8C / 鑫洪泰 / Wisecoco 四家同数，70.128~70.13）。
  //   · DualEye 1.28" 那块没有独立量过有效区，所以这一栏**留空**——
  //     不编一个数出来（它的**像素**口径 168 仍然有效，那是纯几何）。
  // ------------------------------------------------------------
  var ROUND_PANEL = {
    res480: {
      id: "res480",
      tier: "480×480 屏 · 2.8C（最终板）",
      displayRes: 480,
      activeAreaMm10: 7013,             // 70.13 mm（Ø 有效区）
      hint: "2.8C（最终板）：微雪 ESP32-S3-LCD-2.8C，480×480 圆屏 ST7701S，" +
            "可视圆 Ø70.13mm。"
    },
    res240: {
      id: "res240",
      tier: "240×240 屏 · 历史：DualEye（已退货）",
      displayRes: 240,
      activeAreaMm10: null,             // 没量过 ⇒ 不编（见上面那条）
      hint: "历史档：微雪 ESP32-S3-DualEye-Touch-LCD-1.28（两块 240×240 GC9A01A），" +
            "2026-09-22 已退货；档位保留且仍然有效。"
    }
  };

  // 一行"像素 ↔ 毫米"的换算（只在有效区有数时给得出来）。
  // ★ 单位是 **1e-4 mm**（与 C++ 侧 `panelMmPerPx10000` 逐位同口径）：
  //   480 档：70.13mm / 480px = 0.1461 mm/px ⇒ **1461**
  //   （踩过：写成 ×1000 会得到 14610 = 1.46 mm/px，屏在文档里就变成 701 mm。
  //    这个数只用于显示，算错不会崩 —— 所以 test-asset-spec.js 里钉着它，
  //    而且它与 lib/dashcore/panel_view.h 的断言是同一个数。）
  function mmPerPixelX10000(panel) {
    if (!panel || !panel.activeAreaMm10) return null;
    // activeAreaMm10 的单位是 1e-2 mm ⇒ ×100 抬到 1e-4 mm，再除以像素数。
    return Math.round(panel.activeAreaMm10 * 100 / panel.displayRes);
  }

  function tierOf(targetId) {
    var t = ImageBlob.faceTierInfo(targetId);
    var is240 = t.id === "res240";
    var res = is240 ? 240 : 480;
    return {
      id: t.id,
      label: t.label,                                  // ← 2.8C（最终板）/ 历史：DualEye
      displayRes: res,
      faceCanvasMax: t.faceCanvasMax,                  // 320 / 160
      faceRecommended: t.faceSizeRecommended,          // 300 / 152
      faceInnerMostRadius: t.faceInnerMostRadius,      // 163 / 81
      backgroundSide: res,
      circleSafe: circleSafeSide(res),                 // 336 / 168
      // 这块屏的物理口径（2.8C 有 Ø70.13；DualEye 那一档是 null —— 没量过）
      panel: ROUND_PANEL[is240 ? "res240" : "res480"]
    };
  }

  // 某个角色在某个目标板上的"框"（上限 + 推荐 + 该不该有 alpha）
  function slotBox(roleId, targetId) {
    var r = roleById(roleId);
    if (!r) return null;
    var tier = tierOf(targetId);
    if (r.sizeByTier === "background") {
      return { w: tier.backgroundSide, h: tier.backgroundSide,
               max: tier.backgroundSide, recommended: tier.backgroundSide,
               alpha: r.alpha, face: false, tier: tier, role: r };
    }
    return { w: tier.faceRecommended, h: tier.faceRecommended,
             max: tier.faceCanvasMax, recommended: tier.faceRecommended,
             alpha: r.alpha, face: true, tier: tier, role: r };
  }

  // ------------------------------------------------------------
  // 五、体积上限（分区口径，来自两份分区表 —— 由 ImageBlob 读 csv 对账）
  //   ★ 单张没有独立上限：真正的闸门是"**整个镜像** ≤ image 分区"，
  //     由打包器在导出前算（超了直接拒，不会生成一个会被截断的 bin）。
  //     这里给出的是"官方推荐的整套形态"占多少，给用户一个量级感。
  // ------------------------------------------------------------
  function budget(targetId) {
    var tier = tierOf(targetId);
    var bytes = ImageBlob.partitionBytesFor(targetId);
    // 推荐形态：满屏背景 + 两屏各 5 张表情（10 张）
    var bg = tier.backgroundSide * tier.backgroundSide *
             OUTPUT.bytesPerPixelByRole.background;
    var oneFace = tier.faceRecommended * tier.faceRecommended *
                  OUTPUT.bytesPerPixelByRole.face;
    return {
      partitionBytes: bytes,
      headerBytes: OUTPUT.headerBytes,
      backgroundBytes: bg,
      faceBytes: oneFace,
      fullSetBytes: OUTPUT.headerBytes + bg + 10 * oneFace,
      desc: tier.displayRes === 240
        ? "满屏 240 背景 + 10 张 152 表情"
        : "满屏 480 背景 + 10 张 " + tier.faceRecommended + " 表情"
    };
  }

  // ------------------------------------------------------------
  // 六、简单转换：RGBA → 目标槽位（保持长宽比 + 可选裁透明边 + 人话警告）
  //
  // 输入输出都是**纯数据**（RGBA 数组），所以 Node 里能直接测。
  // opts:
  //   roleId     角色 id（决定颜色格式与 alpha 要求）
  //   targetId   目标板（决定框的大小）
  //   maxSide    可选：手工指定的上限（页面里"目标宽度"输入框）
  //   trim       true = 先裁掉四周的透明边
  //   trimAlpha  视为"透明"的阈值（默认 8）
  //   bgColor    [r,g,b]：**不透明**角色（背景）的铺底色（默认主题底色 0x141414）
  //
  // 返回：
  //   { w, h, rgba, cf, warnings[], errors[], info{} }
  //   ★ `errors` 非空 ⇒ **必须拒绝**（上层不要导出）；`warnings` 只是提醒。
  // ------------------------------------------------------------
  function prepareAsset(src, sw, sh, opts) {
    opts = opts || {};
    var box = slotBox(opts.roleId, opts.targetId);
    var out = { w: 0, h: 0, rgba: null, cf: null, warnings: [], errors: [], info: {} };
    if (!box) { out.errors.push("不认识的用途：" + opts.roleId); return out; }
    if (!src || !sw || !sh) { out.errors.push("这张图读不出像素（尺寸为 0）"); return out; }

    var wantAlpha = (box.alpha === "required");
    var maxSide = Math.min(box.max, opts.maxSide ? (opts.maxSide | 0) : box.max);
    out.info.srcW = sw; out.info.srcH = sh;
    out.info.maxSide = maxSide;

    // ---- ① 透明边裁剪（可选）----
    var sx = 0, sy = 0, cw = sw, ch = sh;
    if (opts.trim) {
      var thr = opts.trimAlpha === undefined ? 8 : opts.trimAlpha;
      var minX = sw, minY = sh, maxX = -1, maxY = -1;
      for (var y = 0; y < sh; y++) {
        for (var x = 0; x < sw; x++) {
          if (src[(y * sw + x) * 4 + 3] > thr) {
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
          }
        }
      }
      if (maxX < 0) {
        // 整张全透明 ⇒ 这是错误，不是"裁没了"
        out.errors.push("整张图都是透明的（裁透明边之后什么都不剩）—— 检查一下 alpha 通道");
        return out;
      }
      sx = minX; sy = minY; cw = maxX - minX + 1; ch = maxY - minY + 1;
      out.info.trimmedFrom = { x: minX, y: minY, w: cw, h: ch };
      if (cw !== sw || ch !== sh) {
        out.warnings.push("已裁掉透明边：" + sw + "×" + sh + " → " + cw + "×" + ch +
                          "（省下的空间不算多，但能让'按内容居中'更准）");
      }
    }

    // ---- ② 目标尺寸：**保持长宽比**，contain（只缩不放）----
    //   ★ 为什么"只缩不放"：把一张 100×100 的图放大到 300×300 只会变糊，
    //     而它在屏上占的地方一点没少 —— 这套口径与页面原来的
    //     pickDefaultSize 一致（那次踩过：默认放大把一整套 152 的图变成 132% 溢出）。
    var scale = Math.min(maxSide / cw, maxSide / ch, 1);
    var tw = Math.max(1, Math.round(cw * scale));
    var th = Math.max(1, Math.round(ch * scale));
    out.w = tw; out.h = th;
    out.info.scale = scale;

    if (scale > 1 - 1e-9 && (cw > maxSide || ch > maxSide)) {
      // 理论上到不了（scale 已钳到 1），留个自检
      out.warnings.push("尺寸超过上限但未缩小（这是个 bug，请报）");
    }
    if (opts.maxSide && maxSide < Math.min(cw, ch)) {
      out.warnings.push("为放进 " + maxSide + " 的框，缩到了 " + tw + "×" + th);
    }

    // ---- ③ alpha 要求 ----
    var hasAlpha = false;
    if (wantAlpha) {
      for (var i = 3; i < src.length; i += 4) {
        if (src[i] < 250) { hasAlpha = true; break; }
      }
      if (!hasAlpha) {
        // ★ 这里刻意**只警告不拒绝**：一张全不透明的表情图在技术上是合法的
        //   （它就是不透明方块），只是会把**底下的背景图**挡掉一整块
        //   （表情在"背景之上、弧之下" ⇒ 它挡不住弧，但挡得住背景）。
        //   拒绝它会把"我就想铺满"的人挡在门外；而警告能说清后果。
        out.warnings.push("这个用途**必须带 alpha**（表情要透出底下的背景图），" +
                          "但这张图整张都不透明 ⇒ 屏上会盖住底下的背景图一整块" +
                          "（弧仍然压在它上面，所以被咬掉的是它的四角）。" +
                          "请用带透明背景的 PNG/WebP 重做，或改用 JPEG 做背景。");
      }
    } else {
      // 背景：透明没有意义（铺在最底层），但半透明像素会与底色合成 ——
      // ★ 用**黑边检验**：很多人做过"透明背景的 JPEG/PNG 背景图"，直接铺会在
      //   边缘留一圈黑。这里明确提示会怎么处理。
      for (var j = 3; j < src.length; j += 4) {
        if (src[j] < 250) {
          out.warnings.push("背景图里有半透明像素：会与你设的底色合成" +
                            "（半透明的边缘常来自抠图，容易留一圈脏边）。");
          break;
        }
      }
    }

    // ---- ④ 缩放采样（盒式平均；只在缩小时用，放大直接取最近邻）----
    var dst = new Uint8Array(tw * th * 4);
    var bgc = opts.bgColor || [0x14, 0x14, 0x14];
    for (var oy = 0; oy < th; oy++) {
      for (var ox = 0; ox < tw; ox++) {
        var x0 = sx + Math.floor(ox * cw / tw), x1 = sx + Math.max(x0 + 1 - sx, Math.ceil((ox + 1) * cw / tw));
        var y0 = sy + Math.floor(oy * ch / th), y1 = sy + Math.max(y0 + 1 - sy, Math.ceil((oy + 1) * ch / th));
        if (x1 > sx + cw) x1 = sx + cw;
        if (y1 > sy + ch) y1 = sy + ch;
        var r = 0, g = 0, b = 0, a = 0, n = 0;
        for (var yy = y0; yy < y1; yy++) {
          for (var xx = x0; xx < x1; xx++) {
            var p = (yy * sw + xx) * 4;
            // ★ 按 alpha **加权**求平均：不然透明像素的 RGB（通常是黑或白）
            //   会把边缘染黑/染白 —— 这正是"缩放后一圈脏边"的成因。
            var al = src[p + 3] / 255;
            r += src[p] * al; g += src[p + 1] * al; b += src[p + 2] * al;
            a += src[p + 3]; n++;
          }
        }
        var q = (oy * tw + ox) * 4;
        var aw = a / 255;                       // alpha 权重之和
        dst[q]     = aw > 0 ? Math.round(r / aw) : 0;
        dst[q + 1] = aw > 0 ? Math.round(g / aw) : 0;
        dst[q + 2] = aw > 0 ? Math.round(b / aw) : 0;
        dst[q + 3] = n ? Math.round(a / n) : 0;
        if (!wantAlpha && dst[q + 3] < 255) {
          // 不透明角色：在这里就把 alpha 合成掉（固件侧那个角色用 RGB565，没有 alpha）
          var k = dst[q + 3] / 255;
          dst[q]     = Math.round(dst[q] * k + bgc[0] * (1 - k));
          dst[q + 1] = Math.round(dst[q + 1] * k + bgc[1] * (1 - k));
          dst[q + 2] = Math.round(dst[q + 2] * k + bgc[2] * (1 - k));
          dst[q + 3] = 255;
        }
      }
    }
    out.rgba = dst;
    out.cf = wantAlpha ? OUTPUT.cfByRole.face : OUTPUT.cfByRole.background;

    // ---- ⑤ 圆形可视区 ----
    //   四角到中心的距离必须 ≤ 半径（否则圆屏上被削掉）。
    var diag = Math.sqrt(tw * tw + th * th) / 2;
    var radius = box.tier.displayRes / 2;
    if (diag > radius) {
      out.warnings.push("这张 " + tw + "×" + th + " 的内容四角会超出一块 " +
                        box.tier.displayRes + " 圆屏的可视圆（对角线一半 " +
                        diag.toFixed(0) + " > 半径 " + radius +
                        "）⇒ 建议缩到 " + box.tier.circleSafe + " 以内，或把内容做在圆内。");
    }
    out.info.bytes = tw * th * (wantAlpha ? 3 : 2);
    return out;
  }

  // ------------------------------------------------------------
  // 七、上传前的整份检查（文件级：格式认不认、能不能用在这个角色上）
  //   返回 { ok, errors[], warnings[] } —— errors 非空就不该放进列表。
  //
  //   ★ 用途的**两种写法都收**（2026-09-24 修，这是一次真实故障的根因）：
  //     · **用途名** —— "background" / "face_idle"（本文件、文档、命令行用的就是名字）
  //     · **用途编号** —— 1 / 3 / 12（`ImageBlob.ROLE.Background` 这种；
  //       **页面里 addFiles() 拿到的就是数字**）
  //   以前这里只认名字，而页面递进来的是编号 ⇒ 恒判 "不认识的用途：1"，
  //   返回的又是 ok:false ⇒ **任何图片都进不了列表**。所以这一层归一化是必须的：
  //   谁传错都只会拿到一条看得懂的错，不会再静默失效。
  //   两边那份编号表由 test-asset-spec.js 钉住（"编号 → 用途"必须与
  //   `ImageBlob.ROLE` 逐个对得上；这条以前**没有人钉过**，正是根因）。
  // ------------------------------------------------------------
  function checkUpload(fileName, mime, roleId, targetId) {
    var res = { ok: true, errors: [], warnings: [], format: null };
    var f = formatOf(fileName, mime);
    if (!f) {
      res.ok = false;
      res.errors.push("不认识的格式：" + fileName +
                      "。只接受 PNG / JPEG / WebP" +
                      "（PNG 最好：无损 + 带 alpha）。SVG/GIF/BMP/TIFF 请先转成 PNG。");
      return res;
    }
    res.format = f.id;
    // 编号 → 名字。认不出来的编号**原样**留着，交给下面的 slotBox 报出来
    // （报错里要显示调用方真正传的那个值，不然没法按提示去查）。
    var roleKey = roleId;
    if (typeof roleId === "number") {
      var byNum = roleByNumber(roleId);
      roleKey = byNum ? byNum.id : roleId;
    }
    var box = slotBox(roleKey, targetId);
    if (!box) {
      res.ok = false;
      res.errors.push("不认识的用途：" + roleId +
                      "。用途要给**名字**（如 \"background\" / \"face_idle_r\"）" +
                      "或者给**编号**（如 1 / 3 / 12 —— 那是 `ImageBlob.ROLE` 里的数）。");
      return res;
    }
    if (box.alpha === "required" && f.alpha === "none") {
      res.ok = false;
      res.errors.push(f.label + " **没有 alpha 通道**，而这个用途（" +
                      box.role.label + "）必须带 alpha —— 表情要透出底下的背景图，" +
                      "不透明的方图会把背景盖掉一整块" +
                      "（**弧压在表情上面**，所以它盖不住弧）。" +
                      "请改用带透明背景的 PNG。");
    }
    if (box.alpha === "required" && f.id === "webp") {
      res.warnings.push("WebP 可以带 alpha，但有损模式的边缘会有半透明脏边；" +
                        "做表情图建议用 PNG。");
    }
    return res;
  }

  return {
    FORMATS: FORMATS, MIME_TO_FORMAT: MIME_TO_FORMAT,
    OUTPUT: OUTPUT, ROLES: ROLES, RESERVED_ROLES: RESERVED_ROLES,
    NEXT_FREE_ROLE: NEXT_FREE_ROLE, SUGGESTED_NAME: SUGGESTED_NAME,
    formatOf: formatOf, roleById: roleById, roleByNumber: roleByNumber,
    suggestedName: suggestedName,
    circleSafeSide: circleSafeSide, tierOf: tierOf, slotBox: slotBox,
    // 两块圆屏的物理口径（页面取它来显示"这块屏是圆的"，见上面那段）
    ROUND_PANEL: ROUND_PANEL, mmPerPixelX10000: mmPerPixelX10000,
    budget: budget, prepareAsset: prepareAsset, checkUpload: checkUpload
  };
});
