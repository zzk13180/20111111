/*
 * 极简的长期数据存储工具。C89 标准，无外部依赖，专注于数据的长期可读性。
 *
 * 设计哲学:
 *   用最少的代码、最简单的格式、最大的兼容性来保存数据。
 *   牺牲压缩、加密、索引等现代功能，换取"任何时代任何平台都能解读"的能力。
 *
 * 核心特性:
 *   - C89/C90 标准，无外部依赖，最大兼容性
 *   - TLVC (Type-Length-Value-CRC32) 结构，自描述、可扩展、可验证
 *   - CRC32 校验和，检测位翻转与磁盘损坏
 *   - UTF-8 文本优先，人眼可读
 *   - 追加模式，只增不改，降低数据丢失风险
 *   - 大端序，消除硬件架构差异
 *   - 32 位整数，4GB 上限，保持简单
 *
 * 文件结构:
 *   [文件头: Magic(4B) + TotalSize(4B) + Reserved(4B)]
 *   [数据块: Type(4B) + Length(4B) + Value(Length B) + CRC32(4B)] ...
 *
 *   Magic Number: 0x5A5A4B31 ("ZZK1")
 *
 *   数据类型:
 *     0x00000001 - UTF-8 文本
 *     0x00000002 - 二进制文件（前一个块为其元数据）
 *     0xFFFFFFFF - 填充/对齐
 *
 * 编译与使用:
 *   gcc -std=c89 -Wall -o zzk1 zzk1.c
 *
 *   ./zzk1 create    <archive> <text>                 创建归档
 *   ./zzk1 append    <archive> <text>                 追加文本
 *   ./zzk1 append-file <archive> <file> <description> 追加文件
 *   ./zzk1 list      <archive>                        列出内容
 *   ./zzk1 extract   <archive> <chunk_index> <output>  提取块
 *
 *   append-file 生成两个相邻块：元数据(文本) + 文件内容(二进制)。
 *   提取二进制文件时，使用二进制块的索引（元数据块索引 + 1）。
 *
 * 局限性:
 *   - 无压缩、无加密
 *   - 无随机访问（线性扫描）
 *   - 无删除/修改（追加模式，只能重建整个归档）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC_NUMBER 0x5A5A4B31  /* "ZZK1" */
#define RESERVED     0x00000000
#define HEADER_SIZE  12          /* Magic(4) + TotalSize(4) + Reserved(4) */
#define CHUNK_OVERHEAD 12        /* Type(4) + Length(4) + CRC32(4) */

#define TYPE_TEXT     0x00000001
#define TYPE_BINARY   0x00000002
#define TYPE_PADDING  0xFFFFFFFF

/* unsigned long 在 C89 中保证至少 32 位 */
typedef unsigned long u32;

int write_u32_be(FILE *fp, u32 val);
int read_u32_be(FILE *fp, u32 *val);

/* ========== 工具函数 ========== */

static void die_io(const char *what) {
    perror(what);
    exit(1);
}

static void require_write_u32(FILE *fp, u32 val, const char *what) {
    if (write_u32_be(fp, val) != 0) die_io(what);
}

static void require_fwrite(FILE *fp, const void *buf, size_t len, const char *what) {
    if (len == 0) return;
    if (fwrite(buf, 1, len, fp) != len) die_io(what);
}

/* 安全字符串拼接，防止缓冲区溢出。成功返回 0，截断返回 -1 */
static int append_str(char *dst, size_t dst_cap, size_t *used, const char *src) {
    size_t src_len, avail;

    if (dst_cap == 0 || *used >= dst_cap) {
        if (dst_cap > 0) dst[dst_cap - 1] = '\0';
        return -1;
    }

    src_len = strlen(src);
    avail = (dst_cap - 1) - *used;
    if (src_len > avail) {
        memcpy(dst + *used, src, avail);
        *used += avail;
        dst[*used] = '\0';
        return -1;
    }

    memcpy(dst + *used, src, src_len);
    *used += src_len;
    dst[*used] = '\0';
    return 0;
}

