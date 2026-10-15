/* ============================================================
 * check-webp.js —— WebP 上传的三种情形（车主 2026-10-15 给了 .webp 素材）
 *
 *   node tools/theme-editor/check-webp.js
 *   退出码：0 = 三种情形都符合口径；1 = 有红；
 *           3 = 本机没有可用的无头浏览器（**跳过**，不是失败）
 *
 * ------------------------------------------------------------
 * 车主口径（逐条）：
 *   ① 背景槽：WebP 允许（可用 JPEG 同理），产出走既有 cover 取景路径；
 *   ② 表情槽：WebP **也允许**，但**必须真的有 alpha** —— 解码后读像素检查；
 *      全不透明 ⇒ 按现有规则拒绝，并给「表情要透明背景，请用 PNG」这类中文提示；
 *   ③ **不改任何容器格式**：WebP 只出现在"进槽前的解码"这一步。
 *
 * ------------------------------------------------------------
 * 为什么这一份测试要起一个真浏览器
 * ------------------------------------------------------------
 * "WebP 能不能解、解出来有没有 alpha"是**浏览器的行为**，Node 里没有解码器
 * （而且按红线也不许引第三方库）。所以这里：
 *   · 判据（收不收、报什么错）用 Node 直接测 —— `asset-spec.checkUpload()` 是权威；
 *   · **解码那一段**用无头 Chrome 跑页面自己的 `sendFilesToSlot()` 实测
 *     （夹具是两张极小 WebP：2×2 全不透明 86 字节、2×2 带透明 76 字节，
 *       **内联在本文件里**，不往仓库塞图片、也不碰 theme.json / local/）。
 *
 * ★ 夹具怎么来的：Chrome 的 canvas.toDataURL("image/webp") 会带一段 456 字节的
 *   ICC 描述，把它剥掉（ICCP chunk）再把 RIFF 的长度字段改对，就得到这两个
 *   几十字节的合法 WebP。**本文件自带**这两串 base64，所以离线可复跑。
 * ============================================================ */
"use strict";

const fs = require("fs");
const os = require("os");
const path = require("path");
const cp = require("child_process");

const dir = __dirname;
const AS = require(path.join(dir, "asset-spec.js"));

let pass = 0, fail = 0;
const failures = [];
function ok(cond, what) {
  if (cond) pass++;
  else { fail++; failures.push(what); console.log("  ✗ " + what); }
}
function eq(a, b, what) {
  ok(a === b, what + "  (期望 " + JSON.stringify(b) + "，得到 " + JSON.stringify(a) + ")");
}
function section(t) { console.log("\n== " + t); }

// ------------------------------------------------------------
// 极小 WebP 夹具（内联，几十字节）
// ------------------------------------------------------------
const FIX = {
  // 2×2 全不透明（每个像素 alpha=255）
  opaque2x2: "UklGRk4AAABXRUJQVlA4WAoAAAAgAAAAAQAAAQAAVlA4IDAAAADQAQCdASoCAAIAAUAm" +
             "JaACdLoB+AADsAD+8ut//NgVzXPv9//S4P0uD9Lg/9KQAAA=",
  // 2×2 带透明（只有左上 1 个像素不透明，其余 alpha=0）
  alpha2x2: "UklGRkQAAABXRUJQVlA4WAoAAAAwAAAAAQAAAQAAQUxQSAUAAAAA/wAAAABWUDggGAAAADAB" +
            "AJ0BKgIAAgABQCYlpAADcAD+/TZoAA=="
};

