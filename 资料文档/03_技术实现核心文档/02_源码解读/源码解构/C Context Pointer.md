# C 语言 Context Pointer 模式说明文档

> **一句话定义**
> Context Pointer 模式 = **回调函数 + 显式 `void *` 上下文指针 + 库原样透传**。
> 目的是让 C 的回调函数能够访问调用者的状态，而不必依赖全局变量。

---

## 0. 阅读约定：如何区分函数指针 / 函数定义 / 函数调用

C 里 `callback_t` 这类名字很容易混淆。本文档统一使用下面的标注方式：

| 形态         | 例子                               | 是什么                           |
| ------------ | ---------------------------------- | -------------------------------- |
| **类型**     | `typedef void (*callback_t)(int);` | 函数指针**类型**（typedef）      |
| **变量**     | `callback_t s_cb;`                 | 函数指针**变量**（保存函数地址） |
| **函数定义** | `void my_callback(int e) { ... }`  | 一个**真正的函数**               |
| **取地址**   | `register_callback(my_callback)`   | 函数名退化为**函数指针**         |
| **调用**     | `s_cb(event);`                     | 通过函数指针**调用**函数         |

代码注释里统一标记：

```
/* [类型]     函数指针类型定义 */
/* [FP变量]   函数指针变量 */
/* [函数]     函数定义 */
/* [调用]     函数调用 */
/* [用户数据] 普通结构体 / 变量 */
```

---

## 1. 问题：回调需要状态，但函数指针不携带状态

C 的函数指针**只保存函数地址，不保存任何数据**。

```c
/* [类型] 函数指针类型：接收一个 int，返回 void */
typedef void (*callback_t)(int event);

/* [函数] 注册函数：参数是一个函数指针 */
void register_callback(callback_t cb);
```

库触发事件时只能这样调用：

```c
/* [调用] 通过函数指针调用，只能传 event */
cb(event);
```

可是回调通常需要知道：

- 是哪个实例触发的？
- 往哪个 fd / 串口 / socket 写？
- 计数器、缓冲区、锁在哪里？

**矛盾点**：函数指针没地方放这些状态。

---

## 2. 两种解决方案

- **方案 A：普通模式（全局变量）** —— 状态放全局，回调直接访问。
- **方案 B：Context Pointer 模式** —— 状态打包成结构体，随回调一起注册，库原样传回。

下面用**同一个功能**（统计事件次数）分别实现，代码全部展开。

---

## 3. 方案 A —— 普通模式（全局变量）

### 3.1 库侧代码

```c
/* ============================================================
 *  库侧
 * ============================================================ */

/* [类型] 函数指针类型：接收 int，返回 void
 *        这是一个"类型"，不是变量，也不是函数 */
typedef void (*callback_t)(int event);

/* [FP变量] 函数指针变量：用来保存用户注册进来的回调地址
 *          初始为 NULL */
static callback_t s_cb = NULL;

/* [函数] 注册函数（真正的函数）
 *        参数 cb 是一个函数指针
 *        作用：把用户传进来的函数地址存到 s_cb */
void register_callback(callback_t cb)
{
    s_cb = cb; // 将cb函数注册给s_cb,本例传入的是my_callback
}

/* [函数] 事件触发函数（真正的函数）
 *        作用：如果注册过回调，就通过函数指针调用它 */
void fire_event(int event)
{
    if (s_cb != NULL) {
        s_cb(event);      /* [调用] 通过函数指针调用用户函数 */
    }
}
```

### 3.2 用户侧代码