static u32 get_file_size_u32_or_die(FILE *fp, const char *what) {
    long pos;
    if (fseek(fp, 0, SEEK_END) != 0) die_io(what);
    pos = ftell(fp);
    if (pos < 0) die_io(what);
    if ((unsigned long)pos > 0xFFFFFFFFUL) {
        fprintf(stderr, "Error: file too large for U32 size field.\n");
        exit(1);
    }
    return (u32)(unsigned long)pos;
}

/* 分步前进，避免 fseek 的 long 参数在 32 位平台上溢出 */
static void seek_forward(FILE *fp, u32 offset) {
    const long MAX_STEP = 0x70000000;
    while (offset > 0) {
        long step = (offset > (u32)MAX_STEP) ? MAX_STEP : (long)offset;
        if (fseek(fp, step, SEEK_CUR) != 0) die_io("Error seeking file");
        offset -= (u32)step;
    }
}

/* ========== 序列化 ========== */

/* 将 u32 转换为大端字节序 */
static void u32_to_be(u32 val, unsigned char buf[4]) {
    buf[0] = (unsigned char)((val >> 24) & 0xFF);
    buf[1] = (unsigned char)((val >> 16) & 0xFF);
    buf[2] = (unsigned char)((val >> 8)  & 0xFF);
    buf[3] = (unsigned char)(val & 0xFF);
}

/* 以大端序写入 32 位整数 */
int write_u32_be(FILE *fp, u32 val) {
    unsigned char buf[4];
    u32_to_be(val, buf);
    return (fwrite(buf, 1, 4, fp) != 4) ? -1 : 0;
}

/* 以大端序读取 32 位整数 */
int read_u32_be(FILE *fp, u32 *val) {
    unsigned char buf[4];
    if (fread(buf, 1, 4, fp) != 4) return -1;
    *val = ((u32)buf[0] << 24) |
           ((u32)buf[1] << 16) |
           ((u32)buf[2] << 8)  |
           ((u32)buf[3]);
    return 0;
}

/* ========== CRC32 校验 ========== */

static u32 crc32_table[256];
static int crc32_table_ready = 0;

static void crc32_init(void) {
    u32 i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) {
            c = (c & 1) ? ((c >> 1) ^ 0xEDB88320UL) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_ready = 1;
}

