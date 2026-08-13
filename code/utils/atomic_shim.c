/* atomic_shim.c
 * 为固件库(libisp.so/libsys.so)提供软件实现的 1 字节原子操作符号。
 * 原因：这些库引用 __sync_fetch_and_add_1 / __sync_fetch_and_and_1 /
 *      __atomic_compare_exchange_1，而我用 musl 工具链无A扩展(-march 含 v0p7)编译时，
 *      这些符号在 libgcc.a 为 hidden，无法供 DSO 动态解析。
 * 提供一个非 hidden 的 .so 导出这些符号，规避 libgcc hidden 冲突。
 * 软件实现（1字节，适合 C906 无原子指令场景）。
 */
#include <stdint.h>
#include <stdbool.h>

typedef unsigned char u8;

/* 简易自旋锁（1字节CAS 未就绪时的退化保护；实际单线程推理场景极少竞争） */
static volatile int _shim_lock = 0;

static inline void _lock(void) {
  while (__atomic_test_and_set(&_shim_lock, __ATOMIC_ACQUIRE)) {
    /* spin */
  }
}
static inline void _unlock(void) { __atomic_clear(&_shim_lock, __ATOMIC_RELEASE); }

/* __sync_fetch_and_add_1(ptr, val): 返回旧值，*ptr += val */
u8 __sync_fetch_and_add_1(volatile u8 *ptr, u8 val) {
  _lock();
  u8 old = *ptr;
  *ptr = (u8)(old + val);
  _unlock();
  return old;
}

/* __sync_fetch_and_and_1(ptr, val): 返回旧值，*ptr &= val */
u8 __sync_fetch_and_and_1(volatile u8 *ptr, u8 val) {
  _lock();
  u8 old = *ptr;
  *ptr = (u8)(old & val);
  _unlock();
  return old;
}

/* __atomic_compare_exchange_1(ptr, expected, desired, weak, succ, fail) */
bool __atomic_compare_exchange_1(volatile void *ptr, void *expected, u8 desired,
                                 bool weak, int succ, int fail) {
  (void)weak; (void)succ; (void)fail;
  volatile u8 *p = (volatile u8 *)ptr;
  u8 *exp = (u8 *)expected;
  _lock();
  u8 cur = *p;
  if (cur == *exp) {
    *p = desired;
    _unlock();
    return true;
  }
  *exp = cur;
  _unlock();
  return false;
}
