#pragma once

namespace RainSplashes
{
	namespace Splash
	{
		// Tuning knobs; there is no config file. Scale matters most.
		inline constexpr float kMaxDistance = 2500.0f;
		inline constexpr float kLifetime = 2.0f;
		inline constexpr float kMinScale = 0.80f;
		inline constexpr float kMaxScale = 1.15f;
		inline constexpr float kCooldown = 0.12f;

		void Install();
		void Reset();
	}
}
