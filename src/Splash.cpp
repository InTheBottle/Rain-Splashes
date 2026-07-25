#include "Splash.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>

namespace RainSplashes::Splash
{
	namespace
	{
		constexpr const char* kModel = "RainSplash\\soggyfeet_splash.nif";
		constexpr const char* kConfig = "Data/SKSE/Plugins/RainSplashes.ini";

		constexpr std::uint32_t kParticleFlags = 7;
		constexpr float         kMaxDistance = 2500.0f;
		constexpr float         kLifetime = 2.0f;
		constexpr float         kMinScale = 0.80f;
		constexpr float         kMaxScale = 1.15f;
		constexpr float         kCooldown = 0.12f;
		constexpr float         kSlotExpiry = 5.0f;
		constexpr float         kProbeAbove = 6.0f;
		constexpr float         kProbeBelow = 48.0f;
		constexpr float         kMinGroundNormalZ = 0.30f;

		float       g_scale = 1.0f;
		std::size_t g_maxNPCs = 12;

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

		std::string_view Trim(std::string_view a_text)
		{
			const auto first = a_text.find_first_not_of(" \t\r");
			const auto last = a_text.find_last_not_of(" \t\r");
			return first == std::string_view::npos ? std::string_view{} : a_text.substr(first, last - first + 1);
		}

		template <class T>
		void Read(std::string_view a_value, T& a_out, T a_min, T a_max)
		{
			T parsed{};
			if (std::from_chars(a_value.data(), a_value.data() + a_value.size(), parsed).ec == std::errc{}) {
				a_out = std::clamp(parsed, a_min, a_max);
			}
		}

		// Two keys, so an ini library would be more plumbing than parse.
		void LoadConfig()
		{
			std::ifstream file{ kConfig };
			std::string   line;

			while (std::getline(file, line)) {
				const auto separator = line.find('=');
				if (separator == std::string::npos || Trim(line).starts_with(';')) {
					continue;
				}

				const std::string_view text{ line };
				const auto             key = Trim(text.substr(0, separator));
				const auto             value = Trim(text.substr(separator + 1));

				if (key == "Scale") {
					Read(value, g_scale, 0.1f, 5.0f);
				} else if (key == "MaxNPCs") {
					Read(value, g_maxNPCs, std::size_t{ 0 }, std::size_t{ 200 });
				}
			}

			SKSE::log::info("Scale {}, MaxNPCs {}", g_scale, g_maxNPCs);
		}

		std::mutex                            g_slotLock;
		std::unordered_map<RE::FormID, float> g_npcSlots;
		float                                 g_playerSlot = 0.0f;

		// A stride fires several footstep events, so throttle to one per footfall. NPCs
		// also compete for a limited number of slots so a crowd cannot flood the scene;
		// the player keeps its own.
		bool TakeSlot(RE::FormID a_id, bool a_isPlayer, float a_now)
		{
			std::scoped_lock lock(g_slotLock);

			float* last = &g_playerSlot;

			if (!a_isPlayer) {
				std::erase_if(g_npcSlots,
					[a_now](const auto& a_slot) { return a_now - a_slot.second > kSlotExpiry; });

				const auto it = g_npcSlots.find(a_id);
				if (it == g_npcSlots.end()) {
					if (g_npcSlots.size() >= g_maxNPCs) {
						return false;
					}
					g_npcSlots.emplace(a_id, a_now);
					return true;
				}

				last = &it->second;
			}

			if (a_now - *last < kCooldown) {
				return false;
			}

			*last = a_now;
			return true;
		}

		RE::NiPoint3 FromHavok(const RE::hkVector4& a_vector)
		{
			float values[4]{};
			_mm_storeu_ps(values, a_vector.quad);
			return { values[0], values[1], values[2] };
		}