/* 增量更新 CRC32。用法: crc=0xFFFFFFFFUL; crc=crc32_update(crc,d,n); crc^=0xFFFFFFFFUL; */
static u32 crc32_update(u32 crc, const unsigned char *buf, size_t len) {
    size_t i;
    if (!crc32_table_ready) crc32_init();
    for (i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

/*
 * 写入完整数据块: Type(4) + Length(4) + Value(Length) + CRC32(4)
 * CRC32 覆盖 Type + Length + Value
 */
static void write_chunk(FILE *fp, u32 type, const void *value, u32 length) {
    unsigned char hdr[8];
    u32 crc = 0xFFFFFFFFUL;

    u32_to_be(type, hdr);
    u32_to_be(length, hdr + 4);
    crc = crc32_update(crc, hdr, 8);
    crc = crc32_update(crc, (const unsigned char *)value, (size_t)length);
    crc ^= 0xFFFFFFFFUL;

    require_write_u32(fp, type, "Error writing chunk type");
    require_write_u32(fp, length, "Error writing chunk length");
    require_fwrite(fp, value, (size_t)length, "Error writing chunk value");
    require_write_u32(fp, crc, "Error writing chunk CRC32");
}

/* ========== 文件头操作 ========== */

/*
 * 读取并验证文件头（Magic + TotalSize + Reserved）。
 * 失败时关闭 fp 并退出程序。
 */
static void read_header_or_die(FILE *fp, u32 *total_size, u32 *reserved) {
    u32 magic;
    if (read_u32_be(fp, &magic) != 0 || magic != MAGIC_NUMBER) {
        fprintf(stderr, "Invalid magic number.\n");
        fclose(fp);
        exit(1);
    }
    if (read_u32_be(fp, total_size) != 0) {
        fprintf(stderr, "Error reading total size (truncated header).\n");
        fclose(fp);
        exit(1);
    }
    if (read_u32_be(fp, reserved) != 0) {
        fprintf(stderr, "Error reading reserved field (truncated header).\n");
        fclose(fp);
        exit(1);
    }
}

/*
 * 打开现有归档用于追加。验证文件头，定位到写入位置。
 * 处理三种情况：正常 / 尾部有垃圾数据 / 文件被截断。
 */
static int validate_and_open(const char *filename, FILE **fp_out, u32 *current_size_out) {
    u32 magic, reserved, header_total_size, actual_size;
    FILE *fp = fopen(filename, "rb+");

    if (!fp) {
        perror("Error opening file");
        exit(1);
    }

    if (read_u32_be(fp, &magic) != 0 || magic != MAGIC_NUMBER) {
        fprintf(stderr, "Invalid magic number.\n");
        fclose(fp);
        exit(1);
    }
    if (read_u32_be(fp, &header_total_size) != 0) {
        fprintf(stderr, "Error reading size.\n");
        fclose(fp);
        exit(1);
    }
    if (read_u32_be(fp, &reserved) != 0) {
        fprintf(stderr, "Error reading reserved.\n");
        fclose(fp);
        exit(1);
    }
    if (reserved != RESERVED) {
        fprintf(stderr, "Warning: reserved field is non-zero (%lu).\n", (unsigned long)reserved);
    }
    if (header_total_size < HEADER_SIZE) {
        fprintf(stderr, "Error: invalid total size in header (%lu < %d).\n",
                (unsigned long)header_total_size, HEADER_SIZE);
        fclose(fp);
        exit(1);
    }

    /* 用实际文件大小做追加基准，避免头部 Total Size 与实际不一致 */
    actual_size = get_file_size_u32_or_die(fp, "Error seeking/ftell file");
    if (header_total_size != actual_size) {
        fprintf(stderr, "Warning: header Total Size (%lu) != actual file size (%lu).\n",
                (unsigned long)header_total_size, (unsigned long)actual_size);

        if (actual_size > header_total_size) {
            /* 尾部有垃圾数据，回退到 header 声明的末尾覆盖写入 */
            fprintf(stderr, "Fixing: overwriting trailing garbage data.\n");
            if (fseek(fp, 0, SEEK_SET) != 0) {
                die_io("Error seeking to start of file");
            }
            seek_forward(fp, header_total_size);
            if (current_size_out) *current_size_out = header_total_size;
            *fp_out = fp;
            return 0;
        } else {
            fprintf(stderr, "Error: file truncated. Corrupted.\n");
            fclose(fp);
            exit(1);
        }
    }

    if (fseek(fp, 0, SEEK_END) != 0) die_io("Error seeking to end");
    *fp_out = fp;
    if (current_size_out) *current_size_out = actual_size;
    return 0;
}

/* 更新文件头中的 Total Size 字段 */
static void update_total_size(FILE *fp, u32 added_size, u32 old_size) {
    u32 new_size;
    if (old_size > 0xFFFFFFFFUL - added_size) {
        fprintf(stderr, "Error: file size overflow (exceeds 4GB limit).\n");
        exit(1);
    }
    new_size = old_size + added_size;
    if (fflush(fp) != 0) die_io("Error flushing data before header update");
    if (fseek(fp, 4, SEEK_SET) != 0) die_io("Error seeking to header size field");
    require_write_u32(fp, new_size, "Error writing updated total size");
    if (fflush(fp) != 0) die_io("Error flushing header update");
    if (fseek(fp, 0, SEEK_END) != 0) die_io("Error seeking to end after updating size");
}

/* ========== 命令实现 ========== */

/* create: 创建归档，写入文件头和初始文本块 */
static void cmd_create(const char *filename, const char *initial_text) {
    FILE *fp;
    u32 text_len, total_size = HEADER_SIZE;

    /* 防止误覆盖已有归档 */
    fp = fopen(filename, "rb");
    if (fp) {
        fclose(fp);
        fprintf(stderr, "Error: file '%s' already exists. Delete it first or use append.\n", filename);
        exit(1);
    }

    text_len = (u32)strlen(initial_text);
    if (text_len > 0xFFFFFFFFUL - HEADER_SIZE - CHUNK_OVERHEAD) {
        fprintf(stderr, "Error: text too large (overflow risk).\n");
        exit(1);
    }
    total_size += CHUNK_OVERHEAD + text_len;

    fp = fopen(filename, "wb");
    if (!fp) {
        perror("Error creating file");
        exit(1);
    }

    require_write_u32(fp, MAGIC_NUMBER, "Error writing magic");
    require_write_u32(fp, total_size, "Error writing total size");
    require_write_u32(fp, RESERVED, "Error writing reserved");

    write_chunk(fp, TYPE_TEXT, initial_text, text_len);

    fclose(fp);
    printf("Archive created: %s\n", filename);
}

/* append: 向归档追加文本块 */
static void cmd_append(const char *filename, const char *text) {
    FILE *fp;
    u32 current_size;
    u32 text_len = (u32)strlen(text);
    u32 chunk_size;

    if (text_len > 0xFFFFFFFFUL - CHUNK_OVERHEAD) {
        fprintf(stderr, "Error: text too large (overflow risk).\n");
        exit(1);
    }
    chunk_size = CHUNK_OVERHEAD + text_len;

    validate_and_open(filename, &fp, &current_size);

    if (current_size > 0xFFFFFFFFUL - chunk_size) {
        fprintf(stderr, "Error: file size overflow (exceeds 4GB limit).\n");
        fclose(fp);
        exit(1);
    }

    write_chunk(fp, TYPE_TEXT, text, text_len);

    update_total_size(fp, chunk_size, current_size);
    fclose(fp);
    printf("Appended text to: %s\n", filename);
}

/* append-file: 向归档追加二进制文件（自动生成 元数据块 + 二进制块） */
static void cmd_append_file(const char *archive_name, const char *target_file, const char *description) {
    FILE *fp_archive, *fp_target;
    u32 current_size, target_size, meta_len;
    char metadata[1024];
    unsigned char buffer[4096];
    size_t bytes_read;
    u32 total_added;

    fp_target = fopen(target_file, "rb");
    if (!fp_target) {
        perror("Error opening target file");
        exit(1);
    }

    target_size = get_file_size_u32_or_die(fp_target, "Error seeking/ftell target file");
    if (fseek(fp_target, 0, SEEK_SET) != 0) die_io("Error seeking target file to start");

    /* 构建元数据文本 */
    {
        size_t used = 0;
        int truncated = 0;
        char size_buf[32];

        metadata[0] = '\0';
        sprintf(size_buf, "%lu", (unsigned long)target_size);

        if (append_str(metadata, sizeof(metadata), &used, "Filename: ") != 0) truncated = 1;
        if (append_str(metadata, sizeof(metadata), &used, target_file) != 0) truncated = 1;
        if (append_str(metadata, sizeof(metadata), &used, "\nDescription: ") != 0) truncated = 1;
        if (append_str(metadata, sizeof(metadata), &used, description) != 0) truncated = 1;
        if (append_str(metadata, sizeof(metadata), &used, "\nSize: ") != 0) truncated = 1;
        if (append_str(metadata, sizeof(metadata), &used, size_buf) != 0) truncated = 1;
        if (append_str(metadata, sizeof(metadata), &used, " bytes") != 0) truncated = 1;

        if (truncated) {
            fprintf(stderr, "Warning: metadata truncated to %lu bytes.\n",
                    (unsigned long)(sizeof(metadata) - 1));
        }
    }
    meta_len = (u32)strlen(metadata);

    /* 溢出检查（写入前执行） */
    if (meta_len > 0xFFFFFFFFUL - CHUNK_OVERHEAD ||
        target_size > 0xFFFFFFFFUL - CHUNK_OVERHEAD ||
        (CHUNK_OVERHEAD + meta_len) > 0xFFFFFFFFUL - (CHUNK_OVERHEAD + target_size)) {
        fprintf(stderr, "Error: file size overflow (exceeds 4GB limit).\n");
        fclose(fp_target);
        exit(1);
    }
    total_added = (CHUNK_OVERHEAD + meta_len) + (CHUNK_OVERHEAD + target_size);

    validate_and_open(archive_name, &fp_archive, &current_size);

    if (current_size > 0xFFFFFFFFUL - total_added) {
        fprintf(stderr, "Error: file size overflow (exceeds 4GB limit).\n");
        fclose(fp_target);
        fclose(fp_archive);
        exit(1);
    }

    /* 元数据块 */
    write_chunk(fp_archive, TYPE_TEXT, metadata, meta_len);

    /* 二进制块（流式写入 + 流式 CRC） */
    {
        unsigned char hdr[8];
        u32 bin_crc = 0xFFFFFFFFUL;

        u32_to_be(TYPE_BINARY, hdr);
        u32_to_be(target_size, hdr + 4);
        bin_crc = crc32_update(bin_crc, hdr, 8);

        require_write_u32(fp_archive, TYPE_BINARY, "Error writing binary chunk type");
        require_write_u32(fp_archive, target_size, "Error writing binary chunk length");
        while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp_target)) > 0) {
            require_fwrite(fp_archive, buffer, bytes_read, "Error writing binary chunk value");
            bin_crc = crc32_update(bin_crc, buffer, bytes_read);
        }
        if (ferror(fp_target)) die_io("Error reading target file");
        bin_crc ^= 0xFFFFFFFFUL;
        require_write_u32(fp_archive, bin_crc, "Error writing binary CRC32");
    }

    update_total_size(fp_archive, total_added, current_size);
    fclose(fp_target);
    fclose(fp_archive);
    printf("Appended file '%s' to: %s\n", target_file, archive_name);
}

