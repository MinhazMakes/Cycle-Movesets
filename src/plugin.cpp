#include "logger.h"
#include "Utils.h"
#include "Events.h"
#include "Manager.h"
#include "Serialization.h"
#include "OARAPI.h"
#include "MCP.h"
#include "Hooks.h"
#include "Conditions.h"
#include "OAR/OpenAnimationReplacerAPI-Conditions.h"

namespace fs = std::filesystem;

template <typename T>
void RegisterCondition() {
    extern OAR_API::Conditions::IConditionsInterface* g_oarConditionsInterface;  
    if (!g_oarConditionsInterface) {
        logger::error("OAR Conditions Interface nao disponivel para registrar {}", T::CONDITION_NAME);
        return;
    }

    switch (OAR_API::Conditions::AddCustomCondition<T>()) {
        using enum OAR_API::Conditions::APIResult;
        case OK:
            logger::info("Registrada condicao customizada: {}", T::CONDITION_NAME);
            break;
        case AlreadyRegistered:
            logger::warn("Condicao customizada {} ja registrada!", T::CONDITION_NAME);
            break;
        default:
            logger::error("Falha ao registrar condicao customizada {}!", T::CONDITION_NAME);
            break;
    }
}



void OnMessage(SKSE::MessagingInterface::Message* message) {
    if (message->type == SKSE::MessagingInterface::kInputLoaded) {
    }
    if (message->type == SKSE::MessagingInterface::kPostLoad) {
        OAR_API::Conditions::GetAPI();                                               // Tenta obter a API
        extern OAR_API::Conditions::IConditionsInterface* g_oarConditionsInterface;  // Acesso à interface

        if (g_oarConditionsInterface)  // Verifica se a API foi obtida com sucesso
        {
            // Registra sua nova condição
            RegisterCondition<Conditions::IsEquipSlotOccupied>();
        } else {
            logger::error("Falha ao requisitar a API de Condicoes do OAR.");
        }
    }

    if (message->type == SKSE::MessagingInterface::kDataLoaded) {


        GlobalControl::g_clientID = SkyPromptAPI::RequestClientID();
        if (GlobalControl::g_clientID > 0) {
            SKSE::log::info("ClientID {} recebido da SkyPromptAPI.", GlobalControl::g_clientID);
            if (!SkyPromptAPI::RequestTheme(GlobalControl::g_clientID,
                                            Settings::ShowMenu ? "Cycle Movesets" : "Cycle Movesets_hidden")) {
			    logger::error("Falha ao solicitar o tema 'Cycle Movesets' na SkyPromptAPI.");
            }
        } else {
            SKSE::log::error("Falha ao obter um ClientID da SkyPromptAPI. A API esta instalada?");
        }
        GlobalControl::MenuShowing = SkyPromptAPI::RequestClientID();
        if (GlobalControl::MenuShowing > 0) {
            SKSE::log::info("ClientID {} recebido da SkyPromptAPI.", GlobalControl::MenuShowing);
            if (!SkyPromptAPI::RequestTheme(GlobalControl::MenuShowing, "Cycle Movesets")) {
			    logger::error("Falha ao solicitar o tema 'Cycle Movesets' na SkyPromptAPI.");
            }
        } else {
            SKSE::log::error("Falha ao obter um ClientID da SkyPromptAPI. A API esta instalada?");
        }
        GlobalControl::Dynamicgrip = SkyPromptAPI::RequestClientID();
        if (GlobalControl::Dynamicgrip > 0) {
            SKSE::log::info("ClientID {} recebido da SkyPromptAPI.", GlobalControl::Dynamicgrip);
            if (!SkyPromptAPI::RequestTheme(GlobalControl::MenuShowing, "Cycle Movesets")) {
			    logger::error("Falha ao solicitar o tema 'Cycle Movesets' na SkyPromptAPI.");
            }
        } else {
            SKSE::log::error("Falha ao obter um ClientID da SkyPromptAPI. A API esta instalada?");
        }
        if (auto sourceHolder = RE::ScriptEventSourceHolder::GetSingleton()) {
            sourceHolder->AddEventSink<RE::TESEquipEvent>(GlobalControl::EquipEventSink::GetSingleton());
            logger::info("Equip event sink registrado.");
        }
        AnimationManager::GetSingleton()->PopulateNpcList();
        AnimationManager::GetSingleton()->LoadGameDataForNpcRules();
        AnimationManager::GetSingleton()->PopulatePerkList();
        AnimationManager::GetSingleton()->LoadGameDataForEffects();

        auto dataHandler = RE::TESDataHandler::GetSingleton();
        Hooks::g_rightHandSlot = dataHandler->LookupForm<RE::BGSEquipSlot>(0x13f42, "Skyrim.esm");
        Hooks::g_leftHandSlot = dataHandler->LookupForm<RE::BGSEquipSlot>(0x13f43, "Skyrim.esm");
        Hooks::g_twoHandSlot = dataHandler->LookupForm<RE::BGSEquipSlot>(0x13f45, "Skyrim.esm");
        Hooks::g_cmfhandle = dataHandler->LookupForm<RE::BGSEquipSlot>(0x802, "CMF.esp");
        
        Hooks::g_weapTypeSword = dataHandler->LookupForm<RE::BGSKeyword>(0x1E711, "Skyrim.esm");
        Hooks::g_weapTypeGreatsword = dataHandler->LookupForm<RE::BGSKeyword>(0x6D931, "Skyrim.esm");
        Hooks::g_weapTypeWarAxe = dataHandler->LookupForm<RE::BGSKeyword>(0x1E712, "Skyrim.esm");
        Hooks::g_weapTypeBattleaxe = dataHandler->LookupForm<RE::BGSKeyword>(0x6D932, "Skyrim.esm");
        Hooks::g_weapTypeWarhammer = dataHandler->LookupForm<RE::BGSKeyword>(0x6D930, "Skyrim.esm");
        Hooks::g_canDualWieldTwoHandedKeyword = dataHandler->LookupForm<RE::BGSKeyword>(0x800, "CMF.esp");
        Hooks::g_isActivelyDualWieldingKeyword = dataHandler->LookupForm<RE::BGSKeyword>(0x801, "CMF.esp");

    }
    

    if (message->type == SKSE::MessagingInterface::kNewGame || message->type == SKSE::MessagingInterface::kPostLoadGame) {
        WheelerKeys();
        // 2. Requisitar um ClientID da API SkyPrompt

        if (auto* inputDeviceManager = RE::BSInputDeviceManager::GetSingleton()) {
            inputDeviceManager->AddEventSink(GlobalControl::InputListener::GetSingleton());
            SKSE::log::info("Listener de input registrado com sucesso!");
        }

        // Em algum lugar na inicialização do seu plugin (ex: SKSEPlugin_Load)
        auto* animationEventSource = RE::PlayerCharacter::GetSingleton();
        if (animationEventSource) {
            animationEventSource->AddAnimationGraphEventSink(GlobalControl::AnimationEventHandler::GetSingleton());
            SKSE::log::info("AnimationEventHandler registrado com sucesso.");
        }
        auto* NpcCycle = RE::ScriptEventSourceHolder::GetSingleton();
        if (NpcCycle) {
            NpcCycle->AddEventSink(GlobalControl::NpcCombatTracker::GetSingleton());
            SKSE::log::info("NpcCycleSink (All NPCs) registrado com sucesso.");
        }

        SKSE::GetCameraEventSource()->AddEventSink(GlobalControl::CameraChange::GetSingleton());

        if (auto* ui = RE::UI::GetSingleton(); ui) {
            logger::info("Adding event sink for dialogue menu auto zoom.");
            ui->AddEventSink<RE::MenuOpenCloseEvent>(GlobalControl::MenuOpen::GetSingleton());
        }
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESHitEvent>(GlobalControl::HitEventHandler::GetSingleton());

        GlobalControl::NpcCombatTracker::RegisterSinksForExistingCombatants();

    }

}

SKSEPluginLoad(const SKSE::LoadInterface *skse) {

    SetupLog();
    logger::info("Plugin loaded");
    Hooks::Install();
    //AnimationManager::GetSingleton()->SaveAllSettings();
    SKSE::Init(skse);
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    
    // Registra seu ouvinte de eventos de Ação (sacar/guardar arma)
    auto* eventSource = SKSE::GetActionEventSource();
    if (eventSource) {
        eventSource->AddEventSink(GlobalControl::ActionEventHandler::GetSingleton());
        SKSE::log::info("Ouvinte de eventos de acao registrado com sucesso!");
    }
    AnimationManager::GetSingleton()->ScanAnimationMods();
    UI::RegisterMenu();
    
    return true;
}
