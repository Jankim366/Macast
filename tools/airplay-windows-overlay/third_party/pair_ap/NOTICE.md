# pair_ap 来源说明

本目录中的配对实现取自 Shairport Sync 5.6.2 的 `pair_ap` 组件：

<https://github.com/mikebrady/shairport-sync/tree/5.6.2/pair_ap>

源文件保留各自原有的 MIT License 声明。Windows 构建通过
`sodium_compat.cpp` 使用项目已有的 OpenSSL 实现所需 Curve25519、Ed25519
和随机数操作，没有额外引入 libsodium 运行时。

Macast 仅使用其中的 HomeKit 临时配对、pair-verify TLV 和 AP2
control/event 通道加密逻辑；上层 RTSP、音频、镜像和进程生命周期仍由
Macast 管理。
