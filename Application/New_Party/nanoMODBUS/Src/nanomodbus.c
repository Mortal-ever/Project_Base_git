/*
    nanoMODBUS - A compact MODBUS RTU/TCP C library for microcontrollers

    MIT License

    Copyright (c) 2026 Valerio De Benedetto (@debevv)

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#include "nanomodbus.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * ============================================================================
 * 中文学习注释版说明
 * ============================================================================
 * 本文件在原 nanoMODBUS 实现基础上增加中文阅读注释，不改变协议逻辑。
 *
 * 阅读时可以把源码分成 6 层：
 *
 *   1) get/put/set/get_n 等          : 消息缓冲区与游标操作
 *   2) recv/send                     : nanoMODBUS 与平台 read/write 的边界
 *   3) msg/header/footer             : RTU/TCP ADU 公共封装与校验
 *   4) recv_xxx_res                  : Client 响应解析
 *   5) handle_xxx                    : Server 请求解析与 callback 分发
 *   6) nmbs_xxx 公共 API             : Client/Server 对外接口
 *
 * 最核心的数据结构是 nmbs->msg.buf + nmbs->msg.buf_idx：
 *   - put_xxx()  顺序构造报文，并推进 buf_idx
 *   - get_xxx()  顺序解析报文，并推进 buf_idx
 *   - set_xxx()  定点回写字段，不推进 buf_idx
 *
 * 平台边界：
 *   nanoMODBUS -> platform.read/write/flush/crc_calc -> 项目 Transport/UART/TCP
 * ============================================================================
 */


#define NMBS_UNUSED_PARAM(x) ((x) = (x))

#ifdef NMBS_DEBUG
#include <stdio.h>
#define NMBS_DEBUG_PRINT(...) printf(__VA_ARGS__)
#else
#define NMBS_DEBUG_PRINT(...) (void) (0)
#endif



/* ========================================================================== */
/* 消息缓冲区基础编解码工具 */
/* ========================================================================== */

/**
 * @brief 从当前消息游标位置读取 1 个字节。
 *
 * 读取 msg.buf[buf_idx] 后将 buf_idx 加 1。
 * 这是 nanoMODBUS 最基础的“顺序解析”操作之一。
 */
static uint8_t get_1(nmbs_t* nmbs) {
    uint8_t result = nmbs->msg.buf[nmbs->msg.buf_idx];
    // 顺序读取后推进游标，下一次 get_xxx() 会从下一个字段继续解析。
    nmbs->msg.buf_idx++;
    return result;
}


/**
 * @brief 向当前消息游标位置写入 1 个字节。
 *
 * 写入 msg.buf[buf_idx] 后将 buf_idx 加 1。
 * 用于按顺序构造 Modbus 报文。
 */
static void put_1(nmbs_t* nmbs, uint8_t data) {
    nmbs->msg.buf[nmbs->msg.buf_idx] = data;
    nmbs->msg.buf_idx++;
}


/**
 * @brief 跳过当前消息中的 1 个字节。
 *
 * 不读取数据，只推进 buf_idx。
 */
static void discard_1(nmbs_t* nmbs) {
    nmbs->msg.buf_idx++;
}


#ifndef NMBS_SERVER_DISABLED
#if !defined(NMBS_SERVER_READ_FILE_RECORD_DISABLED) || !defined(NMBS_SERVER_WRITE_FILE_RECORD_DISABLED)
/**
 * @brief 跳过当前消息中的 n 个字节。
 *
 * 不复制、不解析数据，只把 buf_idx 向后移动 n。
 */
static void discard_n(nmbs_t* nmbs, uint16_t n) {
    nmbs->msg.buf_idx += n;
}
#endif
#endif


/**
 * @brief 从当前消息位置读取一个 16 位值。
 *
 * Modbus 多字节字段采用高字节在前的顺序：
 *     buf[i] buf[i+1] = 0x12 0x34  ->  0x1234
 * 读取完成后 buf_idx += 2。
 */
static uint16_t get_2(nmbs_t* nmbs) {
    const uint16_t result =
            ((uint16_t) nmbs->msg.buf[nmbs->msg.buf_idx]) << 8 | (uint16_t) nmbs->msg.buf[nmbs->msg.buf_idx + 1];
    nmbs->msg.buf_idx += 2;
    return result;
}


/**
 * @brief 将一个 16 位值按高字节在前写入消息缓冲区。
 *
 * 例如 0x1234 会写成两个字节：0x12 0x34。
 * 写入完成后 buf_idx += 2。
 */
static void put_2(nmbs_t* nmbs, uint16_t data) {
    nmbs->msg.buf[nmbs->msg.buf_idx] = (uint8_t) ((data >> 8) & 0xFFU);
    // 强转为 uint8_t 会保留 data 的低 8 位。
    nmbs->msg.buf[nmbs->msg.buf_idx + 1] = (uint8_t) data;
    // 一个 16 位字段占 2 个字节，因此游标向后移动 2。
    nmbs->msg.buf_idx += 2;
}


#ifndef NMBS_SERVER_DISABLED
#ifndef NMBS_SERVER_READ_DEVICE_IDENTIFICATION_DISABLED
/**
 * @brief 在消息缓冲区指定 index 位置写入 1 个字节。
 *
 * 与 put_1() 不同：本函数不会修改 buf_idx。
 * 适合报文基本构造完成后“回头修改”固定字段。
 */
static void set_1(nmbs_t* nmbs, uint8_t data, uint8_t index) {
    nmbs->msg.buf[index] = data;
}


/**
 * @brief 在指定 index 位置写入一个 16 位大端值。
 *
 * 与 put_2() 不同：不会推进 buf_idx。
 * 典型用途是回填 Modbus TCP MBAP 的 Length 字段。
 */
static void set_2(nmbs_t* nmbs, uint16_t data, uint8_t index) {
    nmbs->msg.buf[index] = (uint8_t) ((data >> 8) & 0xFFU);
    nmbs->msg.buf[index + 1] = (uint8_t) data;
}
#endif
#endif


/**
 * @brief 取得当前游标开始的连续 n 字节区域。
 *
 * 不执行 memcpy；返回的是 msg.buf 内部地址。
 * 返回后 buf_idx += n，因此本质是“取得这一段并消费它”。
 */
static uint8_t* get_n(nmbs_t* nmbs, uint16_t n) {
    // 这里只取得内部 buffer 的地址，不复制数据。
    uint8_t* msg_buf_ptr = nmbs->msg.buf + nmbs->msg.buf_idx;
    // 将这一段视为已被“消费”。
    nmbs->msg.buf_idx += n;
    return msg_buf_ptr;
}


#ifndef NMBS_SERVER_DISABLED
#ifndef NMBS_SERVER_READ_DEVICE_IDENTIFICATION_DISABLED
/**
 * @brief 将连续 size 个字节复制到当前消息缓冲区。
 *
 * 使用 memcpy() 批量写入，完成后 buf_idx += size。
 */
static void put_n(nmbs_t* nmbs, const uint8_t* data, uint8_t size) {
    memcpy(&nmbs->msg.buf[nmbs->msg.buf_idx], data, size);
    nmbs->msg.buf_idx += size;
}
#endif


#ifndef NMBS_SERVER_WRITE_FILE_RECORD_DISABLED
/**
 * @brief 从消息缓冲区取得 n 个 16 位寄存器，并进行字节交换。
 *
 * 该实现直接把 uint8_t 缓冲区转换为 uint16_t*，随后对每个寄存器
 * 交换高低字节，使其从 Modbus 字节序转换为本机可用的 uint16_t。
 * 同时 buf_idx += n * 2。
 *
 * 注意：这种直接的 uint16_t* 强制转换依赖内存对齐/别名规则，
 * 可移植性不如逐字节的 get_2()。
 */
static uint16_t* get_regs(nmbs_t* nmbs, uint16_t n) {
    uint16_t* msg_buf_ptr = (uint16_t*) (nmbs->msg.buf + nmbs->msg.buf_idx);
    nmbs->msg.buf_idx += n * 2;
    while (n--) {
        msg_buf_ptr[n] = (msg_buf_ptr[n] << 8) | ((msg_buf_ptr[n] >> 8) & 0xFF);
    }
    return msg_buf_ptr;
}
#endif
#endif


#ifndef NMBS_CLIENT_DISABLED
/**
 * @brief 将 n 个 uint16_t 寄存器批量写入消息缓冲区。
 *
 * 写入前对每个寄存器交换高低字节，使缓冲区中的字节排列符合
 * Modbus 高字节在前的格式；buf_idx += n * 2。
 */
static void put_regs(nmbs_t* nmbs, const uint16_t* data, uint16_t n) {
    uint16_t* msg_buf_ptr = (uint16_t*) (nmbs->msg.buf + nmbs->msg.buf_idx);
    nmbs->msg.buf_idx += n * 2;
    while (n--) {
        msg_buf_ptr[n] = (data[n] << 8) | ((data[n] >> 8) & 0xFF);
    }
}
#endif


/**
 * @brief 批量交换 uint16_t 数组中每个元素的高、低 8 位。
 *
 * 例如：0x1234 -> 0x3412。再次调用可恢复原值。
 */
static void swap_regs(uint16_t* data, uint16_t n) {
    while (n--) {
        data[n] = (data[n] << 8) | ((data[n] >> 8) & 0xFF);
    }
}



/* ========================================================================== */
/* 平台收发适配层 */
/* ========================================================================== */

/**
 * @brief 从平台 read() 回调精确接收 count 个字节。
 *
 * 返回值映射规则：
 *   ret == count       -> NMBS_ERROR_NONE
 *   0 <= ret < count   -> NMBS_ERROR_TIMEOUT
 *   ret < 0            -> NMBS_ERROR_TRANSPORT
 *   ret > count        -> NMBS_ERROR_TRANSPORT
 *
 * 对 TCP：如果 msg.complete 已置位，说明整帧已经提前收进 msg.buf，
 * 后续解析阶段无需再次访问底层传输。
 */
static nmbs_error recv(nmbs_t* nmbs, uint16_t count) {
    if (nmbs->msg.complete) {
        return NMBS_ERROR_NONE;
    }

    if (nmbs->msg.buf_idx > sizeof(nmbs->msg.buf) ||
        count > sizeof(nmbs->msg.buf) - nmbs->msg.buf_idx)
        return NMBS_ERROR_INVALID_RESPONSE;
        
    // 读取 count 个字节到 buf[buf_idx]，并推进 buf_idx。
    const int32_t ret =
            nmbs->platform.read(nmbs->msg.buf + nmbs->msg.buf_idx, count, nmbs->byte_timeout_ms, nmbs->platform.arg);

    // nanoMODBUS 要求平台层尽量完成“精确 count 字节”收/发。
    if (ret == count)
        return NMBS_ERROR_NONE;

    if (ret < count) {
        // 负值代表底层 I/O 真正失败；非负但不足 count 代表超时/部分完成。
        if (ret < 0)
            return NMBS_ERROR_TRANSPORT;

        return NMBS_ERROR_TIMEOUT;
    }

    return NMBS_ERROR_TRANSPORT;
}


/**
 * @brief 通过平台 write() 回调发送 count 个字节。
 *
 * 与 recv() 使用相同的返回值约定：
 * 完整发送才算成功，部分发送视为超时，负值视为传输错误。
 */
static nmbs_error send(const nmbs_t* nmbs, uint16_t count) {
    const int32_t ret = nmbs->platform.write(nmbs->msg.buf, count, nmbs->byte_timeout_ms, nmbs->platform.arg);

    // nanoMODBUS 要求平台层尽量完成“精确 count 字节”收/发。
    if (ret == count)
        return NMBS_ERROR_NONE;

    if (ret < count) {
        // 负值代表底层 I/O 真正失败；非负但不足 count 代表超时/部分完成。
        if (ret < 0)
            return NMBS_ERROR_TRANSPORT;

        return NMBS_ERROR_TIMEOUT;
    }

    return NMBS_ERROR_TRANSPORT;
}


/**
 * @brief nanoMODBUS 默认的接收缓冲清理实现。
 *
 * 使用 byte_timeout_ms = 0 调用平台 read()，即执行一次非阻塞读取，
 * 将当前可读的残留数据丢弃。项目可以通过 platform.flush 覆盖它。
 */
static void flush(nmbs_t* nmbs, void* arg) {
    NMBS_UNUSED_PARAM(arg);
    nmbs->platform.read(nmbs->msg.buf, sizeof(nmbs->msg.buf), 0, nmbs->platform.arg);
}



/* ========================================================================== */
/* 消息状态管理 */
/* ========================================================================== */

/**
 * @brief 仅重置消息缓冲区游标。
 *
 * 不清空 buf 内容，因为后续写入/解析只依赖 buf_idx 指示的有效区域。
 */
