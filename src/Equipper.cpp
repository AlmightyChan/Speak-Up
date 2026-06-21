#include "PCH.h"
#include "Equipper.h"
#include "ShoutVoiceHook.h"   // SetShoutVoiceMuted

#include <atomic>
#include <chrono>
#include <thread>

// Include WinUser.h for SendInput/INPUT, then undef the problematic macros that
// conflict with CommonLib's BGSDefaultObjectManager::GetObject<> template. These
// macros only affect the API functions for GetObject (GDI), not template methods.
// We must include AFTER the CommonLib PCH (RE/Skyrim.h) so CommonLib doesn't get
// the #defines — but including WinUser.h last re-introduces them, hence the undef.
//
// Minimal set: WinUser.h needs the basetsd.h/windef.h types. Use WIN32_LEAN_AND_MEAN
// to avoid pulling in GDI, COM, etc. and further reduce the macro blast.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
// Undef macros that WinUser.h defines to the A/W variant and that shadow CommonLib
// template methods. Only the GDI GetObject function is relevant here.
#ifdef GetObject
#  undef GetObject
#endif

namespace logger = SKSE::log;

namespace VSC
{
    // Definition of the extern guard declared in Equipper.h.
    std::atomic<bool> g_injectingShoutKey{ false };

    namespace
    {
        RE::BGSEquipSlot* EquipSlot(RE::DEFAULT_OBJECT a_slot)
        {
            auto* dom = RE::BGSDefaultObjectManager::GetSingleton();
            return dom ? dom->GetObject<RE::BGSEquipSlot>(a_slot) : nullptr;
        }

        // ---- Equip --------------------------------------------------------
        void EquipSpellNow(RE::ActorEquipManager* a_eqm, RE::PlayerCharacter* a_player,
                           RE::SpellItem* a_spell, Hand a_hand)
        {
            auto* rightSlot = EquipSlot(RE::DEFAULT_OBJECT::kRightHandEquip);
            auto* leftSlot  = EquipSlot(RE::DEFAULT_OBJECT::kLeftHandEquip);
            if (a_spell->IsTwoHanded()) {
                a_eqm->EquipSpell(a_player, a_spell, rightSlot);
                return;
            }
            switch (a_hand) {
            case Hand::Left:
                a_eqm->EquipSpell(a_player, a_spell, leftSlot);
                break;
            case Hand::Both:
                a_eqm->EquipSpell(a_player, a_spell, leftSlot);
                a_eqm->EquipSpell(a_player, a_spell, rightSlot);
                break;
            case Hand::Right:
            default:
                a_eqm->EquipSpell(a_player, a_spell, rightSlot);
                break;
            }
        }

        void EquipNow(RE::PlayerCharacter* a_player, RE::TESForm* a_form,
                      Category a_category, Hand a_hand, const std::string& a_name)
        {
            auto* eqm = RE::ActorEquipManager::GetSingleton();
            if (!eqm) {
                logger::error("[equip] no ActorEquipManager");
                return;
            }
            switch (a_category) {
            case Category::Spell:
                if (auto* spell = a_form->As<RE::SpellItem>()) {
                    EquipSpellNow(eqm, a_player, spell, a_hand);
                    logger::info("[equip] spell '{}' (hand={}) — OK", a_name, HandName(a_hand));
                } else {
                    logger::warn("[equip] '{}' is not a SpellItem (0x{:08X}) — NOT equipped",
                                 a_name, a_form->GetFormID());
                }
                break;
            case Category::Power:
                if (auto* power = a_form->As<RE::SpellItem>()) {
                    eqm->EquipSpell(a_player, power, EquipSlot(RE::DEFAULT_OBJECT::kVoiceEquip));
                    logger::info("[equip] power '{}' (voice slot) — OK", a_name);
                } else {
                    logger::warn("[equip] '{}' is not a power SpellItem (0x{:08X}) — NOT equipped",
                                 a_name, a_form->GetFormID());
                }
                break;
            case Category::Shout:
                if (auto* shout = a_form->As<RE::TESShout>()) {
                    eqm->EquipShout(a_player, shout);
                    logger::info("[equip] shout '{}' — OK", a_name);
                } else {
                    logger::warn("[equip] '{}' is not a TESShout (0x{:08X}) — NOT equipped",
                                 a_name, a_form->GetFormID());
                }
                break;
            }
        }

        // ---- Cast (auto-cast) --------------------------------------------
        RE::MagicSystem::CastingSource SourceForHand(Hand a_hand)
        {
            return a_hand == Hand::Left ? RE::MagicSystem::CastingSource::kLeftHand
                                        : RE::MagicSystem::CastingSource::kRightHand;
        }

        // Vanilla dual-cast magicka multiplier (effects' dualCastScale default 2.8).
        constexpr float kDualCastMagickaScale = 2.8f;

        CastSettings g_cast;  // set on the main thread at config load; read on main thread

