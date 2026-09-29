#include <TiltedOnlinePCH.h>

#include <Systems/FaceGenSystem.h>
#include <Services/CreatorTogether.h>

#include <Games/References.h>
#include <PlayerCharacter.h>

#include <Forms/BGSHeadPart.h>
#include <Forms/TESNPC.h>

#include <Components.h>
#include <World.h>

#include <Structs/Tints.h>

#include <Games/Skyrim/NetImmerse/NiTriBasedGeom.h>
#include <Games/Skyrim/NetImmerse/NiRenderedTexture.h>
#include <Games/Skyrim/NetImmerse/BSShaderProperty.h>
#include <Games/Skyrim/NetImmerse/BSLightingShaderProperty.h>
#include <Games/Skyrim/NetImmerse/BSMaskedShaderMaterial.h>
#include <Games/Memory.h>
#include <Renderer.h>
#include <d3d11.h>

__declspec(noinline) NiTriBasedGeom* GetHeadTriBasedGeom(Actor* apActor, uint32_t aPartType)
{
    using TGetObjectByName = NiAVObject*(BSFaceGenNiNode*, const char**, char);
    POINTER_SKYRIMSE(TGetObjectByName, GetObjectByName, 76207);

    BSFaceGenNiNode* pFaceNode = apActor->GetFaceGenNiNode();
    TESNPC* pActorBase = Cast<TESNPC>(apActor->baseForm);

    if (pFaceNode && pActorBase)
    {
        BGSHeadPart* pFacePart = pActorBase->GetHeadPart(aPartType);

        if (pFacePart)
        {
            NiAVObject* pHeadNode = GetObjectByName(pFaceNode, &pFacePart->name.data, 1);

            if (pHeadNode)
            {
                NiTriBasedGeom* pGeometry = pHeadNode->CastToNiTriBasedGeom();

                if (pGeometry)
                {
                    return pGeometry;
                }
            }
        }
    }

    return nullptr;
}

struct TextureHolder;
TP_THIS_FUNCTION(TCreateResourceView, Ni2DBuffer*, TextureHolder, uint32_t, uint32_t);

using TCreateTexture = NiRenderedTexture*(__fastcall)(BSFixedString& aName);
using TCreateTints = void(__fastcall)(const GameArray<TintMask*>& acTints, NiRenderedTexture* apTexture);

std::atomic<bool> FaceGenSystem::RestoreLocalTints{true};

uint64_t FaceGenSystem::HashTintTexture(void* apMaterial) noexcept
{
    // Ni2DBuffer +0x10 is the D3D11 shader resource view (0x14100DD60 / 0x14100EF50 create it and call its
    // GetResource). Copy the texture to a cached staging texture and hash the rows.
    auto* pMaterial = static_cast<BSMaskedShaderMaterial*>(apMaterial);
    auto* pTexture = pMaterial ? static_cast<NiRenderedTexture*>(pMaterial->renderedTexture.object) : nullptr;
    auto* pBuffer = pTexture ? reinterpret_cast<uint8_t*>(pTexture->buffer) : nullptr;
    auto* pView = pBuffer ? *reinterpret_cast<ID3D11ShaderResourceView**>(pBuffer + 0x10) : nullptr;
    auto* pRenderer = BGSRenderer::Get();
    if (!pView || !pRenderer || !pRenderer->pD3dDevice || !pRenderer->pD3dContext)
        return 0;
    ID3D11Resource* pResource = nullptr;
    pView->GetResource(&pResource);
    ID3D11Texture2D* pSource = nullptr;
    if (!pResource || FAILED(pResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pSource))))
    {
        if (pResource)
            pResource->Release();
        return 0;
    }
    pResource->Release();
    D3D11_TEXTURE2D_DESC desc{};
    pSource->GetDesc(&desc);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc = {1, 0};
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* pStaging = nullptr;
    uint64_t hash = 0;
    if (SUCCEEDED(pRenderer->pD3dDevice->CreateTexture2D(&desc, nullptr, &pStaging)))
    {
        pRenderer->pD3dContext->CopySubresourceRegion(pStaging, 0, 0, 0, 0, pSource, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(pRenderer->pD3dContext->Map(pStaging, 0, D3D11_MAP_READ, 0, &mapped)))
        {
            hash = 1469598103934665603ull;
            const auto* pRow = static_cast<const uint8_t*>(mapped.pData);
            const uint32_t rowBytes = (std::min)(mapped.RowPitch, desc.Width * 16u);
            for (uint32_t y = 0; y < desc.Height; ++y, pRow += mapped.RowPitch)
                for (uint32_t x = 0; x < rowBytes; x += 4)
                    hash = (hash ^ *reinterpret_cast<const uint32_t*>(pRow + x)) * 1099511628211ull;
            pRenderer->pD3dContext->Unmap(pStaging, 0);
        }
        pStaging->Release();
    }
    pSource->Release();
    return hash;
}

