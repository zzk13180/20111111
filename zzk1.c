/* 引入标准输入输出库，用于文件操作和打印信息 */
#include <stdio.h>
/* 引入标准库，用于内存分配和通用工具函数 */
#include <stdlib.h>
/* 引入字符串处理库，用于字符串比较和操作 */
#include <string.h>

/* 定义文件头中的魔数，用于标识文件格式 (ZZK1) */
#define MAGIC_NUMBER 0x5A5A4B31
/* 定义保留字段的值，当前为 0 */
#define RESERVED 0x00000000

/* 定义数据块类型：UTF-8 文本 */
#define TYPE_TEXT 0x00000001
/* 定义数据块类型：二进制附件 */
#define TYPE_BINARY 0x00000002
/* 定义数据块类型：填充数据 */
#define TYPE_PADDING 0xFFFFFFFF

/* 定义无符号 32 位整数类型，unsigned long 在 C89 中保证至少 32 位 */
typedef unsigned long u32;

/* C89 需要显式函数原型，避免隐式声明 */
int write_u32_be(FILE *fp, u32 val);
int read_u32_be(FILE *fp, u32 *val);

static void die_io(const char *what) {
    perror(what);
    exit(1);
}

static void require_write_u32(FILE *fp, u32 val, const char *what) {
    if (write_u32_be(fp, val) != 0) {
        die_io(what);
    }
}

static void require_fwrite(FILE *fp, const void *buf, size_t len, const char *what) {
    if (len == 0) {
        return;
    }
    if (fwrite(buf, 1, len, fp) != len) {
        die_io(what);
    }
}

