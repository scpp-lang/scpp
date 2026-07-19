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

## Branch naming and PR conventions / 分支命名与 PR 规范

- Every sub-agent must name its git branch `<role>-agent/<slug>`, where
  `<role>` matches the sub-agent's function (`dev-agent`, `test-agent`,
  `doc-agent`, `book-agent`, etc.) and `<slug>` is a short kebab-case
  description of the task.
- 每个 sub-agent 的 git 分支都必须命名为 `<role>-agent/<slug>`，其中 `<role>`
  对应该 sub-agent 的职能（`dev-agent`、`test-agent`、`doc-agent`、
  `book-agent` 等），`<slug>` 则是对任务的简短 kebab-case 描述。
- Concrete examples: `dev-agent/alignas-impl`,
  `test-agent/array-bound-constant-expr-coverage`,
  `doc-agent/requires-lifetime-spec`,
  `book-agent/ch07-01-packages-project-manifests`.
- 具体例子：`dev-agent/alignas-impl`、
  `test-agent/array-bound-constant-expr-coverage`、
  `doc-agent/requires-lifetime-spec`、
  `book-agent/ch07-01-packages-project-manifests`。
- Never create a branch without this prefix, no matter how small or
  exploratory the change is.
- 无论改动多小、多么带有探索性质，都绝不能创建没有该前缀的分支。
- The `<role>-agent/<slug>` prefix applies ONLY to the branch name. The git
  **commit author/committer identity** for every commit in this repository,
  regardless of which sub-agent or role produced it, must always be
  `xyb <xyb@lotx.name>` — never a role-specific identity such as
  `dev-agent@users.noreply.github.com`. Before committing, run
  `git config user.name xyb && git config user.email xyb@lotx.name` in the
  worktree, and re-verify with `git log -1 --format='%an <%ae>'` after
  committing; if a commit ever shows the wrong identity, fix it with
  `git commit --amend --reset-author --no-edit` (after correcting the config)
  before pushing.
- `<role>-agent/<slug>` 前缀只适用于分支名。本仓库中每一个 commit 的 git
  **author / committer 身份**，无论由哪个 sub-agent 或角色产生，都必须始终是
  `xyb <xyb@lotx.name>`——绝不能使用类似 `dev-agent@users.noreply.github.com`
  这样的角色专属身份。提交前，请在工作目录中运行
  `git config user.name xyb && git config user.email xyb@lotx.name`，并在
  提交后用 `git log -1 --format='%an <%ae>'` 再次核实；如果某个 commit 的身份
  不对，先修正配置，再用 `git commit --amend --reset-author --no-edit` 改正，
  然后再 push。
- Every PR in this repository must contain exactly one commit. If a change
  seems to need multiple commits, split the work into multiple separate PRs
  instead of stacking commits on one branch / PR.
- 本仓库中的每个 PR 都必须只包含一个 commit。如果某项改动看起来需要多个
  commit，应该把工作拆分成多个独立的 PR，而不是在同一个分支 / PR 上堆叠多个
  commit。
- If you end up with more than one commit locally, squash or amend before
  pushing so the branch / PR still contains only one commit.
- 如果本地不小心产生了不止一个 commit，推送前要先 squash 或 amend，确保该分
  支 / PR 最终仍然只有一个 commit。
- Sub-agents and the manager agent must never run `gh pr merge` or any
  equivalent action to merge a PR in this repository, no matter how
  thoroughly validated or green that PR is.
- 无论 sub-agent 还是 manager agent，都绝不能运行 `gh pr merge` 或任何等效
  操作来合并本仓库中的 PR，哪怕这个 PR 已经过充分验证、状态全绿也不行。
- Only the repository owner (the human user) decides when and whether to
  merge. A sub-agent's job ends at opening a fully validated, mergeable PR
  and reporting it back—never at merging it.
- 是否合并、何时合并，只能由仓库所有者（人类用户）决定。sub-agent 的职责止
  于打开一个已充分验证、可合并的 PR 并汇报结果——绝不包括合并它。

### File-scope ownership by role / 按角色划分的文件归属

- dev-agent's PRs primarily touch implementation code and its
  directly-coupled unit tests: `src/`, `libs/`, and `tests/*.cpp` +
  `tests/*_source/` + `tests/*.expected` (gtest-style unit/fixture tests
  that exercise internal APIs 1:1 with the code being changed).
