#pragma once

#include <common/defines.h>

typedef struct {
    isize count;
} RefCount;

typedef struct {
    RefCount ref;
} Ref;

void init_rc(RefCount *);
void increment_rc(RefCount *);
bool decrement_rc(RefCount *);
