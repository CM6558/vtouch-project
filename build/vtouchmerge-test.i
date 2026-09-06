# 1 "src/vtouchmerge.c"
# 1 "<built-in>" 1
# 1 "<built-in>" 3
# 401 "<built-in>" 3
# 1 "<command line>" 1
# 1 "<built-in>" 2
# 1 "src/vtouchmerge.c" 2

# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/errno.h" 1 3 4
# 36 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/errno.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/cdefs.h" 1 3 4
# 335 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/cdefs.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/android/versioning.h" 1 3 4
# 336 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/cdefs.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/android/api-level.h" 1 3 4
# 194 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/android/api-level.h" 3 4
int android_get_application_target_sdk_version() __attribute__((__availability__(android,strict,introduced=24 )));







# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/get_device_api_level_inlines.h" 1 3 4
# 38 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/get_device_api_level_inlines.h" 3 4
int __system_property_get(const char* _Nonnull __name, char* _Nonnull __value);
int atoi(const char* _Nonnull __s) __attribute__((__pure__));

static __inline__ int android_get_device_api_level() {
  char value[92] = { 0 };
  if (__system_property_get("ro.build.version.sdk", value) < 1) return -1;
  int api_level = atoi(value);
  return (api_level > 0) ? api_level : -1;
}
# 203 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/android/api-level.h" 2 3 4
# 337 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/cdefs.h" 2 3 4

# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/android/ndk-version.h" 1 3 4
# 339 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/cdefs.h" 2 3 4
# 37 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/errno.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/errno.h" 1 3 4






# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/errno.h" 1 3 4






# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/errno.h" 1 3 4








# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/errno-base.h" 1 3 4
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/errno.h" 2 3 4
# 8 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/errno.h" 2 3 4
# 8 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/errno.h" 2 3 4
# 38 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/errno.h" 2 3 4
# 52 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/errno.h" 3 4
int* _Nonnull __errno(void) __attribute__((__const__));
# 3 "src/vtouchmerge.c" 2
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 1 3 4
# 38 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/types.h" 1 3 4
# 32 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/types.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stddef.h" 1 3 4
# 72 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stddef.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/__stddef_ptrdiff_t.h" 1 3 4
# 13 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/__stddef_ptrdiff_t.h" 3 4
typedef long int ptrdiff_t;
# 73 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stddef.h" 2 3 4




# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/__stddef_size_t.h" 1 3 4
# 13 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/__stddef_size_t.h" 3 4
typedef long unsigned int size_t;
# 78 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stddef.h" 2 3 4
# 87 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stddef.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/__stddef_wchar_t.h" 1 3 4
# 19 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/__stddef_wchar_t.h" 3 4
typedef unsigned int wchar_t;
# 88 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stddef.h" 2 3 4




# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/__stddef_null.h" 1 3 4
# 93 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stddef.h" 2 3 4
# 107 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stddef.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/__stddef_max_align_t.h" 1 3 4
# 19 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/__stddef_max_align_t.h" 3 4
typedef struct {
  long long __clang_max_align_nonce1
      __attribute__((__aligned__(__alignof__(long long))));
  long double __clang_max_align_nonce2
      __attribute__((__aligned__(__alignof__(long double))));
} max_align_t;
# 108 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stddef.h" 2 3 4




# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/__stddef_offsetof.h" 1 3 4
# 113 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stddef.h" 2 3 4
# 33 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/types.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stdint.h" 1 3 4
# 52 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stdint.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdint.h" 1 3 4
# 32 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdint.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/wchar_limits.h" 1 3 4
# 33 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdint.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stddef.h" 1 3 4
# 34 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdint.h" 2 3 4


typedef signed char __int8_t;
typedef unsigned char __uint8_t;
typedef short __int16_t;
typedef unsigned short __uint16_t;
typedef int __int32_t;
typedef unsigned int __uint32_t;

typedef long __int64_t;
typedef unsigned long __uint64_t;






typedef long __intptr_t;
typedef unsigned long __uintptr_t;





typedef __int8_t int8_t;
typedef __uint8_t uint8_t;

typedef __int16_t int16_t;
typedef __uint16_t uint16_t;

typedef __int32_t int32_t;
typedef __uint32_t uint32_t;

typedef __int64_t int64_t;
typedef __uint64_t uint64_t;

typedef __intptr_t intptr_t;
typedef __uintptr_t uintptr_t;

typedef int8_t int_least8_t;
typedef uint8_t uint_least8_t;

typedef int16_t int_least16_t;
typedef uint16_t uint_least16_t;

typedef int32_t int_least32_t;
typedef uint32_t uint_least32_t;

typedef int64_t int_least64_t;
typedef uint64_t uint_least64_t;

typedef int8_t int_fast8_t;
typedef uint8_t uint_fast8_t;

typedef int64_t int_fast64_t;
typedef uint64_t uint_fast64_t;


typedef int64_t int_fast16_t;
typedef uint64_t uint_fast16_t;
typedef int64_t int_fast32_t;
typedef uint64_t uint_fast32_t;







typedef uint64_t uintmax_t;
typedef int64_t intmax_t;
# 53 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stdint.h" 2 3 4
# 34 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/types.h" 2 3 4


# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/types.h" 1 3 4








# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/types.h" 1 3 4






# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/types.h" 1 3 4








# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/int-ll64.h" 1 3 4








# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/bitsperlong.h" 1 3 4
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/bitsperlong.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/bitsperlong.h" 1 3 4
# 11 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/bitsperlong.h" 2 3 4
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/int-ll64.h" 2 3 4

typedef __signed__ char __s8;
typedef unsigned char __u8;
typedef __signed__ short __s16;
typedef unsigned short __u16;
typedef __signed__ int __s32;
typedef unsigned int __u32;

__extension__ typedef __signed__ long long __s64;
__extension__ typedef unsigned long long __u64;
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/types.h" 2 3 4
# 8 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/types.h" 2 3 4
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/types.h" 2 3 4

# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/posix_types.h" 1 3 4








# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/stddef.h" 1 3 4








# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/compiler_types.h" 1 3 4
# 11 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/compiler_types.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/compiler.h" 1 3 4
# 12 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/compiler_types.h" 2 3 4
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/stddef.h" 2 3 4
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/posix_types.h" 2 3 4


typedef struct {
  unsigned long fds_bits[1024 / (8 * sizeof(long))];
} __kernel_fd_set;
typedef void(* __kernel_sighandler_t) (int);
typedef int __kernel_key_t;
typedef int __kernel_mqd_t;
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/posix_types.h" 1 3 4








typedef unsigned short __kernel_old_uid_t;
typedef unsigned short __kernel_old_gid_t;

# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/posix_types.h" 1 3 4
# 11 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/posix_types.h" 3 4
typedef long __kernel_long_t;
typedef unsigned long __kernel_ulong_t;


typedef __kernel_ulong_t __kernel_ino_t;


typedef unsigned int __kernel_mode_t;


typedef int __kernel_pid_t;


typedef int __kernel_ipc_pid_t;


typedef unsigned int __kernel_uid_t;
typedef unsigned int __kernel_gid_t;


typedef __kernel_long_t __kernel_suseconds_t;


typedef int __kernel_daddr_t;


typedef unsigned int __kernel_uid32_t;
typedef unsigned int __kernel_gid32_t;






typedef unsigned int __kernel_old_dev_t;







typedef __kernel_ulong_t __kernel_size_t;
typedef __kernel_long_t __kernel_ssize_t;
typedef __kernel_long_t __kernel_ptrdiff_t;



typedef struct {
  int val[2];
} __kernel_fsid_t;

typedef __kernel_long_t __kernel_off_t;
typedef long long __kernel_loff_t;
typedef __kernel_long_t __kernel_old_time_t;
typedef __kernel_long_t __kernel_time_t;
typedef long long __kernel_time64_t;
typedef __kernel_long_t __kernel_clock_t;
typedef int __kernel_timer_t;
typedef int __kernel_clockid_t;
typedef char * __kernel_caddr_t;
typedef unsigned short __kernel_uid16_t;
typedef unsigned short __kernel_gid16_t;
# 13 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/posix_types.h" 2 3 4
# 19 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/posix_types.h" 2 3 4
# 12 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/types.h" 2 3 4

typedef __signed__ __int128 __s128 __attribute__((aligned(16)));
typedef unsigned __int128 __u128 __attribute__((aligned(16)));



typedef __u16 __le16;
typedef __u16 __be16;
typedef __u32 __le32;
typedef __u32 __be32;
typedef __u64 __le64;
typedef __u64 __be64;
typedef __u16 __sum16;
typedef __u32 __wsum;



typedef unsigned __poll_t;
# 37 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/types.h" 2 3 4


# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/pthread_types.h" 1 3 4
# 32 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/pthread_types.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/types.h" 1 3 4
# 33 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/pthread_types.h" 2 3 4

typedef struct {
  uint32_t flags;
  void* stack_base;
  size_t stack_size;
  size_t guard_size;
  int32_t sched_policy;
  int32_t sched_priority;

  char __reserved[16];

} pthread_attr_t;


typedef struct {

  int64_t __private[4];



} pthread_barrier_t;



typedef int pthread_barrierattr_t;


typedef struct {

  int32_t __private[12];



} pthread_cond_t;

typedef long pthread_condattr_t;

typedef int pthread_key_t;

typedef struct {

  int32_t __private[10];



} pthread_mutex_t;

typedef long pthread_mutexattr_t;

typedef int pthread_once_t;

typedef struct {

  int32_t __private[14];



} pthread_rwlock_t;

typedef long pthread_rwlockattr_t;


typedef struct {

  int64_t __private;



} pthread_spinlock_t;


typedef long pthread_t;
# 40 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/types.h" 2 3 4


typedef __kernel_gid32_t __gid_t;
typedef __gid_t gid_t;
typedef __kernel_uid32_t __uid_t;
typedef __uid_t uid_t;
typedef __kernel_pid_t __pid_t;
typedef __pid_t pid_t;
typedef uint32_t __id_t;
typedef __id_t id_t;

typedef unsigned long blkcnt_t;
typedef unsigned long blksize_t;
typedef __kernel_caddr_t caddr_t;
typedef __kernel_clock_t clock_t;

typedef __kernel_clockid_t __clockid_t;
typedef __clockid_t clockid_t;

typedef __kernel_daddr_t daddr_t;
typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;

typedef __kernel_mode_t __mode_t;
typedef __mode_t mode_t;

typedef __kernel_key_t __key_t;
typedef __key_t key_t;

typedef __kernel_ino_t __ino_t;
typedef __ino_t ino_t;

typedef uint64_t ino64_t;

typedef uint32_t __nlink_t;
typedef __nlink_t nlink_t;

typedef void* __timer_t;
typedef __timer_t timer_t;

typedef __kernel_suseconds_t __suseconds_t;
typedef __suseconds_t suseconds_t;


typedef uint32_t __useconds_t;
typedef __useconds_t useconds_t;





typedef uint64_t dev_t;



typedef __kernel_time_t __time_t;
typedef __time_t time_t;




typedef int64_t off_t;
typedef off_t loff_t;
typedef loff_t off64_t;
# 115 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/types.h" 3 4
typedef uint32_t __socklen_t;

typedef __socklen_t socklen_t;

typedef __builtin_va_list __va_list;
# 128 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/types.h" 3 4
typedef __kernel_ssize_t ssize_t;


typedef unsigned int uint_t;
typedef unsigned int uint;


typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;

typedef uint32_t u_int32_t;
typedef uint16_t u_int16_t;
typedef uint8_t u_int8_t;
typedef uint64_t u_int64_t;
# 39 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/fadvise.h" 1 3 4
# 40 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/falloc.h" 1 3 4
# 41 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/fcntl.h" 1 3 4








# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/fcntl.h" 1 3 4
# 13 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/fcntl.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/fcntl.h" 1 3 4








# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/flock64.h" 1 3 4
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/fcntl.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/flock.h" 1 3 4
# 60 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/flock.h" 3 4
struct flock { short l_type; short l_whence; off64_t l_start; off64_t l_len; pid_t l_pid; };
struct flock64 { short l_type; short l_whence; off64_t l_start; off64_t l_len; pid_t l_pid; };
# 11 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/fcntl.h" 2 3 4
# 110 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/fcntl.h" 3 4
struct f_owner_ex {
  int type;
  __kernel_pid_t pid;
};
# 14 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/fcntl.h" 2 3 4
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/fcntl.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/openat2.h" 1 3 4
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/openat2.h" 3 4
struct open_how {
  __u64 flags;
  __u64 mode;
  __u64 resolve;
};
# 11 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/fcntl.h" 2 3 4
# 42 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/stat.h" 1 3 4
# 42 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/stat.h" 3 4
struct statx_timestamp {
  __s64 tv_sec;
  __u32 tv_nsec;
  __s32 __reserved;
};
struct statx {
  __u32 stx_mask;
  __u32 stx_blksize;
  __u64 stx_attributes;
  __u32 stx_nlink;
  __u32 stx_uid;
  __u32 stx_gid;
  __u16 stx_mode;
  __u16 __spare0[1];
  __u64 stx_ino;
  __u64 stx_size;
  __u64 stx_blocks;
  __u64 stx_attributes_mask;
  struct statx_timestamp stx_atime;
  struct statx_timestamp stx_btime;
  struct statx_timestamp stx_ctime;
  struct statx_timestamp stx_mtime;
  __u32 stx_rdev_major;
  __u32 stx_rdev_minor;
  __u32 stx_dev_major;
  __u32 stx_dev_minor;
  __u64 stx_mnt_id;
  __u32 stx_dio_mem_align;
  __u32 stx_dio_offset_align;
  __u64 __spare3[12];
};
# 43 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/uio.h" 1 3 4
# 11 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/uio.h" 3 4
struct iovec {
  void * iov_base;
  __kernel_size_t iov_len;
};
# 44 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 2 3 4

# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/fcntl.h" 1 3 4
# 46 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/fcntl.h" 3 4
int fcntl(int __fd, int __op, ...);
# 46 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/seek_constants.h" 1 3 4
# 47 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 2 3 4


# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/lockf.h" 1 3 4
# 61 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/lockf.h" 3 4
int lockf(int __fd, int __op, off_t __length) __attribute__((__availability__(android,strict,introduced=24 )));





int lockf64(int __fd, int __op, off64_t __length) __attribute__((__availability__(android,strict,introduced=24 )));
# 50 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 2 3 4
# 112 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 3 4
int creat(const char* _Nonnull __path, mode_t __mode);

int creat64(const char* _Nonnull __path, mode_t __mode);
# 123 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 3 4
int openat(int __dir_fd, const char* _Nonnull __path, int __flags, ...);

int openat64(int __dir_fd, const char* _Nonnull __path, int __flags, ...);
# 134 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 3 4
int open(const char* _Nonnull __path, int __flags, ...);

int open64(const char* _Nonnull __path, int __flags, ...);
# 148 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 3 4
ssize_t splice(int __in_fd, off64_t* _Null_unspecified __in_offset, int __out_fd, off64_t* _Null_unspecified __out_offset, size_t __length, unsigned int __flags);
# 160 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 3 4
ssize_t tee(int __in_fd, int __out_fd, size_t __length, unsigned int __flags);
# 172 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 3 4
ssize_t vmsplice(int __fd, const struct iovec* _Nonnull __iov, size_t __count, unsigned int __flags);
# 185 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 3 4
int fallocate(int __fd, int __mode, off_t __offset, off_t __length) ;

int fallocate64(int __fd, int __mode, off64_t __offset, off64_t __length);
# 199 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 3 4
int posix_fadvise(int __fd, off_t __offset, off_t __length, int __advice) ;

int posix_fadvise64(int __fd, off64_t __offset, off64_t __length, int __advice);







int posix_fallocate(int __fd, off_t __offset, off_t __length) ;

int posix_fallocate64(int __fd, off64_t __offset, off64_t __length);
# 221 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/fcntl.h" 3 4
ssize_t readahead(int __fd, off64_t __offset, size_t __length);
# 4 "src/vtouchmerge.c" 2
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/limits.h" 1 3
# 21 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/limits.h" 3
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/limits.h" 1 3 4
# 41 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/limits.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/float.h" 1 3 4
# 42 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/limits.h" 2 3 4

# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/limits.h" 1 3 4
# 44 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/limits.h" 2 3 4
# 143 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/limits.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/posix_limits.h" 1 3 4
# 144 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/limits.h" 2 3 4
# 22 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/limits.h" 2 3
# 5 "src/vtouchmerge.c" 2
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/input.h" 1 3 4








# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/time.h" 1 3 4
# 34 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/time.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/time.h" 1 3 4








# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/timespec.h" 1 3 4
# 46 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/timespec.h" 3 4
struct timespec {

  time_t tv_sec;

  long tv_nsec;
};
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/time.h" 2 3 4

# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/time_types.h" 1 3 4








# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/time.h" 1 3 4
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/time_types.h" 2 3 4

struct __kernel_timespec {
  __kernel_time64_t tv_sec;
  long long tv_nsec;
};
struct __kernel_itimerspec {
  struct __kernel_timespec it_interval;
  struct __kernel_timespec it_value;
};
struct __kernel_old_timespec {
  __kernel_old_time_t tv_sec;
  long tv_nsec;
};
struct __kernel_sock_timeval {
  __s64 tv_sec;
  __s64 tv_usec;
};
# 12 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/time.h" 2 3 4



struct timeval {
  __kernel_old_time_t tv_sec;
  __kernel_suseconds_t tv_usec;
};
struct itimerspec {
  struct timespec it_interval;
  struct timespec it_value;
};
struct itimerval {
  struct timeval it_interval;
  struct timeval it_value;
};
struct timezone {
  int tz_minuteswest;
  int tz_dsttime;
};
# 35 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/time.h" 2 3 4


# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/select.h" 1 3 4
# 40 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/select.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/signal.h" 1 3 4
# 35 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/signal.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/sigcontext.h" 1 3 4
# 11 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/sigcontext.h" 3 4
struct sigcontext {
  __u64 fault_address;
  __u64 regs[31];
  __u64 sp;
  __u64 pc;
  __u64 pstate;
  __u8 __reserved[4096] __attribute__((__aligned__(16)));
};
struct _aarch64_ctx {
  __u32 magic;
  __u32 size;
};

struct fpsimd_context {
  struct _aarch64_ctx head;
  __u32 fpsr;
  __u32 fpcr;
  __uint128_t vregs[32];
};

struct esr_context {
  struct _aarch64_ctx head;
  __u64 esr;
};

struct extra_context {
  struct _aarch64_ctx head;
  __u64 datap;
  __u32 size;
  __u32 __reserved[3];
};

struct sve_context {
  struct _aarch64_ctx head;
  __u16 vl;
  __u16 flags;
  __u16 __reserved[2];
};


struct tpidr2_context {
  struct _aarch64_ctx head;
  __u64 tpidr2;
};

struct za_context {
  struct _aarch64_ctx head;
  __u16 vl;
  __u16 __reserved[3];
};

struct zt_context {
  struct _aarch64_ctx head;
  __u16 nregs;
  __u16 __reserved[3];
};

# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/sve_context.h" 1 3 4
# 69 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/sigcontext.h" 2 3 4
# 36 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/signal.h" 2 3 4

# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/signal_types.h" 1 3 4
# 34 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/signal_types.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/signal.h" 1 3 4








# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/signal.h" 1 3 4
# 12 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/signal.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/signal.h" 1 3 4
# 56 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/signal.h" 3 4
typedef struct {
  unsigned long sig[(64 / 64)];
} sigset_t;
typedef unsigned long old_sigset_t;
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/signal-defs.h" 1 3 4
# 45 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/signal-defs.h" 3 4
typedef void __signalfn_t(int);
typedef __signalfn_t * __sighandler_t;
typedef void __restorefn_t(void);
typedef __restorefn_t * __sigrestore_t;
# 61 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/signal.h" 2 3 4



struct __kernel_sigaction {
  __sighandler_t sa_handler;
  unsigned long sa_flags;

  __sigrestore_t sa_restorer;

  sigset_t sa_mask;
};
typedef struct sigaltstack {
  void * ss_sp;
  int ss_flags;
  __kernel_size_t ss_size;
} stack_t;
# 13 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/signal.h" 2 3 4
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/signal.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/siginfo.h" 1 3 4






# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/siginfo.h" 1 3 4
# 11 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/siginfo.h" 3 4
typedef union sigval {
  int sival_int;
  void * sival_ptr;
} sigval_t;
# 25 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/siginfo.h" 3 4
union __sifields {
  struct {
    __kernel_pid_t _pid;
    __kernel_uid32_t _uid;
  } _kill;
  struct {
    __kernel_timer_t _tid;
    int _overrun;
    sigval_t _sigval;
    int _sys_private;
  } _timer;
  struct {
    __kernel_pid_t _pid;
    __kernel_uid32_t _uid;
    sigval_t _sigval;
  } _rt;
  struct {
    __kernel_pid_t _pid;
    __kernel_uid32_t _uid;
    int _status;
    __kernel_clock_t _utime;
    __kernel_clock_t _stime;
  } _sigchld;
  struct {
    void * _addr;

    union {
      int _trapno;
      short _addr_lsb;
      struct {
        char _dummy_bnd[(__alignof__(void *) < sizeof(short) ? sizeof(short) : __alignof__(void *))];
        void * _lower;
        void * _upper;
      } _addr_bnd;
      struct {
        char _dummy_pkey[(__alignof__(void *) < sizeof(short) ? sizeof(short) : __alignof__(void *))];
        __u32 _pkey;
      } _addr_pkey;
      struct {
        unsigned long _data;
        __u32 _type;
        __u32 _flags;
      } _perf;
    };
  } _sigfault;
  struct {
    long _band;
    int _fd;
  } _sigpoll;
  struct {
    void * _call_addr;
    int _syscall;
    unsigned int _arch;
  } _sigsys;
};







typedef struct siginfo {
  union {
    struct { int si_signo; int si_errno; int si_code; union __sifields _sifields; };
    int _si_pad[128 / sizeof(int)];
  };
} siginfo_t;
# 215 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/siginfo.h" 3 4
typedef struct sigevent {
  sigval_t sigev_value;
  int sigev_signo;
  int sigev_notify;
  union {
    int _pad[((64 - (sizeof(int) * 2 + sizeof(sigval_t))) / sizeof(int))];
    int _tid;
    struct {
      void(* _function) (sigval_t);
      void * _attribute;
    } _sigev_thread;
  } _sigev_un;
} sigevent_t;
# 8 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/siginfo.h" 2 3 4
# 11 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/signal.h" 2 3 4
# 35 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/signal_types.h" 2 3 4
# 46 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/signal_types.h" 3 4
typedef int sig_atomic_t;

typedef __sighandler_t sig_t;
typedef __sighandler_t sighandler_t;





typedef sigset_t sigset64_t;
# 76 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/signal_types.h" 3 4
struct sigaction { int sa_flags; union { sighandler_t sa_handler; void (*sa_sigaction)(int, struct siginfo*, void*); }; sigset_t sa_mask; void (*sa_restorer)(void); };
struct sigaction64 { int sa_flags; union { sighandler_t sa_handler; void (*sa_sigaction)(int, struct siginfo*, void*); }; sigset_t sa_mask; void (*sa_restorer)(void); };
# 38 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/signal.h" 2 3 4



# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/ucontext.h" 1 3 4
# 33 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/ucontext.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/signal.h" 1 3 4
# 34 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/ucontext.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/user.h" 1 3 4
# 32 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/user.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stddef.h" 1 3 4
# 33 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/user.h" 2 3 4


# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/page_size.h" 1 3 4
# 36 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/user.h" 2 3 4
# 222 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/user.h" 3 4
struct user_regs_struct {
  uint64_t regs[31];
  uint64_t sp;
  uint64_t pc;
  uint64_t pstate;
};
struct user_fpsimd_struct {
  __uint128_t vregs[32];
  uint32_t fpsr;
  uint32_t fpcr;
};
# 35 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/ucontext.h" 2 3 4
# 105 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/ucontext.h" 3 4
typedef unsigned long greg_t;
typedef greg_t gregset_t[34];
typedef struct user_fpsimd_struct fpregset_t;


typedef struct sigcontext mcontext_t;

typedef struct ucontext {
  unsigned long uc_flags;
  struct ucontext *uc_link;
  stack_t uc_stack;
  union {
    sigset_t uc_sigmask;
    sigset64_t uc_sigmask64;
  };

  char __padding[128 - sizeof(sigset_t)];
  mcontext_t uc_mcontext;
} ucontext_t;
# 42 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/signal.h" 2 3 4
# 54 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/signal.h" 3 4
int __libc_current_sigrtmin(void);
int __libc_current_sigrtmax(void);

extern const char* _Nonnull const sys_siglist[(64 + 1)];
extern const char* _Nonnull const sys_signame[(64 + 1)];



int sigaction(int __signal, const struct sigaction* _Nullable __new_action, struct sigaction* _Nullable __old_action);






int siginterrupt(int __signal, int __flag);

