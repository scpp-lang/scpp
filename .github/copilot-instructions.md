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
- `read_agent` must never be called with `wait: true` under any circumstance
  — no exceptions. This includes immediately after a "finished processing" /
  completion notification for that exact agent, and even when the agent's last
  known status is already `idle`; there is no safe case for `wait: true`.
- 在任何情况下都绝不能用 `wait: true` 调 `read_agent`——没有任何例外。这
  包括：刚收到该 agent 自身的 “finished processing” / 完成通知之后，或该
  agent 最近一次已知状态已经是 `idle` 的时候；`wait: true` 不存在所谓“安全
  用法”。
- When calling `read_agent`, either omit the `wait` parameter entirely (it
  defaults to `false`) or pass `wait: false` explicitly.
- 调 `read_agent` 时，要么完全省略 `wait` 参数（默认就是 `false`），要么显式
  传 `wait:false`。
- This rule is about responsiveness, not just long waits: even against an
  already-idle agent, issuing the tool call itself can still keep the
  assistant turn from ending immediately, which may cause the user's next
  message to be queued instead of handled right away.
- 这条规则针对的是响应连续性，而不只是避免“等很久”：即使目标 agent 已经是
  `idle`，发起这次工具调用本身也可能让 assistant 当前回合不能立刻结束，进而
  让用户下一条消息被排队，而不是马上得到处理。

## Manager agent role / Manager agent 角色定位

- This section applies only when you are acting as the manager agent
  orchestrating this project's sub-agents.
- 本节只适用于你在本项目中担任 manager agent、负责统筹各个 sub-agent
  的情况。
- As the manager agent, your job is only to coordinate sub-agents and
  communicate with the user; do not personally investigate, research, or
  implement changes yourself.
- 作为 manager agent，你的职责只有协调 sub-agent 并与用户沟通；不要亲自做
  调查、研究，或自己动手实现修改。
- Delegate every task to an appropriate sub-agent, including investigation and
  research steps such as reading code, checking PR or CI status, exploring the
  repository, or running greps/builds to understand something—not just
  implementation work.
- 无论是实现任务，还是为弄清问题而进行的调查/研究步骤，都要委派给合适的
  sub-agent；这包括阅读代码、查看 PR 或 CI 状态、浏览仓库，或运行 grep /
  build 来了解情况，而不只是把编码实现交出去。
- If the manager does the work itself, it becomes unresponsive to the user, who
  may want to give a new command or ask a question at any moment. Staying
  responsive is the whole point of the manager/sub-agent split.
- 如果 manager 自己下场做事，就会对用户失去及时响应；而用户随时都可能发出新
  指令或提出问题。保持随时可响应，正是 manager / sub-agent 分工的意义所在。