static void msg_buf_reset(nmbs_t* nmbs) {
    nmbs->msg.buf_idx = 0;
}


/**
 * @brief 重置当前报文的解析/构造状态。
 *
 * 除 buf_idx 外，同时清除 Unit ID、功能码、Transaction ID，
 * 以及 broadcast / ignored / complete 等状态标志。
 */
static void msg_state_reset(nmbs_t* nmbs) {
    msg_buf_reset(nmbs);
    nmbs->msg.unit_id = 0;
    nmbs->msg.fc = 0;
    nmbs->msg.transaction_id = 0;
    nmbs->msg.broadcast = false;
    nmbs->msg.ignored = false;
    nmbs->msg.complete = false;
}


#ifndef NMBS_CLIENT_DISABLED
/**
 * @brief 开始一笔新的 Client 请求。
 *
 * 主要步骤：
 *   1. 生成新的 TCP Transaction ID；
 *   2. flush 掉链路上的残留数据；
 *   3. 重置消息状态；
 *   4. 设置目标 Unit ID、功能码和 Transaction ID；
 *   5. RTU Unit ID=0 时标记为广播。
 */
static void msg_state_req(nmbs_t* nmbs, uint8_t fc) {
    if (nmbs->current_tid == UINT16_MAX)
        nmbs->current_tid = 1;
    else
        nmbs->current_tid++;

    // Flush the remaining data on the line before sending the request
    // 新请求开始前清理旧数据，避免上一帧残留污染本次响应。
    nmbs->platform.flush(nmbs, nmbs->platform.arg);

    msg_state_reset(nmbs);
    nmbs->msg.unit_id = nmbs->dest_address_rtu;
    nmbs->msg.fc = fc;
    nmbs->msg.transaction_id = nmbs->current_tid;
    if (nmbs->msg.unit_id == NMBS_BROADCAST_ADDRESS && nmbs->platform.transport == NMBS_TRANSPORT_RTU)
        nmbs->msg.broadcast = true;
}
#endif



/* ========================================================================== */
/* 实例与平台配置 */
/* ========================================================================== */

/**
 * @brief 创建 nanoMODBUS 基础实例。
 *
 * 初始化超时默认值，校验 platform_conf 是否有效，
 * 并把 platform_conf 按值复制到 nmbs->platform。
 * 因此调用完成后，外部 platform_conf 结构本身可以离开作用域，
 * 但其中 arg 指向的上下文对象仍必须保持有效。
 */
nmbs_error nmbs_create(nmbs_t* nmbs, const nmbs_platform_conf* platform_conf) {
    if (!nmbs)
        return NMBS_ERROR_INVALID_ARGUMENT;

    memset(nmbs, 0, sizeof(nmbs_t));

    nmbs->byte_timeout_ms = -1;
    nmbs->read_timeout_ms = -1;

    if (!platform_conf || platform_conf->initialized != 0xFFFFDEBE)
        return NMBS_ERROR_INVALID_ARGUMENT;

    if (platform_conf->transport != NMBS_TRANSPORT_RTU && platform_conf->transport != NMBS_TRANSPORT_TCP)
        return NMBS_ERROR_INVALID_ARGUMENT;

    if (!platform_conf->read || !platform_conf->write)
        return NMBS_ERROR_INVALID_ARGUMENT;

    // 按值复制配置结构；不是保存 platform_conf 指针。
    nmbs->platform = *platform_conf;

    return NMBS_ERROR_NONE;
}


/**
 * @brief 设置“等待首字节/响应开始”的超时时间。
 *
 * Client 中主要用于等待响应首字节；
 * Server 中用于 nmbs_server_poll() 等待新请求。
 */
void nmbs_set_read_timeout(nmbs_t* nmbs, int32_t timeout_ms) {
    nmbs->read_timeout_ms = timeout_ms;
}


/**
 * @brief 设置报文后续字节之间的超时时间。
 *
 * 首字节通常使用 read_timeout_ms；首字节到达后恢复使用该值。
 */
void nmbs_set_byte_timeout(nmbs_t* nmbs, int32_t timeout_ms) {
    nmbs->byte_timeout_ms = timeout_ms;
}


/**
 * @brief 初始化平台适配配置。
 *
 * 设置默认 CRC、默认 flush，并写入 initialized 魔数。
 * read/write/transport 仍需由上层平台代码提供。
 */
void nmbs_platform_conf_create(nmbs_platform_conf* platform_conf) {
    memset(platform_conf, 0, sizeof(nmbs_platform_conf));
    platform_conf->crc_calc = nmbs_crc_calc;
    platform_conf->flush = flush;
    // Workaround for older user code not calling nmbs_platform_conf_create()
    platform_conf->initialized = 0xFFFFDEBE;
}


/**
 * @brief 设置 Client 下一次请求的目标 Unit ID。
 *
 * 名字保留了 RTU 历史语义；该字段也会用于构造 TCP MBAP 的 Unit Identifier。
 */
void nmbs_set_destination_rtu_address(nmbs_t* nmbs, uint8_t address) {
    nmbs->dest_address_rtu = address;
}


/**
 * @brief 修改平台 read/write/flush/crc 回调收到的用户上下文指针。
 */
void nmbs_set_platform_arg(nmbs_t* nmbs, void* arg) {
    nmbs->platform.arg = arg;
}



/* ========================================================================== */
/* RTU CRC 与 RTU/TCP 报文封装 */
/* ========================================================================== */

/**
 * @brief 计算 Modbus RTU CRC16。
 *
 * 内部使用多项式 0xA001。函数最后交换 CRC 的高低字节，
 * 使其配合统一的 put_2() 后得到 RTU 线上所需的 CRC 字节顺序。
 */
uint16_t nmbs_crc_calc(const uint8_t* data, uint32_t length, void* arg) {
    NMBS_UNUSED_PARAM(arg);
    uint16_t crc = 0xFFFF;
    for (uint_fast32_t i = 0; i < length; i++) {
        crc ^= (uint16_t) data[i];
        for (uint_fast8_t j = 8; j != 0; j--) {
            if ((crc & 0x0001) != 0) {
                crc >>= 1;
                crc ^= 0xA001;
            }
            else
                crc >>= 1;
        }
    }

    return (uint16_t) (crc << 8) | (uint16_t) (crc >> 8);
}

/**
 * @brief 接收并校验报文尾部。
 *
 * RTU：继续接收 2 字节 CRC，并与本地计算值比较。
 * TCP：没有 CRC 尾部，因此直接成功。
 */
static nmbs_error recv_msg_footer(nmbs_t* nmbs) {
    NMBS_DEBUG_PRINT("\n");

    if (nmbs->platform.transport == NMBS_TRANSPORT_RTU) {
        const uint16_t crc = nmbs->platform.crc_calc(nmbs->msg.buf, nmbs->msg.buf_idx, nmbs->platform.arg);

        const nmbs_error err = recv(nmbs, 2);
        if (err != NMBS_ERROR_NONE)
            return err;

        const uint16_t recv_crc = get_2(nmbs);
        if (recv_crc != crc)
            return NMBS_ERROR_CRC;
    }

    return NMBS_ERROR_NONE;
}


/**
 * @brief 接收并解析一帧 RTU/TCP 报文头。
 *
 * 首字节使用 read_timeout_ms 等待；收到首字节后恢复 byte_timeout_ms。
 *
 * RTU：读取 Unit ID + Function Code。
 * TCP：解析 MBAP（Transaction ID、Protocol ID、Length、Unit ID、FC），
 *      根据 Length 一次性把剩余数据接收进 msg.buf，并置 complete=true。
 */
static nmbs_error recv_msg_header(nmbs_t* nmbs, bool* first_byte_received) {
    // We wait for the read timeout here, just for the first message byte
    int32_t old_byte_timeout = nmbs->byte_timeout_ms;
    // 仅等待“报文第一个字节”时临时使用 read_timeout。
    nmbs->byte_timeout_ms = nmbs->read_timeout_ms;

    msg_state_reset(nmbs);

    *first_byte_received = false;

    if (nmbs->platform.transport == NMBS_TRANSPORT_RTU) {
        nmbs_error err = recv(nmbs, 1);

        // 首字节已到，后续字节恢复使用 byte_timeout。
        nmbs->byte_timeout_ms = old_byte_timeout;

        if (err != NMBS_ERROR_NONE)
            return err;

        *first_byte_received = true;

        nmbs->msg.unit_id = get_1(nmbs);

        err = recv(nmbs, 1);
        if (err != NMBS_ERROR_NONE)
            return err;

        nmbs->msg.fc = get_1(nmbs);
    }
    else if (nmbs->platform.transport == NMBS_TRANSPORT_TCP) {
        nmbs_error err = recv(nmbs, 1);

        // 首字节已到，后续字节恢复使用 byte_timeout。
        nmbs->byte_timeout_ms = old_byte_timeout;

        if (err != NMBS_ERROR_NONE)
            return err;

        *first_byte_received = true;

        // Advance buf_idx
        discard_1(nmbs);

        err = recv(nmbs, 7);
        if (err != NMBS_ERROR_NONE)
            return err;

        // Starting over
        msg_buf_reset(nmbs);

        nmbs->msg.transaction_id = get_2(nmbs);
        const uint16_t protocol_id = get_2(nmbs);
        const uint16_t length = get_2(nmbs);
        nmbs->msg.unit_id = get_1(nmbs);
        nmbs->msg.fc = get_1(nmbs);

        if (length < 2 || length > 254)
            return NMBS_ERROR_INVALID_TCP_MBAP;

        // Receive the rest of the message
        err = recv(nmbs, length - 2);
        if (err != NMBS_ERROR_NONE)
            return err;

        if (protocol_id != 0)
            return NMBS_ERROR_INVALID_TCP_MBAP;

        // TCP 的 MBAP Length 已告诉我们整帧长度，剩余 payload 已全部收进 buf。
        nmbs->msg.complete = true;
    }

    return NMBS_ERROR_NONE;
}


/**
 * @brief 根据 RTU/TCP 类型构造公共报文头。
 *
 * RTU：Unit ID + Function Code。
 * TCP：Transaction ID + Protocol ID(0) + Length + Unit ID + Function Code。
 * data_length 表示功能码之后的数据长度。
 */
static void put_msg_header(nmbs_t* nmbs, uint16_t data_length) {
    msg_buf_reset(nmbs);
    // RTU/TCP 报文头的分支处理
    if (nmbs->platform.transport == NMBS_TRANSPORT_RTU) {
        put_1(nmbs, nmbs->msg.unit_id);
    }
    else if (nmbs->platform.transport == NMBS_TRANSPORT_TCP) {
        put_2(nmbs, nmbs->msg.transaction_id);
        put_2(nmbs, 0);
        // MBAP Length = Unit ID(1) + Function Code(1) + PDU Data(data_length)。
        put_2(nmbs, (uint16_t) (1 + 1 + data_length));
        put_1(nmbs, nmbs->msg.unit_id);
    }

    put_1(nmbs, nmbs->msg.fc);
}


#ifndef NMBS_SERVER_DISABLED
#ifndef NMBS_SERVER_READ_DEVICE_IDENTIFICATION_DISABLED
/**
 * @brief 回填 Modbus TCP MBAP 中的 Length 字段。
 *
 * Length 位于 buf[4..5]，因此使用 set_2() 定点修改而不改变 buf_idx。
 */
static void set_msg_header_size(nmbs_t* nmbs, uint16_t data_length) {
    if (nmbs->platform.transport == NMBS_TRANSPORT_TCP) {
        data_length += 2;
        // MBAP Length 字段固定在偏移 4~5，因此使用 set_2() 回填。
        set_2(nmbs, data_length, 4);
    }
}
#endif
#endif


/**
 * @brief 发送当前已经构造完成的消息。
 *
 * RTU 在发送前自动计算并追加 CRC；
 * TCP 不追加 CRC。最终统一调用 send()。
 */
static nmbs_error send_msg(nmbs_t* nmbs) {
    NMBS_DEBUG_PRINT("\n");

    if (nmbs->platform.transport == NMBS_TRANSPORT_RTU) {
        const uint16_t crc = nmbs->platform.crc_calc(nmbs->msg.buf, nmbs->msg.buf_idx, nmbs->platform.arg);
        put_2(nmbs, crc);
    }

    const nmbs_error err = send(nmbs, nmbs->msg.buf_idx);

    return err;
}


#ifndef NMBS_SERVER_DISABLED
/**
 * @brief Server 侧接收请求头并判断该 RTU 请求是否属于本机。
 *
 * Unit ID=0 标记为广播；
 * Unit ID 与本机地址不匹配则标记 ignored，仍会完成报文消费但不执行业务。
 */
