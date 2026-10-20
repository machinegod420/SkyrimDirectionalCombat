#pragma once
#include "DirectionHandler.h"
#include "AIHandler.h"
#include "BlockHandler.h"
#include "AttackHandler.h"
#include "DodgeHandler.h"
#include "FXHandler.h"
#include "Utils.h"
#include "InputHandler.h"
#include "3rdparty/PrecisionAPI.h"
#include "RE/C/CombatController.h"

namespace Hooks
{
	class PrecisionCallback
	{
	public:
		static PRECISION_API::PreHitCallbackReturn PrecisionPrehit(const PRECISION_API::PrecisionHitData& a_precisionHitData);
	};
	
	//https://github.com/D7ry/valhallaCombat/blob/Master/src/include/Hooks.h#L61
	class HookOnMeleeHit
	{
	public:
		static void Install()
		{
			REL::Relocation<uintptr_t> hook{ RELOCATION_ID(37673, 38627) };
			
			SKSE::Trampoline& Trampoline = SKSE::GetTrampoline();
			// for 1.7 looks like its the same hook location as 1.6
			const std::uintptr_t Site = hook.address() + REL::VariantOffset(0x3C0, 0x4A8, 0).offset();
			CheckCallSite(Site, "melee hit", true);
			_OnMeleeHit = Trampoline.write_call<5>(Site, OnMeleeHit);
			logger::info("Hook OnMeleeHit");
		}
		static void OnMeleeHit(RE::Actor* target, RE::HitData& hitData);
		static inline REL::Relocation<decltype(OnMeleeHit)> _OnMeleeHit;
	};

	//https://github.com/D7ry/valhallaCombat/blob/Master/src/include/Hooks.h#L181
	class HookProjectileHit
	{
	public:
		static void Install()
		{
			REL::Relocation<std::uintptr_t> arrowProjectileVtbl{ RE::VTABLE_ArrowProjectile[0] };
			REL::Relocation<std::uintptr_t> missileProjectileVtbl{ RE::VTABLE_MissileProjectile[0] };

			_OnArrowHit = arrowProjectileVtbl.write_vfunc(0xBE, OnArrowHit);
			_OnMissileHit = missileProjectileVtbl.write_vfunc(0xBE, OnMissileHit);

			logger::info("Hook ProjectileHit");
		};
		static void OnArrowHit(RE::Projectile* a_this, RE::hkpAllCdPointCollector* a_AllCdPointCollector);
		static void OnMissileHit(RE::Projectile* a_this, RE::hkpAllCdPointCollector* a_AllCdPointCollector);
		static inline REL::Relocation<decltype(OnArrowHit)> _OnArrowHit;
		static inline REL::Relocation<decltype(OnMissileHit)> _OnMissileHit;
	};

	// A fencer rates a spell only for its left hand: the vanilla tree would otherwise swap
	// the weapon out for it, and each swap back rebuilt the AI record. Vtable slots, so no AE offsets.
	class HookSpellScore
	{
	public:
		static void Install()
		{
			// CombatInventoryItemMagic is abstract: every live spell item is a CombatInventoryItemMagicT
			// instantiation (one per caster kind) with its own vtable copy, so each copy gets the slot.
			// All copies share the base CalculateScore, hence one saved original.
			static constexpr std::array SpellItemVtables{
				RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterOffensive_[0],
				RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterScript_[0],
				RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterParalyze_[0],
				RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterReanimate_[0],
				RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterTargetEffect_[0],
				RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterArmor_[0],
				RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterBoundItem_[0],
				RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterInvisibility_[0],
				RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterLight_[0],
				RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterCloak_[0],
				RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterDisarm_[0],
				RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterStagger_[0],
				RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterSummon_[0],
				RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterRestore_[0],
				RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterWard_[0],
			};
			std::uintptr_t Original = 0;
			for (const auto& Vtbl : SpellItemVtables)
			{
				REL::Relocation<std::uintptr_t> V{ Vtbl };
				const auto Prev = V.write_vfunc(0xC, CalculateScore);
				if (!Original)
				{
					Original = Prev;
				}
				else if (Prev != Original)
				{
					logger::warn("SpellScore: a spell item vtable overrides CalculateScore, hook falls back to the base");
				}
			}
			_CalculateScore = Original;
			logger::info("Hook SpellScore");
		};
		static float CalculateScore(RE::CombatInventoryItemMagic* a_this, RE::CombatController* a_controller);
		static inline REL::Relocation<decltype(CalculateScore)> _CalculateScore;
	};

