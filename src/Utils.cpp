#include "Serialization.h"
#include "Utils.h"
#include "Events.h"
#include <random>
#include <vector>
#include <algorithm> // Para std::max_element

// Scancodes das teclas WASD
constexpr uint32_t W_KEY = 0x11;
constexpr uint32_t A_KEY = 0x1E;
constexpr uint32_t S_KEY = 0x1F;
constexpr uint32_t D_KEY = 0x20;
int GlobalControl::g_directionalState = 0;

struct MatchResult {
    const WeaponCategory* category = nullptr;
    int score = -1;  // Pontuação de especificidade
};

bool isTwoHanded(RE::TESForm* a_weap) {
    if (!a_weap || !a_weap->IsWeapon()) return false;
    auto weap = a_weap->As<RE::TESObjectWEAP>();
    if (weap->IsTwoHandedSword() || weap->IsTwoHandedAxe()) return true;
    return false;
}


void EquipItemWithGripChange(RE::Actor* actor, RE::TESBoundObject* item, RE::BGSEquipSlot* targetSlot) {
    if (!actor || !item || !targetSlot) return;

    auto equipManager = RE::ActorEquipManager::GetSingleton();
    if (!equipManager) return;

    equipManager->EquipObject(actor, item, nullptr, 1, targetSlot);
    RE::SendUIMessage::SendInventoryUpdateMessage(actor, nullptr);
}

void CheckAndEquipDualTwoHandedForNPC(RE::Actor* npc) {
    if (!npc || npc->IsPlayer()) {
        return;
    }

    int npctype;
    npc->GetGraphVariableInt("CycleMovesetNpcType",npctype);  


    auto equippedItemL = npc->GetEquippedObjectInSlot(Hooks::g_leftHandSlot);
    auto equippedItemR = npc->GetEquippedObjectInSlot(Hooks::g_rightHandSlot);

    // 2. Inicializa os ponteiros de arma como nulos
    RE::TESObjectWEAP* leftWeapon = nullptr;
    RE::TESObjectWEAP* rightWeapon = nullptr;

    // 3. Loga e converte o item da mão ESQUERDA (se existir)
    if (equippedItemL) {
        leftWeapon = equippedItemL->As<RE::TESObjectWEAP>();
    } else {
    }

    // 4. Loga e converte o item da mão DIREITA (se existir)
    if (equippedItemR) {
        rightWeapon = equippedItemR->As<RE::TESObjectWEAP>();
    } else {
    }

    if (isTwoHanded(leftWeapon)) {
        logger::info(" NPC already has a two-handed weapon equipped in the left hand.");
        return;
    } else if (isTwoHanded(rightWeapon)) {
        logger::info(" NPC already has a two-handed weapon equipped in the right hand.");
        return;
    }
       
    // 3. Escanear o inventário
    std::vector<RE::TESObjectWEAP*> suitableWeapons;
    auto inventory = npc->GetInventory();

    for (const auto& [item, entry] : inventory) {
        if (item->IsWeapon() && isTwoHanded(item)) {
            suitableWeapons.push_back(item->As<RE::TESObjectWEAP>());
        }
    }

    logger::info("   - Inventory Scan: Found {} suitable two-handed weapons.", suitableWeapons.size());

    // 4. Ação Final
    if (suitableWeapons.size() >= 2) {
        logger::info("  - Check PASSED: Found at least 2 weapons. Proceeding to equip.");

        auto weapon1 = suitableWeapons[0];
        auto weapon2 = suitableWeapons[1];

        logger::info("  -> Equipping '{}' in Right Hand and '{}' in Left Hand.", weapon1->GetName(),
                     weapon2->GetName());

        // --- CORREÇÃO APLICADA ---
        // Chame a nova função centralizada em vez de duas chamadas separadas
        EquipItemWithGripChange(npc, weapon1, Hooks::g_rightHandSlot);
        EquipItemWithGripChange(npc, weapon2, Hooks::g_leftHandSlot);
        npc->SetGraphVariableInt("CycleMovesetNpcType", 1);  

    } else {
        logger::info("  - Check FAILED: Not enough suitable weapons in inventory.");
    }
}

// Esta função é chamada a cada frame de input
RE::BSEventNotifyControl GlobalControl::InputListener::ProcessEvent(RE::InputEvent* const* a_event,
                                                                    RE::BSTEventSource<RE::InputEvent*>*) {
    if (!a_event || !*a_event) {
        return RE::BSEventNotifyControl::kContinue;
    }

    bool umaTeclaDeMovimentoMudou = false;

    for (auto* event = *a_event; event; event = event->next) {
        RE::INPUT_DEVICE device = event->GetDevice();
        
        // Ignora movimentos do mouse para não trocar o dispositivo acidentalmente
        if (device != RE::INPUT_DEVICE::kMouse && device != RE::INPUT_DEVICE::kNone) {
            if (lastUsedDevice != device) {
                lastUsedDevice = device;
                SKSE::log::info("Input device switched to: {}", (int)device);
                // Quando o dispositivo muda, precisamos re-registrar as hotkeys com a API
                GlobalControl::UpdateRegisteredHotkeys();
            }
        }
        // --- LÓGICA DE MOVIMENTO (TECLADO E CONTROLE) ---
        if (event->GetEventType() == RE::INPUT_EVENT_TYPE::kThumbstick) {
            auto* thumbstick = event->AsThumbstickEvent();
            if (thumbstick && thumbstick->IsLeft()) {
                // Normalizamos os valores para evitar pequenas flutuações do analógico
                bool new_c_up = thumbstick->yValue > 0.5f;
                bool new_c_down = thumbstick->yValue < -0.5f;
                bool new_c_left = thumbstick->xValue < -0.5f;
                bool new_c_right = thumbstick->xValue > 0.5f;

                if (c_up != new_c_up || c_down != new_c_down || c_left != new_c_left || c_right != new_c_right) {
                    c_up = new_c_up;
                    c_down = new_c_down;
                    c_left = new_c_left;
                    c_right = new_c_right;
                    umaTeclaDeMovimentoMudou = true;
                }
            }
        } else if (event->GetEventType() == RE::INPUT_EVENT_TYPE::kButton) {
            auto* button = event->AsButtonEvent();
            const uint32_t scanCode = button->GetIDCode();

            // Lógica rigorosa de máquina de estados para cada tecla
            if (scanCode == Settings::keyForward) {
                // Só mude para 'pressionado' se a tecla ESTIVER 'down' E nosso estado atual for 'solto'.
                if (button->IsDown() && !w_pressed) {
                    w_pressed = true;
                    umaTeclaDeMovimentoMudou = true;
                }
                // Só mude para 'solto' se a tecla ESTIVER 'up' E nosso estado atual for 'pressionado'.
                else if (button->IsUp() && w_pressed) {
                    w_pressed = false;
                    umaTeclaDeMovimentoMudou = true;
                }
            } else if (scanCode == Settings::keyLeft) {
                if (button->IsDown() && !a_pressed) {
                    a_pressed = true;
                    umaTeclaDeMovimentoMudou = true;
                } else if (button->IsUp() && a_pressed) {
                    a_pressed = false;
                    umaTeclaDeMovimentoMudou = true;
                }
            } else if (scanCode == Settings::keyBack) {
                if (button->IsDown() && !s_pressed) {
                    s_pressed = true;
                    umaTeclaDeMovimentoMudou = true;
                } else if (button->IsUp() && s_pressed) {
                    s_pressed = false;
                    umaTeclaDeMovimentoMudou = true;
                }
            } else if (scanCode == Settings::keyRight) {
                if (button->IsDown() && !d_pressed) {
                    d_pressed = true;
                    umaTeclaDeMovimentoMudou = true;
                } else if (button->IsUp()) {
                    d_pressed = false;
                    umaTeclaDeMovimentoMudou = true;
                }
            } else if (scanCode == WheelerKeyboard) {
                if (button->IsDown()) {
                    wheelerOpen = true;
                    SkyPromptAPI::RemovePrompt(MovesetSink::GetSingleton(), g_clientID);
                    SkyPromptAPI::RemovePrompt(StancesSink::GetSingleton(), g_clientID);
                } else if (button->IsUp() && ShouldShowPrompts()) {
                    wheelerOpen = false;
                    SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID);
                    SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);
                }
            }

            if (device == RE::INPUT_DEVICE::kGamepad) {
                if (scanCode == WheelerGamepad) {
                    if (button->IsDown()) {
                        wheelerOpen = true;
                        SkyPromptAPI::RemovePrompt(MovesetSink::GetSingleton(), g_clientID);
                        SkyPromptAPI::RemovePrompt(StancesSink::GetSingleton(), g_clientID);
                    } else if (button->IsUp() && ShouldShowPrompts()) {
                        wheelerOpen = false;
                        SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID);
                        SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);
                    }
                }
            }
        }

        // Apenas recalcule a direção se uma das nossas teclas de movimento REALMENTE mudou de estado.
        if (umaTeclaDeMovimentoMudou) {
            UpdateDirectionalState();
        }
        
        return RE::BSEventNotifyControl::kContinue;
    }
}