        // Should this spell be instant-cast, given the settings? If not, the caller
        // equips it instead so the player casts it normally. Out-params explain why.
        bool ShouldInstantCast(RE::SpellItem* a_spell, const char** a_why)
        {
            if (!g_cast.instantCast) { *a_why = "instant cast disabled"; return false; }
            if (a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration &&
                !g_cast.allowConcentration) {
                *a_why = "concentration spell"; return false;
            }
            if (a_spell->GetChargeTime() >= g_cast.longCastThreshold && !g_cast.allowLongCast) {
                *a_why = "long charge time"; return false;
            }
            return true;
        }

        // Vanilla-style "you can't cast that" feedback: play the engine's magic-fail sound,
        // and (for an out-of-magicka failure) flash the magicka meter — same cues the game
        // gives when you try to cast without the mana, so it's clear we RECOGNIZED the
        // spell but couldn't cast it. All best-effort + null-guarded (never crashes).
        void PlayMagicFailSound(RE::PlayerCharacter* a_player)
        {
            auto* dom = RE::BGSDefaultObjectManager::GetSingleton();
            auto* am  = RE::BSAudioManager::GetSingleton();
            if (!dom || !am) return;
            auto* snd = dom->GetObject<RE::BGSSoundDescriptorForm>(RE::DEFAULT_OBJECT::kMagicFailSound);
            if (!snd) return;
            RE::BSSoundHandle handle{};
            if (am->BuildSoundDataFromDescriptor(handle, snd)) {  // BGSSoundDescriptorForm is-a BSISoundDescriptor
                if (a_player && a_player->Get3D()) handle.SetObjectToFollow(a_player->Get3D());
                handle.SetVolume(1.0f);
                handle.Play();
            }
        }

        void FlashMagickaMeter()
        {
            // Use the vanilla engine function FlashHudMenuMeter (SE: 51907, AE: 52845).
            // This is the exact call the game makes when you try to sprint with no stamina,
            // or cast without magicka — it drives the red blink on the attribute bar directly
            // through Scaleform without needing to navigate the HUDData/UIMessageQueue path
            // (which requires the meter's setBlinkingName to reach the right ActionScript
            // handler, and proved unreliable in practice).
            // NOTE: needs in-game confirmation that the blink fires correctly.
            using func_t = void (*)(RE::ActorValue);
            static REL::Relocation<func_t> FlashHudMenuMeter{ RELOCATION_ID(51907, 52845) };
            FlashHudMenuMeter(RE::ActorValue::kMagicka);
        }

        void CastFailFeedback(RE::PlayerCharacter* a_player, RE::MagicSystem::CannotCastReason a_reason)
        {
            PlayMagicFailSound(a_player);
            if (a_reason == RE::MagicSystem::CannotCastReason::kMagicka) {
                FlashMagickaMeter();
            }
        }

