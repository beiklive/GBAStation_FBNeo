# GBAStation_fbneo

GBAStation 生态的街机模拟器核心（Nintendo Switch 移植）。

## 构建

需要 devkitPro（devkitA64）和 `rcheevos` 子模块。

```bash
# 普通完整编译
bash build_local.sh

# 清理后完整编译
bash build_local.sh --clean

# 指定线程数
bash build_local.sh -j 8
```

产物：`GBAStationFBNeoStub.nro`（仓库根目录）。
