// Inspired by
// https://github.com/NVIDIA/DALI/blob/main/include/dali/core/static_switch.h
// and https://github.com/pytorch/pytorch/blob/master/aten/src/ATen/Dispatch.h

#pragma once

/// @param COND       - a boolean expression to switch by
/// @param CONST_NAME - a name given for the constexpr bool variable.
/// @param ...       - code to execute for true and false
///
/// Usage:
/// ```
/// BOOL_SWITCH(flag, BoolConst, [&] {
///     some_function<BoolConst>(...);
/// });
/// ```
#define BOOL_SWITCH(COND, CONST_NAME, ...)                                                                             \
    [&] {                                                                                                              \
        if (COND) {                                                                                                    \
            constexpr static bool CONST_NAME = true;                                                                   \
            return __VA_ARGS__();                                                                                      \
        }                                                                                                              \
        else {                                                                                                         \
            constexpr static bool CONST_NAME = false;                                                                  \
            return __VA_ARGS__();                                                                                      \
        }                                                                                                              \
    }()

#define FP16_SWITCH(COND, ...)                                                                                         \
    [&] {                                                                                                              \
        if (COND) {                                                                                                    \
            using elem_type = cutlass::half_t;                                                                         \
            return __VA_ARGS__();                                                                                      \
        }                                                                                                              \
        else {                                                                                                         \
            using elem_type = cutlass::bfloat16_t;                                                                     \
            return __VA_ARGS__();                                                                                      \
        }                                                                                                              \
    }()

#if 0
#define FWD_HEADDIM_SWITCH(HEADDIM, ...)                                                                               \
    [&] {                                                                                                              \
        if (HEADDIM <= 32) {                                                                                           \
            constexpr static int kHeadDim = 32;                                                                        \
            return __VA_ARGS__();                                                                                      \
        }                                                                                                              \
        else if (HEADDIM <= 64) {                                                                                      \
            constexpr static int kHeadDim = 64;                                                                        \
            return __VA_ARGS__();                                                                                      \
        }                                                                                                              \
        else if (HEADDIM <= 128) {                                                                                     \
            constexpr static int kHeadDim = 128;                                                                       \
            return __VA_ARGS__();                                                                                      \
        }                                                                                                              \
        else if (HEADDIM <= 256) {                                                                                     \
            constexpr static int kHeadDim = 256;                                                                       \
            return __VA_ARGS__();                                                                                      \
        }                                                                                                              \
    }()
#elif 1
#define FWD_HEADDIM_SWITCH(HEADDIM, ...)                                                                               \
    [&] {                                                                                                              \
        if (HEADDIM <= 128) {                                                                                          \
            constexpr static int kHeadDim = 128;                                                                       \
            return __VA_ARGS__();                                                                                      \
        }                                                                                                              \
        else if (HEADDIM <= 256) {                                                                                     \
            constexpr static int kHeadDim = 256;                                                                       \
            return __VA_ARGS__();                                                                                      \
        }                                                                                                              \
    }()
#else
#define FWD_HEADDIM_SWITCH(HEADDIM, ...)                                                                               \
    [&] {                                                                                                              \
        constexpr static int kHeadDim = 128;                                                                           \
        return __VA_ARGS__();                                                                                          \
    }()
#endif