        // Returns true if it cast; false if blocked (caller may fall back to equip).
        bool CastSpellNow(RE::PlayerCharacter* a_player, RE::SpellItem* a_spell,
                          Hand a_hand, bool a_dual, const std::string& a_name,
                          bool a_stanceOrigin = false)
        {
            // Dual-cast gate — ask the ENGINE whether this actor may dual-cast THIS spell
            // via the CanDualCastSpell perk entry point (the exact mechanism the game uses
            // when you hold both cast keys). This respects whatever perk system is
            // installed — vanilla, Requiem, or any mod — with NO hardcoded perk IDs, so
            // it's correct on any load order out of the box. We must check it ourselves
            // because forcing SetDualCasting below bypasses the engine's own gate.
            if (a_dual) {
                float canDual = 0.0f;  // perks Set this to 1.0 if dual-casting is allowed
                RE::BGSEntryPoint::HandleEntryPoint(
                    RE::BGSEntryPoint::ENTRY_POINTS::kCanDualCastSpell,
                    a_player, static_cast<RE::TESForm*>(a_spell), &canDual);
                if (canDual != 1.0f) {
                    logger::info("[cast] '{}' single-cast: actor lacks the perk to dual-cast it",
                                 a_name);
                    a_dual = false;
                }
            }

            // GATE + COST go through the HAND caster so the spell behaves EXACTLY like a
            // normal hand cast — magicka, silence, perks, dual-cast checks are all
            // identical. We do NOT gate through the instant caster (CheckCast on a normal
            // spell via the voice/instant caster can wrongly fail).
            auto* caster = a_player->GetMagicCaster(SourceForHand(a_hand));
            if (!caster) {
                logger::warn("[cast] no caster for '{}'", a_name);
                return false;
            }
            // ORIGIN caster only changes WHERE the projectile/effect spawns on the body —
            // NOT how the spell works (same instant-cast, same magnitude/cost/effects).
            // Bare cast: head/voice node when weapons are sheathed, right hand when drawn.
            // Stance origin applies to DUAL casts too (previously it was skipped for dual,
            // which made a sheathed dual-cast wrongly fire from the hand instead of the
            // head/voice node). Head when sheathed, right hand when drawn.
            RE::MagicCaster* originCaster = caster;
            if (a_stanceOrigin) {
                const bool drawn = a_player->AsActorState()->IsWeaponDrawn();
                auto osrc = drawn ? RE::MagicSystem::CastingSource::kRightHand
                                  : RE::MagicSystem::CastingSource::kInstant;
                if (auto* oc = a_player->GetMagicCaster(osrc)) originCaster = oc;
            }

            // Engine gate: the same "can I cast this now?" check the game runs on a
            // normal cast — magicka, SILENCE, concentration/ward restrictions, etc.
            // (dualCast=a_dual so it checks the 2.8x dual-cast magicka requirement).
            RE::MagicSystem::CannotCastReason reason{};
            if (!caster->CheckCast(a_spell, a_dual, nullptr, &reason, false)) {
                logger::info("[cast] '{}' blocked by CheckCast (reason {})", a_name, static_cast<int>(reason));
                CastFailFeedback(a_player, reason);  // vanilla fail sound (+ magicka-bar flash if OOM)
                return true;  // handled (silenced / no magicka / etc.) — do NOT equip-fallback
            }

            // CastSpellImmediate is a FREE instant cast (it never spends magicka, like
            // Papyrus Spell.Cast). Vanilla deducts in the charge/release flow we bypass,
            // so we deduct here. CalculateMagickaCost is the engine's perk/enchant-aware
            // cost (identical to the magic menu's number).
            float cost = a_spell->CalculateMagickaCost(a_player);
            if (a_dual) {
                // Use the spell's ACTUAL dual-cast magicka scale (per its costliest magic
                // effect, as authored by whatever mod added the spell) instead of a
                // hardcoded number — so Requiem/other overhauls' values are respected.
                float scale = kDualCastMagickaScale;  // vanilla 2.8 fallback if unreadable
                if (auto* eff = a_spell->GetCostliestEffectItem(); eff && eff->baseEffect) {
                    float s = eff->baseEffect->data.dualCastScale;
                    if (s > 0.0f) scale = s;
                }
                cost *= scale;
            }
            if (cost > 0.0f) {
                a_player->AsActorValueOwner()->RestoreActorValue(
                    RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kMagicka, -cost);
            }

            // Dual-cast flag goes on the caster that actually FIRES (originCaster) so a
            // sheathed dual-cast still spawns from the head/voice node. We RESET it right
            // after the (synchronous) cast so a later normal hand cast is not wrongly
            // treated as dual — the flag otherwise persists on the actor's caster.
            // (MEDIUM CONFIDENCE: dual rendering from the instant/head caster needs in-game
            // verification; magicka cost is already deducted at the dual scale above.)
            RE::ActorMagicCaster* dualCaster = nullptr;
            if (a_dual) {
                dualCaster = skyrim_cast<RE::ActorMagicCaster*>(originCaster);
                if (dualCaster) dualCaster->SetDualCasting(true);
            }
            // Fire from the origin caster (visual node) — same method/effects as a hand
            // cast; only the spawn location differs when a_stanceOrigin chose the head.
            originCaster->CastSpellImmediate(a_spell, false, nullptr, 1.0f, false, 0.0f, a_player);
            if (dualCaster) dualCaster->SetDualCasting(false);  // reset — never leak the dual flag
            logger::info("[cast] spell '{}' (hand={}{}, origin={}, magicka -{:.0f})", a_name,
                HandName(a_hand), a_dual ? ", dual" : "",
                originCaster == caster ? "hand" : "head", cost);
            return true;
        }

        void CastPowerNow(RE::PlayerCharacter* a_player, RE::SpellItem* a_power, const std::string& a_name)
        {
            auto* caster = a_player->GetMagicCaster(RE::MagicSystem::CastingSource::kOther);
            if (!caster) {
                logger::warn("[cast] no 'other' caster for power '{}'", a_name);
                return;
            }
            // Gate on cooldown / usability (greater/lesser powers are once-per-day).
            RE::MagicSystem::CannotCastReason reason{};
            if (!caster->CheckCast(a_power, false, nullptr, &reason, false)) {
                logger::info("[cast] power '{}' blocked by CheckCast (reason {})", a_name, static_cast<int>(reason));
                return;
            }
            caster->CastSpellImmediate(a_power, false, nullptr, 1.0f, false, 0.0f, a_player);
            logger::info("[cast] power '{}'", a_name);
        }

