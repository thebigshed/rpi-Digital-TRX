#pragma once
// The Linux GPIO character device, version 2 (kernel 5.10 and later): edge events with pull-ups
// and debounce. The Pi's kernel has it, but the Bootlin toolchain's kernel headers (5.4) are older
// than it, so the definitions are given here when <linux/gpio.h> lacks them. They are the
// kernel's stable user-space ABI, copied from include/uapi/linux/gpio.h
// (GPL-2.0-only WITH Linux-syscall-note).
#include <linux/gpio.h>
#include <linux/types.h>
#include <sys/ioctl.h>

#ifndef GPIO_MAX_NAME_SIZE
#define GPIO_MAX_NAME_SIZE 32
#endif

#ifndef GPIO_V2_LINES_MAX
#define GPIO_V2_LINES_MAX 64
#define GPIO_V2_LINE_NUM_ATTRS_MAX 10

enum gpio_v2_line_flag {
    GPIO_V2_LINE_FLAG_USED = 1ULL << 0,
    GPIO_V2_LINE_FLAG_ACTIVE_LOW = 1ULL << 1,
    GPIO_V2_LINE_FLAG_INPUT = 1ULL << 2,
    GPIO_V2_LINE_FLAG_OUTPUT = 1ULL << 3,
    GPIO_V2_LINE_FLAG_EDGE_RISING = 1ULL << 4,
    GPIO_V2_LINE_FLAG_EDGE_FALLING = 1ULL << 5,
    GPIO_V2_LINE_FLAG_OPEN_DRAIN = 1ULL << 6,
    GPIO_V2_LINE_FLAG_OPEN_SOURCE = 1ULL << 7,
    GPIO_V2_LINE_FLAG_BIAS_PULL_UP = 1ULL << 8,
    GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN = 1ULL << 9,
    GPIO_V2_LINE_FLAG_BIAS_DISABLED = 1ULL << 10,
};

struct gpio_v2_line_values {
    __aligned_u64 bits;
    __aligned_u64 mask;
};

enum gpio_v2_line_attr_id {
    GPIO_V2_LINE_ATTR_ID_FLAGS = 1,
    GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES = 2,
    GPIO_V2_LINE_ATTR_ID_DEBOUNCE = 3,
};

struct gpio_v2_line_attribute {
    __u32 id;
    __u32 padding;
    union {
        __aligned_u64 flags;
        __aligned_u64 values;
        __u32 debounce_period_us;
    };
};

struct gpio_v2_line_config_attribute {
    struct gpio_v2_line_attribute attr;
    __aligned_u64 mask;
};

struct gpio_v2_line_config {
    __aligned_u64 flags;
    __u32 num_attrs;
    __u32 padding[5];
    struct gpio_v2_line_config_attribute attrs[GPIO_V2_LINE_NUM_ATTRS_MAX];
};

struct gpio_v2_line_request {
    __u32 offsets[GPIO_V2_LINES_MAX];
    char consumer[GPIO_MAX_NAME_SIZE];
    struct gpio_v2_line_config config;
    __u32 num_lines;
    __u32 event_buffer_size;
    __u32 padding[5];
    __s32 fd;
};

enum gpio_v2_line_event_id {
    GPIO_V2_LINE_EVENT_RISING_EDGE = 1,
    GPIO_V2_LINE_EVENT_FALLING_EDGE = 2,
};

struct gpio_v2_line_event {
    __aligned_u64 timestamp_ns;
    __u32 id;
    __u32 offset;
    __u32 seqno;
    __u32 line_seqno;
    __u32 padding[6];
};

#define GPIO_V2_GET_LINE_IOCTL _IOWR(0xB4, 0x07, struct gpio_v2_line_request)
#define GPIO_V2_LINE_GET_VALUES_IOCTL _IOWR(0xB4, 0x0E, struct gpio_v2_line_values)
#define GPIO_V2_LINE_SET_VALUES_IOCTL _IOWR(0xB4, 0x0F, struct gpio_v2_line_values)
#endif
