#include "PCH.h"
#include "ShoutVoiceHook.h"

#include <atomic>

namespace logger = SKSE::log;

namespace VSC
{
    namespace
    {
        // true => suppress the player's Thu'um vocalization (silent mode).
        std::atomic<bool> g_muteShoutVoice{ false };

        // Call-site hook of the engine's call to the RE::DialogueItem constructor — the
        // function that builds every spoken dialogue line. VERIFIED: the production mod
        // ConsiderateFollowers patches this exact call site and whitelists the four
        // VoicePower shout subtypes, proving shout vocalizations are created through it.
        //
        // SKSE's trampoline write_call patches an EXISTING `call rel32` instruction (it
        // reads the call's target and returns it so we can invoke the original) — so it
        // must be applied at the CALL SITE, not at the ctor's entry. The call site is
        // Address Library id 25541 + 0xE2 on AE. SE/AE number addresses differently and we
        // have no verified SE call-site id, so we install on AE only and no-op elsewhere.
        //
        // Cancel pattern (also ConsiderateFollowers'): run the original ctor, then for a
        // player VoicePower line free the just-built item and return null — which the
        // shout/voice dispatcher (the only creator of player VoicePower lines) null-checks.
        // The shout's effect/movement is driven separately, so only the voice line drops.
        struct DialogueItemCtorCallHook
        {
            static RE::DialogueItem* thunk(RE::DialogueItem* a_this, RE::TESQuest* a_quest,
                                           RE::TESTopic* a_topic, RE::TESTopicInfo* a_topicInfo,
                                           RE::TESObjectREFR* a_speaker)
            {
                RE::DialogueItem* result = func(a_this, a_quest, a_topic, a_topicInfo, a_speaker);

                // Only the PLAYER's shout (VoicePower) lines, only in silent mode. NPC
                // chatter, menu dialogue, and the player's combat grunts pass through.
                if (g_muteShoutVoice.load() && result && a_topic &&
                    a_speaker && a_speaker->IsPlayerRef()) {
                    using Sub = RE::DIALOGUE_DATA::Subtype;
                    switch (a_topic->data.subtype.get()) {
                    case Sub::kVoicePowerStartShort:
                    case Sub::kVoicePowerStartLong:
                    case Sub::kVoicePowerEndShort:
                    case Sub::kVoicePowerEndLong:
                        delete a_this;   // DialogueItem has a redefined operator delete
                        return nullptr;  // caller null-checks (ConsiderateFollowers pattern)
                    default:
                        break;
                    }
                }
                return result;
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };
    }

    void SetShoutVoiceMuted(bool a_muted)
    {
        g_muteShoutVoice.store(a_muted);
    }

    void InstallShoutVoiceMuteHook()
    {
        // The verified call-site id (25541) is the AE Address Library id. On any non-AE
        // runtime we skip the hook entirely — the mod stays fully functional, shouts just
        // vocalize. Never hook the wrong address on an unknown numbering.
        if (!REL::Module::IsAE()) {
            logger::info("[voice] shout-voice mute: not an AE runtime — silent-shout muting "
                         "disabled (shouts will vocalize). Mod unaffected.");
            return;
        }

        REL::Relocation<std::uintptr_t> target{ REL::ID(25541), 0xE2 };

        // Confirm an actual CALL (E8) sits at the hook site before patching it — guards
        // against the offset shifting on a future game update. If it's not there, skip.
        if (!REL::make_pattern<"E8">().match(target.address())) {
            logger::warn("[voice] shout-voice mute: hook-site signature mismatch on this version "
                         "— muting disabled (shouts will vocalize). Mod unaffected.");
            return;
        }

        SKSE::AllocTrampoline(14);
        auto& trampoline = SKSE::GetTrampoline();
        DialogueItemCtorCallHook::func =
            trampoline.write_call<5>(target.address(), DialogueItemCtorCallHook::thunk);
        logger::info("[voice] shout-voice mute hook installed (DialogueItem ctor call @ 0x{:X})",
            target.address());
    }
}
