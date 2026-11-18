// 从 native 源码里抽出 R"HTML(...)HTML" 原始字符串，检查内嵌 <script> 的语法。
// 用途：WebView2 页面语法错会白屏，编译期抓不到，出包前先过一遍。
const fs = require('fs');
const vm = require('vm');

function extract(file) {
  const src = fs.readFileSync(file, 'utf8');
  const re = /R"HTML\(([\s\S]*?)\)HTML"/g;
  let out = '', m;
  while ((m = re.exec(src))) out += m[1];
  // 首帧状态在 R"HTML 外面用 .arg(stateJson) 注入（`let STATE = )HTML"` 断在那），
  // 拼接后会剩下 `let STATE = ;`——检查语法时补个空对象。
  return out.replace(/let STATE = ;/g, 'let STATE = {};');
}

function checkScripts(html, name) {
  const re = /<script[^>]*>([\s\S]*?)<\/script>/g;
  let m, n = 0, bad = 0;
  while ((m = re.exec(html))) {
    const code = m[1];
    if (!code.trim()) continue;
    n++;
    try {
      new vm.Script(code, { filename: name + '#script' + n });
    } catch (e) {
      bad++;
      console.log(`[FAIL] ${name} script#${n}: ${e.message}`);
      const mm = /#script\d+:(\d+)/.exec(e.stack || '');
      if (mm) {
        const lines = code.split('\n');
        const at = +mm[1];
        for (let i = Math.max(0, at - 3); i < Math.min(lines.length, at + 2); i++)
          console.log(`  ${i + 1}: ${JSON.stringify(lines[i])}`);
      }
    }
  }
  console.log(`${name}: ${n} script block(s), ${bad} error(s)`);
  return bad;
}

let bad = 0;
bad += checkScripts(extract('native/src/ui/WorkWebView.cpp'), 'desktop');
bad += checkScripts(extract('native/src/core/LanWebServer.cpp'), 'mobile');
process.exit(bad ? 1 : 0);