// ------------------------------------------------------------
section("① 判据（Node 侧）：WebP 在背景/表情两种槽位下的收与拒");
const R = {
  bg: AS.checkUpload("bg.webp", "image/webp", "background", "s3"),
  face0: AS.checkUpload("face.webp", "image/webp", "face_idle", "s3"),
  faceOpaque: AS.checkUpload("face.webp", "image/webp", "face_idle", "s3",
                             { alpha: { has: false, min: 255, max: 255 } }),
  faceAlpha: AS.checkUpload("face.webp", "image/webp", "face_idle", "s3",
                            { alpha: { has: true, min: 0, max: 255 } }),
  bgOpaque: AS.checkUpload("bg.webp", "image/webp", "background", "s3",
                           { alpha: { has: false, min: 255, max: 255 } }),
  jpgFace: AS.checkUpload("f.jpg", "image/jpeg", "face_idle", "s3"),
  jpgBg: AS.checkUpload("b.jpg", "image/jpeg", "background", "s3"),
  legacy: AS.checkUpload("x.webp", "image/webp", "face_idle", "s3")
};
{
  // 背景 + WebP（不传像素信息）= 收
  eq(R.bg.ok, true, "背景槽收 WebP");
  eq(R.bg.errors.length, 0, "背景槽的 WebP 没有 error");
  eq(R.bg.format, "webp", "认得出来是 webp");

  // 表情 + WebP（不传像素信息）= 收（只提醒有损脏边）—— 与既有口径一致
  eq(R.face0.ok, true, "表情槽收 WebP（格式级先放行）");
  ok(R.face0.warnings.length > 0, "表情槽的 WebP 有一条提醒（有损边缘脏边）");

  // ★ 表情 + WebP 且**像素全不透明** = **拒**（车主说的"必须真的有 alpha"）
  eq(R.faceOpaque.ok, false, "全不透明的 WebP 当表情被拒");
  ok(R.faceOpaque.errors.length > 0, "有拒绝理由");
  ok(/不透明|alpha/i.test(R.faceOpaque.errors[0]), "理由里点明「像素都是不透明的」");
  ok(/PNG/.test(R.faceOpaque.errors[0]), "理由里给出路（请用 PNG）");
  ok(/WebP/.test(R.faceOpaque.errors[0]), "理由里也提到带透明的 WebP");

  // ★ 表情 + WebP 且**真的有透明像素** = 收
  eq(R.faceAlpha.ok, true, "带透明的 WebP 当表情通过");

  // 背景 + 全不透明的 WebP：背景本来就不要求 alpha ⇒ 照收
  eq(R.bgOpaque.ok, true, "全不透明的 WebP 当背景照收（背景不要求 alpha）");

  // JPEG 的老口径没被改坏：表情拒、背景收
  eq(R.jpgFace.ok, false, "JPEG 当表情仍然被拒（老口径没动）");
  eq(R.jpgBg.ok, true, "JPEG 当背景仍然通过");

  // ★ 不传 opts 时行为与改动前**一模一样**（老调用点不受影响）
  eq(JSON.stringify(R.legacy.errors), "[]", "不传 opts 时不产生新的 error（向后兼容）");
}