static int append_str(char *dst, size_t dst_cap, size_t *used, const char *src) {
    size_t src_len;
    size_t avail;

    if (dst_cap == 0) {
        return -1;
    }
    if (*used >= dst_cap) {
        dst[dst_cap - 1] = '\0';
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
    if (fseek(fp, 0, SEEK_END) != 0) {
        die_io(what);
    }
    pos = ftell(fp);
    if (pos < 0) {
        die_io(what);
    }
    if ((unsigned long)pos > 0xFFFFFFFFUL) {
        fprintf(stderr, "Error: file too large for U32 size field.\n");
        exit(1);
    }
    return (u32)(unsigned long)pos;
}

static void seek_forward(FILE *fp, u32 offset) {
    /* Use a safe step size that fits in signed long (even 32-bit) */
    const long MAX_STEP = 0x70000000; 
    while (offset > 0) {
        long step = (offset > (u32)MAX_STEP) ? MAX_STEP : (long)offset;
        if (fseek(fp, step, SEEK_CUR) != 0) {
            die_io("Error seeking file");
        }
        offset -= step;
    }
}

/* 
 * 函数：write_u32_be
 * 功能：以大端序（Big-Endian）将 32 位整数写入文件
 * 参数：
 *   fp: 文件指针
 *   val: 要写入的 32 位整数值
 * 返回：成功写入返回 0，失败返回 -1
 */
int write_u32_be(FILE *fp, u32 val) {
    /* 定义缓冲区，用于存储 4 个字节 */
    unsigned char buf[4];
    /* 提取最高 8 位 (24-31位) */
    buf[0] = (unsigned char)((val >> 24) & 0xFF);
    /* 提取次高 8 位 (16-23位) */
    buf[1] = (unsigned char)((val >> 16) & 0xFF);
    /* 提取次低 8 位 (8-15位) */
    buf[2] = (unsigned char)((val >> 8) & 0xFF);
    /* 提取最低 8 位 (0-7位) */
    buf[3] = (unsigned char)(val & 0xFF);
    /* 将 4 个字节写入文件 */
    if (fwrite(buf, 1, 4, fp) != 4) {
        /* 如果写入字节数不为 4，表示出错 */
        return -1;
    }
    /* 写入成功 */
    return 0;
}

/* 
 * 函数：read_u32_be
 * 功能：从文件中读取大端序的 32 位整数
 * 参数：
 *   fp: 文件指针
 *   val: 指向存储读取结果的变量的指针
 * 返回：成功读取返回 0，失败或 EOF 返回 -1
 */
int read_u32_be(FILE *fp, u32 *val) {
    /* 定义缓冲区，用于读取 4 个字节 */
    unsigned char buf[4];
    /* 从文件中读取 4 个字节 */
    if (fread(buf, 1, 4, fp) != 4) {
        /* 如果读取字节数不为 4，表示出错或到达文件末尾 */
        return -1;
    }
    /* 将读取的字节按大端序组合成 32 位整数 */
    *val = ((u32)buf[0] << 24) |
           ((u32)buf[1] << 16) |
           ((u32)buf[2] << 8) |
           ((u32)buf[3]);
    /* 读取成功 */
    return 0;
}

/* 
 * 函数：cmd_create
 * 功能：创建一个新的归档文件，并写入文件头和初始文本块
 * 参数：
 *   filename: 文件名
 *   initial_text: 初始文本内容
 */
void cmd_create(const char *filename, const char *initial_text) {
    /* 定义文件指针 */
    FILE *fp;
    /* 定义初始文本的长度 */
    u32 text_len;
    /* 定义文件总大小，初始为文件头大小 */
    u32 total_size = 12; /* Magic(4) + Size(4) + Reserved(4) */

    /* 计算初始文本的长度 */
    text_len = (u32)strlen(initial_text);
    /* 更新文件总大小：加上 TLV 头部 (8字节) 和 Value 长度 */
    /* 检查溢出：Total Size = 12 (Header) + 8 (TLV Header) + text_len */
    /* Max u32 - 20 = 0xFFFFFFEB */
    if (text_len > 0xFFFFFFEBUL) { 
        fprintf(stderr, "Error: text too large (overflow risk).\n");
        exit(1);
    }
    total_size += 8 + text_len;

    /* 以二进制写模式打开文件 ("wb") */
    fp = fopen(filename, "wb");
    /* 检查文件是否成功打开 */
    if (!fp) {
        /* 打开失败，打印错误信息 */
        perror("Error creating file");
        /* 退出程序 */
        exit(1);
    }

    /* --- 写入文件头 --- */
    /* 写入 Magic Number */
    require_write_u32(fp, MAGIC_NUMBER, "Error writing magic");
    /* 写入 Total Size */
    require_write_u32(fp, total_size, "Error writing total size");
    /* 写入 Reserved */
    require_write_u32(fp, RESERVED, "Error writing reserved");

    /* --- 写入初始数据块 (Type 1: Text) --- */
    /* 写入 Type: 1 (Text) */
    require_write_u32(fp, TYPE_TEXT, "Error writing chunk type");
    /* 写入 Length: 文本长度 */
    require_write_u32(fp, text_len, "Error writing chunk length");
    /* 写入 Value: 文本内容 */
    require_fwrite(fp, initial_text, (size_t)text_len, "Error writing chunk value");

    /* 关闭文件 */
    fclose(fp);
    /* 打印成功信息 */
    printf("Archive created: %s\n", filename);
}

/* 
 * 函数：validate_and_open
 * 功能：打开现有文件，验证文件头，并准备追加数据
 * 参数：
 *   filename: 文件名
 *   fp_out: 指向文件指针的指针，用于返回打开的文件
 *   current_size_out: 指向变量的指针，用于返回当前文件头中记录的大小
 * 返回：成功返回 0，失败退出程序
 */
int validate_and_open(const char *filename, FILE **fp_out, u32 *current_size_out) {
    /* 定义临时变量用于读取文件头字段 */
    u32 magic, reserved;
    u32 header_total_size;
    u32 actual_size;
    /* 以读写二进制模式打开文件 ("rb+") */
    FILE *fp = fopen(filename, "rb+");
    
    /* 检查文件是否存在 */
    if (!fp) {
        /* 打开失败 */
        perror("Error opening file");
        /* 退出程序 */
        exit(1);
    }

    /* 读取 Magic Number */
    if (read_u32_be(fp, &magic) != 0 || magic != MAGIC_NUMBER) {
        /* Magic Number 不匹配 */
        fprintf(stderr, "Invalid magic number.\n");
        /* 关闭文件 */
        fclose(fp);
        /* 退出程序 */
        exit(1);
    }

    /* 读取 Total Size */
    if (read_u32_be(fp, &header_total_size) != 0) {
        /* 读取失败 */
        fprintf(stderr, "Error reading size.\n");
        fclose(fp);
        exit(1);
    }

    /* 读取 Reserved */
    if (read_u32_be(fp, &reserved) != 0) {
        /* 读取失败 */
        fprintf(stderr, "Error reading reserved.\n");
        fclose(fp);
        exit(1);
    }

    if (reserved != RESERVED) {
        fprintf(stderr, "Warning: reserved field is non-zero (%lu).\n", (unsigned long)reserved);
    }

    /* 用实际文件大小作为追加基准，避免头部 Total Size 漂移 */
    actual_size = get_file_size_u32_or_die(fp, "Error seeking/ftell file");
    if (header_total_size != actual_size) {
        fprintf(stderr, "Warning: header Total Size (%lu) != actual file size (%lu).\n",
                (unsigned long)header_total_size, (unsigned long)actual_size);
        
        if (actual_size > header_total_size) {
            fprintf(stderr, "Fixing: File has uncommitted data (garbage at end). Overwriting trailing garbage data.\n");
            /* 移动到 header 记录的末尾，准备覆盖垃圾数据 */
            if (fseek(fp, header_total_size, SEEK_SET) != 0) {
                die_io("Error seeking to header total size");
            }
            /* 使用 header 大小作为当前大小 */
            if (current_size_out) {
                *current_size_out = header_total_size;
            }
            *fp_out = fp;
            return 0;
        } else {
            /* actual_size < header_total_size */
            fprintf(stderr, "Error: File is smaller than header claims (Truncated). Corrupted.\n");
            fclose(fp);
            exit(1);
        }
    }

    /* 将文件指针移动到文件末尾，准备追加 */
    if (fseek(fp, 0, SEEK_END) != 0) {
        die_io("Error seeking to end");
    }
    
    /* 返回文件指针 */
    *fp_out = fp;
    if (current_size_out) {
        *current_size_out = actual_size;
    }

    /* 返回成功 */
    return 0;
}

/* 
 * 函数：update_total_size
 * 功能：更新文件头中的 Total Size 字段
 * 参数：
 *   fp: 文件指针
 *   added_size: 新增的数据字节数
 *   old_size: 修改前的文件大小记录
 */
void update_total_size(FILE *fp, u32 added_size, u32 old_size) {
    /* 计算新的总大小 */
    u32 new_size;
    if (old_size > 0xFFFFFFFFUL - added_size) {
        fprintf(stderr, "Error: file size overflow (exceeds 4GB limit).\n");
        exit(1);
    }
    new_size = old_size + added_size;
    /* 定位到文件头中 Total Size 的偏移量 (0x04) */
    if (fseek(fp, 4, SEEK_SET) != 0) {
        die_io("Error seeking to header size field");
    }
    /* 写入新的 Total Size */
    require_write_u32(fp, new_size, "Error writing updated total size");
    /* 将文件指针移回文件末尾，虽然对于关闭文件来说不是必须的，但保持状态一致是个好习惯 */
    if (fseek(fp, 0, SEEK_END) != 0) {
        die_io("Error seeking to end after updating size");
    }
}

/* 
 * 函数：cmd_append
 * 功能：向归档追加文本块
 * 参数：
 *   filename: 归档文件名
 *   text: 要追加的文本内容
 */
void cmd_append(const char *filename, const char *text) {
    /* 定义文件指针 */
    FILE *fp;
    /* 定义文件大小变量 */
    u32 current_size;
    /* 定义文本长度 */
    u32 text_len = (u32)strlen(text);
    /* 计算本块增加的大小 (Type + Length + Value) */
    u32 chunk_size;
    if (text_len > 0xFFFFFFF7UL) {
        fprintf(stderr, "Error: text too large (overflow risk).\n");
        exit(1);
    }
    chunk_size = 8 + text_len;

    /* 验证并打开文件 */
    validate_and_open(filename, &fp, &current_size);

    /* 写入 Type: 1 (Text) */
    require_write_u32(fp, TYPE_TEXT, "Error writing chunk type");
    /* 写入 Length */
    require_write_u32(fp, text_len, "Error writing chunk length");
    /* 写入 Value */
    require_fwrite(fp, text, (size_t)text_len, "Error writing chunk value");

    /* 更新文件头中的总大小 */
    update_total_size(fp, chunk_size, current_size);

    /* 关闭文件 */
    fclose(fp);
    /* 打印成功信息 */
    printf("Appended text to: %s\n", filename);
}

/* 
 * 函数：cmd_append_file
 * 功能：向归档追加二进制文件（带元数据块）
 * 参数：
 *   archive_name: 归档文件名
 *   target_file: 要追加的外部文件名
 *   description: 文件描述
 */
void cmd_append_file(const char *archive_name, const char *target_file, const char *description) {
    /* 定义归档文件指针 */
    FILE *fp_archive;
    /* 定义目标文件指针 */
    FILE *fp_target;
    /* 定义文件大小变量 */
    u32 current_size;
    /* 定义目标文件大小 */
    u32 target_size;
    /* 定义元数据缓冲区 */
    char metadata[1024];
    /* 定义元数据长度 */
    u32 meta_len;
    /* 定义读取缓冲区 */
    unsigned char buffer[4096];
    /* 定义读取字节数 */
    size_t bytes_read;
    /* 定义总增加大小 */
    u32 total_added = 0;

    /* 打开目标文件以读取二进制数据 */
    fp_target = fopen(target_file, "rb");
    /* 检查目标文件是否打开成功 */
    if (!fp_target) {
        perror("Error opening target file");
        exit(1);
    }

    /* 获取目标文件大小 */
    target_size = get_file_size_u32_or_die(fp_target, "Error seeking/ftell target file");
    /* 重置目标文件指针到开头 */
    if (fseek(fp_target, 0, SEEK_SET) != 0) {
        die_io("Error seeking target file to start");
    }

    /* 格式化元数据字符串（避免 sprintf 溢出） */
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
            fprintf(stderr, "Warning: metadata truncated to %lu bytes.\n", (unsigned long)(sizeof(metadata) - 1));
        }
    }
    meta_len = (u32)strlen(metadata);

    /* 验证并打开归档文件 */
    validate_and_open(archive_name, &fp_archive, &current_size);

    /* --- 写入元数据块 (Type 1) --- */
    /* 写入 Type */
    require_write_u32(fp_archive, TYPE_TEXT, "Error writing metadata chunk type");
    /* 写入 Length */
    require_write_u32(fp_archive, meta_len, "Error writing metadata chunk length");
    /* 写入 Value */
    require_fwrite(fp_archive, metadata, (size_t)meta_len, "Error writing metadata chunk value");
    /* 累加增加的大小 */
    if (meta_len > 0xFFFFFFF7UL) {
        fprintf(stderr, "Error: metadata too large (overflow risk).\n");
        exit(1);
    }
    total_added += 8 + meta_len;

    /* --- 写入二进制块 (Type 2) --- */
    /* 写入 Type */
    require_write_u32(fp_archive, TYPE_BINARY, "Error writing binary chunk type");
    /* 写入 Length */
    require_write_u32(fp_archive, target_size, "Error writing binary chunk length");
    /* 循环读取目标文件并写入归档 */
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp_target)) > 0) {
        require_fwrite(fp_archive, buffer, bytes_read, "Error writing binary chunk value");
    }
    if (ferror(fp_target)) {
        die_io("Error reading target file");
    }
    /* 累加增加的大小 */
    if (target_size > 0xFFFFFFF7UL || total_added > 0xFFFFFFFFUL - (8 + target_size)) {
        fprintf(stderr, "Error: file size overflow (exceeds 4GB limit).\n");
        exit(1);
    }
    total_added += 8 + target_size;

    /* 更新文件头总大小 */
    update_total_size(fp_archive, total_added, current_size);

    /* 关闭文件 */
    fclose(fp_target);
    fclose(fp_archive);
    /* 打印成功信息 */
    printf("Appended file '%s' to: %s\n", target_file, archive_name);
}