/* extract: 提取指定块（1-based 索引）到输出文件 */
static void cmd_extract(const char *archive_name, const char *chunk_index_str, const char *output_file) {
    FILE *fp_in, *fp_out;
    u32 total_size, reserved;
    u32 type, length;
    u32 bytes_consumed = 0;  /* 已读取的数据区字节数 */
    u32 data_region;
    int target_index, current_index = 0;
    unsigned char buffer[4096];
    u32 bytes_remaining;
    size_t to_read;
    char *endptr;
    long parsed_value;

    parsed_value = strtol(chunk_index_str, &endptr, 10);
    if (*endptr != '\0' || endptr == chunk_index_str || parsed_value <= 0 || parsed_value > 2147483647L) {
        fprintf(stderr, "Error: Invalid chunk index '%s'. Must be a positive integer >= 1.\n", chunk_index_str);
        exit(1);
    }
    target_index = (int)parsed_value;

    fp_in = fopen(archive_name, "rb");
    if (!fp_in) {
        perror("Error opening archive");
        exit(1);
    }

    read_header_or_die(fp_in, &total_size, &reserved);

    if (total_size < HEADER_SIZE) {
        fprintf(stderr, "Error: invalid total size in header.\n");
        fclose(fp_in);
        exit(1);
    }
    data_region = total_size - HEADER_SIZE;

    /* 只在 total_size 声明的范围内遍历 */
    while (bytes_consumed + 8 <= data_region) {
        if (read_u32_be(fp_in, &type) != 0) break;
        if (read_u32_be(fp_in, &length) != 0) {
            fprintf(stderr, "Warning: Unexpected EOF reading chunk length.\n");
            break;
        }
        bytes_consumed += 8;

        /* 需要 length + 4(CRC32) 字节 */
        if (data_region - bytes_consumed < 4 || length > data_region - bytes_consumed - 4) {
            fprintf(stderr, "Warning: chunk length exceeds remaining data.\n");
            break;
        }

        current_index++;

        if (current_index == target_index) {
            unsigned char hdr[8];
            u32 crc = 0xFFFFFFFFUL;
            u32 stored_crc;

            printf("Extracting Chunk #%d (Type %lu, %lu bytes) to '%s'...\n",
                   target_index, (unsigned long)type, (unsigned long)length, output_file);

            /* CRC 计算：包含 Type + Length */
            u32_to_be(type, hdr);
            u32_to_be(length, hdr + 4);
            crc = crc32_update(crc, hdr, 8);

            fp_out = fopen(output_file, "wb");
            if (!fp_out) {
                perror("Error opening output file");
                fclose(fp_in);
                exit(1);
            }

            bytes_remaining = length;
            while (bytes_remaining > 0) {
                to_read = (bytes_remaining > sizeof(buffer)) ? sizeof(buffer) : (size_t)bytes_remaining;
                if (fread(buffer, 1, to_read, fp_in) != to_read) {
                    fprintf(stderr, "Error reading chunk data.\n");
                    fclose(fp_out);
                    fclose(fp_in);
                    exit(1);
                }
                if (fwrite(buffer, 1, to_read, fp_out) != to_read) {
                    perror("Error writing to output file");
                    fclose(fp_out);
                    fclose(fp_in);
                    exit(1);
                }
                crc = crc32_update(crc, buffer, to_read);
                bytes_remaining -= (u32)to_read;
            }

            crc ^= 0xFFFFFFFFUL;

            /* 读取并验证 CRC32 */
            if (read_u32_be(fp_in, &stored_crc) != 0) {
                fprintf(stderr, "Warning: could not read CRC32.\n");
            } else if (stored_crc != crc) {
                fprintf(stderr, "WARNING: CRC32 MISMATCH (stored: %08lX, computed: %08lX). Data may be corrupted!\n",
                        (unsigned long)stored_crc, (unsigned long)crc);
                fclose(fp_out);
                fclose(fp_in);
                exit(2);
            } else {
                printf("CRC32 verified OK.\n");
            }

            fclose(fp_out);
            fclose(fp_in);
            printf("Extraction complete.\n");
            return;
        }

        /* 跳过 value + CRC32 */
        seek_forward(fp_in, length);
        if (fseek(fp_in, 4, SEEK_CUR) != 0) die_io("Error skipping CRC32");
        bytes_consumed += length + 4;
    }

    fprintf(stderr, "Error: Chunk #%d not found.\n", target_index);
    fclose(fp_in);
    exit(1);
}