// ------------------------------------------------------------
section("② 页面里的判据接线（真源码，不是抄一份）");
{
  const PAGE = fs.readFileSync(path.join(dir, "index.html"), "utf8");
  ok(/function measureAlpha\(/.test(PAGE), "页面里有 measureAlpha()（解码后读像素量 alpha）");
  ok(/anyTransparent/.test(PAGE), "量的是「有没有 alpha < 255 的像素」");
  ok(/AssetSpec\.checkUpload\([^)]*alpha:\s*\{\s*has:/.test(PAGE),
     "把量到的 alpha 交给 asset-spec 的**同一份**判据（页面不自己判）");
  ok(/openCropper\(img, f\.name, spec, mode\)/.test(PAGE),
     "通过后才开框选器（背景=取景、表情=框选）");
  // 红线：不许自己写 WebP 编解码
  ok(!/RIFF|VP8 |WEBP_|decodeWebp|webpDecode/i.test(PAGE),
     "页面里没有自己写 WebP 编解码（交给浏览器 new Image()）");
  ok(/new Image\(\)/.test(PAGE), "用的是浏览器解码（new Image()）");
}

// ------------------------------------------------------------
section("③ 真浏览器：页面的 sendFilesToSlot() 走一遍三种情形");
{
  const chrome = ["C:/Program Files/Google/Chrome/Application/chrome.exe",
                  "C:/Program Files (x86)/Google/Chrome/Application/chrome.exe",
                  "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
                  "C:/Program Files/Microsoft/Edge/Application/msedge.exe"]
    .filter(p => { try { return fs.statSync(p).isFile(); } catch (e) { return false; } });
  if (!chrome.length) {
    console.log("  - 本机找不到 Chrome/Edge：③ 这一节**跳过**（①② 已覆盖判据与接线）");
    console.log("\n" + "=".repeat(56));
    console.log("全部通过:" + pass + " 项断言（③ 跳过，退出码 3）");
    console.log("=".repeat(56));
    process.exit(3);
  }

  // 做法：把 index.html 复制到临时目录（脚本 src 换成绝对路径），在末尾注入
  // 一段探针 —— 探针调**页面自己的** sendFilesToSlot()，喂两个假 File 进去
  // （和拖文件走的是同一条函数），把结果写进 document.body 供 --dump-dom 读回。
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "dsh-webp-"));
  let html = fs.readFileSync(path.join(dir, "index.html"), "utf8");
  html = html.replace(/(<script[^>]*\bsrc=")([^"]+)(")/gi, (all, a, src, b) =>
    (/^https?:/i.test(src) ? all : a + "file:///" + path.join(dir, src).replace(/\\/g, "/") + b));
  const probe = `
<script>
(function () {
  var FIX = ${JSON.stringify(FIX)};
  function fakeFile(name, b64) {
    var bin = atob(b64);
    var bytes = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    return new File([bytes], name, { type: "image/webp" });
  }
  window.addEventListener("load", function () {
    var scenes = [
      { id: "bg",         face: false, fix: "opaque2x2" },
      { id: "opaqueFace", face: true,  fix: "opaque2x2" },
      { id: "alphaFace",  face: true,  fix: "alpha2x2" }
    ];
    var out = {};
    var log = [];
    // 把页面自己的两个"出口"换成记账：
    //   · say()      —— 给人看的提示（拒绝理由/警告都走它）
    //   · openCropper —— 通过检查之后**真的开框选器**那一步（含 mode）
    // 后者是"背景那种情形没被拒"的**正面证据**：背景走到取景那一步时
    // 不会 say 任何东西（它本来就是顺利通过），只有看它有没有被调用才判得准。
    say = function (msg) { log.push("say:" + String(msg)); };
    var openCropperRaw = openCropper;
    openCropper = function (img, name, slot, mode) {
      log.push("crop:" + mode);
      return openCropperRaw.apply(null, arguments);
    };
    function step(i) {
      if (i >= scenes.length) {
        document.body.textContent = "RESULT" + JSON.stringify(out) + "ENDRESULT";
        return;
      }
      var sc = scenes[i];
      log.length = 0;
      var spec = sc.face
        ? { role: ImageBlob.ROLE.FaceIdle, face: true, label: "左屏 · 转速表（+水温） · 怠速" }
        : { role: ImageBlob.ROLE.Background, face: false, label: "背景" };
      try {
        sendFilesToSlot(spec, [fakeFile(sc.id + ".webp", FIX[sc.fix])]);
      } catch (e) {
        out[sc.id] = { error: String((e && e.message) || e) };
        step(i + 1);
        return;
      }
      var waited = 0;
      var tick = setInterval(function () {
        waited += 50;
        // 等到"有结果"（say 了东西，或者真的开了框选器）
        if (log.length || waited > 1500) {
          clearInterval(tick);
          out[sc.id] = { log: log.slice() };
          step(i + 1);
        }
      }, 50);
    }
    step(0);
  });
})();
<\/script>
`;
  html = html.replace("</body>", probe + "</body>");
  const probePath = path.join(tmpDir, "probe.html");
  fs.writeFileSync(probePath, html, "utf8");

  const exe = chrome[0];
  const url = "file:///" + probePath.replace(/\\/g, "/");
  let dom = "";
  try {
    dom = cp.execFileSync(exe, ["--headless=new", "--disable-gpu", "--no-sandbox",
                                "--virtual-time-budget=8000", "--dump-dom", url],
                          { encoding: "utf8", maxBuffer: 64 * 1024 * 1024,
                            stdio: ["ignore", "pipe", "ignore"] });
  } catch (e) {
    dom = (e && e.stdout) ? String(e.stdout) : "";
  }
  try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch (e) { /* ignore */ }

  const m = /RESULT([\s\S]*?)ENDRESULT/.exec(dom);
  ok(!!m, "无头浏览器跑完了探针页（读到了结果）");
  if (!m) {
    console.log("  （浏览器输出片段：" + String(dom).slice(0, 300).replace(/\s+/g, " ") + "）");
  } else {
    let res = null;
    try { res = JSON.parse(m[1]); } catch (e) { res = null; }
    ok(!!res, "结果是合法 JSON");
    if (res) {
      const bg = res.bg || {}, of = res.opaqueFace || {}, af = res.alphaFace || {};
      const text = (o) => (o.log || []).join("\n");
      const bgText = text(bg), ofText = text(of), afText = text(af);
      const firstMsg = (t) => t.split("\n")[0].slice(0, 62);
      console.log("  ① 背景 + WebP      → " + (bg.error ? "抛错：" + bg.error :
                  (bgText ? firstMsg(bgText) + " …" : "（无提示）")));
      console.log("  ② 不透明 WebP 表情 → " + (of.error ? "抛错：" + of.error :
                  (ofText ? firstMsg(ofText) + " …" : "（无提示）")));
      console.log("  ③ 带透明 WebP 表情 → " + (af.error ? "抛错：" + af.error :
                  (afText ? firstMsg(afText) + " …" : "（无提示）")));

      // ① 背景：必须**通过**并进入取景（mode="bg"），且一个字都没抱怨
      ok(!bg.error, "① 背景 + WebP：没有抛错");
      ok(bgText.indexOf("crop:bg") >= 0,
         "① 背景 + WebP：通过了检查并**打开了取景框**（mode=bg）");
      ok(bgText.indexOf("✗") < 0 && bgText.indexOf("不能用在") < 0,
         "① 背景 + WebP：没有被拒（背景可用 WebP）");
      // ② 不透明 WebP 当表情：必须被拒、给出路，且**不许**开框选器
      ok(!of.error, "② 不透明 WebP + 表情：没有抛错（是判据在拒，不是崩了）");
      ok(ofText.indexOf("say:✗") >= 0, "② 不透明 WebP + 表情：被拒（有 ✗ 提示）");
      ok(/不透明|alpha/i.test(ofText), "② 拒绝理由点明了「像素都是不透明的」");
      ok(/PNG/.test(ofText), "② 拒绝理由给出路：请用 PNG");
      ok(ofText.indexOf("crop:") < 0, "② 被拒之后**没有**打开框选器（不该走到那一步）");
      // ③ 带透明 WebP 当表情：必须通过，并进入表情框选（mode="face"）
      ok(!af.error, "③ 带透明 WebP + 表情：没有抛错");
      ok(afText.indexOf("✗") < 0 && afText.indexOf("不能用在") < 0,
         "③ 带透明 WebP + 表情：**通过了**（没有被拒）");
      ok(afText.indexOf("crop:face") >= 0,
         "③ 带透明 WebP + 表情：打开了**表情框选**（mode=face）");
    }
  }
}

console.log("\n" + "=".repeat(56));
if (fail === 0) console.log("全部通过:" + pass + " 项断言");
else {
  console.log("失败 " + fail + " 项 / 共 " + (pass + fail) + " 项:");
  failures.forEach(f => console.log("  - " + f));
}
console.log("=".repeat(56));
process.exit(fail === 0 ? 0 : 1);
