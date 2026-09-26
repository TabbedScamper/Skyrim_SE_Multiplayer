#pragma once
#include <Messages/DialogueListenState.h>
#include <memory>

struct Actor;
namespace DialogueListenNative
{
struct Observation
{
    uint32_t NpcFormId{};
    DialogueListenState State;
};
// Scaleform is observed on its AdvanceMovie thread; only copied text crosses threads.
void ObserveMenu() noexcept;
Observation Read() noexcept;
uint64_t SubtitleHideSerial(uint32_t aFormId) noexcept;
void Reset() noexcept;

class Presentation
{
public:
    Presentation();
    ~Presentation();
    bool Begin(Actor* aNpc) noexcept;
    bool Update(Actor* aNpc, uint32_t aFov) noexcept;
    void End() noexcept;
    static bool ExitPressed() noexcept;
private:
    struct State;
    std::unique_ptr<State> m_state;
};
}