/* list: 列出归档中的所有数据块 */
static void cmd_list(const char *filename) {
    FILE *fp;
    u32 total_size, reserved;
    u32 type, length;
    u32 bytes_consumed = 0;
    u32 data_region;
    int chunk_count = 0;
    unsigned char *buffer;
    u32 stored_crc;
    const char *type_name;

    fp = fopen(filename, "rb");
    if (!fp) {
        perror("Error opening file");
        exit(1);
    }

    read_header_or_die(fp, &total_size, &reserved);

    if (total_size < HEADER_SIZE) {
        fprintf(stderr, "Error: invalid total size in header.\n");
        fclose(fp);
        exit(1);
    }
    data_region = total_size - HEADER_SIZE;

    printf("File: %s (Size: %lu)\n", filename, (unsigned long)total_size);
    printf("----------------------------------------\n");

    /* 只在 total_size 声明的范围内遍历，不读取尾部垃圾 */
    while (bytes_consumed + 8 <= data_region) {
        if (read_u32_be(fp, &type) != 0) break;
        if (read_u32_be(fp, &length) != 0) {
            fprintf(stderr, "Warning: Unexpected EOF reading chunk length.\n");
            break;
        }
        bytes_consumed += 8;

        /* 需要 length + 4(CRC32) 字节 */
        if (data_region - bytes_consumed < 4 || length > data_region - bytes_consumed - 4) {
            fprintf(stderr, "Warning: chunk #%d length (%lu) exceeds remaining data. Stopping.\n",
                    chunk_count + 1, (unsigned long)length);
            break;
        }

        chunk_count++;
        if (type == TYPE_TEXT) type_name = "TEXT";
        else if (type == TYPE_BINARY) type_name = "BINARY";
        else if (type == TYPE_PADDING) type_name = "PADDING";
        else type_name = "UNKNOWN";

        printf("Chunk #%d: Type=%s, Length=%lu bytes\n",
               chunk_count, type_name, (unsigned long)length);

        if (type == TYPE_TEXT) {
            if (length > 0x10000000) {
                fprintf(stderr, "Warning: text chunk too large (%lu). Skipping.\n", (unsigned long)length);
                seek_forward(fp, length);
                if (read_u32_be(fp, &stored_crc) != 0) {
                    fprintf(stderr, "Warning: EOF reading CRC32.\n");
                }
            } else {
                buffer = (unsigned char *)malloc(length + 1);
                if (buffer) {
                    if (fread(buffer, 1, length, fp) == length) {
                        unsigned char hdr[8];
                        u32 computed_crc;

                        buffer[length] = '\0';
                        printf("Content:\n");
                        fwrite(buffer, 1, (size_t)length, stdout);
                        printf("\n");

                        /* 验证 CRC32 */
                        u32_to_be(type, hdr);
                        u32_to_be(length, hdr + 4);
                        computed_crc = 0xFFFFFFFFUL;
                        computed_crc = crc32_update(computed_crc, hdr, 8);
                        computed_crc = crc32_update(computed_crc, buffer, (size_t)length);
                        computed_crc ^= 0xFFFFFFFFUL;

                        if (read_u32_be(fp, &stored_crc) != 0) {
                            fprintf(stderr, "Warning: EOF reading CRC32.\n");
                        } else if (stored_crc == computed_crc) {
                            printf("[CRC32 OK]\n");
                        } else {
                            fprintf(stderr, "WARNING: CRC32 MISMATCH (stored: %08lX, computed: %08lX)\n",
                                    (unsigned long)stored_crc, (unsigned long)computed_crc);
                        }
                    } else {
                        fprintf(stderr, "Warning: Unexpected EOF reading chunk body.\n");
                    }
                    free(buffer);
                } else {
                    fprintf(stderr, "Error: Memory allocation failed.\n");
                    seek_forward(fp, length);
                    if (read_u32_be(fp, &stored_crc) != 0) {
                        fprintf(stderr, "Warning: EOF reading CRC32.\n");
                    }
                }
            }
        } else if (type == TYPE_BINARY) {
            printf("[Binary Data - Skipped]\n");
            seek_forward(fp, length);
            if (read_u32_be(fp, &stored_crc) != 0) {
                fprintf(stderr, "Warning: EOF reading CRC32.\n");
            } else {
                printf("[CRC32: %08lX]\n", (unsigned long)stored_crc);
            }
        } else if (type == TYPE_PADDING) {
            printf("[Padding - Skipped]\n");
            seek_forward(fp, length);
            if (read_u32_be(fp, &stored_crc) != 0) {
                fprintf(stderr, "Warning: EOF reading CRC32.\n");
            }
        } else {
            printf("[Unknown Type - Skipped]\n");
            seek_forward(fp, length);
            if (read_u32_be(fp, &stored_crc) != 0) {
                fprintf(stderr, "Warning: EOF reading CRC32.\n");
            }
        }
        printf("----------------------------------------\n");

        bytes_consumed += length + 4;
    }

    fclose(fp);
}