```c
/* ============================================================
 *  用户侧
 * ============================================================ */

/* [用户数据] 状态放在全局变量里（这就是"普通模式"的特征） */
static int g_fd    = 0;
static int g_count = 0;

/* [函数] 回调函数（真正的函数）
 *        它的地址会被 register_callback 保存到 s_cb
 *        注意：它没有 context 参数，只能靠全局变量拿状态 */
static void my_callback(int event)
{
    g_count++;              /* 读全局变量 */
    (void)g_fd;             /* 读全局变量 */
    (void)event;
}

/* [函数] 初始化函数（真正的函数）
 *        作用：准备全局状态 + 注册回调 */
void app_init(void)
{
    g_fd    = 10;
    g_count = 0;

    /* [调用] 把 my_callback 的地址传给库
     *        函数名 my_callback 会退化成函数指针 */
    register_callback(my_callback);
}
```

### 3.3 调用链

```
[用户]  app_init()
          ├─ g_fd    = 10
          ├─ g_count = 0
          └─ register_callback(my_callback)
                 │
                 │  my_callback 退化为函数指针
                 ▼
[库]     register_callback(cb)
            └─ s_cb = cb            ← 只存了函数地址，没存状态
                 │
                 ▼
[库]     fire_event(event)
            └─ s_cb(event)          ← 通过函数指针调用
                 │
                 ▼
[用户]   my_callback(event)
            ├─ g_count++            ← 读全局变量
            └─ g_fd                 ← 读全局变量
```

### 3.4 问题

| 问题         | 说明                                 |
| ------------ | ------------------------------------ |
| 单实例       | 全局变量只有一份，第二个实例无处安放 |
| 线程不安全   | 多线程共享同一份状态                 |
| 不可重入     | 递归/嵌套调用互相污染                |
| 难测试       | 测试之间残留状态                     |
| 库与业务耦合 | 回调只能服务这组全局变量             |

**根本原因**：状态和回调是**分离**的，连接它们的只有"全局名字"。

---

## 4. 方案 B —— Context Pointer 模式

### 4.1 库侧代码

```c
/* ============================================================
 *  库侧
 * ============================================================ */

/* [类型] 函数指针类型：接收 int + void*，返回 void
 *        多出来的 void* 就是 context pointer */
typedef void (*callback_t)(int event, void *context);

/* [FP变量] 函数指针变量：保存用户的回调地址 */
static callback_t s_cb  = NULL;

/* [变量] 普通指针变量：保存用户的上下文地址
 *        库不解释它，只负责搬运 */
static void      *s_ctx = NULL;

/* [函数] 注册函数（真正的函数）
 *        参数1 cb      ：函数指针
 *        参数2 context ：用户的上下文指针
 *        作用：两者都存下来 */
void register_callback(callback_t cb, void *context)
{
    s_cb  = cb;
    s_ctx = context;
}

/* [函数] 事件触发函数（真正的函数）
 *        作用：通过函数指针调用用户函数，并把 context 原样传回 */
void fire_event(int event)
{
    if (s_cb != NULL) {
        s_cb(event, s_ctx);   /* [调用] 原样透传 context */
    }
}
```

### 4.2 用户侧代码

```c
/* ============================================================
 *  用户侧
 * ============================================================ */

/* [用户数据] 状态打包成结构体（Context Pointer 模式的特征）
 *            不再用全局变量散着放 */
typedef struct {
    int fd;
    int count;
} my_ctx_t;

/* [函数] 回调函数（真正的函数）
 *        参数 context 就是注册时传进来的 &ctx */
static void my_callback(int event, void *context)
{
    /* [转换] 把 void* 转回真实类型 */
    my_ctx_t *ctx = (my_ctx_t *)context;

    ctx->count++;          /* 用自己的状态 */
    (void)ctx->fd;
    (void)event;
}

/* [函数] 初始化函数（真正的函数） */
void app_init(void)
{
    /* [用户数据] 上下文对象
     * static：保证生命周期覆盖整个回调使用期 */
    static my_ctx_t ctx = { .fd = 10, .count = 0 };

    /* [调用] 函数指针 + 上下文指针，一起注册
     *        my_callback 退化为函数指针
     *        &ctx        是上下文指针 */
    register_callback(my_callback, &ctx);
}
```

### 4.3 调用链

