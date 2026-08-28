# BBCF Improvement Mod

[English](README.md) | [中文](README.zh-CN.md)

`BBCF Improvement Mod` 是为《BlazBlue: Centralfiction》制作的功能扩展 Mod。它通过 `dinput8.dll` 注入游戏，提供训练辅助、逐帧分析、录像接管、自定义调色板、额外游戏模式及多项实验功能。

本项目与 Arc System Works 及其关联方没有从属关系。

## 主要功能

- 训练模式增强：命中框显示、帧历史、帧数据、连段信息、训练假人状态与录制槽。
- TAS 逐帧编辑器：支持 P1/P2 同步逐帧输入、基础状态、回退重录、预览和展示播放。
- Replay Takeover：在回放中接管 P1/P2 输入并加载状态。
- 本地回放文件加载与回放相关工具。
- 自定义调色板与特效：可创建、编辑、加载 `.cfpl` 和兼容的 `.hpl` 文件；在线双方均使用 Mod 时可同步调色板信息。
- 额外游戏模式和图形选项。
- 可扩展覆盖层本地化，详见 [docs/localization.md](docs/localization.md)。

## 安装

1. 从 Release 下载发布包。
2. 将 `dinput8.dll` 与 `settings.ini` 放入游戏根目录，即 `BBCF.exe` 所在目录。
3. 如需自定义调色板，将 `palettes.ini` 一并放入游戏根目录。
4. 启动游戏；覆盖层、日志目录和调色板目录会按需创建。

通常不需要自行编译。卸载时删除或重命名 `dinput8.dll` 即可，Mod 不会永久修改游戏文件。

> 建议只使用已合法购买且保持最新版本的游戏。在线功能、对战平衡和官方更新兼容性均由使用者自行判断。

## 覆盖层操作

- 默认使用 `F1` 打开或关闭主覆盖层；可在 `settings.ini` 调整快捷键。
- 拖动窗口空白区域可移动窗口；窗口标题栏双击可折叠。
- 调色板编辑器右下角可拖动调整大小，双击可按内容自动适配。
- `Ctrl` 配合点击滑块或数值控件可直接输入数值。
- 覆盖层窗口位置异常或消失时，删除游戏根目录的 `menus.ini` 可重置窗口布局。

## TAS 逐帧编辑器

TAS 仅限训练对局使用。输入会在游戏每帧的最终 BattleInput 双槽位写入，因此 P1/P2 都可独立操作，不受“主操控位”选择影响。

### 输入格式

P1 与 P2 输入框使用数字方向键记法：

- 方向：`7 8 9 / 4 5 6 / 1 2 3`，其中 `5` 为中性。
- 每个方向数字占用一帧：`66` 为连续两帧前，`656` 为前、中性、前。
- 按键字符会附加到紧邻的前一个方向帧：`5C` 为中性+C，`623C` 为 6、2、3+C，`28D` 为 2、8+D。
- P1/P2 指令长度不一致时，较短的一侧自动补中性 `5`。
- Movie 不设置人为的帧数上限，实际受可用内存和平台可寻址容器大小限制。
- 指令文本支持使用空格、逗号和连字符作为分隔符。
- 按键使用 `A`、`B`、`C`、`D` 表示；多个按键可以附加在同一帧方向上。

### 快捷键

TAS 窗口处于活动状态时可以使用以下快捷键，也可以在 `settings.ini` 中修改：

| 按键 | 默认设置项 | 功能 |
| --- | --- | --- |
| `I` | `TasParseKeybind=I` | 解析当前 P1/P2 指令，等同于点击 `Parse input`。 |
| `L` | `TasAdvanceKeybind=L` | 按当前设置的 `Frame count` 前进；处于 Movie 编辑状态时，从当前 Movie 位置追加或重新录制这些帧。 |
| `J` | `TasRewindKeybind=J` | 按当前设置的 `Frame count` 后退，加载基础状态并回放到目标位置，同时删除目标位置之后的 Movie 帧。 |

