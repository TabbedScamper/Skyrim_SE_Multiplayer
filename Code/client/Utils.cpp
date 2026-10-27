#include <World.h>
#include <Components.h>
#include <Actor.h>
#include <Utils.h>
#include <unordered_map>
#include <vector>
#include <array>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>

namespace Utils
{

std::optional<uint32_t> GetServerId(entt::entity aEntity) noexcept
{
    const auto* pLocalComponent = World::Get().try_get<LocalComponent>(aEntity);
    const auto* pRemoteComponent = World::Get().try_get<RemoteComponent>(aEntity);
    const auto* pObjectComponent = World::Get().try_get<ObjectComponent>(aEntity);

    uint32_t serverId = -1;
    if (pLocalComponent)
        serverId = pLocalComponent->Id;
    else if (pRemoteComponent)
        serverId = pRemoteComponent->Id;
    else if (pObjectComponent)
        serverId = pObjectComponent->Id;
    else
    {
        const auto* pFormIdComponent = World::Get().try_get<FormIdComponent>(aEntity);
        spdlog::debug("{}: This entity has neither a local or remote component: {:X}, form id: {:X}", __FUNCTION__, to_integral(aEntity), pFormIdComponent ? pFormIdComponent->Id : 0);
        return std::nullopt;
    }

    return {serverId};
}

// BEGIN ENTITY LOOKUP INDEX
namespace
{
thread_local uint32_t s_dispatchDepth{};
std::atomic<uint32_t> s_dispatchActive{}, s_dispatchOverlaps{}, s_dispatchReentries{}, s_indexOutside{};
std::atomic<uint32_t> s_indexOutsideReads{}, s_indexOutsideWrites{};
std::atomic<uint64_t> s_dispatchCalls{}, s_dispatchNextLog{};
std::atomic_bool s_dispatchStarted{};

// Native inventory/equip hooks can run on job threads. Publish ownership through
// component signals so those hooks never touch an entt pool.
class OwnershipTokenIndex;
std::atomic<OwnershipTokenIndex*> s_ownershipTokens{};

class OwnershipTokenIndex
{
public:
    explicit OwnershipTokenIndex(entt::registry& aRegistry)
    {
        // Mirror pool order on the writer lane. Duplicate form IDs must retain
        // the baseline view's first-match policy without reading EnTT in a hook.
        m_forms.Size = aRegistry.view<FormIdComponent>().size();
        m_locals.Size = aRegistry.view<LocalComponent>().size();
        m_remotes.Size = aRegistry.view<RemoteComponent>().size();
        m_connections[0] = aRegistry.on_construct<FormIdComponent>().connect<&OwnershipTokenIndex::Refresh>(this);
        m_connections[1] = aRegistry.on_update<FormIdComponent>().connect<&OwnershipTokenIndex::Refresh>(this);
        m_connections[2] = aRegistry.on_destroy<FormIdComponent>().connect<&OwnershipTokenIndex::RemoveAll>(this);
        m_connections[3] = aRegistry.on_construct<LocalComponent>().connect<&OwnershipTokenIndex::Refresh>(this);
        m_connections[4] = aRegistry.on_update<LocalComponent>().connect<&OwnershipTokenIndex::Refresh>(this);
        m_connections[5] = aRegistry.on_destroy<LocalComponent>().connect<&OwnershipTokenIndex::RemoveLocal>(this);
        m_connections[6] = aRegistry.on_construct<RemoteComponent>().connect<&OwnershipTokenIndex::Refresh>(this);
        m_connections[7] = aRegistry.on_update<RemoteComponent>().connect<&OwnershipTokenIndex::Refresh>(this);
        m_connections[8] = aRegistry.on_destroy<RemoteComponent>().connect<&OwnershipTokenIndex::RemoveRemote>(this);
        for (auto entity : aRegistry.view<FormIdComponent>())
            Refresh(aRegistry, entity);
        s_ownershipTokens.store(this, std::memory_order_release);
    }

    ~OwnershipTokenIndex()
    {
        s_ownershipTokens.store(nullptr, std::memory_order_release);
    }

