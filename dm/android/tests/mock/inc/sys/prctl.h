#pragma once
#define PR_SET_NAME 15
#define PR_GET_NAME 16
static inline int prctl(int op, ...) { (void)op; return 0; }