	// called before melee hit has been processed
	//https://github.com/D7ry/valhallaCombat/blob/Master/src/include/Hooks.h#L202
	class HookBeginMeleeHit
	{
	public:
		static void Install()
		{
			REL::Relocation<std::uintptr_t> OnMeleeHitBase{ RELOCATION_ID(37650, 38603) };
			auto& trampoline = SKSE::GetTrampoline();
			const std::uintptr_t Site = OnMeleeHitBase.address() + REL::Relocate(0x38B, 0x45A);
			// The call into DealDamage.
			CheckCallSite(Site, "begin melee hit", true, [](std::uintptr_t a_target) {
				return a_target == REL::Relocation<std::uintptr_t>{ RELOCATION_ID(37673, 38627) }.address();
			});
			_OnBeginMeleeHit = trampoline.write_call<5>(Site, OnBeginMeleeHit);
			logger::info("Hook BeginMeleeHit");

		}
		static void OnBeginMeleeHit(RE::Actor* attacker, RE::Actor* target, std::int64_t a_int1, bool a_bool, void* a_unkptr);

		static inline REL::Relocation<decltype(OnBeginMeleeHit)> _OnBeginMeleeHit;
	};

	//https://github.com/ersh1/Precision/blob/ecff7bbb9dbd21df8f2c74c2b06091135201d360/src/Hooks.h#L13
	class HookUpdate
	{
	public:
		static void Install()
		{
			REL::Relocation<std::uintptr_t> UpdateBase{ RELOCATION_ID(35565, 36564) };
			auto& trampoline = SKSE::GetTrampoline();
			// AE moved this call in 1.7.99: 0xC26 before it, 0xC38 from it on (Precision's offsets).
			const std::size_t Offset = REL::VariantOffset(0x748,
				REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_7_99) ? 0xC38 : 0xC26, 0x748).offset();
			// The call is to a nullsub (ret).
			CheckCallSite(UpdateBase.address() + Offset, "main update", true, [](std::uintptr_t a_target) {
				const auto Op = *reinterpret_cast<const std::uint8_t*>(a_target);
				return Op == 0xC3 || Op == 0xC2;
			});
			_Update = trampoline.write_call<5>(UpdateBase.address() + Offset, Update);
			logger::info("Hook Update");

		}
		static void Update();