    std::optional<ActorOwnershipToken> Find(uint32_t aFormId, bool aRemote) const
    {
        std::lock_guard lock(m_mutex);
        const auto& tokens = aRemote ? m_remote : m_local;
        const auto it = tokens.find(aFormId);
        if (it == tokens.end())
            return std::nullopt;
        const auto& owners = aRemote ? m_remotes : m_locals;
        // EnTT 3.10 selects the smallest pool (FormId wins ties), then walks
        // it backwards. Only candidates for this form are examined.
        const auto& order = owners.Size < m_forms.Size ? owners : m_forms;
        const ActorOwnershipToken* selected = nullptr;
        size_t selectedRank{};
        for (const auto& [entity, token] : it->second)
        {
            const auto rank = order.Positions.find(entity);
            // An incomplete publication cannot establish baseline selection.
            // Reject it instead of throwing through a noexcept hook accessor.
            if (rank == order.Positions.end())
                return std::nullopt;
            if (!selected || rank->second > selectedRank)
            {
                selected = &token;
                selectedRank = rank->second;
            }
        }
        return selected && selected->OwnershipEpoch ? std::optional<ActorOwnershipToken>{*selected} : std::nullopt;
    }

private:
    struct PoolOrder
    {
        size_t Size{};
        std::unordered_map<entt::entity, size_t> Positions;

        template <class T> void Publish(entt::registry& aRegistry, entt::entity aEntity)
        {
            const auto& pool = aRegistry.view<T>().handle();
            Size = pool.size();
            if (pool.contains(aEntity))
                Positions[aEntity] = pool.index(aEntity);
        }

        template <class T> void Remove(entt::registry& aRegistry, entt::entity aEntity)
        {
            // on_destroy runs before swap-and-pop. Publish the resulting rank
            // of the last element, including when it belongs to another key.
            const auto& pool = aRegistry.view<T>().handle();
            const auto last = pool.data()[pool.size() - 1];
            Positions[last] = pool.index(aEntity);
            Positions.erase(aEntity);
            Size = pool.size() - 1;
        }
    };

    void Erase(entt::entity aEntity, bool aRemote)
    {
        auto& keys = aRemote ? m_remoteKeys : m_localKeys;
        auto& tokens = aRemote ? m_remote : m_local;
        if (const auto it = keys.find(aEntity); it != keys.end())
        {
            if (auto entries = tokens.find(it->second); entries != tokens.end())
            {
                std::erase_if(entries->second, [aEntity](const auto& entry) { return entry.first == aEntity; });
                if (entries->second.empty())
                    tokens.erase(entries);
            }
            keys.erase(it);
        }
    }

    void Refresh(entt::registry& aRegistry, entt::entity aEntity)
    {
        const auto* form = aRegistry.try_get<FormIdComponent>(aEntity);
        const auto* local = aRegistry.try_get<LocalComponent>(aEntity);
        const auto* remote = aRegistry.try_get<RemoteComponent>(aEntity);
        std::lock_guard lock(m_mutex);
        m_forms.Publish<FormIdComponent>(aRegistry, aEntity);
        m_locals.Publish<LocalComponent>(aRegistry, aEntity);
        m_remotes.Publish<RemoteComponent>(aRegistry, aEntity);
        Erase(aEntity, false);
        Erase(aEntity, true);
        if (form && local)
        {
            m_local[form->Id].emplace_back(aEntity, ActorOwnershipToken{local->Id, local->OwnershipEpoch});
            m_localKeys[aEntity] = form->Id;
        }
        if (form && remote)
        {
            m_remote[form->Id].emplace_back(aEntity, ActorOwnershipToken{remote->Id, remote->OwnershipEpoch});
            m_remoteKeys[aEntity] = form->Id;
        }
    }

    void RemoveAll(entt::registry& aRegistry, entt::entity aEntity)
    {
        std::lock_guard lock(m_mutex);
        m_forms.Remove<FormIdComponent>(aRegistry, aEntity);
        Erase(aEntity, false);
        Erase(aEntity, true);
    }
    void RemoveLocal(entt::registry& aRegistry, entt::entity aEntity)
    {
        std::lock_guard lock(m_mutex);
        m_locals.Remove<LocalComponent>(aRegistry, aEntity);
        Erase(aEntity, false);
    }
    void RemoveRemote(entt::registry& aRegistry, entt::entity aEntity)
    {
        std::lock_guard lock(m_mutex);
        m_remotes.Remove<RemoteComponent>(aRegistry, aEntity);
        Erase(aEntity, true);
    }

