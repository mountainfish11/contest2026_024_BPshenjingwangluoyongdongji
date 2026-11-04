# logs/ — AI Coding 日志目录

本目录存放与作品代码一并提交的 AI Coding 对话日志。当前日志由 openvela
官方 `contest-log-collector` 的 Codex backfill 适配器生成，并通过官方
`validate-log.py` 校验。

## 目录结构

```text
logs/
└── <github_login>/              # 你的 GitHub 用户名，一人一目录
    ├── manifest.json            # 会话清单
    └── <date>/                  # 日期 YYYY-MM-DD
        └── <tool>__<sid>.jsonl  # 一个会话一个文件（工具名与 session id 用 __ 连接）
```

- `<tool>`：`claude-code` / `opencode` / `codex` / `kiro`
- 每个 `.jsonl` 每行一个事件，由组委会提供的日志归集工具导出。
- 当前提交包含 2 个 Codex 会话、148 条事件；未人工改写日志内容。
- 含本地凭证的会话按官方规则整段排除，没有截取或修改事件。

导出与提交的完整步骤、字段定义见[《AI Coding 日志归集与提交手册》](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_coding_log_guide.md)。
