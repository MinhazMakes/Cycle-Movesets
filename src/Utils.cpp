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
    // No dynamicGrip, isso inclui espadas e machados de duas mãos.
    if (weap->IsTwoHandedSword() || weap->IsTwoHandedAxe()) return true;
    return false;
}
void EquipItemWithGripChange(RE::Actor* actor, RE::TESBoundObject* item, RE::BGSEquipSlot* targetSlot) {
    if (!actor || !item || !targetSlot) return;

    auto equipManager = RE::ActorEquipManager::GetSingleton();
    if (!equipManager) return;

    if (item->IsWeapon() && isTwoHanded(item)) {
        auto weapon = item->As<RE::TESObjectWEAP>();
        auto originalSlot = weapon->GetEquipSlot();

        // A lógica para enganar o motor do jogo continua a mesma
        RE::BGSEquipSlot* tempSlot =
            (targetSlot == Hooks::g_rightHandSlot) ? Hooks::g_leftHandSlot : Hooks::g_rightHandSlot;
        weapon->SetEquipSlot(tempSlot);

        SKSE::GetTaskInterface()->AddTask([=]() {
            // --- INÍCIO DA CORREÇÃO ---
            // Em vez de limpar apenas a mão oposta, vamos limpar AMBAS as mãos
            // para garantir que o personagem esteja pronto para equipar uma arma de 2H,
            // mesmo que seja em um único slot.

            // 1. Desequipa o que estiver na mão direita.
            equipManager->UnequipObject(actor, nullptr, nullptr, 1, Hooks::g_rightHandSlot, true, false, false, true);
            // 2. Desequipa o que estiver na mão esquerda.
            equipManager->UnequipObject(actor, nullptr, nullptr, 1, Hooks::g_leftHandSlot, true, false, false, true);

            // --- FIM DA CORREÇÃO ---

            // Agora, com as mãos garantidamente vazias, equipamos nosso item no slot alvo.
            equipManager->EquipObject(actor, item, nullptr, 1, targetSlot, true, false, false, true);

            // Forçamos a atualização da UI
            RE::SendUIMessage::SendInventoryUpdateMessage(actor, nullptr);

            // Agendamos a restauração do slot original da arma
            SKSE::GetTaskInterface()->AddTask([=]() {
                if (auto form = RE::TESForm::LookupByID(item->GetFormID())) {
                    if (auto weapToRestore = form->As<RE::TESObjectWEAP>()) {
                        weapToRestore->SetEquipSlot(originalSlot);
                    }
                }
            });
        });
    } else {
        // Para armas de uma mão, a lógica padrão do jogo deve funcionar,
        // mas para garantir, podemos também limpar o slot alvo antes.
        SKSE::GetTaskInterface()->AddTask([=]() {
            // Limpa o slot alvo antes de equipar, garantindo a substituição.
            equipManager->UnequipObject(actor, nullptr, nullptr, 1, targetSlot, true, false, false, true);
            equipManager->EquipObject(actor, item, nullptr, 1, targetSlot, true, false, false, true);
            RE::SendUIMessage::SendInventoryUpdateMessage(actor, nullptr);
        });
    }
}