`Frame count` 在 TAS 窗口中设置，必须为正数；Movie 不设置人为的帧数上限。使用快捷键可以减少鼠标在输入框、帧数控件和操作按钮之间来回移动，更适合反复测试连段。Presentation 播放期间 TAS 界面会隐藏，此时按 `I` 的作用是停止播放，而不是解析指令；界面恢复后，`I` 才会执行正常的解析操作。

### 制作连段

1. 进入训练模式并打开 TAS 窗口，点击 `Enter TAS mode`。
2. 在连段起点点击 `Save base state`。后续预览、回退和重录均以此状态为基准。
3. 在 P1/P2 输入框填写下一段指令，点击 `Parse input`。
4. 设置 `Frame count`，点击 `Advance N frames`。
5. 游戏按设定帧数推进，解析后的输入同步写入 Movie。
6. 重复解析与推进，直至连段完成。

`Input progress` 是已消费的解析指令帧数；`Movie frames` 是已写入 Movie 的总帧数。

### 编辑机制

编辑器中有三类相关数据：

- **解析指令**：P1/P2 输入框解析后的临时帧序列，等待写入 Movie。
- **Movie**：已经追加到录像中的帧序列，Preview 和 Presentation 都播放这部分内容。
- **基础状态**：训练模式的原生快照，是预览、后退和重新录制时使用的确定性起点。

应当在连段真正的起点准确点击 `Save base state`。再次保存会替换之前的基础状态，并将编辑游标重置到开头。加载基础状态也会把播放位置重置到第 0 帧，同时增加一次 `Rerecord count`。

第一次点击 `Advance N frames` 或按下 `L` 时，帧会追加到 Movie 末尾。在 Preview 或 Rewind 后处于已有 Movie 的编辑状态时，同样的操作会删除当前编辑位置之后的所有帧，并从当前位置写入新帧。因此可以只修改连段后半段，不需要重新制作前面的内容。

如果解析指令短于要求前进的帧数，剩余帧会使用中性输入；如果要求前进的帧数超过当前解析指令长度，超出部分同样使用中性输入。P1 和 P2 始终独立写入，缺少输入的一侧会自动补中性帧。

### 完整制作示例

1. 进入训练对局并启用 TAS 模式。
2. 使用 `Save base state` 保存连段起点。
3. P1 输入 `623C`，P2 输入 `5`，然后按 `I` 或点击 `Parse input`。
4. 将 `Frame count` 设置为要消费的帧数，按 `L` 或点击 `Advance N frames`。
5. 持续解析新指令并前进，直到完成第一版连段。
6. 使用 `Preview playback` 从基础状态观看 Movie；预览会在 Movie 最后一帧冻结。
7. 输入下一段指令，按 `I` 解析，再按 `L` 从冻结的末尾继续追加帧。
8. 如果要修改前面的部分，按 `J` 按当前帧数后退，输入替代指令后再按 `L`，即可从该位置重新录制。
9. 如果只想让当前解析指令全部变成中性输入并保留输入框文字，使用 `Reset parsed input`；如果要清空完整录像，才使用 `Reset movie`。

### 预览与展示

`Preview playback` 用于继续制作尚未完成的连段：

1. 自动加载基础状态。
2. 连续播放当前 Movie。
3. 录像末帧结束后冻结在当前连段状态。
4. TAS 窗口重新可操作，可继续解析新指令并使用 `Advance N frames` 追加后续帧。

`Presentation playback` 用于观看完整连段：

1. 自动加载基础状态。
2. 先连续执行 60 帧中性 `5`。
3. 连续播放 Movie。
4. 再连续执行 240 帧中性 `5`。
5. 播放结束后恢复正常游戏流程。

展示播放期间 TAS UI 会隐藏；按 TAS 配置中的解析快捷键可停止播放。

### 回退、重录与重置