void FaceGenSystem::Update(World& aWorld, Actor* apActor, FaceGenComponent& aFaceGenComponent) noexcept
{
    std::lock_guard appearanceLock(CreatorTogether::AppearanceMutex());
    if (!apActor || apActor == PlayerCharacter::Get() || apActor->formID == 0x14)
        return;
    POINTER_SKYRIMSE(NiRTTI, NiMaskedShaderRTTI, 414675);
    POINTER_SKYRIMSE(TCreateTexture, CreateTexture, 70717);
    POINTER_SKYRIMSE(TCreateResourceView, CreateResourceView, 77299);
    POINTER_SKYRIMSE(TCreateTints, CreateTints, 27040);
    POINTER_SKYRIMSE(TextureHolder, s_textureHolder, 411393);

    auto pTriBasedGeom = GetHeadTriBasedGeom(apActor, 1);

    if (!pTriBasedGeom)
        return;

    BSShaderProperty* pShaderProperty = niptr_cast<BSShaderProperty>(pTriBasedGeom->effect);

    if (!pShaderProperty)
        return;

    pShaderProperty->IncRef();

    BSLightingShaderProperty* pLightingShader = static_cast<BSLightingShaderProperty*>(pShaderProperty);

    if (pLightingShader->GetRTTI() == NiMaskedShaderRTTI.Get())
    {
        // Face tint generation can be queued before the render device has
        // finished the post-load transition. The native resource helper
        // calls into ID3D11Device; keep this component pending until its
        // renderer and texture holder are both available.
        auto* pRenderer = BGSRenderer::Get();
        if (!pRenderer || !pRenderer->pD3dDevice ||
            !pRenderer->pD3dContext || !BGSRenderer::GetDevice() ||
            !s_textureHolder.Get())
        {
            pShaderProperty->DecRef();
            return;
        }

        BSMaskedShaderMaterial* pMaterial = static_cast<BSMaskedShaderMaterial*>(pLightingShader->material);
        // The property RTTI is shared by all lighting materials. Only feature 4
        // is BSLightingShaderMaterialFacegen, with a tint texture at +0xA0.
        if (!pMaterial)
        {
            pShaderProperty->DecRef();
            return;
        }
        const auto getFeature = reinterpret_cast<uint32_t (*)(void*)>((*reinterpret_cast<void***>(pMaterial))[6]);
        if (getFeature(pMaterial) != 4)
        {
            pShaderProperty->DecRef();
            return;
        }
        // Generated belongs to this head's material. A queued 3D rebuild may replace it after
        // the previous head was tinted, so do not let the component suppress the new texture.
        if (aFaceGenComponent.Generated && pMaterial->renderedTexture)
        {
            pShaderProperty->DecRef();
            return;
        }

        auto* pLocalPlayer = PlayerCharacter::Get();
        auto* pLocalHead = pLocalPlayer ? GetHeadTriBasedGeom(pLocalPlayer, 1) : nullptr;
        auto* pLocalShader = pLocalHead ? niptr_cast<BSShaderProperty>(pLocalHead->effect) : nullptr;
        // A shared property would need a geometry clone, not just a material clone.
        if (pLocalShader == pShaderProperty)
        {
            spdlog::error("Rejected FaceGen tint for actor {:X}: shader property belongs to the local player", apActor->formID);
            pShaderProperty->DecRef();
            return;
        }
        const bool sharedWithLocalPlayer = pLocalShader && pLocalShader->material == pMaterial;

        BSFixedString name("");
        auto pTexture = CreateTexture(name);
        if (!pTexture)
        {
            pShaderProperty->DecRef();
            return;
        }
        pTexture->IncRef();
        pTexture->buffer = CreateResourceView(s_textureHolder.Get(), 512, 512);
        if (!pTexture->buffer)
        {
            pTexture->DecRef();
            pShaderProperty->DecRef();
            return;
        }

        // The native builder (FaceGenSystem::CreateTints, 27040 / 14043C530) writes every layer with alpha > 0 into
        // a 16-slot pass with no bound check; a 17th visible layer overwrote the pass and crashed the receiver in its
        // pointer swap (141554FE0), 2026-09-29. Pass only visible layers, at most 16, as the vanilla creator does.
        std::vector<const Tints::Entry*> visible;
        for (const auto& entry : aFaceGenComponent.FaceTints.Entries)
            if (entry.Alpha > 0.f && visible.size() < 16)
                visible.push_back(&entry);

        GameArray<TintMask*> tints;
        tints.capacity = tints.length = static_cast<uint32_t>(visible.size());
        tints.data = (TintMask**)Memory::Allocate(sizeof(TintMask*) * tints.length);

        for (auto i = 0u; i < tints.length; ++i)
        {
            tints[i] = Memory::New<TintMask>();

            tints[i]->alpha = visible[i]->Alpha;
            tints[i]->color = visible[i]->Color;
            tints[i]->type = visible[i]->Type;

            auto pNewTexture = Memory::New<TESTexture>();
            pNewTexture->Construct();
            pNewTexture->Init();

            pNewTexture->name.Set(visible[i]->Name.c_str());

            tints[i]->texture = pNewTexture;
        }

        CreateTints(tints, pTexture);

        // Every tint job draws into render target 0xF (job runner 0x14043C9D0: image space effect 99 into 0xF, then a
        // job with a target texture gets its own copy, 0x14100EF50). The local player's face tint texture is a LIVE
        // view of 0xF (RaceSex 0x14096BE70 via 0x14100DD60), so this copy's job left the other player's face paint and
        // skin tone on the local player's face (owner reports 2026-09-29: "her Nord's face paint and skin tone
        // changed" whenever the host moved a slider). Queue the local player's own tints right after it, with no
        // target, exactly as the creator does: jobs run in order, so 0xF ends each batch holding the local face.
        if (pLocalPlayer && RestoreLocalTints.load(std::memory_order_relaxed))
            CreateTints(pLocalPlayer->GetTints(), nullptr);

        for (auto i = 0u; i < tints.length; ++i)
        {
            tints[i]->texture->~TESTexture();
            Memory::Free(tints[i]->texture);
            Memory::Free(tints[i]);
        }

        Memory::Free(tints.data);

        // FaceGen materials can be interned across heads. Their CRC/equality do
        // not include the generated tint texture (IDs 106788 / 106714,
        // VAs 141526A40 / 141523970). CommonLib BSShaderProperty::SetMaterial
        // documents the unique argument; native manager ID 107719 honors it.
        // SetMaterial(unique=true), ID 105544, clones through the native manager
        // and releases the old material correctly. Do not write through the old
        // pointer: that changes every head using it, including the local player.
        using TSetMaterial = void(BSShaderProperty*, void*, bool);
        POINTER_SKYRIMSE(TSetMaterial, SetMaterial, 105544);
        SetMaterial.Get()(pLightingShader, pMaterial, true);
        auto* pPrivateMaterial = static_cast<BSMaskedShaderMaterial*>(pLightingShader->material);
        if (!pPrivateMaterial || pPrivateMaterial == pMaterial)
        {
            pTexture->DecRef();
            pShaderProperty->DecRef();
            return;
        }
        pPrivateMaterial->renderedTexture = pTexture;
        pTexture->DecRef();

        if (sharedWithLocalPlayer)
            spdlog::info("Player {:X}: detached FaceGen material shared with the local player", apActor->formID);

        aFaceGenComponent.Generated = true;
        spdlog::info("Player {:X}: face tint generated, {} layers, texture {}", apActor->formID, tints.length, fmt::ptr(pTexture));
    }

    pShaderProperty->DecRef();
}

void FaceGenSystem::Setup(World& aWorld, const entt::entity aEntity, const Tints& acTints) noexcept
{
    if (acTints.Entries.empty())
    {
        aWorld.remove<FaceGenComponent>(aEntity);
        return;
    }
    auto& component = aWorld.emplace_or_replace<FaceGenComponent>(aEntity);
    component.FaceTints = acTints;
}