static nmbs_error recv_req_header(nmbs_t* nmbs, bool* first_byte_received) {
    const nmbs_error err = recv_msg_header(nmbs, first_byte_received);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (nmbs->platform.transport == NMBS_TRANSPORT_RTU) {
        // Check if the request is for us
        if (nmbs->msg.unit_id == NMBS_BROADCAST_ADDRESS)
            nmbs->msg.broadcast = true;
        else if (nmbs->msg.unit_id != nmbs->address_rtu)
            nmbs->msg.ignored = true;
        else
            nmbs->msg.ignored = false;
    }

    return NMBS_ERROR_NONE;
}


/**
 * @brief Server 侧构造正常响应头。
 *
 * 实际封装由 put_msg_header() 完成，本函数主要表达“这是响应”的语义。
 */
static void put_res_header(nmbs_t* nmbs, uint16_t data_length) {
    put_msg_header(nmbs, data_length);
    NMBS_DEBUG_PRINT("%d NMBS res -> address_rtu %d\tfc %d\t", nmbs->address_rtu, nmbs->address_rtu, nmbs->msg.fc);
}


/**
 * @brief Server 发送 Modbus Exception Response。
 *
 * 异常响应的功能码 = 原功能码 + 0x80，随后携带 1 字节异常码。
 * RTU 广播请求不允许返回响应，因此广播时直接成功返回。
 */
static nmbs_error send_exception_msg(nmbs_t* nmbs, uint8_t exception) {
    if (nmbs->msg.broadcast) {
        return NMBS_ERROR_NONE;
    }

    nmbs->msg.fc += 0x80;
    put_msg_header(nmbs, 1);
    put_1(nmbs, exception);

    NMBS_DEBUG_PRINT("%d NMBS res -> address_rtu %d\texception %d", nmbs->address_rtu, nmbs->address_rtu, exception);

    return send_msg(nmbs);
}
#endif


/**
 * @brief Client 侧接收并验证响应头。
 *
 * 会保存请求的 TID/Unit ID/FC，然后接收响应并检查：
 *   - TCP Transaction ID 是否匹配；
 *   - RTU Unit ID 是否匹配；
 *   - Function Code 是否匹配；
 *   - FC+0x80 时解析 Modbus Exception。
 */
static nmbs_error recv_res_header(nmbs_t* nmbs) {
    // 保存请求的 Transaction ID / Unit ID / Function Code，用于后续验证响应
    const uint16_t req_transaction_id = nmbs->msg.transaction_id;
    const uint8_t req_unit_id = nmbs->msg.unit_id;
    const uint8_t req_fc = nmbs->msg.fc;

    bool first_byte_received = false;
    nmbs_error err;

    /* A delayed response from an earlier TCP transaction is a complete but
     * stale ADU, not evidence that the socket is disconnected. The TCP header
     * reader has already consumed the complete ADU when its MBAP length is
     * valid, so discard it and keep waiting for this request's transaction ID.
     * The platform callback still enforces the transaction's total deadline. */
    do {
        err = recv_msg_header(nmbs, &first_byte_received);
        if (err != NMBS_ERROR_NONE)
            return err;
    } while ((nmbs->platform.transport == NMBS_TRANSPORT_TCP) &&
             (nmbs->msg.transaction_id != req_transaction_id));

    if (nmbs->platform.transport == NMBS_TRANSPORT_RTU && nmbs->msg.unit_id != req_unit_id)
        return NMBS_ERROR_INVALID_UNIT_ID;

    if (nmbs->msg.fc != req_fc) {
        if (nmbs->msg.fc - 0x80 == req_fc) {
            err = recv(nmbs, 1);
            if (err != NMBS_ERROR_NONE)
                return err;

            const uint8_t exception = get_1(nmbs);
            err = recv_msg_footer(nmbs);
            if (err != NMBS_ERROR_NONE)
                return err;

            if (exception < 1 || exception > 4)
                return NMBS_ERROR_INVALID_RESPONSE;

            NMBS_DEBUG_PRINT("%d NMBS res <- address_rtu %d\texception %d\n", nmbs->address_rtu, nmbs->msg.unit_id,
                             exception);
            // 正数 1~4 直接作为 nmbs_error 返回，调用者可区分协议异常与通信错误。
            return (nmbs_error) exception;
        }
        
        return NMBS_ERROR_INVALID_RESPONSE;
        // 收到的功能码既不是请求的原功能码，也不是异常响应的功能码，视为无效响应。
    }

    NMBS_DEBUG_PRINT("%d NMBS res <- address_rtu %d\tfc %d\t", nmbs->address_rtu, nmbs->msg.unit_id, nmbs->msg.fc);

    return NMBS_ERROR_NONE;
}


#ifndef NMBS_CLIENT_DISABLED
/**
 * @brief Client 侧构造请求头。
 *
 * 实际格式由 put_msg_header() 根据 RTU/TCP 选择。
 */
static void put_req_header(nmbs_t* nmbs, uint16_t data_length) {
    put_msg_header(nmbs, data_length);
#ifdef NMBS_DEBUG
    printf("%d ", nmbs->address_rtu);
    printf("NMBS req -> ");
    if (nmbs->platform.transport == NMBS_TRANSPORT_RTU) {
        if (nmbs->msg.broadcast)
            printf("broadcast\t");
        else
            printf("address_rtu %d\t", nmbs->dest_address_rtu);
    }

    printf("fc %d\t", nmbs->msg.fc);
#endif
}
#endif


#if !defined(NMBS_CLIENT_DISABLED) ||                                                                                  \
        (!defined(NMBS_SERVER_DISABLED) &&                                                                             \
         (!defined(NMBS_SERVER_READ_COILS_DISABLED) || !defined(NMBS_SERVER_READ_DISCRETE_INPUTS_DISABLED)))

/* ========================================================================== */
/* Client 响应解析 */
/* ========================================================================== */

/**
 * @brief 解析 FC01/FC02 的离散量读取响应。
 *
 * 验证响应头，读取 Byte Count，再把打包的 bit 字节复制到 bitfield，
 * 最后校验 RTU CRC（TCP 则无需 CRC）。
 */
