/*
 * gpiod.h — 仅供主机端（Windows/MinGW）语法与语义检查用的严格桩头文件。
 *
 * 目的：libgpiod 只在 Linux 上存在。按 libgpiod 1.x 官方签名复刻工程
 * 用到的子集，使开发机也能真实编译 T113 工程自身的代码。
 *
 * 重要：本文件不是 libgpiod 的实现。交叉编译必须使用目标平台真实的
 *       <gpiod.h>。误用本桩会在链接期因缺少 gpiod_* 实现而失败。
 *       本桩不含 libgpiod 2.x 的 gpiod_line_request_* 新 API。
 */
#ifndef ACCESSTERMINAL_DEVSTUB_GPIOD_H
#define ACCESSTERMINAL_DEVSTUB_GPIOD_H

#ifdef __cplusplus
extern "C" {
#endif

struct gpiod_chip;
struct gpiod_line;

/* ---- chip ---- */
struct gpiod_chip* gpiod_chip_open_by_name(const char* name);
struct gpiod_chip* gpiod_chip_open(const char* path);
void gpiod_chip_close(struct gpiod_chip* chip);

/* ---- line ---- */
struct gpiod_line* gpiod_chip_get_line(struct gpiod_chip* chip,
                                       unsigned int offset);

int gpiod_line_request_input(struct gpiod_line* line, const char* consumer);
int gpiod_line_request_output(struct gpiod_line* line, const char* consumer,
                              int default_val);

int gpiod_line_get_value(struct gpiod_line* line);
int gpiod_line_set_value(struct gpiod_line* line, int value);
void gpiod_line_release(struct gpiod_line* line);

#ifdef __cplusplus
}
#endif

#endif /* ACCESSTERMINAL_DEVSTUB_GPIOD_H */
