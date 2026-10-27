#include <TiltedOnlinePCH.h>

#include <Games/Primitives.h>

void NiRefObject::IncRef()
{
    InterlockedIncrement(&refCount);
}

void NiRefObject::DecRef()
{
    if (Release())
        Destroy();
}

bool NiRefObject::Release()
{
    return InterlockedDecrement(&refCount) == 0;
}

void BSRecursiveLock::Lock()
{
    const auto tid = GetCurrentThreadId();
    _mm_lfence();

    if (m_tid == tid)
    {
        InterlockedIncrement(&m_counter);
    }
    else
    {
        if (InterlockedCompareExchange(&m_counter, 1, 0))
        {
            while (true)
            {
                _mm_pause();
                if (!InterlockedCompareExchange(&m_counter, 1, 0))
                {
                    break;
                }
                std::this_thread::yield();
            }

            _mm_lfence();
        }

        m_tid = tid;
        _mm_sfence();
    }
}

void BSRecursiveLock::Unlock()
{
    _mm_lfence();
    if (m_tid == GetCurrentThreadId())
    {
        if (m_counter == 1)
        {
            m_tid = 0;
            _mm_mfence();
            InterlockedCompareExchange(&m_counter, 0, 1);
        }
        else
        {
            InterlockedDecrement(&m_counter);
        }
    }
}

void BSReadWriteLock::LockForRead() noexcept
{
    using TLock = void(BSReadWriteLock*);
    POINTER_SKYRIMSE(TLock, lockForRead, 68233);
    lockForRead.Get()(this);
}

void BSReadWriteLock::UnlockForRead() noexcept
{
    using TUnlock = void(BSReadWriteLock*);
    POINTER_SKYRIMSE(TUnlock, unlockForRead, 68239);
    unlockForRead.Get()(this);
}

void BSReadWriteLock::LockForWrite() noexcept
{
    using TLock = void(BSReadWriteLock*);
    POINTER_SKYRIMSE(TLock, lockForWrite, 68234);
    lockForWrite.Get()(this);
}

void BSReadWriteLock::UnlockForWrite() noexcept
{
    using TUnlock = void(BSReadWriteLock*);
    POINTER_SKYRIMSE(TUnlock, unlockForWrite, 68240);
    unlockForWrite.Get()(this);
}

bool BSReadWriteLock::TryLockForRead() noexcept
{
    // A writer already on this thread may take a recursive engine read lock.
    if (uiWriterThread == GetCurrentThreadId() && (uiLock & 0x80000000u))
    {
        LockForRead();
        return true;
    }
    auto* count = reinterpret_cast<volatile LONG*>(&uiLock);
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const auto current = static_cast<uint32_t>(*count);
        if (current & 0x80000000u || current == 0x7fffffffu)
            return false;
        if (static_cast<uint32_t>(InterlockedCompareExchange(count, static_cast<LONG>(current + 1),
                static_cast<LONG>(current))) == current)
            return true;
    }
    return false;
}