```
[用户]  app_init()
          ├─ 准备 ctx = {fd=10, count=0}
          └─ register_callback(my_callback, &ctx)
                 │
                 │  my_callback → 函数指针
                 │  &ctx        → 上下文指针
                 ▼
[库]     register_callback(cb, context)
            ├─ s_cb  = cb           ← 存函数地址
            └─ s_ctx = context      ← 存上下文地址（关键）
                 │
                 ▼
[库]     fire_event(event)
            └─ s_cb(event, s_ctx)   ← 函数指针 + 上下文，一起传
                 │
                 ▼
[用户]   my_callback(event, context)
            ├─ ctx = (my_ctx_t *)context   ← 转回真实类型
            ├─ ctx->count++                ← 用自己的状态
            └─ ctx->fd                     ← 用自己的状态
```

### 4.4 和方案 A 的核心差异

| 环节           | 方案 A                      | 方案 B                     |
| -------------- | --------------------------- | -------------------------- |
| 状态存在哪     | 全局变量 `g_fd` / `g_count` | 结构体 `ctx`（调用者私有） |
| 注册时传什么   | 只传函数指针                | 函数指针 + `&ctx`          |
| 库保存什么     | 只保存函数指针              | 保存函数指针 + 上下文指针  |
| 回调怎么拿状态 | 通过全局名字                | 通过参数 `context`         |
| 回调签名       | `void cb(int)`              | `void cb(int, void*)`      |

**一句话**：
方案 A 靠**全局名字**找状态，方案 B 靠**参数**拿状态。

---

## 5. 两个方案并排对比

### 5.1 数据结构

```
方案 A（全局模式）
   ┌──────────────┐         ┌──────────────────┐
   │ my_callback  │ ──────► │ g_fd, g_count    │   耦合：靠全局名字
   │  (函数)      │         │  (全局变量)      │
   └──────────────┘         └──────────────────┘
        ▲
        │ s_cb（函数指针）
        │
   ┌──────────────┐
   │  库内部      │
   └──────────────┘

方案 B（Context Pointer）
   ┌──────────────┐         ┌──────────────────┐
   │ my_callback  │ ◄────── │  ctx             │   解耦：靠参数传递
   │  (函数)      │         │  (用户私有结构体)│
   └──────────────┘         └──────────────────┘
        ▲                            ▲
        │ s_cb                       │ s_ctx
        │                            │
   ┌──────────────────────────────────────────┐
   │  库内部（只搬运，不解释）                │
   └──────────────────────────────────────────┘
```

### 5.2 特性对比

| 特性         | 方案 A 全局变量 | 方案 B Context Pointer |
| ------------ | --------------- | ---------------------- |
| 多实例       | ❌ 不支持        | ✅ 天然支持             |
| 线程安全     | ❌ 需加锁        | ✅ 各用各的 ctx         |
| 可重入       | ❌               | ✅                      |
| 可测试       | ❌ 状态残留      | ✅ 隔离                 |
| 库与业务耦合 | 高              | 低（库只认 `void*`）   |
| 代码复杂度   | 低              | 略高                   |
| 适用场景     | 单实例小工具    | 通用库、多实例、多线程 |

---

## 6. 多实例：Context Pointer 的真正威力

方案 B 最直接的好处：**同一个回调函数，服务 N 个实例**。

### 6.1 库侧代码

```c
/* ============================================================
 *  库侧：支持多个 slot
 * ============================================================ */

/* [类型] 函数指针类型 */
typedef void (*callback_t)(int event, void *context);

/* [用户数据] 一个 slot = 一个"函数指针 + 一个上下文指针" */
typedef struct {
    callback_t cb;        /* 函数指针 */
    void      *context;   /* 上下文指针 */
} slot_t;

/* [变量] slot 数组 */
#define MAX_SLOTS 8
static slot_t s_slots[MAX_SLOTS];
static int    s_count = 0;

/* [函数] 注册：返回 slot 编号 */
int register_callback(callback_t cb, void *context)
{
    if (s_count >= MAX_SLOTS) return -1;

    s_slots[s_count].cb      = cb;
    s_slots[s_count].context = context;
    return s_count++;
}

/* [函数] 触发：根据 slot 编号调用对应回调 */
void fire_event(int slot, int event)
{
    if (slot >= 0 && slot < s_count) {
        s_slots[slot].cb(event, s_slots[slot].context);
        /*       ^^^^^^^^^^ 函数指针    ^^^^^^^^^^^^^^^^^ 上下文 */
    }
}
```

