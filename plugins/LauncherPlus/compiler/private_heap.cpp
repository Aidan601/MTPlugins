// Private heap for every allocation of this DLL: cod2map's output depends on the order of process-heap
// addresses (BSP node split ties), so its allocation pattern must not change.
#include <windows.h>
#include <new>
#include <cstdlib>

namespace
{
    HANDLE Heap()
    {
        static HANDLE heap = HeapCreate(0, 1 << 20, 0);
        return heap;
    }

    void* Allocate(size_t size)
    {
        if (void* p = HeapAlloc(Heap(), 0, size ? size : 1)) return p;
        throw std::bad_alloc();
    }
}

void* operator new(size_t size) { return Allocate(size); }
void* operator new[](size_t size) { return Allocate(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept { return HeapAlloc(Heap(), 0, size ? size : 1); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return HeapAlloc(Heap(), 0, size ? size : 1); }
void operator delete(void* p) noexcept { if (p) HeapFree(Heap(), 0, p); }
void operator delete[](void* p) noexcept { if (p) HeapFree(Heap(), 0, p); }
void operator delete(void* p, size_t) noexcept { if (p) HeapFree(Heap(), 0, p); }
void operator delete[](void* p, size_t) noexcept { if (p) HeapFree(Heap(), 0, p); }
