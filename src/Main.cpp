#include "Splash.h"

#include <SKSE/SKSE.h>

using namespace RainSplashes;

namespace
{
	void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			Splash::Install();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			Splash::Reset();
			break;
		default:
			break;
		}
	}
}

SKSEPluginInfo(
	.Version = { 1, 1, 0, 0 },
	.Name = "RainSplashes",
	.Author = "bottle")

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	// Init sets up the log file and pattern for us.
	SKSE::Init(a_skse);

	const auto messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(MessageHandler)) {
		return false;
	}

	SKSE::log::info("RainSplashes loaded");
	return true;
}
