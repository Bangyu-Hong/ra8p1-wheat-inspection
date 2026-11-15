# 小麦麦穗项目文档导航

| 内容 | 入口 |
|---|---|
| 项目总览、个人贡献与展示图 | [项目首页](../README.md) |
| 产品源码与模块导航 | [RA8P1 说明](../src/ra8p1/README.md) |
| 工程版本与完成状态 | [VERSION_STATUS.md](./VERSION_STATUS.md) |
| 开发环境与工程使用 | [SETUP.md](./SETUP.md) |
| 原始交付记录 | [交付总览](./ra8p1/handover/00_先看_交付总览.md) |
| 历史验收状态 | [当前进度与验收状态](./ra8p1/handover/02_当前进度与验收状态.md) |
| 串口屏最终协议 | [2026-08-19 协议](./ra8p1/hmi/WHEAT_串口屏最终通信协议_20260819.md) |
| 展示图片与PPT来源页码 | [IMAGE_SOURCES.md](./IMAGE_SOURCES.md) |
| 整理前后路径对应 | [CSV 映射](./文件整理映射.csv) / [JSON 映射](./file_mapping.json) |
| 当前材料所依据的原件 | [original_files.json](./original_files.json) |
| 整理统计与检查结果 | [统计](./organization_result.json) / [检查结果](./verification_result.json) |

`handover`、`hmi` 中为原始文档副本，正文中的旧绝对路径与历史描述按原样保留。当前工程入口以项目首页和整理映射为准。

原始文件完整保留在 `archive/originals`。此前整理涉及的其他项目副本与上一版说明已移入 `archive/previous_organization`，不再作为当前项目入口。归档默认不纳入 Git，完整原始目录的校验清单也保留在该历史归档中。

执行 `python tools/verify_materials.py` 可检查整理后的文件与文档链接。本地存在归档时一并核对原件；从 GitHub 下载的版本只检查已发布文件，清单中标记 `local_only` 的历史固件与本机调试配置允许缺省。
