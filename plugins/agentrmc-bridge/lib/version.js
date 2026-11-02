'use strict';
// 插件版本的单一来源：包根的 VERSION 文件（构建时 native/tools/sync-plugin-version.ps1
// 把它同步进 3 个 plugin.json 和 2 个 marketplace.json）。
// hook 和 MCP server 都读这里，报的是「这份已经加载进内存的代码是哪一版」——
// 宿主装完不重启还是旧代码，光看安装目录判断不了。
// 读不到返回空串：版本读数缺失绝不能拖垮提醒本身。

const fs = require('fs');
const path = require('path');

module.exports = function readVersion() {
  try {
    return fs
      .readFileSync(path.join(__dirname, '..', 'VERSION'), 'utf8')
      .trim();
  } catch {
    return '';
  }
};
