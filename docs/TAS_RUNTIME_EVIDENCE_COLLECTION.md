# TAS 文件恢复：第一轮实机证据采集

状态：源码准备，未编译、未运行。诊断版本 `tas-project-archive-4-runtime-evidence`。
本轮只收集正常保存产生的数据，不尝试文件恢复，不加 Hook，不修改原生加载条件。
仍遵守用户“不编译／不触发 CI”指示；下面步骤供之后允许构建、部署时使用。

## 为什么现在需要真实对局数据

静态证据（VA，以 0x400000 为 image base）：
- 0x785B30 遍历 context +2C/+30 的注册表，通过 vtable +0 保存。
- 0x785CB0 / 0x785520 分别通过 context +B0/+B4 的注册表调用 vtable +0/+4。
- context +00 的 12 字节任务模板复制到 +14 工作队列（0x7852B0），条目为
  `{object, partitionIndex, partitionCount}`（0x785920）。保存/加载分派分别在
  0x785BC0 / 0x785430；主线程和三个 worker 都会消费，0x785D40 是 worker 循环。
- 0x785CB0/0x785520 返回前等待三个事件。队列锁未占用不等于所有消费者完成。
- 首段 raw / addressed 的实际顺序、动态长度，由上述实例及内部指针/计数决定。
  反汇编可继续研究函数，但不能提供这一次真实保存的注册对象及对象状态。
- +178 的缓存命中值会被 0x4BF9FA 后的路径直接返回，未命中则在 0x4BFAEE
  调用另一个运行时对象的 vtable +14，再在 0x4BFB48 缓存结果。不能擅自清零。
- +174 的写入/定长受 context +184 开关控制（0x785BA0、0x785B62）。未启用时
  选中槽内容可能没有本次更新；必须先观察实际配置，不依据默认值推翻现有行为。

**下一处需要输入：目标对局 X/Y 的注册表、真实虚函数目标、任务分片及辅助开关。**
这不是宣称所有静态工程已完成；拿到这些目标后仍要沿具体方法继续静态研究。

## 采集机制和限制

环境变量 `BBCF_TAS_CAPTURE_EVIDENCE=1` 开启；否则报告为 opt-in-disabled。
在 A、B 原有 post-save 边界各执行一次，附入 V4 `.bbtas`，不每帧采集。
证据也以 `[TAS][EVIDENCE] begin/end` 分块写入现有日志；启用正常日志并保留本次文件。
若辅助捕获失败无法导出，先交付这些日志即可，不要求反复覆盖槽。
A归档失败时B归档可能被既有条件跳过，因此日志里只有A证据也应照实提交。
`VirtualQuery` 过滤未提交/guard/no-access/特殊映射后使用 `ReadProcessMemory`。
对象前缀最多读取 0x60 字节作为诊断观察，不声称是对象边界；不递归追随其中指针。
只采集：context 注册/任务队列、对象虚表与三个表项、主模块内方法前16字节、
十槽描述、复制目的地映射、+17C/+180 回调及 +17C 全局包装对象、PID/创建时间/PE信息。
报告最多250 KiB左右，文件字段上限256 KiB/base；任何截断标为不完整。

它不是调用轨迹：没有记录每次 CA0/CD0 发生顺序或消费者线程时序。
`observedStable=1` 不是对象生命周期；`captureComplete=1` 不是恢复资格。
某槽读取失败不代表随机波机制失效。所有结果仍 `nativeLoadAllowed=false`。
采集会增加保存时耗时；可能影响时序，必须单独看观察开关有无回归。
文件含内存字节和地址，应私下交付，不上传公共 issue。不需要完整进程转储。

## 最小操作（之后构建部署后）

1. 保留原有稳定 DLL、设置和连段文本，不覆盖唯一副本。使用含上述版本号的新 DLL。
2. 让 BBCF 进程继承 `BBCF_TAS_CAPTURE_EVIDENCE=1`，不要用 `setx` 做全局持久改动。
   例如在 PowerShell 中设置 `$env:BBCF_TAS_CAPTURE_EVIDENCE='1'` 后从同一环境启动游戏。
   如果由已运行的 Steam 创建游戏，它可能不继承该变量；不要假设设置成功，检查报告。
   不自动重启或终止 Steam/游戏。用户自行决定启动方式。