sighandler_t _Nonnull signal(int __signal, sighandler_t _Nullable __handler);
int sigaddset(sigset_t* _Nonnull __set, int __signal);





int sigdelset(sigset_t* _Nonnull __set, int __signal);





int sigemptyset(sigset_t* _Nonnull __set);





int sigfillset(sigset_t* _Nonnull __set);





int sigismember(const sigset_t* _Nonnull __set, int __signal);






int sigpending(sigset_t* _Nonnull __set);





int sigprocmask(int __how, const sigset_t* _Nullable __new_set, sigset_t* _Nullable __old_set);





int sigsuspend(const sigset_t* _Nonnull __mask);





int sigwait(const sigset_t* _Nonnull __set, int* _Nonnull __signal);
# 145 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/signal.h" 3 4
int raise(int __signal);
int kill(pid_t __pid, int __signal);
int killpg(int __pgrp, int __signal);
int tgkill(int __tgid, int __tid, int __signal);

int sigaltstack(const stack_t* _Nullable __new_signal_stack, stack_t* _Nullable __old_signal_stack);

void psiginfo(const siginfo_t* _Nonnull __info, const char* _Nullable __msg);
void psignal(int __signal, const char* _Nullable __msg);

int pthread_kill(pthread_t __pthread, int __signal);
# 164 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/signal.h" 3 4
int pthread_sigmask(int __how, const sigset_t* _Nullable __new_set, sigset_t* _Nullable __old_set);
# 173 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/signal.h" 3 4
int sigqueue(pid_t __pid, int __signal, const union sigval __value) __attribute__((__availability__(android,strict,introduced=23 )));
int sigtimedwait(const sigset_t* _Nonnull __set, siginfo_t* _Nullable __info, const struct timespec* _Nullable __timeout) __attribute__((__availability__(android,strict,introduced=23 )));
# 184 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/signal.h" 3 4
int sigwaitinfo(const sigset_t* _Nonnull __set, siginfo_t* _Nullable __info) __attribute__((__availability__(android,strict,introduced=23 )));
# 41 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/select.h" 2 3 4



typedef unsigned long fd_mask;
# 57 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/select.h" 3 4
typedef struct {
  fd_mask fds_bits[1024/(8 * sizeof(fd_mask))];
} fd_set;
# 74 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/select.h" 3 4
void __FD_CLR_chk(int, fd_set* _Nonnull , size_t);
void __FD_SET_chk(int, fd_set* _Nonnull, size_t);
int __FD_ISSET_chk(int, const fd_set* _Nonnull, size_t);
# 98 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/select.h" 3 4
int select(int __max_fd_plus_one, fd_set* _Nullable __read_fds, fd_set* _Nullable __write_fds, fd_set* _Nullable __exception_fds, struct timeval* _Nullable __timeout);
# 109 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/select.h" 3 4
int pselect(int __max_fd_plus_one, fd_set* _Nullable __read_fds, fd_set* _Nullable __write_fds, fd_set* _Nullable __exception_fds, const struct timespec* _Nullable __timeout, const sigset_t* _Nullable __mask);
# 38 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/time.h" 2 3 4



int gettimeofday(struct timeval* _Nullable __tv, struct timezone* _Nullable __tz);
int settimeofday(const struct timeval* _Nullable __tv, const struct timezone* _Nullable __tz);

int getitimer(int __which, struct itimerval* _Nonnull __current_value);
int setitimer(int __which, const struct itimerval* _Nonnull __new_value, struct itimerval* _Nullable __old_value);

int utimes(const char* _Nonnull __path, const struct timeval __times[_Nullable 2]);
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/input.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/ioctl.h" 1 3 4
# 37 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/ioctl.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/ioctl.h" 1 3 4








# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/ioctl.h" 1 3 4






# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/ioctl.h" 1 3 4
# 8 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/ioctl.h" 2 3 4
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/ioctl.h" 2 3 4
# 38 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/ioctl.h" 2 3 4




# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/termios.h" 1 3 4
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/termios.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/termios.h" 1 3 4






# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/termios.h" 1 3 4








# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/termbits.h" 1 3 4






# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/termbits.h" 1 3 4








# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/termbits-common.h" 1 3 4








typedef unsigned char cc_t;
typedef unsigned int speed_t;
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/termbits.h" 2 3 4
typedef unsigned int tcflag_t;

struct termios {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t c_line;
  cc_t c_cc[19];
};
struct termios2 {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t c_line;
  cc_t c_cc[19];
  speed_t c_ispeed;
  speed_t c_ospeed;
};
struct ktermios {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t c_line;
  cc_t c_cc[19];
  speed_t c_ispeed;
  speed_t c_ospeed;
};
# 8 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/termbits.h" 2 3 4
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/termios.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/ioctls.h" 1 3 4






# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/ioctls.h" 1 3 4
# 8 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/ioctls.h" 2 3 4
# 11 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/termios.h" 2 3 4
struct winsize {
  unsigned short ws_row;
  unsigned short ws_col;
  unsigned short ws_xpixel;
  unsigned short ws_ypixel;
};

struct termio {
  unsigned short c_iflag;
  unsigned short c_oflag;
  unsigned short c_cflag;
  unsigned short c_lflag;
  unsigned char c_line;
  unsigned char c_cc[8];
};
# 8 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/termios.h" 2 3 4
# 11 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/termios.h" 2 3 4
# 43 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/ioctl.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/tty.h" 1 3 4
# 44 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/ioctl.h" 2 3 4

# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/ioctl.h" 1 3 4
# 43 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/ioctl.h" 3 4
int ioctl(int __fd, int __op, ...);
# 60 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/ioctl.h" 3 4
int ioctl(int __fd, unsigned __op, ...) __attribute__((__overloadable__)) __attribute__((__enable_if__(1, ""))) __asm__("ioctl");
# 46 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/ioctl.h" 2 3 4
# 11 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/input.h" 2 3 4


# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/input-event-codes.h" 1 3 4
# 14 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/input.h" 2 3 4
struct input_event {

  struct timeval time;
# 30 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/input.h" 3 4
  __u16 type;
  __u16 code;
  __s32 value;
};

struct input_id {
  __u16 bustype;
  __u16 vendor;
  __u16 product;
  __u16 version;
};
struct input_absinfo {
  __s32 value;
  __s32 minimum;
  __s32 maximum;
  __s32 fuzz;
  __s32 flat;
  __s32 resolution;
};
struct input_keymap_entry {

  __u8 flags;
  __u8 len;
  __u16 index;
  __u32 keycode;
  __u8 scancode[32];
};
struct input_mask {
  __u32 type;
  __u32 codes_size;
  __u64 codes_ptr;
};
# 125 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/input.h" 3 4
struct ff_replay {
  __u16 length;
  __u16 delay;
};
struct ff_trigger {
  __u16 button;
  __u16 interval;
};
struct ff_envelope {
  __u16 attack_length;
  __u16 attack_level;
  __u16 fade_length;
  __u16 fade_level;
};
struct ff_constant_effect {
  __s16 level;
  struct ff_envelope envelope;
};
struct ff_ramp_effect {
  __s16 start_level;
  __s16 end_level;
  struct ff_envelope envelope;
};
struct ff_condition_effect {
  __u16 right_saturation;
  __u16 left_saturation;
  __s16 right_coeff;
  __s16 left_coeff;
  __u16 deadband;
  __s16 center;
};
struct ff_periodic_effect {
  __u16 waveform;
  __u16 period;
  __s16 magnitude;
  __s16 offset;
  __u16 phase;
  struct ff_envelope envelope;
  __u32 custom_len;
  __s16 * custom_data;
};
struct ff_rumble_effect {
  __u16 strong_magnitude;
  __u16 weak_magnitude;
};
struct ff_effect {
  __u16 type;
  __s16 id;
  __u16 direction;
  struct ff_trigger trigger;
  struct ff_replay replay;
  union {
    struct ff_constant_effect constant;
    struct ff_ramp_effect ramp;
    struct ff_periodic_effect periodic;
    struct ff_condition_effect condition[2];
    struct ff_rumble_effect rumble;
  } u;
};
# 6 "src/vtouchmerge.c" 2
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/uinput.h" 1 3 4
# 13 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/uinput.h" 3 4
struct uinput_ff_upload {
  __u32 request_id;
  __s32 retval;
  struct ff_effect effect;
  struct ff_effect old;
};
struct uinput_ff_erase {
  __u32 request_id;
  __s32 retval;
  __u32 effect_id;
};



struct uinput_setup {
  struct input_id id;
  char name[80];
  __u32 ff_effects_max;
};

struct uinput_abs_setup {
  __u16 code;
  struct input_absinfo absinfo;
};
# 58 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/uinput.h" 3 4
struct uinput_user_dev {
  char name[80];
  struct input_id id;
  __u32 ff_effects_max;
  __s32 absmax[(0x3f + 1)];
  __s32 absmin[(0x3f + 1)];
  __s32 absfuzz[(0x3f + 1)];
  __s32 absflat[(0x3f + 1)];
};
# 7 "src/vtouchmerge.c" 2
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/poll.h" 1 3 4
# 37 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/poll.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/poll.h" 1 3 4






# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/poll.h" 1 3 4






# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/poll.h" 1 3 4
# 34 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/poll.h" 3 4
struct pollfd {
  int fd;
  short events;
  short revents;
};
# 8 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/poll.h" 2 3 4
# 8 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/poll.h" 2 3 4
# 38 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/poll.h" 2 3 4

# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 1 3 4
# 38 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/xlocale.h" 1 3 4
# 44 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/xlocale.h" 3 4
struct __locale_t;




typedef struct __locale_t* locale_t;
# 39 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 2 3 4




struct __timezone_t;
# 52 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
typedef struct __timezone_t* timezone_t;
# 61 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
extern char* _Nonnull tzname[];


extern int daylight;


extern long int timezone;

struct sigevent;




struct tm {

  int tm_sec;

  int tm_min;

  int tm_hour;

  int tm_mday;

  int tm_mon;

  int tm_year;

  int tm_wday;

  int tm_yday;

  int tm_isdst;

  long int tm_gmtoff;

  const char* _Nullable tm_zone;
};
# 108 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
time_t time(time_t* _Nullable __t);
# 118 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
int nanosleep(const struct timespec* _Nonnull __duration, struct timespec* _Nullable __remainder);
# 130 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
char* _Nullable asctime(const struct tm* _Nonnull __tm);
# 140 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
char* _Nullable asctime_r(const struct tm* _Nonnull __tm, char* _Nonnull __buf);







double difftime(time_t __lhs, time_t __rhs);
# 159 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
time_t mktime(struct tm* _Nonnull __tm);
# 184 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
struct tm* _Nullable localtime(const time_t* _Nonnull __t);
# 196 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
struct tm* _Nullable localtime_r(const time_t* _Nonnull __t, struct tm* _Nonnull __tm);
# 216 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
time_t timelocal(struct tm* _Nonnull __tm);
# 227 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
struct tm* _Nullable gmtime(const time_t* _Nonnull __t);
# 238 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
struct tm* _Nullable gmtime_r(const time_t* _Nonnull __t, struct tm* _Nonnull __tm);




time_t timegm(struct tm* _Nonnull __tm);







char* _Nullable strptime(const char* _Nonnull __s, const char* _Nonnull __fmt, struct tm* _Nonnull __tm) __attribute__((__format__(strftime, 2, 0)));
# 268 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
size_t strftime(char* _Nonnull __buf, size_t __n, const char* _Nonnull __fmt, const struct tm* _Nullable __tm) __attribute__((__format__(strftime, 3, 0)));




size_t strftime_l(char* _Nonnull __buf, size_t __n, const char* _Nonnull __fmt, const struct tm* _Nullable __tm, locale_t _Nonnull __l) __attribute__((__format__(strftime, 3, 0)));
# 285 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
char* _Nullable ctime(const time_t* _Nonnull __t);
# 295 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
char* _Nullable ctime_r(const time_t* _Nonnull __t, char* _Nonnull __buf);
# 309 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
void tzset(void);
# 356 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
clock_t clock(void);
# 366 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
int clock_getcpuclockid(pid_t __pid, clockid_t* _Nonnull __clock) __attribute__((__availability__(android,strict,introduced=23 )));
# 376 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
int clock_getres(clockid_t __clock, struct timespec* _Nullable __resolution);







int clock_gettime(clockid_t __clock, struct timespec* _Nonnull __ts);
# 395 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
int clock_nanosleep(clockid_t __clock, int __flags, const struct timespec* _Nonnull __time, struct timespec* _Nullable __remainder);