static nmbs_error recv_read_discrete_res(nmbs_t* nmbs, nmbs_bitfield values) {
    nmbs_error err = recv_res_header(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    err = recv(nmbs, 1);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint8_t coils_bytes = get_1(nmbs);
    NMBS_DEBUG_PRINT("b %d\t", coils_bytes);

    if (coils_bytes > NMBS_BITFIELD_BYTES_MAX) {
        return NMBS_ERROR_INVALID_RESPONSE;
    }

    err = recv(nmbs, coils_bytes);
    if (err != NMBS_ERROR_NONE)
        return err;

    NMBS_DEBUG_PRINT("coils ");
    for (uint_fast8_t i = 0; i < coils_bytes; i++) {
        const uint8_t coil = get_1(nmbs);
        if (values)
            values[i] = coil;
        NMBS_DEBUG_PRINT("%d ", coil);
    }

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    return NMBS_ERROR_NONE;
}
#endif


#if !defined(NMBS_CLIENT_DISABLED) ||                                                                                  \
        (!defined(NMBS_SERVER_DISABLED) && (!defined(NMBS_SERVER_READ_HOLDING_REGISTERS_DISABLED) ||                   \
                                            !defined(NMBS_SERVER_READ_INPUT_REGISTERS_DISABLED)))
/**
 * @brief 解析 FC03/FC04 等“读取寄存器”响应。
 *
 * Byte Count 必须等于 quantity * 2；随后使用 get_2() 顺序解析每个寄存器。
 */
static nmbs_error recv_read_registers_res(nmbs_t* nmbs, uint16_t quantity, uint16_t* registers) {
    nmbs_error err = recv_res_header(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    err = recv(nmbs, 1);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint8_t registers_bytes = get_1(nmbs);
    NMBS_DEBUG_PRINT("b %d\t", registers_bytes);

    // 每个寄存器 2 字节；响应 Byte Count 必须与请求数量严格一致。
    if (registers_bytes > 250 || registers_bytes != quantity * 2)
        return NMBS_ERROR_INVALID_RESPONSE;

    err = recv(nmbs, registers_bytes);
    if (err != NMBS_ERROR_NONE)
        return err;

    NMBS_DEBUG_PRINT("regs ");
    for (uint_fast8_t i = 0; i < registers_bytes / 2; i++) {
        const uint16_t reg = get_2(nmbs);
        if (registers)
            registers[i] = reg;
        NMBS_DEBUG_PRINT("%d ", reg);
    }

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    return NMBS_ERROR_NONE;
}
#endif


/**
 * @brief 解析 FC05 写单线圈响应。
 *
 * 正常响应会回显请求地址和值；两者必须与请求完全一致。
 */
nmbs_error recv_write_single_coil_res(nmbs_t* nmbs, uint16_t address, uint16_t value_req) {
    nmbs_error err = recv_res_header(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    err = recv(nmbs, 4);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint16_t address_res = get_2(nmbs);
    const uint16_t value_res = get_2(nmbs);

    NMBS_DEBUG_PRINT("a %d\tvalue %d", address, value_res);

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (address_res != address)
        return NMBS_ERROR_INVALID_RESPONSE;

    if (value_res != value_req)
        return NMBS_ERROR_INVALID_RESPONSE;

    return NMBS_ERROR_NONE;
}


/**
 * @brief 解析 FC06 写单寄存器响应，并校验地址/值回显。
 */
nmbs_error recv_write_single_register_res(nmbs_t* nmbs, uint16_t address, uint16_t value_req) {
    nmbs_error err = recv_res_header(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    err = recv(nmbs, 4);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint16_t address_res = get_2(nmbs);
    const uint16_t value_res = get_2(nmbs);
    NMBS_DEBUG_PRINT("a %d\tvalue %d ", address, value_res);

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (address_res != address)
        return NMBS_ERROR_INVALID_RESPONSE;

    if (value_res != value_req)
        return NMBS_ERROR_INVALID_RESPONSE;

    return NMBS_ERROR_NONE;
}


/**
 * @brief 解析 FC15 写多个线圈响应，并校验起始地址/数量回显。
 */
nmbs_error recv_write_multiple_coils_res(nmbs_t* nmbs, uint16_t address, uint16_t quantity) {
    nmbs_error err = recv_res_header(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    err = recv(nmbs, 4);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint16_t address_res = get_2(nmbs);
    const uint16_t quantity_res = get_2(nmbs);
    NMBS_DEBUG_PRINT("a %d\tq %d", address_res, quantity_res);

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (address_res != address)
        return NMBS_ERROR_INVALID_RESPONSE;

    if (quantity_res != quantity)
        return NMBS_ERROR_INVALID_RESPONSE;

    return NMBS_ERROR_NONE;
}


/**
 * @brief 解析 FC16 写多个寄存器响应，并校验起始地址/数量回显。
 */
nmbs_error recv_write_multiple_registers_res(nmbs_t* nmbs, uint16_t address, uint16_t quantity) {
    nmbs_error err = recv_res_header(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    err = recv(nmbs, 4);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint16_t address_res = get_2(nmbs);
    const uint16_t quantity_res = get_2(nmbs);
    NMBS_DEBUG_PRINT("a %d\tq %d", address_res, quantity_res);

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (address_res != address)
        return NMBS_ERROR_INVALID_RESPONSE;

    if (quantity_res != quantity)
        return NMBS_ERROR_INVALID_RESPONSE;

    return NMBS_ERROR_NONE;
}


/**
 * @brief 解析 FC20 Read File Record 响应。
 *
 * 验证 Reference Type、记录数量，并将记录数据转换成本机寄存器字节序。
 */
nmbs_error recv_read_file_record_res(nmbs_t* nmbs, uint16_t* registers, uint16_t count) {
    nmbs_error err = recv_res_header(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    err = recv(nmbs, 1);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint8_t response_size = get_1(nmbs);
    if (response_size > 250) {
        return NMBS_ERROR_INVALID_RESPONSE;
    }

    err = recv(nmbs, response_size);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint8_t subreq_data_size = get_1(nmbs) - 1;
    const uint8_t subreq_reference_type = get_1(nmbs);
    uint16_t* subreq_record_data = (uint16_t*) get_n(nmbs, subreq_data_size);

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (registers) {
        if (subreq_reference_type != 6)
            return NMBS_ERROR_INVALID_RESPONSE;

        if (count != (subreq_data_size / 2))
            return NMBS_ERROR_INVALID_RESPONSE;

        swap_regs(subreq_record_data, subreq_data_size / 2);
        memcpy(registers, subreq_record_data, subreq_data_size);
    }

    return NMBS_ERROR_NONE;
}


/**
 * @brief 解析 FC21 Write File Record 响应。
 *
 * FC21 正常响应回显请求内容，因此需要比较文件号、记录号、长度和数据。
 */
nmbs_error recv_write_file_record_res(nmbs_t* nmbs, uint16_t file_number, uint16_t record_number,
                                      const uint16_t* registers, uint16_t count) {
    nmbs_error err = recv_res_header(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    err = recv(nmbs, 1);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint8_t response_size = get_1(nmbs);
    if (response_size > 251)
        return NMBS_ERROR_INVALID_RESPONSE;

    err = recv(nmbs, response_size);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint8_t subreq_reference_type = get_1(nmbs);
    const uint16_t subreq_file_number = get_2(nmbs);
    const uint16_t subreq_record_number = get_2(nmbs);
    const uint16_t subreq_record_length = get_2(nmbs);
    NMBS_DEBUG_PRINT("a %d\tr %d\tl %d\t fwrite ", subreq_file_number, subreq_record_number, subreq_record_length);

    uint16_t subreq_data_size = subreq_record_length * 2;
    uint16_t* subreq_record_data = (uint16_t*) get_n(nmbs, subreq_data_size);

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (registers) {
        if (subreq_reference_type != 6)
            return NMBS_ERROR_INVALID_RESPONSE;

        if (subreq_file_number != file_number)
            return NMBS_ERROR_INVALID_RESPONSE;

        if (subreq_record_number != record_number)
            return NMBS_ERROR_INVALID_RESPONSE;

        if (subreq_record_length != count)
            return NMBS_ERROR_INVALID_RESPONSE;

        swap_regs(subreq_record_data, subreq_record_length);
        if (memcmp(registers, subreq_record_data, subreq_data_size) != 0)
            return NMBS_ERROR_INVALID_RESPONSE;
    }

    return NMBS_ERROR_NONE;
}

/**
 * @brief 解析 FC43/MEI 0x0E Read Device Identification 响应。
 *
 * 处理 conformity level、More Follows、Next Object ID 和对象列表，
 * 并把对象字符串拷贝到调用者提供的缓冲区。
 */
nmbs_error recv_read_device_identification_res(nmbs_t* nmbs, uint8_t buffers_count, char** buffers_out,
                                               uint8_t buffers_length, const uint8_t* order, uint8_t* ids_out,
                                               uint8_t* next_object_id_out, uint8_t* objects_count_out) {
    nmbs_error err = recv_res_header(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    err = recv(nmbs, 6);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint8_t mei_type = get_1(nmbs);
    if (mei_type != 0x0E)
        return NMBS_ERROR_INVALID_RESPONSE;

    const uint8_t read_device_id_code = get_1(nmbs);
    if (read_device_id_code < 1 || read_device_id_code > 4)
        return NMBS_ERROR_INVALID_RESPONSE;

    const uint8_t conformity_level = get_1(nmbs);
    if (conformity_level < 1 || (conformity_level > 3 && conformity_level < 0x81) || conformity_level > 0x83)
        return NMBS_ERROR_INVALID_RESPONSE;

    const uint8_t more_follows = get_1(nmbs);
    if (more_follows != 0 && more_follows != 0xFF)
        return NMBS_ERROR_INVALID_RESPONSE;

    uint8_t next_object_id = get_1(nmbs);

    const uint8_t objects_count = get_1(nmbs);
    if (objects_count_out)
        *objects_count_out = objects_count;

    if (buffers_count == 0) {
        buffers_out = NULL;
    }
    else if (objects_count > buffers_count)
        return NMBS_ERROR_INVALID_ARGUMENT;

    if (more_follows == 0)
        next_object_id = 0x7F;    // This value is reserved in the spec, we use it to signal the stream is finished

    if (next_object_id_out)
        *next_object_id_out = next_object_id;

    uint8_t res_size_left = 253 - 7;
    for (uint_fast8_t i = 0; i < objects_count; i++) {
        err = recv(nmbs, 2);
        if (err != NMBS_ERROR_NONE)
            return err;

        const uint8_t object_id = get_1(nmbs);
        const uint8_t object_length = get_1(nmbs);
        res_size_left -= 2;

        if (object_length > res_size_left)
            return NMBS_ERROR_INVALID_RESPONSE;

        err = recv(nmbs, object_length);
        if (err != NMBS_ERROR_NONE)
            return err;

        const char* str = (const char*) get_n(nmbs, object_length);

        if (ids_out)
            ids_out[i] = object_id;

        uint8_t buf_index = i;
        if (order)
            buf_index = order[object_id];
        if (buffers_out) {
#ifndef _WIN32
            strncpy(buffers_out[buf_index], str, buffers_length);
#else
            strncpy_s(buffers_out[buf_index], buffers_length, str, object_length);
#endif
            buffers_out[buf_index][object_length] = 0;
        }
    }

    return recv_msg_footer(nmbs);
}


#ifndef NMBS_SERVER_DISABLED
#if !defined(NMBS_SERVER_READ_COILS_DISABLED) || !defined(NMBS_SERVER_READ_DISCRETE_INPUTS_DISABLED)

/* ========================================================================== */
/* Server 功能码处理 */
/* ========================================================================== */

/**
 * @brief Server 端 FC01/FC02 的共享处理器。
 *
 * 解析 address/quantity -> 校验协议范围 -> 调用业务 callback ->
 * 按 bitfield 打包响应。被忽略的 RTU 地址只消费请求而不执行业务。
 */
static nmbs_error handle_read_discrete(nmbs_t* nmbs,
                                       nmbs_error (*callback)(uint16_t, uint16_t, nmbs_bitfield, uint8_t, void*)) {
    nmbs_error err = recv(nmbs, 4);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint16_t address = get_2(nmbs);
    const uint16_t quantity = get_2(nmbs);

    NMBS_DEBUG_PRINT("a %d\tq %d", address, quantity);

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (!nmbs->msg.ignored) {
        if (quantity < 1 || quantity > 2000)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

        if ((uint32_t) address + (uint32_t) quantity > ((uint32_t) 0xFFFF) + 1)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);

        if (callback) {
            nmbs_bitfield bitfield = {0};
            err = callback(address, quantity, bitfield, nmbs->msg.unit_id, nmbs->callbacks.arg);
            if (err != NMBS_ERROR_NONE) {
                // callback 可以直接返回 Modbus Exception；其它内部错误统一映射为设备故障。
        if (nmbs_error_is_exception(err))
                    return send_exception_msg(nmbs, err);

                return send_exception_msg(nmbs, NMBS_EXCEPTION_SERVER_DEVICE_FAILURE);
            }

            if (!nmbs->msg.broadcast) {
                const uint8_t discrete_bytes = (quantity + 7) / 8;
                put_res_header(nmbs, 1 + discrete_bytes);

                put_1(nmbs, discrete_bytes);

                NMBS_DEBUG_PRINT("b %d\t", discrete_bytes);

                NMBS_DEBUG_PRINT("coils ");
                for (uint_fast8_t i = 0; i < discrete_bytes; i++) {
                    put_1(nmbs, bitfield[i]);
                    NMBS_DEBUG_PRINT("%d ", bitfield[i]);
                }

                err = send_msg(nmbs);
                if (err != NMBS_ERROR_NONE)
                    return err;
            }
        }
        else {
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_FUNCTION);
        }
    }
    else {
        return recv_read_discrete_res(nmbs, NULL);
    }

    return NMBS_ERROR_NONE;
}
#endif


#if !defined(NMBS_SERVER_READ_HOLDING_REGISTERS_DISABLED) || !defined(NMBS_SERVER_READ_INPUT_REGISTERS_DISABLED)
/**
 * @brief Server 端 FC03/FC04 的共享处理器。
 *
 * 解析请求、检查地址和数量、调用寄存器读取 callback，
 * 然后按 Modbus 大端顺序构造寄存器响应。
 */
static nmbs_error handle_read_registers(nmbs_t* nmbs,
                                        nmbs_error (*callback)(uint16_t, uint16_t, uint16_t*, uint8_t, void*)) {
    nmbs_error err = recv(nmbs, 4); // 回收4个字节数据
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint16_t address = get_2(nmbs);
    const uint16_t quantity = get_2(nmbs);

    NMBS_DEBUG_PRINT("a %d\tq %d", address, quantity);

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (!nmbs->msg.ignored) {
        if (quantity < 1 || quantity > 125)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

        if ((uint32_t) address + (uint32_t) quantity > ((uint32_t) 0xFFFF) + 1)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);

        if (callback) {
            uint16_t regs[125] = {0};
            err = callback(address, quantity, regs, nmbs->msg.unit_id, nmbs->callbacks.arg);
            if (err != NMBS_ERROR_NONE) {
                // callback 可以直接返回 Modbus Exception；其它内部错误统一映射为设备故障。
        if (nmbs_error_is_exception(err))
                    return send_exception_msg(nmbs, err);

                return send_exception_msg(nmbs, NMBS_EXCEPTION_SERVER_DEVICE_FAILURE);
            }

            // TODO check all these read request broadcast use cases
            if (!nmbs->msg.broadcast) {
                const uint8_t regs_bytes = quantity * 2;
                put_res_header(nmbs, 1 + regs_bytes);

                put_1(nmbs, regs_bytes);

                NMBS_DEBUG_PRINT("b %d\t", regs_bytes);

                NMBS_DEBUG_PRINT("regs ");
                for (uint_fast16_t i = 0; i < quantity; i++) {
                    put_2(nmbs, regs[i]);
                    NMBS_DEBUG_PRINT("%d ", regs[i]);
                }

                err = send_msg(nmbs);
                if (err != NMBS_ERROR_NONE)
                    return err;
            }
        }
        else {
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_FUNCTION);
        }
    }
    else {
        return recv_read_registers_res(nmbs, quantity, NULL);
    }

    return NMBS_ERROR_NONE;
}
#endif


#ifndef NMBS_SERVER_READ_COILS_DISABLED
/**
 * @brief Server FC01 Read Coils 入口，复用 handle_read_discrete()。
 */
static nmbs_error handle_read_coils(nmbs_t* nmbs) {
    return handle_read_discrete(nmbs, nmbs->callbacks.read_coils);
}
#endif


#ifndef NMBS_SERVER_READ_DISCRETE_INPUTS_DISABLED
/**
 * @brief Server FC02 Read Discrete Inputs 入口。
 */
static nmbs_error handle_read_discrete_inputs(nmbs_t* nmbs) {
    return handle_read_discrete(nmbs, nmbs->callbacks.read_discrete_inputs);
}
#endif


#ifndef NMBS_SERVER_READ_HOLDING_REGISTERS_DISABLED
/**
 * @brief Server FC03 Read Holding Registers 入口。
 */
static nmbs_error handle_read_holding_registers(nmbs_t* nmbs) {
    return handle_read_registers(nmbs, nmbs->callbacks.read_holding_registers);
}
#endif


#ifndef NMBS_SERVER_READ_INPUT_REGISTERS_DISABLED
/**
 * @brief Server FC04 Read Input Registers 入口。
 */
static nmbs_error handle_read_input_registers(nmbs_t* nmbs) {
    return handle_read_registers(nmbs, nmbs->callbacks.read_input_registers);
}
#endif


#ifndef NMBS_SERVER_WRITE_SINGLE_COIL_DISABLED
/**
 * @brief Server FC05 Write Single Coil 处理器。
 *
 * Coil 值只接受 0x0000 或 0xFF00；调用业务 callback 后，
 * 非广播请求按规范回显地址和值。
 */
static nmbs_error handle_write_single_coil(nmbs_t* nmbs) {
    nmbs_error err = recv(nmbs, 4);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint16_t address = get_2(nmbs);
    const uint16_t value = get_2(nmbs);

    NMBS_DEBUG_PRINT("a %d\tvalue %d", address, value);

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (!nmbs->msg.ignored) {
        if (nmbs->callbacks.write_single_coil) {
            if (value != 0 && value != 0xFF00)
                return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

            err = nmbs->callbacks.write_single_coil(address, value == 0 ? false : true, nmbs->msg.unit_id,
                                                    nmbs->callbacks.arg);
            if (err != NMBS_ERROR_NONE) {
                // callback 可以直接返回 Modbus Exception；其它内部错误统一映射为设备故障。
        if (nmbs_error_is_exception(err))
                    return send_exception_msg(nmbs, err);

                return send_exception_msg(nmbs, NMBS_EXCEPTION_SERVER_DEVICE_FAILURE);
            }

            if (!nmbs->msg.broadcast) {
                put_res_header(nmbs, 4);

                put_2(nmbs, address);
                put_2(nmbs, value);
                NMBS_DEBUG_PRINT("a %d\tvalue %d", address, value);

                err = send_msg(nmbs);
                if (err != NMBS_ERROR_NONE)
                    return err;
            }
        }
        else {
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_FUNCTION);
        }
    }
    else {
        return recv_write_single_coil_res(nmbs, address, value);
    }

    return NMBS_ERROR_NONE;
}
#endif


#ifndef NMBS_SERVER_WRITE_SINGLE_REGISTER_DISABLED
/**
 * @brief Server FC06 Write Single Register 处理器。
 *
 * 调用业务 callback 写入寄存器，非广播时回显地址和值。
 */
static nmbs_error handle_write_single_register(nmbs_t* nmbs) {
    nmbs_error err = recv(nmbs, 4);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint16_t address = get_2(nmbs);
    const uint16_t value = get_2(nmbs);

    NMBS_DEBUG_PRINT("a %d\tvalue %d", address, value);

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (!nmbs->msg.ignored) {
        if (nmbs->callbacks.write_single_register) {
            err = nmbs->callbacks.write_single_register(address, value, nmbs->msg.unit_id, nmbs->callbacks.arg);
            if (err != NMBS_ERROR_NONE) {
                // callback 可以直接返回 Modbus Exception；其它内部错误统一映射为设备故障。
        if (nmbs_error_is_exception(err))
                    return send_exception_msg(nmbs, err);

                return send_exception_msg(nmbs, NMBS_EXCEPTION_SERVER_DEVICE_FAILURE);
            }

            if (!nmbs->msg.broadcast) {
                put_res_header(nmbs, 4);

                put_2(nmbs, address);
                put_2(nmbs, value);
                NMBS_DEBUG_PRINT("a %d\tvalue %d", address, value);

                err = send_msg(nmbs);
                if (err != NMBS_ERROR_NONE)
                    return err;
            }
        }
        else {
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_FUNCTION);
        }
    }
    else {
        return recv_write_single_register_res(nmbs, address, value);
    }

    return NMBS_ERROR_NONE;
}
#endif


#ifndef NMBS_SERVER_WRITE_MULTIPLE_COILS_DISABLED
/**
 * @brief Server FC15 Write Multiple Coils 处理器。
 *
 * 校验 quantity、Byte Count 和地址范围，将位数据交给业务 callback，
 * 非广播时返回起始地址与写入数量。
 */
static nmbs_error handle_write_multiple_coils(nmbs_t* nmbs) {
    nmbs_error err = recv(nmbs, 5);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint16_t address = get_2(nmbs);
    const uint16_t quantity = get_2(nmbs);
    const uint8_t coils_bytes = get_1(nmbs);

    NMBS_DEBUG_PRINT("a %d\tq %d\tb %d\tcoils ", address, quantity, coils_bytes);

    if (coils_bytes > 246)
        return NMBS_ERROR_INVALID_REQUEST;

    err = recv(nmbs, coils_bytes);
    if (err != NMBS_ERROR_NONE)
        return err;

    nmbs_bitfield coils = {0};
    for (uint_fast8_t i = 0; i < coils_bytes; i++) {
        coils[i] = get_1(nmbs);
        NMBS_DEBUG_PRINT("%d ", coils[i]);
    }

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (!nmbs->msg.ignored) {
        if (quantity < 1 || quantity > 0x07B0)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

        if ((uint32_t) address + (uint32_t) quantity > ((uint32_t) 0xFFFF) + 1)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);

        if (coils_bytes == 0)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

        if ((quantity + 7) / 8 != coils_bytes)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

        if (nmbs->callbacks.write_multiple_coils) {
            err = nmbs->callbacks.write_multiple_coils(address, quantity, coils, nmbs->msg.unit_id,
                                                       nmbs->callbacks.arg);
            if (err != NMBS_ERROR_NONE) {
                // callback 可以直接返回 Modbus Exception；其它内部错误统一映射为设备故障。
        if (nmbs_error_is_exception(err))
                    return send_exception_msg(nmbs, err);

                return send_exception_msg(nmbs, NMBS_EXCEPTION_SERVER_DEVICE_FAILURE);
            }

            if (!nmbs->msg.broadcast) {
                put_res_header(nmbs, 4);

                put_2(nmbs, address);
                put_2(nmbs, quantity);
                NMBS_DEBUG_PRINT("a %d\tq %d", address, quantity);

                err = send_msg(nmbs);
                if (err != NMBS_ERROR_NONE)
                    return err;
            }
        }
        else {
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_FUNCTION);
        }
    }
    else {
        return recv_write_multiple_coils_res(nmbs, address, quantity);
    }

    return NMBS_ERROR_NONE;
}
#endif


#ifndef NMBS_SERVER_WRITE_MULTIPLE_REGISTERS_DISABLED
/**
 * @brief Server FC16 Write Multiple Registers 处理器。
 *
 * 解析并校验寄存器数据，调用业务 callback；
 * 非广播时回显起始地址与写入数量。
 */
static nmbs_error handle_write_multiple_registers(nmbs_t* nmbs) {
    nmbs_error err = recv(nmbs, 5);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint16_t address = get_2(nmbs);
    const uint16_t quantity = get_2(nmbs);
    const uint8_t registers_bytes = get_1(nmbs);

    NMBS_DEBUG_PRINT("a %d\tq %d\tb %d\tregs ", address, quantity, registers_bytes);

    err = recv(nmbs, registers_bytes);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (registers_bytes > 246)
        return NMBS_ERROR_INVALID_REQUEST;

    uint16_t registers[0x007B];
    for (uint_fast8_t i = 0; i < registers_bytes / 2; i++) {
        registers[i] = get_2(nmbs);
        NMBS_DEBUG_PRINT("%d ", registers[i]);
    }

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (!nmbs->msg.ignored) {
        if (quantity < 1 || quantity > 0x007B)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

        if ((uint32_t) address + (uint32_t) quantity > ((uint32_t) 0xFFFF) + 1)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);

        if (registers_bytes == 0)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

        if (registers_bytes != quantity * 2)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

        if (nmbs->callbacks.write_multiple_registers) {
            err = nmbs->callbacks.write_multiple_registers(address, quantity, registers, nmbs->msg.unit_id,
                                                           nmbs->callbacks.arg);
            if (err != NMBS_ERROR_NONE) {
                // callback 可以直接返回 Modbus Exception；其它内部错误统一映射为设备故障。
        if (nmbs_error_is_exception(err))
                    return send_exception_msg(nmbs, err);

                return send_exception_msg(nmbs, NMBS_EXCEPTION_SERVER_DEVICE_FAILURE);
            }

            if (!nmbs->msg.broadcast) {
                put_res_header(nmbs, 4);

                put_2(nmbs, address);
                put_2(nmbs, quantity);
                NMBS_DEBUG_PRINT("a %d\tq %d", address, quantity);

                err = send_msg(nmbs);
                if (err != NMBS_ERROR_NONE)
                    return err;
            }
        }
        else {
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_FUNCTION);
        }
    }
    else {
        return recv_write_multiple_registers_res(nmbs, address, quantity);
    }

    return NMBS_ERROR_NONE;
}
#endif

