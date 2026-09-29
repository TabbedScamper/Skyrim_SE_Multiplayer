#include <TiltedOnlinePCH.h>

#include <Events/PapyrusFunctionRegisterEvent.h>

#include <Services/PapyrusService.h>

PapyrusService::PapyrusService(entt::dispatcher& aDispatcher) noexcept
{
    m_papyrusFunctionRegisterConnection = aDispatcher.sink<PapyrusFunctionRegisterEvent>().connect<&PapyrusService::HandlePapyrusFunctionEvent>(this);
}

const void* PapyrusService::Get(const String& acNamespace, const String& acFunction) const noexcept
{
    const auto itor = m_functions.find(acNamespace + "::" + acFunction);
    if (itor != std::end(m_functions))
        return itor->second;

    return nullptr;
}

void PapyrusService::HandlePapyrusFunctionEvent(const PapyrusFunctionRegisterEvent& acEvent) noexcept
{
    m_functions[acEvent.Namespace + "::" + acEvent.Name] = acEvent.Function;
}

const void* ResolvePapyrusFunction(const char* apNamespace, const char* apName) noexcept
{
    const void* pFunction = World::Get().ctx().at<PapyrusService>().Get(apNamespace, apName);
    if (!pFunction)
    {
        static std::atomic<uint32_t> s_logs{};
        if (s_logs.fetch_add(1, std::memory_order_relaxed) < 50)
            spdlog::warn("Papyrus native {}.{} not registered yet; the call is skipped", apNamespace, apName);
    }
    return pFunction;
}