int clock_settime(clockid_t __clock, const struct timespec* _Nonnull __ts);







int timer_create(clockid_t __clock, struct sigevent* _Nullable __event, timer_t _Nonnull * _Nonnull __timer_ptr);







int timer_delete(timer_t _Nonnull __timer);







int timer_settime(timer_t _Nonnull __timer, int __flags, const struct itimerspec* _Nonnull __new_value, struct itimerspec* _Nullable __old_value);







int timer_gettime(timer_t _Nonnull _timer, struct itimerspec* _Nonnull __ts);
# 444 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/time.h" 3 4
int timer_getoverrun(timer_t _Nonnull __timer);
# 40 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/poll.h" 2 3 4




typedef unsigned int nfds_t;







int poll(struct pollfd* _Nullable __fds, nfds_t __count, int __timeout_ms);
# 62 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/poll.h" 3 4
int ppoll(struct pollfd* _Nullable __fds, nfds_t __count, const struct timespec* _Nullable __timeout, const sigset_t* _Nullable __mask);
# 8 "src/vtouchmerge.c" 2

# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdio.h" 1 3 4
# 44 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdio.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stdarg.h" 1 3 4
# 55 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stdarg.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/__stdarg___gnuc_va_list.h" 1 3 4
# 12 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/__stdarg___gnuc_va_list.h" 3 4
typedef __builtin_va_list __gnuc_va_list;
# 56 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stdarg.h" 2 3 4




# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/__stdarg_va_list.h" 1 3 4
# 12 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/__stdarg_va_list.h" 3 4
typedef __builtin_va_list va_list;
# 61 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stdarg.h" 2 3 4




# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/__stdarg_va_arg.h" 1 3 4
# 66 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stdarg.h" 2 3 4




# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/__stdarg___va_copy.h" 1 3 4
# 71 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stdarg.h" 2 3 4




# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/__stdarg_va_copy.h" 1 3 4
# 76 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stdarg.h" 2 3 4
# 45 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdio.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stddef.h" 1 3 4
# 46 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdio.h" 2 3 4
# 55 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdio.h" 3 4
typedef off_t fpos_t;
typedef off64_t fpos64_t;

struct __sFILE;
typedef struct __sFILE FILE;


extern FILE* _Nonnull stdin __attribute__((__availability__(android,strict,introduced=23 )));
extern FILE* _Nonnull stdout __attribute__((__availability__(android,strict,introduced=23 )));
extern FILE* _Nonnull stderr __attribute__((__availability__(android,strict,introduced=23 )));
# 106 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdio.h" 3 4
void clearerr(FILE* _Nonnull __fp);
int fclose(FILE* _Nonnull __fp);
int feof(FILE* _Nonnull __fp);
int ferror(FILE* _Nonnull __fp);
int fflush(FILE* _Nullable __fp);
int fgetc(FILE* _Nonnull __fp);
char* _Nullable fgets(char* _Nonnull __buf, int __size, FILE* _Nonnull __fp);
int fprintf(FILE* _Nonnull __fp , const char* _Nonnull __fmt, ...) __attribute__((__format__(printf, 2, 3)));
int fputc(int __ch, FILE* _Nonnull __fp);
int fputs(const char* _Nonnull __s, FILE* _Nonnull __fp);
size_t fread(void* _Nonnull __buf, size_t __size, size_t __count, FILE* _Nonnull __fp);
int fscanf(FILE* _Nonnull __fp, const char* _Nonnull __fmt, ...) __attribute__((__format__(scanf, 2, 3)));
size_t fwrite(const void* _Nonnull __buf, size_t __size, size_t __count, FILE* _Nonnull __fp);
int getc(FILE* _Nonnull __fp);
int getchar(void);
ssize_t getdelim(char* _Nullable * _Nonnull __line_ptr, size_t* _Nonnull __line_length_ptr, int __delimiter, FILE* _Nonnull __fp);
ssize_t getline(char* _Nullable * _Nonnull __line_ptr, size_t* _Nonnull __line_length_ptr, FILE* _Nonnull __fp);

void perror(const char* _Nullable __msg);
int printf(const char* _Nonnull __fmt, ...) __attribute__((__format__(printf, 1, 2)));
int putc(int __ch, FILE* _Nonnull __fp);
int putchar(int __ch);
int puts(const char* _Nonnull __s);
int remove(const char* _Nonnull __path);
void rewind(FILE* _Nonnull __fp);
int scanf(const char* _Nonnull __fmt, ...) __attribute__((__format__(scanf, 1, 2)));
void setbuf(FILE* _Nonnull __fp, char* _Nullable __buf);
int setvbuf(FILE* _Nonnull __fp, char* _Nullable __buf, int __mode, size_t __size);
int sscanf(const char* _Nonnull __s, const char* _Nonnull __fmt, ...) __attribute__((__format__(scanf, 2, 3)));
int ungetc(int __ch, FILE* _Nonnull __fp);
int vfprintf(FILE* _Nonnull __fp, const char* _Nonnull __fmt, va_list __args) __attribute__((__format__(printf, 2, 0)));
int vprintf(const char* _Nonnull __fp, va_list __args) __attribute__((__format__(printf, 1, 0)));

int dprintf(int __fd, const char* _Nonnull __fmt, ...) __attribute__((__format__(printf, 2, 3)));
int vdprintf(int __fd, const char* _Nonnull __fmt, va_list __args) __attribute__((__format__(printf, 2, 0)));





int sprintf(char* _Null_unspecified __s, const char* _Nonnull __fmt, ...)
    __attribute__((__format__(printf, 2, 3))) ;
int vsprintf(char* _Null_unspecified __s, const char* _Nonnull __fmt, va_list __args)
    __attribute__((__format__(printf, 2, 0))) ;
char* _Nullable tmpnam(char* _Nullable __s)
    __attribute__((__deprecated__("tmpnam is unsafe, use mkstemp or tmpfile instead")));

char* _Nullable tempnam(const char* _Nullable __dir, const char* _Nullable __prefix)
    __attribute__((__deprecated__("tempnam is unsafe, use mkstemp or tmpfile instead")));







int rename(const char* _Nonnull __old_path, const char* _Nonnull __new_path);







int renameat(int __old_dir_fd, const char* _Nonnull __old_path, int __new_dir_fd, const char* _Nonnull __new_path);
# 207 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdio.h" 3 4
int fseek(FILE* _Nonnull __fp, long __offset, int __whence);
long ftell(FILE* _Nonnull __fp);
# 233 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdio.h" 3 4
int fgetpos(FILE* _Nonnull __fp, fpos_t* _Nonnull __pos);
int fsetpos(FILE* _Nonnull __fp, const fpos_t* _Nonnull __pos);
int fseeko(FILE* _Nonnull __fp, off_t __offset, int __whence);
off_t ftello(FILE* _Nonnull __fp);


FILE* _Nullable funopen(const void* _Nullable __cookie,
              int (* _Null_unspecified __read_fn)(void* _Nonnull, char* _Nonnull, int),
              int (* _Null_unspecified __write_fn)(void* _Nonnull, const char* _Nonnull, int),
              fpos_t (* _Nullable __seek_fn)(void* _Nonnull, fpos_t, int),
              int (* _Nullable __close_fn)(void* _Nonnull));




int fgetpos64(FILE* _Nonnull __fp, fpos64_t* _Nonnull __pos) __attribute__((__availability__(android,strict,introduced=24 )));
int fsetpos64(FILE* _Nonnull __fp, const fpos64_t* _Nonnull __pos) __attribute__((__availability__(android,strict,introduced=24 )));
int fseeko64(FILE* _Nonnull __fp, off64_t __offset, int __whence) __attribute__((__availability__(android,strict,introduced=24 )));
off64_t ftello64(FILE* _Nonnull __fp) __attribute__((__availability__(android,strict,introduced=24 )));






FILE* _Nullable funopen64(const void* _Nullable __cookie,
                int (* _Null_unspecified __read_fn)(void* _Nonnull, char* _Nonnull, int),
                int (* _Null_unspecified __write_fn)(void* _Nonnull, const char* _Nonnull, int),
                fpos64_t (* _Nullable __seek_fn)(void* _Nonnull, fpos64_t, int),
                int (* _Nullable __close_fn)(void* _Nonnull)) __attribute__((__availability__(android,strict,introduced=24 )));




FILE* _Nullable fopen(const char* _Nonnull __path, const char* _Nonnull __mode);


FILE* _Nullable fopen64(const char* _Nonnull __path, const char* _Nonnull __mode) __attribute__((__availability__(android,strict,introduced=24 )));


FILE* _Nullable freopen(const char* _Nullable __path, const char* _Nonnull __mode, FILE* _Nonnull __fp);


FILE* _Nullable freopen64(const char* _Nullable __path, const char* _Nonnull __mode, FILE* _Nonnull __fp) __attribute__((__availability__(android,strict,introduced=24 )));


FILE* _Nullable tmpfile(void);


FILE* _Nullable tmpfile64(void) __attribute__((__availability__(android,strict,introduced=24 )));



int snprintf(char* _Null_unspecified __buf, size_t __size, const char* _Nonnull __fmt, ...) __attribute__((__format__(printf, 3, 4)));
int vfscanf(FILE* _Nonnull __fp, const char* _Nonnull __fmt, va_list __args) __attribute__((__format__(scanf, 2, 0)));
int vscanf(const char* _Nonnull __fmt , va_list __args) __attribute__((__format__(scanf, 1, 0)));
int vsnprintf(char* _Null_unspecified __buf, size_t __size, const char* _Nonnull __fmt, va_list __args) __attribute__((__format__(printf, 3, 0)));
int vsscanf(const char* _Nonnull __s, const char* _Nonnull __fmt, va_list __args) __attribute__((__format__(scanf, 2, 0)));
# 299 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdio.h" 3 4
FILE* _Nullable fdopen(int __fd, const char* _Nonnull __mode);
int fileno(FILE* _Nonnull __fp);
int pclose(FILE* _Nonnull __fp);
FILE* _Nullable popen(const char* _Nonnull __command, const char* _Nonnull __mode);
void flockfile(FILE* _Nonnull __fp);
int ftrylockfile(FILE* _Nonnull __fp);
void funlockfile(FILE* _Nonnull __fp);
int getc_unlocked(FILE* _Nonnull __fp);
int getchar_unlocked(void);
int putc_unlocked(int __ch, FILE* _Nonnull __fp);
int putchar_unlocked(int __ch);



FILE* _Nullable fmemopen(void* _Nullable __buf, size_t __size, const char* _Nonnull __mode) __attribute__((__availability__(android,strict,introduced=23 )));
FILE* _Nullable open_memstream(char* _Nonnull * _Nonnull __ptr, size_t* _Nonnull __size_ptr) __attribute__((__availability__(android,strict,introduced=23 )));




int asprintf(char* _Nullable * _Nonnull __s_ptr, const char* _Nonnull __fmt, ...) __attribute__((__format__(printf, 2, 3)));
char* _Nullable fgetln(FILE* _Nonnull __fp, size_t* _Nonnull __length_ptr);
int fpurge(FILE* _Nonnull __fp);
void setbuffer(FILE* _Nonnull __fp, char* _Nullable __buf, int __size);
int setlinebuf(FILE* _Nonnull __fp);
int vasprintf(char* _Nullable * _Nonnull __s_ptr, const char* _Nonnull __fmt, va_list __args) __attribute__((__format__(printf, 2, 0)));


void clearerr_unlocked(FILE* _Nonnull __fp) __attribute__((__availability__(android,strict,introduced=23 )));
int feof_unlocked(FILE* _Nonnull __fp) __attribute__((__availability__(android,strict,introduced=23 )));
int ferror_unlocked(FILE* _Nonnull __fp) __attribute__((__availability__(android,strict,introduced=23 )));




int fileno_unlocked(FILE* _Nonnull __fp) __attribute__((__availability__(android,strict,introduced=24 )));
# 10 "src/vtouchmerge.c" 2
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdlib.h" 1 3 4
# 32 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdlib.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/alloca.h" 1 3 4
# 33 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdlib.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/wait.h" 1 3 4
# 38 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/wait.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/wait.h" 1 3 4
# 39 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/wait.h" 2 3 4
# 34 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdlib.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/malloc.h" 1 3 4
# 29 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/malloc.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stddef.h" 1 3 4
# 30 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/malloc.h" 2 3 4
# 58 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/malloc.h" 3 4
void* _Nullable malloc(size_t __byte_count) __attribute__((__malloc__)) __attribute__((__alloc_size__(1))) __attribute__((__warn_unused_result__));
# 67 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/malloc.h" 3 4
void* _Nullable calloc(size_t __item_count, size_t __item_size) __attribute__((__malloc__)) __attribute__((__alloc_size__(1,2))) __attribute__((__warn_unused_result__));
# 77 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/malloc.h" 3 4
void* _Nullable realloc(void* _Nullable __ptr, size_t __byte_count) __attribute__((__alloc_size__(2))) __attribute__((__warn_unused_result__));
# 100 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/malloc.h" 3 4
void free(void* _Nullable __ptr);
# 111 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/malloc.h" 3 4
void* _Nullable memalign(size_t __alignment, size_t __byte_count) __attribute__((__malloc__)) __attribute__((__alloc_size__(2))) __attribute__((__warn_unused_result__));