#ifndef NMBS_SERVER_READ_FILE_RECORD_DISABLED
/**
 * @brief Server FC20 Read File Record 处理器。
 *
 * 支持一个请求中的多个子请求，逐个校验文件/记录参数，
 * 调用 read_file_record callback，并组装文件记录响应。
 */
static nmbs_error handle_read_file_record(nmbs_t* nmbs) {
    nmbs_error err = recv(nmbs, 1);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint8_t request_size = get_1(nmbs);
    if (request_size > 245)
        return NMBS_ERROR_INVALID_REQUEST;

    err = recv(nmbs, request_size);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint8_t subreq_header_size = 7;
    const uint8_t subreq_count = request_size / subreq_header_size;

    struct {
        uint8_t reference_type;
        uint16_t file_number;
        uint16_t record_number;
        uint16_t record_length;
    }
#if defined(__STDC_NO_VLA__) || defined(_MSC_VER) || defined(__CC_ARM)
    subreq[35];    // 245 / subreq_header_size
#else
    subreq[subreq_count];
#endif

    uint16_t response_data_size = 0;

    for (uint8_t i = 0; i < subreq_count; i++) {
        subreq[i].reference_type = get_1(nmbs);
        subreq[i].file_number = get_2(nmbs);
        subreq[i].record_number = get_2(nmbs);
        subreq[i].record_length = get_2(nmbs);

        response_data_size += (uint16_t) (2U + (subreq[i].record_length * 2U));
    }

    discard_n(nmbs, request_size % subreq_header_size);

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (!nmbs->msg.ignored) {
        if (request_size % subreq_header_size)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

        if (request_size < 0x07 || request_size > 0xF5)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

        for (uint8_t i = 0; i < subreq_count; i++) {
            if (subreq[i].reference_type != 0x06)
                return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);

            if (subreq[i].file_number == 0x0000)
                return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);

            if (subreq[i].record_number > 0x270F)
                return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);

            if (subreq[i].record_length == 0 || subreq[i].record_length > 124)
                return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);

            NMBS_DEBUG_PRINT("a %d\tr %d\tl %d\t fread ", subreq[i].file_number, subreq[i].record_number,
                             subreq[i].record_length);
        }

        if (response_data_size > 251)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

        put_res_header(nmbs, 1 + response_data_size);
        put_1(nmbs, (uint8_t) response_data_size);

        if (nmbs->callbacks.read_file_record) {
            for (uint8_t i = 0; i < subreq_count; i++) {
                uint16_t subreq_data_size = subreq[i].record_length * 2;
                put_1(nmbs, subreq_data_size + 1);
                put_1(nmbs, 0x06);    // add Reference Type const
                uint16_t* subreq_data = (uint16_t*) get_n(nmbs, subreq_data_size);

                err = nmbs->callbacks.read_file_record(subreq[i].file_number, subreq[i].record_number, subreq_data,
                                                       subreq[i].record_length, nmbs->msg.unit_id, nmbs->callbacks.arg);
                if (err != NMBS_ERROR_NONE) {
                    // callback 可以直接返回 Modbus Exception；其它内部错误统一映射为设备故障。
        if (nmbs_error_is_exception(err))
                        return send_exception_msg(nmbs, err);

                    return send_exception_msg(nmbs, NMBS_EXCEPTION_SERVER_DEVICE_FAILURE);
                }

                swap_regs(subreq_data, subreq[i].record_length);
            }
        }
        else {
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_FUNCTION);
        }

        if (!nmbs->msg.broadcast) {
            err = send_msg(nmbs);
            if (err != NMBS_ERROR_NONE)
                return err;
        }
    }
    else {
        return recv_read_file_record_res(nmbs, NULL, 0);
    }

    return NMBS_ERROR_NONE;
}
#endif

#ifndef NMBS_SERVER_WRITE_FILE_RECORD_DISABLED
/**
 * @brief Server FC21 Write File Record 处理器。
 *
 * 先完整校验各子请求，再调用 write_file_record callback。
 * 正常响应要求回显原请求，因此代码会保存/恢复 buf_idx。
 */
static nmbs_error handle_write_file_record(nmbs_t* nmbs) {
    nmbs_error err = recv(nmbs, 1);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint8_t request_size = get_1(nmbs);
    if (request_size > 251) {
        return NMBS_ERROR_INVALID_REQUEST;
    }

    err = recv(nmbs, request_size);
    if (err != NMBS_ERROR_NONE)
        return err;

    // We can save the msg.buf index and use it later for context recovery.
    const uint16_t msg_buf_idx = nmbs->msg.buf_idx;
    discard_n(nmbs, request_size);

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (!nmbs->msg.ignored) {
        const uint8_t subreq_header_size = 7;
        uint16_t size = request_size;
        nmbs->msg.buf_idx = msg_buf_idx;    // restore context

        if (request_size < 7)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

        do {
            const uint8_t subreq_reference_type = get_1(nmbs);
            const uint16_t subreq_file_number_c = get_2(nmbs);
            const uint16_t subreq_record_number_c = get_2(nmbs);
            const uint16_t subreq_record_length_c = get_2(nmbs);
            discard_n(nmbs, subreq_record_length_c * 2);

            if (subreq_reference_type != 0x06)
                return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);

            if (subreq_file_number_c == 0x0000)
                return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);

            if (subreq_record_number_c > 0x270F)
                return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);

            if (subreq_record_length_c > 122)
                return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);

            NMBS_DEBUG_PRINT("a %d\tr %d\tl %d\t fwrite ", subreq_file_number_c, subreq_record_number_c,
                             subreq_record_length_c);
            size -= (subreq_header_size + (subreq_record_length_c * 2));
        } while (size >= subreq_header_size);

        if (size)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

        // checks completed

        size = request_size;
        nmbs->msg.buf_idx = msg_buf_idx;    // restore context

        do {
            discard_1(nmbs);
            const uint16_t subreq_file_number = get_2(nmbs);
            const uint16_t subreq_record_number = get_2(nmbs);
            const uint16_t subreq_record_length = get_2(nmbs);
            uint16_t* subreq_data = get_regs(nmbs, subreq_record_length);

            if (nmbs->callbacks.write_file_record) {
                err = nmbs->callbacks.write_file_record(subreq_file_number, subreq_record_number, subreq_data,
                                                        subreq_record_length, nmbs->msg.unit_id, nmbs->callbacks.arg);
                if (err != NMBS_ERROR_NONE) {
                    // callback 可以直接返回 Modbus Exception；其它内部错误统一映射为设备故障。
        if (nmbs_error_is_exception(err))
                        return send_exception_msg(nmbs, err);

                    return send_exception_msg(nmbs, NMBS_EXCEPTION_SERVER_DEVICE_FAILURE);
                }

                swap_regs(subreq_data, subreq_record_length);    // restore swapping
            }
            else {
                return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_FUNCTION);
            }

            size -= (subreq_header_size + (subreq_record_length * 2));
        } while (size >= subreq_header_size);

        if (!nmbs->msg.broadcast) {
            // The normal response to 'Write File' is an echo of the request.
            // We can restore the buffer index and response msg.
            nmbs->msg.buf_idx = msg_buf_idx;
            discard_n(nmbs, request_size);

            err = send_msg(nmbs);
            if (err != NMBS_ERROR_NONE)
                return err;
        }
    }
    else {
        return recv_write_file_record_res(nmbs, 0, 0, NULL, 0);
    }

    return NMBS_ERROR_NONE;
}
#endif