		static inline REL::Relocation<decltype(Update)> _Update;
	};



	class HookCharacter
	{
	public:
		static void Install()
		{
			REL::Relocation<std::uintptr_t> CharacterVtbl{ RE::VTABLE_Character[0] };
			_Update = CharacterVtbl.write_vfunc(0xAD, Update);
			logger::info("Hook CharacterUpdate");
		}

		static void Update(RE::Actor* a_this, float a_delta);

		static inline REL::Relocation<decltype(Update)> _Update;
	};

	class HookPlayerCharacter
	{
	public:
		static void Install()
		{
			REL::Relocation<std::uintptr_t> PlayerCharacterVtbl{ RE::VTABLE_PlayerCharacter[0] };
			_Update = PlayerCharacterVtbl.write_vfunc(0xAD, Update);
			logger::info("Hook PlayerUpdate");
		}

		static void Update(RE::Actor* a_this, float a_delta);

		static inline REL::Relocation<decltype(Update)> _Update;
	};

	class HookMouseMovement
	{
	public:
		static void Install()
		{
			REL::Relocation<std::uintptr_t> LookHandlerVtbl{ RE::VTABLE_LookHandler[0] };
			_ProcessThumbstick = LookHandlerVtbl.write_vfunc(0x2, ProcessThumbstick);
			_ProcessMouseMove = LookHandlerVtbl.write_vfunc(0x3, ProcessMouseMove);
			logger::info("Hook MouseMovement");
		}
		// shared implementation
		static void SharedInputMNB(int x, int y);
		static void SharedInputForHonor(int x, int y);
		static void SharedInputKCD(int x, int y);
		static void SharedInputMouse(int x, int y);
		static void SharedInput(int x, int y);
		static void ProcessThumbstick(RE::LookHandler* a_this, RE::ThumbstickEvent* a_event, RE::PlayerControlsData* a_data);
		static void ProcessMouseMove(RE::LookHandler* a_this, RE::MouseMoveEvent* a_event, RE::PlayerControlsData* a_data);

		static inline REL::Relocation<decltype(ProcessThumbstick)> _ProcessThumbstick;
		static inline REL::Relocation<decltype(ProcessMouseMove)> _ProcessMouseMove;
	};

	// AI attack action only, has no effect on player
	//https://github.com/D7ry/valhallaCombat/blob/Master/src/include/Hooks.h#L220
	class HookOnAttackAction
	{
	public:
		static void Install()
		{
			auto& trampoline = SKSE::GetTrampoline();
			REL::Relocation<std::uintptr_t> AttackActionBase{ RELOCATION_ID(48139, 49170) };
			const std::uintptr_t Site = AttackActionBase.address() + REL::Relocate(0x4D7, 0x435);
			// The call is a virtual-call stub (mov rax,[rcx]; jmp [rax+28h]). The compiler folds
			// identical stubs, so its bytes count as well as CommonLib's address for it.
			CheckCallSite(Site, "attack action", true, [](std::uintptr_t a_target) {
				static constexpr std::uint8_t Stub[] = { 0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, 0x28 };
				return a_target == REL::Relocation<std::uintptr_t>{ RELOCATION_ID(43239, 44440) }.address() ||
					std::memcmp(reinterpret_cast<const void*>(a_target), Stub, sizeof(Stub)) == 0;
			});
			_PerformAttackAction = trampoline.write_call<5>(Site, PerformAttackAction);
			logger::info("Hook AI attack");
			logger::info("WARNING: THIS MOD MAY NOT RETURN ON RELOCATION_ID(48139, 49170). THIS MAY RESULT IN COMPATIBILITY ISSUES WITH OTHER SKSE PLUGINS");
		}

	private:
		static bool PerformAttackAction(RE::TESActionData* a_actionData);

		static inline REL::Relocation<decltype(PerformAttackAction)> _PerformAttackAction;
	};
	class HookAttackRange
	{

	};



	class HookAttackHandler
	{
	public:
		static void Install()
		{
			REL::Relocation<std::uintptr_t> hook{ RE::VTABLE_AttackBlockHandler[0]};
			_ProcessAttackHook = hook.write_vfunc(0x4, ProcessAttackHook);
			logger::info("Hook attack block handler");
			logger::info("WARNING: THIS MOD MAY NOT RETURN ON VTABLE_AttackBlockHandler. THIS MAY RESULT IN COMPATIBILITY ISSUES WITH OTHER SKSE PLUGINS");
		}

		static bool CanPlayerAttack();
		// Feedback for a press CanPlayerAttack turned down, whatever the reason.
		static void OnPlayerAttackRefused();
	private:
		static bool	ProcessAttackHook(RE::AttackBlockHandler* handler, RE::ButtonEvent* a_event, RE::PlayerControlsData* a_data);

		static inline REL::Relocation<decltype(ProcessAttackHook)> _ProcessAttackHook;
	};


	class HookNotifyAnimationGraph
	{
	public:
		static void Install()
		{
			REL::Relocation<uintptr_t> AnimGraphVtbl_PC { RE::VTABLE_PlayerCharacter[3] };
			REL::Relocation<uintptr_t> AnimGraphVtbl_NPC{ RE::VTABLE_Character[3] };
			_NotifyAnimationGraph_PC = AnimGraphVtbl_PC.write_vfunc(0x1, NotifyAnimationGraph_PC);
			_NotifyAnimationGraph_NPC = AnimGraphVtbl_NPC.write_vfunc(0x1, NotifyAnimationGraph_NPC);
		}
		static bool NotifyAnimationGraph_PC(RE::IAnimationGraphManagerHolder* a_graphHolder, const RE::BSFixedString& eventName);
		static bool NotifyAnimationGraph_NPC(RE::IAnimationGraphManagerHolder* a_graphHolder, const RE::BSFixedString& eventName);
		static inline REL::Relocation<decltype(NotifyAnimationGraph_PC)> _NotifyAnimationGraph_PC;
		static inline REL::Relocation<decltype(NotifyAnimationGraph_NPC)> _NotifyAnimationGraph_NPC;
	};



	// Dynamic spacing: rewrites Skyrim's combat engagement band per actor, so
	// NPCs give ground when winded or locked out and press when they hold the
	// advantage.
	class HookCombatAdvanceRadius
	{
	public:
		static void Install();
		static void ComputeAdvanceRadii(RE::Actor* a_actor, RE::Actor* a_target, float* a_outInner, float* a_outOuter);
		static inline decltype(&ComputeAdvanceRadii) _ComputeAdvanceRadii = nullptr;
	};

	// Character::ProcessMotionData: this frame's root-motion translation, actor-local, y the
	// animmotion forward axis. AMR patches calls inside it, so the entry detour sees its curves too.
	class HookProcessMotionData
	{
	public:
		static void Install();
		static bool ProcessMotionData(RE::Character* a_this, float a_dt, RE::NiPoint3* a_translation, RE::NiPoint3* a_rotation, bool* a_flag);
		static inline decltype(&ProcessMotionData) _ProcessMotionData = nullptr;
	};

	// Actor::ApplyWeaponHitSpellItems: casts the weapon's enchantment, its poison and perk hit
	// spells. The engine calls it before the hit data exists, so a block never stopped them.
	class HookWeaponHitSpells
	{
	public:
		static void Install();
		static void ApplyWeaponHitSpellItems(RE::Actor* a_attacker, RE::InventoryEntryData* a_weapon, bool a_leftHand, RE::TESObjectREFR* a_target);
		static inline decltype(&ApplyWeaponHitSpellItems) _ApplyWeaponHitSpellItems = nullptr;
	};

	// hooks bhkCharacterStateOnGround::SimulateStatePhysics (vfunc 8) so we can write
	// velocityMod inside the physics tick — writing it from a game-logic update doesn't
	// stick because the controller's solver consumes/decays it on the same tick.
	class HookCharacterStateOnGround
	{
	public:
		static void Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_bhkCharacterStateOnGround[0] };
			_SimulateStatePhysics = vtbl.write_vfunc(8, SimulateStatePhysics);
			logger::info("Hook bhkCharacterStateOnGround::SimulateStatePhysics");
		}
		static void SimulateStatePhysics(RE::bhkCharacterStateOnGround* a_this, RE::bhkCharacterController* a_controller);
		static inline REL::Relocation<decltype(SimulateStatePhysics)> _SimulateStatePhysics;
	};

	// cancels an active dodge as soon as the actor leaves the ground (stairs, ledges, etc.)
	class HookCharacterStateInAir
	{
	public:
		static void Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_bhkCharacterStateInAir[0] };
			_SimulateStatePhysics = vtbl.write_vfunc(8, SimulateStatePhysics);
			logger::info("Hook bhkCharacterStateInAir::SimulateStatePhysics");
		}
		static void SimulateStatePhysics(RE::bhkCharacterStateInAir* a_this, RE::bhkCharacterController* a_controller);
		static inline REL::Relocation<decltype(SimulateStatePhysics)> _SimulateStatePhysics;
	};

	class HookAnimEvent
	{
	public:
		static void Install()
		{
			REL::Relocation<uintptr_t> AnimEventVtbl_NPC{ RE::VTABLE_Character[2] };
			REL::Relocation<uintptr_t> AnimEventVtbl_PC{ RE::VTABLE_PlayerCharacter[2] };
			_ProcessEvent_NPC = AnimEventVtbl_NPC.write_vfunc(0x1, ProcessEvent_NPC);
			_ProcessEvent_PC = AnimEventVtbl_PC.write_vfunc(0x1, ProcessEvent_PC);

			logger::info("Hook ProccessAnimEvent");
		}

	private:
		// shared implementation
		static void ProcessCharacterEvent(RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_sink, RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_eventSource);
		static RE::BSEventNotifyControl ProcessEvent_NPC(RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_sink, RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_eventSource);
		static RE::BSEventNotifyControl ProcessEvent_PC(RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_sink, RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_eventSource);

		static inline REL::Relocation<decltype(ProcessEvent_NPC)> _ProcessEvent_NPC;
		static inline REL::Relocation<decltype(ProcessEvent_PC)> _ProcessEvent_PC;
	};


	class Hooks
	{
	public:
		static void Install();
	};

}