    mutable std::mutex m_mutex;
    PoolOrder m_forms, m_locals, m_remotes;
    std::unordered_map<uint32_t, std::vector<std::pair<entt::entity, ActorOwnershipToken>>> m_local, m_remote;
    std::unordered_map<entt::entity, uint32_t> m_localKeys, m_remoteKeys;
    std::array<entt::scoped_connection, 9> m_connections;
};

void ObserveDispatchStack(const char* aKind)
{
    // Only called for the first eight overlaps/reentries. A changing worker
    // thread alone is not a race; retain the native caller for actual overlap.
    void* stack[12]{};
    const auto frames = CaptureStackBackTrace(1, 12, stack, nullptr);
    spdlog::info("Scale lane: dispatch-stack kind={} thread={} depth={} active={} frames={} pcs=[{},{},{},{},{},{},{},{},{},{},{},{}]",
        aKind, GetCurrentThreadId(), s_dispatchDepth,
        s_dispatchActive.load(std::memory_order_relaxed), frames,
        stack[0], stack[1], stack[2], stack[3], stack[4], stack[5],
        stack[6], stack[7], stack[8], stack[9], stack[10], stack[11]);
}

void ObserveIndexAccess(const char* aOperation, uint64_t aKey, const char* aIndex, bool aMutation = false)
{
    if (!s_dispatchDepth && s_dispatchStarted.load(std::memory_order_relaxed))
    {
        s_indexOutside.fetch_add(1, std::memory_order_relaxed);
        auto& counter = aMutation ? s_indexOutsideWrites : s_indexOutsideReads;
        const auto count = counter.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 8)
        {
            spdlog::info("Scale lane: outside operation={} key={} thread={} active={} mutation={} index={}",
                aOperation, aKey, GetCurrentThreadId(),
                s_dispatchActive.load(std::memory_order_relaxed), aMutation, aIndex);
            if (aMutation)
            {
                // First eight signal callbacks only. Read traffic must not use
                // up the writer evidence budget. No native/registry mutation.
                void* stack[12]{};
                const auto frames = CaptureStackBackTrace(1, 12, stack, nullptr);
                spdlog::info("Scale lane: writer-stack operation={} entity={} thread={} frames={} pcs=[{},{},{},{},{},{},{},{},{},{},{},{}]",
                    aOperation, aKey, GetCurrentThreadId(), frames,
                    stack[0], stack[1], stack[2], stack[3], stack[4], stack[5],
                    stack[6], stack[7], stack[8], stack[9], stack[10], stack[11]);
            }
        }
    }
}

// Intended only for synchronous World dispatch, never native actor hooks.
// The mutex protects containers, not registry pool access or the returned
// entity's lifetime. Lane diagnostics observe violations without hiding them.
template <class... TComponents>
class EntityViewIndex
{
public:
    using KeyFunction = uint64_t (*)(entt::registry&, entt::entity);
    EntityViewIndex(entt::registry& aRegistry, KeyFunction aKey)
        : m_registry(aRegistry), m_key(aKey)
    {
        size_t connection{};
        ((m_connections[connection++] = aRegistry.on_construct<TComponents>().template connect<&EntityViewIndex::Update>(this),
          m_connections[connection++] = aRegistry.on_update<TComponents>().template connect<&EntityViewIndex::Update>(this),
          m_connections[connection++] = aRegistry.on_destroy<TComponents>().template connect<&EntityViewIndex::Remove>(this)), ...);
        for (auto entity : aRegistry.view<TComponents...>())
            Update(aRegistry, entity);
    }

    std::optional<entt::entity> Find(uint64_t aKey) const
    {
        ObserveIndexAccess("find", aKey, __FUNCSIG__);
        std::lock_guard lock(m_lock);
        const auto it = m_byKey.find(aKey);
        if (it == m_byKey.end())
            return std::nullopt;
        if (it->second.size() == 1)
            return it->second.front();
        // EnTT 3.10 walks the smallest pool backwards. Compare only this key's
        // candidates, including pool swaps and changes of leading pool.
        const auto& pool = m_registry.view<TComponents...>().handle();
        return *std::max_element(it->second.begin(), it->second.end(),
            [&](auto left, auto right) { return pool.index(left) < pool.index(right); });
    }

private:
    void Remove(entt::registry&, entt::entity aEntity)
    {
        ObserveIndexAccess("remove", entt::to_integral(aEntity), __FUNCSIG__, true);
        std::lock_guard lock(m_lock);
        Erase(aEntity);
    }

    // Called only with m_lock held. Keep signal diagnostics outside the lock,
    // including the erase performed while updating an existing key.
    void Erase(entt::entity aEntity)
    {
        const auto previous = m_keys.find(aEntity);
        if (previous == m_keys.end())
            return;
        auto it = m_byKey.find(previous->second);
        std::erase(it->second, aEntity);
        if (it->second.empty())
            m_byKey.erase(it);
        m_keys.erase(previous);
    }