#ifndef NMBS_SERVER_READ_WRITE_REGISTERS_DISABLED
/**
 * @brief Server FC23 Read/Write Multiple Registers 处理器。
 *
 * 先处理写寄存器，再读取 holding registers 并返回读取结果。
 */
static nmbs_error handle_read_write_registers(nmbs_t* nmbs) {
    nmbs_error err = recv(nmbs, 9);
    if (err != NMBS_ERROR_NONE)
        return err;
    // 解析请求参数
    const uint16_t read_address = get_2(nmbs);
    const uint16_t read_quantity = get_2(nmbs);
    const uint16_t write_address = get_2(nmbs);
    const uint16_t write_quantity = get_2(nmbs);

    const uint8_t byte_count_write = get_1(nmbs);

    NMBS_DEBUG_PRINT("ra %d\trq %d\t wa %d\t wq %d\t b %d\tregs ", read_address, read_quantity, write_address,
                     write_quantity, byte_count_write);

    if (byte_count_write > 242)
        return NMBS_ERROR_INVALID_REQUEST;

    err = recv(nmbs, byte_count_write);
    if (err != NMBS_ERROR_NONE)
        return err;

#if defined(__STDC_NO_VLA__) || defined(_MSC_VER)
    uint16_t registers[0x007B];
#else
    uint16_t registers[byte_count_write / 2];
#endif
    for (uint_fast8_t i = 0; i < byte_count_write / 2; i++) {
        registers[i] = get_2(nmbs);
        NMBS_DEBUG_PRINT("%d ", registers[i]);
    }

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;
    // 确认这帧是发给本设备的
    if (!nmbs->msg.ignored) {
        if (read_quantity < 1 || read_quantity > 0x007D)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

        if (write_quantity < 1 || write_quantity > 0x007B)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

        if (byte_count_write != write_quantity * 2)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

        if ((uint32_t) read_address + (uint32_t) read_quantity > ((uint32_t) 0xFFFF) + 1)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);

        if ((uint32_t) write_address + (uint32_t) write_quantity > ((uint32_t) 0xFFFF) + 1)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);

        if (!nmbs->callbacks.write_multiple_registers || !nmbs->callbacks.read_holding_registers)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_FUNCTION);

        err = nmbs->callbacks.write_multiple_registers(write_address, write_quantity, registers, nmbs->msg.unit_id,
                                                       nmbs->callbacks.arg);
        if (err != NMBS_ERROR_NONE) {
            // callback 可以直接返回 Modbus Exception；其它内部错误统一映射为设备故障。
        if (nmbs_error_is_exception(err))
                return send_exception_msg(nmbs, err);

            return send_exception_msg(nmbs, NMBS_EXCEPTION_SERVER_DEVICE_FAILURE);
        }

        if (!nmbs->msg.broadcast) {
#if defined(__STDC_NO_VLA__) || defined(_MSC_VER)
            uint16_t regs[125];
#else
            uint16_t regs[read_quantity];
#endif
            err = nmbs->callbacks.read_holding_registers(read_address, read_quantity, regs, nmbs->msg.unit_id,
                                                         nmbs->callbacks.arg);
            if (err != NMBS_ERROR_NONE) {
                // callback 可以直接返回 Modbus Exception；其它内部错误统一映射为设备故障。
        if (nmbs_error_is_exception(err))
                    return send_exception_msg(nmbs, err);

                return send_exception_msg(nmbs, NMBS_EXCEPTION_SERVER_DEVICE_FAILURE);
            }

            const uint8_t regs_bytes = read_quantity * 2;
            put_res_header(nmbs, 1 + regs_bytes);

            put_1(nmbs, regs_bytes);

            NMBS_DEBUG_PRINT("b %d\t", regs_bytes);

            NMBS_DEBUG_PRINT("regs ");
            for (uint_fast16_t i = 0; i < read_quantity; i++) {
                put_2(nmbs, regs[i]);
                NMBS_DEBUG_PRINT("%d ", regs[i]);
            }

            err = send_msg(nmbs); // 发送响应
            if (err != NMBS_ERROR_NONE)
                return err;
        }
    }
    else {
        return recv_write_multiple_registers_res(nmbs, write_address, write_quantity);
    }

    return NMBS_ERROR_NONE;
}
#endif

#ifndef NMBS_SERVER_READ_DEVICE_IDENTIFICATION_DISABLED
/**
 * @brief Server FC43 / MEI 0x0E 设备标识处理器。
 *
 * 根据 Read Device ID Code 和对象映射返回 Basic/Regular/Extended/Specific 数据。
 * 因响应长度和对象数量需要最终才能确定，过程中会使用 set_1/set_2 回填字段。
 */
static nmbs_error handle_read_device_identification(nmbs_t* nmbs) {
    nmbs_error err = recv(nmbs, 3);
    if (err != NMBS_ERROR_NONE)
        return err;

    const uint8_t mei_type = get_1(nmbs);
    const uint8_t read_device_id_code = get_1(nmbs);
    const uint8_t object_id = get_1(nmbs);

    NMBS_DEBUG_PRINT("c %d\to %d", read_device_id_code, object_id);

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (!nmbs->msg.ignored) {
        if (!nmbs->callbacks.read_device_identification_map || !nmbs->callbacks.read_device_identification)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_FUNCTION);

        if (mei_type != 0x0E)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_FUNCTION);

        if (read_device_id_code < 1 || read_device_id_code > 4)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_VALUE);

        if (object_id > 6 && object_id < 0x80)
            return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);

        if (!nmbs->msg.broadcast) {
            char str[NMBS_DEVICE_IDENTIFICATION_STRING_LENGTH];

            nmbs_bitfield_256 map;
            nmbs_bitfield_reset(map);

            err = nmbs->callbacks.read_device_identification_map(map);
            if (err != NMBS_ERROR_NONE) {
                // callback 可以直接返回 Modbus Exception；其它内部错误统一映射为设备故障。
        if (nmbs_error_is_exception(err))
                    return send_exception_msg(nmbs, err);

                return send_exception_msg(nmbs, NMBS_EXCEPTION_SERVER_DEVICE_FAILURE);
            }

            put_res_header(nmbs, 0);    // Length will be set later
            put_1(nmbs, 0x0E);
            put_1(nmbs, read_device_id_code);
            put_1(nmbs, 0x83);

            if (read_device_id_code == 4) {
                if (!nmbs_bitfield_read(map, object_id))
                    return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);

                put_1(nmbs, 0);    // More follows
                put_1(nmbs, 0);    // Next Object Id
                put_1(nmbs, 1);    // Number of objects

                str[0] = 0;
                err = nmbs->callbacks.read_device_identification(object_id, str);
                if (err != NMBS_ERROR_NONE) {
                    // callback 可以直接返回 Modbus Exception；其它内部错误统一映射为设备故障。
        if (nmbs_error_is_exception(err))
                        return send_exception_msg(nmbs, err);

                    return send_exception_msg(nmbs, NMBS_EXCEPTION_SERVER_DEVICE_FAILURE);
                }

                const size_t str_len = strlen(str);

                put_1(nmbs, object_id);    // Object id
                put_1(nmbs, str_len);      // Object length
                put_n(nmbs, (uint8_t*) str, str_len);

                set_msg_header_size(nmbs, 6 + 2 + str_len);

                return send_msg(nmbs);
            }

            const uint8_t more_follows_idx = nmbs->msg.buf_idx;
            put_1(nmbs, 0);
            const uint8_t next_object_id_idx = nmbs->msg.buf_idx;
            put_1(nmbs, 0);
            const uint8_t number_of_objects_idx = nmbs->msg.buf_idx;
            put_1(nmbs, 0);

            int16_t res_size_left = 253 - 7;

            uint8_t last_id = 0;
            uint8_t msg_size = 6;
            uint8_t res_more_follows = 0;
            uint8_t res_next_object_id = 0;
            uint8_t res_number_of_objects = 0;

            switch (read_device_id_code) {
                case 1:
                    if (object_id > 0x02)
                        return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);
                    last_id = 0x02;
                    break;
                case 2:
                    if (object_id < 0x03 || object_id > 0x06)
                        return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);
                    last_id = 0x06;
                    break;
                case 3:
                    if (object_id < 0x80)
                        return send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS);
                    last_id = 0xFF;
                    break;
                default:
                    // Unreachable
                    break;
            }

            for (uint16_t id = object_id; id <= last_id; id++) {
                if (!nmbs_bitfield_read(map, id)) {
                    if (id < 0x03)
                        return send_exception_msg(nmbs, NMBS_EXCEPTION_SERVER_DEVICE_FAILURE);
                    continue;
                }

                str[0] = 0;
                err = nmbs->callbacks.read_device_identification((uint8_t) id, str);
                if (err != NMBS_ERROR_NONE) {
                    // callback 可以直接返回 Modbus Exception；其它内部错误统一映射为设备故障。
        if (nmbs_error_is_exception(err))
                        return send_exception_msg(nmbs, err);

                    return send_exception_msg(nmbs, NMBS_EXCEPTION_SERVER_DEVICE_FAILURE);
                }

                const int16_t str_len = (int16_t) strlen(str);

                res_size_left = (int16_t) (res_size_left - 2 - str_len);
                if (res_size_left < 0) {
                    res_more_follows = 0xFF;
                    res_next_object_id = id;
                    break;
                }

                put_1(nmbs, (uint8_t) id);    // Object id
                put_1(nmbs, str_len);         // Object length
                put_n(nmbs, (uint8_t*) str, str_len);

                msg_size += (2 + str_len);

                res_number_of_objects++;
            }

            set_1(nmbs, res_more_follows, more_follows_idx);
            set_1(nmbs, res_next_object_id, next_object_id_idx);
            set_1(nmbs, res_number_of_objects, number_of_objects_idx);

            set_msg_header_size(nmbs, msg_size);

            return send_msg(nmbs);
        }
    }
    else {
        return recv_read_device_identification_res(nmbs, 0, NULL, 0, NULL, NULL, NULL, NULL);
    }

    return NMBS_ERROR_NONE;
}
#endif



/* ========================================================================== */
/* Server 分发与轮询 */
/* ========================================================================== */

/**
 * @brief Server 功能码分发器。
 *
 * 根据当前 msg.fc 调用对应的 handle_xxx()；
 * 未实现/未启用的功能码返回 Illegal Function 异常。
 */
