#include "PCH.h"
#include "Equipper.h"

#include <chrono>
#include <thread>

namespace logger = SKSE::log;

namespace VSC
{
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
                    logger::info("[equip] spell '{}' (hand={})", a_name, HandName(a_hand));
                }
                break;
            case Category::Power:
                if (auto* power = a_form->As<RE::SpellItem>()) {
                    eqm->EquipSpell(a_player, power, EquipSlot(RE::DEFAULT_OBJECT::kVoiceEquip));
                    logger::info("[equip] power '{}' (voice slot)", a_name);
                }
                break;
            case Category::Shout:
                if (auto* shout = a_form->As<RE::TESShout>()) {
                    eqm->EquipShout(a_player, shout);
                    logger::info("[equip] shout '{}'", a_name);
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
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return;
            auto hud = ui->GetMenu<RE::HUDMenu>();
            if (!hud) return;
            auto* meter = hud->GetRuntimeData().magicka;
            auto* fm = RE::MessageDataFactoryManager::GetSingleton();
            auto* is = RE::InterfaceStrings::GetSingleton();
            auto* q  = RE::UIMessageQueue::GetSingleton();
            if (!meter || !fm || !is || !q) return;
            auto* creator = fm->GetCreator<RE::HUDData>(is->hudData);
            if (!creator) return;
            auto* data = static_cast<RE::HUDData*>(creator->Create());
            if (!data) return;
            data->type = RE::HUDData::Type::kSetBlinking;
            data->text = meter->setBlinkingName.c_str();
            q->AddMessage(RE::HUDMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kUpdate, data);
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
            RE::MagicCaster* originCaster = caster;
            if (a_stanceOrigin && !a_dual) {
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

            if (a_dual) {
                if (auto* actorCaster = skyrim_cast<RE::ActorMagicCaster*>(caster)) {
                    actorCaster->SetDualCasting(true);
                }
            }
            // Fire from the origin caster (visual node) — same method/effects as a hand
            // cast; only the spawn location differs when a_stanceOrigin chose the head.
            originCaster->CastSpellImmediate(a_spell, false, nullptr, 1.0f, false, 0.0f, a_player);
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

        void CastShoutNow(RE::PlayerCharacter* a_player, RE::TESShout* a_shout,
                          const std::string& a_name, int a_level)
        {
            // a_level (0/1/2) selects the spoken word level ("Fus"=0, "Fus Ro"=1,
            // "Fus Ro Dah"=2). -1 (shouldn't normally happen) = highest unlocked.
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

            // Shout cooldown: the engine stores the running cooldown in the player's
            // HighProcessData.voiceRecoveryTime; each word level carries its own
            // recoveryTime. We enforce cooldown HERE (the "can't shout while it's
            // recovering" rule) instead of CheckCast — CheckCast on a voice spell via
            // the voice caster always fails, which was silently blocking every shout.
            RE::HighProcessData* high = nullptr;
            if (auto* proc = a_player->GetActorRuntimeData().currentProcess) {
                high = proc->high;
            }
            if (high && high->voiceRecoveryTime > 0.0f) {
                logger::info("[cast] shout '{}' on cooldown ({:.1f}s left) — ignored",
                    a_name, high->voiceRecoveryTime);
                return;
            }

            // Cast the word-level spell DIRECTLY from the power/shout caster
            // (CastingSource::kInstant -> magicCasters[kPowerOrShout], the same caster
            // the engine uses for shouts). This is the in-engine cast — no equip +
            // simulated shout-key (DBU's method, which the design explicitly avoids).
            auto* caster = a_player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
            if (!caster) {
                logger::warn("[cast] no power/shout caster for shout '{}'", a_name);
                return;
            }
            caster->CastSpellImmediate(spell, false, nullptr, 1.0f, false, 0.0f, a_player);

            // Start the cooldown + record the active shout (so the engine/UI sees it).
            // We set voiceRecoveryTime to the engine's OWN per-word value (variation.
            // recoveryTime) because casting the word-spell directly bypasses the shout
            // handler that would normally set it — so without this there'd be no cooldown
            // at all. We are not inventing a number; it's exactly what a manual shout uses.
            if (high) {
                // Apply the player's shout-recovery multiplier (kShoutRecoveryMult) so
                // cooldown-reducing effects/items (e.g. Blessing of Talos = 0.8) work
                // exactly as they do for a manual shout. Computed at cast time, same as
                // the engine does — a mid-cooldown change doesn't retroactively apply.
                float mult = a_player->AsActorValueOwner()->GetActorValue(
                    RE::ActorValue::kShoutRecoveryMult);
                if (mult <= 0.0f) mult = 1.0f;  // unmodified default is 1.0; guard bad reads
                high->voiceRecoveryTime = variation.recoveryTime * mult;
                high->currentShout      = a_shout;
                high->currentShoutVariation =
                    static_cast<RE::TESShout::VariationID>(level);
            }

            // Notify shout-detection mods (Thunderchild, Forceful Tongue, etc.) by
            // firing the SAME engine event a manual shout fires. This is an OBSERVER
            // event (the engine fires it to notify listeners; it does not itself trigger
            // a cast), so sending it makes those mods react WITHOUT doubling the effect.
            if (auto* src = RE::ShoutAttack::GetEventSource()) {
                RE::ShoutAttack::Event evt{ a_shout };
                src->SendEvent(&evt);
            }

            // Optional: play the visible shout body animation. Vanilla uses ONE shout
            // animation regardless of word level, driven by the behavior-graph event
            // "shoutStart" (paired with "shoutStop"). CastSpellImmediate bypasses the
            // shout-key pipeline that would fire it, so we fire it ourselves. A wrong/
            // absent event name is a harmless no-op. We ALWAYS schedule a paired
            // "shoutStop" shortly after so the pose can never get stuck.
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
                         "casting {}, cooldown {:.1f}s)",
                a_name, level + 1, spell->GetFormID(),
                static_cast<int>(spell->GetDelivery()),
                static_cast<int>(spell->GetCastingType()),
                variation.recoveryTime);
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
                }
                break;
            case Category::Power:
                if (auto* power = a_form->As<RE::SpellItem>()) CastPowerNow(a_player, power, a_name);
                break;
            case Category::Shout:
                if (auto* shout = a_form->As<RE::TESShout>()) CastShoutNow(a_player, shout, a_name, a_shoutLevel);
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
