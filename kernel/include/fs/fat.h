#pragma once

#include <kernel/compiler.h>

struct blockdev;
struct filesystem;

struct filesystem *fat_mount(struct blockdev *dev);