// Esta função calcula o valor final da sua variável
void GlobalControl::InputListener::UpdateDirectionalState() {
    //static int DirecionalCycleMoveset = 0;
    int VariavelAnterior = directionalState;
    
    

    // Prioriza o input do teclado. Se qualquer tecla WASD estiver pressionada, ignore o controle.
    // Caso contrário, use o estado do controle.
    bool FRENTE = w_pressed || (!w_pressed && !a_pressed && !s_pressed && !d_pressed && c_up);
    bool TRAS = s_pressed || (!w_pressed && !a_pressed && !s_pressed && !d_pressed && c_down);
    bool ESQUERDA = a_pressed || (!w_pressed && !a_pressed && !s_pressed && !d_pressed && c_left);
    bool DIREITA = d_pressed || (!w_pressed && !a_pressed && !s_pressed && !d_pressed && c_right);

    // A lógica de decisão permanece a mesma, mas agora usa as variáveis combinadas
    if (FRENTE && ESQUERDA) {
        directionalState  = 8;  // Noroeste
    } else if (FRENTE && DIREITA) {
        directionalState  = 2;  // Nordeste
    } else if (TRAS && ESQUERDA) {
        directionalState  = 6;  // Sudoeste
    } else if (TRAS && DIREITA) {
        directionalState  = 4;  // Sudeste
    } else if (FRENTE) {
        directionalState  = 1;  // Norte (Frente)
    } else if (ESQUERDA) {
        directionalState  = 7;  // Oeste (Esquerda)
    } else if (TRAS) {
        directionalState  = 5;  // Sul (Trás)
    } else if (DIREITA) {
        directionalState  = 3;  // Leste (Direita)
    } else {
        directionalState  = 0;  // Parado
    }

    // Opcional: só imprime no log se o valor mudar, para não poluir o log.
    if (VariavelAnterior != directionalState ) {
        //SKSE::log::info("DirecionalCycleMoveset  alterado para: {}", directionalState );
        GlobalControl::UpdateSkyPromptTexts();
        // Aqui você enviaria o valor para sua animação, por exemplo:
        // RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("MinhaVariavelDirecional",
        // directionalState );
        if (ShouldShowPrompts() && !GlobalControl::MovesetChangesOpen && !GlobalControl::StanceChangesOpen) {
            SkyPromptAPI::SendPrompt(GlobalControl::StancesSink::GetSingleton(), GlobalControl::g_clientID);
            SkyPromptAPI::SendPrompt(GlobalControl::MovesetSink::GetSingleton(), GlobalControl::g_clientID);
            //SKSE::log::info("SkyPrompt reenviado devido à mudança de direção.");
            
        }
        if (!ShouldShowPrompts()) {
            SkyPromptAPI::RemovePrompt(GlobalControl::StancesSink::GetSingleton(), GlobalControl::g_clientID);
            SkyPromptAPI::RemovePrompt(GlobalControl::MovesetSink::GetSingleton(), GlobalControl::g_clientID);
            //SKSE::log::info("SkyPrompt reenviado devido à mudança de direção.");
            
        }

        if (ShouldShowPrompts() && GlobalControl::MovesetChangesOpen && !GlobalControl::StanceChangesOpen) {
            SkyPromptAPI::SendPrompt(GlobalControl::MovesetChangesSink::GetSingleton(), GlobalControl::MenuShowing);
            
            //SKSE::log::info("SkyPrompt reenviado devido à mudança de direção e menu aberto.");
        }
    }
    RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("DirecionalCycleMoveset", directionalState);
    GlobalControl::UpdatePowerAttackGlobals();
    /*if (wheelerOpen) {
        wheelerOpen = false;
        SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID);
        SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);
    }*/
}

// NOVA FUNÇÃO AUXILIAR PARA QUALQUER ATOR
std::string GetActorWeaponCategoryName(RE::Actor* targetActor) {
    if (!targetActor) return "Unarmed";

    // 1. Obter os objetos equipados em ambas as mãos
    auto rightHand = targetActor->GetEquippedObject(false);
    auto leftHand = targetActor->GetEquippedObject(true);

    RE::TESObjectWEAP* rightWeapon = rightHand ? rightHand->As<RE::TESObjectWEAP>() : nullptr;
    RE::TESObjectWEAP* leftWeapon = leftHand ? leftHand->As<RE::TESObjectWEAP>() : nullptr;
    RE::TESObjectARMO* leftArmor = leftHand ? leftHand->As<RE::TESObjectARMO>() : nullptr;

    // 2. Lógica inicial de checagem de estado
    // É "Unarmed" apenas se AMBAS as mãos estiverem efetivamente vazias (ou com itens não relevantes)
    if (!rightWeapon && !leftWeapon && (!leftArmor || !leftArmor->IsShield())) {
        return "Unarmed";
    }

    // 3. Determinar os tipos para ambas as mãos (padrão 0.0 para "vazio")
    double rightHandType = rightWeapon ? static_cast<double>(rightWeapon->GetWeaponType()) : 0.0;


    double leftHandType = 0.0;
    if (leftWeapon) {
        leftHandType = static_cast<double>(leftWeapon->GetWeaponType());
    } else if (leftArmor && leftArmor->IsShield()) {
        leftHandType = 11.0;  // Tipo para escudo
    }

    // ==============================================================================
    // A lógica de correspondência e pontuação agora funciona universalmente
    // ==============================================================================

    const auto& allCategories = AnimationManager::GetSingleton()->GetCategories();
    std::vector<MatchResult> matches;
    std::string fallbackCategory = "Sem Categoria";  // Novo padrão para quando não há correspondência

    for (const auto& pair : allCategories) {
        const WeaponCategory& category = pair.second;

        double adjustedEquippedTypeValue = (category.equippedTypeValue == 10.0) ? 6.0 : category.equippedTypeValue;
        // A. Checagem de Tipo
        bool rightHandTypeMatch = (adjustedEquippedTypeValue == rightHandType);
        bool leftHandTypeMatch =
            (category.leftHandEquippedTypeValue < 0.0 || category.leftHandEquippedTypeValue == leftHandType);

        if (rightHandTypeMatch && leftHandTypeMatch) {
            // B. Checagem de Keywords (apenas se a arma correspondente existir)
            bool rightKeywordsMatch = category.keywords.empty();
            if (!rightKeywordsMatch && rightWeapon) {
                for (const auto& keyword : category.keywords) {
                    if (rightWeapon->HasKeywordString(keyword)) {
                        rightKeywordsMatch = true;
                        break;
                    }
                }
            }

            bool leftKeywordsMatch = category.leftHandKeywords.empty();
            if (!leftKeywordsMatch && leftWeapon) {  // Só checa keywords em armas na mão esquerda
                for (const auto& keyword : category.leftHandKeywords) {
                    if (leftWeapon->HasKeywordString(keyword)) {
                        leftKeywordsMatch = true;
                        break;
                    }
                }
            }

            // C. Se tudo corresponde, calcula o score
            if (rightKeywordsMatch && leftKeywordsMatch) {
                int score = 0;
                // Keywords são o critério mais importante
                if (!category.keywords.empty()) score += 4;
                if (!category.leftHandKeywords.empty()) score += 4;

                // Tipos específicos são o segundo critério mais importante
                // Damos um score maior se a mão direita (principal) for definida
                if (category.equippedTypeValue > 0.0) score += 2;
                if (category.leftHandEquippedTypeValue >= 0.0) score += 1;

                matches.push_back({&category, score});
            }
        }
    }

    // Se não houver correspondências, retorna o fallback
    if (matches.empty()) {
        // Poderíamos adicionar uma lógica aqui para encontrar a categoria base (e.g., "Sword") se quiséssemos,
        // mas retornar "Sem Categoria" é mais seguro para evitar falsos positivos.
        return fallbackCategory;
    }

    // Encontra o elemento com o maior score
    auto bestMatch = std::max_element(matches.begin(), matches.end(),
                                      [](const MatchResult& a, const MatchResult& b) { return a.score < b.score; });

    return bestMatch->category->name;
}

// NOVA VERSÃO SIMPLIFICADA
std::string GetCurrentWeaponCategoryName() {
    // Esta função agora simplesmente chama a função principal com o jogador como alvo.
    // Isso evita duplicar código e centraliza toda a lógica em um só lugar.
    return GetActorWeaponCategoryName(RE::PlayerCharacter::GetSingleton());
}

std::span<const SkyPromptAPI::Prompt> GlobalControl::StancesSink::GetPrompts() const {
    return prompts; }

void GlobalControl::StancesSink::ProcessEvent(SkyPromptAPI::PromptEvent event) const {
    auto eventype = event.type;
    if (!g_isWeaponDrawn) {
        return;
    }
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;
    auto animManager = AnimationManager::GetSingleton();
    std::string categoryName = GetCurrentWeaponCategoryName();
    switch (eventype) {
        case SkyPromptAPI::kAccepted:
            if(!except) {
                const auto availableStances = animManager->GetAvailableStances(player, categoryName);
                if (availableStances.empty()) {
                    return;  // Não faz nada se não houver stances disponíveis
                }
                except = true;
                GlobalControl::StanceChangesOpen = true;
                SkyPromptAPI::RemovePrompt(MovesetSink::GetSingleton(), g_clientID);
                SkyPromptAPI::RemovePrompt(StancesSink::GetSingleton(), g_clientID);
                /*if (!Settings::ShowMenu) {
                    SkyPromptAPI::RequestTheme(GlobalControl::g_clientID, "Cycle Movesets");
                }*/
                if (!SkyPromptAPI::SendPrompt(StancesChangesSink::GetSingleton(), MenuShowing)) {
                    logger::error("Skyprompt didnt worked Stances Changes Sink");
                }
                break;
            }
                
        case SkyPromptAPI::kUp:
            except = false;
            GlobalControl::StanceChangesOpen = false;
            /*if (!Settings::ShowMenu) {
                SkyPromptAPI::RequestTheme(GlobalControl::g_clientID, "Cycle Movesets_hidden");
            }*/
            SkyPromptAPI::RemovePrompt(StancesChangesSink::GetSingleton(), MenuShowing);
            if (SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID)){}
            if (!SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID)) {
                logger::error("Skyprompt didnt worked Moveset Sink");
            }
            break;        
        case SkyPromptAPI::kTimeout:
            if (SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID)){}
            if (!SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID)) {
                logger::error("Skyprompt didnt worked Moveset Sink");
            }
            break;        
        case SkyPromptAPI::kDeclined:
            g_currentMoveset = 0;
            g_currentStance = 0;
            UpdatePowerAttackGlobals();
            UpdateSkyPromptTexts();
            SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID);
            SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);
            RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("testarone", g_currentMoveset);
            RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("cycle_instance", g_currentStance);
            if (!SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), GlobalControl::g_clientID)) {
                logger::error("Skyprompt didnt worked Moveset Sink");
            }
            break;   
     
    }

}

std::span<const SkyPromptAPI::Prompt> GlobalControl::StancesChangesSink::GetPrompts() const {

    return prompts; }

