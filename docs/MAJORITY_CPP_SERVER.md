# 多数覆盖组 C++ 接入与服务器实验

日期：2026-09-13。实际项目目录：C:/Users/lcx/Desktop/second-work。

## 已完成内容

缺失的 MajorityIndex.h/.cpp、mgi_build_index.cpp、mgi_query_index.cpp 已写入并编译。它们衔接已有 LayeredCompletion 与 FactorIndex::build_selected。没有修改 PscanOnFli.cpp/.h；新工具的残余聚类调用现有 CoreConnectivity 模式，外部 μ 包含自身，调用底层时减一且仅减一次。

当前交付是可运行的实验版，不是已证明加速十倍的最终算法。没有在本机运行 Yelp、IMDB large 或 Foursquare，也没有服务器新计时结果。本次完成了本地代码核对与独立参考差分，不冒充已经完成额外的独立人工/代理最终审查。

## 算法接入

- 离线输入仅为原始图与 schema。输出目录包含 base.bri（现有 BRI v2 原始关系及两跳度数元数据）和 groups.mgi（原始关系双向多数组 CSR）。不接受查询元路径、ε、μ，不保存元路径投影图或相似度表。
- 多数组构建沿非空二进制区间递归划分已排序的原始邻接行。每行仅向满足严格多数条件的区间输出一次成员记录。每个原始关系的两个方向总成员记录不超过 4m，组头、原始关系和类型数组另计。索引大小不是任意元路径数量的函数。
- groups.mgi 按关系 ID 和方向组织，反向查询不会误用正向组；文件包含原始图指纹、长度和 CRC64。保存及加载按数组流式处理，不另建完整 payload 副本。CRC64 用于意外损坏/配错图检测，不是对恶意伪造文件的密码学保证。
- 在线加载两份索引后，先进行分层可达裁剪、整分量直接完成与相似度证书处理。已完成分量不进入因子构建；剩余目标调用 build_selected，原始中间层不删减，之后运行原 PSCAN 并合并原始 ID 下的完整结果。
- 无法证明的分量整体回退；同类型有向关系等不满足真实逆关系条件的查询禁用证书，保留已有查询路径。新接口不增加用户可调覆盖率、缓存大小或组大小参数。

两跳精确度数沿用现有离线元数据计算，它可能是离线耗时大项；新增组成员的线性界不代表整个离线构建是线性时间。分层判定未命中时仍会额外扫描原始关系；因此真实数据上可能加速，也可能变慢。

## 已运行验证

在全新 build-majority-verified 目录使用 MinGW GCC 15.2、CMake Release 构建，未删除或复用带旧 R: 路径的 build-cmake 缓存。CMake 原有目标声明順序能够正常配置，不需要为此重排文件。

- verify_majority_index：手算组成员、双向成员界、序列化往返、配错图、损坏校验、截断、追加字节、异常偏移和成员检查全部通过。
- verify_selected_factor：严格递增选择、空选择、全选择复用元数据、重复类型半路径经过未选中的中间顶点等检查全部通过。
- audit_majority_cpp.py：127 组真实 C++ 查询通过；完整核心、簇归属和角色对照独立参考与现有查询，簇另对照上游 PSCAN。实际 groups.mgi 的全部组内容与 Python 多数组模型逐组比较，查询前后索引哈希不变，离线阶段时间相加一致。
- 指定提前完成和标量证书案例的 residual_vertices=0、residual_half_expansion_entries=0；混合案例只为 13 个残余目标构建因子，另有 6 个核心目标直接完成。
- 同类型有向关系 A-A-A 的额外微型检查确认 fast_path_supported=0，簇与角色均保持原控制程序行为。这是兼容性检查，不是对该输入独立论文语义的新增证明。
- 既有 verify_adaptive_fli 回归 all_passed=1、cluster_cases=64800。
- Python 新旧模型共 32 项测试通过；服务器脚本 5 项测试通过，包括每组只运行一次、μ 映射、异常计时、角色不一致拒绝加速比、完整成功/失败记录及归档排除索引。

本地未执行 Linux 的 /usr/bin/time 与 timeout 实际封装，也未在 Linux 重新编译；这些在服务器脚本启动时执行。脚本的调度、失败处理和计时口径已通过模拟测试，不能把模拟耗时当成性能结果。

## 服务器执行

