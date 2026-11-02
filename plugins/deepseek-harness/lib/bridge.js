// AgentRMC 本机回环口客户端（零依赖）。
//
// 与原版插件（hooks/agentrmc-notify.js、mcp/server.js）走同一个接口，所以
// 原生端不需要为 DeepSeek Harness 单开一条路：
//   POST /notify           气泡（stop / permission 会念 + 拉前台，其它只冒泡）
//   POST /speak            气泡 + TTS 念一句
//   GET  /state            桌宠状态
//   GET  /work-items       「待我处理」队列
//   POST /work-items/update 汇报 progress / blocked / ready
//   POST /work-items/ack   标记一条已处理
//
// 区别只在于：原版是「一次性 node 脚本」，每次事件都要冷启动一个进程；这里是
// 常驻 Host 插件，直接在进程里发 HTTP，省掉冷启动。超时与落盘策略照搬：
// 连不上 / 5xx / 超时都写 pending-events.jsonl，原生端启动或每 5 秒补投。
'use strict';

import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';

const HOST = '127.0.0.1';

/** 默认落盘目录与原版一致：%APPDATA%\LIG\AgentRMCNative。 */
export function resolveSpoolDir(configured) {
  const value = String(configured || '').trim();
  if (value) return value;
  return path.join(process.env.APPDATA || os.tmpdir(), 'LIG', 'AgentRMCNative');
}

/**
 * 建一个桥接客户端。
 * @param {object} options
 * @param {number} options.port  AgentBridge 端口（原生端默认 45915）。
 * @param {string} options.spoolDir 离线补投目录。
 * @param {number} options.timeoutMs 单次请求超时。
 * @param {boolean} options.dryRun 只记日志不真发（测试 / 排查用）。
 * @param {(line: string) => void} [options.log] 诊断日志。
 */
export function createBridge(options) {
  const port = Number(options.port) || 45915;
  const timeoutMs = Number(options.timeoutMs) || 1500;
  const spoolDir = options.spoolDir;
  const dryRun = options.dryRun === true;
  const log = typeof options.log === 'function' ? options.log : () => {};

  // 失败日志节流：60 秒内只报一次，其余计数攒着下次一起说。
  let failures = 0;
  let lastLoggedAt = 0;
  function noteFailure(message) {
    failures += 1;
    const now = Date.now();
    if (now - lastLoggedAt < 60_000) return;
    lastLoggedAt = now;
    log(message + (failures > 1 ? '（此前还有 ' + (failures - 1) + ' 条同样失败）' : ''));
  }

  function request(method, route, payload) {
    return new Promise((resolve, reject) => {
      const body = payload === undefined ? '' : JSON.stringify(payload);
      const headers = body
        ? {
            'Content-Type': 'application/json',
            'Content-Length': Buffer.byteLength(body),
          }
        : { 'Content-Length': 0 };
      const req = http.request(
        { host: HOST, port, path: route, method, headers, timeout: timeoutMs },
        (res) => {
          let data = '';
          res.setEncoding('utf8');
          res.on('data', (chunk) => {
            data += chunk;
          });
          res.on('end', () => resolve({ status: res.statusCode, body: data }));
        },
      );
      req.on('timeout', () => req.destroy(new Error('AgentRMC 响应超时')));
      req.on('error', (error) => reject(error));
      if (body) req.write(body);
      req.end();
    });
  }

  /** 落盘一条待补投事件；落盘也失败就彻底放弃——通知不能挡会话。 */
  function spool(payload) {
    try {
      fs.mkdirSync(spoolDir, { recursive: true });
      fs.appendFileSync(
        path.join(spoolDir, 'pending-events.jsonl'),
        JSON.stringify(payload) + '\n',
      );
      return true;
    } catch (error) {
      log('spool failed: ' + (error && error.message));
      return false;
    }
  }

  /**
   * 发一条 /notify。**不返回 Promise**：监听器在 Agent 主链路上（approval /
   * user-questions 是 waterfall，turn-stopping 是 serial），等网络会拖住会话。
   * 失败按原版规则落盘，原生端会补投。
   */
  function notify(payload) {
    if (dryRun) {
      log('dry-run notify: ' + JSON.stringify(payload));
      return;
    }
    request('POST', '/notify', payload).then(
      (res) => {
        if (res.status >= 500) {
          noteFailure('AgentRMC 返回 HTTP ' + res.status + '，通知先落盘');
          spool(payload);
        } else {
          failures = 0;
        }
      },
      (error) => {
        // 宿主不重启，桌宠没开的时候每条事件都会失败：日志按分钟节流，
        // 否则一个长会话能把 Host 日志刷满（原版是一次性进程，看不见这问题）。
        noteFailure('AgentRMC 没连上（' + (error && error.message) + '），通知先落盘等补投');
        spool(payload);
      },
    );
  }

  return {
    port,
    spoolDir,
    request,
    notify,
    spool,
    post(route, payload) {
      return request('POST', route, payload);
    },
    get(route) {
      return request('GET', route);
    },
  };
}
