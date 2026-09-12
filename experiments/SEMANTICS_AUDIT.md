# 小图语义审计与包含森林诊断口径

原始审计：2026-09-11。修复记录更新：2026-09-12。本文原始审计部分记录的是修复前状态；后续修复见下节。没有进行真实数据集性能实验。

## 后续修复：当前工作区状态

BRI 主查询入口已统一为论文闭邻域 μ（至少 2），内部调用 pscan_mu_from_hinscan 得到 μ−1。
底层 run_pscan_on_fli、run_region_completion 仍统计其他相似邻居，不应再次减一；原始 third_party/pscan 未修改。
BRI 的 result/roles 文件名继续使用用户输入的论文 μ，标准输出增加 semantics_version、mu_counts_self 和 pscan_other_mu，便于识别不同版本。

角色计算已修正：所有有簇归属的非核心点均标为 border；只有无簇归属的点再按邻居涉及的簇判断 hub/outlier。
实现按需为半路径倒排表汇总最多两个不同簇 ID（包含已归属簇的非核心邻居），不会展开全部投影边。
该摘要仅存在于一次在线查询内，每张所需倒排表至多扫描一次；两个 ID 是精确 hub 谓词所需的信息，不是可调阈值。
新增的 role_ms 包含在 noncore_ms 和完整在线计算时间里，不能重复相加；工作区是在线内存，不增加离线索引文件。

verify_hinscan_semantics.py 是修复后的通过型回归入口：73 个小图配置 × 5 个查询版本全部匹配独立参考，另有 15 项非法 μ 拒绝检查。
其中 64 个配置穷举四顶点简单图结构，但每图只选一组 ε/μ；没有宣称穷举全部参数。
本次报告：C:/Users/lcx/AppData/Local/Temp/hinscan-correctness-8253pbti/verification/verification.json。
两个 C++ 小图验证器也已结束：verify_anchor_filter 报告 1,680 个聚类检查、verify_adaptive_fli 报告 64,800 个聚类检查，均 all_passed=1；这些计数包括同一合成图上的参数、缓存及执行模式组合，不代表不同真实数据集数。
Python 单元测试现为 14 项，全部通过。C++ 合成夹具目录：C:/Users/lcx/AppData/Local/Temp/hinscan-cpp-correctness-5c6c92c9fcc14264be1493858f7d6d10。
原始 audit_semantics.py 保留为修复前差异复现及独立参考模块；不要把它的历史差异预期当作当前程序必须满足的回归条件。

尚未完成：非对称闭合元路径及多关系类型输入支持、新索引实现、完整基线角色计时的迁移。
旧 UPI/EQI/SCI 等实验入口不在本次主查询接口迁移范围内，不能据此宣称所有历史工具已采用相同论文接口。
新旧双方都须计算完整角色，才能重测加速比。run_server_ablation.py 的共享计时函数遇到新语义标记会明确报错，阻止依赖它的历史脚本继续生成混口径结论；请暂勿运行旧服务器性能脚本。

当前回归命令（本机小图或服务器均可）：

    cmake --build build-ablation-local --parallel 2 --target bri_query_index bri_query_witness_exclusion bri_query_core_connectivity bri_query_core_lean bri_query_regions bri_build_index hin_materialize pscan_baseline
    python -B experiments/verify_hinscan_semantics.py --bin-dir build-ablation-local/bin
    python -B -m unittest discover -s experiments -p test_audit_semantics.py -v

下文“未修改生产算法”“待实施修复”描述的是原始审计时点，不是当前工作区状态。

### 本次改动文件清单

- src/scan/PscanOnFli.cpp：正确角色判定、论文 μ 适配函数、超大 μ 的安全等价处理。
- src/scan/PscanOnFli.h：区分论文/底层参数口径，增加角色耗时及工作量统计。
- src/tools/bri_query_index.cpp：主查询调用参数适配，输出语义版本与纯计算时间。
- src/tools/bri_query_regions.cpp：区域查询采用同一参数适配及版本标记。
- src/scan/RegionCompletion.h：补充底层参数约定注释，未改变区域算法。
- src/tools/verify_anchor_filter.cpp：修正独立参考核心及角色规则。
- src/tools/verify_adaptive_fli.cpp：修正独立参考核心及角色规则。
- experiments/verify_hinscan_semantics.py：新增五版本通过型回归脚本。
- experiments/test_audit_semantics.py：增加新回归输出保护和历史脚本防混用单元测试。
- experiments/run_server_ablation.py：共享计时函数拒绝混用新旧语义。
- experiments/SEMANTICS_AUDIT.md：记录本次修复、验证、范围限制及文件清单。

