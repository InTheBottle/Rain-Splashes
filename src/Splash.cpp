#include "Splash.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <span>
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

		// steady_clock counts from system boot, and a float holds seconds that large
		// to worse than kCooldown once the machine has been up a fortnight. Counting
		// from plugin load instead keeps the magnitude small enough to stay exact.
		const std::chrono::steady_clock::time_point g_epoch = std::chrono::steady_clock::now();

		float NowSeconds()
		{
			return std::chrono::duration<float>(std::chrono::steady_clock::now() - g_epoch).count();
		}

		std::string_view Trim(std::string_view a_text)
		{
			const auto first = a_text.find_first_not_of(" \t\r");
			const auto last = a_text.find_last_not_of(" \t\r");
			return first == std::string_view::npos ? std::string_view{} : a_text.substr(first, last - first + 1);
		}

		bool KeyIs(std::string_view a_key, std::string_view a_name)
		{
			return std::ranges::equal(a_key, a_name, [](char a_lhs, char a_rhs) {
				return std::tolower(static_cast<unsigned char>(a_lhs)) ==
				       std::tolower(static_cast<unsigned char>(a_rhs));
			});
		}

		// Anything the ini asks for that does not survive gets said out loud, so a
		// typo does not look like the plugin quietly ignoring the file.
		template <class T>
		void Read(std::string_view a_key, std::string_view a_value, T& a_out, T a_min, T a_max)
		{
			T parsed{};
			if (std::from_chars(a_value.data(), a_value.data() + a_value.size(), parsed).ec != std::errc{}) {
				SKSE::log::warn("{} is not a number, keeping {}", a_key, a_out);
				return;
			}

			a_out = std::clamp(parsed, a_min, a_max);
			if (a_out != parsed) {
				SKSE::log::warn("{} out of range, clamped to {}", a_key, a_out);
			}
		}

		// Two keys, so an ini library would be more plumbing than parse.
		void LoadConfig()
		{
			std::ifstream file{ kConfig };
			std::string   line;

			while (std::getline(file, line)) {
				// Notepad still writes a BOM, which would otherwise glue itself to the first key.
				if (line.starts_with("\xEF\xBB\xBF")) {
					line.erase(0, 3);
				}

				const auto separator = line.find('=');
				if (separator == std::string::npos || Trim(line).starts_with(';')) {
					continue;
				}

				const std::string_view text{ line };
				const auto             key = Trim(text.substr(0, separator));
				const auto             value = Trim(text.substr(separator + 1));

				if (KeyIs(key, "Scale")) {
					Read(key, value, g_scale, 0.1f, 5.0f);
				} else if (KeyIs(key, "MaxNPCs")) {
					Read(key, value, g_maxNPCs, std::size_t{ 0 }, std::size_t{ 200 });
				} else {
					SKSE::log::warn("Ignoring unknown key {}", key);
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

		enum class Foot
		{
			kLeft,
			kRight,
			kEither
		};

		std::optional<Foot> FootFromTag(std::string_view a_tag)
		{
			if (a_tag == "JumpDown") {
				return Foot::kEither;
			}

			if (!a_tag.starts_with("Foot")) {
				return std::nullopt;
			}

			if (a_tag.contains("Left")) {
				return Foot::kLeft;
			}

			if (a_tag.contains("Right")) {
				return Foot::kRight;
			}

			return Foot::kEither;
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

		struct Ground
		{
			RE::NiPoint3 position;
			RE::NiPoint3 normal;
		};

		std::optional<Ground> ProbeGround(RE::bhkWorld* a_world, float a_havok, const RE::NiPoint3& a_origin)
		{
			const RE::NiPoint3 from{ a_origin.x, a_origin.y, a_origin.z + kProbeAbove };
			const RE::NiPoint3 to{ a_origin.x, a_origin.y, a_origin.z - kProbeBelow };

			RE::bhkPickData pick;
			pick.rayInput.from = from * a_havok;
			pick.rayInput.to = to * a_havok;
			pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
			{
				RE::BSReadLockGuard lock(a_world->worldLock);
				a_world->PickObject(pick);
			}

			if (!pick.rayOutput.HasHit()) {
				return std::nullopt;
			}

			auto*              ref = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
			const RE::NiPoint3 normal = FromHavok(pick.rayOutput.normal);
			if ((ref && ref->As<RE::Actor>()) || normal.z < kMinGroundNormalZ) {
				return std::nullopt;
			}

			return Ground{ from + (to - from) * pick.rayOutput.hitFraction, normal };
		}

		struct Bones
		{
			RE::BSFixedString ankle;
			RE::BSFixedString toe;
		};

		std::optional<RE::NiPoint3> FootAnchor(RE::NiAVObject* a_root, const Bones& a_bones)
		{
			auto* ankle = a_root->GetObjectByName(a_bones.ankle);
			if (!ankle) {
				return std::nullopt;
			}

			auto* toe = a_root->GetObjectByName(a_bones.toe);
			return toe ? (ankle->world.translate + toe->world.translate) * 0.5f : ankle->world.translate;
		}

		Placement FootPlacement(RE::Actor* a_actor, RE::TESObjectCELL* a_cell, Foot a_foot)
		{
			static const auto& bones = *new std::array<Bones, 2>{
				Bones{ "NPC L Foot [Lft ]", "NPC L Toe0 [LToe]" },
				Bones{ "NPC R Foot [Rft ]", "NPC R Toe0 [RToe]" }
			};

			Placement placement{ a_actor->GetPosition(), {} };

			auto* world = a_cell->GetbhkWorld();
			if (!world) {
				return placement;
			}

			const float havok = RE::bhkWorld::GetWorldScale();
			auto*       root = a_actor->Get3D(false);

			std::span<const Bones> candidates{};
			if (root) {
				candidates = bones;
				if (a_foot == Foot::kLeft) {
					candidates = candidates.first(1);
				} else if (a_foot == Foot::kRight) {
					candidates = candidates.last(1);
				}
			}

			float best = (std::numeric_limits<float>::max)();

			for (const auto& bone : candidates) {
				const auto anchor = FootAnchor(root, bone);
				if (!anchor) {
					continue;
				}

				const auto ground = ProbeGround(world, havok, *anchor);
				if (!ground) {
					continue;
				}

				if (const float clearance = anchor->z - ground->position.z; clearance < best) {
					best = clearance;
					placement = { ground->position, UprightOn(ground->normal) };
				}
			}

			if (best == (std::numeric_limits<float>::max)()) {
				if (const auto ground = ProbeGround(world, havok, a_actor->GetPosition())) {
					placement = { ground->position, UprightOn(ground->normal) };
				}
			}

			return placement;
		}

		void OnFootstep(RE::Actor* a_actor, std::string_view a_tag)
		{
			const auto foot = FootFromTag(a_tag);
			if (!foot) {
				return;
			}

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

			SKSE::GetTaskInterface()->AddTask([id, scale, foot = *foot]() {
				auto* form = RE::TESForm::LookupByID(id);
				auto* actor = form ? form->As<RE::Actor>() : nullptr;
				auto* cell = actor ? actor->GetParentCell() : nullptr;
				if (!cell || !actor->Get3D()) {
					return;
				}

				// Picking the havok world wants the main thread.
				const Placement placement = FootPlacement(actor, cell, foot);

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
						OnFootstep(actor.get(), a_event->tag);
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
		if (!SKSE::GetTaskInterface()) {
			SKSE::log::error("Task interface unavailable - footstep splashes disabled");
			return;
		}

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