        // ----------------------------------------------------------------
        // ShoutNeedsRealPipeline — GENERAL detection (not hardcoded to any shout).
        //
        // Returns true ONLY for archetypes whose effect a plain CastSpellImmediate of the
        // word-level spell cannot reproduce — engine SELF/WORLD state that only fires when
        // the native voice pipeline runs: a forced movement/transform script (kScript:
        // Whirlwind Sprint, Bend Will, Dragon Aspect), the ethereal state (kEtherealize),
        // the global time scale (kSlowTime), or spawned world objects/creatures
        // (kSpawnHazard: Storm Call/Cyclone; kSpawnScriptedRef; kSummonCreature: Call
        // Dragon / Call of Valor). These keep the pipeline even in SILENT mode because the
        // shout simply will not work otherwise — so they vocalize either way.
        //
        // Everything else is delivered fine by a direct word-level cast and is therefore
        // LEFT OFF this list so it stays SILENT in the default mode: target-applied effects
        // like Unrelenting Force's push (kStagger), debuffs, drains, fear, cloaks, etc. The
        // old list included kStagger et al., which wrongly forced Unrelenting Force onto the
        // vocalizing pipeline (and, via the key-tap, collapsed it to word 1) — contradicting
        // even this function's own original note that "Unrelenting Force -> NORMAL path".
        //
        // Verified against this user's actual (Requiem) load order via the record data:
        // Requiem reimplements many shouts as kScript (Aura Whisper, Call Dragon, Animal
        // Allegiance, Clear Skies, Bend Will) on top of vanilla's kScript Whirlwind Sprint,
        // so the archetype check auto-catches them all with NO per-shout hardcoding. The
        // effect-layer shouts (Unrelenting Force=kStagger, Fire/Frost Breath + Marked for
        // Death + Cyclone=ValueModifier, Dismay=Demoralize, Elemental Fury=PeakValueModifier)
        // are correctly LEFT OFF and take the silent direct cast at the correct word level.
        //
        // MEDIUM CONFIDENCE on the direct-cast set — needs in-game verification that the
        // direct cast fully delivers each now-instant shout's effect. If one is missing on
        // the instant path, add its archetype here. ShoutUseRealCast=0 forces instant for all.
        // ----------------------------------------------------------------
        bool ShoutNeedsRealPipeline(RE::SpellItem* a_spell)
        {
            if (!a_spell) return false;
            for (const auto* eff : a_spell->effects) {
                if (!eff || !eff->baseEffect) continue;
                using AID = RE::EffectArchetypes::ArchetypeID;
                switch (eff->baseEffect->data.archetype) {
                // Engine self/world state a direct CastSpellImmediate cannot reproduce:
                case AID::kScript:            // script-driven shouts (Whirlwind Sprint + most
                                              // Requiem shouts — see note above)
                case AID::kEtherealize:       // Become Ethereal
                case AID::kSlowTime:          // Slow Time (global time scale)
                case AID::kSpawnHazard:       // Storm Call / Cyclone hazards
                case AID::kSpawnScriptedRef:  // Throw Voice
                case AID::kSummonCreature:    // Call of Valor / Call Dragon
                // Input-loop / transformation archetypes: no vanilla shout uses them, but
                // auto-catch any MODDED shout that does (it would not survive a direct cast):
                case AID::kTelekinesis:
                case AID::kWerewolfFeed:
                case AID::kWerewolf:
                case AID::kGrabActor:
                case AID::kVampireLord:
                    return true;
                default:
                    break;
                }
            }
            return false;
        }

        // ShoutKeyHoldMs — how long to HOLD the synthetic Shout key for a given word level.
        //
        // The engine picks the released word from how long Shout is held, comparing the
        // hold against two game settings: fShoutTime1 (hold >= this -> word 2) and
        // fShoutTime2 (hold >= this -> word 3). We read them LIVE (Requiem and other
        // overhauls retune them) so the timing is always correct for the active setup —
        // not a hardcoded guess. word 1 is a brief tap below fShoutTime1; word 2 sits
        // midway between the two thresholds (the only value that must fall in a window);
        // word 3 clears fShoutTime2 with margin (over-holding just caps at the top word).
        int ShoutKeyHoldMs(int a_level)
        {
            float t1 = 0.2f, t2 = 0.9f;  // engine defaults if the GMSTs are unreadable
            if (auto* gmsts = RE::GameSettingCollection::GetSingleton()) {
                if (auto* s1 = gmsts->GetSetting("fShoutTime1")) t1 = s1->GetFloat();
                if (auto* s2 = gmsts->GetSetting("fShoutTime2")) t2 = s2->GetFloat();
            }
            // Sanitize degenerate/corrupt GMSTs so EVERY tier gets a positive, ordered hold
            // window (a 0/negative value would otherwise collapse word 2/3 back to a word-1 tap).
            if (t1 <= 0.0f) t1 = 0.2f;
            if (t2 <= t1)   t2 = t1 + 0.7f;
            float secs;
            if (a_level >= RE::TESShout::VariationIDs::kThree) {
                secs = t2 + 0.30f;                          // word 3: past fShoutTime2
            } else if (a_level == RE::TESShout::VariationIDs::kTwo) {
                secs = (t1 + t2) * 0.5f;                    // word 2: midway in the window
            } else {
                secs = t1 * 0.3f;                           // word 1: brief tap below t1
            }
            return static_cast<int>(secs * 1000.0f);
        }