上一轮新增的 experiments/audit_semantics.py 本轮未修改。未改 third_party、离线构建器、索引序列化格式、数据集或历史结果。

## 本轮实际完成

- 新增独立 Python 集合参考实现：audit_semantics.py。
- 新增 13 项单元测试：test_audit_semantics.py，全部通过。其中穷举了 4 个顶点的 64 种无向简单图，检查投影及 μ 映射。
- 对 8 个合成用例分别运行原始 pSCAN、当前 core_connectivity（模式 9）、core_lean（模式 11），每种 μ 口径各运行一次。
- 目标顶点最多 14 个，另有 2 个不支持输入的显式拒绝检查。
- 报告保存命令、源码/二进制 SHA256、闭邻域、参考簇/角色和逐顶点差异。
- 审计完成不代表生产算法正确：本次报告明确为 audit_completed=true、paper_semantics_verified=false。

本次完整日志：C:/Users/lcx/AppData/Local/Temp/hinscan-semantics-9oqt7nhg/audit/audit.json。
该路径是本机临时产物，可能被系统清理；可用下述命令在指定新目录重现。请勿将本次脚本执行时长解释为算法性能。

## 已确认的差异

核对对象是 [HINSCAN 论文](https://lai.me/files/HINSCAN-final.pdf) 的非独立模型正式定义 3.5–3.11，不是未取得的作者实现。其闭邻域计入自身；未归属任何簇的顶点，才按邻居涉及的簇判断 hub/outlier。

### 1. μ 的计数口径

当前 pSCAN 和索引实现统计“其他相似邻居”，没有计入自身。论文参数与底层参数的对应关系为：

    upstream_mu = paper_mu - 1，paper_mu >= 2

三角形，ε=1，论文 μ=3：三个顶点都是核心。向当前程序直接传入 μ=3，则三个都不是核心；传入 μ=2 后与论文参考一致。

这是接口口径适配，不需要更换 pSCAN 的聚类逻辑；本轮只在审计调用中做映射，未改变任何生产入口。
源码线索：third_party/pscan/Graph.cpp 的 pSCAN、similar_degree、effective_degree 初始化。

### 2. 顶点角色

当前 src/scan/PscanOnFli.cpp 根据非核心顶点自己的簇归属数输出角色：0→outlier，1→border，多个→hub。
这个规则不能替代论文的 hub 判定。

统一 μ 后，两个当前在线实现的结果如下：

| 用例 | 核心及簇归属 | 角色差异 |
| --- | --- | --- |
| triangle_mu_boundary | 与参考一致 | 无 |
| unassigned_hub | 与参考一致 | 顶点 8：应为 hub，实际 outlier |
| overlapping_border | 与参考一致 | 顶点 10：属于两个簇的非核心点，实际被标 hub |
| hub_via_border_neighbors | 与参考一致 | 顶点 12：经两个已归属簇的非核心邻居成为 hub，实际 outlier |
| single_border | 与参考一致 | 无 |
| isolates | 与参考一致 | 无 |
| long_path_dedup | 与参考一致 | 无 |
| mu_above_degree | 与参考一致 | 无 |

所有 8 个用例的 C++ 同构图转换均与独立全路径遍历一致。上述一致仅限这些用例，不能作为任意输入正确性的证明。

原始 pSCAN 只输出核心与非核心簇归属，没有 hub/outlier 文件，因此不能单独充当完整角色校验器。
verify_anchor_filter.cpp、verify_adaptive_fli.cpp 虽使用直接遍历，但采用同样的 μ 和角色规则；它们的通过不足以消除这次发现的语义偏差。

### 3. 输入支持范围

A-B-C-A 非对称闭合元路径以及同一类型对具有多个关系类型的输入，被当前查询/转换程序显式拒绝。
拒绝已复现，但不意味着论文的问题定义允许我们排除这些输入。审计没有给前者擅自定义有向聚类语义。
包含森林的邻域公式目前也仅论证到 P=H·H逆 的非独立模型；不能因此宣称覆盖所有查询元路径。

## 对旧实验的影响与修复顺序

旧运行时间仍是当时实现的真实测量，但准确表述应为“同一旧参数/角色口径下，本地转换+pSCAN 与索引实现的比较”，不能写成“已证明与论文全部输出一致”。

例如旧底层 μ=5 在核心计数上对应论文 μ=6，不应直接改标签为论文 μ=5。
新旧双方都需要使用相同的论文参数适配，并完成角色计算后，才能比较完整在线计算时间。尚未实现的角色工作不能按零成本处理。

后续顺序：

1. 在入口层统一论文 μ 到底层 pSCAN 的映射，保留原始 third_party 源码。
2. 在簇归属确定后修正角色判定，并让主验证器使用独立参考规则。
3. 补齐或明确尚未覆盖的合法输入，不擅自收窄用户的问题定义。
4. 正确性通过后，再评估新索引；保留旧实验记录，不覆盖历史结果。

以上是待实施工作，本轮没有修改生产实现。

## 包含森林：下一阶段诊断口径（尚未实现）

目标是判断一个查询无关、近线性大小的结构，能否真正替代在线的昂贵工作；不是现在承诺十倍加速。

离线仅处理原图每一种有向关系视图 r 的去重邻接集合 A_r(u)：

- 完全相同的非空集合合并为带顶点数权重的类。
- 每类至多选择一个严格包含它的父类，形成森林；包含关系必须精确验证。
- 父类选择可采用最小基数严格超集、相同基数按固定 ID 打破平局；不保存全部包含对。
- 保留原始关系邻接和类映射；空集合单独处理。
- 单父森林可能丢失真实的跨分支包含关系，这是覆盖率损失，不能当作“不包含”的证明。
- 原始邻接、非空关系行及森林元数据可保持 O(n+m+schema) 存储；找父类的时间不因此自动线性，必须单独测量。

在对称查询 P=H·H逆 中，若首关系集合 A_r(u) 包含于 A_r(v)，且 H(u) 非空，则半路径可达集合也包含；由此得到闭邻域包含。令在线算出的精确闭度数为 du、dv，则这对顶点的相似度可由 sqrt(du/dv) 精确判断，不需要交集扫描。

相同首关系集合的活跃顶点可以按类权重计数；半路径可达集合为空时，各顶点仅有自身，不能把整个类当成互相相似。
包含证书给出的相似邻居数只是下界：下界达到 μ 可证明核心，未达到不能证明非核心。
全部核心确定后，最近核心祖先连接可保持森林可比核心对的连通性；跨分支、跨树的连接及非核心归属仍需精确补算。

### 离线统计（不得使用查询）

- 每关系原始去重边数、非空行数、等价类数、类权重分布。
- 父边数、深度分布、类内顶点对数、祖先后代的加权顶点对数；不枚举全部可比对。
- 构建纯计算时间、索引实际文件字节数、原始基础结构字节数、构建峰值内存。
- 图指纹、源码版本；峰值工作内存与落盘索引大小分开。

### 在线诊断（固定原有查询，不增加用户参数）

- 半路径结构构建、精确度数计算、森林查询元数据准备的时间。
- 活跃/不活跃类数，以及由证书直接证明的核心顶点权重。
- 实际需要做的相似度检查中，有多少可由森林替代；不要用理论全部顶点对作分母。
- 将原本已被度数界剪枝或连通性剪枝省掉的工作排除，防止重复记收益。
- 跨分支、跨树剩余昂贵检查数及耗时，证书组件之间仍需处理的连接。
- 非核心簇归属、hub/outlier 判定、完整结果生成的纯计算时间。
- 所有在线准备和精确补算均计入；诊断插桩时间不冒充最终算法时间。

核心/簇的参考答案若用于事后测覆盖率，必须标成事后诊断，不能作为在线算法免费获得的信息。
判定继续投入的依据是“覆盖了多少主导耗时，扣除新增开销后的净收益”，不是森林边多、压缩率高或理论可比对多。
目前没有真实数据的包含覆盖率，因此既不能断言足以十倍加速，也不能断言该方向无效。

### 写新索引前需要补充的小图验证

相等集合、包含链、分叉、菱形偏序的单父信息损失、半路径失活、最近核心祖先跳过非核心、相似度恰好等于 ε、μ 大于类权重，以及本次三个角色反例。

这些是计划中的森林正确性用例，不在已通过的 13 项语义测试内。
真实 Yelp、IMDB large、Foursquare 的结构统计仍只在服务器运行。

## 重现本轮审计

先构建所需目标：

    cmake --build build-ablation-local --parallel 2 --target pscan_baseline hin_materialize bri_build_index bri_query_core_connectivity bri_query_core_lean

运行测试及审计：

    python -B -m unittest discover -s experiments -p test_audit_semantics.py -v
    python -B experiments/audit_semantics.py --bin-dir build-ablation-local/bin --output server-results/semantics-audit-new

输出目录必须不存在，脚本不会覆盖既有结果。--bin-dir、--output 是审计工具路径，不是新增聚类参数。
退出码 0 表示本次已知差异被成功复现，并非算法通过论文正确性验证；未来修复后应另加通过型回归检查，保留该历史审计说明。
