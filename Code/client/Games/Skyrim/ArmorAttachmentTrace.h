#pragma once

// Called only from the game thread. Returns immediately unless opted in at launch.
void DrainArmorAttachmentTrace() noexcept;