        // ----------------------------------------------------------------
        // TryCastShoutRealPipeline — full-pipeline path (movement/script shouts,
        // and ALL shouts when the player wants the character to vocalize the Thu'um).
        //
        // Equips the shout, then injects the bound Shout key via Win32 SendInput so
        // the engine runs its full voice pipeline (animation + Thu'um sound + movement
        // + scripts + cooldown). The key is HELD for a_holdMs and released async, so
        // the engine charges to the spoken WORD LEVEL — an instantaneous tap always
        // released word 1 (the "every shout only casts Fus" bug).
        //
        // Sets g_injectingShoutKey=true for the whole hold (down..up) so
        // ListenHotkeySink can ignore the synthetic events and avoid toggling
        // push-to-talk if PTT happens to be bound to the same key.
        //
        // Returns true  = injected (real pipeline running).
        // Returns false = ActorEquipManager missing or SendInput returned != 2
        //                 (caller falls back to the instant path).
        //
        // MEDIUM CONFIDENCE — needs in-game verification:
        //   • Reliable in borderless-window / windowed mode (game processes Win32 input).
        //   • Fullscreen-exclusive (D3D exclusive) may ignore SendInput entirely —
        //     Windows routes injected events to the foreground window but some titles
        //     hook DirectInput/RawInput separately.  Set ShoutUseRealCast=0 as escape.
        // ----------------------------------------------------------------
        bool TryCastShoutRealPipeline(RE::PlayerCharacter* a_player, RE::TESShout* a_shout,
                                      RE::SpellItem* a_spell, RE::HighProcessData* a_high,
                                      int a_level, int a_holdMs, const std::string& a_name)
        {
            auto* eqm = RE::ActorEquipManager::GetSingleton();
            if (!eqm) {
                logger::warn("[cast] shout '{}' real-pipeline path: no ActorEquipManager",
                             a_name);
                return false;
            }

            // 1. Equip the shout so the engine knows which shout to fire on key press.
            eqm->EquipShout(a_player, a_shout);

            // 2. Set the word-level on HighProcessData so the engine fires the correct
            //    tier (word 1/2/3) rather than whatever it last had equipped.
            if (a_high) {
                a_high->currentShout          = a_shout;
                a_high->currentShoutVariation = static_cast<RE::TESShout::VariationID>(a_level);
            }

            // 3. Read the bound Shout key from ControlMap (player-configured DX scan code).
            //    Treat 0, 0xFF (kInvalid byte), and 0xFFFFFFFF as unmapped — fall back to INI.
            std::uint32_t shoutScan = 0;
            if (auto* cm = RE::ControlMap::GetSingleton()) {
                std::uint32_t mapped = static_cast<std::uint32_t>(
                    cm->GetMappedKey("Shout", RE::INPUT_DEVICE::kKeyboard));
                if (mapped != 0 && mapped != 0xFF && mapped != 0xFFFFFFFF) {
                    shoutScan = mapped;
                }
            }
            if (shoutScan == 0) {
                shoutScan = g_cast.shoutKeyDX;  // INI fallback (default 0x39 = Space)
                logger::info("[cast] shout '{}' ControlMap shout key unmapped — "
                             "using ShoutKeyDX=0x{:X}", a_name, shoutScan);
            }

            // 4. Acquire the single-injection lock (also the guard ListenHotkeySink uses to
            //    ignore BOTH synthetic events for the whole hold, so the injected Shout key
            //    can't toggle PTT if they share a binding). exchange(true): if a previous
            //    shout's key-hold is STILL in flight, DROP this one rather than spawn a second
            //    thread that would fight over the same scan code and mis-charge the word level
            //    (the very bug the hold prevents). Only the owning keyup thread clears it.
            if (g_injectingShoutKey.exchange(true)) {
                logger::info("[cast] shout '{}' ignored — a shout-key injection is already in "
                             "flight (mid-hold)", a_name);
                return true;  // handled — do NOT fall through to the instant path (no double cast)
            }

            // 5. Inject key-DOWN now; release after a_holdMs. Skyrim derives the released
            //    word level from how long Shout is HELD — an instantaneous down+up taps
            //    out at word 1 (the long-standing "every shout only casts Fus" bug), so we
            //    hold the synthetic key for the spoken word level's duration and let the
            //    engine charge to that tier. KEYEVENTF_SCANCODE sends a hardware-style
            //    event DirectInput/RawInput picks up like a physical key press.
            INPUT down{};
            down.type       = INPUT_KEYBOARD;
            down.ki.wScan   = static_cast<WORD>(shoutScan);
            down.ki.dwFlags = KEYEVENTF_SCANCODE;
            UINT sent = ::SendInput(1, &down, sizeof(INPUT));
            if (sent != 1) {
                g_injectingShoutKey.store(false);
                logger::warn("[cast] shout '{}' SendInput(down) returned {} (expected 1) — "
                             "falling back to instant path", a_name, sent);
                return false;  // caller falls through to instant path
            }

            // Release the key after the hold on a detached thread: SendInput is thread-safe
            // and touches no game objects, so this is safe off the main thread, and async
            // means we never block the main thread for up to ~1.6s while the shout charges.
            const int holdMs = a_holdMs < 0 ? 0 : a_holdMs;
            std::thread([shoutScan, holdMs]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
                INPUT up{};
                up.type       = INPUT_KEYBOARD;
                up.ki.wScan   = static_cast<WORD>(shoutScan);
                up.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
                ::SendInput(1, &up, sizeof(INPUT));
                g_injectingShoutKey.store(false);  // clear the guard once Shout is released
            }).detach();

