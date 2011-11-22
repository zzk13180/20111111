# 时间胶囊（自用）

极简的长期数据存储工具，使用 C89 标准，专注于数据的长期可读性。

## 核心特性

- **C89/C90**：最大兼容性，无外部依赖
- **TLV 格式**：Type-Length-Value 结构，自描述、可扩展
- **文本优先**：UTF-8 编码，最高可读性
- **追加模式**：支持持续添加数据
- **4GB 限制**：使用 32 位整数，保持简单
- **大端序**：消除硬件架构差异

## 文件结构

```
[文件头: Magic(4B) + Size(4B) + Reserved(4B)]
[数据块 1: Type(4B) + Length(4B) + Value]
[数据块 2: ...]
```

**数据类型**：
- `0x00000001` - UTF-8 文本（信息、JSON、元数据）
- `0x00000002` - 二进制文件（图片、音频等，需配合元数据）
- `0xFFFFFFFF` - 填充/对齐

## 编译与使用

```bash
# 编译
gcc -std=c89 -Wall -o zzk1 zzk1.c

# 创建归档
./zzk1 create data.zzk1 "字符串"

# 追加文本
./zzk1 append data.zzk1 "另一个字符串"

# 追加文件
./zzk1 append-file data.zzk1 jpg.jpg "图片描述"

# 列出内容
./zzk1 list data.zzk1

# 提取数据（注意索引，图片内容在图片元数据块之后）
./zzk1 extract data.zzk1 4 output.jpg
```
