# D2：Derivation / Environment 对象分层

状态：**latest-only 的 raw 选择、直接组合、typed Quest 与 Environment 交付已实现**。外部生成内容仍是 [DERIVED_ARTIFACTS.md](DERIVED_ARTIFACTS.md) 中的提案，不是已支持的功能。

依据：[设计对齐审查 D2](DESIGN_ALIGNMENT_REVIEW.md#d2-derivation--environment-的职责分层不同)、[论文](memix.pdf) §4.2，以及用户明确要求：**不考虑兼容，直接实现最新模型**。

## 1. 决定与范围

```text
Store locators ──选择 Quest──resolve──> D_python ──mount python──┐
Store locators ──选择 Quest──resolve──> D_project ──mount project─┤
                                                               E ──Profile──> .memory/
                    组合 Quest ──resolve 固定的挂载声明───────────┘
```

- **Derivation** 是具体、不可变、可独立识别和读取的选择结果，不只是待执行的 recipe。
- **Environment** 直接挂载一个或多个 D。组合不会产生“合成 D”，也没有单数 owner D。
- **Quest** 的 recipe 决定结果类型：选择返回 D，组合返回 E。
- **Profile** 只交付 E；D 可以直接浏览和检查 provenance，但不能直接激活。

本轮实现 raw-only `file_tree`。D1 的信任/语义优先级、D3 的主动失效通知、external producer、local/generated payload、嵌套 E 挂载、子树裁剪与 GC 不在范围内。

旧格式不再是运行分支。旧 Store 不会被自动删除、迁移、重写或改号；遇到不支持的数据应显式失败，用户可自行选择在新的 Store 中重新选择和组合。

## 2. 对象职责与不变量

| 对象 | 保存什么 | 不负责什么 |
| --- | --- | --- |
| 选择 recipe / Quest | locators、选择参数、可变 name/intent、历史 | 不自动创建 E |
| D | 冻结 recipe、每个输入的实际解析、raw 输出、冻结说明 | 不因 mount 改变而重建 |
| 组合 recipe / Quest | 固定 D IDs、output role、mount、组合用途 | 不跟随上游 Quest/latest |
| E | 冻结组合 recipe、bindings、组合说明、可校验 closure | 不拥有一份合成 D |
| Profile | E 的有序激活历史与 current | 不直接指向 D，不自动刷新 |

必须满足：

1. 多个 E 可以复用同一个 D，D 的 identity、payload 和 provenance 不变。
2. 换 mount 或一个 binding，只改变组合结果；未变的 D 不重建。
3. `D:p` 与 `E:mount/p` 返回相同的、验证过的字节。
4. E 的文件集合从 bindings 计算，不另存一份权威 Derivation manifest。
5. 读取和 replay 不从 mutable Quest catalog 补齐冻结说明。
6. 不用 `bindings.front()` / `derivations.front()` 代表整个 E 的 owner 或消费归属。

## 3. Derivation：独立选择和浏览

### 3.1 内建选择

库的 JSON authoring 接口只接受 `memix.view.expr.v2` 的 typed context `inputs`；旧 v1、`sources` JSON、Environment 输入和忽略式字段解析已移除。源码 locator 列表构造器生成相同的 v2 expression。

`memix.view.select.v1` recipe 只接受 Store context 输入。canonical 字段顺序是 `schema`、`inputs`、`target`，输入顺序保留；`target` 当前只实现 `file_tree`。

选择发布 `memix.view.derivation.v4`：

- `recipe` 是完整 recipe；`ir_hash = SHA256(canonical recipe)`。
- `operator_version` 为 `select-raw-files-v1`。
- `resolved_inputs` 保存 raw 文件来源；`input_resolutions` 保存**每个** context 输入的解析，包括 optional 空选择及被输出去重遮蔽的输入。
- `resolved_view_inputs` 为空；`files` 只接受 `raw_blob`。未知输出类型不回退成 source。
- 每个文件记录 path、role、media type、size、content hash 和 source；当前 raw 文件的 role 为 `source`。
- `runtime_quest` 是冻结说明。recipe hash、解析记录、输出描述与说明都受 v4 basis 约束；创建时间不参与 identity。
- outputs 的 artifact ID 和路径必须属于这个 D，不能用 closure 中某个上游 artifact 冒充。

同路径、同 blob 的重复选择可去重为一个输出，但不能丢掉输入级解析记录。不同 blob 的文件碰撞，以及文件/目录冲突，均失败。这里没有 D1 的自动覆盖策略。

D 在临时目录中完整构造并冻结 manifest 与输出子目录。为兼容 macOS 14 的 rename 权限要求，仅临时目录根保持可写；重命名后立即冻结已发布的根目录，冻结失败则撤回发布并清理，撤回失败显式报告对象路径。缓存命中仍验证 manifest 和 identity，不就地修补旧结果。

### 3.2 浏览边界

- `cat` 输出实际校验过的 byte buffer，而不是检查完后再次打开路径。
- `ls VIEW [PATH]` 只列目录的直接子项；`tree` 支持 `--path` / `--depth`。
- `show --json` 展示完整 manifest，可检查 recipe、输入解析、输出和 provenance。
- 读取依据 manifest 与 Store blob，不信任可替换的 `tree/` symlink。组合和目录查询以固定缓冲区流式校验 size/hash，不为校验而整文件载入内存；实际读取与 Copy 仍校验交付的字节。
- 浏览不创建 E、Profile 或新的 resolution。
- `show` 与 Inspector 预览不记消费；成功的 `cat/ls/tree` 才记消费。

生成结果将来同样属于独立 D，但当前不接受 external recipe 或 `local` payload。不会为未实现的类型伪造 `StoredObject`。

## 4. Environment：直接挂载多个 Derivation

### 4.1 组合 recipe

`memix.view.compose.v1` 只接受固定 D ID，不接受 Store locator、Quest ID、`latest`、E ID 或子目录裁剪。ID 前缀只能在 D namespace 中解析，持久化时保存完整 ID。

以下是形状示例，尖括号内容不是有效 ID：

```json
{
  "schema": "memix.view.compose.v1",
  "bindings": [
    {"mount": "project", "derivation": "<project-derivation-id>", "output": "file_tree"},
    {"mount": "python", "derivation": "<python-derivation-id>", "output": "file_tree"}
  ]
}
```

- 至少一个 binding，只挂完整 `file_tree`。
- 根 mount 在 JSON 中是 `""`；CLI 省略 mount 或写 `.` 正规化到根。
- 非根 mount 是 relative POSIX path，拒绝空组件、`.`/`..`、尾部 `/`、反斜杠、绝对/drive/UNC 路径。
- 按 `(mount, derivation, output)` 排序；参数顺序不影响 recipe 或 E ID。
- 同 D 可以挂多次，目录可以合并；重复 binding、同最终文件路径（即使 bytes 相同）、文件/目录前缀冲突均失败。
- `README.md` 保留检查发生在**装配后的 E 根**，不是 D 根。绑定落入 E 的 `README.md` 或其目录前缀时拒绝发布。

### 4.2 Environment v3 与 identity

E 存在 `views/environments/<id>.json`，schema 为 `memix.view.environment.v3`，包含：

| 字段 | 契约 |
| --- | --- |
| `environment_id`, `created_at` | identity 与非 identity 时间 |
| `recipe`, `ir_hash` | canonical compose recipe 及其 hash |
| `runtime_quest` | 冻结组合说明 |
| `projection_version` | `mount-file-trees-v1` |
| `bindings` | 与 recipe 一致的 canonical bindings |
| `closure` | 从实际 bindings 重算并精确比较的集合 |

```text
ir_hash = SHA256(canonical compose recipe)

environment_id = SHA256(canonical {
    schema: "memix.view.environment.basis.v3",
    ir_hash,
    runtime_quest,
    projection_version,
    bindings
})
```

basis 的字段顺序如上；binding 字段顺序是 `mount`、`derivation`、`output`。ID 是 lowercase SHA-256，不使用主机绝对路径或隐式 Unicode 正规化。

`created_at`、closure 缓存、物化树和 insertion 状态不进入 basis。closure 不入 hash **不表示可被信任**：额外、遗漏、重复或伪造的成员都必须被拒绝。

同 bindings、同冻结说明得到同 E。修改组合 Quest 注释后重新 compose 可以得到新 E，但不会改变 D。singleton E 和多 binding E 使用同一个协议。

### 4.3 输出与 closure

当前 raw-only 图只有 `E → D → raw blob`，不接受嵌套 E 或伪装成 raw 的生成数据：

- `derivations` 是直接 bindings 引用的 distinct D IDs。
- `artifacts` 是这些 D 声明、且经直接归属验证的 output IDs。
- `raw_objects` 是所需 raw blob IDs。
- `environments` 当前为空。

集合排序、去重。E 的输出从 D 的 manifest 计算，每个文件落到 `mount / output_path`；不从第一个 D 的缓存树列举，也不递归扫描任意目录作为输出来源。发布、读取和激活 E 时验证其完整引用；实际交付验证 size/hash。

未来 external evidence 会扩大这张依赖图，其递归 closure、循环检测与 generated owner 协议仍属于 derived 提案，不能把当前 raw-only 验收说成已经支持任意混合图。

## 5. Quest：typed results、历史与消费

`memix.quest.v2` metadata 保存 `ir_hash`、`result_kind`、`materialization`、name/intent、created_at。当前 `materialization` 只支持 `builtin`：

| Recipe | 结果 | resolve |
| --- | --- | --- |
| `memix.view.select.v1` | `derivation` | 解析 Store refs，产生或复用 D |
| `memix.view.compose.v1` | `environment` | 验证 pinned D，产生或复用 E；不产生 D |

类型由 canonical recipe 校验。历史使用 `memix.quest.resolution.v2`：

```json
{
  "schema": "memix.quest.resolution.v2",
  "resolved_at": "<timestamp>",
  "result": {"kind": "derivation", "id": "<derivation-id>"}
}
```

库的 `QuestInfo::latest_result` 是 optional tagged reference，不再用两份可空字符串推断类型。E 历史使用 `kind: "environment"`。一个 Quest 的历史不能混入另一种结果。`select/compose/resolve --json` 返回 `quest`、typed `result`、`inserted`；`define --json` 声明 `result_kind`，`quests/find --json` 提供 `latest_result`（未 resolve 为 `null`）。

发布对象后、追加 history 成功后才能报告 materialization 成功。history 失败可能留下可复用的对象，错误包含已经发布的 kind/ID 与 Quest ID；不切换 Profile。损坏或未终止的历史拒绝继续追加，不自动修复。

### Freshness

选择暂沿用按需、类型 generation 粒度的 ref 检查，完整保留 optional/重叠输入解析。组合只 pin D，没有 followed-ref inputs，故无 ref drift；这不表示语义最新。替换 D 会改变组合 recipe 和 Quest ID。D3 的 reverse index、通知和细粒度语义 freshness 仍未实现。

### Receipts

- 只使用 `receipts.jsonl`，格式为 `memix.quest.receipt.v2`，含 typed `result`；**没有 sidecar**。
- D 的 `cat/ls/tree` 归其选择 Quest；E 的读取、激活、rollback 归组合 Quest。
- 一次 E 操作只记一次，不向 mounted D 的 Quest 扇出消费事件。
- `ls` 单独计数；统计分别列出 D reads 与 E reads，不给 D 伪造 E ID。
- 失败读取不记成功 receipt；成功读取/激活的 receipt 仍是 best-effort，缺失 catalog 不阻止 replay。
- `stats` 对损坏日志显式报错；列表的 history/receipt enrichment 仍是 best-effort，不能将缺少统计解释为“从未使用”。完整恢复工具不在本轮。

## 6. 当前公共接口

```bash
# 选择返回 D 与其 Quest ID，不创建 E。
memix view select --source 'rules/lang/@latest' --intent 'Shared Python rules'
memix view list --kind derivation
memix view ls "derivation:$D_PYTHON"
memix view tree "derivation:$D_PYTHON" --path rules --depth 2
memix view cat "derivation:$D_PYTHON" rules/lang/python.md
memix view show "derivation:$D_PYTHON" --json

# 组合返回 E 与独立的组合 Quest ID。
memix view compose --mount "$D_PYTHON=python" --mount "$D_PROJECT=project" \
  --intent 'Project development context'
memix view cat "environment:$ENV" python/rules/lang/python.md
memix view activate "environment:$ENV" --into ./.memory
```

- `select` 用 repeatable `--source`；`compose` 用 repeatable `--mount DERIVATION[=MOUNT]`；都支持 `--intent`、`--name`、`--json`。
- `define --kind selection --source ...` / `define --kind composition --mount ...` 只登记 recipe；省略 `--kind` 默认 selection，不执行旧 define 逻辑。
- `cat/ls/tree/show` 接受 `derivation:ID` / `environment:ID`；裸 ID 固定表示 E，不跨 namespace 猜测。
- 默认 `list` 列 E，`list --kind derivation` 列 D。
- `activate` 只接受 E。交付单个 D 也必须先显式 compose。
- `build`、`--view` 和库的单步 `build` 已移除，不提供兼容 wrapper。

库入口为 `select`、`compose`、`define_composition`、`resolve_derivation`、`resolve_environment`、typed `resolve_quest`、`view_tree`、`view_ls`、`read_output`。`read_output` 返回验证后的 bytes 和 provenance；`record_receipt=false` 用于 Inspector。当前文件 DTO 为 raw-only `ResolvedViewFile`。

`ViewEnvironment` 保存 bindings、多个加载后的 D、computed files 和自己的 Quest ID，不再有单数 `.derivation`。TUI/Inspector 的 E 浏览使用完整输出集合；D 的独立 CLI 浏览不依赖 GUI 包装 E。C++ API/ABI 与 JSON 契约均为破坏性更新。

## 7. 激活、说明与 Profile history

- Link 与 Copy 都使用验证后的 E 输出，不信任 cached D `tree/`。
- Copy 在临时目录复制并校验最终 bytes，再安装到 workspace；激活失败恢复 current 并清理未提交的 generation。
- README 使用 E 与各 canonical binding 的冻结说明，列出所有 mount/D/output，不把第一个 D 当成整体。
- catalog 的修改、删除不改变保存的 D/E 或 replay bytes；重建才能冻结新的说明。
- `memix.view.activation.v2` / `memix.view.activation.marker.v2` 保存 profile、generation、environment、activated_at、mode、workspace；**没有单数 derivation 字段**。
- rollback 指向记录的 E，重放其 bindings，不跟随 Store refs，也不重新选择。

## 8. 格式边界：没有兼容路径

| 对象/操作 | 当前支持 |
| --- | --- |
| 选择 | select recipe v1 → raw-only D v4 |
| 组合 | compose recipe v1 → E v3 |
| Quest / history / receipts | v2 / v2 / v2 |
| Profile generation / Copy marker | v2 / v2 |
| D v1/v2/v3、E v1/v2、旧 Quest/日志/Profile | unsupported；不转换、不改号、不 replay 适配 |
| external recipe / `local` payload / derive | 尚未实现，显式拒绝 |

不会自动升级用户 Store，也不会为旧数据生成新 ID 后声称等价。验证全部使用临时 Store；没有运行用户数据迁移脚本。

## 9. 与 derived 提案的共同边界

未来 `derive` 提交独立 D，再由现有 compose 路径交付。generated bytes 留在 owning D，不变成 raw Store blob；invisible evidence 仍需 closure。外部 evidence 首版拟 pin E，多 binding E 不能被压成单数 owner D。上述扩展尚未实现，详见 [DERIVED_ARTIFACTS.md](DERIVED_ARTIFACTS.md)。

论文在 D2 上保留为目标分层，不修改 PDF。D1/D3 不随本轮自动宣布完成。

## 10. 验收与验证

已加入回归：独立 D 且无包装 E；直接多 binding 且 D 数不增加；canonical 排序/根 mount；碰撞；typed history/receipts 与独立消费归属；D/E byte equality；payload 损坏、伪造 closure、输出归属与未知字段拒绝；旧 schema 拒绝且不改写；冻结说明；E-only Link/Copy 与多 D rollback；损坏日志拒绝追加；CLI typed workflow。

当前运行记录见 [设计审查验证](DESIGN_ALIGNMENT_REVIEW.md#6-已执行的审查验证)。构建和验证必须经 `nix develop --command …`，使用 **Ninja**，包含完整测试、`all_verify_header_sets`、格式检查和 **clang-tidy**。工具链限制和未执行的平台验证必须如实记录，不能用历史测试结果替代本次检查。