    void Update(entt::registry& aRegistry, entt::entity aEntity)
    {
        ObserveIndexAccess("update", entt::to_integral(aEntity), __FUNCSIG__, true);
        std::lock_guard lock(m_lock);
        if (!aRegistry.all_of<TComponents...>(aEntity))
            return;
        const auto key = m_key(aRegistry, aEntity);
        const auto previous = m_keys.find(aEntity);
        if (previous != m_keys.end() && previous->second == key)
            return;
        Erase(aEntity);
        m_byKey[key].push_back(aEntity);
        m_keys.emplace(aEntity, key);
    }

    entt::registry& m_registry;
    KeyFunction m_key;
    mutable std::mutex m_lock;
    std::unordered_map<uint64_t, std::vector<entt::entity>> m_byKey;
    std::unordered_map<entt::entity, uint64_t> m_keys;
    std::array<entt::scoped_connection, 3 * sizeof...(TComponents)> m_connections;
};

template <class TComponent>
uint64_t IdentityKey(entt::registry& aRegistry, entt::entity aEntity)
{
    return aRegistry.get<TComponent>(aEntity).Id;
}
struct ClientEntityIndex
{
    explicit ClientEntityIndex(entt::registry& aRegistry)
        : LocalForms(aRegistry, IdentityKey<FormIdComponent>),
          RemoteActors(aRegistry, IdentityKey<RemoteComponent>), MovementIds(aRegistry, IdentityKey<RemoteComponent>),
          LocalIds(aRegistry, IdentityKey<LocalComponent>),
          RemoteIds(aRegistry, IdentityKey<RemoteComponent>),
          ObjectIds(aRegistry, IdentityKey<ObjectComponent>),
          RemoteForms(aRegistry, IdentityKey<FormIdComponent>), Ownership(aRegistry) {}

    EntityViewIndex<FormIdComponent, LocalComponent> LocalForms;
    EntityViewIndex<RemoteComponent, FormIdComponent> RemoteActors;
    EntityViewIndex<RemoteComponent, InterpolationComponent, RemoteAnimationComponent> MovementIds;
    EntityViewIndex<LocalComponent> LocalIds;
    EntityViewIndex<RemoteComponent> RemoteIds;
    EntityViewIndex<ObjectComponent> ObjectIds;
    EntityViewIndex<FormIdComponent, RemoteComponent> RemoteForms;
    OwnershipTokenIndex Ownership;
};

ClientEntityIndex& EntityIndex(entt::registry& aRegistry)
{
    if (auto* index = aRegistry.ctx().find<ClientEntityIndex>())
        return *index;
    return aRegistry.ctx().emplace<ClientEntityIndex>(aRegistry);
}
}
// END ENTITY LOOKUP INDEX

EntityDispatchScope::EntityDispatchScope() noexcept
{
    s_dispatchStarted.store(true, std::memory_order_relaxed);
    if (s_dispatchDepth++)
    {
        const auto count = s_dispatchReentries.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 8)
        {
            spdlog::info("Scale lane: reentry thread={} depth={} count={}",
                GetCurrentThreadId(), s_dispatchDepth, count);
            ObserveDispatchStack("reentry");
        }
    }
    else if (s_dispatchActive.fetch_add(1, std::memory_order_relaxed))
    {
        const auto count = s_dispatchOverlaps.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 8)
        {
            spdlog::info("Scale lane: overlap thread={} active={} count={}",
                GetCurrentThreadId(),
                s_dispatchActive.load(std::memory_order_relaxed), count);
            ObserveDispatchStack("overlap");
        }
    }
}

EntityDispatchScope::~EntityDispatchScope() noexcept
{
    if (--s_dispatchDepth)
        return;
    s_dispatchActive.fetch_sub(1, std::memory_order_relaxed);
    s_dispatchCalls.fetch_add(1, std::memory_order_relaxed);
    const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    auto next = s_dispatchNextLog.load(std::memory_order_relaxed);
    if (now >= next && s_dispatchNextLog.compare_exchange_strong(next, now + 5000, std::memory_order_relaxed))
        spdlog::info("Scale lane: summary calls={} overlaps={} reentries={} outside={} outside-reads={} outside-writes={} (cumulative)",
            s_dispatchCalls.load(std::memory_order_relaxed), s_dispatchOverlaps.load(std::memory_order_relaxed),
            s_dispatchReentries.load(std::memory_order_relaxed), s_indexOutside.load(std::memory_order_relaxed),
            s_indexOutsideReads.load(std::memory_order_relaxed), s_indexOutsideWrites.load(std::memory_order_relaxed));
}

