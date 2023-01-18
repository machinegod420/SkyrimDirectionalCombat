#pragma once
#include "DirectionHandler.h"
#include "AIHandler.h"
#include "BlockHandler.h"
#include "AttackHandler.h"
#include "FXHandler.h"
#include "Utils.h"
#include "InputHandler.h"
#include "3rdparty/PrecisionAPI.h"

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

			_OnMeleeHit = Trampoline.write_call<5>(hook.address() + REL::VariantOffset(0x3C0, 0x4A8, 0).offset(), OnMeleeHit);
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

	// called before melee hit has been processed
	//https://github.com/D7ry/valhallaCombat/blob/Master/src/include/Hooks.h#L202
	class HookBeginMeleeHit
	{
	public:
		static void Install()
		{
			REL::Relocation<std::uintptr_t> OnMeleeHitBase{ RELOCATION_ID(37650, 38603) };
			auto& trampoline = SKSE::GetTrampoline();
			_OnBeginMeleeHit = trampoline.write_call<5>(OnMeleeHitBase.address() + REL::Relocate(0x38B, 0x45A), OnBeginMeleeHit);
			logger::info("Hook BeginMeleeHit");
			logger::info("WARNING: THIS MOD MAY NOT RETURN ON RELOCATION_ID(37650, 38603). THIS MAY RESULT IN COMPATIBILITY ISSUES WITH OTHER SKSE PLUGINS");

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
			_Update = trampoline.write_call<5>(UpdateBase.address() + REL::Relocate(0x748, 0xC26), Update);
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
			_PerformAttackAction = trampoline.write_call<5>(AttackActionBase.address() + REL::Relocate(0x4D7, 0x435), PerformAttackAction);
			logger::info("Hook AI attack");
			logger::info("WARNING: THIS MOD MAY NOT RETURN ON RELOCATION_ID(48139, 49170). THIS MAY RESULT IN COMPATIBILITY ISSUES WITH OTHER SKSE PLUGINS");
		}

	private:
		static bool PerformAttackAction(RE::TESActionData* a_actionData);

		static inline REL::Relocation<decltype(PerformAttackAction)> _PerformAttackAction;
	};

	//https://github.com/max-su-2019/SCAR/blob/main/src/Hook_AttackStart.h#L95
	class HookHasAttackAngle
	{
	public:
		static void Install()
		{
			auto& trampoline = SKSE::GetTrampoline();

			REL::Relocation<std::uintptr_t> AttackDistanceBase{ RELOCATION_ID(48139, 49170) };
			_GetAttackAngle = trampoline.write_call<5>(AttackDistanceBase.address() + REL::Relocate(0x493, 0x3F1), GetAttackAngle);
			logger::info("Hook AI Check Attack Angle");
		}

	private:
		static bool GetAttackAngle(RE::Actor* a_attacker, RE::Actor* a_target, const RE::NiPoint3& a3, const RE::NiPoint3& a4, RE::BGSAttackData* a_attackData, float a6, void* a7, bool a8);

		static inline REL::Relocation<decltype(GetAttackAngle)> _GetAttackAngle;
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
			_NotifyAnimationGraph_NPC = AnimGraphVtbl_PC.write_vfunc(0x1, NotifyAnimationGraph_NPC);
		}
		static bool NotifyAnimationGraph_PC(RE::IAnimationGraphManagerHolder* a_graphHolder, const RE::BSFixedString& eventName);
		static bool NotifyAnimationGraph_NPC(RE::IAnimationGraphManagerHolder* a_graphHolder, const RE::BSFixedString& eventName);
		static inline REL::Relocation<decltype(NotifyAnimationGraph_PC)> _NotifyAnimationGraph_PC;
		static inline REL::Relocation<decltype(NotifyAnimationGraph_NPC)> _NotifyAnimationGraph_NPC;
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