/* 
 * 函数：cmd_extract
 * 功能：从归档中提取指定块的数据到文件
 * 参数：
 *   archive_name: 归档文件名
 *   chunk_index_str: 块索引字符串（1-based）
 *   output_file: 输出文件名
 */
void cmd_extract(const char *archive_name, const char *chunk_index_str, const char *output_file) {
    FILE *fp_in, *fp_out;
    u32 magic, total_size, reserved;
    u32 type, length;
    int target_index;
    int current_index = 0;
    unsigned char buffer[4096];
    u32 bytes_remaining;
    size_t to_read;
    char *endptr;
    long parsed_value;

    /* 使用 strtol 替代 atoi，提供更好的错误检查 */
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

    /* Skip header */
    if (read_u32_be(fp_in, &magic) != 0 || magic != MAGIC_NUMBER) {
        fprintf(stderr, "Invalid magic number.\n");
        fclose(fp_in);
        exit(1);
    }
    read_u32_be(fp_in, &total_size);
    read_u32_be(fp_in, &reserved);

    while (1) {
        if (read_u32_be(fp_in, &type) != 0) break; /* EOF */
        if (read_u32_be(fp_in, &length) != 0) {
            fprintf(stderr, "Warning: Unexpected EOF reading chunk length.\n");
            break;
        }
        
        current_index++;
        
        if (current_index == target_index) {
            /* Found the chunk */
            printf("Extracting Chunk #%d (Type %lu, %lu bytes) to '%s'...\n", 
                   target_index, (unsigned long)type, (unsigned long)length, output_file);
            
            fp_out = fopen(output_file, "wb");
            if (!fp_out) {
                perror("Error opening output file");
                fclose(fp_in);
                exit(1);
            }

            bytes_remaining = length;
            while (bytes_remaining > 0) {
                to_read = (bytes_remaining > sizeof(buffer)) ? sizeof(buffer) : bytes_remaining;
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
                bytes_remaining -= to_read;
            }

            fclose(fp_out);
            fclose(fp_in);
            printf("Extraction complete.\n");
            return;
        }

        /* Skip this chunk */
        seek_forward(fp_in, length);
    }

    fprintf(stderr, "Error: Chunk #%d not found.\n", target_index);
    fclose(fp_in);
    exit(1);
}

