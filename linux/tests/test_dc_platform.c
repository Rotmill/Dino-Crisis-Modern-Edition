#include "dc_platform.h"

#include <stdio.h>

int main(void)
{
    LARGE_INTEGER frequency, first, second;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart != 1000000000ll) return 1;
    if (!QueryPerformanceCounter(&first)) return 2;
    Sleep(2);
    if (!QueryPerformanceCounter(&second) || second.QuadPart <= first.QuadPart) return 3;
    if (GetTickCount64() == 0 || GetCurrentThreadId() == 0) return 4;
    puts("native platform test: ok");
    return 0;
}