void CheckAndEquipDualTwoHandedForNPC(RE::Actor* npc) {
    // 1. Validações Iniciais
    if (!npc || npc->IsPlayer() || !Hooks::g_canDualWieldTwoHandedKeyword) {
        // Adicionamos um log aqui caso a keyword não tenha sido carregada, um erro comum.
        if (!Hooks::g_canDualWieldTwoHandedKeyword) {
            logger::warn("Check failed: The Dual Wield Keyword is not loaded!");
        }
        return;
    }

    // Log de início de verificação
    logger::info("-> Running checks for '{}'", npc->GetName());

    // A condição original era `!npc->IsInCombat()`. Vamos relaxar essa condição para
    // confiar no estado do evento, mas podemos deixar um log para ver o que a função retorna.
    logger::info("   - Checking IsInCombat(): {}", npc->IsInCombat());

    // Verificamos a keyword.
    if (!npc->HasKeyword(Hooks::g_canDualWieldTwoHandedKeyword)) {
        logger::info("   - Check FAILED: NPC does not have the required keyword.");
        return;
    }
    logger::info("   - Check PASSED: NPC has the keyword.");

    // 2. Prevenção: Já está equipado?
    auto leftWeaponForm = npc->GetEquippedObject(true);
    auto rightWeaponForm = npc->GetEquippedObject(false);
    std::string leftItemName = leftWeaponForm ? leftWeaponForm->GetName() : "None";
    std::string rightItemName = rightWeaponForm ? rightWeaponForm->GetName() : "None";
    RE::FormID leftFormID = leftWeaponForm ? leftWeaponForm->GetFormID() : 0;
    RE::FormID rightFormID = rightWeaponForm ? rightWeaponForm->GetFormID() : 0;

    logger::info("   - Equipment Check: Left Hand='{}' (ID: {:X}), Right Hand='{}' (ID: {:X})", leftItemName,
                 leftFormID, rightItemName, rightFormID);
    // --- FIM DOS LOGS DE DEPURAÇÃO ---

    // --- CONDIÇÃO CORRIGIDA ---
    if (leftWeaponForm && rightWeaponForm &&
        // AQUI ESTÁ A CORREÇÃO CRÍTICA: garante que os itens são diferentes
        leftWeaponForm->GetFormID() != rightWeaponForm->GetFormID() &&
        isTwoHanded(leftWeaponForm->As<RE::TESBoundObject>()) &&
        isTwoHanded(rightWeaponForm->As<RE::TESBoundObject>())) {
        logger::info("   - Check FAILED: NPC is already dual wielding two different two-handed weapons.");
        return;
    }
    logger::info("   - Check PASSED: NPC is not currently dual wielding two-handed weapons.");

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
        logger::info("   - Check PASSED: Found at least 2 weapons. Proceeding to equip.");

        auto weapon1 = suitableWeapons[0];
        auto weapon2 = suitableWeapons[1];

        logger::info("   -> Equipping '{}' in Right Hand and '{}' in Left Hand.", weapon1->GetName(),
                     weapon2->GetName());

        EquipItemWithGripChange(npc, weapon1, Hooks::g_rightHandSlot);
        EquipItemWithGripChange(npc, weapon2, Hooks::g_leftHandSlot);

        logger::info("   -> EQUIP SUCCEEDED for '{}'", npc->GetName());
    } else {
        logger::info("   - Check FAILED: Not enough suitable weapons in inventory.");
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

    switch (eventype) {
        case SkyPromptAPI::kAccepted:
            if(!except) {
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
            //StanceText = "Stances";
            //MovesetText = "Movesets";
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
    std::string categoryName = GetCurrentWeaponCategoryName();
    const auto& category = AnimationManager::GetSingleton()->GetCategories().at(categoryName);
    const int numStances = category.instances.size();

    switch (event.type) {
        case SkyPromptAPI::kAccepted:
            if (event.prompt.eventID == 2) {
                g_currentStance -= 1;
                if (g_currentStance < 1) {
                    g_currentStance = numStances;  // Vai para o último
                }
                UpdatePowerAttackGlobals();
                UpdateSkyPromptTexts();
                SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), MenuShowing);
                SkyPromptAPI::SendPrompt(StancesChangesSink::GetSingleton(), MenuShowing);
                SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);
                SkyPromptAPI::RemovePrompt(MovesetSink::GetSingleton(), g_clientID);
                break;
            }
            if (event.prompt.eventID == 3) {
                g_currentStance += 1;
                if (g_currentStance > numStances) {
                    g_currentStance = 1;  // Volta para o primeiro
                }
                UpdatePowerAttackGlobals();
                UpdateSkyPromptTexts();
                SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), MenuShowing);
                SkyPromptAPI::SendPrompt(StancesChangesSink::GetSingleton(), MenuShowing);
                SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);
                SkyPromptAPI::RemovePrompt(MovesetSink::GetSingleton(), g_clientID);
                break;
            }
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

    // REQUERIMENTO 5: Mostra a nova contagem (x/y) imediatamente
    GlobalControl::StanceChangesOpen = true;
    logger::info("O valor de MovesetText é: {}", MovesetText);
    g_currentMoveset = 1;
    RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("testarone", g_currentMoveset);
    RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("cycle_instance", g_currentStance);  
}