### 6.2 用户侧代码

```c
/* ============================================================
 *  用户侧：两个实例，共用同一个回调函数
 * ============================================================ */

/* [用户数据] 上下文结构体 */
typedef struct {
    const char *name;
    int         fd;
    int         count;
} my_ctx_t;

/* [函数] 一个回调，服务所有实例 */
static void my_callback(int event, void *context)
{
    my_ctx_t *ctx = (my_ctx_t *)context;
    ctx->count++;              /* 各自加各自的 */
    (void)ctx->name;
    (void)event;
}

/* [函数] 初始化 */
void app_init(void)
{
    /* [用户数据] 两个独立的上下文对象 */
    static my_ctx_t ctx_a = { .name = "A", .fd = 10, .count = 0 };
    static my_ctx_t ctx_b = { .name = "B", .fd = 20, .count = 0 };

    /* [调用] 同一个函数指针，不同的上下文 */
    int slot_a = register_callback(my_callback, &ctx_a);
    int slot_b = register_callback(my_callback, &ctx_b);

    /* [调用] 触发时互不影响 */
    fire_event(slot_a, 1);   /* ctx_a.count = 1 */
    fire_event(slot_b, 1);   /* ctx_b.count = 1，互不影响 */
}
```

### 6.3 调用链

```
[用户]  app_init()
          ├─ ctx_a = {A, 10, 0}
          ├─ ctx_b = {B, 20, 0}
          ├─ register_callback(my_callback, &ctx_a)  → slot_a
          └─ register_callback(my_callback, &ctx_b)  → slot_b

[库]    s_slots[0] = { my_callback, &ctx_a }
        s_slots[1] = { my_callback, &ctx_b }
              ▲              ▲
              │              │
          函数指针        上下文指针

[用户]  fire_event(slot_a, 1)
[库]      s_slots[0].cb(1, s_slots[0].context)
              │                   │
              ▼                   ▼
[用户]    my_callback(1, &ctx_a)
              └─ ctx->count++     ← 改的是 ctx_a

[用户]  fire_event(slot_b, 1)
[库]      s_slots[1].cb(1, s_slots[1].context)
              └─ my_callback(1, &ctx_b)
                    └─ ctx->count++   ← 改的是 ctx_b
```

**方案 A 做不到这一点**：一个全局变量只能服务一个实例，第二个实例要么加全局开关，要么直接崩溃。

---

## 7. 真实场景：nanomodbus 的 `arg`

你最初看到的代码正是这个模式：

```c
typedef struct nmbs_platform_conf {
    nmbs_transport transport;

    /* [类型] 这里定义的是"函数指针成员"
     *        每个成员都是一个函数指针，等待用户赋值 */
    int32_t (*read)(uint8_t *buf, uint16_t count,
                    int32_t byte_timeout_ms, void *arg);
    int32_t (*write)(const uint8_t *buf, uint16_t count,
                     int32_t byte_timeout_ms, void *arg);
    uint16_t (*crc_calc)(const uint8_t *data, uint32_t length, void *arg);
    void     (*flush)(nmbs_t *nmbs, void *arg);

    /* [变量] 上下文指针：用户塞进来，库原样透传给上面四个函数 */
    void *arg;

    uint32_t initialized;
} nmbs_platform_conf;
```

### 7.1 用户侧代码（全部展开）