void InitializeEntityIndex(entt::registry& aRegistry)
{
    EntityIndex(aRegistry);
}

std::optional<entt::entity> FindEntityByServerIdOnRunner(uint32_t aServerId) noexcept
{
    auto& index = EntityIndex(World::Get());
    // Match the generic lookup even for duplicate IDs and entities without a
    // FormIdComponent. A remote-only actor view would change that precedence.
    if (const auto local = index.LocalIds.Find(aServerId))
        return local;
    if (const auto remote = index.RemoteIds.Find(aServerId))
        return remote;
    return index.ObjectIds.Find(aServerId);
}

std::optional<ActorOwnershipToken> GetLocalOwnershipTokenOnRunner(uint32_t aFormId) noexcept
{
    const auto entity = EntityIndex(World::Get()).LocalForms.Find(aFormId);
    if (!entity)
        return std::nullopt;
    const auto& local = World::Get().get<LocalComponent>(*entity);
    return local.OwnershipEpoch ? std::optional<ActorOwnershipToken>{{local.Id, local.OwnershipEpoch}} : std::nullopt;
}

std::optional<ActorOwnershipToken> GetRemoteOwnershipTokenOnRunner(uint32_t aFormId) noexcept
{
    const auto entity = EntityIndex(World::Get()).RemoteForms.Find(aFormId);
    if (!entity)
        return std::nullopt;
    const auto& remote = World::Get().get<RemoteComponent>(*entity);
    return remote.OwnershipEpoch ? std::optional<ActorOwnershipToken>{{remote.Id, remote.OwnershipEpoch}} : std::nullopt;
}

std::optional<entt::entity> FindEntityByServerId(const uint32_t aServerId) noexcept
{
    const auto localView = World::Get().view<LocalComponent>();
    const auto localIt = std::find_if(
        localView.begin(), localView.end(),
        [localView, aServerId](const entt::entity aEntity) { return localView.get<LocalComponent>(aEntity).Id == aServerId; });
    if (localIt != localView.end())
        return *localIt;

    const auto remoteView = World::Get().view<RemoteComponent>();
    const auto remoteIt = std::find_if(
        remoteView.begin(), remoteView.end(),
        [remoteView, aServerId](const entt::entity aEntity) { return remoteView.get<RemoteComponent>(aEntity).Id == aServerId; });
    if (remoteIt != remoteView.end())
        return *remoteIt;

    const auto objectView = World::Get().view<ObjectComponent>();
    const auto objectIt = std::find_if(
        objectView.begin(), objectView.end(),
        [objectView, aServerId](const entt::entity aEntity) { return objectView.get<ObjectComponent>(aEntity).Id == aServerId; });
    if (objectIt != objectView.end())
        return *objectIt;

    return std::nullopt;
}

std::optional<entt::entity> FindMovementEntityByServerId(uint32_t aServerId) noexcept
{
    // The packet has no ownership epoch. The baseline selects the first entity
    // in this exact view matching the server ID, including duplicate IDs. A
    // second token lookup adds no validation or lifetime protection.
    return EntityIndex(World::Get()).MovementIds.Find(aServerId);
}

std::optional<entt::entity> FindLocalEntityByFormId(uint32_t aFormId) noexcept
{
    return EntityIndex(World::Get()).LocalForms.Find(aFormId);
}

std::optional<entt::entity> FindRemoteActorByServerId(uint32_t aServerId) noexcept
{
    return EntityIndex(World::Get()).RemoteActors.Find(aServerId);
}

std::optional<ActorOwnershipToken> GetLocalOwnershipToken(const uint32_t aFormId) noexcept
{
    auto* tokens = s_ownershipTokens.load(std::memory_order_acquire);
    return tokens ? tokens->Find(aFormId, false) : std::nullopt;
}

std::optional<ActorOwnershipToken> GetRemoteOwnershipToken(const uint32_t aFormId) noexcept
{
    auto* tokens = s_ownershipTokens.load(std::memory_order_acquire);
    return tokens ? tokens->Find(aFormId, true) : std::nullopt;
}

void ShowHudMessage(const TiltedPhoques::String& acMessage)
{
    using TShowHudMessage = void(const char*, const char*, bool);

    POINTER_SKYRIMSE(TShowHudMessage, s_showHudMessage, 52933);

    s_showHudMessage(acMessage.c_str(), nullptr, false);
}

} // namespace Utils
