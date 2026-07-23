# 真机 UI 验证 runbook（等开发者启动一次提权代理）

> 状态:已完成并归档(2026-07-24)——2026-07-21 真机验收已通过，结果记录于 `docs/TODO.md` §0。

> 目的：验证「卡厄思梦境」角色详情页的 UI 交互——点头像切角色、点标签切内容。
> 提权由**开发者手动启动一次**（AI 无法自我提权，安全门所致）；之后 AI 非提权
> 驱动 input-agent 完成全部点击+截图对比，无需再提权。
> 目标窗口：主窗 hwnd=0x20CFE，class GLFW30，标题「卡厄思梦境」，1600x900。
> 捕获已真机验证：delta=(0,0)，非提权可截图。

## 开发者的唯一动作：启动提权代理（一条命令，批准一次 UAC）

```powershell
$q = "$env:TEMP\uf-queue.jsonl"; $r = "$env:TEMP\uf-results.jsonl"
$o = "$env:TEMP\uf-out"; New-Item -ItemType Directory -Force $o | Out-Null
Set-Content -Path $q -Value "" -Encoding utf8   # 先由非提权侧创建队列文件
Start-Process -Verb RunAs -FilePath "E:\github\umbraflow-cpp\build\x64-debug\bin\m0-demo.exe" `
  -ArgumentList @("input-agent","--hwnd","0x20CFE","--queue",$q,"--results",$r,"--output-dir",$o,"--idle-timeout-s","600")
```

`--output-dir` 是必需的安全边界：所有截图输出被限定在该目录内（提权 agent
不能被非提权驱动诱导写到目录外的受保护文件）。输出文件名用相对路径、且必须是
**新文件**（`CREATE_NEW` 独占，不覆盖已存在文件）；`settle_ms` 上限 5000。

启动后代理常驻、每 ~100ms 轮询队列文件。开发者到此为止，可离开。

## AI 驱动的点击验证（非提权，往队列追加命令）

坐标为客户区像素（1600x900，delta=(0,0) 所以 = 帧像素）。当前状态：
选中头像=賽雷妮爾（顶部带边框），选中标签=能力值。

输出文件名为相对 `--output-dir` 的**新文件**（不覆盖已存在文件）。

验证 1 — 头像切换（点第 2 个头像应换角色）：
```json
{"op":"capture","out":"before-avatar.png"}
{"op":"click","x":45,"y":228,"out_before":"av-b.png","out_after":"av-a.png","settle_ms":600}
```
判据：av-a.png 右侧角色名/能力值 ≠ av-b.png（角色已切换）。

验证 2 — 标签切换（点「卡牌」应换内容）：
```json
{"op":"click","x":185,"y":347,"out_before":"tab-b.png","out_after":"tab-a.png","settle_ms":600}
```
判据：tab-a.png 中间内容从"能力值/攻擊力…"切成卡牌界面。

其余可选标签坐标（客户区）：夥伴 ~(185,430)、存檔資料 ~(205,515)、
潛力 ~(185,600)、記憶碎片 ~(205,683)、顯現自我意識 ~(225,768)。
其余头像 y（x≈45）：#3≈345、#4≈452、#5≈558、#6≈665、#7≈770。

结束：`{"op":"quit"}` 让代理退出。

## AI 的判定方式

对每对 before/after PNG 缩小后 Read 目视对比，确认 UI 确实按预期切换；
把结论（切换成功/失败 + 观察）写进报告。全程只做角色详情页导航
（头像、标签），不做开发者未授权的其他操作。
