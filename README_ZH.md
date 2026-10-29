<img align="center" src="macast_slogan.png" alt="slogan" height="auto"/>

# Macast

[![visitor](https://visitor-badge.glitch.me/badge?page_id=xfangfang.Macast)](https://github.com/xfangfang/Macast/releases/latest)
[![stars](https://img.shields.io/badge/dynamic/json?label=github%20stars&query=stargazers_count&url=https%3A%2F%2Fapi.github.com%2Frepos%2Fxfangfang%2FMacast)](https://github.com/xfangfang/Macast)
[![plugins](https://shields-staging.herokuapp.com/github/directory-file-count/xfangfang/Macast-plugins?type=dir&label=plugins)](https://github.com/xfangfang/Macast-plugins)
[![build](https://img.shields.io/github/workflow/status/xfangfang/Macast/Build%20Macast)](https://github.com/xfangfang/Macast/actions/workflows/build-macast.yaml)
[![mac](https://img.shields.io/badge/MacOS-10.14%20and%20higher-lightgrey?logo=Apple)](https://github.com/xfangfang/Macast/releases/latest)
[![windows](https://img.shields.io/badge/Windows-10-lightgrey?logo=Windows)](https://github.com/xfangfang/Macast/releases/latest)
[![linux](https://img.shields.io/badge/Linux-Xorg-lightgrey?logo=Linux)](https://github.com/xfangfang/Macast/releases/latest)

[README_EN](README.md)

Macast是一个跨平台的 **菜单栏\状态栏** 应用，用户可以使用电脑接收发送自手机的视频、图片和音乐，支持主流视频音乐软件和其他任何符合DLNA协议的投屏软件。

当前分支额外支持 **Windows AirPlay 屏幕镜像与音频**，同时保留 Macast
原有的菜单栏、托盘和设置页面。


😂 **请尽量使用英语在Github交流，如果喜欢的话可以点个star关注后续更多协议支持的更新**



## 安装

进入页面选择对应的操作系统下载即可，应用使用方法及截图见下方。

- ### MacOS || Windows || Debian

  下载地址1:  [Macast 最新正式版 github下载](https://github.com/xfangfang/Macast/releases/latest)

  下载地址2:  [Macast 最新正式版 gitee下载（上面访问无效可使用此备用链接）](https://gitee.com/xfangfang/Macast/releases/)

- ### 包管理
  你也可以使用包管理器安装macast  
  ```shell
  # 需要 python>=3.6
  pip install macast
  ```

  请查看我们的wiki页面获取更多的包管理相关信息（如：aur）: [Macast/wiki/Installation#package-manager](https://github.com/xfangfang/Macast/wiki/Installation#package-manager)  
  Linux用户使用包管理器安装时运行可能会有问题，建议替换如下两个库为我修改过的库（分别负责`菜单显示`与`文本复制`）：

  ```shell
  pip install git+https://github.com/xfangfang/pystray.git
  pip install git+https://github.com/xfangfang/pyperclip.git
  ```

  **Linux用户如果安装或运行有问题，可以查看 [这里](https://github.com/xfangfang/Macast/wiki/Installation#linux)**

- ### 从源码构建或运行

  构建请参阅: [Macast Development](docs/Development.md) 和 [build-macast.yaml](https://github.com/xfangfang/Macast/blob/main/.github/workflows/build-macast.yaml)
  
  运行只需要clone仓库，根据不同的操作系统于requirements文件中安装相关的包，并在项目根目录运行 `Macast.py` 即可。



## 使用方法

- **普通用户**  
  1. 打开应用后，**菜单栏 \ 状态栏 \ 任务栏** 会出现一个图标，这时你的设备就可以接收来自同一局域网的DLNA投放了。

- **进阶用户**  
  1. 通过手动加载 [Macast插件](https://gitee.com/xfangfang/Macast-plugins), Macast可以支持调用其他播放器，如：IINA、PotPlayer等等，或适配国内各家私有的DLNA协议. 
  2. 在应用内点击高级设置，可以直接在内置的插件商店中快速下载插件（使用github仓库地址，如果网络条件不好刷新不出来，那么还是通过手动加载的方式下载吧）
  3. 支持修改默认播放器的快捷键或其他参数，见：[#how-to-set-personal-configurations-to-mpv](https://github.com/xfangfang/Macast/wiki/FAQ#how-to-set-personal-configurations-to-mpv)
  4. Windows 下可以在同一份 Macast 中同时使用 DLNA 和 AirPlay 屏幕镜像。

- **程序员**  
  1. 可以依照教程完成自己的脚本，快速地适配到你喜欢的播放器，或者增加一些新的功能插件，比如：边下边看，自动复制视频链接等等。教程和一些示例代码在：[Macast/wiki/Custom-Renderer](https://github.com/xfangfang/Macast/wiki/Custom-Renderer)  
  2. 也可以参考 [nirvana](https://github.com/xfangfang/Macast-plugins/tree/main/nirvana) 快速适配第三方魔改的DLNA协议。

欢迎大家提交代码到[Macast插件](https://github.com/xfangfang/Macast-plugins)。  
**注意：不要轻易加载非官方仓库下载的插件，这里“插件”本身是可以运行在电脑上的任意代码，不建议加载非官方提供的插件。**


## 开发计划

- [x] 完成第一版应用，支持MacOS
- [x] 添加对Linux和Windows的支持
- [x] 完善协议，增强软件适配性
- [x] 统一MacOS与其他平台的UI
- [x] 添加多播放器支持
- [x] 添加多网卡支持
- [x] 添加自定义端口和自定义播放器名称
- [ ] 改进目前的播放器控制页面
- [x] 增加插件商店
- [x] 添加bilibili弹幕投屏
- [x] 管理独立的 AirPlay-Windows 镜像接收进程

AirPlay 屏幕镜像与音频接收进程的部署、配置和验证方式见
[docs/AirPlay.md](docs/AirPlay.md)。AirPlay-Windows 自身的控制界面默认不显示，
仅在检测到镜像会话时显示带标准 Windows 边框的镜像画面。

## Windows AirPlay 屏幕镜像

当前分支基于
[AirPlay-Windows](https://github.com/moieric11/AirPlay-Windows)
增加独立 AirPlay 接收进程管理：

- 支持 iPhone/iPad H.264、HEVC 屏幕镜像和 AirPlay 音频。
- DLNA 与 AirPlay 可以同时运行。
- 控制界面仍然是原版 Macast，不使用 AirPlay-Windows 的首页和导航。
- AirPlay-Windows 在后台隐藏启动，不再弹出自己的管理窗口。
- Macast 启动后会等待上一实例退出并确认 `_airplay`、`_raop` 两项 mDNS
  服务注册成功，才开始显示 `Running`；启动阶段异常退出或未能发布服务时会
  自动重试，避免每次重启 Macast 都要手动点“重启 AirPlay”。
- AirPlay 后端会优先选择真实 Wi-Fi/以太网接口，并把对应 IPv4 和 MAC 写入
  mDNS 记录；FlClash、ZeroTier、VPN 等虚拟网卡不会抢占 AirPlay 广播。
  DACP 反向控制也使用同一个物理 LAN 接口；链路本地 IPv6 地址会保留
  `%接口序号` scope。
- 每份安装使用物理网卡的真实 MAC 作为 AirPlay `deviceid`，并用持久化
  Ed25519 身份与 MAC 共同派生 `pi`。因此同一局域网内多台名称相同的 Macast
  会被识别为不同接收端，即使误复制了同一个 `identity.key` 也不会重复。
  mDNS SRV 主机名仍使用真实 Windows 主机名，确保解析到物理 LAN 地址。
- AirPlay 后端每秒检查一次实际 LAN 接口和 IPv4。切换 Wi-Fi、手机热点或
  网线后会自动撤销旧 mDNS 记录并重新发布，不再需要手动重启 AirPlay。
- PyInstaller 临时目录每次启动都会变化，因此 Macast 会把 AirPlay 后端复制到
  固定的用户配置目录，并按该固定路径维护 Windows 防火墙规则。首次运行固定
  路径后端时会请求一次管理员权限，以取消旧临时路径留下的 Block 规则并添加
  LocalSubnet 入站 Allow 规则。
- 开始镜像时显示标准 Windows 视频窗口，带原生最小化、最大化和关闭按钮。
- 镜像窗口始终匹配当前视频比例；拖动上下边和左右边时会按实际拖动方向缩放。
- 关闭镜像窗口只会断开当前发送端并隐藏窗口，AirPlay 接收服务、RTSP
  服务和 mDNS 广播会继续运行，可直接再次投屏。
- AirPlay 设备名称以 Macast 原版的 friendly name 为基础，并在发现阶段附加
  6 位稳定身份后缀，用于区分同一局域网内名称相同的多台接收端。
- Macast 原生 AirPlay 子菜单会显示歌曲名、歌手和播放时间，并提供上一首、
  播放、暂停、播放/暂停切换、下一首、音量加、音量减和静音切换。
- Windows 系统媒体卡片会显示 `Macast AirPlay`、歌曲名、歌手，以及可用的
  上一首、暂停/播放和下一首按钮；不会再显示
  `ms-resource:AppName` 的通用媒体播放器卡片。
- Windows 通过 DLNA 播放媒体时，会显示当前媒体名称和可用的播放/暂停按钮；
  DLNA 空闲后媒体卡片自动消失。
- 多设备、多协议采用“最后投放者优先”：新 AirPlay 设备会断开上一台
  AirPlay，并停止正在播放的 DLNA；新的 DLNA 投放会暂停或断开 AirPlay，
  并替换当前媒体。不会拒绝最新设备，也不会把多路声音混在一起播放。
- 如果被抢占的是可控制的 AirPlay 音乐，Macast 会通过 DACP 暂停而不是
  粗暴断开连接；安卓 DLNA 停止后，iPhone 音乐会恢复播放。屏幕镜像或
  无法控制的发送端仍使用强制抢占。
- AirPlay 音频会记录实际使用的 SDL/WASAPI 输出设备。若 Windows 默认输出
  是 `ToDesk Virtual Audio`、`RemoteControl Virtual Audio` 或网易虚拟声卡，
  接收器会优先切换到可用的本地扬声器；也可通过环境变量
  `MACAST_AIRPLAY_AUDIO_DEVICE` 指定设备名称。
- 元数据与反向控制使用发送端的 DMAP/DACP 服务；歌词、完整播放列表和下一首
  预览通常无法获取。
- Macast 退出时自动停止 AirPlay 接收进程，异常退出时由 Job Object 清理。

首次使用时，可能需要允许 `airplay-windows-macast.exe` 通过 Windows
防火墙访问本地网络。详细设置和已知限制见 [docs/AirPlay.md](docs/AirPlay.md)。

### 构建 Windows AirPlay 后端

```powershell
powershell -ExecutionPolicy Bypass -File tools/prepare_airplay_windows.ps1
powershell -ExecutionPolicy Bypass -File tools/build_airplay_windows_macast.ps1
```

首次构建会通过 vcpkg 安装 FFmpeg、OpenSSL 和 SDL2，可能需要较长时间。
构建结果是一个自包含的
`bin\AirPlay-Windows\airplay-windows-macast.exe`。

已知限制：同一时间仅支持一个 AirPlay 发送端；不支持 DRM 视频；AirPlay
后端当前仅支持 Windows 10/11 x64。

## 出现问题的可能原因及解决办法（更详细内容见项目的wiki）

0. 应用闪退  
    大概率是由windows的hyper-v占用端口号导致的，建议修改hyper-v占用的端口号范围或修改本应用的启动端口号（[Macast配置文件位置](https://github.com/xfangfang/Macast/wiki/FAQ#where-is-the-configuration-file-located)）
2. 无法搜索到Macast——被电脑防火墙拦截  
    手机尝试访问 http://电脑ip:1068，如:192.168.1.123:1068 如果出现helloworld 等字样排除问题。  
    *具体端口号见应用菜单设置的第一项，如果没有则为默认的1068*
2. 无法搜索到Macast——路由器问题  
    路由器需要开启UPnP，关闭ap隔离，确认固件正常（部分openwrt有可能有问题）
4. 无法搜索到Macast——手机软件有问题  
    可以重启软件或更换软件尝试，或向其他投屏接收端电视测试
    尝试在搜索页面等待久一点（最多1分钟如果搜不到那应该就是别的问题了）
    如操作系统为IOS，注意要开启软件的**本地网络发现**权限
5. 无法搜索到Macast——网络问题  
    请确定手机和电脑处在同一网段下，比如说：电脑连接光猫的网线，手机连接路由器wifi，这种情况大概率是不在同一网段的，可以查看手机和电脑的ip前缀是否相同。
6. 无法搜索到Macast——其他未知问题  
    尝试在同一局域网手机投电视，如果可以正常投说明问题还是出在电脑端，继续检查电脑问题或查看如何报告bug

## 对于反馈问题的说明

  1. 先确保自己有认真读过使用说明
  2. 在提issue时，请及时地回复作者的消息，太多人提完问题或者反馈就消失，提之前先看别人问过没有，提之后积极参与讨论。如果您做不到回复issue，请不要随便提issue浪费开发者的时间。
  3. 遇到问题不要只说现象，请附带所有你认为能帮助开发者解决问题的信息，这会让开发者认为你很聪明，且极大的帮助加快解决你的问题与节省开发者的时间。
  4.  如果你遇到了某个问题，请优先考虑是自己没有看使用说明，比如我遇到过很多很多遭遇了投屏搜索不到的用户，直接评论说，“这个软件用不了”。用不了那是我编出来逗你玩的吗？检查一下自己的防火墙OK？
  5. 如果你不能自己去写，请不要提出那种很难实现的需求，开发者愿意解决的是：“我有个需求，讨论一下要怎么实现” 而不是 “可以帮我给这个软件加上***功能吗？”

## 如何报告bug
  准备以下信息，推荐到Github报告问题，点击 **[new issue](https://github.com/xfangfang/Macast/issues/new/choose)** 去反馈问题：
  1. 你的电脑系统类型和版本：如Win10 20h2
  2. 你使用的手机系统和软件：如 安卓 bilibili
  3. bug复现：如何复现bug与bug是否可以稳定复现
  4. 程序运行的log（复现问题时候的log）：  
    - windows下载debug版应用, 拖入cmd执行，复现问题后，关闭应用，ctrl-a全选复制：[download debug](https://github.com/xfangfang/Macast/releases/latest)  
    - mac 终端输入：`/Applications/Macast.app/Contents/MacOS/Macast` 回车运行，复现问题后，关闭应用，复制log  
    - linux 安装deb后，命令行运行 `macast` \\ 或直接从源码运行 \\ 或包管理安装后命令行运行 `macast-cli`，复现问题后，关闭应用，复制log  

## 用户反馈

点击链接加入群聊【小方的软件工地】：[983730955](https://jq.qq.com/?_wv=1027&k=4ioK8gQs)

当然也可以考虑捐赠 ~~获得贵宾售后服务（开玩笑）~~ 支持Macast和他的开发者们为了这个软件熬过的日日夜夜

<img align="center" width="400" src="sponsorships.png" alt="sponsorships" height="auto"/>

<img align="center" width="400" src="https://service-65diwogz-1252652631.bj.apigw.tencentcs.com/release/sponsor.svg" alt="sponsors" height="auto"/>

## 使用截图
*如果系统设置为中文，Macast会自动切换中文界面*  

在投放视频或其他媒体文件后，可以点击应用图标复制媒体下载链接  
<img align="center" width="400" src="https://gitee.com/xfangfang/xfangfang/raw/master/assets/img/macast/copy_uri.png" alt="copy_uri" height="auto"/>

支持选择第三方播放器  
<img align="center" width="400" src="https://gitee.com/xfangfang/xfangfang/raw/master/assets/img/macast/select_renderer.png" alt="select_renderer" height="auto"/>


## 相关链接

[UPnP™ Device Architecture 1.1](http://upnp.org/specs/arch/UPnP-arch-DeviceArchitecture-v1.1.pdf)

[UPnP™ Resources](http://upnp.org/resources/upnpresources.zip)

[UPnP™ ContentDirectory:1 service](http://upnp.org/specs/av/UPnP-av-ContentDirectory-v1-Service.pdf)

[UPnP™ MediaRenderer:1 device](http://upnp.org/specs/av/UPnP-av-MediaRenderer-v1-Device.pdf)

[UPnP™ AVTransport:1 service](http://upnp.org/specs/av/UPnP-av-AVTransport-v1-Service.pdf)

[UPnP™ RenderingControl:1 service](http://upnp.org/specs/av/UPnP-av-RenderingControl-v1-Service.pdf)

[python-upnp-ssdp-example](https://github.com/ZeWaren/python-upnp-ssdp-example)
