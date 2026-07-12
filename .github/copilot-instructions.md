# Copilot Instructions / Copilot 工作指令

These rules apply to any Copilot agent (manager or sub-agent) working in this
repository. 以下规则适用于在本仓库中工作的任何 Copilot agent（无论是 manager
还是 sub-agent）。

## Sub-agent coordination / Sub agent 协作

- Never blockingly wait on a sub-agent (e.g. never call `read_agent` with
  `wait: true`, and never poll it in a loop). Dispatch sub-agent work in the
  background and return control immediately, since the user may want to
  communicate at any time.
- 永远不要阻塞地等待一个 sub agent（例如绝不用 `wait: true` 调
  `read_agent`，也不要写轮询循环等它完成）。派发任务后立即返回，把控制权交还
  给用户，因为用户随时可能插话交流。
- Check on sub-agent progress non-blockingly instead (e.g. `read_agent`
  without `wait`, or `wait: false`), and read full results once a completion
  notification arrives.
- 需要查看 sub agent 进度时，用非阻塞方式查询状态（如 `read_agent` 不带
  `wait` 或 `wait:false`），收到完成通知后再去读取结果。