void GlobalControl::StancesChangesSink::ProcessEvent(SkyPromptAPI::PromptEvent event) const {
    auto player = RE::PlayerCharacter::GetSingleton();
    std::string categoryName = GetCurrentWeaponCategoryName();
    auto animManager = AnimationManager::GetSingleton();
    const auto availableStances = animManager->GetAvailableStances(player, categoryName);
    const int numStances = availableStances.size();
    bool stanceChanged = false;  // Flag para saber se a stance realmente mudou
    int oldStanceListIndex = (g_currentStance > 0 && g_currentStance <= numStances) ? g_currentStance - 1 : -1;
    int originalStanceIndexBeforeChange =
        (oldStanceListIndex != -1) ? availableStances[oldStanceListIndex].originalIndex : 0;

    switch (event.type) {
        case SkyPromptAPI::kAccepted:
            if (event.prompt.eventID == 2 || event.prompt.eventID == 3) {  // Next ou Back
                if (numStances > 0) {                                      // Só muda se houver stances disponíveis
                    int oldStanceValue = g_currentStance;                  // Guarda o valor *antes* de mudar
                    if (event.prompt.eventID == 2) {                       // Back
                        g_currentStance = (g_currentStance - 1);
                        if (g_currentStance < 1) g_currentStance = numStances;  // Cicla para o fim
                    } else {                                                    // Next (event.prompt.eventID == 3)
                        g_currentStance = (g_currentStance % numStances) + 1;   // Cicla para o início
                    }

                    // Verifica se o valor realmente mudou
                    if (g_currentStance != oldStanceValue) {
                        stanceChanged = true;  // Define a flag AQUI
                    }
                }
            }
            break;  // Sai do case kAccepted
        case SkyPromptAPI::kTimeout:
            SkyPromptAPI::SendPrompt(StancesChangesSink::GetSingleton(), MenuShowing);
            break;
        case SkyPromptAPI::kUp:
            if (event.prompt.eventID == 0) {
                GlobalControl::StanceChangesOpen = false;
                /*if (!Settings::ShowMenu) {
                    SkyPromptAPI::RequestTheme(GlobalControl::g_clientID, "Cycle Movesets_hidden");
                }*/
                SkyPromptAPI::RemovePrompt(StancesChangesSink::GetSingleton(), MenuShowing);
                if (SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID)) {
                }
                if (!SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID)) {
                    logger::error("Skyprompt didnt worked Moveset Sink");
                }
            }
            break;
    }
    if (stanceChanged) {
        int currentListIndex = (g_currentStance > 0) ? g_currentStance - 1 : -1;
        int originalStanceIndexToApply = (currentListIndex != -1) ? availableStances[currentListIndex].originalIndex
                                                                  : 0;  // 0 se g_currentStance for 0

        // 1. Aplica a nova stance no jogo (ou 0 se resetado)
        RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("cycle_instance", originalStanceIndexToApply);

        // 2. Reseta o moveset e aplica o primeiro disponível (ou 0)
        g_currentMoveset = 0;  // Começa resetado
        int originalMovesetIndexToApply = 0;
        const ModInstance* firstModInst = nullptr;           // Ponteiro para o ModInstance do primeiro moveset
        const SubAnimationInstance* firstSubInst = nullptr;  // Ponteiro para o SubAnimationInstance do primeiro moveset
        std::vector<AppliedEffect> firstMovesetCombinedEffects;  // Guarda os efeitos combinados do primeiro moveset
        if (originalStanceIndexToApply > 0) {                    // Se temos uma stance válida
            const auto availableMovesets =
                animManager->GetAvailableMovesets(player, categoryName, originalStanceIndexToApply);
            if (!availableMovesets.empty()) {
                g_currentMoveset = 1;                                              // Seleciona o primeiro da lista
                originalMovesetIndexToApply = availableMovesets[0].originalIndex;  // Pega o índice real

                // --- NOVA LÓGICA: Coleta os efeitos do primeiro moveset ---
                auto cat_it = animManager->GetCategories().find(categoryName);
                if (cat_it != animManager->GetCategories().end()) {
                    const WeaponCategory& category = cat_it->second;
                    if (originalStanceIndexToApply > 0 && originalStanceIndexToApply <= category.instances.size()) {
                        const auto& stanceInstance = category.instances[originalStanceIndexToApply - 1];
                        // Efeitos da Stance (sempre incluídos nos efeitos do moveset)
                        firstMovesetCombinedEffects.insert(firstMovesetCombinedEffects.end(),
                                                           stanceInstance.appliedEffects.begin(),
                                                           stanceInstance.appliedEffects.end());

                        // Encontra o ModInstance e SubAnimationInstance do *primeiro* moveset
                        // (originalMovesetIndexToApply)
                        int parentCounter = 0;
                        auto& mutableStanceInstance =
                            const_cast<CategoryInstance&>(stanceInstance);  // Necessário para iterar
                        for (const auto& modInst : mutableStanceInstance.modInstances) {
                            if (!modInst.isSelected) continue;
                            for (const auto& subInst : modInst.subAnimationInstances) {
                                bool isParent = !(subInst.pFront || subInst.pBack || subInst.pLeft || subInst.pRight ||
                                                  subInst.pFrontRight || subInst.pFrontLeft || subInst.pBackRight ||
                                                  subInst.pBackLeft || subInst.pRandom || subInst.pDodge);
                                if (!subInst.isSelected || !isParent) continue;
                                parentCounter++;
                                if (parentCounter == originalMovesetIndexToApply) {  // Achou o primeiro moveset
                                    firstModInst = &modInst;
                                    firstSubInst = &subInst;
                                    goto found_first_moveset;  // Sai dos loops
                                }
                            }
                        }
                    found_first_moveset:;  // Label para o goto
                        if (firstModInst) {
                            firstMovesetCombinedEffects.insert(firstMovesetCombinedEffects.end(),
                                                               firstModInst->appliedEffects.begin(),
                                                               firstModInst->appliedEffects.end());
                        }
                        if (firstSubInst) {
                            firstMovesetCombinedEffects.insert(firstMovesetCombinedEffects.end(),
                                                               firstSubInst->appliedEffects.begin(),
                                                               firstSubInst->appliedEffects.end());
                        }
                        // Remove duplicatas
                        std::sort(firstMovesetCombinedEffects.begin(), firstMovesetCombinedEffects.end());
                        firstMovesetCombinedEffects.erase(
                            std::unique(firstMovesetCombinedEffects.begin(), firstMovesetCombinedEffects.end()),
                            firstMovesetCombinedEffects.end());
                    }
                }
                // --- FIM DA NOVA LÓGICA ---
            }
        }
        RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("testarone", originalMovesetIndexToApply);

        // 3. Coleta os efeitos da NOVA stance (será vazio se g_currentStance == 0)
        std::vector<AppliedEffect> newStanceEffects;
        if (originalStanceIndexToApply > 0) {
            auto cat_it = animManager->GetCategories().find(categoryName);
            if (cat_it != animManager->GetCategories().end()) {
                const WeaponCategory& category = cat_it->second;
                if (originalStanceIndexToApply <= category.instances.size()) {
                    newStanceEffects = category.instances[originalStanceIndexToApply - 1].appliedEffects;
                }
            }
        }

        // 4. Aplica/Remove efeitos da Stance
        //    ApplyAndTrackEffects compara newStanceEffects com g_lastAppliedStanceEffects.
        //    - Efeitos em g_lastAppliedStanceEffects que NÃO estão em newStanceEffects são REMOVIDOS.
        //    - Efeitos em newStanceEffects que NÃO estão em g_lastAppliedStanceEffects são ADICIONADOS.
        //    - g_lastAppliedStanceEffects é atualizado para ser igual a newStanceEffects.
        SKSE::log::info("[Stance Change/Reset] Aplicando {} efeitos da Stance {}", newStanceEffects.size(),
                        g_currentStance);
        SKSE::log::info("[Stance Change/Reset] Aplicando {} efeitos combinados do Moveset Padrão (Lista Index {})",
                        firstMovesetCombinedEffects.size(), g_currentMoveset);
        ApplyAndTrackEffects(player, newStanceEffects, g_lastAppliedStanceEffects);
        ApplyAndTrackEffects(player, firstMovesetCombinedEffects, g_lastAppliedMovesetEffects);


        // 6. Atualiza outras lógicas e a UI
        UpdatePowerAttackGlobals();  // Atualiza DPA/CPA baseado na nova stance/moveset
        UpdateSkyPromptTexts();      // Atualiza TODOS os textos da UI para refletir o novo estado

        // 7. Reenvia os prompts do menu ATUAL (StancesChangesSink) para mostrar o nome correto
        if (GlobalControl::StanceChangesOpen) {  // Só reenvia se o menu ainda estiver aberto
            SkyPromptAPI::SendPrompt(StancesChangesSink::GetSingleton(), MenuShowing);
        } else {  // Se foi um kDeclined, reenvia os menus principais
            SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID);
            SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);
        }

    } else if (event.type == SkyPromptAPI::kAccepted) {
        // Se kAccepted foi pressionado mas stanceChanged é false (ex: só tem 1 stance),
        // apenas reenvia o prompt atual para resetar o timer.
        SkyPromptAPI::SendPrompt(StancesChangesSink::GetSingleton(), MenuShowing);
    }

}

std::span<const SkyPromptAPI::Prompt> GlobalControl::MovesetSink::GetPrompts() const {
    return prompts; }

void GlobalControl::MovesetSink::ProcessEvent(SkyPromptAPI::PromptEvent event) const {
    auto eventype = event.type;
    if (!g_isWeaponDrawn) {
        return;
    }
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;
    auto animManager = AnimationManager::GetSingleton();
    std::string categoryName = GetCurrentWeaponCategoryName();
    const auto availableStances = animManager->GetAvailableStances(player, categoryName);
    int originalStanceIndex = 0;

    if (g_currentStance > 0 && g_currentStance <= availableStances.size()) {
        originalStanceIndex = availableStances[g_currentStance - 1].originalIndex;
    } else if (g_currentStance == 0 && !availableStances.empty()) {
        originalStanceIndex = availableStances[0].originalIndex;
    }
    switch (eventype) {

        case SkyPromptAPI::kAccepted:
            if (!except) {
                const auto availableMovesets =
                    animManager->GetAvailableMovesets(player, categoryName, originalStanceIndex);
                if (availableMovesets.empty()) {
                    return;  // Não faz nada se não houver movesets
                }
                except = true;
                GlobalControl::MovesetChangesOpen = true;
                /*if (!Settings::ShowMenu) {
                    SkyPromptAPI::RequestTheme(GlobalControl::g_clientID, "Cycle Movesets");
                }*/
                SkyPromptAPI::RemovePrompt(StancesSink::GetSingleton(), GlobalControl::g_clientID);
                SkyPromptAPI::RemovePrompt(MovesetSink::GetSingleton(), GlobalControl::g_clientID);
                if (!SkyPromptAPI::SendPrompt(MovesetChangesSink::GetSingleton(), MenuShowing)) {
                    logger::error("Skyprompt didnt worked Stances Changes Sink");
                }
                SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), MenuShowing);
                break;
            }
        case SkyPromptAPI::kUp:
            except = false;
            GlobalControl::MovesetChangesOpen = false;
            /*if (!Settings::ShowMenu) {
                SkyPromptAPI::RequestTheme(GlobalControl::g_clientID, "Cycle Movesets_hidden");
            }*/
            SkyPromptAPI::RemovePrompt(MovesetChangesSink::GetSingleton(), MenuShowing);
            SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);
            SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID);
            break;
        case SkyPromptAPI::kDeclined:
            g_currentMoveset = 1;
            UpdatePowerAttackGlobals();
            UpdateSkyPromptTexts();
            RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("testarone", g_currentMoveset);
            //GlobalControl::MovesetText = "Moveset";
            SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);
            break;
    }
}

std::span<const SkyPromptAPI::Prompt> GlobalControl::MovesetChangesSink::GetPrompts() const { 
    return prompts; }
    

