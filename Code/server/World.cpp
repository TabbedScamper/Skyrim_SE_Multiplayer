#include <World.h>
#include <Components.h>

#include <Services/CharacterService.h>
#include <Services/ObjectService.h>
#include <Services/QuestService.h>
#include <Services/ServerListService.h>
#include <Services/ActorValueService.h>
#include <Services/AdminService.h>
#include <Services/InventoryService.h>
#include <Services/MagicService.h>
#include <Services/OverlayService.h>
#include <Services/CommandService.h>
#include <Services/StringCacheService.h>
#include <Services/CombatService.h>
#include <Services/WeatherService.h>
#include <Services/ScriptService.h>
#include <Services/MapService.h>
#include <Services/DiagnosticsService.h>
#include <Services/CameraService.h>
#include <Services/SceneTimelineService.h>

#include <CampaignLedger.h>
#include <console/Setting.h>

#include <es_loader/ESLoader.h>

namespace
{
Console::StringSetting sCampaignDatabasePath{
    "Campaign:sDatabasePath", "Path to the durable shared-campaign database", "Data/SkyrimSEMultiplayer.campaign.sqlite3", Console::SettingsFlags::kLocked};
Console::StringSetting sCampaignModManifestPath{
    "Campaign:sModManifestPath", "Path to the pinned campaign plugin manifest", "Data/SkyrimSEMultiplayer.plugins.manifest", Console::SettingsFlags::kLocked};
} // namespace

World::World()
{
    m_pCampaignLedger = TiltedPhoques::MakeUnique<Campaign::Ledger>(sCampaignDatabasePath.value());
    const auto campaign = m_pCampaignLedger->GetMetadata();
    spdlog::info("Loaded shared campaign {} at revision {} (authority epoch {})", campaign.CampaignId, campaign.Revision, campaign.AuthorityEpoch);

    m_spAdminService = std::make_shared<AdminService>(*this, m_dispatcher);
    spdlog::default_logger()->sinks().push_back(std::static_pointer_cast<spdlog::sinks::sink>(m_spAdminService));

    ctx().emplace<CharacterService>(*this, m_dispatcher);
    ctx().emplace<PlayerService>(*this, m_dispatcher);
    ctx().emplace<CalendarService>(*this, m_dispatcher);
    ctx().emplace<ObjectService>(*this, m_dispatcher);
    auto& modsComponent = ctx().emplace<ModsComponent>(sCampaignModManifestPath.value());
    ctx().emplace<ServerListService>(*this, m_dispatcher);
    ctx().emplace<QuestService>(*this, m_dispatcher);
    ctx().emplace<PartyService>(*this, m_dispatcher);
    ctx().emplace<CameraService>(*this, m_dispatcher);
    ctx().emplace<SceneTimelineService>(*this, m_dispatcher);
    ctx().emplace<ActorValueService>(*this, m_dispatcher);
    ctx().emplace<InventoryService>(*this, m_dispatcher);
    ctx().emplace<MagicService>(*this, m_dispatcher);
    ctx().emplace<OverlayService>(*this, m_dispatcher);
    ctx().emplace<CommandService>(*this, m_dispatcher);
    ctx().emplace<StringCacheService>(*this, m_dispatcher);
    ctx().emplace<CombatService>(*this, m_dispatcher);
    ctx().emplace<WeatherService>(*this, m_dispatcher);
    ctx().emplace<MapService>(*this, m_dispatcher);
    ctx().emplace<DiagnosticsService>(*this, m_dispatcher);

    ESLoader::ESLoader loader;
    // emplace loaded mods into modscomponent.
    m_recordCollection = loader.BuildRecordCollection();
    for (const auto& it : loader.GetLoadOrder())
    {
        modsComponent.AddServerMod(it);
    }
    if (modsComponent.LoadPinnedManifest())
        spdlog::info("Loaded pinned campaign plugin manifest from {}", sCampaignModManifestPath.value());
    else if (modsComponent.IsManifestPinned())
        spdlog::error("Pinned campaign plugin manifest is invalid and must be repaired: {}", sCampaignModManifestPath.value());

    // late initialize the ScriptService to ensure all components are valid
    m_pScriptService = TiltedPhoques::MakeUnique<ScriptService>(*this, m_dispatcher);
}

World::~World()
{
    m_pScriptService.reset();
}