size_t malloc_usable_size(const void* _Nullable __ptr);
# 143 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/malloc.h" 3 4
struct mallinfo { size_t arena; size_t ordblks; size_t smblks; size_t hblks; size_t hblkhd; size_t usmblks; size_t fsmblks; size_t uordblks; size_t fordblks; size_t keepcost; };







struct mallinfo mallinfo(void);




struct mallinfo2 { size_t arena; size_t ordblks; size_t smblks; size_t hblks; size_t hblkhd; size_t usmblks; size_t fsmblks; size_t uordblks; size_t fordblks; size_t keepcost; };






struct mallinfo2 mallinfo2(void) __asm__("mallinfo");
# 192 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/malloc.h" 3 4
int malloc_info(int __must_be_zero, FILE* _Nonnull __fp) __attribute__((__availability__(android,strict,introduced=23 )));
# 321 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/malloc.h" 3 4
enum HeapTaggingLevel {




  M_HEAP_TAGGING_LEVEL_NONE = 0,






  M_HEAP_TAGGING_LEVEL_TBI = 1,





  M_HEAP_TAGGING_LEVEL_ASYNC = 2,





  M_HEAP_TAGGING_LEVEL_SYNC = 3,

};
# 35 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdlib.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stddef.h" 1 3 4
# 36 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdlib.h" 2 3 4








__attribute__((__noreturn__)) void abort(void) __attribute__((__nomerge__));
__attribute__((__noreturn__)) void exit(int __status);
__attribute__((__noreturn__)) void _Exit(int __status);

int atexit(void (* _Nonnull __fn)(void));

int at_quick_exit(void (* _Nonnull __fn)(void));
void quick_exit(int __status) __attribute__((__noreturn__));

char* _Nullable getenv(const char* _Nonnull __name);
int putenv(char* _Nonnull __assignment);
int setenv(const char* _Nonnull __name, const char* _Nonnull __value, int __overwrite);
int unsetenv(const char* _Nonnull __name);
int clearenv(void);

char* _Nullable mkdtemp(char* _Nonnull __template);
char* _Nullable mktemp(char* _Nonnull __template) __attribute__((__deprecated__("mktemp is unsafe, use mkstemp or tmpfile instead")));



int mkostemp64(char* _Nonnull __template, int __flags) __attribute__((__availability__(android,strict,introduced=23 )));
int mkostemp(char* _Nonnull __template, int __flags) __attribute__((__availability__(android,strict,introduced=23 )));
int mkostemps64(char* _Nonnull __template, int __suffix_length, int __flags) __attribute__((__availability__(android,strict,introduced=23 )));
int mkostemps(char* _Nonnull __template, int __suffix_length, int __flags) __attribute__((__availability__(android,strict,introduced=23 )));


int mkstemp64(char* _Nonnull __template);
int mkstemp(char* _Nonnull __template);


int mkstemps64(char* _Nonnull __template, int __flags) __attribute__((__availability__(android,strict,introduced=23 )));


int mkstemps(char* _Nonnull __template, int __flags);

long strtol(const char* _Nonnull __s, char* _Nullable * _Nullable __end_ptr, int __base);
long long strtoll(const char* _Nonnull __s, char* _Nullable * _Nullable __end_ptr, int __base);
unsigned long strtoul(const char* _Nonnull __s, char* _Nullable * _Nullable __end_ptr, int __base);
unsigned long long strtoull(const char* _Nonnull __s, char* _Nullable * _Nullable __end_ptr, int __base);

int posix_memalign(void* _Nullable * _Nullable __memptr, size_t __alignment, size_t __size);







double strtod(const char* _Nonnull __s, char* _Nullable * _Nullable __end_ptr);
long double strtold(const char* _Nonnull __s, char* _Nullable * _Nullable __end_ptr);







int atoi(const char* _Nonnull __s) __attribute__((__pure__));
long atol(const char* _Nonnull __s) __attribute__((__pure__));
long long atoll(const char* _Nonnull __s) __attribute__((__pure__));

__attribute__((__warn_unused_result__)) char* _Nullable realpath(const char* _Nonnull __path, char* _Nullable __resolved);
# 122 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdlib.h" 3 4
int system(const char* _Nonnull __command);

void* _Nullable bsearch(const void* _Nonnull __key, const void* _Nullable __base, size_t __nmemb, size_t __size, int (* _Nonnull __comparator)(const void* _Nonnull __lhs, const void* _Nonnull __rhs));

void qsort(void* _Nullable __base, size_t __nmemb, size_t __size, int (* _Nonnull __comparator)(const void* _Nullable __lhs, const void* _Nullable __rhs));

uint32_t arc4random(void);
uint32_t arc4random_uniform(uint32_t __upper_bound);
void arc4random_buf(void* _Nonnull __buf, size_t __n);



int rand_r(unsigned int* _Nonnull __seed_ptr);

double drand48(void);
double erand48(unsigned short __xsubi[_Nonnull 3]);
long jrand48(unsigned short __xsubi[_Nonnull 3]);


void lcong48(unsigned short __param[_Nonnull 7]) __attribute__((__availability__(android,strict,introduced=23 )));


long lrand48(void);
long mrand48(void);
long nrand48(unsigned short __xsubi[_Nonnull 3]);
unsigned short* _Nonnull seed48(unsigned short __seed16v[_Nonnull 3]);
void srand48(long __seed);

char* _Nullable initstate(unsigned int __seed, char* _Nonnull __state, size_t __n);
char* _Nullable setstate(char* _Nonnull __state);

int getpt(void);
int posix_openpt(int __flags);
char* _Nullable ptsname(int __fd);
int ptsname_r(int __fd, char* _Nonnull __buf, size_t __n);
int unlockpt(int __fd);







typedef struct {
  int quot;
  int rem;
} div_t;

div_t div(int __numerator, int __denominator) __attribute__((__const__));

typedef struct {
  long int quot;
  long int rem;
} ldiv_t;

ldiv_t ldiv(long __numerator, long __denominator) __attribute__((__const__));

typedef struct {
  long long int quot;
  long long int rem;
} lldiv_t;

lldiv_t lldiv(long long __numerator, long long __denominator) __attribute__((__const__));
# 200 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdlib.h" 3 4
const char* _Nullable getprogname(void);
void setprogname(const char* _Nonnull __name);

int mblen(const char* _Nullable __s, size_t __n) ;
size_t mbstowcs(wchar_t* _Nullable __dst, const char* _Nullable __src, size_t __n);
int mbtowc(wchar_t* _Nullable __wc_ptr, const char* _Nullable __s, size_t __n);
int wctomb(char* _Nullable __dst, wchar_t __wc);

size_t wcstombs(char* _Nullable __dst, const wchar_t* _Nullable __src, size_t __n);

size_t __ctype_get_mb_cur_max(void);






int abs(int __x) __attribute__((__const__));
long labs(long __x) __attribute__((__const__));
long long llabs(long long __x) __attribute__((__const__));

float strtof(const char* _Nonnull __s, char* _Nullable * _Nullable __end_ptr);
double atof(const char* _Nonnull __s) __attribute__((__pure__));
int rand(void);
void srand(unsigned int __seed);
long random(void);
void srandom(unsigned int __seed);
int grantpt(int __fd);

long long strtoll_l(const char* _Nonnull __s, char* _Nullable * _Nullable __end_ptr, int __base, locale_t _Nonnull __l);
unsigned long long strtoull_l(const char* _Nonnull __s, char* _Nullable * _Nullable __end_ptr, int __base, locale_t _Nonnull __l);
long double strtold_l(const char* _Nonnull __s, char* _Nullable * _Nullable __end_ptr, locale_t _Nonnull __l);
# 243 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdlib.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/android/legacy_stdlib_inlines.h" 1 3 4
# 36 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/android/legacy_stdlib_inlines.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdlib.h" 1 3 4
# 37 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/android/legacy_stdlib_inlines.h" 2 3 4




static __inline__ double strtod_l(const char* _Nonnull __s, char* _Nullable * _Nullable __end_ptr, locale_t _Nonnull __l) {
  return strtod(__s, __end_ptr);
}

static __inline__ float strtof_l(const char* _Nonnull __s, char* _Nullable * _Nullable __end_ptr, locale_t _Nonnull __l) {
  return strtof(__s, __end_ptr);
}

static __inline__ long strtol_l(const char* _Nonnull __s, char* _Nullable * _Nullable __end_ptr, int __base, locale_t _Nonnull __l) {
  return strtol(__s, __end_ptr, __base);
}
# 244 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/stdlib.h" 2 3 4
# 11 "src/vtouchmerge.c" 2
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/string.h" 1 3 4
# 33 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/string.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stddef.h" 1 3 4
# 34 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/string.h" 2 3 4


# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/strcasecmp.h" 1 3 4
# 49 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/strcasecmp.h" 3 4
int strcasecmp(const char* _Nonnull __s1, const char* _Nonnull __s2) __attribute__((__pure__));






int strcasecmp_l(const char* _Nonnull __s1, const char* _Nonnull __s2, locale_t _Nonnull __l) __attribute__((__pure__)) __attribute__((__availability__(android,strict,introduced=23 )));
# 68 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/strcasecmp.h" 3 4
int strncasecmp(const char* _Nonnull __s1, const char* _Nonnull __s2, size_t __n) __attribute__((__pure__));






int strncasecmp_l(const char* _Nonnull __s1, const char* _Nonnull __s2, size_t __n, locale_t _Nonnull __l) __attribute__((__pure__)) __attribute__((__availability__(android,strict,introduced=23 )));
# 37 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/string.h" 2 3 4




# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/strings.h" 1 3 4
# 64 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/strings.h" 3 4
static __inline__ __attribute__((__always_inline__)) void __bionic_bcopy(const void* _Nonnull b1, void* _Nonnull b2, size_t len) {
  __builtin_memmove(b2, b1, len);
}



static __inline__ __attribute__((__always_inline__)) void __bionic_bzero(void* _Nonnull b, size_t len) {
  __builtin_memset(b, 0, len);
}
# 81 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/strings.h" 3 4
static __inline__ int ffs(int __n) {
  return __builtin_ffs(__n);
}
# 92 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/strings.h" 3 4
static __inline__ int ffsl(long __n) {
  return __builtin_ffsl(__n);
}
# 103 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/strings.h" 3 4
static __inline__ int ffsll(long long __n) {
  return __builtin_ffsll(__n);
}
# 42 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/string.h" 2 3 4


void* _Nullable memccpy(void* _Nonnull __dst, const void* _Nonnull __src, int __stop_char, size_t __n);
void* _Nullable memchr(const void* _Nonnull __s, int __ch, size_t __n) __attribute__((__pure__));




void* _Nullable memrchr(const void* _Nonnull __s, int __ch, size_t __n) __attribute__((__pure__));

int memcmp(const void* _Nonnull __lhs, const void* _Nonnull __rhs, size_t __n) __attribute__((__pure__));
void* _Nonnull memcpy(void* _Nonnull, const void* _Nonnull, size_t);



void* _Nonnull mempcpy(void* _Nonnull __dst, const void* _Nonnull __src, size_t __n) __attribute__((__availability__(android,strict,introduced=23 )));



void* _Nonnull memmove(void* _Nonnull __dst, const void* _Nonnull __src, size_t __n);







void* _Nonnull memset(void* _Nonnull __dst, int __ch, size_t __n);
# 84 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/string.h" 3 4
void* _Nullable memmem(const void* _Nonnull __haystack, size_t __haystack_size, const void* _Nonnull __needle, size_t __needle_size) __attribute__((__pure__));

char* _Nullable strchr(const char* _Nonnull __s, int __ch) __attribute__((__pure__));
char* _Nullable __strchr_chk(const char* _Nonnull __s, int __ch, size_t __n);
# 99 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/string.h" 3 4
char* _Nonnull strchrnul(const char* _Nonnull __s, int __ch) __attribute__((__pure__)) __attribute__((__availability__(android,strict,introduced=24 )));





