#pragma once
#include <Games/Events.h>
#include <memory>
#include <string>
struct World;
struct MenuOpenCloseEvent;
struct NotifyHarness;
struct NotifyDoorVote;
struct UpdateEvent;
struct HarnessService : BSTEventSink<TESQuestStageEvent>, BSTEventSink<TESCellFullyLoadedEvent>,
    BSTEventSink<TESCellAttachDetachEvent>, BSTEventSink<TESSceneEvent>,
    BSTEventSink<MenuOpenCloseEvent>, BSTEventSink<TESTriggerEnterEvent>, BSTEventSink<TESPackageEvent>
{
    explicit HarnessService(World&);
    ~HarnessService();
    static void MainThreadUpdate() noexcept;
    static bool OwnsDriver() noexcept;
    static bool IsEnabled() noexcept;
    // TriggerGate delivers a remote player's trip straight to the script sinks, bypassing the
    // trigger event dispatcher: report it so a party-trigger step sees the party's entry.
    static void OnPartyTriggerDelivered(uint32_t aTriggerFormId, uint32_t aRemoteFormId) noexcept;
    std::string Command(const std::string&);
    BSTEventResult OnEvent(const TESQuestStageEvent*, const EventDispatcher<TESQuestStageEvent>*) override;
    BSTEventResult OnEvent(const TESCellFullyLoadedEvent*, const EventDispatcher<TESCellFullyLoadedEvent>*) override;
    BSTEventResult OnEvent(const TESCellAttachDetachEvent*, const EventDispatcher<TESCellAttachDetachEvent>*) override;
    BSTEventResult OnEvent(const TESSceneEvent*, const EventDispatcher<TESSceneEvent>*) override;
    BSTEventResult OnEvent(const MenuOpenCloseEvent*, const EventDispatcher<MenuOpenCloseEvent>*) override;
    BSTEventResult OnEvent(const TESTriggerEnterEvent*, const EventDispatcher<TESTriggerEnterEvent>*) override;
    BSTEventResult OnEvent(const TESPackageEvent*, const EventDispatcher<TESPackageEvent>*) override;
private:
    void Tick();
    void OnUpdate(const UpdateEvent&);
    void OnMessage(const NotifyHarness&);
    void OnDoor(const NotifyDoorVote&);
    struct Impl;
    std::unique_ptr<Impl> m;
};