```c
/* ============================================================
 *  用户侧
 * ============================================================ */

/* [用户数据] 上下文结构体 */
typedef struct {
    int  fd;
    bool verbose;
} my_port_t;

/* [函数] 读回调（真正的函数）
 *        库调用它时，arg 就是用户注册的 &port */
static int32_t my_read(uint8_t *buf, uint16_t count,
                       int32_t timeout, void *arg)
{
    my_port_t *port = (my_port_t *)arg;   /* 转回真实类型 */
    if (port->verbose) printf("read %u\n", count);
    return read(port->fd, buf, count);
}

/* [函数] 写回调（真正的函数） */
static int32_t my_write(const uint8_t *buf, uint16_t count,
                        int32_t timeout, void *arg)
{
    my_port_t *port = (my_port_t *)arg;
    (void)timeout;
    return write(port->fd, buf, count);
}

/* [函数] 初始化 */
void setup(void)
{
    /* [用户数据] 上下文对象
     * static：保证生命周期覆盖整个通信期间 */
    static my_port_t port = { .fd = open_port(), .verbose = true };

    /* [用户数据] 库配置结构体 */
    nmbs_platform_conf conf;
    nmbs_platform_conf_create(&conf);

    conf.transport = NMBS_TRANSPORT_RTU;

    /* [赋值] 把函数地址赋给函数指针成员
     *        my_read 退化为函数指针 */
    conf.read  = my_read;
    conf.write = my_write;

    /* [赋值] 把上下文地址赋给 arg 成员
     *        这是整个模式的关键 */
    conf.arg   = &port;

    /* [调用] 把整个配置交给库
     *        库会把 conf.arg 保存起来 */
    /* nmbs_init(&nmbs, &conf); */
}
```

### 7.2 调用链

```
[用户]  setup()
          ├─ port = {fd, verbose}
          ├─ conf.read  = my_read          ← 函数指针赋值
          ├─ conf.write = my_write         ← 函数指针赋值
          ├─ conf.arg   = &port            ← 上下文指针赋值
          └─ nmbs_init(&nmbs, &conf)
                  │
                  ▼
[库]      nmbs_init 内部
            ├─ nmbs->read  = conf.read
            ├─ nmbs->write = conf.write
            └─ nmbs->arg   = conf.arg     ← 把 &port 存下来

（库要读数据时）
[库]      nmbs->read(buf, count, timeout, nmbs->arg)
                                            │
                                            │ 就是 &port，原样透传
                                            ▼
[用户]    my_read(buf, count, timeout, arg)
            ├─ port = (my_port_t *)arg
            └─ read(port->fd, buf, count)
```

**关键点**：

- `conf.read` 是**函数指针**，被赋值为 `my_read` 这个函数。
- `conf.arg` 是**普通指针变量**，被赋值为 `&port`。
- 库调用 `conf.read(...)` 时，把 `conf.arg` 作为最后一个参数传进去。
- 所以 `my_read` 里的 `arg` 参数，就是用户当初塞进 `conf.arg` 的那个 `&port`。

---

## 8. 和 C++ 的对比

| 对比项   | C++ `this`             | C Context Pointer       |
| -------- | ---------------------- | ----------------------- |
| 谁传     | 编译器隐式传           | 库显式传（作为参数）    |
| 指向     | 调用该成员函数的对象   | 用户自定义的任意结构体  |
| 在签名里 | 不出现                 | 出现（`void *context`） |
| 是否必须 | 每个非静态成员函数都有 | 可以传 `NULL`           |
| 能否改变 | 不能                   | 用户随时可以重新注册    |

**心智模型**：

```
C++  成员函数  =  普通函数 + 隐式 this
C    回调      =  函数指针 + 显式 context 参数
闭包          =  函数     + 捕获的环境
```

三者目的一致：**让函数能访问"属于自己"的状态**。

---

## 9. 使用规范与陷阱

### 9.1 生命周期

`context` 指向的对象必须**活得比回调注册久**。