- `Rewind N frames`：重新加载基础状态，回放到当前编辑位置减去 N 帧，删除该点之后的 Movie，并冻结在回退点以便重录。
- `Reset movie`：停止播放、加载基础状态、清空 Movie，并将编辑位置归零。
- `Reset parsed input`：保留 Movie、基础状态和输入框文本；将当前已解析的 P1/P2 指令帧全部改为中性 `5`，且将解析游标归零。再次点击 `Parse input` 可从输入框文本重新生成指令。
- `Resume game`：解除逐帧冻结，让游戏正常推进。确认不再继续编辑当前连段后再使用。
- `Import` / `Export`：读写游戏工作目录中的 `tas_movie.txt`。导入后应在匹配的起点重新保存基础状态。

## Frame History

Frame History 为双方角色分别显示状态行和属性无敌行。非闲置帧以色块表示状态：

- 绿色：启动。
- 红色：活跃。
- 蓝色：收招。
- 黄色：防御硬直。
- 紫色：受击硬直。
- 青色：特殊状态，例如冲刺。
- 其他颜色：无法归入上述类别的状态。

属性无敌以叠加图形标识头部、身体、足部、飞行道具和投技等类别。

## 调色板

首次启动后，调色板目录位于 `BBCF_IM/Palettes/`。将 `.cfpl` 或 `.hpl` 文件放入对应角色目录即可在游戏内加载和选择。

旧 `.hpl` 特效文件使用 `<palette>_effectXX.hpl` 命名，例如 `Nyx_Izanami_effect01.hpl`。以 `_effectbloom.hpl` 结尾的文件会启用 Bloom 特效。`palettes.ini` 可将自定义调色板分配给游戏内调色板槽位。

## 崩溃报告与排障

发生崩溃时，Mod 会在 `BBCF_IM/CrashReports/Crash_<timestamp>/` 创建报告目录，其中包括：

- `crash.dmp`：崩溃转储与嵌入式日志。
- `logs.txt`：最近日志环形缓冲区。
- `crash_context.txt`：异常信息与 Mod 元数据。

反馈问题时，请压缩并提供完整的 `Crash_<timestamp>` 目录。

常见排查步骤：

1. 临时移除或重命名 `dinput8.dll`，确认问题是否仍可复现。
2. 确认游戏根目录存在 `settings.ini`。
3. 关闭会注入或覆盖 D3D 的工具，例如录屏软件、性能监控覆盖层等。
4. 显示异常或黑屏时，检查 `settings.ini` 中的渲染宽高是否与游戏显示设置匹配。
5. 覆盖层窗口丢失时，删除 `menus.ini` 重置位置。

## 开发构建

当前工程使用 Visual Studio 的 `v143` 工具集和 Windows SDK。打开 [BBCF_IM.sln](BBCF_IM.sln)，选择：

```text
Configuration: ReleaseDeploy
Platform:      Win32
Toolset:       v143
```

构建产物为 `bin/Release/dinput8.dll`。发布或手动安装时，将 DLL、`settings.ini` 和需要的 `palettes.ini` 放入游戏根目录。ReleaseDeploy 使用静态运行库，适合普通用户分发。

## 致谢

感谢以下参与者及 BBCF PC 社区提供的开发、测试、反馈与支持：

- GrimFlash
- KoviDomi
- Neptune
- Rouzel
- Dormin
- NeoStrayCat
- KDing
- PC_volt
- MorphRed
- Tadatys (sublimacija)

另外感谢 Atom0s 的 DirectX 9 Hooking 文章，以及 Durante 的 dsfix 源码。

## 免责声明

```text
BBCF Improvement Mod 与 Arc System Works 及其合作伙伴或关联方无关。
本项目不应被用于恶意用途、获取不公平线上优势，或解锁未发布、未购买的内容。
请仅在已合法购买和拥有的官方游戏副本上使用。

使用本 Mod 的风险由使用者自行承担。项目维护者不对使用本程序造成的任何后果负责。
```