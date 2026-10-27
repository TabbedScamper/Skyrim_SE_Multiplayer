#include <TiltedOnlinePCH.h>

#include <Events/PapyrusFunctionRegisterEvent.h>

#include <Services/PapyrusService.h>

PapyrusService::PapyrusService(entt::dispatcher& aDispatcher) noexcept
{
    m_papyrusFunctionRegisterConnection = aDispatcher.sink<PapyrusFunctionRegisterEvent>().connect<&PapyrusService::HandlePapyrusFunctionEvent>(this);
}

const void* PapyrusService::Get(const String& acNamespace, const String& acFunction) const noexcept
{
    std::lock_guard lock(m_lock);
    const auto itor = m_functions.find(acNamespace + "::" + acFunction);
    if (itor != std::end(m_functions))
        return itor->second;

    return nullptr;
}

void PapyrusService::HandlePapyrusFunctionEvent(const PapyrusFunctionRegisterEvent& acEvent) noexcept
{
    std::lock_guard lock(m_lock);
    m_functions[acEvent.Namespace + "::" + acEvent.Name] = acEvent.Function;
}

void PapyrusService::Register(const char* apNamespace, const char* apName, void* apFunction) noexcept
{
    if (!apNamespace || !apName)
        return;
    std::lock_guard lock(m_lock);
    m_functions[String(apNamespace) + "::" + apName] = apFunction;
}

size_t PapyrusService::RegisteredCount() const noexcept
{
    std::lock_guard lock(m_lock);
    return m_functions.size();
}

std::atomic<uint32_t> g_unresolvedPapyrusNatives{};

const void* ResolvePapyrusFunction(const char* apNamespace, const char* apName) noexcept
{
    const auto& service = World::Get().ctx().at<PapyrusService>();
    const void* pFunction = service.Get(apNamespace, apName);
    if (!pFunction)
    {
        // Before the VM registers its natives a miss is expected. After that it is a wrong or removed name, and the
        // call silently returns a default: say so loudly, once per name, and count it for the test gates.
        if (service.RegisteredCount() > 0)
        {
            static std::mutex s_lock;
            static std::set<std::string> s_reported;
            std::lock_guard lock(s_lock);
            if (s_reported.emplace(std::string(apNamespace) + "." + apName).second)
            {
                ++g_unresolvedPapyrusNatives;
                spdlog::error("Papyrus native {}.{} is not registered although {} natives are; every call returns a "
                    "default value", apNamespace, apName, service.RegisteredCount());
            }
        }
        else
        {
            static std::atomic<uint32_t> s_logs{};
            if (s_logs.fetch_add(1, std::memory_order_relaxed) < 50)
                spdlog::warn("Papyrus native {}.{} not registered yet; the call is skipped", apNamespace, apName);
        }
    }
    return pFunction;
}