            logger::info("[cast] shout '{}' (word level {}, spell {:08X}, shoutKeyDX=0x{:X}, "
                         "hold {}ms) [real-pipeline — equip+SendInput, held to charge word level]",
                a_name, a_level + 1, a_spell->GetFormID(), shoutScan, holdMs);
            return true;
        }

        // ----------------------------------------------------------------
        // CastShoutInstant — PRIMARY (silent) path: voice-slot cast.
        //
        // Casts the exact word-level spell through the VOICE/shout caster slot
        // (kOther) via CastSpellImmediate — the Dragonborn Unlimited / DSN technique.
        // Routing through the voice slot (rather than kInstant) lets the shout's full
        // magic effects fire — including the ones historically thought to need the key
        // pipeline (Unrelenting Force's ragdoll, etc.) — while staying SILENT, because a
        // magic cast does not trigger the Thu'um voice line. Instant, no key-hold.
        //
        // Adds tactile feedback so the cast feels like a real shout:
        //   - Screen shake via RE::ShakeCamera (strength 0.5, ~0.4s).
        //   - ShoutAttack event so shout-detection mods (Thunderchild, etc.) fire.
        //   - Cooldown set manually (bypassed by CastSpellImmediate).
        //   - Optional animation (playShoutAnimation INI flag).
        //
        // MEDIUM CONFIDENCE — needs in-game verification:
        //   - ShakeCamera feel (strength/duration) is a first-guess; tune in game.
        //   - Whether every movement/script shout (Whirlwind Sprint, Become Ethereal,
        //     Slow Time) fully fires through the voice slot — verify in game; the
        //     key-hold pipeline remains available as the ShoutPlayVoice-ON path.
        // ----------------------------------------------------------------
        void CastShoutInstant(RE::PlayerCharacter* a_player, RE::TESShout* a_shout,
                              RE::SpellItem* a_spell, const RE::TESShout::Variation& a_variation,
                              RE::HighProcessData* a_high, int a_level, const std::string& a_name)
        {
            // Cast through the VOICE/shout caster slot (kOther = magicCasters[kPowerOrShout]),
            // NOT the kInstant caster — the Dragonborn Unlimited / DSN technique. Routing the
            // word-level spell through the voice slot fires the shout's full magic effects
            // (e.g. Unrelenting Force's ragdoll) instantly, while remaining SILENT (a magic
            // cast does not trigger the Thu'um voice line). No key-hold, no delay.
            auto* caster = a_player->GetMagicCaster(RE::MagicSystem::CastingSource::kOther);
            if (!caster) {
                logger::warn("[cast] no voice/shout caster for shout '{}'", a_name);
                return;
            }
            caster->CastSpellImmediate(a_spell, false, nullptr, 1.0f, false, 0.0f, a_player);

            // Screen shake: modest tactile feedback (dragon-voice shout feel without the
            // full pipeline).  Null-guard the 3D node for the position source.
            // MEDIUM CONFIDENCE: ShakeCamera strength 0.5 / duration 0.4 is a first estimate.
            auto* node = a_player->Get3D();
            RE::NiPoint3 pos = node ? node->world.translate : RE::NiPoint3{ 0.f, 0.f, 0.f };
            RE::ShakeCamera(0.5f, pos, 0.4f);

            // Manually set cooldown (bypassed by CastSpellImmediate).
            if (a_high) {
                float mult = a_player->AsActorValueOwner()->GetActorValue(
                    RE::ActorValue::kShoutRecoveryMult);
                if (mult <= 0.0f) mult = 1.0f;
                a_high->voiceRecoveryTime     = a_variation.recoveryTime * mult;
                a_high->currentShout          = a_shout;
                a_high->currentShoutVariation = static_cast<RE::TESShout::VariationID>(a_level);
            }

            // Notify shout-detection mods (Thunderchild, Forceful Tongue, etc.).
            if (auto* src = RE::ShoutAttack::GetEventSource()) {
                RE::ShoutAttack::Event evt{ a_shout };
                src->SendEvent(&evt);
            }

            // Optional body animation.
            if (g_cast.playShoutAnimation) {
                a_player->NotifyAnimationGraph("shoutStart");
                std::thread([] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
                    if (auto* task = SKSE::GetTaskInterface()) {
                        task->AddTask([] {
                            if (auto* p = RE::PlayerCharacter::GetSingleton()) {
                                p->NotifyAnimationGraph("shoutStop");
                            }
                        });
                    }
                }).detach();
            }

