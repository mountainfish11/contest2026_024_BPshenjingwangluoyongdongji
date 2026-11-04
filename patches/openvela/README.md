# openvela Patch Series

这些补丁由已验证工作区中的提交通过 `git format-patch` 生成，目录内文件名的
数字前缀就是应用顺序。请优先使用 `tools/apply_openvela_patches.sh`，不要把补丁
逐个复制覆盖到源码树。

补丁生成来源：

```text
nuttx:             861540f7d52..792b2d2d68d
packages/ai_agent: 964117f..c463b96
apps:              c90b4e6c5..3fed86b66
```

详细说明见 `docs/openvela_workspace_dependencies.md`。
