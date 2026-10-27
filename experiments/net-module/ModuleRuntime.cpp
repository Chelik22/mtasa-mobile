// Keep the native lock representation identical to SharedUtil.Misc.hpp: AddRef
// may be inlined in a different MTA DLL and call that DLL's nonvirtual Lock().
// GPL-3.0 implementation follows upstream Shared/sdk/SharedUtil.Misc.hpp.
#ifdef WIN32
    #include <windows.h>
#else
    #include <pthread.h>
#endif
#include "MtaBitStream.h"

namespace SharedUtil
{
    CCriticalSection CRefCountable::ms_CS;
#ifdef WIN32
    CCriticalSection::CCriticalSection() : m_pCriticalSection(new CRITICAL_SECTION)
    {
        InitializeCriticalSection(static_cast<CRITICAL_SECTION*>(m_pCriticalSection));
    }
    CCriticalSection::~CCriticalSection()
    {
        DeleteCriticalSection(static_cast<CRITICAL_SECTION*>(m_pCriticalSection));
        delete static_cast<CRITICAL_SECTION*>(m_pCriticalSection);
    }
    void CCriticalSection::Lock()
    {
        EnterCriticalSection(static_cast<CRITICAL_SECTION*>(m_pCriticalSection));
    }
    void CCriticalSection::Unlock()
    {
        LeaveCriticalSection(static_cast<CRITICAL_SECTION*>(m_pCriticalSection));
    }
#else
    CCriticalSection::CCriticalSection() : m_pCriticalSection(new pthread_mutex_t)
    {
        pthread_mutex_init(static_cast<pthread_mutex_t*>(m_pCriticalSection), nullptr);
    }
    CCriticalSection::~CCriticalSection()
    {
        pthread_mutex_destroy(static_cast<pthread_mutex_t*>(m_pCriticalSection));
        delete static_cast<pthread_mutex_t*>(m_pCriticalSection);
    }
    void CCriticalSection::Lock()
    {
        pthread_mutex_lock(static_cast<pthread_mutex_t*>(m_pCriticalSection));
    }
    void CCriticalSection::Unlock()
    {
        pthread_mutex_unlock(static_cast<pthread_mutex_t*>(m_pCriticalSection));
    }
#endif
}  // namespace SharedUtil
