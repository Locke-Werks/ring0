#include "stack.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Phase 7 commit 40: stub. The next commit wires up SuspendThread +
   StackWalk64 + dbghelp here. */

int stack_capture(unsigned pid)
{
    (void)pid;
    return 0;
}

void stack_clear(void) {}
