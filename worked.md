# 三代同步二代：当前进度（2026-10-10）

本轮实现、资源同步、桌面回归和 ARM64 交叉构建已完成。三代 YDP03X 使用 **800×254 横屏**。
本轮未连接设备，未部署或进行实机验收，也未创建 Git 提交或发布更新。
下一步是使用配套验机包验证真实设备的音乐、Home 解锁和原生 ASR 事件顺序。

## 用户要求及完成情况

- PenMods 设置接近三代原厂：使用原厂返回栏、圆角背景、80×44 开关，以及标题 28px、值 26px、说明 24px 的行布局；修正长值重叠、说明换行、toast 和视觉代理选择框裁切。
- AI 补齐文本渲染 full/basic/plain、扫描快速发送、语音输入，以及模型 Responses API、思考强度、厂商联网搜索/来源和图片生成配置。模型编辑保存保留已有设置、自定义 endpoint 和温度 0。
- 按压反馈缩放减至 0.99，动画 90ms。键盘取消字体放大，快速点击高亮至少 150ms；文字仍在松开时立即提交，长按/取消/禁用正确清理。
- AI 工具栏按钮 56×44，图标 32px 居中；触摸区避开原厂返回按钮，消息列表留出侧栏空间。
- 文件管理音乐入口刷新播放列表并调用三代原生播放器；使用原生实体构造/setter，成功后记录当前曲目，停止或失败可重试，支持 MP3 配对歌词，并清理音频引用。
- 快速设置绘制电池轮廓、填充、百分比、低电红色和充电绿色闪电。
- 按键小助手取消激活使用普通明文键盘，文案“请输入文本”。只有完全相等的密码才退出；错误输入关闭键盘并打开原厂词典查询，仍保持激活；取消仍保持激活。第四次 Home 松开并关闭原页面后再打开输入框。
- 原生语音完成后向 AI 转发最终文本一次；取消、隐藏、离页、关闭 AI 语音或按键小助手激活时不转发，AI 模式抑制原厂回答、TTS 和动作。

## 源文件与改动边界

- 主仓库：`/home/haiku/build/PenMods`。
- 二代参考 QRC：`/home/haiku/build/qrc/ydp02x`。
- 三代可编辑 QRC：`/home/haiku/build/qrc/ydp03x`。
- 三代暂存副本：`build/ydp03x-regression-qrc`，与外部三代源树一致。
- 本轮 22 个 QML/资源文件已精确写回外部源树；清单及写回前后 SHA256 在 `build/qa/qrc-regression-sync.json`。
- `resource/models/YDP03X/qrc_qml.h` 已从实际外部源树重新生成，全部 700 个资源文件一致，不能手工编辑。
- 本轮继续阶段涉及的 C++ 为 `src/filemanager/player/MusicPlayer.cpp/.h` 和 `src/filemanager/FileManager.cpp/.h`。这些文件及仓库其他文件有任务前的改动，不可整体回滚或当作全部属于本轮。
- Chatbot 后端原先已支持新增模型字段，本轮主要补齐 QML 配置入口和保存行为，未修改 `src/chatbot/Backend.cpp/.h`。
- 先前交接快照已保留在 `build/qa/worked-before-regression-close.md`，其中的“尚未测试/构建/同步”属于历史状态。

主要 QML：

- 解锁：`qml/settingpages/PenModsPageHost.qml`、`PenModsAntiEmbs.qml`、`PenModsAntiEmbsUnlock.qml`。
- 设置：`PenModsSettingRow.qml`、`ChatAssistantSettings.qml`、模型/提示词管理与详情页、`ApiCacheStatsPage.qml`，以及 `qml/components/DescribedClickableTextBox.qml`、`DescribedSwitchItem.qml`。
- 键盘：`qml/input/YQwertyKey.qml`。
- 电池：`qml/components/YPenModsBatteryIndicator.qml`、`qml/YQuickSettingLayer.qml`。
- AI 工具栏：`qml/assistant/components/ChatToolButton.qml`、`qml/ChatAssistant.qml`。
- 语音：`qml/YVoiceAssistant.qml`、`qml/assistant/YSpeechAsrResult.qml`。
- 附件与音乐入口：`qml/assistant/components/AttachmentChipsBar.qml`、`qml/audiopages/FileManagerPageComponent.qml`。

## 已完成验证

