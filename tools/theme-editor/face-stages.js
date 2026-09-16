/* ============================================================
 * face-stages.js —— 表情阶段表的**网页端镜像**
 *
 * 固件那边的唯一事实来源是 lib/dashcore/face_stages.h:
 *   · kFaceStages —— 9 条阶段用例(转速/水温/速度 × 低中高 → 该显示哪张表情)
 *   · kFaceFallback —— 缺图时的降级链
 *   · kFaceRoleId   —— 槽位 → 图片角色编号
 * 这个文件把三张表抄一遍给浏览器用(表情导入页的"阶段模拟"就靠它),
 * 抄错了不会崩、只会"预览说红区脸、真车上是常态脸" ——
 * 所以 tools/theme-editor/test-face-stages.js 会**解析 face_stages.h**
 * 并逐字段比对。改了那边不改这边,Node 测试立刻红。
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

  // ---- 7 个表情槽位,**顺序 = Face 枚举顺序 = 数组下标** ----
  // 顺序来自 lib/dashcore/expression.h,别重排:固件端拿枚举值当下标。
  // ★ 曾经有 Blink(眨眼),已删除 —— 它只跟时间有关、与车速/转速/水温都无关,
  //   既讲不出"什么工况下会眨眼",也没法在阶段模拟里体现。
  //   编号 11/16(当年的眨眼图)作为保留编号留空,不复用。
  var FACES = [
    { key: "Idle",     label: "常态", roleL: 3,  roleR: 6  },
    { key: "Cruise",   label: "巡航", roleL: 12, roleR: 17 },
    { key: "Sport",    label: "运动", roleL: 13, roleR: 18 },
    { key: "Redline",  label: "红区", roleL: 4,  roleR: 7  },
    { key: "Surprise", label: "惊喜", roleL: 5,  roleR: 8  },
    { key: "Cold",     label: "冷车", roleL: 14, roleR: 19 },
    { key: "Hot",      label: "过热", roleL: 15, roleR: 20 }
  ];

  function faceIndex(key) {
    for (var i = 0; i < FACES.length; i++) if (FACES[i].key === key) return i;
    return -1;
  }
  function face(key) { return FACES[faceIndex(key)]; }
  function roleFor(side, key) {
    var f = face(key);
    if (!f) return null;
    return (side === 0 || side === "L") ? f.roleL : f.roleR;
  }

  // ---- 缺图降级链(下标 = 槽位号;顺序 = 优先顺序) ----
  // 与 face_stages.h 的 kFaceFallback 逐行对应。
  var FALLBACK = {
    Idle:     ["Idle", "Cruise", "Sport", "Redline"],
    Cruise:   ["Cruise", "Idle", "Sport", "Redline"],
    Sport:    ["Sport", "Cruise", "Redline", "Idle"],
    Redline:  ["Redline", "Sport", "Surprise", "Idle"],
    Surprise: ["Surprise", "Redline", "Sport", "Idle"],
    Cold:     ["Cold", "Idle", "Cruise", "Sport"],
    Hot:      ["Hot", "Surprise", "Redline", "Idle"]
  };

  // 该状态该用哪张图 —— 与固件 dash_ui.cpp 的 faceResolve() 同一套规则。
  // hasRole 是回调 (roleId) => boolean,由调用方回答"这张图导入了没有"。
  // 返回角色编号;整屏一张图都没有 → 返回 null(交给程序化占位表情)。
  function resolve(side, key, hasRole) {
    var chain = FALLBACK[key] || [];
    var i, id;
    for (i = 0; i < chain.length; i++) {
      id = roleFor(side, chain[i]);
      if (id !== null && hasRole(id)) return id;
    }
    // 兜底:链里一张都没有(比如只导入了"冷车"一张),有图就用
    for (i = 0; i < FACES.length; i++) {
      id = roleFor(side, FACES[i].key);
      if (hasRole(id)) return id;
    }
    return null;
  }

  // ---- 9 条阶段用例(与 face_stages.h 的 kFaceStages 逐条对应) ----
  //
  // ★★ 每组**只能变自己那一维**,另外两维固定(速度组固定 rpm=900 怠速、
  //    水温组/转速组固定 speed=0)。这是用户实际试用后提的要求,不是洁癖:
  //    点"速度·中"时**转速表不应该跟着动** —— 否则画面里两个表同时变,
  //    根本看不出这一档到底改了什么。
  //    曾经违反过:速度组的 rpm 写成 1500/2600(想借转速凑出巡航/运动脸),
  //    结果点速度档时转速弧和转速表的表情一起变。
  //    其实"速度≥30/≥90"这两条阈值自己就能给出巡航/运动,不需要借转速。
  //    固件侧有 test_stage_groups_isolate_one_dimension 钉住同一条规则。
  var STAGES = [
    { group: "rpm",     level: "low",  rpm: 800,  speed: 0,   coolant: 85,  face: "Idle"     },
    { group: "rpm",     level: "mid",  rpm: 3200, speed: 0,   coolant: 85,  face: "Cruise"   },
    { group: "rpm",     level: "high", rpm: 6600, speed: 0,   coolant: 85,  face: "Redline"  },
    { group: "coolant", level: "low",  rpm: 900,  speed: 0,   coolant: 60,  face: "Cold"     },
    { group: "coolant", level: "mid",  rpm: 900,  speed: 0,   coolant: 85,  face: "Idle"     },
    { group: "coolant", level: "high", rpm: 900,  speed: 0,   coolant: 115, face: "Hot"      },
    { group: "speed",   level: "low",  rpm: 900,  speed: 0,   coolant: 85,  face: "Idle"     },
    { group: "speed",   level: "mid",  rpm: 900,  speed: 55,  coolant: 85,  face: "Cruise"   },
    { group: "speed",   level: "high", rpm: 900,  speed: 110, coolant: 85,  face: "Sport"    }
  ];

  // 界面上的分组与中文标签(纯展示,固件不关心)
  var GROUPS = [
    { key: "rpm",     label: "转速", unit: "rpm",  levels: { low: "低", mid: "中", high: "高" } },
    { key: "coolant", label: "水温", unit: "°C",   levels: { low: "低", mid: "中", high: "高" } },
    { key: "speed",   label: "速度", unit: "km/h", levels: { low: "低", mid: "中", high: "高" } }
  ];

  function stagesOf(group) {
    return STAGES.filter(function (s) { return s.group === group; });
  }

  // 该阶段的"卖点"数字,给界面上的小标签用(例:转速 · 中 · 3200 rpm)
  function stageValueText(st) {
    var g = GROUPS.filter(function (x) { return x.key === st.group; })[0];
    var v = st.group === "rpm" ? st.rpm : (st.group === "coolant" ? st.coolant : st.speed);
    return v + " " + (g ? g.unit : "");
  }

  return {
    FACES: FACES,
    FALLBACK: FALLBACK,
    STAGES: STAGES,
    GROUPS: GROUPS,
    face: face,
    faceIndex: faceIndex,
    roleFor: roleFor,
    resolve: resolve,
    stagesOf: stagesOf,
    stageValueText: stageValueText
  };
});