std::span<const SkyPromptAPI::Prompt> GlobalControl::MovesetSink::GetPrompts() const {
    return prompts; }

void GlobalControl::MovesetSink::ProcessEvent(SkyPromptAPI::PromptEvent event) const {
    auto eventype = event.type;
    if (!g_isWeaponDrawn) {
        return;
    }
    switch (eventype) {

        case SkyPromptAPI::kAccepted:
            if (!except) {
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

    // REQUERIMENTO 1, 2, 3: Pegar todas as informações necessárias
    std::string category = GetCurrentWeaponCategoryName();
    // O índice do cache é 0-3, mas a stance no jogo é 1-4.
    int stanceIndex = GlobalControl::g_currentStance - 1;
    int maxMovesets = AnimationManager::GetMaxMovesetsFor(category, stanceIndex);

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
                logger::info("teste {}", MovesetText);
                RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("testarone", g_currentMoveset);
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
                logger::info("teste {}", MovesetText);
                RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("testarone", g_currentMoveset);
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
        logger::info("Menu relevante '{}' mudou de estado. is_open agora é: {}", event->menuName.c_str(),
                     event->opening);
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

        const RE::FormID formID = actor->GetFormID();
        const std::string_view eventName = a_event->tag;

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
            case RE::ACTOR_COMBAT_STATE::kSearching:
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
            if (!actor->IsPlayerRef() && actor->IsInCombat()) {
                SKSE::log::info("[NpcCombatTracker] Ator '{}' ({:08X}) já está em combate. Registrando sink...",
                                actor->GetName(), actor->GetFormID());
                // Usamos a mesma função de registro que já existe!
                RegisterSink(actor);
                CheckAndEquipDualTwoHandedForNPC(actor);
            }
        }
    }
    SKSE::log::info("[NpcCombatTracker] Verificação concluída.");
}

