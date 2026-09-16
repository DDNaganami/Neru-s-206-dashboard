/* ============================================================
 * face-stages.js —— 表情阶段表的**网页端镜像**
 *
 * 固件那边的唯一事实来源是 lib/dashcore/face_stages.h:
 *   · kFaceStages      —— 阶段用例(每行同时给出左右两屏该显示哪张脸)
 *   · kFaceLeftStates / kFaceRightStates —— 每屏实际会产生哪几个状态
 *   · kFaceFallback    —— 缺图时的降级链
 *   · kFaceRoleId      —— 槽位 → 图片角色编号
 * 这个文件把这几张表抄一遍给浏览器用(表情导入页的"阶段模拟"就靠它),
 * 抄错了不会崩、只会"预览说红区脸、真车上是常态脸" ——
 * 所以 tools/theme-editor/test-face-stages.js 会**解析 face_stages.h**
 * 并逐字段比对。改了那边不改这边,Node 测试立刻红。
 *
 * ★ 两屏是**各自独立**的:
 *     左屏(转速表)只看转速 —— 常态/巡航/运动/红区
 *     右屏(速度表)只看车速 —— 常态/巡航/运动/超速
 *   水温两个表都不参与(只驱动水温弧与水温数字)。
 *   所以"某阶段该显示什么"必须按屏分别回答,这也是这里用
 *   SCREENS[].states 而不是一张全局状态表的原因。
 *
 * 同时给浏览器(<script src>)和 Node(require)用:
 * 不写 import/export,只在末尾挂到 globalThis / module.exports。
 * ============================================================ */
