# 环境来源

`Studio.recipe.json` 保存此模板环境的重建配方。正式资源由同版本的引擎 `EnvironmentCooker` 生成：先 `sample soft` 写 `Sources/Studio.hdr`，再以 `64 16 64 512` 参数 `cook` 至 `Content/Environments/Studio.seenv`。源 HDR、Cooker 的真实 sidecar 与最终环境一同分发；不得用空文件或另一个文件的摘要代替。

HDR 放在 Content 之外，作为来源文件保留，不注册成另一张普通颜色贴图。材质运行时只引用真实 `.seenv` 的固定资产 ID。全局 SH 从该环境的实际卷积辐照数据投影，局部探针保持关闭。