```c
/* ❌ 错误：栈上对象，函数返回就失效 */
void bad(void)
{
    my_ctx_t ctx;                         /* 栈变量 */
    register_callback(my_callback, &ctx); /* 函数返回后 &ctx 悬空 */
}   /* ← 回调再访问 ctx 就是未定义行为 */

/* ✅ 正确：static、堆分配、或生命周期覆盖整个使用期 */
static my_ctx_t ctx;
register_callback(my_callback, &ctx);
```

### 9.2 类型一致

注册时传 `&my_ctx_t`，回调里就必须转回 `my_ctx_t *`。
传错类型是**未定义行为**，编译器不会报错。

```c
/* 注册时 */
register_callback(my_callback, &my_ctx);        /* 传 my_ctx_t* */

/* 回调里必须对应 */
static void my_callback(int e, void *context)
{
    my_ctx_t *ctx = (my_ctx_t *)context;        /* ✅ 类型一致 */
    /* other_t *o = (other_t *)context; */      /* ❌ 类型不一致 */
}
```

**建议**：每个模块只定义一种 context 类型，回调第一行立刻转换。

### 9.3 所有权

- 库**不拥有** `context`。
- 库**不负责释放** `context`。
- **谁分配，谁释放**。

### 9.4 多线程

Context Pointer 模式本身不提供线程安全，但**让线程安全变得容易**：

- 每个线程用独立的 `ctx` → 天然无锁。
- 共享 `ctx` → 需自己加锁。

### 9.5 `NULL` 处理

不需要上下文时可以传 `NULL`，回调里做检查：

```c
static void cb(int event, void *context)
{
    if (!context) return;
    /* ... */
}
```

### 9.6 多回调共享同一个 context

像 nanomodbus 那样，`read` / `write` / `crc_calc` / `flush` 共用一个 `arg`。
如果它们需要不同状态，就把它们打包进一个结构体：

```c
typedef struct {
    int  fd;
    bool verbose;
    int  read_count;
    int  write_count;
} my_port_t;
```

---

## 10. 总结

### 10.1 核心思想

> **状态不再放在全局变量里，而是打包进结构体，随回调一起注册，由库原样透传，回调再转回真实类型。**

### 10.2 调用链一句话

```
用户注册（函数指针 + 上下文指针）
      │
      ▼
库保存（两个都存）
      │
      ▼
事件触发
      │
      ▼
库调用（函数指针(事件, 上下文指针)）
      │
      ▼
回调转类型，访问自己的状态
```

### 10.3 关键区分

| 东西                      | 是什么           | 在模式中的角色     |
| ------------------------- | ---------------- | ------------------ |
| `callback_t`              | 函数指针**类型** | 定义回调的签名     |
| `s_cb` / `conf.read`      | 函数指针**变量** | 保存用户函数地址   |
| `my_callback` / `my_read` | **函数**         | 用户实现，被调用   |
| `s_ctx` / `conf.arg`      | 普通指针**变量** | 保存用户上下文地址 |
| `ctx` / `port`            | **用户数据**     | 被回调访问的状态   |
| `register_callback(...)`  | **函数调用**     | 注册               |
| `s_cb(event, s_ctx)`      | **函数指针调用** | 触发               |

### 10.4 何时用

- 库/框架提供回调接口
- 回调需要访问调用者状态
- 需要多实例、多线程、可重入、可测试
- 想让库与业务数据结构解耦

### 10.5 何时可以不用

- 单实例、单线程的小工具
- 库本身就是单例设计（如全局日志器）

### 10.6 常见命名

| 库         | 名字                                    |
| ---------- | --------------------------------------- |
| pthread    | `arg`                                   |
| qsort_r    | `arg`                                   |
| libevent   | `arg` / `ctx`                           |
| GLib       | `user_data`                             |
| nanomodbus | `arg`                                   |
| 通用惯例   | `context` / `ctx` / `cookie` / `opaque` |

**记住一句话**：
Context Pointer 模式，就是 C 语言里**用显式参数模拟 C++ this / 闭包**的标准手法。