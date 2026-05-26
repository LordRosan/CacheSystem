# CacheSystem 测试与 Benchmark

## 测试入口

- `cache_system_correctness_tests`: 只运行 correctness tests，可由 CTest 调用。
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

## 性能基线

Benchmark 报告写入 `results/test-YYYY-MM-DD-HH-MM-SS.md`。

`性能测试结果` 会按 `组别` 展示用例，记录 `测试参数`、`ns/op`、`ops/s`、`命中率` 和 `相对组内 best`。`相对组内 best` 只在同一 `组别` 内比较最低 `ns/op`，报告会单独列出每组 best 用例，避免跨场景比较造成误读。`命中率` 是 CacheSystem 的核心上下文指标：相同 `ns/op` 在高命中和低命中场景下代表的缓存质量完全不同，因此报告必须保留。

首次 benchmark 会生成 `results/performance-baseline.tsv`。后续报告会把当前 `ns/op` 与 baseline 对比：

- `ok`: 当前结果未慢于 baseline，或变化未超过阈值。
- `watch`: 当前结果超过 baseline 15%，且 `Δ ns/op` 超过 1ns，需要结合机器状态和代码改动判断是否是真退化。
- `new`: baseline 中没有该用例。

`性能基线对比` 会记录 `Δ ns/op`、`Δ %`、`状态` 和 `备注`。`备注` 用于解释 `ok/watch/new` 的判定原因。

Baseline 不会自动覆盖，避免一次异常机器状态污染长期对比。需要重新设定 baseline 时，手动删除或替换 `results/performance-baseline.tsv`。

## Warning Policy

库目标、correctness target 和 benchmark target 都启用 `-Wall -Wextra -Wpedantic`。可用 `-DCACHESYSTEM_WARNINGS_AS_ERRORS=ON` 把 warning 提升为 error。
