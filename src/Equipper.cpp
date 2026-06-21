#include "PCH.h"
#include "Equipper.h"

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
        // Returns true when ANY effect of the word-spell has a magic-effect archetype
        // that cannot be replicated by a plain CastSpellImmediate — these archetypes
        // either drive engine movement (kEtherealize, kSlowTime, kStagger), spawn
        // world objects (kSpawnHazard, kSpawnScriptedRef, kSummonCreature), require
        // the full Papyrus script pipeline (kScript), or need other engine state that
        // only fires when the voice pipeline runs natively.
        //
        // MEDIUM CONFIDENCE — archetype list calibrated against vanilla shouts:
        //   Whirlwind Sprint   = kScript  (forced via real pipeline)
        //   Become Ethereal    = kEtherealize
        //   Slow Time          = kSlowTime
        //   Storm Call         = kSpawnHazard + kScript
        //   Call Dragon        = kSummonCreature + kScript
        //   Cyclone            = kSpawnHazard
        //   Bend Will (dragon) = kScript
        //   Stagger (some)     = kStagger
        // Unrelenting Force = kValueModifier (magnitude impulse) -> NORMAL path.
        // If a modded shout mis-archives one of these archetypes on an otherwise
        // simple effect, it will incorrectly use the real pipeline — that is safe
        // (more feedback, no breakage) but may need the INI master switch to override.
        // ----------------------------------------------------------------
        bool ShoutNeedsRealPipeline(RE::SpellItem* a_spell)
        {
            if (!a_spell) return false;
            for (const auto* eff : a_spell->effects) {
                if (!eff || !eff->baseEffect) continue;
                using AID = RE::EffectArchetypes::ArchetypeID;
                switch (eff->baseEffect->data.archetype) {
                case AID::kScript:
                case AID::kCloak:
                case AID::kEtherealize:
                case AID::kSlowTime:
                case AID::kSpawnHazard:
                case AID::kSpawnScriptedRef:
                case AID::kBoundWeapon:
                case AID::kSummonCreature:
                case AID::kBanish:
                case AID::kDisguise:
                case AID::kAccumulateMagnitude:
                case AID::kStagger:
                case AID::kTelekinesis:
                case AID::kGrabActor:
                    return true;
                default:
                    break;
                }
            }
            return false;
        }

        // ----------------------------------------------------------------
        // TryCastShoutRealPipeline — NEEDS-REAL path.
        //
        // Equips the shout, sets HighProcessData word level, then injects the
        // bound Shout key via Win32 SendInput so the engine runs its full voice
        // pipeline (animation + sound + movement + scripts + cooldown).
        //
        // Sets g_injectingShoutKey=true for the duration of the SendInput call
        // so ListenHotkeySink can ignore the synthetic event and avoid toggling
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
                                      int a_level, const std::string& a_name)
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

            // 4. Set the guard so ListenHotkeySink ignores this synthetic event
            //    (prevents the injected Shout key from toggling PTT if they share a binding).
            g_injectingShoutKey.store(true);

            // 5. Inject key-down + key-up.  KEYEVENTF_SCANCODE sends a hardware-style
            //    event that DirectInput/RawInput picks up like a physical key press.
            INPUT inputs[2]{};
            inputs[0].type       = INPUT_KEYBOARD;
            inputs[0].ki.wScan   = static_cast<WORD>(shoutScan);
            inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;
            inputs[1].type       = INPUT_KEYBOARD;
            inputs[1].ki.wScan   = static_cast<WORD>(shoutScan);
            inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
            UINT sent = ::SendInput(2, inputs, sizeof(INPUT));

            // Clear the guard immediately after (the engine will have queued the event).
            g_injectingShoutKey.store(false);

            if (sent != 2) {
                logger::warn("[cast] shout '{}' SendInput returned {} (expected 2) — "
                             "falling back to instant path", a_name, sent);
                return false;  // caller falls through to instant path
            }

            logger::info("[cast] shout '{}' (word level {}, spell {:08X}, shoutKeyDX=0x{:X}) "
                         "[real-pipeline — equip+SendInput]",
                a_name, a_level + 1, a_spell->GetFormID(), shoutScan);
            return true;
        }

        // ----------------------------------------------------------------
        // CastShoutInstant — NORMAL path.
        //
        // Uses CastSpellImmediate on the kInstant caster for snappy casting of
        // shouts whose effects are fully replicated by immediate cast (e.g.
        // Unrelenting Force, Elemental Fury, Ice Form, etc.).
        //
        // Adds tactile feedback so the cast feels like a real shout:
        //   - Screen shake via RE::ShakeCamera (strength 0.5, ~0.4s).
        //   - ShoutAttack event so shout-detection mods (Thunderchild, etc.) fire.
        //   - Cooldown set manually (bypassed by CastSpellImmediate).
        //   - Optional animation (playShoutAnimation INI flag).
        //
        // MEDIUM CONFIDENCE — needs in-game verification:
        //   - ShakeCamera feel (strength/duration) is a first-guess; tune in game.
        //   - Dragon-voice vocalization (thu'um audio) requires the full engine voice
        //     pipeline and will NOT play here; only spell-effect sounds fire.
        // ----------------------------------------------------------------
        void CastShoutInstant(RE::PlayerCharacter* a_player, RE::TESShout* a_shout,
                              RE::SpellItem* a_spell, const RE::TESShout::Variation& a_variation,
                              RE::HighProcessData* a_high, int a_level, const std::string& a_name)
        {
            auto* caster = a_player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
            if (!caster) {
                logger::warn("[cast] no power/shout caster for shout '{}'", a_name);
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
                         "casting {}, cooldown {:.1f}s) [instant path — CastSpellImmediate]",
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
            // DUAL PATH SELECTION
            //
            // ShoutUseRealCast=0 (legacy master switch): force the instant path for
            // ALL shouts regardless of archetype (keeps old behavior for users who
            // need to avoid SendInput, e.g. exclusive fullscreen or stripped perms).
            //
            // ShoutUseRealCast=1 (default — dual-path auto-detect):
            //   NEEDS-REAL: ShoutNeedsRealPipeline() returned true -> run the engine's
            //     full voice pipeline via EquipShout + SendInput.  This handles Whirlwind
            //     Sprint (kScript), Become Ethereal (kEtherealize), Slow Time (kSlowTime),
            //     Storm Call (kSpawnHazard), Call Dragon (kSummonCreature), Cyclone, etc.
            //     If SendInput fails (returns != 2), fall through to the instant path as
            //     a best-effort fallback so the shout still does *something*.
            //   NORMAL: no problematic archetypes (e.g. Unrelenting Force, Elemental Fury,
            //     Ice Form, Aura Whisper) -> CastSpellImmediate + ShakeCamera feedback.
            //     Fast, no equip, no key injection.
            // ----------------------------------------------------------------
            const bool needsReal = g_cast.shoutUseRealCast && ShoutNeedsRealPipeline(spell);

            if (needsReal) {
                logger::info("[cast] shout '{}' word{} spell {:08X} -> NEEDS-REAL pipeline "
                             "(archetype check)", a_name, level + 1, spell->GetFormID());
                if (TryCastShoutRealPipeline(a_player, a_shout, spell, high, level, a_name)) {
                    return;  // real pipeline running — done
                }
                // SendInput failed or ActorEquipManager missing — fall through to instant
                // path so the shout still does its magic-effect portion.
                logger::warn("[cast] shout '{}' real pipeline failed — falling back to instant path",
                             a_name);
            }

            // NORMAL / fallback instant path.
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