- 真实三代基础 QML + 修改页面，替代宿主 C++ 服务的桌面回归：**34 passed，0 failed**。
- 覆盖页面加载、模型配置往返与绑定、渲染切换、键盘高亮/长按/取消、语音最终结果/去重/取消、解锁伪装、工具栏返回触摸区和截图。
- 测试源：`build/qa/native/tst_PageLoading.qml`；结果：`build/qa/native-current-results.txt`。
- 截图：`build/qa/screenshots/` 中 menu、network、power、settings、assistant、model、proxy、keyboard、battery。使用原厂图标与桌面 Noto CJK 字体，设备仍使用 `fontManager` 原厂字体。
- 音乐测试直接编译生产 `MusicPlayer.cpp`，使用原生符号/服务替身，覆盖首次起播、缺失符号、实体生命周期/对齐、歌词、播放/暂停同曲、停止/失败重试、引用清理、非 MP3 别名和删除文件拒绝。
- 音乐测试源：`build/qa/music/test.cpp`；结果：`build/qa/music/results.txt`。
- 生成资源与全部 700 个源文件逐项一致；非生成文件的 `git diff --check` 通过。
- `PenMods` 与 `PenModsResources` 均通过 YDP03X ARM64 release 交叉构建。日志分别为 `build/ydp03x-regression-build.log` 和 `build/ydp03x-regression-resources-build.log`。
- 两份库均为 ARM64 ELF，包含 `PenMods.Platform=YDP03X`。主库最高 GLIBC 需求 2.27，资源库 2.17。
- 仅有已有的 filler/QProcess 弃用警告。本次没有重新构建可选 FFmpeg 播放器插件。

## 构建产物与验机包

产物：

- `build/linux/arm64-v8a/release/libPenMods.so`
- `build/linux/arm64-v8a/release/libPenModsResources.so`
- 配套验机包：`build/PenMods-YDP03X-2.4.0-regression-fix.tar.gz`

最终库 SHA256：

```text
a3c3af26a49ebd482613eb5c942f94c79ada0336e502f9e014396016b856e13d  libPenMods.so
67e8631f53f72ab734bf2e0c568e2b0f0787d5bef9a9c280bbbee0dc82d4be23  libPenModsResources.so
```

包包含两份库、SHA256SUMS、部署说明、同步文档、当前交接记录、回归结果与截图。
完整功能与手工回归说明见 `doc/YDP03X_SYNC.md`。

## 构建和验证命令

当前 xmake 配置为 YDP03X、arm64-v8a、release、dev、zigcc、Qt `/home/haiku/program/qt`、cross `aarch64-linux-gnu.2.27`、qemu=false。
主库包含资源回退，所以资源更新后两份库均已重新构建。

```sh
bash scripts/gen_qt_res.sh /home/haiku/build/qrc/ydp03x resource/models/YDP03X
python scripts/unpack_qrc.py resource/models/YDP03X/qrc_qml.h --check -o /home/haiku/build/qrc/ydp03x
xmake build PenMods
xmake build PenModsResources
```

桌面回归：

```sh
xvfb-run -a env QT_QUICK_BACKEND=software \
  LD_PRELOAD=/home/haiku/build/PenMods/build/qa/libQAResources.so \
  PENMODS_QA_RCC=/home/haiku/build/PenMods/build/qa/native.rcc \
  qmltestrunner -input build/qa/native -import build/qa/native-imports
```

必须运行整套 QML 测试：原厂 utils.js 的 Array 扩展需在 fixture 初始化时消除可枚举性，CLI 单测选择早于该初始化。
所有 Zig xmake 与 ADB 命令在主机执行，不运行 `xmake run`。
外部 QRC 源树不在沙箱可写根内，后续写回仍须使用沙箱外执行，并检查目标是否产生新改动。

## 剩余实机验收

本轮 ADB 未连接设备，不能将桌面回归或交叉编译当作实机验收。
本分支三代资源默认加载路径为 `/userdisk/PenMods/libPenModsResources.so`，部署时主库与资源库须配套替换并重启宿主，核对两个 SHA256。

1. 文件管理初次播放、标题/歌词/进度、暂停续播、切歌、停止后重试，以及真实音频锁引用。
2. 连续四次 Home 松开后的输入框时序、明文提示、完全相等密码退出、错词查询与取消仍激活。
3. 原生 ASR 实际事件顺序、最终文本只发送一次、取消/离页不发送、AI 模式无原厂 TTS。
4. 原厂字体下的设置长文本、工具栏返回触摸区、150ms 高亮、电池低电/充电状态。
5. 模型配置重启持久化和联网请求；桌面用例只验证配置与转发逻辑，未调用真实提供商接口。

## 已核实的原生接口依据

宿主 ELF：`/home/haiku/build/pen/output/YoudaoDictPen`。

- `YColumnMediaEntity` 大小 0x68，构造/析构 0x4e0efc / 0x4e0fc8；使用原生 setter 写入字段，不依赖 QMetaType 注册。
- `YMediaManager::playAudio` 0x516c2c，同步复制临时实体；内部已经调用 onClickedPlay，不再重复调用。
- 原生 playState：PLAYING=0、PAUSED=1、STOPPED=2；三代管理器私有指针偏移 0x28，播放序号私有区偏移 0x64。
- 三代打开播放器使用 `YBaseSignalAggregate::showAudioPlayer`。
- ASR `setAsrResult` 0x568f2c、`setRecognizing` 0x568e28、`stopAsrRecord` 0x56a0a8、`startAsrRecord` 0x56a2c8、`onSpeechResult` 0x56b368。
- ASR 反汇编记录在 `build/qa/stopAsrRecord-disasm.txt` 和 `build/qa/onSpeechResult-disasm.txt`，实际事件顺序仍待设备确认。

已使用 `penmods-device-workflow` 技能。没有授权提交 Git 或启动子代理。
