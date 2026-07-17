# AppOpt

[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-blue.svg)](LICENSE)

一个使用 C 语言编写的 Android 应用线程 CPU 亲和性优化工具，通过 **cpuset** 机制将指定应用的线程绑定到预设的 CPU 核心簇，实现精细化调度控制，兼顾性能与功耗。

---

## 特性

| 特性 | 说明 |
|------|------|
| **自定义规则** | 配置文件定义「包名 → CPU 核心」映射，支持线程级通配符匹配 |
| **热加载** | 基于 inotify 实时监控配置文件变更，无需重启服务 |
| **低开销** | PID 跟踪 + 二分查找 + 增量扫描，最小化 `/proc` 遍历成本 |
| **多架构** | 支持 arm64-v8a / armeabi-v7a / x86_64 |
| **Magisk 模块** | 以 Magisk / KernelSU 模块形式分发，开机自启 |

---

## 编译

### 依赖

- Android NDK r27c 或更高版本

### 本地编译

```bash
# arm64-v8a
aarch64-linux-android21-clang -O2 -s -static \
  -o module/bin/arm64-v8a/AppOpt module/AppOpt.c

# armeabi-v7a
armv7a-linux-androideabi21-clang -O2 -s -static \
  -o module/bin/armeabi-v7a/AppOpt module/AppOpt.c

# x86_64
x86_64-linux-android21-clang -O2 -s -static \
  -o module/bin/x86_64/AppOpt module/AppOpt.c
```

编译完成后，将 `module/` 目录打包为 zip 即可刷入：

```bash
cd module && zip -r ../AppOpt.zip .
```

> CI 构建由 GitHub Actions 自动完成，产物可从 [Releases](https://github.com/AkiHaza/AkiAppOpt/releases) 获取。

---

## 使用

### 命令行

```
AppOpt [选项]

选项:
  -c <路径>    指定配置文件路径（默认: ./applist.conf）
  -s <秒>      设置检查间隔，必须 ≥ 1（默认: 2）
  -v           显示版本信息
  -h           显示帮助

示例:
  AppOpt -c /data/applist.conf -s 3
```

### 配置文件格式 (`applist.conf`)

```
# 以 # 开头的行为注释
# 格式:
#   包名=CPU范围              → 进程级规则
#   包名{线程名}=CPU范围       → 线程级规则

# 示例
com.tencent.mm=4-6
com.tencent.mm{NetworkThread}=7
com.miHoYo.Yuanshen=4-7
com.miHoYo.Yuanshen{RenderThread}=6-7
```

**语法说明：**

| 语法元素 | 说明 |
|----------|------|
| `包名` | 从 `/proc/<pid>/cmdline` 提取的进程名 |
| `{线程名}` | 可选，支持 `fnmatch` 通配符（`*` `?` `[...]`） |
| `CPU范围` | 支持 `0-3`、`4,5,6`、`0-3,6-7` 等写法 |

**匹配优先级：**

- 线程级规则优先于进程级规则
- 多条线程规则命中同一线程时，取最长字面匹配（`*` 和 `?` 不计入字面长度）

---

## 模块结构

刷入后 Magisk 模块目录：

```
/data/adb/modules/AppOpt/
├── bin/
│   ├── arm64-v8a/AppOpt
│   ├── armeabi-v7a/AppOpt
│   └── x86_64/AppOpt
├── service.sh          ← 开机启动脚本
├── module.prop         ← 模块元信息
├── customize.sh        ← 刷入时安装脚本
└── applist.conf        ← 默认配置文件
```

---

## 工作原理

```
┌─────────────┐     ┌──────────────┐     ┌─────────────────────┐
│ applist.conf │ ──▶ │ 规则解析器    │ ──▶ │ /dev/cpuset/AppOpt/ │
└─────────────┘     └──────────────┘     │   ├── 4-6/           │
                                         │   ├── 7/             │
       ┌─────────────────────┐           │   └── 6-7/           │
       │  inotify 热加载      │◀──────────│   (cpuset 分组)      │
       └─────────────────────┘           └─────────┬───────────┘
                                                   │
┌──────────┐     ┌──────────────┐     ┌───────────▼───────────┐
│  /proc   │ ──▶ │ 进程/线程扫描 │ ──▶ │ sched_setaffinity()  │
│ 扫描     │     │ + PID 跟踪    │     │ + tasks 文件写入      │
└──────────┘     └──────────────┘     └───────────────────────┘
```

1. 服务启动时解析 `applist.conf`，在 `/dev/cpuset/AppOpt/` 下创建对应 cpuset 分组
2. 周期性扫描 `/proc`，通过 PID 跟踪与增量扫描机制降低开销
3. 将匹配线程的 PID 写入对应 cpuset 的 `tasks` 文件，同时调用 `sched_setaffinity()` 设置 CPU 亲和性
4. inotify 事件驱动配置文件热加载，自动回退到轮询模式作为兼容方案

---

## 文档

详细使用说明与配置指南：[http://appopt.suto.top](http://appopt.suto.top)

---

## 许可证

[GNU General Public License v3.0](LICENSE)
