# 构建与基线运行

## 环境

- CMake 3.20 或更高版本
- 支持 C++17 的编译器
- 当前机器可使用 MinGW-w64 GCC 15.2

## Windows + MinGW

通常可直接运行：

```powershell
cmake -S . -B build-cmake -G Ninja -DCMAKE_CXX_COMPILER=C:/mingw64/bin/g++.exe -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake --parallel
```

当前机器安装的 CMake 4.2.0-rc2 在中文源码路径下会异常退出。可临时把项目映射到纯英文盘符，构建完成后取消映射：

```powershell
subst R: "C:\Users\lcx\Desktop\第二项工作"
cmake -S R:/ -B R:/build-cmake -G Ninja -DCMAKE_CXX_COMPILER=C:/mingw64/bin/g++.exe -DCMAKE_BUILD_TYPE=Release
cmake --build R:/build-cmake --parallel
subst R: /D
```

生成的程序位于 `build-cmake/bin/`：

- `pscan_baseline.exe`：未修改的上游 pSCAN。
- `hin_materialize.exe`：读取标准 HIN 文本并生成 pSCAN 二进制同构图。
- `fli_build_index.exe`：离线构建半路径因子索引。
- `fli_query_index.exe`：全候选精确扫描验证版。
- `pscan_on_fli.exe`：在 FLI 上按 pSCAN 逻辑执行的正式在线入口。
- `bri_build_index.exe`：仅由图与 schema 建立基础关系索引，不指定元路径。
- `bri_query_index.exe`：V11 无路径预建在线入口，临时构建 FLI，按需精确查询，不读写历史路径缓存。
- `verify_adaptive_fli.exe`：新邻域引擎、度数构建和聚类的独立参考测试。

## pSCAN 输入与运行

pSCAN 读取一个包含以下两个文件的目录：

- `b_degree.bin`
- `b_adj.bin`

运行格式：

```powershell
build/bin/pscan_baseline.exe <graph_directory> <epsilon> <mu> [output]
```

上游仓库固定把结果写入输入目录。为保持 `data/raw` 只读，带 `output` 参数的运行只能指向 `data/derived` 下的工作副本，不能直接指向原始数据目录。

当前 CMake 目标直接编译上游的三个源文件，没有改动 pSCAN 算法实现。上游版本固定在提交 `009f48856d7e37ffca83a74e56170655a6d329b6`。

## 物化 HIN 基线

先把对称元路径对应的同构图写入 `data/derived`：

```powershell
build-cmake/bin/hin_materialize.exe data/raw/hin_text/dblp_small A-P-A data/derived/transformed/dblp_small/APA
```

再调用 pSCAN：

```powershell
build-cmake/bin/pscan_baseline.exe data/derived/transformed/dblp_small/APA 0.5 5 output
```

读取器当前支持：

- `base.txt` 中的顶点类型和关系类型声明；
- `edge/<relation-id>.txt` 中带首行关系头的局部 ID 边；
- 按关系正向或反向遍历；
- 类型名称或数字类型 ID 表示的对称元路径；
- 生成 pSCAN 所需的有序、无自环、双向邻接二进制文件。

它目前是 NIHINScan 的**物化基线工具**，还不是最终的免物化算法。其目的在于先固定正确输出和分阶段计时口径。

## FLI-0 离线索引

构建半路径因子索引：

```powershell
build-cmake/bin/fli_build_index.exe data/raw/hin_text/dblp_small A-P-A data/derived/indexes/dblp_small/APA.fli
```

直接从离线索引执行查询，不再读取 HIN，也不需要物化图：

```powershell
build-cmake/bin/fli_query_index.exe data/derived/indexes/dblp_small/APA.fli 0.5 5 data/results/fli0_offline/dblp_small/APA
```

按 pSCAN 的 `sd/ed` 剪枝、core-first 调度、并查集合并和非核心挂载逻辑执行：

```powershell
build-cmake/bin/pscan_on_fli.exe data/derived/indexes/dblp_small/APA.fli 0.5 5 data/results/pscan_on_fli/dblp_small/APA
```

命令末尾可选第五个参数为查询内邻域缓存上限（MiB），V11 默认 32。0 表示不保留邻域记录，仍会生成单条临时邻域；正数启用有界精确缓存，查询结束释放，不是持久的半离线索引。该额度不含进程其余内存：

```powershell
build-cmake/bin/pscan_on_fli.exe data/derived/indexes/imdb_legacy/AMDMA.fli 0.5 5 data/results/pscan_on_fli/imdb_legacy/AMDMA_cache32 32
```

重跑 12 组参数并与上游 pSCAN 逐字节对比：

```powershell
experiments/validate_pscan_on_fli.ps1
```

IMDB 上的 0/8/32 MiB 缓存扫描：

```powershell
experiments/benchmark_imdb_cache_sweep.ps1
```

对照因子索引与已有物化图的全部邻居和相似边：

```powershell
build-cmake/bin/fli_verify.exe data/raw/hin_text/dblp_small A-P-A data/derived/transformed/dblp_small/APA 0.5
```

`fli_scan.exe` 仍保留为“读取 HIN、现场构建索引、立即查询”的端到端入口；正式重复参数实验应先使用 `fli_build_index.exe` 构建一次索引，再使用 `pscan_on_fli.exe` 查询。

## V11：离线不指定元路径的主线

```powershell
build-cmake/bin/bri_build_index.exe data/raw/hin_text/imdb_legacy experiments/v11/reproduce/base.bri
build-cmake/bin/bri_query_index.exe experiments/v11/reproduce/base.bri A-M-D-M-A 0.5 5 experiments/v11/reproduce/result
```