void GlobalControl::MovesetChangesSink::ProcessEvent(SkyPromptAPI::PromptEvent event) const {

    auto player = RE::PlayerCharacter::GetSingleton();
    std::string categoryName = GetCurrentWeaponCategoryName();
    auto animManager = AnimationManager::GetSingleton();

    // O índice da stance (g_currentStance) ainda está no formato de lista (ex: 2 de 2)
    // Precisamos encontrar o índice original que ele representa.
    int stanceOriginalIndex = g_currentStance;  // Fallback
    auto availableStances = animManager->GetAvailableStances(player, categoryName);
    if (g_currentStance > 0 && g_currentStance <= availableStances.size()) {
        stanceOriginalIndex = availableStances[g_currentStance - 1].originalIndex;
    }

    // SUBSTITUA ISSO:
    // int maxMovesets = AnimationManager::GetMaxMovesetsFor(category, stanceIndex);
    // POR ISSO:
    const auto availableMovesets = animManager->GetAvailableMovesets(player, categoryName, stanceOriginalIndex);
    const int maxMovesets = availableMovesets.size();

    // Se não há movesets configurados para esta stance/arma, não faz nada.
    if (maxMovesets <= 0) {
        RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("testarone",0);  // Garante que nenhuma animação toque
        return;
    }

    //logger::info("before kup");
    switch (event.type) {
        case SkyPromptAPI::kAccepted:
            if (event.prompt.eventID == 2) {
                g_currentMoveset -= 1;
                if (g_currentMoveset < 1) {
                    g_currentMoveset = maxMovesets;  // Vai para o último
                }
                UpdatePowerAttackGlobals();
                UpdateSkyPromptTexts();
                SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), MenuShowing);
                SkyPromptAPI::SendPrompt(MovesetChangesSink::GetSingleton(), MenuShowing);
                break;
            }
            if (event.prompt.eventID == 3) {
                g_currentMoveset += 1;
                if (g_currentMoveset > maxMovesets) {
                    g_currentMoveset = 1;  // Volta para o primeiro
                }
                UpdatePowerAttackGlobals();
                UpdateSkyPromptTexts();
                SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), MenuShowing);
                SkyPromptAPI::SendPrompt(MovesetChangesSink::GetSingleton(), MenuShowing);
                break;
            }
        case SkyPromptAPI::kTimeout:
            SkyPromptAPI::SendPrompt(MovesetChangesSink::GetSingleton(), MenuShowing);
            break;
        case SkyPromptAPI::kUp:
            if (event.prompt.eventID == 1) {
                GlobalControl::MovesetChangesOpen = false;
                if (!Settings::ShowMenu) {
                    SkyPromptAPI::RequestTheme(GlobalControl::g_clientID, "Cycle Movesets_hidden");
                }
                SkyPromptAPI::RemovePrompt(MovesetChangesSink::GetSingleton(), MenuShowing);
                SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);
                SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID);
            }
            logger::info("kUp aceito");
            break;
    }
    int currentListIndex = g_currentMoveset - 1;  // 0-based index
    int originalMovesetIndex = availableMovesets[currentListIndex].originalIndex;
    RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("testarone", originalMovesetIndex);
    
}

RE::BSEventNotifyControl GlobalControl::CameraChange::ProcessEvent(const SKSE::CameraEvent* a_event,
                                                          RE::BSTEventSource<SKSE::CameraEvent>*) {


    if (!a_event) {
        return RE::BSEventNotifyControl::kContinue;
    }
    if (!RE::PlayerCamera::GetSingleton()->IsInThirdPerson()) {
        Cycleopen = false;
        SkyPromptAPI::RemovePrompt(StancesSink::GetSingleton(), g_clientID);
        SkyPromptAPI::RemovePrompt(MovesetSink::GetSingleton(), g_clientID);
        SkyPromptAPI::RemovePrompt(StancesChangesSink::GetSingleton(), MenuShowing);
        SkyPromptAPI::RemovePrompt(MovesetChangesSink::GetSingleton(), MenuShowing);
        //logger::info("me retorna aqui vei");
    }
    if (ShouldShowPrompts() && !Cycleopen) {
        Cycleopen = true;
        SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID);
        SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);
    }
    

    return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl GlobalControl::ActionEventHandler::ProcessEvent(const SKSE::ActionEvent* a_event,
                                                                         RE::BSTEventSource<SKSE::ActionEvent>*) {
 

    if (a_event->actor->IsPlayerRef()) {
        // Jogador comeou a sacar a arma
        if (a_event->type == SKSE::ActionEvent::Type::kBeginDraw) {
            SKSE::log::info("Arma sacada, mostrando o menu.");
            g_isWeaponDrawn = true;  // Define nosso controle como verdadeiro
            // Envia os prompts para a API, fazendo o menu aparecer
            UpdatePowerAttackGlobals();
            UpdateSkyPromptTexts();
            if (ShouldShowPrompts()) {
                Cycleopen = true;
                SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID);
                SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);
            } else
                {
                SKSE::log::info("ta dando ruim");
            }
        
        }
        else if (a_event->type == SKSE::ActionEvent::Type::kEndSheathe) {
            //SKSE::log::info("Arma guardada, escondendo o menu.");
            g_isWeaponDrawn = false;  // Define nosso controle como falso
            // Limpa os prompts da API, fazendo o menu desaparecer
            SkyPromptAPI::RemovePrompt(StancesSink::GetSingleton(), g_clientID);
            SkyPromptAPI::RemovePrompt(MovesetSink::GetSingleton(), g_clientID);
            SkyPromptAPI::RemovePrompt(StancesChangesSink::GetSingleton(), MenuShowing);
            SkyPromptAPI::RemovePrompt(MovesetChangesSink::GetSingleton(), MenuShowing);
        }
    }
    return RE::BSEventNotifyControl::kContinue;
}



void GlobalControl::ApplyAndTrackEffects(RE::Actor* actor, const std::vector<AppliedEffect>& newEffectsConst,
                                         std::vector<AppliedEffect>& lastAppliedEffects) {
    if (!actor) return;
    // Só aplicamos ao jogador por enquanto, pode ser expandido para NPCs depois
    if (!actor->IsPlayerRef()) return;

    // Usamos cópias para poder ordenar sem modificar os originais
    std::vector<AppliedEffect> newEffects = newEffectsConst;
    std::vector<AppliedEffect> oldEffects = lastAppliedEffects;

    // Ordena ambas as listas para usar algoritmos eficientes
    std::sort(newEffects.begin(), newEffects.end());
    std::sort(oldEffects.begin(), oldEffects.end());

    // 1. Encontra efeitos para REMOVER (presentes em oldEffects, mas NÃO em newEffects)
    std::vector<AppliedEffect> toRemove;
    std::set_difference(oldEffects.begin(), oldEffects.end(), newEffects.begin(), newEffects.end(),
                        std::back_inserter(toRemove));

    for (const auto& effect : toRemove) {
        RE::TESForm* form = RE::TESForm::LookupByID(effect.formID);
        if (!form) {
            SKSE::log::warn("ApplyAndTrackEffects: FormID {:08X} não encontrado para remoção.", effect.formID);
            continue;
        }

        switch (effect.type) {
            case AppliedEffect::EffectType::Perk:
                if (auto perk = form->As<RE::BGSPerk>()) {
                    if (actor->HasPerk(perk)) {  // Verifica antes de remover
                        SKSE::log::info("Removendo Perk: {}", perk->GetName());
                        actor->RemovePerk(perk);
                    }
                }
                break;
            case AppliedEffect::EffectType::MagicEffect:
                // A remoção de MGEF direto é complexa. A forma mais comum é remover
                // o SPELL que o aplicou. Se o MGEF foi aplicado por um spell
                // adicionado por esta função, a lógica abaixo funciona.
            case AppliedEffect::EffectType::Spell:
                if (auto spell = form->As<RE::SpellItem>()) {
                    if (actor->HasSpell(spell)) {  // Verifica antes de remover
                        SKSE::log::info("Removendo Spell/Effect Source: {}", spell->GetName());
                        actor->RemoveSpell(spell);
                        // IMPORTANTE: Isso remove o spell da lista, mas não necessariamente
                        // remove MGEFs ativos instantaneamente se eles tiverem duração.
                        // Pode ser necessário usar Dispel ou funções SKSE mais avançadas se
                        // a remoção imediata de MGEFs for crucial.
                    }
                }
                break;
        }
    }

    // 2. Encontra efeitos para ADICIONAR (presentes em newEffects, mas NÃO em oldEffects)
    std::vector<AppliedEffect> toAdd;
    std::set_difference(newEffects.begin(), newEffects.end(), oldEffects.begin(), oldEffects.end(),
                        std::back_inserter(toAdd));

    for (const auto& effect : toAdd) {
        RE::TESForm* form = RE::TESForm::LookupByID(effect.formID);
        if (!form) {
            SKSE::log::warn("ApplyAndTrackEffects: FormID {:08X} não encontrado para adição.", effect.formID);
            continue;
        }

        switch (effect.type) {
            case AppliedEffect::EffectType::Perk:
                if (auto perk = form->As<RE::BGSPerk>()) {
                    if (!actor->HasPerk(perk)) {  // Verifica antes de adicionar
                        SKSE::log::info("Adicionando Perk: {}", perk->GetName());
                        actor->AddPerk(perk, 1);  // Adiciona rank 1
                    }
                }
                break;
            case AppliedEffect::EffectType::MagicEffect:
                if (auto mgef = form->As<RE::EffectSetting>()) {
                    // Aplicar MGEF diretamente é difícil. A abordagem padrão é
                    // criar/encontrar um Spell que contenha APENAS este MGEF
                    // e adicioná-lo ao jogador.
                    SKSE::log::warn("Aplicação direta de Magic Effect ({}) não é ideal. Considere usar um Spell.",
                                    mgef->GetName());
                    // Exemplo (requer helper GetOrCreateSpellForMGEF):
                    // RE::SpellItem* dummySpell = GetOrCreateSpellForMGEF(mgef);
                    // if (dummySpell && !actor->HasSpell(dummySpell)) {
                    //     actor->AddSpell(dummySpell);
                    // }
                }
                break;
            case AppliedEffect::EffectType::Spell:
                //if (auto spell = form->As<RE::SpellItem>()) {
                //    // Adiciona Habilidades/Poderes Menores passivamente
                //    if (spell->GetSpellType() == RE::MagicSystem::SpellType::kAbility ||
                //        spell->GetSpellType() == RE::MagicSystem::SpellType::kLesserPower) {
                //        if (!actor->HasSpell(spell)) {
                //            SKSE::log::info("Adicionando Habilidade/Poder Menor: {} ({:08X})", spell->GetName(),
                //                            spell->GetFormID());
                //            actor->AddSpell(spell);
                //        }
                //    } else {  // Casta outros tipos de Spells
                //        SKSE::log::info("Tentando castar Spell: {} ({:08X})", spell->GetName(), spell->GetFormID());

                //        // --- CORREÇÃO: Usar função membro do Actor ou utility function ---
                //        // Verifica se o ator pode castar usando o MagicCaster apropriado
                //        auto caster = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
                //        RE::MagicSystem::CannotCastReason reason;
                //        if (caster && actor->CheckCast(spell, false, &reason)) {  // Usa CheckCast do Actor
                //            // Se puder castar, inicia o cast usando uma função apropriada.
                //            // Muitas vezes, para casts instantâneos "fire and forget",
                //            // AddSpell pode ser suficiente se o spell tiver as flags corretas,
                //            // ou usamos funções de nível superior se disponíveis.
                //            // Uma alternativa comum é usar Papyrus ou SKSE para iniciar o cast.

                //            // Tentativa 1: Usar AddSpell pode funcionar para alguns spells "fire and forget"
                //            // actor->AddSpell(spell); // Isso pode não *castar* imediatamente.

                //            // Tentativa 2: A forma mais robusta costuma ser via SKSE ou Papyrus Utils se disponíveis.
                //            // Exemplo com SKSE (se você tiver SKSE/Papyrus integrado):
                //            // auto papyrusVm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
                //            // RE::BSTSmartPointer<RE::BSScript::IObjectHandlePolicy> policy; // Obtenha a policy
                //            // RE::VMHandle handle = policy->GetHandleForObject(actor->GetFormType(), actor);
                //            // if(papyrusVm && handle != policy->EmptyHandle()) {
                //            //    auto args = RE::MakeFunctionArguments(spell);
                //            //    papyrusVm->DispatchStaticCall("Actor", "Cast", args); // Chamando Actor.Cast() do
                //            //    Papyrus SKSE::log::info("Cast iniciado via Papyrus Actor.Cast");
                //            // } else {
                //            //    SKSE::log::error("Falha ao obter VM ou Handle para castar {}", spell->GetName());
                //            // }

                //            // *** Solução de Fallback Comum (usando funções do jogo via RE/REL) ***
                //            // Se as opções acima não funcionarem, esta é uma forma mais direta,
                //            // embora dependa de encontrar o offset correto para a função de cast.
                //            // Esta função específica pode variar, mas uma candidata comum é:
                //            using CastFunction = void (*)(RE::MagicCaster*, RE::MagicItem*, bool, float, bool, float,
                //                                          RE::TESObjectREFR*);
                //            // Obtenha o endereço da função (pode precisar de REL::ID ou Offset Scanner)
                //            // REL::Relocation<CastFunction> func{ REL::ID(xxxxx) }; // Substitua xxxxx pelo ID correto
                //            // if (func) {
                //            //    func(caster, spell, false, 1.0f, false, 0.0f, actor); // Chama a função do jogo
                //            //    SKSE::log::info("Cast iniciado via função nativa.");
                //            // } else {
                //            //    SKSE::log::error("Função nativa de Cast não encontrada.");
                //            // }

                //            // *** A Opção Mais Simples (mas pode não ser ideal para todos os spells): Adicionar e
                //            // Remover *** Isso garante que os MGEFs sejam aplicados, mas não é um "cast" real.
                //            if (!actor->HasSpell(spell)) {  // Evita adicionar múltiplas vezes
                //                SKSE::log::info("Adicionando Spell (como habilidade temporária): {}", spell->GetName());
                //                actor->AddSpell(spell);
                //                // Idealmente, você precisaria de um mecanismo para remover este spell depois,
                //                // talvez ao trocar de stance/moveset novamente, usando a lógica de `toRemove`.
                //            }

                //        } else {
                //            SKSE::log::warn("Não foi possível castar {}. Razão: {}", spell->GetName(),
                //                            static_cast<int>(reason));
                //        }
                //    }
                //}
                if (auto spell = form->As<RE::SpellItem>()) {
                    // --- CORREÇÃO: Simplificado para sempre usar AddSpell ---
                    // Verifica se o ator já não tem o spell antes de adicionar
                    if (!actor->HasSpell(spell)) {
                        // Adiciona QUALQUER tipo de spell configurado passivamente
                        SKSE::log::info("Adicionando Spell (passivo): {} ({:08X})", spell->GetName(),
                                        spell->GetFormID());
                        actor->AddSpell(spell);
                        // A lógica em 'toRemove' cuidará da remoção posterior.
                    } else {
                        SKSE::log::debug("Spell {} ({:08X}) já presente no ator.", spell->GetName(),
                                         spell->GetFormID());
                    }
                    // --- FIM DA CORREÇÃO ---
                } else {
                    SKSE::log::warn("FormID {:08X} (Plugin: {}) não é um SpellItem válido para adição.", effect.formID,
                                    effect.pluginName);
                }
                break;
        }
    }

    // 3. Atualiza a lista de rastreamento para a próxima mudança
    lastAppliedEffects = newEffectsConst;  // Armazena a lista NÃO ordenada original
}

