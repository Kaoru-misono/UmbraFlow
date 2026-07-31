# Atlas extraction reference

## Module table

| key | 范围 | knowledge 背景文档 |
| --- | --- | --- |
| `core` | `modules/core` | `docs/knowledge/cn/module-core.md` |
| `domain` | `modules/domain` | `docs/knowledge/cn/module-domain.md` |
| `vision-image` | `modules/vision` + `modules/image`（一份 JSON 覆盖两个模块） | `docs/knowledge/cn/module-vision-image.md` |
| `annotation` | `modules/annotation` | `docs/knowledge/cn/module-annotation.md` |
| `controller` | `modules/controller` | `docs/knowledge/cn/module-controller.md` |
| `engine` | `modules/engine` | `docs/knowledge/cn/module-engine.md` |
| `script` | `modules/script/source`（跳过 vendored Luau：`modules/script/external/`） | `docs/knowledge/cn/module-script.md` |
| `entry-cli` | `entry/cli`（`entry/m0-demo` 已冻结，summary 一句话带过即可） | `docs/knowledge/cn/entry-cli.md` |
| `entry-input-agent` | `entry/input-agent` | `docs/knowledge/cn/entry-input-agent.md` |
| `entry-workbench` | `entry/workbench` | `docs/knowledge/cn/entry-workbench.md` |

## JSON schema

```json
{
  "module": "<key>",
  "title": "<模块标题，如 modules/engine — 运行时编排>",
  "summary": "<2-4 句中文：模块负责什么、在系统里的位置>",
  "notOwned": ["<不负责的事，每条一句，可选，最多 6 条>"],
  "dependsOn": ["core", "domain"],
  "readingOrder": [{"path": "<仓库相对路径>", "why": "<为什么先读它>"}],
  "classes": [
    {
      "name": "<完全限定名，如 uf::engine::EngineSession>",
      "kind": "class|struct|enum|functions|alias|concept",
      "file": "<仓库相对路径，正斜杠>",
      "line": 42,
      "responsibility": "<1-2 句中文：它为什么存在、守住什么不变量>",
      "members": [{"sig": "<签名，保持代码原样>", "note": "<一句话中文>"}],
      "lifetime": "<所有权/生命周期/线程契约，无特别契约则省略>",
      "related": ["<完全限定名，只列本仓库类型>"]
    }
  ],
  "flows": [{"title": "<流程名>", "steps": [{"text": "<一步一句>", "file": "<可选>", "line": 0}]}]
}
```

## Reader agent prompt template

> 你在为 C++23 仓库 `<repo root>` 生成「代码地图集」的结构化数据，渲染成帮助仓库作者阅读源码的 HTML 参考页。
>
> 目标模块：`<title>`；范围：`<paths>`。
>
> 1. 先读 `<doc>` 作为背景（文档可能滞后，以代码为准）。
> 2. 读模块 manifest.txt 和全部 `.hpp`，必要时打开 `.cpp` 确认行为；tests/ 可用来确认公开用法，但不为测试文件建条目。
> 3. 结果写入 `scripts/generate_code_atlas/data/<key>.json`（UTF-8），写完用 `python -c "import json,io;json.load(io.open(r'<path>',encoding='utf-8'))"` 校验。
>
> 硬性要求：
> - 散文中文；标识符、签名保持原样。
> - 每个公开 class / struct / enum class / 重要自由函数组必须有条目（自由函数按主题聚合，kind="functions"，name 用如 `uf::core::checked-arithmetic` 的主题名）。仅存在于 .cpp 的内部 helper 可省略。
> - `line` 必须用 Grep -n 或 Read 核实的真实声明行号，禁止估算。
> - members 只列公开表面，每类 ≤12 条；enum 列全部枚举值（多时分组概括）。
> - flows 只在有清晰运行时序时给 0-2 条。
>
> 返回值：只返回 `{"module":"<key>","path":"<写入路径>","classCount":N}`。

## sig 字段规范

`members[].sig` 会被 render.py 解析成「函数名 / 参数 / 返回值」并重排展示，因此必须是
**单条、干净的声明**：

- 一条 sig 只写一个声明。拷贝/移动构造这类成对出现的，拆成两条 member（或用顶层 `;`
  分隔，渲染器会拆开，但不推荐依赖）。
- 禁止写定义片段（`class Foo { ... }`、`struct X : Base... { using ...; }`）。展示一个
  类型的存在用类型名本身做 sig（如 `class ResolvedPage`），细节写进 note。
- 函数尽量用 trailing-return 原样（`click(Point<ClientSpace>, ObservationLease const&) -> Status`）；
  渲染器会自动补 `auto` 和折行。
- 数据成员、枚举值照抄声明即可，不带 `{ }` 花括号体。

## Gap backfill rules

verify.py 的 `GAP` 行逐个三选一：

- **A 嵌套/细节**：已收录类型的嵌套类型、`detail`/private 实现——不新建条目；若调用者会接触（参数/返回值），确认父条目 members 或 related 里可见。
- **B 公开顶层类型**：调用者直接使用——按 schema 追加完整条目，插在语义相邻处。
- **C tag 聚合**：一组无成员 tag（如坐标空间标签）——聚合为一条，kind="struct"，name 用主题名（如 `uf::domain::space-tags`），line 指向第一个 tag 的真实行号，members 每 tag 一行。

主题聚合条目（name 含 `-`，非真实标识符）verify.py 自动放行，只校验行号范围。

## 已知的预期 GAP

`SourceProvenance`/`RegressionExpectation` 的 variant 成员、controller `detail/` 访问桥、
各类 passkey tag（`OpenTag`、`HandleKey`）、私有嵌套结构（`Payload`、`Slot`、
`CandidateReport`、`GrayTemplate` 等）——均已确认为规则 A，留在 GAP 列表即可。