            logger::info("[cast] shout '{}' (word level {}, spell {:08X}, delivery {}, "
                         "casting {}, cooldown {:.1f}s) [voice-slot — CastSpellImmediate via kOther]",
                a_name, a_level + 1, a_spell->GetFormID(),
                static_cast<int>(a_spell->GetDelivery()),
                static_cast<int>(a_spell->GetCastingType()),
                a_variation.recoveryTime);
        }

        void CastShoutNow(RE::PlayerCharacter* a_player, RE::TESShout* a_shout,
                          const std::string& a_name, int a_level)
        {
            // a_level (0/1/2) selects the spoken word level ("Fus"=0, "Fus Ro"=1,
            // "Fus Ro Dah"=2). -1 = highest unlocked (CommandGrammar routes a bare shout
            // name to Action::Cast with shoutLevel -1; "cast by name" uses this path).
            int level = a_level;
            if (level < 0) {
                for (int i = RE::TESShout::VariationIDs::kThree; i >= RE::TESShout::VariationIDs::kOne; --i) {
                    const auto& v = a_shout->variations[i];
                    if (v.word && v.spell && v.word->GetKnown()) { level = i; break; }
                }
                if (level < 0) level = RE::TESShout::VariationIDs::kOne;
            }
            if (level > RE::TESShout::VariationIDs::kThree) level = RE::TESShout::VariationIDs::kThree;
            const auto& variation = a_shout->variations[level];
            auto* spell = variation.spell;
            if (!spell) {
                logger::warn("[cast] shout '{}' has no spell at word level {}", a_name, level + 1);
                return;
            }

            // Shout cooldown gate (shared by both paths).
            RE::HighProcessData* high = nullptr;
            if (auto* proc = a_player->GetActorRuntimeData().currentProcess) {
                high = proc->high;
            }
            if (high && high->voiceRecoveryTime > 0.0f) {
                logger::info("[cast] shout '{}' on cooldown ({:.1f}s left) — ignored",
                    a_name, high->voiceRecoveryTime);
                return;
            }

            // ----------------------------------------------------------------
            // PATH SELECTION  (two settings + per-shout archetype)
            //
            // The native voice PIPELINE (EquipShout + held Shout key) is the ONLY path that
            // reproduces a shout's full engine behaviour — crucially the movement/script
            // shouts (Whirlwind Sprint's dash, Become Ethereal, Slow Time, Storm Call). The
            // VOICE-SLOT instant cast (CastSpellImmediate via kOther) is faster and needs no
            // SendInput, but it does NOT carry those script effects — VERIFIED in-game: silent
            // Whirlwind Sprint via the voice slot fires the shout but never moves the player.
            //
            // So we route by what the shout NEEDS, not by the voice toggle:
            //   usePipeline = ShoutUseRealCast && (ShoutPlayVoice || ShoutNeedsRealPipeline)
            //
            // ShoutNeedsRealPipeline(spell) (archetype-driven: kScript, kEtherealize,
            //   kSlowTime, kSpawnHazard, kSummonCreature, …): these MUST run the pipeline to
            //   work at all, in EVERY mode. In SILENT mode the Thu'um voice line they would
            //   normally speak is suppressed by the AE DialogueItem mute hook
            //   (SetShoutVoiceMuted, see ShoutVoiceHook) — so they move AND stay silent.
            //   The hook drops only the player's VoicePower line; the movement/script (driven
            //   separately) is untouched. (On non-AE the hook is absent, so they vocalize.)
            //
            // ShoutPlayVoice=ON: the player WANTS the character to vocalize, so run the
            //   pipeline for EVERY shout (the mute is off, so the Thu'um plays).
            //
            // SILENT + simple shout (Unrelenting Force, Fire Breath, …): no script to lose,
            //   so take the fast VOICE-SLOT instant cast at the spoken word level.
            //
            // ShoutUseRealCast=0 (escape hatch): forbid SendInput entirely (exclusive-
            //   fullscreen / stripped-perms) — voice-slot for everything (movement shouts may
            //   not fully fire, accepted trade-off for that environment).
            // ----------------------------------------------------------------
            const bool needsRealForEffect = ShoutNeedsRealPipeline(spell);
            const bool usePipeline =
                g_cast.shoutUseRealCast && (g_cast.shoutPlayVoice || needsRealForEffect);

            if (usePipeline) {
                const int holdMs = ShoutKeyHoldMs(level);
                logger::info("[cast] shout '{}' word{} spell {:08X} -> PIPELINE ({}{}, hold {}ms)",
                    a_name, level + 1, spell->GetFormID(),
                    g_cast.shoutPlayVoice ? "Thu'um voice ON" : "silent + muted",
                    needsRealForEffect ? ", effect needs pipeline" : "", holdMs);
                if (TryCastShoutRealPipeline(a_player, a_shout, spell, high, level, holdMs, a_name)) {
                    return;  // real pipeline running — done
                }
                // SendInput failed or ActorEquipManager missing — fall through to the
                // voice-slot cast so the shout still fires its magic-effect portion.
                logger::warn("[cast] shout '{}' pipeline unavailable — falling back to voice-slot cast",
                             a_name);
            }

            // SILENT + simple shout (or fallback): voice-slot instant cast at the word level.
            logger::info("[cast] shout '{}' word{} spell {:08X} -> VOICE-SLOT instant cast{}",
                a_name, level + 1, spell->GetFormID(),
                needsRealForEffect ? " (FALLBACK — archetype wants pipeline; effect may not fire)" : "");
            CastShoutInstant(a_player, a_shout, spell, variation, high, level, a_name);
        }

        void CastNow(RE::PlayerCharacter* a_player, RE::TESForm* a_form,
                     Category a_category, Hand a_hand, bool a_dual, const std::string& a_name,
                     int a_shoutLevel, bool a_stanceOrigin)
        {
            switch (a_category) {
            case Category::Spell:
                if (auto* spell = a_form->As<RE::SpellItem>()) {
                    const char* why = nullptr;
                    if (ShouldInstantCast(spell, &why)) {
                        CastSpellNow(a_player, spell, a_hand, a_dual, a_name, a_stanceOrigin);
                    } else {
                        // Concentration / long-charge / instant-cast-off: equip it so the
                        // player casts it normally (correct behavior for those spell types).
                        if (auto* eqm = RE::ActorEquipManager::GetSingleton()) {
                            EquipSpellNow(eqm, a_player, spell, g_cast.equipHand);
                        }
                        logger::info("[cast] '{}' not instant-cast ({}) — equipped to {} instead",
                            a_name, why, HandName(g_cast.equipHand));
                    }
                } else {
                    logger::warn("[cast] '{}' is not a SpellItem (0x{:08X}) — NOT cast",
                                 a_name, a_form->GetFormID());
                }
                break;
            case Category::Power:
                if (auto* power = a_form->As<RE::SpellItem>()) {
                    CastPowerNow(a_player, power, a_name);
                } else {
                    logger::warn("[cast] '{}' is not a power SpellItem (0x{:08X}) — NOT cast",
                                 a_name, a_form->GetFormID());
                }
                break;
            case Category::Shout:
                if (auto* shout = a_form->As<RE::TESShout>()) {
                    CastShoutNow(a_player, shout, a_name, a_shoutLevel);
                } else {
                    logger::warn("[cast] '{}' is not a TESShout (0x{:08X}) — NOT cast",
                                 a_name, a_form->GetFormID());
                }
                break;
            }
        }
    }

    void SetCastSettings(const CastSettings& a_settings)
    {
        g_cast = a_settings;
        // Silent mode (ShoutPlayVoice OFF) => mute the player's Thu'um voice line; the
        // shout effect/movement still runs. Voice ON => let the character vocalize.
        SetShoutVoiceMuted(!a_settings.shoutPlayVoice);
    }

    void Execute(const RosterEntry& a_entry, Action a_action, Hand a_hand, bool a_dual, int a_shoutLevel,
                 bool a_stanceOrigin)
    {
        // Capture the FormID (stable) rather than the TESForm* (could be invalidated
        // by a load-order change between grammar build and execution); re-resolve on
        // the main thread.
        RE::FormID   formID   = a_entry.formID;
        Category     category = a_entry.category;
        std::string  name     = a_entry.name;

        auto* taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            logger::error("[exec] no task interface for '{}'", name);
            return;
        }
        taskInterface->AddTask([formID, category, name, a_action, a_hand, a_dual, a_shoutLevel, a_stanceOrigin]() {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* form   = RE::TESForm::LookupByID(formID);
            if (!player || !form) {
                logger::error("[exec] missing player/form for '{}' (0x{:08X})", name, formID);
                return;
            }
            if (a_action == Action::Cast) {
                CastNow(player, form, category, a_hand, a_dual, name, a_shoutLevel, a_stanceOrigin);
            } else {
                EquipNow(player, form, category, a_hand, name);
            }
        });
    }

    void EquipEntry(const RosterEntry& a_entry, Hand a_hand)
    {
        Execute(a_entry, Action::Equip, a_hand, false);
    }
}