void GlobalControl::TriggerSmartRandomNumber([[maybe_unused]] const std::string& eventSource) {
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return;
    }

    std::string category = GetCurrentWeaponCategoryName();
    int stanceIndex = g_currentStance - 1;
    int maxMovesets = AnimationManager::GetMaxMovesetsFor(category, stanceIndex);

    if (maxMovesets <= 0) {  // Alterado para <= 0, pois 1 moveset não tem o que ciclar.
        return;
    }

    int nextMoveset = 1;

    // --- INÍCIO DA NOVA LÓGICA ---
    if (Settings::RandomCycle) {  // Se a nova checkbox "Random cycle" estiver ativa
        if (maxMovesets > 1) {
            // Nova lógica aleatória sem restrições
            std::random_device rd;
            std::mt19937 gen(rd());
            // Gera um número entre 1 e maxMovesets, garantindo que não seja o mesmo que o atual.
            std::uniform_int_distribution<> distrib(1, maxMovesets);
            do {
                nextMoveset = distrib(gen);
            } while (nextMoveset == g_currentMoveset);
        }
    } else {  // Se for o cycle moveset padrão (agora sequencial)
        nextMoveset = g_currentMoveset + 1;
        if (nextMoveset > maxMovesets) {
            nextMoveset = 1;  // Volta para o primeiro
        }
    }
    // --- FIM DA NOVA LÓGICA ---

    g_currentMoveset = nextMoveset;
    player->SetGraphVariableInt("testarone", g_currentMoveset);
    UpdatePowerAttackGlobals();
    UpdateSkyPromptTexts();

    // A lógica de comboState não é mais necessária para o modo sequencial ou o novo modo aleatório
    // g_comboState.previousMoveset = g_comboState.lastMoveset;
    // g_comboState.lastMoveset = nextMoveset;

    if (ShouldShowPrompts()) {
        SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID);
        SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);
    }
}

bool GlobalControl::IsAnyMenuOpen() { 
    const auto ui = RE::UI::GetSingleton();
   /* if (!ui->menuStack.empty()) {
        return true;
    }*/

    // 2. Verifica menus que precisam do cursor (a maioria dos menus ImGui, como o Wheeler)
    // Se este contador for maior que 0, algo está forçando o cursor a aparecer.
    for (const auto a_name : blockedMenus) {
       if (ui->IsMenuOpen(a_name)) {
        return true;
       }
    }
    return false;
}


bool GlobalControl::IsThirdPerson() {
    return !RE::PlayerCamera::GetSingleton()->IsInFirstPerson();
}