char* _Nullable strrchr(const char* _Nonnull __s, int __ch) __attribute__((__pure__));
char* _Nullable __strrchr_chk(const char* _Nonnull __s, int __ch, size_t __n);

size_t strlen(const char* _Nonnull __s) __attribute__((__pure__));
size_t __strlen_chk(const char* _Nonnull __s, size_t __n);

int strcmp(const char* _Nonnull __lhs, const char* _Nonnull __rhs) __attribute__((__pure__));
char* _Nonnull stpcpy(char* _Nonnull __dst, const char* _Nonnull __src);
char* _Nonnull strcpy(char* _Nonnull __dst, const char* _Nonnull __src);
char* _Nonnull strcat(char* _Nonnull __dst, const char* _Nonnull __src);
char* _Nullable strdup(const char* _Nonnull __s);

char* _Nullable strstr(const char* _Nonnull __haystack, const char* _Nonnull __needle) __attribute__((__pure__));




char* _Nullable strcasestr(const char* _Nonnull __haystack, const char* _Nonnull __needle) __attribute__((__pure__));

char* _Nullable strtok(char* _Nullable __s, const char* _Nonnull __delimiter);
char* _Nullable strtok_r(char* _Nullable __s, const char* _Nonnull __delimiter, char* _Nonnull * _Nonnull __pos_ptr);

char* _Nonnull strerror(int __errno_value);


char* _Nonnull strerror_l(int __errno_value, locale_t _Nonnull __l) __attribute__((__availability__(android,strict,introduced=23 )));



char* _Nonnull strerror_r(int __errno_value, char* _Nullable __buf, size_t __n) __asm__("__gnu_strerror_r") __attribute__((__availability__(android,strict,introduced=23 )));
# 167 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/string.h" 3 4
const char* _Nonnull strerrordesc_np(int __errno_value) __asm__("strerror");


size_t strnlen(const char* _Nonnull __s, size_t __n) __attribute__((__pure__));
char* _Nonnull strncat(char* _Nonnull __dst, const char* _Nonnull __src, size_t __n);
char* _Nullable strndup(const char* _Nonnull __s, size_t __n);
int strncmp(const char* _Nonnull __lhs, const char* _Nonnull __rhs, size_t __n) __attribute__((__pure__));
char* _Nonnull stpncpy(char* _Nonnull __dst, const char* _Nonnull __src, size_t __n);
char* _Nonnull strncpy(char* _Nonnull __dst, const char* _Nonnull __src, size_t __n);

size_t strlcat(char* _Nonnull __dst, const char* _Nonnull __src, size_t __n);
size_t strlcpy(char* _Nonnull __dst, const char* _Nonnull __src, size_t __n);

size_t strcspn(const char* _Nonnull __s, const char* _Nonnull __reject) __attribute__((__pure__));
char* _Nullable strpbrk(const char* _Nonnull __s, const char* _Nonnull __accept) __attribute__((__pure__));
char* _Nullable strsep(char* _Nullable * _Nonnull __s_ptr, const char* _Nonnull __delimiter);
size_t strspn(const char* _Nonnull __s, const char* _Nonnull __accept);

char* _Nonnull strsignal(int __signal);

int strcoll(const char* _Nonnull __lhs, const char* _Nonnull __rhs) __attribute__((__pure__));
size_t strxfrm(char* _Null_unspecified __dst, const char* _Nonnull __src, size_t __n);

int strcoll_l(const char* _Nonnull __lhs, const char* _Nonnull __rhs, locale_t _Nonnull __l) __attribute__((__pure__));
size_t strxfrm_l(char* _Null_unspecified __dst, const char* _Nonnull __src, size_t __n, locale_t _Nonnull __l);
# 208 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/string.h" 3 4
char* _Nonnull basename(const char* _Nonnull __path) __asm__("__gnu_basename") __attribute__((__availability__(android,strict,introduced=23 )));
# 12 "src/vtouchmerge.c" 2

# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/socket.h" 1 3 4
# 36 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/socket.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/socket.h" 1 3 4






# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/socket.h" 1 3 4
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/socket.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/sockios.h" 1 3 4






# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/sockios.h" 1 3 4
# 8 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/sockios.h" 2 3 4
# 11 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/asm-generic/socket.h" 2 3 4
# 8 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/socket.h" 2 3 4
# 37 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/socket.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/sockios.h" 1 3 4
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/sockios.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/aarch64-linux-android/asm/sockios.h" 1 3 4
# 11 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/sockios.h" 2 3 4
# 38 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/socket.h" 2 3 4




# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/sockaddr_storage.h" 1 3 4
# 38 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/sockaddr_storage.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/sa_family_t.h" 1 3 4
# 39 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/sa_family_t.h" 3 4
typedef unsigned short sa_family_t;
# 39 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/sockaddr_storage.h" 2 3 4

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"






struct sockaddr_storage {
  union {
    struct {
      sa_family_t ss_family;
      char __data[128 - sizeof(sa_family_t)];
    };
    void* __align;
  };
};
#pragma clang diagnostic pop
# 43 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/socket.h" 2 3 4




struct timespec;
# 60 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/socket.h" 3 4
enum {
  SHUT_RD = 0,

  SHUT_WR,

  SHUT_RDWR

};

