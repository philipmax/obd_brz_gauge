#pragma once
/* Keep real host file types but prevent the real driver from touching /sdcard. */
#include_next <sys/stat.h>
int sd_test_stat(const char *path, struct stat *info);
int sd_test_mkdir(const char *path, mode_t mode);
#define stat(path, info) sd_test_stat((path), (info))
#define mkdir(path, mode) sd_test_mkdir((path), (mode))