同步更新的 src/、experiments/、CMakeLists.txt；保留服务器原始 data/ 和 third_party/pscan/ 源码。不要同步 Windows 的 build-* 目录、可执行文件、write_test.txt 或 123.txt。本文档可一并同步。

在服务器项目根目录，只需要：

```bash
python3 -B experiments/run_server_majority.py
```

需要 Linux、Python 3 标准库、CMake >=3.20、g++（C++17）、GNU time 和 timeout。默认使用两路编译；查询程序本身没有新增多线程代码。默认每条子进程命令限制 7200 秒，超时被记录为失败，不会生成虚假的加速比。已有环境变量 DATA_ROOT、BUILD_DIR、RESULT_ROOT、BUILD_JOBS、LIMIT_SECONDS 仅用于运行环境设置，不是算法查询参数；正常执行无需设置。

默认数据位置：

```text
data/raw/hin_text/yelp/
data/raw/entity_relation/imdb_large/
data/raw/entity_relation/foursquare/
```

沿用已存在的无损格式适配器，不筛选、不采样，不修改原数据。如果 Yelp 声明域需要修正，会在 data-audit.json 记录。生成的规范化副本和基线临时投影仅在该数据集实验期间保留，结束后清理；最终离线索引保留于本次结果目录，历史结果不会覆盖。

每个数据集离线构建一次。每条查询分别执行一次重建的 HINSCAN 基线、现有 mode 9 控制查询、新算法查询，不使用历史耗时，不扫描额外参数：

| 数据集 | 元路径 | ε | μ（含自身） |
| --- | --- | ---: | ---: |
| Yelp | U-R-B-R-U | 0.9 | 5 |
| IMDB large | movie-actor-movie | 0.5 | 5 |
| Foursquare | user-venue-user | 0.5 | 5 |

## 看哪些结果

程序结束会打印 Send this archive:，把对应的 .tar.gz 拉回本机即可。归档只含日志、计时、JSON 和 TSV，不包含数据集、索引、临时投影。即便失败也生成 status.json 和日志归档；all_passed=false 时不要把结果当成功实验。

summary.tsv 的主要列：

| 列 | 含义 |
| --- | --- |
| hinscan_compute_s | 本次重新物化计算 + 上游 PSCAN 的不含 I/O 计算时间 |
| offline_compute_s | 邻接组织 + 两跳元数据 + 多数组构建；不含原始输入读取、索引序列化写出 |
| online_compute_s | 加载索引完成后的查询参数解析、分层证明、残余因子构建、PSCAN、结果合并；不含索引读取与校验、结果写出 |
| speedup_vs_hinscan | hinscan_compute_s / online_compute_s，仅结果一致时报告 |
| index_bytes | base.bri 与 groups.mgi 的实际文件大小之和 |
| residual_vertices | 仍需精确回退的目标点数，越大通常意味着证书覆盖较少 |

offline.tsv 另列出规范化结构文本大小与索引比例；分母不包含实体描述字符串，也不是原始下载压缩包大小。原始输入解析与缓冲属于读取阶段，邻接分配、填充、排序去重计入离线计算；索引序列化 CRC 属于写出阶段。读取阶段的图绑定指纹和 payload 校验计入 index_load_ms，不偷偷算作零工作。

基线是项目依据论文流程重建的版本，不是 HINSCAN 官方源码。上游 PSCAN 本身只输出簇，不计算论文完整角色；新算法在线时间包含角色，因此两者输出工作口径仍有这项不对称，必须在后续论文表格中披露。

## 文件清单

本次恢复后新增：

- src/index/MajorityIndex.h
- src/index/MajorityIndex.cpp
- src/tools/mgi_build_index.cpp
- src/tools/mgi_query_index.cpp
- experiments/run_server_majority.py
- experiments/test_server_majority.py
- docs/MAJORITY_CPP_SERVER.md

本次恢复后修改：experiments/audit_majority_cpp.py（增加实际存储组、索引不变、计时及非法参数检查）；docs/superpowers/plans/2026-09-13-majority-cpp.md（记录验收状态）。

此前已存在并在本次衔接验证、未重新覆盖：src/scan/LayeredCompletion.h/.cpp、src/index/FactorIndex.h/.cpp、src/tools/verify_majority_index.cpp、src/tools/verify_selected_factor.cpp、CMakeLists.txt、.gitignore。同步时需要一起带上它们。现有 PSCAN 文件没有改动；123.txt、write_test.txt 和其它用户文件保留原状。没有自动提交或推送 Git。
