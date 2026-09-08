# HINSCAN 第二项工作：数据集目录

本目录是从 `C:\Users\lcx\Desktop\WHINSCAN\Dataset` 整理得到的**无重复工作副本**。原目录没有被修改或删除。整理日期：2026-09-01。

## 目录约定

```text
data/
├─ raw/
│  ├─ hin_text/          可按 base.txt + edge/ + node/ 读取的 HIN 文本格式
│  ├─ entity_relation/   entity_*.txt + relation_*.txt，需先统一编号并转换
│  └─ custom/            其他格式，目前只有 Amazon 邻接表格式
├─ reference_outputs/    旧程序生成的聚类结果或已物化同构图，不属于原始输入
├─ derived/              后续转换数据、离线索引和临时同构图的存放位置
├─ results/              后续实验结果的存放位置
└─ manifests/            数据集清单、重复关系和逐文件校验信息
```

`raw/` 视为只读。格式转换、索引构建和查询结果不得写回 `raw/`，统一写入 `derived/` 或 `results/`。

注意：`dblp_apv_full/base.txt` 在合法的数值模式后附带了 `=== schema ===` 说明段。读取器应先读取类型数及对应类型行，再读取关系类型数及对应关系行，随后停止解析，不能假设整个文件都只包含数字。

## 现有数据集

| 数据集目录 | 格式 | 规模/特点 | 当前状态 | 建议用途 |
|---|---|---|---|---|
| `hin_text/dblp_small` | HIN 文本 | A/P/V，约 3.2 万顶点、5.3 万关系 | 可直接读取 | 第一套正确性测试；A-P-A、A-P-V-P-A |
| `hin_text/movies` | HIN 文本 | A/M/D/W，约 2.4 万顶点、5.6 万关系 | 可直接读取 | 第一套正确性测试；A-M-A、A-M-D-M-A |
| `hin_text/aminer_dblp_apv_small` | HIN 文本 | A/P/V，约 9.1 万顶点、8.8 万关系 | 可直接读取 | 小规模性能与映射验证 |
| `hin_text/imdb_legacy` | HIN 文本 | A/M/D，约 27.5 万顶点、56.6 万关系 | 可直接读取 | 中等规模基线 |
| `hin_text/yelp` | HIN 文本 | U/R/B，约 147 万顶点、187 万关系 | 可直接读取 | 中大规模基线；U-R-B-R-U |
| `hin_text/dblp_apv_full` | HIN 文本 | A/P/V，约 1254 万顶点、3707 万关系 | 可读取；解析器须按声明行数停止 | 大规模主实验 |
| `hin_text/dblp_v18` | HIN 文本 | A/P/V/T，约 2038 万顶点、6356 万关系 | 可直接读取 | 超大规模扩展实验 |
| `entity_relation/dblp` | 实体—关系 | author/paper/conf/focus | 需转换 | 与 HINSCAN 论文型数据对照 |
| `entity_relation/foursquare` | 实体—关系 | user/venue/date/city/category | 需转换 | 高扇出枢轴与免物化收益测试 |
| `entity_relation/imdb_large` | 实体—关系 | movie 加 9 类人员 | 需转换；costume 类型为空 | 多元模式与不同元路径测试 |
| `entity_relation/instacart` | 实体—关系 | user/product/aisle/department | 需转换 | 商品图扩展实验 |
| `custom/amazon` | 自定义邻接表 | Item/User/View/Brand | 需单独转换 | 暂不作为首轮实现目标 |
| `hin_text/kegg1` | 不完整 HIN 文本 | O/G，小图，无 node/ | `base.txt` 含 `93s` 拼写错误 | 仅作人工检查，不进自动实验 |
| `hin_text/kegg2` | 不完整 HIN 文本 | O/G，小图，无 node/ | 缺 node/ | 仅作人工检查，不进自动实验 |

更精确的目录、体量、状态和元路径建议见 `manifests/datasets.csv`。

## 首轮实验选择

建议按以下顺序接入，而不是一次处理所有格式：

1. `dblp_small`：验证 HIN 读取、元路径投影、NIHINScan 与 pSCAN 聚类结果是否一致。
2. `movies`：验证另一种模式及长度为 2、4 的对称元路径。
3. `aminer_dblp_apv_small`：验证较大图、映射文件和计时框架。
4. `imdb_legacy` 或 `yelp`：开始比较物化与免物化运行时间、峰值内存。
5. `dblp_apv_full`、`dblp_v18`：算法正确稳定后再做大规模实验。
6. `foursquare`：完成实体—关系格式转换器后，用于检验高扇出枢轴场景。

## 已分离的旧输出

- `reference_outputs/imdb_legacy/clustering_result.txt`
- `reference_outputs/movies/clustering_result.txt`
- `reference_outputs/movies/homograph_AMDMA.txt`
- `reference_outputs/movies/homograph_weighted_AMDMA.txt`

这些文件可用于交叉检查，但不能当作原始输入，也不能计入新算法的索引构建时间。

## 校验与追溯

- `manifests/source_inventory.csv`：原目录全部 168 个文件的大小、修改时间与 SHA-256。
- `manifests/file_manifest.csv`：整理后 112 个文件与原文件的一一映射；当前 `verified` 全为 `True`。
- `manifests/duplicates.csv`：完全重复目录、重复文件和被排除副本。
- `manifests/datasets.csv`：数据集级别的格式、规模、状态和实验优先级。