3. 进入原来能固定随机波的 Training Match，保存 base，等待 A/B 保存流程完成。
   使用原逐帧连段正常播放确认仍正常；导出 `X.bbtas` 和输入文本备份。
4. **不要为了诊断读回旧文件并强制加载。** 在同一对局改变位置/资源，用正常 Save base
   覆盖旧 A/B 为 Y，导出 `Y.bbtas`。这一步会替换编辑器当前 base；先完成 X 导出。
5. 用现有 Read / validate archive 分别读取 X、Y。预期仅校验，不恢复、不摆位。
6. 私下提供 X、Y 和本次日志；说明两个人物、场景、X连段及固定随机波结果，是否有采集卡顿。
   若导出不可用/报错，停止。提供明确错误与日志，不反复开关诊断、不继续覆盖更多槽。

先只做同一对局。重进对局、重启游戏恢复都不是本轮测试。

## 离线提取（不接触游戏）

Windows Python 3，仓库根目录执行（选择新的输出文件名）：

```powershell
python tools/inspect_tas_archive.py 'D:\TAS-evidence\X.bbtas' > 'D:\TAS-evidence\X-report.json'
python tools/inspect_tas_archive.py 'D:\TAS-evidence\Y.bbtas' > 'D:\TAS-evidence\Y-report.json'
```

需检查进程退出码为0；脚本在整文件校验成功后才输出 JSON。它是证据提取器，不等同
C++完整结构预检，更不提供恢复许可。A/B 的 runtimeEvidence 不应为 opt-in-disabled。
不需要用户解释十六进制内容，直接交付文件即可。

本地文件 SHA256（脚本不启动/附加游戏，只创建一个新 JSON）：

```powershell
.\tools\collect_tas_identity.ps1 -GameDirectory 'D:\SteamLibrary\steamapps\common\BlazBlue Centralfiction' -OutputFile 'D:\TAS-evidence\identity.json' -Archives @('D:\TAS-evidence\X.bbtas','D:\TAS-evidence\Y.bbtas')
```

在运行前确认实际游戏目录，采集期间不替换 EXE/DLL。磁盘哈希不是已加载镜像的证明。
脚本本轮未执行，PowerShell/C++兼容性仍须后续验证。

## 收到数据后的判定与后续

- 先核对 V4、版本标识、哈希、同一 PID/创建时间、A/B帧差、物理槽及管理器身份。
- 区分辅助流未启用、捕获失败、结构不支持；不把它们混称文件损坏。
- 用注册表/虚表 RVA 缩小反汇编到真正参与保存的对象，建立 raw/addressed 调用模式，
  并核对首段字节总量和 +0C 操作数。不能用字节相似匹配代替证明。
- 对比 X/Y 历史及回调目标，继续追查返回句柄/资源的实际分配释放路径；相同地址不够。
- 如果仍缺动态调用次数，只对已经识别的少量入口设计下一轮有界轨迹采集，单独审查
  ABI、线程、寄存器/标志保护。不预装广泛 Hook，也不要求现在提供崩溃转储。

## 原生恢复事务仍未实现，禁止越过的门槛

1. 冻结编辑器操作，校验完整文件和同一进程/对局/对象生命周期；未知就拒绝。
2. 确认原生 worker 已完成；不拿 spin-lock 或自行调用等待事件来猜测同步。
3. 在当前已注册且由 TAS 拥有的槽建立候选；只改已证明的源指针/游标。
   不能覆盖 whole manager、事件句柄、共享槽和回调对象。
4. 恢复涉及的辅助历史及资源必须有明确合并/重建策略；不清空伪装成功。
5. 写入前备份可回滚范围。任何写入/加载异常后隔离槽并停止播放，不发布新电影。
   memcpy回滚不能撤销已执行回调的外部副作用，故不能承诺任意异常都能恢复旧局面。
6. 原生加载成功且验证状态之后才发布输入/base/游标，禁用旧 keyframe/undo 引用。

完成这些门槛后，才执行真正的 X导出→Y覆盖→X文件恢复→原连段/随机波一致验收。