- dev-agent 的 PR 主要改动实现代码，以及与之直接耦合的单元测试：`src/`、
  `libs/`，以及 `tests/*.cpp` + `tests/*_source/` + `tests/*.expected`
  （以 gtest 风格、与被改动代码内部 API 一一对应的单元 / fixture 测试）。
- If a dev-agent change causes existing `blackbox_test/` cases to fail
  (e.g. it intentionally changes or removes language syntax/behavior
  those cases exercise), dev-agent may make minimal fixes to restore a
  passing build: migrating a case's syntax to match the new behavior,
  correcting stale expected output/values, or deleting a case that now
  tests something entirely inapplicable or nonexistent.
- 如果 dev-agent 的改动导致现有的 `blackbox_test/` 用例失败（例如它有意
  改变或移除了这些用例所验证的语言语法 / 行为），dev-agent 可以做最小化
  的修复来恢复构建通过：迁移某个用例的语法以匹配新行为、修正过时的期望
  输出 / 数值，或删除一个现在测试内容已完全不适用 / 不存在的用例。
- dev-agent must never add brand-new test cases to `blackbox_test/`.
  Authoring new coverage—including replacement coverage for a case
  dev-agent deleted, and regression tests for any incidental bug found
  along the way—is exclusively test-agent's job.
- dev-agent 绝不能向 `blackbox_test/` 添加全新的测试用例。编写新的覆
  盖——包括为 dev-agent 删除的用例补充替代覆盖，以及为顺带发现的偶发
  bug 补充回归测试——完全是 test-agent 的工作。
- That new coverage lands as a follow-up test-agent PR based on the
  dev-agent's own branch, not `main`, per the existing convention that a
  test-agent PR covering an in-flight dev-agent feature is based on the
  dev-agent's branch.
- 这些新增覆盖会以一个后续的 test-agent PR 落地，该 PR 基于 dev-agent
  自己的分支创建，而不是基于 `main`——这也符合现有惯例：覆盖一个尚在进
  行中的 dev-agent 功能的 test-agent PR，应该基于该 dev-agent 的分支。

## Testing philosophy / 测试原则

- All tests in this repository—especially `blackbox_test/`, but the principle
  applies generally—must be written against the spec / intended design, never
  against whatever the current implementation happens to do.
- 本仓库中的所有测试——尤其是 `blackbox_test/`，但这一原则同样适用于其他测
  试——都必须依据规范 / 预期设计来编写，绝不能按当前实现“碰巧在做什么”来写。
- The whole purpose of tests here is to catch implementation bugs by checking
  behavior against the documented or intended contract. A test written to match
  current (possibly buggy) behavior cannot catch a bug; it only tautologically
  confirms that the code does what the code currently does.
- 这里写测试的根本目的，是把程序行为与文档化的 / 预期的契约进行比对，从而暴
  露实现 bug。如果测试只是去迎合当前实现（而当前实现本身可能有 bug），那它就不
  可能发现 bug；它只是在同义反复地证明“代码会做它现在正在做的事”。
- If the spec says one thing and the current implementation does another, the
  test must assert the spec / intended behavior, even if that means the test is
  red today. In that situation, the failing test is doing its job by exposing a
  real bug; do not "fix" the test by weakening it to accommodate the buggy
  implementation.
- 如果规范要求的是一种行为，而当前实现表现出另一种行为，测试也必须断言规范 /
  预期行为，即使这意味着它在今天会是红的。在这种情况下，测试失败恰恰说明它正确
  地完成了职责：它暴露了一个真实 bug；不要为了迁就有 bug 的实现而把测试“修
  软”。
- Concrete example: if asked to add a test for `std::move()` on primitive or
  enum types, it is wrong to encode today's actual rejection behavior just
  because the current compiler rejects it. The correct test asserts the
  spec-conforming behavior—that such code should work—and is expected to remain
  red until the compiler bug is fixed.
- 具体例子：如果有人要求为 primitive / enum 类型上的 `std::move()` 补测试，
  那么因为“当前编译器今天会拒绝它”就把这种拒绝行为写进测试，是错误的。正确做法
  是断言符合规范的行为——这种代码本来就应该工作——并接受该测试在编译器 bug 被修
  复之前持续保持红灯。