static nmbs_error handle_req_fc(nmbs_t* nmbs) {
    NMBS_DEBUG_PRINT("fc %d\t", nmbs->msg.fc);

    nmbs_error err = NMBS_ERROR_NONE;
    switch (nmbs->msg.fc) {
#ifndef NMBS_SERVER_READ_COILS_DISABLED
        case 1:
            err = handle_read_coils(nmbs);
            break;
#endif

#ifndef NMBS_SERVER_READ_DISCRETE_INPUTS_DISABLED
        case 2:
            err = handle_read_discrete_inputs(nmbs);
            break;
#endif

#ifndef NMBS_SERVER_READ_HOLDING_REGISTERS_DISABLED
        case 3:
            err = handle_read_holding_registers(nmbs);
            break;
#endif

#ifndef NMBS_SERVER_READ_INPUT_REGISTERS_DISABLED
        case 4:
            err = handle_read_input_registers(nmbs);
            break;
#endif

#ifndef NMBS_SERVER_WRITE_SINGLE_COIL_DISABLED
        case 5:
            err = handle_write_single_coil(nmbs);
            break;
#endif

#ifndef NMBS_SERVER_WRITE_SINGLE_REGISTER_DISABLED
        case 6:
            err = handle_write_single_register(nmbs);
            break;
#endif

#ifndef NMBS_SERVER_WRITE_MULTIPLE_COILS_DISABLED
        case 15:
            err = handle_write_multiple_coils(nmbs);
            break;
#endif

#ifndef NMBS_SERVER_WRITE_MULTIPLE_REGISTERS_DISABLED
        case 16:
            err = handle_write_multiple_registers(nmbs);
            break;
#endif

#ifndef NMBS_SERVER_READ_FILE_RECORD_DISABLED
        case 20:
            err = handle_read_file_record(nmbs);
            break;
#endif

#ifndef NMBS_SERVER_WRITE_FILE_RECORD_DISABLED
        case 21:
            err = handle_write_file_record(nmbs);
            break;
#endif

#ifndef NMBS_SERVER_READ_WRITE_REGISTERS_DISABLED
        case 23:
            err = handle_read_write_registers(nmbs);
            break;
#endif

#ifndef NMBS_SERVER_READ_DEVICE_IDENTIFICATION_DISABLED
        case 43:
            err = handle_read_device_identification(nmbs);
            break;
#endif
        default:
            // 新请求开始前清理旧数据，避免上一帧残留污染本次响应。
    nmbs->platform.flush(nmbs, nmbs->platform.arg);
            if (!nmbs->msg.ignored)
                err = send_exception_msg(nmbs, NMBS_EXCEPTION_ILLEGAL_FUNCTION);
    }

    return err;
}


/**
 * @brief 初始化 Server 数据模型回调表。
 *
 * 先清零，再写 initialized 魔数。具体 read/write callback 由应用层填写。
 */
void nmbs_callbacks_create(nmbs_callbacks* callbacks) {
    memset(callbacks, 0, sizeof(nmbs_callbacks));
    callbacks->initialized = 0xFFFFDEBE;
}


/**
 * @brief 创建 Server 实例。
 *
 * 在基础 nmbs_create() 之上保存 RTU 本机地址和 Server callbacks。
 */
nmbs_error nmbs_server_create(nmbs_t* nmbs, uint8_t address_rtu, const nmbs_platform_conf* platform_conf,
                              const nmbs_callbacks* callbacks) {
    if (platform_conf->transport == NMBS_TRANSPORT_RTU && address_rtu == 0)
        return NMBS_ERROR_INVALID_ARGUMENT;

    if (!callbacks || callbacks->initialized != 0xFFFFDEBE)
        return NMBS_ERROR_INVALID_ARGUMENT;

    nmbs_error ret = nmbs_create(nmbs, platform_conf);
    if (ret != NMBS_ERROR_NONE)
        return ret;

    nmbs->address_rtu = address_rtu;
    nmbs->callbacks = *callbacks;

    return NMBS_ERROR_NONE;
}


/**
 * @brief Server 主轮询入口：接收并处理一笔 Modbus 请求。
 *
 * 典型调用链：
 *   recv_req_header() -> handle_req_fc() -> callback -> send_msg()
 * 如果只收到首字节后出错，会尝试继续消费/清理相应报文状态。
 */
nmbs_error nmbs_server_poll(nmbs_t* nmbs) {
    msg_state_reset(nmbs);// 清理上一笔消息状态

    bool first_byte_received = false;
    nmbs_error err = recv_req_header(nmbs, &first_byte_received); // 等待新请求
    if (err != NMBS_ERROR_NONE) {
        if (!first_byte_received && err == NMBS_ERROR_TIMEOUT)
            return NMBS_ERROR_NONE;

        return err;
    }

#ifdef NMBS_DEBUG
    printf("%d ", nmbs->address_rtu);
    printf("NMBS req <- ");
    if (nmbs->platform.transport == NMBS_TRANSPORT_RTU) {
        if (nmbs->msg.broadcast)
            printf("broadcast\t");
        else
            printf("address_rtu %d\t", nmbs->msg.unit_id);
    }
#endif

    err = handle_req_fc(nmbs);
    if (err != NMBS_ERROR_NONE) {
        if (err != NMBS_ERROR_TIMEOUT)
            // 新请求开始前清理旧数据，避免上一帧残留污染本次响应。
            nmbs->platform.flush(nmbs, nmbs->platform.arg);

        return err;
    }

    return NMBS_ERROR_NONE;
}

/**
 * @brief 修改 Server 数据模型 callback 的用户上下文指针。
 */
void nmbs_set_callbacks_arg(nmbs_t* nmbs, void* arg) {
    nmbs->callbacks.arg = arg;
}
#endif


#ifndef NMBS_CLIENT_DISABLED

/* ========================================================================== */
/* Client 公共 API */
/* ========================================================================== */

/**
 * @brief 创建 Client 实例。
 *
 * Client 本身没有额外动态资源，主要复用 nmbs_create() 初始化协议实例。
 */
nmbs_error nmbs_client_create(nmbs_t* nmbs, const nmbs_platform_conf* platform_conf) {
    return nmbs_create(nmbs, platform_conf);
}


/**
 * @brief Client FC01/FC02 共享实现。
 *
 * 校验 quantity/address -> 开始请求状态 -> 组包 -> 发送 ->
 * 非广播情况下接收并解析离散量响应。
 */