(function (root, factory) {
  "use strict";
  var api = factory();
  if (typeof module === "object" && module && module.exports) {
    module.exports = api;                 // Node
  }
  root.FaceStages = api;                  // 浏览器
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  // ---- 两屏各自的 4 个状态(**顺序 = 该屏档位从低到高**) ----
  // 角色编号与 image_blob.h 的 ImageRole 一致(由 test-face-stages.js 对账)。
  // ★ 两屏的状态集合**不一样**:左屏有"红区"没有"超速",右屏反过来。
  //   哪一屏有哪些状态由固件状态机决定(expression.cpp),这里只是声明。
  var SCREENS = [
    {
      key: "left", idx: 0, short: "左", label: "左屏 · 转速表",
      gauge: "rpm", unit: "rpm", gaugeLabel: "转速",
      states: [
        { key: "Idle",    label: "常态", role: 3  },
        { key: "Cruise",  label: "巡航", role: 12 },
        { key: "Sport",   label: "运动", role: 13 },
        { key: "Redline", label: "红区", role: 4  }
      ]
    },
    {
      key: "right", idx: 1, short: "右", label: "右屏 · 速度表",
      gauge: "speed", unit: "km/h", gaugeLabel: "车速",
      states: [
        { key: "Idle",      label: "常态", role: 6  },
        { key: "Cruise",    label: "巡航", role: 17 },
        { key: "Sport",     label: "运动", role: 18 },
        // 第 4 档:>130 km/h。角色号 8 沿用(当年是"惊喜"),改名不改号。
        { key: "Overspeed", label: "超速", role: 8  }
      ]
    }
  ];

  function screen(side) {
    if (typeof side === "number") return SCREENS[side] || SCREENS[0];
    for (var i = 0; i < SCREENS.length; i++) if (SCREENS[i].key === side) return SCREENS[i];
    return SCREENS[0];
  }
  function state(side, stateKey) {
    var st = screen(side).states;
    for (var i = 0; i < st.length; i++) if (st[i].key === stateKey) return st[i];
    return null;
  }
  // 该屏该状态的图片角色编号(这一屏没有这个状态 → null)
  function roleFor(side, stateKey) {
    var s = state(side, stateKey);
    return s ? s.role : null;
  }
  function stateLabel(side, stateKey) {
    var s = state(side, stateKey);
    return s ? s.label : stateKey;
  }
  function statesOf(side) { return screen(side).states; }

  // ---- 缺图降级链(按状态名;顺序 = 优先顺序) ----
  // 与 face_stages.h 的 kFaceFallback 逐行对应。
  // 链里出现"这一屏没有的状态"没有副作用:roleFor 会返回 null,
  // resolve 会直接跳过它(与固件侧 g_face_ok 恒为 false 同理)。
  var FALLBACK = {
    Idle:      ["Idle", "Cruise", "Sport", "Redline"],
    Cruise:    ["Cruise", "Idle", "Sport", "Redline"],
    Sport:     ["Sport", "Cruise", "Redline", "Idle"],
    Redline:   ["Redline", "Sport", "Cruise", "Idle"],
    Overspeed: ["Overspeed", "Sport", "Cruise", "Idle"]
  };

  // 该状态该用哪张图 —— 与固件 dash_ui.cpp 的 faceResolve() 同一套规则。
  // hasRole 是回调 (roleId) => boolean,由调用方回答"这张图导入了没有"。
  // 返回角色编号;这一屏一张表情图都没有 → 返回 null(交给程序化占位表情)。
  function resolve(side, stateKey, hasRole) {
    var chain = FALLBACK[stateKey] || [];
    var i, id;
    for (i = 0; i < chain.length; i++) {
      id = roleFor(side, chain[i]);
      if (id !== null && hasRole(id)) return id;
    }
    // 兜底:链里一张都没有,有图就用
    var sts = statesOf(side);
    for (i = 0; i < sts.length; i++) {
      if (hasRole(sts[i].role)) return sts[i].role;
    }
    return null;
  }

  // ---- 阶段用例(与 face_stages.h 的 kFaceStages 逐条对应) ----
  //
  // ★★ 两条硬规则(固件与网页两端都有测试钉住):
  //   ① 每组只变自己那一维:速度组固定 rpm=900(怠速)、转速组固定 speed=0。
  //      否则点"速度·中"时转速表跟着动,看不出这一档改了什么。
  //   ② **只有该屏自己的那一路能改它的表情**:转速组的右屏恒为常态、
  //      速度组的左屏恒为常态、水温组两屏都不动。
  //      这就是"每屏一套独立表情"的可执行定义。
  //
  // 转速四档用的是**实车地标**(用户实测:点火怠速 900 / 稳定巡航 2000 /
  // 表盘上限 6000;运动取中间 4200)—— 所以表里那一行就是车上真会出现的那一格。
  // 车速四档 0 / 55 / 110 / 140:分别落在 常态/巡航/运动/超速。
  // ★ 每屏 4 个状态、表里就 4 条 —— 每个状态都能被"点"出来。
  //   (当年右屏第 4 档是"急加速惊喜",它不是按车速分档的,所以表里没有它,
  //    用户试用时发现"这个档选不出来" —— 改成超速后就补齐了。)
  var STAGES = [
    // 转速:只驱动左屏;右屏恒为常态(车速一直是 0)
    { group: "rpm", level: "low",     rpm: 900,  speed: 0,   coolant: 85,  left: "Idle",    right: "Idle" },
    { group: "rpm", level: "mid",     rpm: 2000, speed: 0,   coolant: 85,  left: "Cruise",  right: "Idle" },
    { group: "rpm", level: "high",    rpm: 4200, speed: 0,   coolant: 85,  left: "Sport",   right: "Idle" },
    { group: "rpm", level: "redline", rpm: 6000, speed: 0,   coolant: 85,  left: "Redline", right: "Idle" },
    // 车速:只驱动右屏;左屏恒为常态(转速一直是怠速)
    { group: "speed", level: "low",  rpm: 900, speed: 0,   coolant: 85, left: "Idle", right: "Idle"      },
    { group: "speed", level: "mid",  rpm: 900, speed: 55,  coolant: 85, left: "Idle", right: "Cruise"    },
    { group: "speed", level: "high", rpm: 900, speed: 110, coolant: 85, left: "Idle", right: "Sport"     },
    { group: "speed", level: "over", rpm: 900, speed: 140, coolant: 85, left: "Idle", right: "Overspeed" },
    // 水温:两屏表情都不动,只影响水温弧与水温数字
    { group: "coolant", level: "low",  rpm: 900, speed: 0, coolant: 60,  left: "Idle", right: "Idle" },
    { group: "coolant", level: "mid",  rpm: 900, speed: 0, coolant: 85,  left: "Idle", right: "Idle" },
    { group: "coolant", level: "high", rpm: 900, speed: 0, coolant: 115, left: "Idle", right: "Idle" }
  ];

  // 量程上限 —— 画弧进度要用,必须与固件一致:
  //   rpm   → lib/dashcore/vehicle_state.h 的 kRpmMax(表盘刻度上限,实车 6000)
  //   speed → 同文件的 kSpeedMax
  // 不一致的后果是"预览里弧的进度和真车不一样"(而且看不出来),
  // 所以 test-face-stages.js 会把这两个数从 vehicle_state.h 里解析出来对比。
  var MAX = { rpm: 6000, speed: 210 };

  // 界面上的分组与中文标签(纯展示,固件不关心)
  // faces:false = 这一组不影响表情(界面要明确写出来,免得用户以为调了没用)
  var GROUPS = [
    { key: "rpm",     label: "转速", unit: "rpm",  faces: true,
      levels: { low: "低", mid: "中", high: "高", redline: "红区" } },
    { key: "speed",   label: "车速", unit: "km/h", faces: true,
      levels: { low: "低", mid: "中", high: "高", over: "超速" } },
    { key: "coolant", label: "水温", unit: "°C",   faces: false,
      levels: { low: "低", mid: "中", high: "高" } }
  ];

  function group(key) {
    for (var i = 0; i < GROUPS.length; i++) if (GROUPS[i].key === key) return GROUPS[i];
    return null;
  }
  function stagesOf(key) {
    return STAGES.filter(function (s) { return s.group === key; });
  }
  function stageValueText(st) {
    var g = group(st.group);
    var v = st.group === "rpm" ? st.rpm : (st.group === "coolant" ? st.coolant : st.speed);
    return v + " " + (g ? g.unit : "");
  }
  // 该阶段某一屏该显示什么(状态名)
  function faceOf(st, side) {
    return (screen(side).key === "left") ? st.left : st.right;
  }

  return {
    SCREENS: SCREENS,
    FALLBACK: FALLBACK,
    STAGES: STAGES,
    GROUPS: GROUPS,
    MAX: MAX,
    screen: screen,
    state: state,
    statesOf: statesOf,
    roleFor: roleFor,
    stateLabel: stateLabel,
    resolve: resolve,
    group: group,
    stagesOf: stagesOf,
    stageValueText: stageValueText,
    faceOf: faceOf
  };
});