/* 
 * 函数：cmd_list
 * 功能：列出归档文件中的所有数据块
 * 参数：
 *   filename: 归档文件名
 */
void cmd_list(const char *filename) {
    /* 定义文件指针 */
    FILE *fp;
    /* 定义头部字段变量 */
    u32 magic, total_size, reserved;
    /* 定义块字段变量 */
    u32 type, length;
    /* 定义块计数器 */
    int chunk_count = 0;
    /* 定义缓冲区 */
    unsigned char *buffer;

    /* 以只读二进制模式打开文件 */
    fp = fopen(filename, "rb");
    if (!fp) {
        perror("Error opening file");
        exit(1);
    }

    /* 读取并验证文件头 */
    if (read_u32_be(fp, &magic) != 0 || magic != MAGIC_NUMBER) {
        fprintf(stderr, "Invalid magic number.\n");
        fclose(fp);
        exit(1);
    }
    read_u32_be(fp, &total_size);
    read_u32_be(fp, &reserved);

    /* 打印文件头信息 */
    printf("File: %s (Size: %lu)\n", filename, (unsigned long)total_size);
    printf("----------------------------------------\n");

    /* 循环读取数据块 */
    while (1) {
        /* 尝试读取 Type */
        if (read_u32_be(fp, &type) != 0) {
            /* 如果读取失败（EOF），则退出循环 */
            break;
        }
        /* 读取 Length */
        if (read_u32_be(fp, &length) != 0) {
            /* 如果有 Type 但没有 Length，说明文件截断 */
            fprintf(stderr, "Warning: Unexpected EOF reading chunk length.\n");
            break;
        }

        /* 增加块计数 */
        chunk_count++;
        /* 打印块头部信息 */
        printf("Chunk #%d: Type=%lu, Length=%lu bytes\n", chunk_count, (unsigned long)type, (unsigned long)length);

        /* 根据类型处理 Value */
        if (type == TYPE_TEXT) {
            /* 安全检查：防止 length + 1 溢出或分配过大内存 */
            if (length == 0xFFFFFFFF || length > 0x10000000) { /* 限制为 256MB */
                fprintf(stderr, "Warning: Text chunk too large (%lu). Skipping print.\n", (unsigned long)length);
                seek_forward(fp, length);
            } else {
                /* 如果是文本，分配内存读取并打印 */
                buffer = (unsigned char *)malloc(length + 1);
                if (buffer) {
                    /* 读取内容 */
                    if (fread(buffer, 1, length, fp) == length) {
                        /* 添加字符串结束符 */
                        buffer[length] = '\0';
                        /* 打印内容 */
                        printf("Content:\n%s\n", buffer);
                    } else {
                        fprintf(stderr, "Warning: Unexpected EOF reading chunk body.\n");
                    }
                    /* 释放内存 */
                    free(buffer);
                } else {
                    fprintf(stderr, "Error: Memory allocation failed.\n");
                    /* 跳过该块 */
                    seek_forward(fp, length);
                }
            }
        } else if (type == TYPE_BINARY) {
            /* 如果是二进制，跳过内容并提示 */
            printf("[Binary Data - Skipped]\n");
            seek_forward(fp, length);
        } else if (type == TYPE_PADDING) {
            /* 如果是填充，跳过 */
            printf("[Padding - Skipped]\n");
            seek_forward(fp, length);
        } else {
            /* 未知类型，跳过（向前兼容） */
            printf("[Unknown Type - Skipped]\n");
            seek_forward(fp, length);
        }
        printf("----------------------------------------\n");
    }

    /* 关闭文件 */
    fclose(fp);
}

