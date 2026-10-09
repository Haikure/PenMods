# YDP03X 内嵌 FFmpeg 播放器

文件管理中的视频默认进入内嵌 FFmpeg 播放页。插件未安装、QML 导入失败、
解码/音频输出失败或启动超过 20 秒时，自动回退 `/userdisk/VideoPlayer`（MPV）。
回退前释放内嵌播放器的音频设备，并把播放位置传给 MPV；一次打开最多启动一次。
退出页面或切换文件后，旧请求不能触发回退。MPV 启动失败会显示错误。

界面参考 BiliPocket：底部渐变控制栏、蓝色进度条、播放/暂停、前后跳转 10 秒、
倍速、字幕开关、画面缩放。横滑预览进度，松手跳转；长按临时 2 倍速。
支持同目录同名 ASS/LRC 字幕，字幕解析失败仅显示提示。视频随主界面左右手模式旋转。
布局兼容 320×170 和三代 800×254。文件管理中的音频仍使用系统音频播放器。

YDP03X 已启用 VPU，接入不改内核、设备树或硬解环境。插件默认尝试 MPP 硬解，
沿用二代插件内置的软解回退。设备库齐全时构建 H.264/HEVC/VP8 的 RKMPP 解码器。

## 构建

主库、资源库和播放器插件是三个独立 xmake 目标。插件源码和依赖头文件在
`external/ffmpeg-player`，默认不加载其依赖图；构建插件时启用 `PENMODS_WITH_PLAYER=1`。

设备链接库及软链接与二代仓库一样保存在
`external/ffmpeg-player/external/dictpen/`，构建直接使用仓库内的相对路径，无需指定库位置。
最终插件使用三代设备已有的系统 ALSA/MPP/DRM 库，部署脚本不会替换系统库。

启用播放器构建目标：

```sh
export PENMODS_WITH_PLAYER=1
```

新构建目录配置示例；已有三代配置可直接执行 `xmake f -y` 加载新增依赖：

```sh
xmake f --qt="$HOME/program/qt" --arch=arm64-v8a \
  --build-platform=YDP03X --target-channel=dev --toolchain=zigcc \
  --cross=aarch64-linux-gnu.2.27 -m release -y
```

OpenSSL 的配方固定使用 **`no-tests`**，仅构建库并执行 `install_sw`，不编译/运行测试套件。
FFmpeg 的缓存版本为 9，避免误用旧 OpenSSL 依赖名称的缓存。

## QML 资源

QML 源文件保存在外部资源仓库 `~/build/qrc/ydp03x`。播放器页面位于其中的
`qml/audiopages/`，包括 `ExternalPlayer.qml`、`ExternalPlayerContent.qml`、
`ExternalPlayerEmbedded.qml`、`PlayerButton.qml`、`PlayerTransportIcon.qml`，
以及文件管理入口 `FileManagerPageComponent.qml` 和兼容入口 `VideoPlayer.qml`。

编辑外部资源仓库的 QML，然后在 PenMods 项目根目录按原来的双参数流程生成资源：

```sh
bash scripts/gen_qt_res.sh "$HOME/build/qrc/ydp03x" resource/models/YDP03X
xmake build ffmpegplayerplugin
xmake build PenMods
xmake build PenModsResources
```

项目内仅保留生成的 `resource/models/YDP03X/qrc_qml.h`，不要手改这个文件。
资源生成会在外部资源目录中写入中间文件；该目录不在沙箱可写范围时，在主机执行。
xmake/Zig 构建及 ADB 操作同样按根目录 AGENTS.md 要求在主机执行。

## 部署

```sh
# 检查所有产物和哈希，打印目标路径，不连接设备
bash scripts/deploy_ffmpeg_player.sh --dry-run

# 首次接入同时部署主库、资源库、插件和 qmldir
bash scripts/deploy_ffmpeg_player.sh --serial <设备序列号>
```

目标路径：

```text
/userdisk/PenMods/libPenMods.so
/userdisk/PenMods/libPenModsResources.so
/userdisk/PenMods/qml/FFmpegPlayer/libffmpegplayerplugin.so
/userdisk/PenMods/qml/FFmpegPlayer/qmldir
```

脚本先上传临时文件并逐个验证 SHA256，再 rename 替换，避免覆盖宿主正在映射的库。
所有产物就位后重启 YoudaoDictPen，复核哈希并检查进程仍在运行。
以后只更新播放器引擎时可用 `--plugin-only`；调试产物可用 `--mode debug`。
