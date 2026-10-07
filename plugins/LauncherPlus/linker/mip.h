#pragma once
// Parallel replacement for the linker's mip-level builder (linker+0x7897A0), rows on worker threads with the stock
// operation order (doubles), so the bytes are identical. (parallel_mip).
// void MipLevel(const float* src, const float* weight, int srcW, int srcH, int levelW, int levelH, int mode, float* out)
#include <windows.h>
#include <emmintrin.h>

namespace mip
{
    struct Constants { double half, pi, halfAgain; float one; };   // read from the linker image at install

    struct Job
    {
        const float* src;
        const float* weight;
        int srcW, xstep, ystep, mode, cols;
        float* out;
        double threshold;
        volatile LONG nextRow;
        int rows;
        unsigned int mxcsr;
    };

    inline void Row(const Job& j, int k)
    {
        float* o = j.out + 4 * (size_t)k * j.cols;
        const long long rowBase = (long long)k * j.ystep;
        for (int c = 0; c < j.cols; ++c, o += 4)
        {
            double r = 0.0, g = 0.0, b = 0.0, a = 0.0, sum = 0.0;
            const long long colBase = (long long)c * j.xstep;
            for (int dy = 0; dy < j.ystep; ++dy)
            {
                const long long index = (long long)j.srcW * (rowBase + dy) + colBase;
                const float* p = j.src + 4 * index;
                const float* w = j.weight + index;
                for (int dx = 0; dx < j.xstep; ++dx, p += 4, ++w)
                {
                    if (j.mode == 4)
                    {
                        // Running max in R, G, B, A take the larger of that max and this sample (stock quirk).
                        const double R = (double)p[0];
                        if (!(r > R)) r = R;
                        const double G = (double)p[1];
                        g = (r > G) ? r : G;
                        const double B = (double)p[2];
                        b = (r > B) ? r : B;
                        const double A = (double)p[3];
                        a = (r > A) ? r : A;
                    }
                    else
                    {
                        const double wd = (double)*w;
                        r += (double)p[0] * wd;
                        g += (double)p[1] * wd;
                        b += (double)p[2] * wd;
                        a += (double)p[3] * wd;
                        sum += wd;
                    }
                }
            }
            if (sum > 0.0) { r /= sum; g /= sum; b /= sum; a /= sum; }
            o[0] = (float)r;
            o[1] = (float)g;
            o[2] = (float)b;
            o[3] = j.mode == 5 ? (sum >= j.threshold ? 1.0f : 0.0f) : (float)a;
        }
    }

    inline void Drain(Job& j)
    {
        const unsigned int saved = _mm_getcsr();
        _mm_setcsr(j.mxcsr);   // the caller's rounding, FTZ and DAZ
        for (;;)
        {
            const int k = (int)InterlockedIncrement(&j.nextRow) - 1;
            if (k >= j.rows) break;
            Row(j, k);
        }
        _mm_setcsr(saved);
    }

    // Fixed pool of worker threads, the caller works too. Threads die with the process.
    class Pool
    {
    public:
        void Start(int workers)
        {
            InitializeSRWLock(&lock_);
            InitializeConditionVariable(&wake_);
            InitializeConditionVariable(&done_);
            for (int i = 0; i < workers; ++i)
            {
                HANDLE t = CreateThread(nullptr, 0, &Pool::Main, this, 0, nullptr);
                if (t) { CloseHandle(t); ++workers_; }
            }
        }
        int Workers() const { return workers_; }

        void Run(Job& j)
        {
            AcquireSRWLockExclusive(&lock_);
            job_ = &j;
            busy_ = workers_;
            ++generation_;
            ReleaseSRWLockExclusive(&lock_);
            WakeAllConditionVariable(&wake_);
            Drain(j);
            AcquireSRWLockExclusive(&lock_);
            while (busy_ > 0) SleepConditionVariableSRW(&done_, &lock_, INFINITE, 0);
            job_ = nullptr;
            ReleaseSRWLockExclusive(&lock_);
        }

    private:
        static DWORD WINAPI Main(void* self)
        {
            Pool& p = *static_cast<Pool*>(self);
            unsigned long long seen = 0;
            for (;;)
            {
                AcquireSRWLockExclusive(&p.lock_);
                while (p.generation_ == seen) SleepConditionVariableSRW(&p.wake_, &p.lock_, INFINITE, 0);
                seen = p.generation_;
                Job* j = p.job_;
                ReleaseSRWLockExclusive(&p.lock_);
                if (j) Drain(*j);
                AcquireSRWLockExclusive(&p.lock_);
                if (--p.busy_ == 0) WakeConditionVariable(&p.done_);
                ReleaseSRWLockExclusive(&p.lock_);
            }
        }

        SRWLOCK lock_;
        CONDITION_VARIABLE wake_, done_;
        Job* job_ = nullptr;
        unsigned long long generation_ = 0;
        int busy_ = 0;
        int workers_ = 0;
    };
}
