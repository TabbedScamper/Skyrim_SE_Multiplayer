#include <TiltedOnlinePCH.h>
#include <Services/SaidOnceService.h>

#include <Forms/TESTopicInfo.h>
#include <Messages/RequestSaidOnce.h>
#include <Messages/NotifySaidOnce.h>
#include <World.h>

#include <deque>
#include <mutex>

namespace
{
using TPostTopicInfo = void(TESTopicInfo*, int32_t, void*, bool);
TPostTopicInfo* s_postTopicInfo{};
std::mutex s_queueMutex;
std::deque<uint32_t> s_localInfos;
std::deque<GameId> s_remoteInfos;
constexpr size_t kMaxInfosPerFrame = 32;

void HookPostTopicInfo(TESTopicInfo* apInfo, int32_t aType, void* apSpeaker, bool aFlag)
{
    const bool wasSaid = apInfo && apInfo->saidOnce;
    s_postTopicInfo(apInfo, aType, apSpeaker, aFlag);
    if (apInfo && !wasSaid && apInfo->saidOnce && !(apInfo->infoFlags & 0x10))
    {
        std::lock_guard lock(s_queueMutex);
        s_localInfos.push_back(apInfo->formID);
    }
}

TiltedPhoques::Initializer s_initSaidOnce([]()
{
    POINTER_SKYRIMSE(TPostTopicInfo, postTopicInfo, 25616);
    s_postTopicInfo = postTopicInfo.Get();
    TP_HOOK(&s_postTopicInfo, HookPostTopicInfo);
});
}

SaidOnceService::SaidOnceService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld), m_transport(aTransport)
    , m_notifyConnection(aDispatcher.sink<NotifySaidOnce>().connect<&SaidOnceService::OnNotify>(this))
{
}

void SaidOnceService::OnNotify(const NotifySaidOnce& aMessage) noexcept
{
    std::lock_guard lock(s_queueMutex);
    s_remoteInfos.push_back(aMessage.Info);
}

void SaidOnceService::OnMainFrame() noexcept
{
    if (!m_transport.IsConnected())
    {
        std::lock_guard lock(s_queueMutex);
        s_localInfos.clear();
        s_remoteInfos.clear();
        return;
    }

    std::deque<uint32_t> local;
    std::deque<GameId> remote;
    {
        std::lock_guard lock(s_queueMutex);
        for (size_t i = 0; i < kMaxInfosPerFrame && !s_localInfos.empty(); ++i)
        {
            local.push_back(s_localInfos.front());
            s_localInfos.pop_front();
        }
        for (size_t i = 0; i < kMaxInfosPerFrame && !s_remoteInfos.empty(); ++i)
        {
            remote.push_back(s_remoteInfos.front());
            s_remoteInfos.pop_front();
        }
    }

    for (const auto formId : local)
    {
        RequestSaidOnce request;
        if (m_world.GetModSystem().GetServerModId(formId, request.Info))
            m_transport.Send(request);
    }
    for (const auto& id : remote)
    {
        const auto formId = m_world.GetModSystem().GetGameId(id);
        auto* form = TESForm::GetById(formId);
        if (!form || form->formType != FormType::Info)
            continue;
        auto* info = static_cast<TESTopicInfo*>(form);
        if (info->saidOnce || (info->infoFlags & 0x10))
            continue;
        info->saidOnce = true;
        info->MarkChanged(0x80000000);
    }
}