void GlobalControl::UpdateSkyPromptTexts() {
    auto animManager = AnimationManager::GetSingleton();
    std::string categoryName = GetCurrentWeaponCategoryName();
    auto categoryIt = animManager->GetCategories().find(categoryName);
    const auto& category = categoryIt->second;
    const int numStances = category.stanceNames.size();
    // --- LÓGICA PARA STANCES  ---
    if (g_currentStance == 0) {
        StanceText = "Stances";
        StanceNextText = animManager->GetStanceName(categoryName, 0);
        StanceBackText = animManager->GetStanceName(categoryName, numStances - 1);
    } else {
        int currentStanceIndex = g_currentStance - 1;
        // CORREÇÃO: Lógica de ciclo dinâmica
        int nextStanceIndex = (currentStanceIndex + 1) >= numStances ? 0 : currentStanceIndex + 1;
        int backStanceIndex = (currentStanceIndex - 1) < 0 ? numStances - 1 : currentStanceIndex - 1;

        StanceText = animManager->GetStanceName(categoryName, currentStanceIndex);
        StanceNextText = animManager->GetStanceName(categoryName, nextStanceIndex);
        StanceBackText = animManager->GetStanceName(categoryName, backStanceIndex);
    }
    int validStanceIndexForMoveset = g_currentStance - 1;

    // --- LÓGICA PARA MOVESETS  ---
    int maxMovesets = animManager->GetMaxMovesetsFor(categoryName, validStanceIndexForMoveset);
    int currentMovesetIndex = g_currentMoveset;  // 1-N
    if (maxMovesets > 0) {
        int dirState = InputListener::GetDirectionalState();
        //SKSE::log::info("[UpdateSkyPromptTexts] Chamando GetCurrentMovesetName com dirState: {}", dirState);
        std::string currentMovesetName =
            animManager->GetCurrentMovesetName(categoryName, validStanceIndexForMoveset, currentMovesetIndex, dirState);
        MovesetText = std::format("{} ({}/{})", currentMovesetName, currentMovesetIndex, maxMovesets);

        if (maxMovesets > 1) {
            int nextMovesetIndex = (currentMovesetIndex % maxMovesets) + 1;
            int backMovesetIndex = (currentMovesetIndex - 2 + maxMovesets) % maxMovesets + 1;
            MovesetNextText =
                animManager->GetCurrentMovesetName(categoryName, validStanceIndexForMoveset, nextMovesetIndex, 0);
            MovesetBackText =
                animManager->GetCurrentMovesetName(categoryName, validStanceIndexForMoveset, backMovesetIndex, 0);
        } else {
            MovesetNextText = "Back";
            MovesetBackText = "Next";
        }
    } else {
        MovesetText = "Movesets";
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
        logger::info("[UpdatePromptVisibility] Condições atendidas. Mostrando prompts.");
        Cycleopen = true;
        // Talvez seja necessário atualizar os textos antes de enviar
        UpdateSkyPromptTexts();
        SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID);
        SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);

    } else if (!shouldBeVisible && Cycleopen) {
        // CONDIÇÃO: Não deveriam estar visíveis, mas estão -> ESCONDER
        logger::info("[UpdatePromptVisibility] Condições não atendidas. Escondendo prompts.");
        Cycleopen = false;
        SkyPromptAPI::RemovePrompt(StancesSink::GetSingleton(), g_clientID);
        SkyPromptAPI::RemovePrompt(MovesetSink::GetSingleton(), g_clientID);
        SkyPromptAPI::RemovePrompt(StancesChangesSink::GetSingleton(), MenuShowing);
        SkyPromptAPI::RemovePrompt(MovesetChangesSink::GetSingleton(), MenuShowing);
    }
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
    SkyPromptAPI::SendPrompt(this, GlobalControl::Dynamicgrip);  // Use seu ClientID aqui
}

void GlobalControl::EquipMenu::Hide() const {
    // Simplesmente remove os prompts
    SkyPromptAPI::RemovePrompt(this, GlobalControl::Dynamicgrip);  // Use seu ClientID aqui
}

RE::BSEventNotifyControl GlobalControl::EquipEventSink::ProcessEvent(const RE::TESEquipEvent* a_event,
                                                                     RE::BSTEventSource<RE::TESEquipEvent>*) {
    // Só nos importamos com eventos do jogador e quando ele está EQUIPANDO algo
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!a_event || a_event->actor.get() != player || !a_event->equipped) {
        return RE::BSEventNotifyControl::kContinue;
    }


    // Acessa o inventário do jogador para ver o que está realmente equipado
    if (auto invChanges = player->GetInventoryChanges()) {
        if (auto entryList = invChanges->entryList) {
            // Itera sobre todos os itens equipados
            for (auto& entry : *entryList) {
    #ifdef GetObject
    #undef GetObject
    #endif
                if (entry && entry->GetObject() && entry->IsWorn()) {
                    auto item = entry->GetObject();

                    // Se encontrarmos uma arma de 2 mãos que está com o slot ERRADO...
                    if (isTwoHanded(item)) {  // 'isTwoHanded' é a função que já criamos
                        auto weapon = item->As<RE::TESObjectWEAP>();
                        if (weapon->GetEquipSlot() != Hooks::g_twoHandSlot) {
                            // ...nós a CORRIGIMOS imediatamente!
                            logger::warn("Arma de duas mãos '{}' encontrada com slot incorreto. Corrigindo.",
                                         weapon->GetName());
                            weapon->SetEquipSlot(Hooks::g_twoHandSlot);
                        }
                    }
                }
            }
        }
    }

    return RE::BSEventNotifyControl::kContinue;
}
