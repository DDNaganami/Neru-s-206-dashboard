// ab-diff.js —— 逐像素比较两套 pcpreview 落帧（用于"量程真的驱动了像素"这类 A/B）
//   node ab-diff.js <dirA> <dirB> [帧名...]
// 只读 24bpp BMP；不依赖任何库。
const fs = require('fs');
const path = require('path');

function readBmp(p) {
  const b = fs.readFileSync(p);
  if (b.length < 54 || b[0] !== 0x42 || b[1] !== 0x4d) throw new Error('不是 BMP: ' + p);
  const off = b.readUInt32LE(10);
  const w = b.readInt32LE(18);
  const h = b.readInt32LE(22);
  const bpp = b.readUInt16LE(28);
  if (bpp !== 24) throw new Error('只支持 24bpp，这里是 ' + bpp);
  return { b, w, h, off, row: Math.floor((bpp * w + 31) / 32) * 4, bpp };
}
function px(f, x, y) {
  const yy = f.h > 0 ? (f.h - 1 - y) : y;          // BMP 行序：正高度=自下而上
  const i = f.off + yy * f.row + x * 3;
  return [f.b[i + 2], f.b[i + 1], f.b[i]];          // BGR -> RGB
}

const [, , dirA, dirB, ...only] = process.argv;
if (!dirA || !dirB) { console.error('用法: node ab-diff.js <dirA> <dirB> [帧名...]'); process.exit(2); }

const names = only.length ? only : fs.readdirSync(dirA).filter(n => /\.bmp$/i.test(n)).sort();
let best = { name: null, n: 0 };
let rows = [];
for (const n of names) {
  const pa = path.join(dirA, n), pb = path.join(dirB, n);
  if (!fs.existsSync(pa) || !fs.existsSync(pb)) { rows.push([n, 'missing', '', '']); continue; }
  const A = readBmp(pa), B = readBmp(pb);
  if (A.w !== B.w || A.h !== B.h) throw new Error('尺寸不同: ' + n);
  let n_ = 0, x0 = 1e9, y0 = 1e9, x1 = -1, y1 = -1, dmax = 0;
  for (let y = 0; y < A.h; y++) {
    for (let x = 0; x < A.w; x++) {
      const a = px(A, x, y), c = px(B, x, y);
      const d = Math.abs(a[0] - c[0]) + Math.abs(a[1] - c[1]) + Math.abs(a[2] - c[2]);
      if (d > 0) {
        n_++;
        if (d > dmax) dmax = d;
        if (x < x0) x0 = x; if (x > x1) x1 = x;
        if (y < y0) y0 = y; if (y > y1) y1 = y;
      }
    }
  }
  if (n_ > best.n) best = { name: n, n: n_ };
  rows.push([n, n_, n_ ? `x${x0}-${x1} y${y0}-${y1}` : '-', n_ ? dmax : '-']);
}
rows.sort((a, b) => (b[1] === 'missing' ? -1 : b[1]) - (a[1] === 'missing' ? -1 : a[1]));
const show = rows.slice(0, 6);
console.log(`比较 ${names.length} 帧：A=${dirA}  B=${dirB}`);
console.log('差异像素最多的帧：');
for (const r of show) console.log(`  ${r[0]}  差异=${r[1]}  包围盒=${r[2]}  最大通道差=${r[3]}`);
const nz = rows.filter(r => typeof r[1] === 'number' && r[1] > 0).length;
console.log(`有差异的帧：${nz}/${names.length}；最大差异：${best.name} (${best.n} px = ${(best.n / (480 * 480) * 100).toFixed(2)}% 画布)`);