RE::BSEventNotifyControl GlobalControl::MenuOpen::ProcessEvent(const RE::MenuOpenCloseEvent* event,
                                                               RE::BSTEventSource<RE::MenuOpenCloseEvent>*) {
    if (!event) {
             return RE::BSEventNotifyControl::kContinue;
    }
    const std::array<RE::BSFixedString, 3> relevantMenus = {RE::InventoryMenu::MENU_NAME, RE::ContainerMenu::MENU_NAME,
                                                            RE::FavoritesMenu::MENU_NAME};
    bool isRelevantMenu = false;
    for (const auto& menuName : relevantMenus) {
        if (event->menuName == menuName) {
            isRelevantMenu = true;
            break;
        }
    }

    if (isRelevantMenu) {
        // A mágica acontece aqui!
        // event->opening é 'true' se o menu está abrindo, e 'false' se está fechando.
        // Nós simplesmente atribuímos esse valor à nossa flag.
        Hooks::is_open.store(event->opening);
        //logger::info("Menu relevante '{}' mudou de estado. is_open agora é: {}", event->menuName.c_str(),event->opening);
    }
    if (event->opening) {

        if (Cycleopen) {
            Cycleopen = false;
            SkyPromptAPI::RemovePrompt(StancesSink::GetSingleton(), g_clientID);
            SkyPromptAPI::RemovePrompt(MovesetSink::GetSingleton(), g_clientID);
        }
    }
    // Se um menu está FECHANDO
    else {
        // Após o fechamento, verificamos se NENHUM outro menu está aberto.
        // É importante chamar IsAnyMenuOpen() AQUI.
        if (ShouldShowPrompts() && !Cycleopen) {
            Cycleopen = true;
            UpdateSkyPromptTexts();
            SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID);
            SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);
        }
    }
    
    return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl GlobalControl::AnimationEventHandler::ProcessEvent(
    const RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) {
    auto player = RE::PlayerCharacter::GetSingleton();
     // --- LOG DE DIAGNÓSTICO ---
    // Apenas loga se o timer deveria estar rodando, para não poluir o log 100% do tempo
    if (g_comboState.isTimerRunning) {
        auto now = std::chrono::steady_clock::now();
        auto time_left_ms = std::chrono::duration_cast<std::chrono::milliseconds>(g_comboState.comboTimeoutTimestamp - now).count();
        //SKSE::log::info("[UpdateHandler] Checando timer... g_comboState.isTimerRunning: {}. Tempo restante: {} ms", g_comboState.isTimerRunning,time_left_ms);
    }
    

    if (a_event && a_event->holder && a_event->holder->IsPlayerRef()) {
        const std::string_view eventName = a_event->tag;
        if (g_comboState.isTimerRunning && std::chrono::steady_clock::now() >= g_comboState.comboTimeoutTimestamp) {
            g_comboState.isTimerRunning = false;

            // --- LOG DE DIAGNÓSTICO ---
            //SKSE::log::info("[UpdateHandler] TIMEOUT! Fim de combo.");

            if (Settings::CycleMoveset) {
                SKSE::GetTaskInterface()->AddTask([]() { TriggerSmartRandomNumber("Fim de Combo (C++)"); });
            }
        }
        else if(eventName == "weaponSwing" || eventName == "weaponLeftSwing" ||
            eventName == "h2hAttack" || eventName == "PowerAttack_Start_end") {
            //SKSE::log::info("[AnimationEventHandler] Evento '{}' detectado. Timer INICIADO. g_comboState.isTimerRunning AGORA É: {}",eventName, g_comboState.isTimerRunning);
            //SKSE::log::info("[AnimationEventHandler] Evento '{}' detectado. Timer INICIADO.", eventName);
            // Apenas definimos o estado e o momento em que o combo deve terminar.
            g_comboState.isTimerRunning = true;
            auto timeout_ms = std::chrono::milliseconds(static_cast<int>(Settings::CycleTimer * 1000));
            g_comboState.comboTimeoutTimestamp = std::chrono::steady_clock::now() + timeout_ms;
            

        } else if (eventName == "weaponDraw" || eventName == "weaponSheathe") {
            g_comboState.isTimerRunning = false;  // Cancela qualquer combo pendente
            if (Settings::CycleMoveset) {
                TriggerSmartRandomNumber(std::string(eventName));
            }
        }

        //if (eventName == "HitFrame") {
        //    SKSE::log::info("Evento 'HitFrame' recebido do jogador!");
        //    // Coloque aqui a sua lógica para o HitFrame...

        //} else if (eventName == "Bfco_AttackStartFX") {
        //    player->SetGraphVariableBool("NEW_BFCO_IsInComboWindow", true);
        //    player->SetGraphVariableInt("NEW_BFCO_IsNormalAttacking", 0);
        //    player->SetGraphVariableInt("NEW_BFCO_IsPowerAttacking", 0);
        //    SKSE::log::info("Evento 'Bfco_AttackStartFX' recebido. Abrindo a janela de combo...");
        //    // Ex: GetGraphVariable("BFCO_IsInComboWindow")->SetBool(true);

        //} else if (eventName == "MCO_PowerWinClose" || eventName == "MCO_WinClose") {
        //    player->SetGraphVariableBool("NEW_BFCO_IsInComboWindow", false);
        //    //player->SetGraphVariableInt("NEW_BFCO_IsNormalAttacking", 0);
        //    //player->SetGraphVariableInt("NEW_BFCO_IsPowerAttacking", 1);
        //    SKSE::log::info("Evento 'BFCO_IsPlayerInputOK' recebido. Fechando a janela de combo...");

        //}else if (eventName == "MCO_PowerWinOpen" || eventName == "MCO_WinOpen") {
        //    player->SetGraphVariableBool("NEW_BFCO_IsInComboWindow", true);
        //    player->SetGraphVariableInt("NEW_BFCO_IsPowerAttacking", 1);
        //    //player->SetGraphVariableInt("NEW_BFCO_IsNormalAttacking", 0);
        //    //player->SetGraphVariableInt("NEW_BFCO_IsPowerAttacking", 1);
        //    SKSE::log::info("Evento 'BFCO_IsPlayerInputOK' recebido. Fechando a janela de combo...");

        //}

    }
    return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl GlobalControl::NpcCycleSink::ProcessEvent(const RE::BSAnimationGraphEvent* a_event,
                                                                   RE::BSTEventSource<RE::BSAnimationGraphEvent>*) {

    // a_event->holder nos dá o ator que gerou o evento.
    if (a_event && a_event->holder) {
        auto actor = a_event->holder->As<RE::Actor>();
        if (!actor) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto npc = const_cast<RE::Actor*>(actor);
        const RE::FormID formID = actor->GetFormID();
        const std::string_view eventName = a_event->tag;
        //logger::info("[NPC Anim Event] Ator: '{}' ({:08X}), Evento: '{}'", actor->GetName(), actor->GetFormID(),eventName);

        if (eventName == "weaponSwing") {
            // --- Lógica de atualização com Mutex ---
            std::lock_guard<std::mutex> lock(g_comboStateMutex);  // Trava o mutex (destrava automaticamente no fim do escopo)

            // Acessa (ou cria, se não existir) o estado de combo para ESTE ator específico
            auto& state = g_npcComboStates[formID];

            state.isTimerRunning = true;
            auto timeout_ms = std::chrono::milliseconds(static_cast<int>(fComboTimeout * 1000));
            state.comboTimeoutTimestamp = std::chrono::steady_clock::now() + timeout_ms;

            //SKSE::log::info("[AnimationEventHandler] Ator {:08X} iniciou/resetou combo com evento '{}'.", formID, eventName);

        } else if (eventName == "weaponDraw" || eventName == "weaponSheathe") {
            std::lock_guard<std::mutex> lock(g_comboStateMutex);
            // Se o ator estiver no nosso mapa, cancela o timer dele.
            if (g_npcComboStates.count(formID)) {
                g_npcComboStates[formID].isTimerRunning = false;
            }
            
        }
    }
    // Lista para guardar os FormIDs dos atores cujo combo expirou.
    // Não podemos modificar o mapa enquanto iteramos sobre ele, então guardamos para depois.
    std::vector<RE::FormID> expiredCombos;
    auto now = std::chrono::steady_clock::now();

    {  // Criamos um escopo para o lock
        std::lock_guard<std::mutex> lock(g_comboStateMutex);

        for (auto& [formID, state] : g_npcComboStates) {
            if (state.isTimerRunning && now >= state.comboTimeoutTimestamp) {
                state.isTimerRunning = false;
                expiredCombos.push_back(formID);
            }
        }
    }  // O mutex é liberado aqui

    // Agora, fora do lock, disparamos o evento para cada combo que expirou.
    for (const auto& formID : expiredCombos) {
        // Precisamos encontrar o ponteiro do ator a partir do FormID
        auto actor = RE::TESForm::LookupByID<RE::Actor>(formID);
        if (actor) {
            //SKSE::log::info("[UpdateHandler] Combo do ator {:08X} expirou.", formID);
            // Adicionamos a lógica para chamar a função para o ator específico
            // Usando SKSE::GetTaskInterface() ainda é uma boa prática
            //SKSE::GetTaskInterface()->AddTask([actor]() { NPCrandomNumber(actor, "Fim de Combo"); });
        }
    }
    return RE::BSEventNotifyControl::kContinue;
}

void GlobalControl::NPCrandomNumber(RE::Actor* targetActor, const std::string& eventSource) {
    if (!targetActor) return;

    std::string category = GetActorWeaponCategoryName(targetActor);

    // 1. Pega a lista de candidatos DISPONÍVEIS
    std::vector<MovesetCandidate> availableMovesets =
        AnimationManager::GetSingleton()->GetAvailableMovesetIndices(targetActor, category);

    if (availableMovesets.size() < 2) {
        if (!availableMovesets.empty()) {
            const auto& chosenMoveset = availableMovesets[0];
            targetActor->SetGraphVariableInt("CycleMovesetNpcType", chosenMoveset.priority);
            targetActor->SetGraphVariableInt("testarone", chosenMoveset.index);
        }
        return;
    }

    // 2. A lógica de "random inteligente" agora opera sobre a lista de candidatos
    RE::FormID formID = targetActor->GetFormID();
    std::lock_guard<std::mutex> lock(g_comboStateMutex);
    auto& state = g_npcComboStates[formID];

    // 3. Filtra a lista para não repetir os 2 últimos
    std::vector<MovesetCandidate> choices = availableMovesets;
    choices.erase(std::remove(choices.begin(), choices.end(), state.lastMoveset), choices.end());
    choices.erase(std::remove(choices.begin(), choices.end(), state.previousMoveset), choices.end());

    if (choices.empty()) {
        choices = availableMovesets;
    }

    // Lógica de pesos (a mesma de antes)
    std::vector<double> weights;
    weights.reserve(choices.size());
    for (size_t i = 0; i < choices.size(); ++i) {
        weights.push_back(static_cast<double>(choices.size() - i));
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::discrete_distribution<> distrib(weights.begin(), weights.end());

    int chosenListIndex = distrib(gen);
    const MovesetCandidate& chosenMoveset = choices[chosenListIndex];

    // 4. AQUI ESTÁ A CORREÇÃO FINAL: Enviamos a prioridade e o índice corretos!
    targetActor->SetGraphVariableInt("CycleMovesetNpcType", chosenMoveset.priority);
    targetActor->SetGraphVariableInt("testarone", chosenMoveset.index);

    // 5. Atualiza o estado com o candidato completo
    state.previousMoveset = state.lastMoveset;
    state.lastMoveset = chosenMoveset;

    SKSE::log::info("{} (Ator {:08X}): Escolheu o moveset #{} da prioridade {}", eventSource, formID,
                    chosenMoveset.index, chosenMoveset.priority);
}

RE::BSEventNotifyControl GlobalControl::NpcCombatTracker::ProcessEvent(const RE::TESCombatEvent* a_event,
                                                                       RE::BSTEventSource<RE::TESCombatEvent>*) {
    if (!a_event || !a_event->actor) {
        return RE::BSEventNotifyControl::kContinue;
    }

    auto actor = a_event->actor.get();
    logger::info("Processando evento de combate para ator: {}", actor ? actor->GetName() : "Nulo");
    auto player = RE::PlayerCharacter::GetSingleton();
    // --- INÍCIO DA MODIFICAÇÃO ---

    // 1. Lógica específica para o JOGADOR
    if (a_event->actor->IsPlayerRef()) {
        // Se a configuração 'OnlyCombat' estiver desligada, não precisamos fazer nada aqui.
        if (!Settings::OnlyCombat) {
            return RE::BSEventNotifyControl::kContinue;
        }

        switch (a_event->newState.get()) {
            case RE::ACTOR_COMBAT_STATE::kCombat:
                // Jogador ENTROU em combate. Mostra o menu se as condições forem válidas.
                if (ShouldShowPrompts() && !Cycleopen) {
                    Cycleopen = true;
                    SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID);
                    SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);
                }
                break;

            case RE::ACTOR_COMBAT_STATE::kNone:
                // Jogador SAIU de combate. Esconde o menu.
                Cycleopen = false;
                SkyPromptAPI::RemovePrompt(StancesSink::GetSingleton(), g_clientID);
                SkyPromptAPI::RemovePrompt(MovesetSink::GetSingleton(), g_clientID);
                SkyPromptAPI::RemovePrompt(StancesChangesSink::GetSingleton(), MenuShowing);
                SkyPromptAPI::RemovePrompt(MovesetChangesSink::GetSingleton(), MenuShowing);
                break;
        }
        return RE::BSEventNotifyControl::kContinue;  // Finaliza após tratar o jogador
    }

    // 2. Lógica existente para NPCs (agora dentro de um else ou após o if do jogador)
    // O if original que ignorava o jogador foi removido daqui
    auto* npc = actor->As<RE::Actor>();
    if (npc) {  // Garante que é um ator válido
        switch (a_event->newState.get()) {
            case RE::ACTOR_COMBAT_STATE::kCombat:
                GlobalControl::NpcCombatTracker::RegisterSink(npc);
                CheckAndEquipDualTwoHandedForNPC(npc);
                break;
            case RE::ACTOR_COMBAT_STATE::kNone:
                GlobalControl::NpcCombatTracker::UnregisterSink(npc);
                CheckAndEquipDualTwoHandedForNPC(npc);
                break;
        }
    }

    // --- FIM DA MODIFICAÇÃO ---

    return RE::BSEventNotifyControl::kContinue;
}