/* ========== 入口 ========== */

int main(int argc, char *argv[]) {
    const char *command;

    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s create <archive> <text>\n", argv[0]);
        printf("  %s append <archive> <text>\n", argv[0]);
        printf("  %s append-file <archive> <file> <description>\n", argv[0]);
        printf("  %s extract <archive> <chunk_index> <output_file>\n", argv[0]);
        printf("  %s list <archive>\n", argv[0]);
        return 1;
    }

    command = argv[1];

    if (strcmp(command, "create") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: %s create <archive> <text>\n", argv[0]);
            return 1;
        }
        cmd_create(argv[2], argv[3]);
    } else if (strcmp(command, "append") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: %s append <archive> <text>\n", argv[0]);
            return 1;
        }
        cmd_append(argv[2], argv[3]);
    } else if (strcmp(command, "append-file") == 0) {
        if (argc != 5) {
            fprintf(stderr, "Usage: %s append-file <archive> <file> <description>\n", argv[0]);
            return 1;
        }
        cmd_append_file(argv[2], argv[3], argv[4]);
    } else if (strcmp(command, "extract") == 0) {
        if (argc != 5) {
            fprintf(stderr, "Usage: %s extract <archive> <chunk_index> <output_file>\n", argv[0]);
            return 1;
        }
        cmd_extract(argv[2], argv[3], argv[4]);
    } else if (strcmp(command, "list") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s list <archive>\n", argv[0]);
            return 1;
        }
        cmd_list(argv[2]);
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
        return 1;
    }

    return 0;
}