struct sockaddr {
  sa_family_t sa_family;
  char sa_data[14];
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"

struct linger {
  int l_onoff;
  int l_linger;
};

struct msghdr {
  void* msg_name;
  socklen_t msg_namelen;
  struct iovec* msg_iov;
  size_t msg_iovlen;
  void* msg_control;
  size_t msg_controllen;
  int msg_flags;
};

struct mmsghdr {
  struct msghdr msg_hdr;
  unsigned int msg_len;
};

struct cmsghdr {
  size_t cmsg_len;
  int cmsg_level;
  int cmsg_type;
};

#pragma clang diagnostic pop
# 115 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/socket.h" 3 4
struct cmsghdr* _Nullable __cmsg_nxthdr(struct msghdr* _Nonnull __msg, struct cmsghdr* _Nonnull __cmsg);





struct ucred {
  pid_t pid;
  uid_t uid;
  gid_t gid;
};
# 286 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/socket.h" 3 4
extern int accept(int __fd, struct sockaddr* _Nullable __addr, socklen_t* _Nullable __addr_length);
extern int accept4(int __fd, struct sockaddr* _Nullable __addr, socklen_t* _Nullable __addr_length, int __flags);
extern int bind(int __fd, const struct sockaddr* _Nonnull __addr, socklen_t __addr_length);
extern int connect(int __fd, const struct sockaddr* _Nonnull __addr, socklen_t __addr_length);
extern int getpeername(int __fd, struct sockaddr* _Nonnull __addr, socklen_t* _Nonnull __addr_length);
extern int getsockname(int __fd, struct sockaddr* _Nonnull __addr, socklen_t* _Nonnull __addr_length);
extern int getsockopt(int __fd, int __level, int __option, void* _Nullable __value, socklen_t* _Nonnull __value_length);
extern int listen(int __fd, int __backlog);
extern int recvmmsg(int __fd, struct mmsghdr* _Nonnull __msgs, unsigned int __msg_count, int __flags, const struct timespec* _Nullable __timeout);
extern ssize_t recvmsg(int __fd, struct msghdr* _Nonnull __msg, int __flags);
extern int sendmmsg(int __fd, const struct mmsghdr* _Nonnull __msgs, unsigned int __msg_count, int __flags);
extern ssize_t sendmsg(int __fd, const struct msghdr* _Nonnull __msg, int __flags);
extern int setsockopt(int __fd, int __level, int __option, const void* _Nullable __value, socklen_t __value_length);
extern int shutdown(int __fd, int __how);
extern int socket(int __af, int __type, int __protocol);
extern int socketpair(int __af, int __type, int __protocol, int __fds[_Nonnull 2]);

ssize_t recv(int __fd, void* _Nullable __buf, size_t __n, int __flags);
ssize_t send(int __fd, const void* _Nonnull __buf, size_t __n, int __flags);

extern ssize_t sendto(int __fd, const void* _Nonnull __buf, size_t __n, int __flags, const struct sockaddr* _Nullable __dst_addr, socklen_t __dst_addr_length);
extern ssize_t recvfrom(int __fd, void* _Nullable __buf, size_t __n, int __flags, struct sockaddr* _Nullable __src_addr, socklen_t* _Nullable __src_addr_length);
# 14 "src/vtouchmerge.c" 2
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/stat.h" 1 3 4
# 102 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/stat.h" 3 4
struct stat { dev_t st_dev; ino_t st_ino; mode_t st_mode; nlink_t st_nlink; uid_t st_uid; gid_t st_gid; dev_t st_rdev; unsigned long __pad1; off_t st_size; int st_blksize; int __pad2; long st_blocks; struct timespec st_atim; struct timespec st_mtim; struct timespec st_ctim; unsigned int __unused4; unsigned int __unused5; };
struct stat64 { dev_t st_dev; ino_t st_ino; mode_t st_mode; nlink_t st_nlink; uid_t st_uid; gid_t st_gid; dev_t st_rdev; unsigned long __pad1; off_t st_size; int st_blksize; int __pad2; long st_blocks; struct timespec st_atim; struct timespec st_mtim; struct timespec st_ctim; unsigned int __unused4; unsigned int __unused5; };
# 139 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/stat.h" 3 4
int chmod(const char* _Nonnull __path, mode_t __mode);
int fchmod(int __fd, mode_t __mode);
int mkdir(const char* _Nonnull __path, mode_t __mode);

int fstat(int __fd, struct stat* _Nonnull __buf);
int fstat64(int __fd, struct stat64* _Nonnull __buf);
int fstatat(int __dir_fd, const char* _Nonnull __path, struct stat* _Nonnull __buf, int __flags);
int fstatat64(int __dir_fd, const char* _Nonnull __path, struct stat64* _Nonnull __buf, int __flags);
int lstat(const char* _Nonnull __path, struct stat* _Nonnull __buf);
int lstat64(const char* _Nonnull __path, struct stat64* _Nonnull __buf);
int stat(const char* _Nonnull __path, struct stat* _Nonnull __buf);
int stat64(const char* _Nonnull __path, struct stat64* _Nonnull __buf);

int mknod(const char* _Nonnull __path, mode_t __mode, dev_t __dev);
mode_t umask(mode_t __mask);





int mkfifo(const char* _Nonnull __path, mode_t __mode);


int mkfifoat(int __dir_fd, const char* _Nonnull __path, mode_t __mode) __attribute__((__availability__(android,strict,introduced=23 )));



int fchmodat(int __dir_fd, const char* _Nonnull __path, mode_t __mode, int __flags);
int mkdirat(int __dir_fd, const char* _Nonnull __path, mode_t __mode);
int mknodat(int __dir_fd, const char* _Nonnull __path, mode_t __mode, dev_t __dev);
# 196 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/stat.h" 3 4
int utimensat(int __dir_fd, const char* _Null_unspecified __path, const struct timespec __times[_Nullable 2], int __flags);
# 208 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/stat.h" 3 4
int futimens(int __fd, const struct timespec __times[_Nullable 2]);
# 15 "src/vtouchmerge.c" 2
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/un.h" 1 3 4
# 37 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/un.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/un.h" 1 3 4








# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/socket.h" 1 3 4
# 11 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/socket.h" 3 4
typedef unsigned short __kernel_sa_family_t;
# 10 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/linux/un.h" 2 3 4

struct sockaddr_un {
  __kernel_sa_family_t sun_family;
  char sun_path[108];
};
# 38 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/sys/un.h" 2 3 4
# 16 "src/vtouchmerge.c" 2
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 1 3 4
# 31 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/18/include/stddef.h" 1 3 4
# 32 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 2 3 4





# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/getentropy.h" 1 3 4
# 38 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 2 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/getopt.h" 1 3 4
# 41 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/getopt.h" 3 4
int getopt(int __argc, char* const _Nonnull __argv[_Nullable], const char* _Nonnull __options);




extern char* _Nullable optarg;






extern int optind;






extern int opterr;




extern int optopt;
# 39 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 2 3 4




# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/sysconf.h" 1 3 4
# 347 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/sysconf.h" 3 4
long sysconf(int __name);
# 44 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 2 3 4
# 77 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
extern char* _Nullable * _Nullable environ;

__attribute__((__noreturn__)) void _exit(int __status);
# 88 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
pid_t fork(void);
# 118 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
pid_t vfork(void) __attribute__((__returns_twice__));







pid_t getpid(void);







pid_t gettid(void);

pid_t getpgid(pid_t __pid);
int setpgid(pid_t __pid, pid_t __pgid);
pid_t getppid(void);
pid_t getpgrp(void);
int setpgrp(void);
pid_t getsid(pid_t __pid);
pid_t setsid(void);

int execv(const char* _Nonnull __path, char* _Nullable const* _Nullable __argv);
int execvp(const char* _Nonnull __file, char* _Nullable const* _Nullable __argv);
int execvpe(const char* _Nonnull __file, char* _Nullable const* _Nullable __argv, char* _Nullable const* _Nullable __envp);
int execve(const char* _Nonnull __file, char* _Nullable const* _Nullable __argv, char* _Nullable const* _Nullable __envp);
int execl(const char* _Nonnull __path, const char* _Nullable __arg0, ...) __attribute__((__sentinel__));
int execlp(const char* _Nonnull __file, const char* _Nullable __arg0, ...) __attribute__((__sentinel__));
int execle(const char* _Nonnull __path, const char* _Nullable __arg0, ... )
    __attribute__((__sentinel__(1)));






int nice(int __incr);
# 169 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
int setegid(gid_t __gid);
# 180 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
int seteuid(uid_t __uid);
# 191 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
int setgid(gid_t __gid);
# 202 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
int setregid(gid_t __rgid, gid_t __egid);
# 213 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
int setresgid(gid_t __rgid, gid_t __egid, gid_t __sgid);
# 224 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
int setresuid(uid_t __ruid, uid_t __euid, uid_t __suid);
# 235 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
int setreuid(uid_t __ruid, uid_t __euid);
# 246 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
int setuid(uid_t __uid);

uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int getgroups(int __size, gid_t* _Nullable __list);
int setgroups(size_t __size, const gid_t* _Nullable __list);
int getresuid(uid_t* _Nonnull __ruid, uid_t* _Nonnull __euid, uid_t* _Nonnull __suid);
int getresgid(gid_t* _Nonnull __rgid, gid_t* _Nonnull __egid, gid_t* _Nonnull __sgid);
char* _Nullable getlogin(void);






long fpathconf(int __fd, int __name);
long pathconf(const char* _Nonnull __path, int __name);

int access(const char* _Nonnull __path, int __mode);
int faccessat(int __dirfd, const char* _Nonnull __path, int __mode, int __flags);
int link(const char* _Nonnull __old_path, const char* _Nonnull __new_path);
int linkat(int __old_dir_fd, const char* _Nonnull __old_path, int __new_dir_fd, const char* _Nonnull __new_path, int __flags);
int unlink(const char* _Nonnull __path);
int unlinkat(int __dirfd, const char* _Nonnull __path, int __flags);
int chdir(const char* _Nonnull __path);
int fchdir(int __fd);
int rmdir(const char* _Nonnull __path);
int pipe(int __fds[_Nonnull 2]);

int pipe2(int __fds[_Nonnull 2], int __flags);

int chroot(const char* _Nonnull __path);
int symlink(const char* _Nonnull __old_path, const char* _Nonnull __new_path);
int symlinkat(const char* _Nonnull __old_path, int __new_dir_fd, const char* _Nonnull __new_path);
ssize_t readlink(const char* _Nonnull __path, char* _Nonnull __buf, size_t __buf_size);
ssize_t readlinkat(int __dir_fd, const char* _Nonnull __path, char* _Nonnull __buf, size_t __buf_size);
int chown(const char* _Nonnull __path, uid_t __owner, gid_t __group);
int fchown(int __fd, uid_t __owner, gid_t __group);
int fchownat(int __dir_fd, const char* _Nonnull __path, uid_t __owner, gid_t __group, int __flags);
int lchown(const char* _Nonnull __path, uid_t __owner, gid_t __group);
char* _Nullable getcwd(char* _Nullable __buf, size_t __size);

void sync(void);
# 299 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
int close(int __fd);
# 311 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
ssize_t read(int __fd, void* _Null_unspecified __buf, size_t __count);
# 323 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
ssize_t write(int __fd, const void* _Null_unspecified __buf, size_t __count);

int dup(int __old_fd);
int dup2(int __old_fd, int __new_fd);
int dup3(int __old_fd, int __new_fd, int __flags);
int fsync(int __fd);
int fdatasync(int __fd);
# 339 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
int truncate(const char* _Nonnull __path, off_t __length);
off_t lseek(int __fd, off_t __offset, int __whence);
ssize_t pread(int __fd, void* _Nonnull __buf, size_t __count, off_t __offset);
ssize_t pwrite(int __fd, const void* _Nonnull __buf, size_t __count, off_t __offset);
int ftruncate(int __fd, off_t __length);


int truncate64(const char* _Nonnull __path, off64_t __length);
off64_t lseek64(int __fd, off64_t __offset, int __whence);
ssize_t pread64(int __fd, void* _Nonnull __buf, size_t __count, off64_t __offset);
ssize_t pwrite64(int __fd, const void* _Nonnull __buf, size_t __count, off64_t __offset);
int ftruncate64(int __fd, off64_t __length);

int pause(void);
unsigned int alarm(unsigned int __seconds);
unsigned int sleep(unsigned int __seconds);
int usleep(useconds_t __microseconds);

int gethostname(char* _Nonnull _buf, size_t __buf_size);


int sethostname(const char* _Nonnull __name, size_t __n) __attribute__((__availability__(android,strict,introduced=23 )));



int brk(void* _Nonnull __addr);
void* _Nullable sbrk(ptrdiff_t __increment);

int isatty(int __fd);
char* _Nullable ttyname(int __fd);
int ttyname_r(int __fd, char* _Nonnull __buf, size_t __buf_size);

int acct(const char* _Nullable __path);
# 380 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
int getpagesize(void) __attribute__((__const__));

long syscall(long __number, ...);

int daemon(int __no_chdir, int __no_close);
# 394 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
pid_t tcgetpgrp(int __fd);
int tcsetpgrp(int __fd, pid_t __pid);
# 459 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/android/legacy_unistd_inlines.h" 1 3 4
# 36 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/android/legacy_unistd_inlines.h" 3 4
# 1 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/swab.h" 1 3 4
# 41 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/bits/swab.h" 3 4
static __inline__ void swab(const void* _Nonnull __void_src, void* _Nonnull __void_dst, ssize_t __byte_count) {
  const uint8_t* __src = ((const uint8_t*) (__void_src));
  uint8_t* __dst = ((uint8_t*) (__void_dst));
  while (__byte_count > 1) {
    uint8_t x = *__src++;
    uint8_t y = *__src++;
    *__dst++ = y;
    *__dst++ = x;
    __byte_count -= 2;
  }
}
# 37 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/android/legacy_unistd_inlines.h" 2 3 4
# 460 "C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/../sysroot/usr/include/unistd.h" 2 3 4
# 17 "src/vtouchmerge.c" 2





static volatile sig_atomic_t stop_flag;
static int input_fd=-1,u_fd=-1,listen_fd=-1,ctl_fd=-1,hb_fd=-1;
static char sock_path[4096]="/data/local/tmp/vtouch-merge.sock";
static int vslots=10,phys_slots,total_slots,axmin[2],axmax[2],selected_slot;
struct contact { int id,x,y,down,pending_up; };
static int next_tracking_id = 1;
static struct contact phys[64],virt[32];

static void on_signal(int s){(void)s;stop_flag=1;}

static int parse_long(const char *s,long lo,long hi,int *out){char *e;long v;if(!s||!*s)return -1;(*__errno())=0;v=strtol(s,&e,10);if((*__errno())||*e||v<lo||v>hi)return -1;*out=(int)v;return 0;}
static int bit(const unsigned long *b,int n){return (int)((b[n/(8*sizeof(unsigned long))]>>(n%(8*sizeof(unsigned long))))&1UL);}
static int validate_device(const char *p,int *slots,int *xmin,int *xmax,int *ymin,int *ymax){
 unsigned long ev[(0x1f +8)/(8*sizeof(unsigned long))],abs[(0x3f +8)/(8*sizeof(unsigned long))],prop[(0x1f +8)/(8*sizeof(unsigned long))];struct input_absinfo a;int f;
 memset(ev,0,sizeof ev);memset(abs,0,sizeof abs);memset(prop,0,sizeof prop);f=open(p,00000000|00004000|02000000);if(f<0)return -1;
 if(ioctl(f,(((2U) << (((0 + 8) + 8) + 14)) | (('E') << (0 + 8)) | ((0x20 + (0)) << 0) | ((sizeof ev) << ((0 + 8) + 8))),ev)<0||ioctl(f,(((2U) << (((0 + 8) + 8) + 14)) | (('E') << (0 + 8)) | ((0x20 + (0x03)) << 0) | ((sizeof abs) << ((0 + 8) + 8))),abs)<0||ioctl(f,(((2U) << (((0 + 8) + 8) + 14)) | (('E') << (0 + 8)) | ((0x09) << 0) | ((sizeof prop) << ((0 + 8) + 8))),prop)<0||!bit(ev,0x03)||!bit(abs,0x2f)||!bit(abs,0x39)||!bit(abs,0x35)||!bit(abs,0x36)||!bit(prop,0x01)){close(f);return -1;}
 if(ioctl(f,(((2U) << (((0 + 8) + 8) + 14)) | ((('E')) << (0 + 8)) | (((0x40 + (0x2f))) << 0) | ((((sizeof(struct input_absinfo)))) << ((0 + 8) + 8))),&a)<0||a.minimum<0||a.maximum>=64){close(f);return -1;}*slots=a.maximum-a.minimum+1;
 if(ioctl(f,(((2U) << (((0 + 8) + 8) + 14)) | ((('E')) << (0 + 8)) | (((0x40 + (0x35))) << 0) | ((((sizeof(struct input_absinfo)))) << ((0 + 8) + 8))),&a)<0||a.maximum<=a.minimum){close(f);return -1;}*xmin=a.minimum;*xmax=a.maximum;
 if(ioctl(f,(((2U) << (((0 + 8) + 8) + 14)) | ((('E')) << (0 + 8)) | (((0x40 + (0x36))) << 0) | ((((sizeof(struct input_absinfo)))) << ((0 + 8) + 8))),&a)<0||a.maximum<=a.minimum){close(f);return -1;}*ymin=a.minimum;*ymax=a.maximum;close(f);return 0;
}
static int discover(char *out,size_t n){FILE *f=fopen("/proc/bus/input/devices","r");char line[512];if(!f)return -1;while(fgets(line,sizeof line,f)){if(line[0]=='H'){char *p=strstr(line,"event");int k;if(p&&sscanf(p,"event%d",&k)==1){snprintf(out,n,"/dev/input/event%d",k);if(validate_device(out,&phys_slots,&axmin[0],&axmax[0],&axmin[1],&axmax[1])==0){fclose(f);return 0;}}}}fclose(f);return -1;}
static int emit(int t,int c,int v){struct input_event e;ssize_t n;memset(&e,0,sizeof e);e.type=t;e.code=c;e.value=v;do n=write(u_fd,&e,sizeof e);while(n<0&&(*__errno())==4);return n==(ssize_t)sizeof e?0:-1;}
static int syn(void){return emit(0x00,0,0);}
static int setup_uinput(void){struct uinput_setup s;struct uinput_abs_setup a;int i;
 u_fd=open("/dev/uinput",00000001|00004000|02000000);if(u_fd<0)return -1;
 if(ioctl(u_fd,(((1U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((100)) << 0) | ((((sizeof(int)))) << ((0 + 8) + 8))),0x00)<0||ioctl(u_fd,(((1U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((100)) << 0) | ((((sizeof(int)))) << ((0 + 8) + 8))),0x01)<0||ioctl(u_fd,(((1U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((100)) << 0) | ((((sizeof(int)))) << ((0 + 8) + 8))),0x03)<0||ioctl(u_fd,(((1U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((101)) << 0) | ((((sizeof(int)))) << ((0 + 8) + 8))),0x14a)<0||ioctl(u_fd,(((1U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((101)) << 0) | ((((sizeof(int)))) << ((0 + 8) + 8))),0x145)<0||ioctl(u_fd,(((1U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((110)) << 0) | ((((sizeof(int)))) << ((0 + 8) + 8))),0x01)<0)goto fail;
 for(i=0;i<total_slots;i++)if(ioctl(u_fd,(((1U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((103)) << 0) | ((((sizeof(int)))) << ((0 + 8) + 8))),0x2f)<0)goto fail;
 if(ioctl(u_fd,(((1U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((103)) << 0) | ((((sizeof(int)))) << ((0 + 8) + 8))),0x39)<0||ioctl(u_fd,(((1U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((103)) << 0) | ((((sizeof(int)))) << ((0 + 8) + 8))),0x35)<0||ioctl(u_fd,(((1U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((103)) << 0) | ((((sizeof(int)))) << ((0 + 8) + 8))),0x36)<0||ioctl(u_fd,(((1U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((103)) << 0) | ((((sizeof(int)))) << ((0 + 8) + 8))),0x37)<0)goto fail;
 memset(&s,0,sizeof s);s.id.bustype=0x06;strncpy((char *)s.name,"vtouch-merged",80 -1);if(ioctl(u_fd,(((1U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((3)) << 0) | ((((sizeof(struct uinput_setup)))) << ((0 + 8) + 8))),&s)<0)goto fail;
 memset(&a,0,sizeof a);a.code=0x2f;a.absinfo.maximum=total_slots-1;if(ioctl(u_fd,(((1U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((4)) << 0) | ((((sizeof(struct uinput_abs_setup)))) << ((0 + 8) + 8))),&a)<0)goto fail;
 a.code=0x39;a.absinfo.maximum=65535;if(ioctl(u_fd,(((1U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((4)) << 0) | ((((sizeof(struct uinput_abs_setup)))) << ((0 + 8) + 8))),&a)<0)goto fail;
 a.code=0x35;a.absinfo.minimum=axmin[0];a.absinfo.maximum=axmax[0];if(ioctl(u_fd,(((1U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((4)) << 0) | ((((sizeof(struct uinput_abs_setup)))) << ((0 + 8) + 8))),&a)<0)goto fail;
 a.code=0x36;a.absinfo.minimum=axmin[1];a.absinfo.maximum=axmax[1];if(ioctl(u_fd,(((1U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((4)) << 0) | ((((sizeof(struct uinput_abs_setup)))) << ((0 + 8) + 8))),&a)<0)goto fail;
 a.code=0x37;a.absinfo.maximum=0x02;if(ioctl(u_fd,(((1U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((4)) << 0) | ((((sizeof(struct uinput_abs_setup)))) << ((0 + 8) + 8))),&a)<0)goto fail;if(ioctl(u_fd,(((0U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((1)) << 0) | ((0) << ((0 + 8) + 8))))<0)goto fail;return 0;
fail: ioctl(u_fd,(((0U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((2)) << 0) | ((0) << ((0 + 8) + 8))));close(u_fd);u_fd=-1;return -1;
}
static void cleanup(void){int i;if(ctl_fd>=0){close(ctl_fd);ctl_fd=-1;}if(listen_fd>=0){close(listen_fd);listen_fd=-1;unlink(sock_path);}if(input_fd>=0){



 close(input_fd);input_fd=-1;}if(u_fd>=0){ioctl(u_fd,(((0U) << (((0 + 8) + 8) + 14)) | ((('U')) << (0 + 8)) | (((2)) << 0) | ((0) << ((0 + 8) + 8))));close(u_fd);u_fd=-1;}if(hb_fd>=0){close(hb_fd);hb_fd=-1;}for(i=0;i<64;i++)memset(&phys[i],0,sizeof phys[i]);for(i=0;i<32;i++)memset(&virt[i],0,sizeof virt[i]);}
static int make_socket(void){struct sockaddr_un a;mode_t old;listen_fd=socket(1,1|02000000,0);if(listen_fd<0)return -1;memset(&a,0,sizeof a);a.sun_family=1;if(strlen(sock_path)>=sizeof a.sun_path)goto fail;unlink(sock_path);strncpy(a.sun_path,sock_path,sizeof a.sun_path-1);if(bind(listen_fd,(struct sockaddr *)&a,sizeof a)<0)goto fail;old=umask(007);if(chmod(sock_path,0660)<0){umask(old);goto fail;}umask(old);if(listen(listen_fd,1)<0)goto fail;return 0;fail:close(listen_fd);listen_fd=-1;unlink(sock_path);return -1;}
static int any_down(void){int i;for(i=0;i<phys_slots;i++)if(phys[i].down)return 1;for(i=0;i<vslots;i++)if(virt[i].down)return 1;return 0;}
static int emit_frame(void){int i;if(u_fd<0)return -1;for(i=0;i<phys_slots;i++)if(phys[i].pending_up){if(emit(0x03,0x2f,i)||emit(0x03,0x39,-1))return -1;}for(i=0;i<phys_slots;i++)if(phys[i].down){if(emit(0x03,0x2f,i)||emit(0x03,0x39,phys[i].id)||emit(0x03,0x35,phys[i].x)||emit(0x03,0x36,phys[i].y)||emit(0x03,0x37,0x00))return -1;}for(i=0;i<vslots;i++){if(virt[i].pending_up){if(emit(0x03,0x2f,phys_slots+i)||emit(0x03,0x39,-1))return -1;}else if(virt[i].down){if(emit(0x03,0x2f,phys_slots+i)||emit(0x03,0x39,virt[i].id)||emit(0x03,0x35,virt[i].x)||emit(0x03,0x36,virt[i].y)||emit(0x03,0x37,0x00))return -1;}}if(emit(0x01,0x14a,any_down())||emit(0x01,0x145,any_down())||syn())return -1;for(i=0;i<phys_slots;i++)phys[i].pending_up=0;for(i=0;i<vslots;i++)virt[i].pending_up=0;return 0;}
static void owner_reset(void){int i;for(i=0;i<vslots;i++)if(virt[i].down){virt[i].down=0;virt[i].pending_up=1;}if(emit_frame()<0)stop_flag=1;}
static int set_virtual(struct contact *state,int slot,const char *name,int x,int y){if(!strcmp(name,"down")){if(state[slot].down||state[slot].pending_up)return -1;state[slot].id=next_tracking_id++;if(next_tracking_id>65535)next_tracking_id=1;state[slot].down=1;}else if(!strcmp(name,"move")){if(!state[slot].down)return -1;}else if(!strcmp(name,"up")){if(!state[slot].down)return -1;state[slot].down=0;state[slot].pending_up=1;}else return -1;state[slot].x=x;state[slot].y=y;return 0;}
static int command(char *line,int *frame_open,int *seen){char *t,*st;int slot,x,y;static struct contact staged[32];static int staged_id;line[strcspn(line,"\r\n")]=0;t=strtok_r(line," \t",&st);if(!t)return -1;
 if(!strcmp(t,"ping")){dprintf(ctl_fd,"pong\n");return 0;}if(!strcmp(t,"res")){dprintf(ctl_fd,"res %d %d\n",axmax[0]-axmin[0]+1,axmax[1]-axmin[1]+1);return 0;}if(!strcmp(t,"reset")){owner_reset();dprintf(ctl_fd,"ok\n");return 0;}
 if(!strcmp(t,"down")||!strcmp(t,"move")||!strcmp(t,"up")){char *ss=strtok_r(((void*)0)," \t",&st),*sx=strtok_r(((void*)0)," \t",&st),*sy=strtok_r(((void*)0)," \t",&st);if(*frame_open||!ss||!sx||!sy||strtok_r(((void*)0)," \t",&st)||parse_long(ss,0,vslots-1,&slot)||parse_long(sx,axmin[0],axmax[0],&x)||parse_long(sy,axmin[1],axmax[1],&y)||set_virtual(virt,slot,t,x,y)||emit_frame()<0){dprintf(ctl_fd,"err point\n");return -1;}dprintf(ctl_fd,"ok\n");return 0;}
 if(!strcmp(t,"begin_frame")){if(*frame_open||strtok_r(((void*)0)," \t",&st)){dprintf(ctl_fd,"err frame\n");return -1;}memcpy(staged,virt,sizeof staged);staged_id=next_tracking_id;*frame_open=1;memset(seen,0,(size_t)vslots);dprintf(ctl_fd,"ok\n");return 0;}
 if(!strcmp(t,"point")){char *ss=strtok_r(((void*)0)," \t",&st),*state=strtok_r(((void*)0)," \t",&st),*sx=strtok_r(((void*)0)," \t",&st),*sy=strtok_r(((void*)0)," \t",&st);if(!*frame_open||!ss||!state||!sx||!sy||strtok_r(((void*)0)," \t",&st)||parse_long(ss,0,vslots-1,&slot)||parse_long(sx,axmin[0],axmax[0],&x)||parse_long(sy,axmin[1],axmax[1],&y)||seen[slot]||set_virtual(staged,slot,state,x,y)){dprintf(ctl_fd,"err point\n");return -1;}seen[slot]=1;dprintf(ctl_fd,"ok\n");return 0;}
 if(!strcmp(t,"end_frame")){if(!*frame_open||strtok_r(((void*)0)," \t",&st)){dprintf(ctl_fd,"err frame\n");return -1;}memcpy(virt,staged,sizeof virt);next_tracking_id=staged_id;if(emit_frame()<0){memcpy(virt,staged,sizeof virt);*frame_open=0;dprintf(ctl_fd,"err frame\n");return -1;}*frame_open=0;dprintf(ctl_fd,"ok\n");return 0;}dprintf(ctl_fd,"err unknown\n");return -1;}
static void physical_events(void){struct input_event e;ssize_t n;while((n=read(input_fd,&e,sizeof e))==(ssize_t)sizeof e){if(e.type==0x03&&e.code==0x2f){selected_slot=e.value;if(selected_slot<0||selected_slot>=phys_slots)selected_slot=0;}else if(e.type==0x03&&selected_slot<phys_slots){if(e.code==0x39){if(e.value<0){phys[selected_slot].down=0;phys[selected_slot].pending_up=1;}else{phys[selected_slot].id=e.value;phys[selected_slot].down=1;}}else if(e.code==0x35)phys[selected_slot].x=e.value;else if(e.code==0x36)phys[selected_slot].y=e.value;}if(e.type==0x00&&e.code==0&&emit_frame()<0)stop_flag=1;}if(n<0&&((*__errno())==19||(*__errno())==5))stop_flag=1;}
static void apply_args(int argc,char **argv){int i,n;for(i=1;i<argc;i++){if(!strcmp(argv[i],"-s")&&i+1<argc){strncpy(sock_path,argv[++i],sizeof sock_path-1);sock_path[sizeof sock_path-1]=0;}else if(!strcmp(argv[i],"-v")&&i+1<argc&&parse_long(argv[++i],1,32,&n)==0)vslots=n;else if(strcmp(argv[i],"-s")&&strcmp(argv[i],"-v")){fprintf(stderr,"usage: %s [-s socket] [-v virtual-slots]\n",argv[0]);}}}
int vtouchmerge_worker(int heartbeat_fd,int argc,char **argv){char dev[4096],line[512],b[256];size_t used=0;struct pollfd p[3];int frame=0,seen[32];hb_fd=heartbeat_fd;apply_args(argc,argv);memset(phys,0,sizeof phys);if(discover(dev,sizeof dev)<0)return 2;total_slots=phys_slots+vslots;if(setup_uinput()<0)return 3;input_fd=open(dev,00000000|00004000|02000000);if(input_fd<0){cleanup();return 4;}



 if(make_socket()<0){cleanup();return 6;}if(hb_fd>=0)write(hb_fd,"R",1);
 while(!stop_flag){p[0]=(struct pollfd){input_fd,0x0001|0x0010|0x0008,0};p[1]=(struct pollfd){listen_fd,0x0001,0};p[2]=(struct pollfd){ctl_fd,-1,0};if(ctl_fd>=0)p[2].events=0x0001|0x0010;int r=poll(p,ctl_fd>=0?3:2,1000);if(hb_fd>=0)write(hb_fd,"H",1);if(r<0){if((*__errno())==4)continue;break;}if(p[0].revents&0x0001)physical_events();if(p[0].revents&(0x0010|0x0008))break;if(ctl_fd<0&&(p[1].revents&0x0001)){ctl_fd=accept4(listen_fd,((void*)0),((void*)0),02000000|00004000);used=0;frame=0;}if(ctl_fd>=0&&(p[2].revents&(0x0010|0x0008))){owner_reset();close(ctl_fd);ctl_fd=-1;}if(ctl_fd>=0&&(p[2].revents&0x0001)){ssize_t n=read(ctl_fd,b,sizeof b);if(n<=0){owner_reset();close(ctl_fd);ctl_fd=-1;}else{size_t i;for(i=0;i<(size_t)n;i++){if(b[i]=='\n'){line[used]=0;command(line,&frame,seen);used=0;}else if(used<sizeof line-1&&b[i]>=32&&b[i]<=126)line[used++]=b[i];else if(used>=sizeof line-1){used=0;dprintf(ctl_fd,"err line too long\n");}}}}}cleanup();return 0;}

int main(int argc,char **argv){signal(15,on_signal);signal(2,on_signal);signal(13,(( __sighandler_t) 1));return vtouchmerge_worker(-1,argc,argv);}