/* 
 * 函数：main
 * 功能：程序入口，解析命令行参数并调用相应功能
 * 参数：
 *   argc: 参数个数
 *   argv: 参数列表
 * 返回：程序退出码
 */
int main(int argc, char *argv[]) {
    /* 定义命令字符串指针 */
    const char *command;

    /* 检查参数个数，至少需要一个命令 */
    if (argc < 2) {
        /* 打印用法帮助 */
        printf("Usage:\n");
        printf("  %s create <archive> <text>\n", argv[0]);
        printf("  %s append <archive> <text>\n", argv[0]);
        printf("  %s append-file <archive> <file> <description>\n", argv[0]);
        printf("  %s extract <archive> <chunk_index> <output_file>\n", argv[0]);
        printf("  %s list <archive>\n", argv[0]);
        return 1;
    }

    /* 获取命令字符串 */
    command = argv[1];

    /* 匹配 create 命令 */
    if (strcmp(command, "create") == 0) {
        /* 检查参数个数 */
        if (argc != 4) {
            fprintf(stderr, "Usage: %s create <archive> <text>\n", argv[0]);
            return 1;
        }
        /* 调用 create 功能 */
        cmd_create(argv[2], argv[3]);
    } 
    /* 匹配 append 命令 */
    else if (strcmp(command, "append") == 0) {
        /* 检查参数个数 */
        if (argc != 4) {
            fprintf(stderr, "Usage: %s append <archive> <text>\n", argv[0]);
            return 1;
        }
        /* 调用 append 功能 */
        cmd_append(argv[2], argv[3]);
    } 
    /* 匹配 append-file 命令 */
    else if (strcmp(command, "append-file") == 0) {
        /* 检查参数个数 */
        if (argc != 5) {
            fprintf(stderr, "Usage: %s append-file <archive> <file> <description>\n", argv[0]);
            return 1;
        }
        /* 调用 append-file 功能 */
        cmd_append_file(argv[2], argv[3], argv[4]);
    } 
    /* 匹配 extract 命令 */
    else if (strcmp(command, "extract") == 0) {
        /* 检查参数个数 */
        if (argc != 5) {
            fprintf(stderr, "Usage: %s extract <archive> <chunk_index> <output_file>\n", argv[0]);
            return 1;
        }
        /* 调用 extract 功能 */
        cmd_extract(argv[2], argv[3], argv[4]);
    }
    /* 匹配 list 命令 */
    else if (strcmp(command, "list") == 0) {
        /* 检查参数个数 */
        if (argc != 3) {
            fprintf(stderr, "Usage: %s list <archive>\n", argv[0]);
            return 1;
        }
        /* 调用 list 功能 */
        cmd_list(argv[2]);
    } 
    /* 未知命令 */
    else {
        fprintf(stderr, "Unknown command: %s\n", command);
        return 1;
    }

    /* 程序正常退出 */
    return 0;
}