		// Columns are the local axes in world space, so local Z lands on a_up.
		RE::NiMatrix3 UprightOn(const RE::NiPoint3& a_up)
		{
			const RE::NiPoint3 aside =
				std::fabs(a_up.x) < 0.9f ? RE::NiPoint3{ 1.0f, 0.0f, 0.0f } : RE::NiPoint3{ 0.0f, 1.0f, 0.0f };

			RE::NiPoint3 right = aside.Cross(a_up);
			right.Unitize();
			const RE::NiPoint3 forward = a_up.Cross(right);

			return { RE::NiPoint3{ right.x, forward.x, a_up.x },
				RE::NiPoint3{ right.y, forward.y, a_up.y },
				RE::NiPoint3{ right.z, forward.z, a_up.z } };
		}

		struct Placement
		{
			RE::NiPoint3  position;
			RE::NiMatrix3 rotation;
		};

		// Ankle bones ride above the surface the foot stands on, so the ground has to be
		// picked for. The planted foot is the one with the least clearance over it -
		// comparing ankle heights alone inverts on a slope, where the trailing foot hangs
		// lower than the one that just landed.
		Placement FootPlacement(RE::Actor* a_actor, RE::TESObjectCELL* a_cell)
		{
			static const RE::BSFixedString feet[]{ "NPC L Foot [Lft ]", "NPC R Foot [Rft ]" };

			// Fallback for first person 3D, which has no foot bones.
			Placement placement{ a_actor->GetPosition(), {} };

			auto* world = a_cell->GetbhkWorld();
			if (!world) {
				return placement;
			}

			const float havok = RE::bhkWorld::GetWorldScale();
			float       best = (std::numeric_limits<float>::max)();

			for (const auto& name : feet) {
				auto* foot = a_actor->GetNodeByName(name);
				if (!foot) {
					continue;
				}

				const RE::NiPoint3 ankle = foot->world.translate;
				const RE::NiPoint3 from{ ankle.x, ankle.y, ankle.z + kProbeAbove };
				const RE::NiPoint3 to{ ankle.x, ankle.y, ankle.z - kProbeBelow };

				RE::bhkPickData pick;
				pick.rayInput.from = from * havok;
				pick.rayInput.to = to * havok;
				pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
				{
					RE::BSReadLockGuard lock(world->worldLock);
					world->PickObject(pick);
				}

				if (!pick.rayOutput.HasHit()) {
					continue;
				}

				// The probe starts inside the actor's own leg, and whether the collision
				// filter drops bipeds is runtime data.
				auto*              ref = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
				const RE::NiPoint3 normal = FromHavok(pick.rayOutput.normal);
				if ((ref && ref->As<RE::Actor>()) || normal.z < kMinGroundNormalZ) {
					continue;
				}

				const RE::NiPoint3 ground = from + (to - from) * pick.rayOutput.hitFraction;
				if (const float clearance = ankle.z - ground.z; clearance < best) {
					best = clearance;
					placement = { ground, UprightOn(normal) };
				}
			}

			return placement;
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

			const RE::FormID id = a_actor->GetFormID();
			if (!TakeSlot(id, a_actor == player, NowSeconds())) {
				return;
			}

			const float scale = RandomInRange(kMinScale, kMaxScale) * g_scale;

			SKSE::GetTaskInterface()->AddTask([id, scale]() {
				auto* form = RE::TESForm::LookupByID(id);
				auto* actor = form ? form->As<RE::Actor>() : nullptr;
				auto* cell = actor ? actor->GetParentCell() : nullptr;
				if (!cell || !actor->Get3D()) {
					return;
				}

				// Picking the havok world wants the main thread.
				const Placement placement = FootPlacement(actor, cell);

				RE::BSTempEffectParticle::Spawn(cell, kLifetime, kModel, placement.rotation,
					placement.position, scale, kParticleFlags, nullptr);
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
		LoadConfig();

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
		std::scoped_lock lock(g_slotLock);
		g_npcSlots.clear();
		g_playerSlot = 0.0f;
	}
}