void GlobalControl::NpcCombatTracker::RegisterSink(RE::Actor* a_actor) {
    if (!a_actor || a_actor->IsPlayerRef()) return;

    std::unique_lock lock(g_mutex);
    if (g_trackedNPCs.find(a_actor->GetFormID()) == g_trackedNPCs.end()) {
        a_actor->AddAnimationGraphEventSink(&g_npcSink);
        g_trackedNPCs.insert(a_actor->GetFormID());
        //SKSE::log::info("[NpcCombatTracker] Começando a rastrear animações do ator {:08X}", a_actor->GetFormID());
    }
}

void GlobalControl::NpcCombatTracker::UnregisterSink(RE::Actor* a_actor) {
    if (!a_actor || a_actor->IsPlayerRef()) return;

    std::unique_lock lock(g_mutex);
    if (g_trackedNPCs.find(a_actor->GetFormID()) != g_trackedNPCs.end()) {
        a_actor->RemoveAnimationGraphEventSink(&g_npcSink);
        g_trackedNPCs.erase(a_actor->GetFormID());
        //SKSE::log::info("[NpcCombatTracker] Parando de rastrear animações do ator {:08X}", a_actor->GetFormID());
    }
}

void GlobalControl::NpcCombatTracker::RegisterSinksForExistingCombatants() {
    SKSE::log::info("[NpcCombatTracker] Verificando NPCs já em combate após carregar o jogo...");

    auto* processLists = RE::ProcessLists::GetSingleton();
    if (!processLists) {
        SKSE::log::warn("[NpcCombatTracker] Não foi possível obter ProcessLists.");
        return;
    }

    // Itera sobre todos os atores que estão "ativos" no jogo
    for (auto& actorHandle : processLists->highActorHandles) {
        if (auto actor = actorHandle.get().get()) {
            // A função IsInCombat() nos diz se o ator já está em um estado de combate
            if (!actor->IsPlayerRef()) {
                CheckAndEquipDualTwoHandedForNPC(actor);
                if (actor->IsInCombat()) {
                    SKSE::log::info("[NpcCombatTracker] Ator '{}' ({:08X}) já está em combate. Registrando sink...",
                                    actor->GetName(), actor->GetFormID());
                    // Usamos a mesma função de registro que já existe!
                    RegisterSink(actor);
                }
            }

        }
    }
    SKSE::log::info("[NpcCombatTracker] Verificação concluída.");
}

void GlobalControl::UpdateSkyPromptTexts() {
    auto animManager = AnimationManager::GetSingleton();
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;  // Não faz nada se o jogador não existir

    std::string categoryName = GetCurrentWeaponCategoryName();

    // --- NOVA LÓGICA PARA STANCES ---

    // 1. Busca APENAS as stances que o jogador pode usar (com perks e não vazias)
    const auto availableStances = animManager->GetAvailableStances(player, categoryName);
    const int numStances = availableStances.size();
    int currentStanceListIndex = -1;  // Índice na lista (0-based)
    int originalStanceIndex = 0;      // Índice real da stance (1-based)
    // --- LÓGICA PARA STANCES  ---
    if (g_currentStance > 0 && g_currentStance <= numStances) {
        currentStanceListIndex = g_currentStance - 1;
        originalStanceIndex = availableStances[currentStanceListIndex].originalIndex;
    } else {
        g_currentStance = 0;  // Reseta se o índice for inválido (ex: mudou de arma)
    }

    if (g_currentStance == 0) {
        StanceText = "Styles";                                                     // Texto padrão
        StanceNextText = (numStances > 0) ? availableStances[0].name : "Next";      // Próximo é o primeiro da lista
        StanceBackText = (numStances > 0) ? availableStances.back().name : "Back";  // Anterior é o último da lista
    } else {
        // Calcula os índices de ciclo DENTRO da lista de stances disponíveis
        int nextListIndex = (currentStanceListIndex + 1) % numStances;
        int backListIndex = (currentStanceListIndex - 1 + numStances) % numStances;

        // Pega os nomes da nossa lista
        StanceText = availableStances[currentStanceListIndex].name;
        StanceNextText = availableStances[nextListIndex].name;
        StanceBackText = availableStances[backListIndex].name;
    }

    // --- NOVA LÓGICA PARA MOVESETS ---

    // 2. Busca APENAS os movesets disponíveis para a stance selecionada
    const auto availableMovesets = animManager->GetAvailableMovesets(player, categoryName, originalStanceIndex);
    const int maxMovesets = availableMovesets.size();

    int currentMovesetListIndex = -1;  // Índice na lista (0-based)
    int originalMovesetIndex = 0;      // Índice real do moveset (1-based)

    if (g_currentMoveset > 0 && g_currentMoveset <= maxMovesets) {
        currentMovesetListIndex = g_currentMoveset - 1;
        originalMovesetIndex = availableMovesets[currentMovesetListIndex].originalIndex;
    } else {
        g_currentMoveset = (maxMovesets > 0) ? 1 : 0;  // Reseta para 1 se houver movesets, 0 se não
        if (g_currentMoveset > 0) {
            currentMovesetListIndex = 0;
            originalMovesetIndex = availableMovesets[0].originalIndex;
        }
    }

    // O GetCurrentMovesetName ainda é útil pois ele lida com a lógica de nomes direcionais
    // Mas agora passamos os índices ORIGINAIS corretos para ele.
    int stanceIdxForName =
        (originalStanceIndex > 0) ? originalStanceIndex - 1 : -1;  // Converte de 1-based para 0-based

    if (maxMovesets > 0) {
        int dirState = InputListener::GetDirectionalState();

        std::string currentMovesetName =
            animManager->GetCurrentMovesetName(player, categoryName, stanceIdxForName, originalMovesetIndex, dirState);

        // Mostra o índice da LISTA (ex: 1/2) e não o índice original
        MovesetText = std::format("{} ({}/{})", currentMovesetName, g_currentMoveset, maxMovesets);

        if (maxMovesets > 1) {
            // Calcula os próximos índices na LISTA
            int nextMovesetListIndex = (currentMovesetListIndex + 1) % maxMovesets;
            int backMovesetListIndex = (currentMovesetListIndex - 1 + maxMovesets) % maxMovesets;

            // Pega os índices ORIGINAIS correspondentes
            int nextOriginalIndex = availableMovesets[nextMovesetListIndex].originalIndex;
            int backOriginalIndex = availableMovesets[backMovesetListIndex].originalIndex;

            // Busca os nomes usando os índices ORIGINAIS
            MovesetNextText =
                animManager->GetCurrentMovesetName(player,categoryName, stanceIdxForName, nextOriginalIndex, 0);
            MovesetBackText =
                animManager->GetCurrentMovesetName(player,categoryName, stanceIdxForName, backOriginalIndex, 0);
        } else {
            MovesetNextText = "Back";
            MovesetBackText = "Next";
        }
    } else {
        MovesetText = "Moves";
        MovesetNextText = "Back";
        MovesetBackText = "Next";
    }

    stance_actual = SkyPromptAPI::Prompt(StanceText, 0, 0, SkyPromptAPI::PromptType::kSinglePress, 20,
                                         Stances_menu,
                                         0xFFFFFFFF, 0.999f);
    moveset_actual = SkyPromptAPI::Prompt(MovesetText, 1, 0, SkyPromptAPI::PromptType::kSinglePress,
                                          20, Moveset_menu,
                                          0xFFFFFFFF, 0.999f);
    menu_stance = SkyPromptAPI::Prompt(StanceText, 0, 0, SkyPromptAPI::PromptType::kHoldAndKeep,
                                       Settings::ShowMenu ? 20 : 0, Stances_menu);
    stance_next = SkyPromptAPI::Prompt(StanceNextText, 3, 0, SkyPromptAPI::PromptType::kSinglePress,
                                       20, Next_key);
    stance_back = SkyPromptAPI::Prompt(StanceBackText, 2, 0, SkyPromptAPI::PromptType::kSinglePress,
                                       20, Back_key);
    menu_moveset = SkyPromptAPI::Prompt(MovesetText, 1, 0, SkyPromptAPI::PromptType::kHoldAndKeep,
                                        Settings::ShowMenu ? 20 : 0, Moveset_menu);
    moveset_next = SkyPromptAPI::Prompt(MovesetNextText, 3, 0, SkyPromptAPI::PromptType::kSinglePress,
                                        20, Next_key);
    moveset_back = SkyPromptAPI::Prompt(MovesetBackText, 2, 0, SkyPromptAPI::PromptType::kSinglePress,
                                        20, Back_key);

    StancesSink::GetSingleton()->UpdatePrompts();
    StancesChangesSink::GetSingleton()->UpdatePrompts();
    MovesetSink::GetSingleton()->UpdatePrompts();
    MovesetChangesSink::GetSingleton()->UpdatePrompts();
}

void GlobalControl::UpdatePowerAttackGlobals() {
    if (!Settings::bfcoDirectionalAttacks) {
        return;  // Se a opção não estiver ativa, não faz nada.
    }
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;

    std::string category = GetCurrentWeaponCategoryName();
    int stanceIndex = g_currentStance > 0 ? g_currentStance - 1 : 0;
    int movesetIndex = g_currentMoveset;

    // --- ALTERAÇÃO PRINCIPAL AQUI ---
    // 1. O tipo da variável "tags" agora precisa do escopo da classe.
    // 2. A função é chamada através do singleton do AnimationManager.
    MovesetTags tags = AnimationManager::GetSingleton()->GetCurrentMovesetTags(category, stanceIndex, movesetIndex);
    // --- FIM DA ALTERAÇÃO ---
    int directionalState = 0;
    if (bool success = player->GetGraphVariableInt("DirecionalCycleMoveset", directionalState); success) {
        // SUCESSO! A chamada funcionou.
        // 'directionalState' agora tem o "valor que veio da graphvalue".
        // Use a variável aqui.
        //SKSE::log::info("Valor obtido com sucesso: {}", directionalState);

    } else {
        // FALHA! A chamada não funcionou.
        // Lide com o erro aqui. A variável 'directionalState' não foi alterada.
        SKSE::log::warn("Não foi possível obter o valor de 'DirecionalCycleMoveset'.");
    }
    bool isDpaAvailableForCurrentDirection = false;
    switch (directionalState) {
        case 1:  // Frente
            isDpaAvailableForCurrentDirection = tags.dpaTags.hasA;
            break;
        case 5:  // Trás
            isDpaAvailableForCurrentDirection = tags.dpaTags.hasB;
            break;
        case 7:  // Esquerda
            isDpaAvailableForCurrentDirection = tags.dpaTags.hasL;
            break;
        case 3:  // Direita
            isDpaAvailableForCurrentDirection = tags.dpaTags.hasR;
            break;
        // Para todas as outras direções (diagonais, parado), o valor será false.
        default:
            isDpaAvailableForCurrentDirection = false;
            break;
    }
    const auto tesDataHandler = RE::TESDataHandler::GetSingleton();

    if (tesDataHandler) {
       RE::TESGlobal* bfcoDPA_Global = tesDataHandler->LookupForm<RE::TESGlobal>(0x84E, "SCSI-ACTbfco-Main.esp");

        if (bfcoDPA_Global) {
         bfcoDPA_Global->value = isDpaAvailableForCurrentDirection ? 1.0f : 0.0f;

         //SKSE::log::info("[UpdatePowerAttack] Global 'bfcoTG_DirPowerAttack' set to {}",bfcoDPA_Global->value);
        }
        else {
          //SKSE::log::warn("[UpdatePowerAttack] Global 'bfcoTG_DirPowerAttack' não encontrado.");

        }
    }

    player->SetGraphVariableBool("BFCO_HasCombo", tags.hasCPA);
    //SKSE::log::info("[UpdatePowerAttack] GraphVar 'BFCO_HasCombo' set to {}", tags.hasCPA);
}