static nmbs_error read_discrete(nmbs_t* nmbs, uint8_t fc, uint16_t address, uint16_t quantity, nmbs_bitfield values) {
    if (quantity < 1 || quantity > NMBS_BITFIELD_MAX)
        return NMBS_ERROR_INVALID_ARGUMENT;

    if ((uint32_t) address + (uint32_t) quantity > ((uint32_t) 0xFFFF) + 1)
        return NMBS_ERROR_INVALID_ARGUMENT;

    msg_state_req(nmbs, fc);
    put_req_header(nmbs, 4);

    put_2(nmbs, address);
    put_2(nmbs, quantity);

    NMBS_DEBUG_PRINT("a %d\tq %d", address, quantity);

    const nmbs_error err = send_msg(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    return recv_read_discrete_res(nmbs, values);
}


/**
 * @brief Client FC01 Read Coils 公共 API。
 */
nmbs_error nmbs_read_coils(nmbs_t* nmbs, uint16_t address, uint16_t quantity, nmbs_bitfield coils_out) {
    return read_discrete(nmbs, 1, address, quantity, coils_out);
}


/**
 * @brief Client FC02 Read Discrete Inputs 公共 API。
 */
nmbs_error nmbs_read_discrete_inputs(nmbs_t* nmbs, uint16_t address, uint16_t quantity, nmbs_bitfield inputs_out) {
    return read_discrete(nmbs, 2, address, quantity, inputs_out);
}

/**
 * @brief Client FC03/FC04 共享实现。
 *
 * 请求体为 Address + Quantity；响应由 recv_read_registers_res() 解析。
 */
static nmbs_error read_registers(nmbs_t* nmbs, uint8_t fc, uint16_t address, uint16_t quantity, uint16_t* registers) {
    if (quantity < 1 || quantity > 125)
        return NMBS_ERROR_INVALID_ARGUMENT;

    if ((uint32_t) address + (uint32_t) quantity > ((uint32_t) 0xFFFF) + 1)
        return NMBS_ERROR_INVALID_ARGUMENT;

    msg_state_req(nmbs, fc); // 构造事物上下文
    put_req_header(nmbs, 4); // 将请求头写入缓冲区，长度为 4 字节

    put_2(nmbs, address);   // 将寄存器地址写入缓冲区
    put_2(nmbs, quantity);  // 将寄存器数量写入缓冲区

    NMBS_DEBUG_PRINT("a %d\tq %d ", address, quantity);

    const nmbs_error err = send_msg(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    return recv_read_registers_res(nmbs, quantity, registers);
}


/**
 * @brief Client FC03 Read Holding Registers 公共 API。
 */
nmbs_error nmbs_read_holding_registers(nmbs_t* nmbs, uint16_t address, uint16_t quantity, uint16_t* registers_out) {
    return read_registers(nmbs, 3, address, quantity, registers_out);
}


/**
 * @brief Client FC04 Read Input Registers 公共 API。
 */
nmbs_error nmbs_read_input_registers(nmbs_t* nmbs, uint16_t address, uint16_t quantity, uint16_t* registers_out) {
    return read_registers(nmbs, 4, address, quantity, registers_out);
}


/**
 * @brief Client FC05 Write Single Coil。
 *
 * true 编码为 0xFF00，false 编码为 0x0000；
 * RTU 广播发送后不等待响应。
 */
nmbs_error nmbs_write_single_coil(nmbs_t* nmbs, uint16_t address, bool value) {
    msg_state_req(nmbs, 5);
    put_req_header(nmbs, 4);

    const uint16_t value_req = value ? 0xFF00 : 0;

    put_2(nmbs, address);
    put_2(nmbs, value_req);

    NMBS_DEBUG_PRINT("a %d\tvalue %d ", address, value_req);

    const nmbs_error err = send_msg(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (!nmbs->msg.broadcast)
        return recv_write_single_coil_res(nmbs, address, value_req);

    return NMBS_ERROR_NONE;
}


/**
 * @brief Client FC06 Write Single Register。
 *
 * RTU 广播请求发送完成后不等待响应。
 */
nmbs_error nmbs_write_single_register(nmbs_t* nmbs, uint16_t address, uint16_t value) {
    msg_state_req(nmbs, 6);
    put_req_header(nmbs, 4);

    put_2(nmbs, address);
    put_2(nmbs, value);

    NMBS_DEBUG_PRINT("a %d\tvalue %d", address, value);

    const nmbs_error err = send_msg(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    // RTU 广播地址 0 不会有从站响应，因此广播写请求发送后立即成功返回。
    if (!nmbs->msg.broadcast)
        return recv_write_single_register_res(nmbs, address, value);

    return NMBS_ERROR_NONE;
}


/**
 * @brief Client FC15 Write Multiple Coils。
 *
 * quantity 最大 1968；请求中的线圈按位打包，Byte Count=(quantity+7)/8。
 */
nmbs_error nmbs_write_multiple_coils(nmbs_t* nmbs, uint16_t address, uint16_t quantity, const nmbs_bitfield coils) {
    if (quantity < 1 || quantity > 0x07B0)
        return NMBS_ERROR_INVALID_ARGUMENT;

    if ((uint32_t) address + (uint32_t) quantity > ((uint32_t) 0xFFFF) + 1)
        return NMBS_ERROR_INVALID_ARGUMENT;

    uint8_t coils_bytes = (quantity + 7) / 8;

    msg_state_req(nmbs, 15);
    put_req_header(nmbs, 5 + coils_bytes);

    put_2(nmbs, address);
    put_2(nmbs, quantity);
    put_1(nmbs, coils_bytes);
    NMBS_DEBUG_PRINT("a %d\tq %d\tb %d\t", address, quantity, coils_bytes);

    NMBS_DEBUG_PRINT("coils ");
    for (int i = 0; i < coils_bytes; i++) {
        put_1(nmbs, coils[i]);
        NMBS_DEBUG_PRINT("%d ", coils[i]);
    }

    const nmbs_error err = send_msg(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (!nmbs->msg.broadcast)
        return recv_write_multiple_coils_res(nmbs, address, quantity);

    return NMBS_ERROR_NONE;
}


/**
 * @brief Client FC16 Write Multiple Registers。
 *
 * 每个寄存器按高字节在前写入，请求包含 Byte Count=quantity*2。
 */
nmbs_error nmbs_write_multiple_registers(nmbs_t* nmbs, uint16_t address, uint16_t quantity, const uint16_t* registers) {
    if (quantity < 1 || quantity > 0x007B)
        return NMBS_ERROR_INVALID_ARGUMENT;

    if ((uint32_t) address + (uint32_t) quantity > ((uint32_t) 0xFFFF) + 1)
        return NMBS_ERROR_INVALID_ARGUMENT;

    const uint8_t registers_bytes = quantity * 2;

    msg_state_req(nmbs, 16);
    put_req_header(nmbs, 5 + registers_bytes);

    put_2(nmbs, address);
    put_2(nmbs, quantity);
    put_1(nmbs, registers_bytes);
    NMBS_DEBUG_PRINT("a %d\tq %d\tb %d\t", address, quantity, registers_bytes);

    NMBS_DEBUG_PRINT("regs ");
    for (int i = 0; i < quantity; i++) {
        put_2(nmbs, registers[i]);
        NMBS_DEBUG_PRINT("%d ", registers[i]);
    }

    const nmbs_error err = send_msg(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (!nmbs->msg.broadcast)
        return recv_write_multiple_registers_res(nmbs, address, quantity);

    return NMBS_ERROR_NONE;
}


/**
 * @brief Client FC20 Read File Record。
 *
 * 当前 API 构造单个文件记录子请求，Reference Type 固定为 0x06。
 */
nmbs_error nmbs_read_file_record(nmbs_t* nmbs, uint16_t file_number, uint16_t record_number, uint16_t* registers,
                                 uint16_t count) {
    if (file_number == 0x0000)
        return NMBS_ERROR_INVALID_ARGUMENT;

    if (record_number > 0x270F)
        return NMBS_ERROR_INVALID_ARGUMENT;

    // In expected response: max PDU length = 253, assuming a single file request, (253 - 1 - 1 - 1 - 1) / 2 = 124
    if (count < 1 || count > 124 || !registers)
        return NMBS_ERROR_INVALID_ARGUMENT;

    msg_state_req(nmbs, 20);
    put_req_header(nmbs, 8);

    put_1(nmbs, 7);    // add Byte Count
    put_1(nmbs, 6);    // add Reference Type const
    put_2(nmbs, file_number);
    put_2(nmbs, record_number);
    put_2(nmbs, count);
    NMBS_DEBUG_PRINT("a %d\tr %d\tl %d\t fread ", file_number, record_number, count);

    const nmbs_error err = send_msg(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    return recv_read_file_record_res(nmbs, registers, count);
}


/**
 * @brief Client FC21 Write File Record。
 *
 * 使用 put_regs() 批量编码寄存器数据；正常响应会回显请求。
 */
nmbs_error nmbs_write_file_record(nmbs_t* nmbs, uint16_t file_number, uint16_t record_number, const uint16_t* registers,
                                  uint16_t count) {
    if (file_number == 0x0000)
        return NMBS_ERROR_INVALID_ARGUMENT;

    if (record_number > 0x270F)
        return NMBS_ERROR_INVALID_ARGUMENT;

    if (count < 1 || count > 122 || !registers)
        return NMBS_ERROR_INVALID_ARGUMENT;

    const uint16_t data_size = count * 2;

    msg_state_req(nmbs, 21);
    put_req_header(nmbs, 8 + data_size);

    put_1(nmbs, 7 + data_size);    // add Byte Count
    put_1(nmbs, 6);                // add Reference Type const
    put_2(nmbs, file_number);
    put_2(nmbs, record_number);
    put_2(nmbs, count);
    put_regs(nmbs, registers, count);
    NMBS_DEBUG_PRINT("a %d\tr %d\tl %d\t fwrite ", file_number, record_number, count);

    const nmbs_error err = send_msg(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (!nmbs->msg.broadcast)
        return recv_write_file_record_res(nmbs, file_number, record_number, registers, count);

    return NMBS_ERROR_NONE;
}


/**
 * @brief Client FC23 Read/Write Multiple Registers。
 *
 * 一帧中同时携带读地址/数量与写地址/数量、写数据，
 * 响应只返回读取到的寄存器。
 */
nmbs_error nmbs_read_write_registers(nmbs_t* nmbs, uint16_t read_address, uint16_t read_quantity,
                                     uint16_t* registers_out, uint16_t write_address, uint16_t write_quantity,
                                     const uint16_t* registers) {
    if (read_quantity < 1 || read_quantity > 0x007D)
        return NMBS_ERROR_INVALID_ARGUMENT;

    if ((uint32_t) read_address + (uint32_t) read_quantity > ((uint32_t) 0xFFFF) + 1)
        return NMBS_ERROR_INVALID_ARGUMENT;

    if (write_quantity < 1 || write_quantity > 0x0079)
        return NMBS_ERROR_INVALID_ARGUMENT;

    if ((uint32_t) write_address + (uint32_t) write_quantity > ((uint32_t) 0xFFFF) + 1)
        return NMBS_ERROR_INVALID_ARGUMENT;

    const uint8_t registers_bytes = write_quantity * 2;

    msg_state_req(nmbs, 23);
    put_req_header(nmbs, 9 + registers_bytes);

    put_2(nmbs, read_address);
    put_2(nmbs, read_quantity);
    put_2(nmbs, write_address);
    put_2(nmbs, write_quantity);
    put_1(nmbs, registers_bytes);

    NMBS_DEBUG_PRINT("read a %d\tq %d ", read_address, read_quantity);
    NMBS_DEBUG_PRINT("write a %d\tq %d\tb %d\t", write_address, write_quantity, registers_bytes);

    NMBS_DEBUG_PRINT("regs ");
    for (int i = 0; i < write_quantity; i++) {
        put_2(nmbs, registers[i]);
        NMBS_DEBUG_PRINT("%d ", registers[i]);
    }

    const nmbs_error err = send_msg(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    return recv_read_registers_res(nmbs, read_quantity, registers_out);
}


/**
 * @brief Client 读取 Basic Device Identification（对象 0x00~0x02）。
 *
 * 根据 More Follows / Next Object ID 自动发送后续请求，直到完整接收。
 */
nmbs_error nmbs_read_device_identification_basic(nmbs_t* nmbs, char* vendor_name, char* product_code,
                                                 char* major_minor_revision, uint8_t buffers_length) {
    const uint8_t order[3] = {0, 1, 2};
    char* buffers[3] = {vendor_name, product_code, major_minor_revision};
    uint8_t total_received = 0;
    uint8_t next_object_id = 0x00;

    while (next_object_id != 0x7F) {
        msg_state_req(nmbs, 43);
        put_msg_header(nmbs, 3);
        put_1(nmbs, 0x0E);
        put_1(nmbs, 1);
        put_1(nmbs, next_object_id);

        nmbs_error err = send_msg(nmbs);
        if (err != NMBS_ERROR_NONE)
            return err;

        uint8_t objects_received = 0;
        err = recv_read_device_identification_res(nmbs, 3, buffers, buffers_length, order, NULL, &next_object_id,
                                                  &objects_received);
        if (err != NMBS_ERROR_NONE)
            return err;

        total_received += objects_received;
        if (total_received > 3)
            return NMBS_ERROR_INVALID_RESPONSE;

        if (objects_received == 0)
            return NMBS_ERROR_INVALID_RESPONSE;
    }

    return NMBS_ERROR_NONE;
}


/**
 * @brief Client 读取 Regular Device Identification（对象 0x03~0x06）。
 */
nmbs_error nmbs_read_device_identification_regular(nmbs_t* nmbs, char* vendor_url, char* product_name, char* model_name,
                                                   char* user_application_name, uint8_t buffers_length) {
    const uint8_t order[7] = {0, 0, 0, 0, 1, 2, 3};
    char* buffers[4] = {vendor_url, product_name, model_name, user_application_name};
    uint8_t total_received = 0;
    uint8_t next_object_id = 0x03;

    while (next_object_id != 0x7F) {
        msg_state_req(nmbs, 43);
        put_req_header(nmbs, 3);
        put_1(nmbs, 0x0E);
        put_1(nmbs, 2);
        put_1(nmbs, next_object_id);

        nmbs_error err = send_msg(nmbs);
        if (err != NMBS_ERROR_NONE)
            return err;

        uint8_t objects_received = 0;
        err = recv_read_device_identification_res(nmbs, 4, buffers, buffers_length, order, NULL, &next_object_id,
                                                  &objects_received);
        if (err != NMBS_ERROR_NONE)
            return err;

        total_received += objects_received;
        if (total_received > 4)
            return NMBS_ERROR_INVALID_RESPONSE;

        if (objects_received == 0)
            return NMBS_ERROR_INVALID_RESPONSE;
    }

    return NMBS_ERROR_NONE;
}


/**
 * @brief Client 读取 Extended Device Identification（对象 ID >= 0x80）。
 *
 * 支持设备返回多段响应，并持续从 Next Object ID 继续读取。
 */
nmbs_error nmbs_read_device_identification_extended(nmbs_t* nmbs, uint8_t object_id_start, uint8_t* ids, char** buffers,
                                                    uint8_t ids_length, uint8_t buffer_length,
                                                    uint8_t* objects_count_out) {
    if (object_id_start < 0x80)
        return NMBS_ERROR_INVALID_ARGUMENT;

    uint8_t total_received = 0;
    uint8_t next_object_id = object_id_start;

    while (next_object_id != 0x7F) {
        msg_state_req(nmbs, 43);
        put_req_header(nmbs, 3);
        put_1(nmbs, 0x0E);
        put_1(nmbs, 3);
        put_1(nmbs, next_object_id);

        nmbs_error err = send_msg(nmbs);
        if (err != NMBS_ERROR_NONE)
            return err;

        uint8_t objects_received = 0;
        err = recv_read_device_identification_res(nmbs, ids_length - total_received, &buffers[total_received],
                                                  buffer_length, NULL, &ids[total_received], &next_object_id,
                                                  &objects_received);
        if (err != NMBS_ERROR_NONE)
            return err;

        total_received += objects_received;
    }

    *objects_count_out = total_received;

    return NMBS_ERROR_NONE;
}


/**
 * @brief Client 按指定 Object ID 读取单个设备标识对象（Specific Access）。
 */
nmbs_error nmbs_read_device_identification(nmbs_t* nmbs, uint8_t object_id, char* buffer, uint8_t buffer_length) {
    if (object_id > 0x06 && object_id < 0x80)
        return NMBS_ERROR_INVALID_ARGUMENT;

    msg_state_req(nmbs, 43);
    put_req_header(nmbs, 3);
    put_1(nmbs, 0x0E);
    put_1(nmbs, 4);
    put_1(nmbs, object_id);

    const nmbs_error err = send_msg(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    char* buf[1] = {buffer};
    return recv_read_device_identification_res(nmbs, 1, buf, buffer_length, NULL, NULL, NULL, NULL);
}


/**
 * @brief 发送自定义/原始 Modbus PDU。
 *
 * 调用者提供 Function Code 和 PDU Data；nanoMODBUS 仍负责 RTU/TCP 头、
 * RTU CRC 和底层发送。PDU Data 的字段字节序由调用者自行保证。
 */
nmbs_error nmbs_send_raw_pdu(nmbs_t* nmbs, uint8_t fc, const uint8_t* data, uint16_t data_len) {
    if (data_len > 252 || (data_len > 0 && !data))
        return NMBS_ERROR_INVALID_ARGUMENT;

    msg_state_req(nmbs, fc);
    put_msg_header(nmbs, data_len);

    NMBS_DEBUG_PRINT("raw ");
    for (uint16_t i = 0; i < data_len; i++) {
        put_1(nmbs, data[i]);
        NMBS_DEBUG_PRINT("%d ", data[i]);
    }

    return send_msg(nmbs);
}


/**
 * @brief 接收原始 PDU 请求对应的响应数据。
 *
 * nanoMODBUS 仍负责响应头、异常和 RTU CRC 校验；
 * 调用者负责解释 data_out 中的功能码私有数据。
 */
nmbs_error nmbs_receive_raw_pdu_response(nmbs_t* nmbs, uint8_t* data_out, uint8_t data_out_len) {
    nmbs_error err = recv_res_header(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    err = recv(nmbs, data_out_len);
    if (err != NMBS_ERROR_NONE)
        return err;

    if (data_out) {
        for (uint16_t i = 0; i < data_out_len; i++)
            data_out[i] = get_1(nmbs);
    }
    else {
        for (uint16_t i = 0; i < data_out_len; i++)
            get_1(nmbs);
    }

    err = recv_msg_footer(nmbs);
    if (err != NMBS_ERROR_NONE)
        return err;

    return NMBS_ERROR_NONE;
}
#endif


#ifndef NMBS_STRERROR_DISABLED

/* ========================================================================== */
/* 错误字符串 */
/* ========================================================================== */

/**
 * @brief 将 nmbs_error 转换为便于日志打印的英文字符串。
 */
const char* nmbs_strerror(nmbs_error error) {
    switch (error) {
        case NMBS_ERROR_INVALID_REQUEST:
            return "invalid request received";

        case NMBS_ERROR_INVALID_UNIT_ID:
            return "invalid unit ID received";

        case NMBS_ERROR_INVALID_TCP_MBAP:
            return "invalid TCP MBAP received";

        case NMBS_ERROR_CRC:
            return "invalid CRC received";

        case NMBS_ERROR_TRANSPORT:
            return "transport error";

        case NMBS_ERROR_TIMEOUT:
            return "timeout";

        case NMBS_ERROR_INVALID_RESPONSE:
            return "invalid response received";

        case NMBS_ERROR_INVALID_ARGUMENT:
            return "invalid argument provided";

        case NMBS_ERROR_NONE:
            return "no error";

        case NMBS_EXCEPTION_ILLEGAL_FUNCTION:
            return "modbus exception 1: illegal function";

        case NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS:
            return "modbus exception 2: illegal data address";

        case NMBS_EXCEPTION_ILLEGAL_DATA_VALUE:
            return "modbus exception 3: data value";

        case NMBS_EXCEPTION_SERVER_DEVICE_FAILURE:
            return "modbus exception 4: server device failure";

        default:
            return "unknown error";
    }
}
#endif

