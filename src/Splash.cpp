#include "Splash.h"

#include <chrono>
#include <mutex>
#include <random>
#include <unordered_map>

namespace RainSplashes::Splash
{
	namespace
	{
		// Relative to Data/meshes/.
		constexpr const char* kModel = "RainSplash\\soggyfeet_splash.nif";

		constexpr std::uint32_t kParticleFlags = 7;

		std::mt19937& RNG()
		{
			static thread_local std::mt19937 rng{ std::random_device{}() };
			return rng;
		}

		float RandomInRange(float a_min, float a_max)
		{
			std::uniform_real_distribution<float> dist(a_min, a_max);
			return dist(RNG());
		}

		float NowSeconds()
		{
			using namespace std::chrono;
			return duration<float>(steady_clock::now().time_since_epoch()).count();
		}

		std::mutex                            g_cooldownLock;
		std::unordered_map<RE::FormID, float> g_lastSplash;

		// A stride fires several footstep events, so throttle to one per footfall.
		bool TakeCooldown(RE::FormID a_id, float a_now)
		{
			std::scoped_lock lock(g_cooldownLock);

			if (g_lastSplash.size() > 256) {
				std::erase_if(g_lastSplash,
					[a_now](const auto& a_entry) { return a_now - a_entry.second > 30.0f; });
			}

			const auto [it, inserted] = g_lastSplash.try_emplace(a_id, a_now);
			if (inserted) {
				return true;
			}

			if (a_now - it->second < kCooldown) {
				return false;
			}

			it->second = a_now;
			return true;
		}

		// The lower foot is the planted one; avoids relying on event tag names.
		RE::NiAVObject* PlantedFoot(RE::Actor* a_actor)
		{
			static const RE::BSFixedString leftFoot{ "NPC L Foot [Lft ]" };
			static const RE::BSFixedString rightFoot{ "NPC R Foot [Rft ]" };

			auto* left = a_actor->GetNodeByName(leftFoot);
			auto* right = a_actor->GetNodeByName(rightFoot);

			if (!left) {
				return right;
			}
			if (!right) {
				return left;
			}

			return left->world.translate.z <= right->world.translate.z ? left : right;
		}

		void OnFootstep(RE::Actor* a_actor)
		{
			if (!a_actor || a_actor->IsDead() || !a_actor->Get3D()) {
				return;
			}

			if (auto* state = a_actor->AsActorState(); state && (state->IsSwimming() || state->IsFlying())) {
				return;
			}

			// IsRaining() already accounts for precipitation fade in and out.
			const auto* sky = RE::Sky::GetSingleton();
			if (!sky || sky->mode.get() != RE::Sky::Mode::kFull || !sky->IsRaining()) {
				return;
			}

			const auto* cell = a_actor->GetParentCell();
			if (!cell || !cell->IsExteriorCell()) {
				return;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || a_actor->GetPosition().GetDistance(player->GetPosition()) > kMaxDistance) {
				return;
			}

			if (!TakeCooldown(a_actor->GetFormID(), NowSeconds())) {
				return;
			}

			const float scale = RandomInRange(kMinScale, kMaxScale);

			// First person 3D has no foot bones, so fall back to the actor itself.
			auto* foot = PlantedFoot(a_actor);

			// The foot bone rides at ankle height, so sit the effect on the ground.
			const RE::NiPoint3 base = a_actor->GetPosition();
			const RE::NiPoint3 position =
				foot ? RE::NiPoint3{ foot->world.translate.x, foot->world.translate.y, base.z } : base;

			const RE::FormID id = a_actor->GetFormID();

			SKSE::GetTaskInterface()->AddTask([id, position, scale]() {
				auto* form = RE::TESForm::LookupByID(id);
				auto* actor = form ? form->As<RE::Actor>() : nullptr;
				if (!actor) {
					return;
				}

				auto* cell = actor->GetParentCell();
				if (!cell) {
					return;
				}

				// NiMatrix3 defaults to identity; the NiPoint3 overload is ambiguous.
				RE::BSTempEffectParticle::Spawn(cell, kLifetime, kModel, RE::NiMatrix3(), position,
					scale, kParticleFlags, nullptr);
			});
		}

		class FootstepSink : public RE::BSTEventSink<RE::BGSFootstepEvent>
		{
		public:
			static FootstepSink* GetSingleton()
			{
				static FootstepSink singleton;
				return std::addressof(singleton);
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::BGSFootstepEvent*                 a_event,
				RE::BSTEventSource<RE::BGSFootstepEvent>*) override
			{
				if (a_event) {
					if (const auto actor = a_event->actor.get()) {
						OnFootstep(actor.get());
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			FootstepSink() = default;
		};
	}

	void Install()
	{
		auto* manager = RE::BGSFootstepManager::GetSingleton();
		if (!manager) {
			SKSE::log::error("BGSFootstepManager unavailable - footstep splashes disabled");
			return;
		}

		manager->AddEventSink(FootstepSink::GetSingleton());
		SKSE::log::info("Listening for footsteps");
	}

	void Reset()
	{
		std::scoped_lock lock(g_cooldownLock);
		g_lastSplash.clear();
	}
}