bool GlobalControl::ShouldShowPrompts() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return false;
    }

    // --- Início da Seção de Depuração ---

    // 1. Avalia cada condição individualmente e armazena em uma variável
    bool settingOnlyCombat = Settings::OnlyCombat;
    bool playerInCombat = player->IsInCombat();
    bool combatConditionMet = !settingOnlyCombat || playerInCombat;

    bool weaponDrawn = g_isWeaponDrawn;
    bool thirdPerson = IsThirdPerson();
    bool noMenusOpen = !IsAnyMenuOpen();  

    // 2. Imprime o status de cada condição no log
    // Use logger::info ou logger::debug, dependendo de como seu log está configurado
    /*logger::info("--- [Debug ShouldShowPrompts] ---");
    logger::info("1. Condição de Combate:");
    logger::info("   - Settings::OnlyCombat = {}", settingOnlyCombat);
    logger::info("   - player->IsInCombat() = {}", playerInCombat);
    logger::info("   -> combatConditionMet   = {}", combatConditionMet);
    logger::info("---------------------------------");
    logger::info("2. Outras Condições:");
    logger::info("   - g_isWeaponDrawn      = {}", weaponDrawn);
    logger::info("   - IsThirdPerson()      = {}", thirdPerson);
    logger::info("   - !IsAnyMenuOpen()     = {}", noMenusOpen);
    logger::info("---------------------------------");*/

    // 3. Calcula o resultado final
    bool finalResult = weaponDrawn && thirdPerson && noMenusOpen && combatConditionMet;

    /*logger::info("==> RESULTADO FINAL: {}", finalResult);
    logger::info("--- [Fim do Debug] ---");*/

    // --- Fim da Seção de Depuração ---

    return finalResult;
}

void GlobalControl::UpdatePromptVisibility() {
    bool shouldBeVisible = ShouldShowPrompts();

    // A variável 'Cycleopen' rastreia se os prompts JÁ ESTÃO visíveis.

    if (shouldBeVisible && !Cycleopen) {
        // CONDIÇÃO: Deveriam estar visíveis, mas não estão -> MOSTRAR
        //logger::info("[UpdatePromptVisibility] Condições atendidas. Mostrando prompts.");
        Cycleopen = true;
        // Talvez seja necessário atualizar os textos antes de enviar
        UpdateSkyPromptTexts();
        SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID);
        SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);

    } else if (!shouldBeVisible && Cycleopen) {
        // CONDIÇÃO: Não deveriam estar visíveis, mas estão -> ESCONDER
        //logger::info("[UpdatePromptVisibility] Condições não atendidas. Escondendo prompts.");
        Cycleopen = false;
        SkyPromptAPI::RemovePrompt(StancesSink::GetSingleton(), g_clientID);
        SkyPromptAPI::RemovePrompt(MovesetSink::GetSingleton(), g_clientID);
        SkyPromptAPI::RemovePrompt(StancesChangesSink::GetSingleton(), MenuShowing);
        SkyPromptAPI::RemovePrompt(MovesetChangesSink::GetSingleton(), MenuShowing);
    }
}

void GlobalControl::Intall() {
    auto& trampoline = SKSE::GetTrampoline();
    constexpr size_t size_per_hook = 14;
    trampoline.create(size_per_hook * 3);
    const REL::Relocation<std::uintptr_t> target{REL::RelocationID(37938, 38894)};  
    Equip2H::func =
        trampoline.write_call<5>(target.address() + REL::Relocate(0xe5, 0x170), Equip2H::thunk);
    const REL::Relocation<std::uintptr_t> targetU{REL::RelocationID(37945, 38901)};  
    Unequip2H::func = trampoline.write_call<5>(targetU.address() + REL::Relocate(0x138, 0x1b9), Unequip2H::thunk);
}

std::span<const SkyPromptAPI::Prompt> GlobalControl::EquipMenu::GetPrompts() const { return prompts; }



void GlobalControl::EquipMenu::ProcessEvent(SkyPromptAPI::PromptEvent event) const {
    if (event.type != SkyPromptAPI::PromptEventType::kAccepted) {
        return;
    }

    auto player = RE::PlayerCharacter::GetSingleton();
    // Usa a função auxiliar para pegar o item que estava sob o cursor
    const auto itemEntry = Hooks::GetSelectedEntryInMenu();

    if (!player || !itemEntry) {
        return;
    }
    #ifdef GetObject
    #undef GetObject
    #endif
    auto itemToEquip = itemEntry->GetObject();
    if (!itemToEquip) {
        return;
    }

    switch (event.prompt.eventID) {
        case 0:  // Equipar na mão ESQUERDA
            logger::info("Equipando {} na mão esquerda.", itemToEquip->GetName());
            EquipItemWithGripChange(player, itemToEquip, Hooks::g_leftHandSlot);
            break;

        case 1:  // Equipar na mão DIREITA
            logger::info("Equipando {} na mão direita.", itemToEquip->GetName());
            EquipItemWithGripChange(player, itemToEquip, Hooks::g_rightHandSlot);
            break;
        case 2:  // Equipar nas duas mãos
            logger::info("Equipando {} nas duas maos.", itemToEquip->GetName());
            EquipItemWithGripChange(player, itemToEquip, Hooks::g_twoHandSlot);
            break;
    }
}

void GlobalControl::EquipMenu::Show(const RE::TESBoundObject* a_weapon) const {
    // A chave está aqui: RE::Inventory3DManager
    // Este singleton gerencia o modelo 3D que você vê no menu.
    if (const auto a_ref = RE::Inventory3DManager::GetSingleton()->tempRef) {
        // 'tempRef' é uma referência temporária para o modelo 3D na tela.
        // Pegamos o FormID dela para usar como nosso refid.
        const auto refid = a_ref->GetFormID();

        // ATUALIZAMOS O REFID DOS NOSSOS PROMPTS!
        Left_Hand.refid = refid;
        Right_Hand.refid = refid;
    } else {
        // Se, por algum motivo, não houver um modelo 3D, voltamos para 0.
        Left_Hand.refid = 0;
        Right_Hand.refid = 0;
    }

    // Agora que os prompts têm o refid correto, nós os enviamos.
    SkyPromptAPI::SendPrompt(this, GlobalControl::Dynamicgrip);  
}

void GlobalControl::EquipMenu::Hide() const {
    SkyPromptAPI::RemovePrompt(this, GlobalControl::Dynamicgrip);  
}

RE::BSEventNotifyControl GlobalControl::EquipEventSink::ProcessEvent(const RE::TESEquipEvent* a_event,
                                                                     RE::BSTEventSource<RE::TESEquipEvent>*) {
    // Só nos importamos com eventos do jogador e quando ele está EQUIPANDO algo
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!a_event || a_event->actor.get() != player || !a_event->equipped) {
        return RE::BSEventNotifyControl::kContinue;
    }

    return RE::BSEventNotifyControl::kContinue;
}



void GlobalControl::Equip2H::thunk(std::int64_t* a, RE::Actor* a_actor, RE::TESForm* a_form, std::int64_t* extraData,
                                   int count, std::int64_t* equipSlot, char queueEquip, char forceEquip,
                                   char playSounds, char applyNow) {
    RE::TESObjectWEAP* weapon = nullptr;
    RE::BGSEquipSlot* originalSlot = nullptr;

    // 1. VERIFICAR E ALTERAR (ANTES de chamar func)
    if (a_form && a_form->IsWeapon() && isTwoHanded(a_form)) {
        weapon = a_form->As<RE::TESObjectWEAP>();
        originalSlot = weapon->GetEquipSlot();  // Salva o slot original (ex: g_twoHandSlot)

        // Força o jogo a pensar que é um item de mão direita
        
        weapon->SetEquipSlot(Hooks::g_rightHandSlot);
        func(a, a_actor, a_form, extraData, count, equipSlot, queueEquip, true, playSounds, true);

        // 3. RESTAURAR (DEPOIS de chamar func)
        if (weapon && originalSlot) {
            // Restaura o slot original para o estado normal (2H)
            weapon->SetEquipSlot(originalSlot);
        }
        return;
    }

    
    return func(a, a_actor, a_form, extraData, count, equipSlot, queueEquip, forceEquip, playSounds, applyNow);
}

std::int64_t GlobalControl::Unequip2H::thunk(std::int64_t* a, RE::Actor* a_actor, RE::TESForm* a_form,
                                             std::int64_t* extraData) {
    RE::TESObjectWEAP* weapon = nullptr;
    RE::BGSEquipSlot* originalSlot = nullptr;

    // 1. VERIFICAR E ALTERAR (ANTES de chamar func)
    if (a_form && a_form->IsWeapon() && isTwoHanded(a_form)) {
        weapon = a_form->As<RE::TESObjectWEAP>();
        originalSlot = weapon->GetEquipSlot();
        weapon->SetEquipSlot(Hooks::g_rightHandSlot);  // Finge que é 1H para desequipar
    }

    // 2. CHAMAR A FUNÇÃO ORIGINAL DO JOGO
    std::int64_t result = func(a, a_actor, a_form, extraData);

    // 3. RESTAURAR (DEPOIS de chamar func)
    if (weapon && originalSlot) {
        weapon->SetEquipSlot(originalSlot);  // Restaura para 2H
    }

    return result;
}
