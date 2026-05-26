# CacheSystem 测试与 Benchmark

## 测试入口

- `cache_system_unit_tests`: 运行确定性 unit correctness tests，覆盖 API contract、边界条件和异常路径。
- `cache_system_stress_tests`: 运行 deterministic stress tests，覆盖 sustained cache mutation 和并发 sharded cache 路径。
- `cache_system_randomized_tests`: 运行固定 seed 的 randomized model tests，对照独立 LRU model 验证随机 put/get/erase 序列。
- `cache_system_correctness_tests`: 运行全量 correctness tests，可由 CTest 调用，也作为 benchmark 前置 gate。
- `cache_system_benchmark`: Release-only benchmark，运行前会先执行同一组 correctness tests。
- `cache_system_package_validation`: 安装当前 build，然后用独立 CMake validation project 执行 `find_package(CacheSystem CONFIG REQUIRED)`。

## 推荐命令

```powershell
cmake --preset release
cmake --build cmake-build-release
ctest --test-dir cmake-build-release --output-on-failure
.\cmake-build-release\cache_system_benchmark.exe
```

Debug library-only：

```powershell
cmake --preset debug-library
cmake --build cmake-build-debug
ctest --test-dir cmake-build-debug --output-on-failure
```

Static analysis：

```powershell
cmake --preset analysis
cmake --build cmake-build-analysis
```

`analysis` preset 会启用 `CACHESYSTEM_ENABLE_GCC_ANALYZER=ON`，通过 GCC `-fanalyzer` 执行本地 static analysis。`CACHESYSTEM_ENABLE_CLANG_TIDY=ON` 保留为手动可选项，用于能够稳定解析当前工具链 headers 的环境。

## 性能基线

Benchmark 报告写入 `results/test-YYYY-MM-DD-HH-MM-SS.md`。

`性能测试结果` 会按 `组别` 展示用例，记录 `测试参数`、`操作次数/样本`、`样本数`、`median ms`、`min ns/op`、`median ns/op`、`p95 ns/op`、`stddev ns/op`、`ops/s`、`命中率` 和 `相对组内 best`。`相对组内 best` 只在同一 `组别` 内比较最低 `median ns/op`，报告会单独列出每组 best 用例，避免跨场景比较造成误读。`命中率` 是 CacheSystem 的核心上下文指标：相同 `ns/op` 在高命中和低命中场景下代表的缓存质量完全不同，因此报告必须保留。

首次 benchmark 会生成 `results/performance-baseline.tsv`。后续报告会把当前 `median ns/op` 与 baseline 对比：

- `ok`: 当前结果未慢于 baseline，或变化未超过阈值。
- `watch`: 当前结果超过 baseline 15%，且 `Δ ns/op` 超过 1ns，需要结合机器状态和代码改动判断是否是真退化。
- `new`: baseline 中没有该用例。

`性能基线对比` 会记录 `Δ ns/op`、`Δ %`、`状态` 和 `备注`。`备注` 用于解释 `ok/watch/new` 的判定原因。Baseline 文件不会自动升级旧数据格式；旧的单次 `ns/op` baseline 仍可作为历史参考，但重新设定 baseline 时建议基于多样本 median 报告。

Baseline 不会自动覆盖，避免一次异常机器状态污染长期对比。需要重新设定 baseline 时，手动删除或替换 `results/performance-baseline.tsv`。

## Warning Policy

库目标、correctness targets 和 benchmark target 都启用 `-Wall -Wextra -Wpedantic`。可用 `-DCACHESYSTEM_WARNINGS_AS_ERRORS=ON` 把 warning 提升为 error。
