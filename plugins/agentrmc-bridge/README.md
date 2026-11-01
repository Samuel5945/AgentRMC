# AgentRMC Bridge（ZCode 插件）

把 [AgentRMC](https://github.com/AgentRMC/Agentrmc) 桌宠接进 ZCode：

- **自动提醒（hooks）**：ZCode 需要确认工具调用（PermissionRequest）、一轮
  回答结束（Stop）时，桌宠自动冒气泡提示；桌宠收起时走系统托盘。AgentRMC
  没开就静默跳过（<100ms），不拖慢会话。
- **主动交互（MCP）**：三个工具——`pet_notify`（阶段进度气泡）、
  `pet_speak`（让桌宠 TTS 说一句话）、`pet_state`（查桌宠状态），配
  `agentrmc-pet` skill 教模型何时用、怎么不刷屏。
- **传输**：全部走 AgentRMC 本机回环口 `127.0.0.1:45915`（AgentBridge），
  无密钥、无外部依赖（hooks 与 MCP server 都是零依赖 node 脚本）。

## 前提

- ZCode（桌面版）+ 本插件；
- AgentRMC v0.2.0 及以上（带 AgentBridge 的安装包）；
- node 在 PATH（hooks 与 MCP 都用 `node` 调起）。

## 安装

应用市场添加本地目录（本仓库 `plugins/`，内含 `marketplace.json`），安装
「AgentRMC 桌宠联动」即可；hooks 与 MCP 随插件自动生效。

> 如果你之前手工在 `~/.zcode/cli/config.json` 里配过
> `PermissionRequest` / `Stop` hook（指向 `~/.zcode/hooks/agentrmc-notify.js`），
> 装完插件请删掉那两条，避免通知重复。

## 文件

- `hooks/hooks.json` + `hooks/agentrmc-notify.js`：两个 hook 的脚本；
- `mcp/server.js`：stdio MCP server（pet_notify / pet_speak / pet_state）；
- `skills/agentrmc-pet/SKILL.md`：模型使用指南。