该入口不读取历史 T-SECI / SCI 等索引，不需要 coverage-floor，也不会将查询的 FLI 持久化。输入仍是原来的图、元路径、epsilon、mu，图在离线阶段转换为 BRI。当前支持对称元路径。测试和完整计时口径见 `docs/UPI_V11_按需精确邻域与首次查询优化.md`。

```powershell
build-cmake/bin/verify_adaptive_fli.exe experiments/v11/fixtures
experiments/benchmark_v11.ps1
experiments/validate_v11.ps1
```

## V12 因子块试验（默认尚未替换 V11）

以下入口仍只读取同一份 BRI，在线生成临时块证书，不写回索引；元路径、epsilon、mu 的含义不变：

```powershell
build-cmake/bin/bri_query_blocks.exe experiments/v11/benchmark/imdb_legacy/base.bri A-M-D-M-A 0.5 5 experiments/v12/demo
```

`bri_query_block_seed.exe` 是仅做块证明、不跳过候选的消融入口，不是不同的聚类定义。普通 `bri_query_index.exe` 仍关闭块模式。

```powershell
experiments/validate_v12.ps1
experiments/benchmark_v12.ps1 -IncludeHinBaseline
```

完整结果和正确性约束见 `docs/V12_因子块证书试验.md`。

## V13 见证交集界试验（默认尚未替换 V11）

最终试验入口自动按缓存压力启用精确见证交集界，并延迟不必要的邻域激活。输入参数仍与 V11/V12 相同，不增加离线索引或覆盖阈值：

```powershell
build-cmake/bin/bri_query_adaptive_bounds.exe experiments/v11/benchmark/yelp/base.bri U-R-B-R-U 0.9 5 experiments/v13/demo
```

`bri_query_witness_bounds` 是无条件启用新界的消融入口，部分数据会退化，不作为默认推荐。实现、边界及结果见 `docs/V13_见证交集界与按需激活试验.md`。

## V14 见证缺失上界试验

该入口在 V13 精确见证计数基础上增加单见证缺失上界，仍不新增用户参数或持久索引：

```powershell
build-cmake/bin/bri_query_witness_exclusion.exe experiments/v11/benchmark/yelp/base.bri U-R-B-R-U 0.9 5 experiments/v14/demo
```

`bri_query_witness_bitmaps` 仅保留位图消融试验，不是最终 V14 主入口。结果与正确性说明见 `docs/V14_见证缺失上界试验.md`；默认 `bri_query_index` 尚未替换。

## V14 性能诊断入口

`bri_profile_v14` 测邻域/见证侧，`bri_profile_control_v14` 测状态表/并查集侧，参数与 V14 相同。两者有诊断开销，不应代替正式查询程序报加速比。普通入口不启用诊断计时。测量口径见 `docs/V14_分阶段性能诊断.md`。

## BRI 半增量 UPI（历史阈值方案，不用于 V11 主表）

最后一个参数是离线最多预构建的半元路径数，默认为64。设置为0可以只构建与元路径组合数无关的 BRI：

```powershell
build-cmake/bin/upi_build_index.exe data/raw/hin_text/movies experiments/upi_hybrid/movies 2 100 "0.3,0.5,0.7,0.9" 0
```

未预构建的合法对称元路径会从 BRI 生成临时 FLI 并精确查询：

```powershell
build-cmake/bin/upi_query_index.exe experiments/upi_hybrid/movies A-M-D-M-A 0.5 5 experiments/upi_hybrid/results/movies
```

可以在明确的自适应缓存预算内将热路径晋升为 FLI+T-SECI。下例使用 16 MiB 总缓存上限：

```powershell
build-cmake/bin/upi_promote_path.exe experiments/upi_hybrid/movies A-M-D-M-A 0.3 16
```

之后使用原 `upi_query_index.exe` 查询即会自动命中缓存 T-SECI。

## 无覆盖阈值研究原型

以下工具不需要 `coverage-floor`，用于复现 V9 第一轮实验。FNI 是安全的邻域哈希上界；BSI 在固定记录预算内保存部分精确公共邻居数。二者目前是实验工具，尚未进入 UPI 默认路由。

```powershell
build-cmake/bin/fni_build_index.exe data/derived/indexes/imdb_legacy/AMDMA.fli data/derived/indexes/imdb_legacy/AMDMA.fni
build-cmake/bin/bsi_build_index.exe data/derived/indexes/imdb_legacy/AMDMA.fli data/derived/indexes/imdb_legacy/AMDMA-ratio-500k.bsi 500000
build-cmake/bin/pscan_on_fli.exe data/derived/indexes/imdb_legacy/AMDMA.fli 0.5 5 tmp/v9 0 data/derived/indexes/imdb_legacy/AMDMA.fni data/derived/indexes/imdb_legacy/AMDMA-ratio-500k.bsi
```

若只测试 BSI，将 FNI 文件位置写为 `none`。

### BEQI 类级预算原型

BEQI 将预算化公共邻居计数提升到闭邻域等价类商图，并对缺失类计数精确回退 FLI：

```powershell
build-cmake/bin/beqi_build_index.exe data/derived/indexes/imdb_legacy/AMDMA.fli data/derived/indexes/imdb_legacy/AMDMA-work-500k.beqi 500000
build-cmake/bin/pscan_on_beqi.exe data/derived/indexes/imdb_legacy/AMDMA-work-500k.beqi data/derived/indexes/imdb_legacy/AMDMA.fli 0.5 5 tmp/beqi-imdb
```

第三个构建参数是离线实验的类计数条数预算，不是在线聚类参数。
