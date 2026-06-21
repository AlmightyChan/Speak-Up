// ============================================================================
// Speak Up — SKSE plugin entry + logging.
//
// Reads the player's live spellbook and recognizes spoken commands against a
// live grammar to cast/equip spells, powers, and shouts, plus hands-free
// utilities. Recognition runs in-process and fully offline via Vosk (loaded
// from libvosk.dll at runtime) — there is NO companion process and NO IPC.
// Built on CommonLibSSE-NG; see docs/ for the architecture notes.
//
// License: GPL-3.0. Speak Up is a spiritual successor to Dragonborn Unlimited
// and Dragonborn Speaks Naturally — no code from either is used; both were
// inspirations only.
// ============================================================================

#include "PCH.h"
#include "TestHarness.h"
#include "VoiceController.h"

namespace logger = SKSE::log;

// ----------------------------------------------------------------------------
// Observability first: a dedicated, flush-on-every-line log so any crash leaves
// the exact last action on disk. Lives at the conventional SKSE log directory:
//   Documents/My Games/Skyrim Special Edition/SKSE/VoiceSpellcasting.log
// ----------------------------------------------------------------------------
static void SetupLog()
{
    auto logsFolder = SKSE::log::log_directory();
    if (!logsFolder) {
        SKSE::stl::report_and_fail("VSC: SKSE log_directory not provided — cannot create log."sv);
    }

    auto logFilePath = *logsFolder / std::format("{}.log", PLUGIN_NAME);
    // Rotating sink (NOT truncate-on-open) so previous sessions survive a relaunch —
    // dev testing kept losing the prior run's recognition log. Keeps ~2 x 1 MB files
    // (SpeakUp.log + SpeakUp.1.log); small footprint since debug logging is default-on.
    // A startup banner in each file marks where sessions begin.
    //
    // P2 fix: path::string() throws std::runtime_error when the Windows user profile
    // path (Documents/…) contains non-ASCII characters (non-Latin usernames, accented
    // letters, CJK, etc.) because MSVC's std::filesystem cannot represent them in the
    // system ANSI codepage.  We catch that and fall back to C:\Windows\Temp\SpeakUp.log,
    // which is always ASCII, so the plugin still loads and leaves a trace of the failure.
    std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> sink;
    try {
        sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            logFilePath.string(), 1u * 1024u * 1024u, 2u);
    } catch (const std::exception&) {
        sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            R"(C:\Windows\Temp\SpeakUp.log)", 1u * 1024u * 1024u, 2u);
    }
    auto log = std::make_shared<spdlog::logger>("VSC", std::move(sink));

    log->set_level(spdlog::level::info);
    // Flush on every line: a CTD must not lose the last action that caused it.
    log->flush_on(spdlog::level::info);

    spdlog::set_default_logger(std::move(log));
    spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
}

// ----------------------------------------------------------------------------
// SKSE lifecycle messages.
//   kPostLoad   — start the in-process Vosk recognizer.
//   kDataLoaded — forms exist; enumerate the live spell roster, install the
//                 debug hotkey, register the SpellsLearned sink.
// ----------------------------------------------------------------------------
static void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
    switch (a_msg->type) {
    case SKSE::MessagingInterface::kPostLoad:
        logger::info("kPostLoad — starting in-process recognizer");
        VSC::VoiceController::Get().Start();
        break;
    case SKSE::MessagingInterface::kDataLoaded:
        logger::info("kDataLoaded — forms available");
        VSC::VoiceController::Get().RegisterEvents();
#if VSC_ENABLE_TEST_HARNESS
        VSC::InstallTestHarness();
#endif
        break;
    case SKSE::MessagingInterface::kPostLoadGame:
    case SKSE::MessagingInterface::kNewGame:
        logger::info("save loaded — refreshing grammar");
        VSC::VoiceController::Get().MarkGameReady();
        break;
    default:
        break;
    }
}

// ----------------------------------------------------------------------------
// Plugin declaration (CommonLibSSE-NG). UsesAddressLibrary(true) +
// UsesNoStructs(true) declare version-independence so SKSE loads the one binary
// on ALL runtimes (SE and AE). Without UsesNoStructs, SKSE gates the plugin to
// pre-1.6.629 only and DISABLES it on AE (e.g. LoreRim) — the exact trap DBU's
// era of plugins fell into.
// ----------------------------------------------------------------------------
extern "C" __declspec(dllexport) constinit SKSE::PluginVersionData SKSEPlugin_Version = [] {
    SKSE::PluginVersionData v{};
    v.PluginVersion({ PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_PATCH });
    v.PluginName(PLUGIN_NAME);
    v.AuthorName(PLUGIN_AUTHOR);
    v.UsesAddressLibrary(true);
    v.UsesSigScanning(false);
    v.UsesNoStructs(true);
    return v;
}();

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    SetupLog();

    logger::info("============================================================");
    logger::info("{} v{} — by {}", PLUGIN_NAME, PLUGIN_VERSION, PLUGIN_AUTHOR);
    logger::info("live, offline voice spellcasting — plug-and-play");
    logger::info("============================================================");

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(MessageHandler)) {
        logger::error("failed to register message listener — aborting load");
        return false;
    }

    logger::info("loaded; awaiting kDataLoaded");
    return true;
}
