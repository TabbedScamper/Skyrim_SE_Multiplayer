#pragma once

/**
 * @brief Experience added to an arbitrary skill.
 */
struct AddExperienceEvent
{
    AddExperienceEvent(int32_t aSkill, float aExperience)
        : Skill(aSkill)
        , Experience(aExperience)
    {
    }

    int32_t Skill{};
    float Experience{};
};
