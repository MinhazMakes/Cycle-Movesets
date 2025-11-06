#include "Serialization.h"
#include "Utils.h"
#include "Events.h"
#include <random>
#include <vector>
#include <algorithm> // Para std::max_element
#include "Settings.h"

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

bool isOneHanded(RE::TESForm* a_weap) {
    if (!a_weap || !a_weap->IsWeapon()) return false;
    auto weap = a_weap->As<RE::TESObjectWEAP>();
    // Verifica explicitamente os tipos de 1H
    if (weap->IsOneHandedSword() || weap->IsOneHandedDagger() || weap->IsOneHandedAxe() || weap->IsOneHandedMace()) {
        return true;
    }
    return false;
}

bool isShield(RE::TESForm* a_item) {
    if (!a_item || !a_item->IsArmor()) return false;
    auto armor = a_item->As<RE::TESObjectARMO>();
    return armor->IsShield();
}

bool NCheckActorHasPerks(RE::Actor* actor, const std::vector<PerkDef>& perks) {
    if (!actor) return false;        // Segurança
    if (perks.empty()) return true;  // Se não há perks, está disponível

    for (const auto& perkDef : perks) {
        auto* perkForm = RE::TESForm::LookupByID<RE::BGSPerk>(perkDef.formID);
        if (!perkForm || !actor->HasPerk(perkForm)) {
            // O ator não tem um dos perks necessários
            return false;
        }
    }
    // O ator tem todos os perks
    return true;
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

    auto equipManager = RE::ActorEquipManager::GetSingleton();
    if (!equipManager) {
        return;
    }

    logger::info("--- [CheckAndEquipHandle] Iniciando verificação para '{}' ({:08X}) ---", npc->GetName(),
                 npc->GetFormID());

    // 1. Obter o status atual do equipamento
    auto equippedItemL = npc->GetEquippedObjectInSlot(Hooks::g_leftHandSlot);
    auto equippedItemR = npc->GetEquippedObjectInSlot(Hooks::g_rightHandSlot);
    auto equippedItem2H = npc->GetEquippedObjectInSlot(Hooks::g_twoHandSlot);

    bool isR_2H = isTwoHanded(equippedItemR);
    bool isL_2H = isTwoHanded(equippedItemL);
    bool is2H_2H = isTwoHanded(equippedItem2H);

    // 2. Verificações de saída antecipada
    if (isL_2H && isR_2H) {
        logger::info("  - Status: NPC já está empunhando duas armas de duas mãos (Dual 2H).");
        logger::info("--- [CheckAndEquipHandle] Fim da verificação ---");
        return;  // Já está em dual wielding 2H
    }

    if (isR_2H && equippedItemL != nullptr) {
        std::string leftItemName = "Item Desconhecido";
        if (equippedItemL) {
            leftItemName = equippedItemL->GetName();
        }
        logger::info("  - Status: NPC já está empunhando 2H na direita e '{}' na esquerda. Não interferir.",
                     leftItemName);
        logger::info("--- [CheckAndEquipHandle] Fim da verificação ---");
        return;
    }
    // 3. Verificar Perks
    logger::info("  - Verificando Perks...");
    bool hasBase2HPerks = NCheckActorHasPerks(npc, handle::npc2HConfig.requiredPerks);
    bool hasDual2HPerks = NCheckActorHasPerks(npc, handle::npc2HConfig.requiredPerksDual2H);
    if (is2H_2H) {
        if (hasBase2HPerks || hasDual2HPerks) {
        } else {
            logger::info("  - Status: NPC está empunhando uma arma de duas mãos (slot 2H). Não interferir.");
            logger::info("--- [CheckAndEquipHandle] Fim da verificação ---");
            return;  // Equipamento 2H padrão, não interferir
        }
    }
    // Se o NPC não tem nem os perks base, não fazemos nada.
    if (!hasBase2HPerks) {
        logger::info("  - Falha na Verificação: NPC não possui os perks base '2H Handle'.");
        logger::info("--- [CheckAndEquipHandle] Fim da verificação ---");
        return;
    }

    logger::info("  - Status Perks: Base 2H Handle: {}. Dual 2H: {}.", hasBase2HPerks, hasDual2HPerks);

    // 4. Escanear inventário para itens NÃO EQUIPADOS
    logger::info("  - Escaneando inventário...");
    std::vector<RE::TESObjectWEAP*> twoHandedWeapons;
    std::vector<RE::TESObjectWEAP*> oneHandedWeapons;
    std::vector<RE::TESObjectARMO*> shields;
    std::vector<RE::SpellItem*> spells;

    RE::TESObjectWEAP* r_weap = equippedItemR ? equippedItemR->As<RE::TESObjectWEAP>() : nullptr;
    RE::TESObjectWEAP* l_weap = equippedItemL ? equippedItemL->As<RE::TESObjectWEAP>() : nullptr;
    RE::TESObjectARMO* l_armo = equippedItemL ? equippedItemL->As<RE::TESObjectARMO>() : nullptr;

    auto inventory = npc->GetInventory();
    for (const auto& [item, data] : inventory) {
        int count = data.first;
        if (count <= 0) continue;

        if (isTwoHanded(item)) {
            auto weap = item->As<RE::TESObjectWEAP>();
            if (weap == r_weap) count--;                                       // Um já está equipado R
            if (weap == l_weap) count--;                                       // Um já está equipado L
            for (int i = 0; i < count; i++) twoHandedWeapons.push_back(weap);  // Adiciona cópias restantes
        } else if (isOneHanded(item)) {
            auto weap = item->As<RE::TESObjectWEAP>();
            if (weap == r_weap) count--;
            if (weap == l_weap) count--;
            for (int i = 0; i < count; i++) oneHandedWeapons.push_back(weap);
        } else if (isShield(item)) {
            auto armo = item->As<RE::TESObjectARMO>();
            if (armo == l_armo) count--;
            for (int i = 0; i < count; i++) shields.push_back(armo);
        }
    }


    logger::info("   - Inventário (disponível): {} 2H, {} 1H, {} Escudos, {} Magias.", twoHandedWeapons.size(),
                 oneHandedWeapons.size(), shields.size(), spells.size());

    // Função auxiliar para equipar o melhor item de fallback na mão esquerda
    auto equipLeftHandFallback = [&](RE::Actor* actor) {
        if (!oneHandedWeapons.empty()) {
            logger::info("  -> Equipando '{}' na Mão Esquerda (Fallback 1H).", oneHandedWeapons[0]->GetName());
            EquipItemWithGripChange(actor, oneHandedWeapons[0], Hooks::g_leftHandSlot);
        } else if (!shields.empty()) {
            logger::info("  -> Equipando '{}' na Mão Esquerda (Fallback Escudo).", shields[0]->GetName());
            EquipItemWithGripChange(actor, shields[0], Hooks::g_leftHandSlot);
        } else {
            logger::info("  -> Nenhuma opção de fallback para a Mão Esquerda. Deixando vazia.");
        }
    };

    // 5. Árvore de Decisão Principal
    if (isR_2H) {
        // Caso A: NPC já tem uma 2H na mão direita. Só precisamos gerenciar a mão esquerda.
        logger::info("  - Ação: NPC já tem 2H na mão direita. Gerenciando mão esquerda...");
        if (hasDual2HPerks) {
            if (!twoHandedWeapons.empty()) {
                // Tenta equipar uma segunda arma 2H
                logger::info("  -> Equipando '{}' na Mão Esquerda (Dual 2H).", twoHandedWeapons[0]->GetName());
                EquipItemWithGripChange(npc, twoHandedWeapons[0], Hooks::g_leftHandSlot);
            } else {
                // Fallback para Dual 2H (não há outra 2H)
                logger::info("  - Ação: Perks 'Dual 2H' presentes, mas sem segunda arma 2H. Usando fallback...");
                equipLeftHandFallback(npc);
            }
        } else {
            // Tem perks base, mas não dual.
            logger::info("  - Ação: Perks 'Base 2H' presentes. Usando fallback para mão esquerda...");
            equipLeftHandFallback(npc);
        }
    } else {
        // Caso B: NPC NÃO tem uma 2H na mão direita, mas deveria.
        logger::info("  - Ação: NPC não tem 2H na mão direita. Tentando equipar...");

        // Encontra a primeira 2H disponível no inventário
        RE::TESObjectWEAP* rightWeaponToEquip = nullptr;
        if (!twoHandedWeapons.empty()) {
            rightWeaponToEquip = twoHandedWeapons[0];
        } else if (isTwoHanded(r_weap)) {
            // Caso especial: a arma 2H já estava equipada, mas não no slot 'RightHand'
            // (Isso não deveria acontecer por causa do check 'is2H_2H', mas é uma segurança)
            rightWeaponToEquip = r_weap;
        }

        if (!rightWeaponToEquip) {
            logger::warn("  - Falha na Ação: NPC tem perks '2H Handle' mas não há armas 2H no inventário.");
            logger::info("--- [CheckAndEquipHandle] Fim da verificação ---");
            return;
        }

        // Equipa a arma 2H na mão direita
        logger::info("  -> Equipando '{}' na Mão Direita.", rightWeaponToEquip->GetName());
        EquipItemWithGripChange(npc, rightWeaponToEquip, Hooks::g_rightHandSlot);

        // Agora, gerencia a mão esquerda com base nos perks
        if (hasDual2HPerks) {
            RE::TESObjectWEAP* leftWeaponToEquip = nullptr;
            // Tenta encontrar uma *segunda* arma 2H
            if (twoHandedWeapons.size() > 1) {
                // Se a arma que equipamos na direita veio do inventário (não estava equipada),
                // twoHandedWeapons[1] é uma segunda arma válida.
                if (rightWeaponToEquip == twoHandedWeapons[0]) {
                    leftWeaponToEquip = twoHandedWeapons[1];
                }
            } else if (twoHandedWeapons.size() == 1 && rightWeaponToEquip != twoHandedWeapons[0]) {
                // Se equipamos uma arma que já estava equipada (r_weap), a arma em twoHandedWeapons[0]
                // é uma segunda arma válida.
                leftWeaponToEquip = twoHandedWeapons[0];
            }

            if (leftWeaponToEquip) {
                // Encontrou uma segunda arma 2H
                logger::info("  -> Equipando '{}' na Mão Esquerda (Dual 2H).", leftWeaponToEquip->GetName());
                EquipItemWithGripChange(npc, leftWeaponToEquip, Hooks::g_leftHandSlot);
            } else {
                // Fallback para Dual 2H
                logger::info("  - Ação: Perks 'Dual 2H' presentes, mas sem segunda arma 2H. Usando fallback...");
                equipLeftHandFallback(npc);
            }
        } else {
            // Tem perks base, mas não dual.
            logger::info("  - Ação: Perks 'Base 2H' presentes. Usando fallback para mão esquerda...");
            equipLeftHandFallback(npc);
        }
    }

    logger::info("--- [CheckAndEquipHandle] Fim da verificação ---");
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
        int previousDirectionalState = directionalState;
        // Apenas recalcule a direção se uma das nossas teclas de movimento REALMENTE mudou de estado.
        if (umaTeclaDeMovimentoMudou) {
            UpdateDirectionalState();
        }

        if (directionalState != previousDirectionalState && g_isWeaponDrawn) {
            // Chama a nova função para atualizar os efeitos
            UpdateEffectsForDirectionalChange(previousDirectionalState, directionalState);
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

    // 1. Obter os objetos DIRETAMENTE DOS SLOTS (como sugerido)
    auto itemR = targetActor->GetEquippedObjectInSlot(Hooks::g_rightHandSlot);
    auto itemL = targetActor->GetEquippedObjectInSlot(Hooks::g_leftHandSlot);
    auto item2H = targetActor->GetEquippedObjectInSlot(Hooks::g_twoHandSlot);

    RE::TESObjectWEAP* rightWeapon = nullptr;
    RE::TESObjectWEAP* leftWeapon = nullptr;
    RE::TESObjectARMO* leftArmor = nullptr;  // Para escudos

    // 2. Lógica de prioridade para determinar o que está equipado
    if (item2H && item2H->IsWeapon()) {
        // Caso 1: Arma de Duas Mãos Padrão (ocupa o slot 2H)
        rightWeapon = item2H->As<RE::TESObjectWEAP>();
        leftWeapon = nullptr;  // Slot 2H ocupa ambas as mãos
    } else {
        // Caso 2: Dual Wield (1H ou 2H), 1H + Escudo, ou 1H + Vazio
        if (itemR && itemR->IsWeapon()) {
            rightWeapon = itemR->As<RE::TESObjectWEAP>();
        }
        if (itemL) {  // Slot da mão esquerda está ocupado
            if (itemL->IsWeapon()) {
                leftWeapon = itemL->As<RE::TESObjectWEAP>();
            } else if (itemL->IsArmor()) {
                leftArmor = itemL->As<RE::TESObjectARMO>();
            }
        }
    }

    // 3. Lógica de checagem de estado (igual à original)
    if (!rightWeapon && !leftWeapon && (!leftArmor || !leftArmor->IsShield())) {
        return "Unarmed";
    }

    // 4. Determinar os tipos (igual à original)
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
        case SkyPromptAPI::kDeclined: {  // Adiciona escopo
            SKSE::log::info("[StanceSink Decline] Pressionado. Verificando Settings::CycleMoveset (g_StyleFirst)...");

            // Assume-se que Settings::CycleMoveset é a variável para g_StyleFirst
            if (!Settings::CycleMoveset) {
                // g_StyleFirst é FALSE: Reseta tudo para 0 e limpa efeitos
                SKSE::log::info("[StanceSink Decline] Resetando stance e moveset para 0.");
                g_currentStance = 0;
                g_currentMoveset = 0;

                // Aplica 0 nas variáveis do jogo
                RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("cycle_instance", 0);
                RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("testarone", 0);

                // Limpa TODOS os efeitos
                SKSE::log::info("[StanceSink Decline] Limpando todos os efeitos de Stance e Moveset.");
                ApplyAndTrackEffects(player, {}, g_lastAppliedStanceEffects);
                ApplyAndTrackEffects(player, {}, g_lastAppliedMovesetEffects);

            } else {
                // g_StyleFirst é TRUE: Vai para a primeira stance/moveset disponível
                SKSE::log::info("[StanceSink Decline] Indo para a primeira Stance/Moveset disponível.");
                const auto availableStances = animManager->GetAvailableStances(player, categoryName);
                const int numStances = availableStances.size();

                if (numStances > 0) {
                    g_currentStance = 1;  // Seleciona a primeira stance da lista
                    int originalStanceIndexToApply = availableStances[0].originalIndex;

                    // 1. Aplica a primeira stance no jogo
                    RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("cycle_instance",
                                                                             originalStanceIndexToApply);
                    SKSE::log::info("[StanceSink Decline] Setando Stance (Original Index): {}",
                                    originalStanceIndexToApply);

                    // 2. Reseta o moveset e aplica o primeiro disponível da PRIMEIRA stance
                    g_currentMoveset = 0;
                    int originalMovesetIndexToApply = 0;
                    const ModInstance* firstModInst = nullptr;
                    const SubAnimationInstance* firstSubInst = nullptr;
                    std::vector<AppliedEffect> firstMovesetCombinedEffects;

                    const auto availableMovesets =
                        animManager->GetAvailableMovesets(player, categoryName, originalStanceIndexToApply);
                    if (!availableMovesets.empty()) {
                        g_currentMoveset = 1;
                        originalMovesetIndexToApply = availableMovesets[0].originalIndex;

                        // Coleta efeitos do primeiro moveset (mesma lógica de StancesChangesSink)
                        auto cat_it = animManager->GetCategories().find(categoryName);
                        if (cat_it != animManager->GetCategories().end()) {
                            const WeaponCategory& category = cat_it->second;
                            if (originalStanceIndexToApply > 0 &&
                                originalStanceIndexToApply <= category.instances.size()) {
                                const auto& stanceInstance = category.instances[originalStanceIndexToApply - 1];
                                firstMovesetCombinedEffects.insert(firstMovesetCombinedEffects.end(),
                                                                   stanceInstance.appliedEffects.begin(),
                                                                   stanceInstance.appliedEffects.end());
                                
                                int parentCounter = 0;
                                auto& mutableStanceInstance = const_cast<CategoryInstance&>(stanceInstance);
                                for (const auto& modInst : mutableStanceInstance.modInstances) { 
                                    if (!modInst.isSelected) continue;
                                    for (const auto& subInst : modInst.subAnimationInstances) {
                                        bool isParent =
                                            !(subInst.pFront || subInst.pBack || subInst.pLeft || subInst.pRight ||
                                              subInst.pFrontRight || subInst.pFrontLeft || subInst.pBackRight ||
                                              subInst.pBackLeft || subInst.pRandom || subInst.pDodge);
                                        if (!subInst.isSelected || !isParent) continue;
                                        parentCounter++;
                                        if (parentCounter == originalMovesetIndexToApply) {
                                            firstModInst = &modInst;
                                            firstSubInst = &subInst;
                                            goto found_first_moveset_decline_stance_sink;
                                        }
                                    }
                                }
                            found_first_moveset_decline_stance_sink:;
                                if (firstModInst)
                                    firstMovesetCombinedEffects.insert(firstMovesetCombinedEffects.end(),
                                                                       firstModInst->appliedEffects.begin(),
                                                                       firstModInst->appliedEffects.end());
                                if (firstSubInst)
                                    firstMovesetCombinedEffects.insert(firstMovesetCombinedEffects.end(),
                                                                       firstSubInst->appliedEffects.begin(),
                                                                       firstSubInst->appliedEffects.end());
                                std::sort(firstMovesetCombinedEffects.begin(), firstMovesetCombinedEffects.end());
                                firstMovesetCombinedEffects.erase(
                                    std::unique(firstMovesetCombinedEffects.begin(), firstMovesetCombinedEffects.end()),
                                    firstMovesetCombinedEffects.end());
                            }
                        }
                    }  // Fim if (!availableMovesets.empty())

                    RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("testarone", originalMovesetIndexToApply);
                    SKSE::log::info("[StanceSink Decline] Setando Moveset (Original Index): {}",
                                    originalMovesetIndexToApply);

                    // 3. Coleta efeitos da PRIMEIRA stance
                    std::vector<AppliedEffect> firstStanceEffects;
                    auto cat_it = animManager->GetCategories().find(categoryName);
                    if (cat_it != animManager->GetCategories().end()) {
                        const WeaponCategory& category = cat_it->second;
                        if (originalStanceIndexToApply > 0 && originalStanceIndexToApply <= category.instances.size()) {
                            firstStanceEffects = category.instances[originalStanceIndexToApply - 1].appliedEffects;
                        }
                    }

                    // 4. Aplica/Remove efeitos para refletir o estado da PRIMEIRA stance e PRIMEIRO moveset
                    SKSE::log::info("[StanceSink Decline] Aplicando {} efeitos da primeira Stance",
                                    firstStanceEffects.size());
                    ApplyAndTrackEffects(player, firstStanceEffects, g_lastAppliedStanceEffects);
                    SKSE::log::info("[StanceSink Decline] Aplicando {} efeitos combinados do primeiro Moveset",
                                    firstMovesetCombinedEffects.size());
                    ApplyAndTrackEffects(player, firstMovesetCombinedEffects, g_lastAppliedMovesetEffects);

                } else {
                    // Não há stances disponíveis, reseta tudo como no caso 'false'
                    SKSE::log::info("[StanceSink Decline] Nenhuma stance disponível, resetando tudo.");
                    g_currentStance = 0;
                    g_currentMoveset = 0;
                    RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("cycle_instance", 0);
                    RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("testarone", 0);
                    ApplyAndTrackEffects(player, {}, g_lastAppliedStanceEffects);
                    ApplyAndTrackEffects(player, {}, g_lastAppliedMovesetEffects);
                }
            }

            // Atualiza UI e Reenvia Prompts Principais
            UpdatePowerAttackGlobals();
            UpdateSkyPromptTexts();
            SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID);
            SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);
        }  // Fim do escopo
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

        SKSE::log::info("[Stance Change/Reset] Aplicando {} efeitos combinados do Moveset Padrão (Lista Index {})",
                        firstMovesetCombinedEffects.size(), g_currentMoveset);
        //ApplyAndTrackEffects(player, newStanceEffects, g_lastAppliedStanceEffects);
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
        case SkyPromptAPI::kDeclined: {  // Adiciona escopo para variáveis locais
            SKSE::log::info("[MovesetSink Decline] Resetando para o primeiro moveset.");
            const auto availableMovesets = animManager->GetAvailableMovesets(player, categoryName, originalStanceIndex);
            const int maxMovesets = availableMovesets.size();

            // Define o moveset para o primeiro disponível (se houver)
            g_currentMoveset = (maxMovesets > 0) ? 1 : 0;
            int originalMovesetIndexToApply = 0;
            if (g_currentMoveset > 0) {
                originalMovesetIndexToApply = availableMovesets[0].originalIndex;  // Pega o índice real do primeiro
            }

            // 1. Aplica o primeiro moveset (ou 0) no jogo
            RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("testarone", originalMovesetIndexToApply);
            SKSE::log::info("[MovesetSink Decline] Setando Moveset (Original Index): {}", originalMovesetIndexToApply);

            // 2. Coleta TODOS os efeitos aplicáveis para o PRIMEIRO moveset (Stance + Moveset 1 + SubMoveset 1)
            std::vector<AppliedEffect> firstMovesetCombinedEffects;
            auto cat_it = animManager->GetCategories().find(categoryName);
            if (cat_it != animManager->GetCategories().end()) {
                const WeaponCategory& category = cat_it->second;
                // Garante que temos uma stance válida para buscar efeitos
                if (originalStanceIndex > 0 && originalStanceIndex <= category.instances.size()) {
                    const auto& stanceInstance = category.instances[originalStanceIndex - 1];
                    // Efeitos da Stance (sempre incluídos)
                    firstMovesetCombinedEffects.insert(firstMovesetCombinedEffects.end(),
                                                       stanceInstance.appliedEffects.begin(),
                                                       stanceInstance.appliedEffects.end());

                    // Encontra o ModInstance e SubAnimationInstance correspondentes ao PRIMEIRO moveset
                    // (originalMovesetIndexToApply)
                    if (originalMovesetIndexToApply > 0) {
                        int parentCounter = 0;
                        const ModInstance* firstModInst = nullptr;
                        const SubAnimationInstance* firstSubInst = nullptr;
                        auto& mutableStanceInstance = const_cast<CategoryInstance&>(stanceInstance);

                        for (const auto& modInst : mutableStanceInstance.modInstances) {
                            if (!modInst.isSelected) continue;
                            for (const auto& subInst : modInst.subAnimationInstances) {
                                bool isParent =
                                    !(subInst.pFront || subInst.pBack || subInst.pLeft ||
                                      subInst.pRight || subInst.pFrontRight || subInst.pFrontLeft ||
                                      subInst.pBackRight || subInst.pBackLeft || subInst.pRandom || subInst.pDodge);
                                if (!subInst.isSelected || !isParent) continue;
                                parentCounter++;
                                if (parentCounter == originalMovesetIndexToApply) {  // Achou o primeiro moveset
                                    firstModInst = &modInst;
                                    firstSubInst = &subInst;
                                    goto found_first_moveset_decline_sink;  // Label única
                                }
                            }
                        }
                    found_first_moveset_decline_sink:;
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
                    }
                }
            }
            // Remove duplicatas
            std::sort(firstMovesetCombinedEffects.begin(), firstMovesetCombinedEffects.end());
            firstMovesetCombinedEffects.erase(
                std::unique(firstMovesetCombinedEffects.begin(), firstMovesetCombinedEffects.end()),
                firstMovesetCombinedEffects.end());

            // 3. Aplica/Remove efeitos para refletir o estado do PRIMEIRO moveset
            SKSE::log::info("[MovesetSink Decline] Aplicando {} efeitos combinados do primeiro Moveset",
                            firstMovesetCombinedEffects.size());
            ApplyAndTrackEffects(player, firstMovesetCombinedEffects, g_lastAppliedMovesetEffects);

            // 4. Atualiza UI e Reenvia Prompt
            UpdatePowerAttackGlobals();
            UpdateSkyPromptTexts();
            SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);  // Reenvia o prompt principal
        }  // Fim do escopo
        break;
    }
}

std::span<const SkyPromptAPI::Prompt> GlobalControl::MovesetChangesSink::GetPrompts() const { 
    return prompts; }
    

void GlobalControl::MovesetChangesSink::ProcessEvent(SkyPromptAPI::PromptEvent event) const {
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;  // Sai se não houver jogador
    std::string categoryName = GetCurrentWeaponCategoryName();
    auto animManager = AnimationManager::GetSingleton();

    // Encontra o índice ORIGINAL da stance atual
    const auto availableStances = animManager->GetAvailableStances(player, categoryName);
    int stanceOriginalIndex = 0;  // 0 = Nenhum
    if (g_currentStance > 0 && g_currentStance <= availableStances.size()) {
        stanceOriginalIndex = availableStances[g_currentStance - 1].originalIndex;
    } else if (g_currentStance == 0 && !availableStances.empty()) {  // Se stance é 0, usa a primeira disponível
        // Isso pode acontecer se o jogador sacar a arma sem ter selecionado uma stance antes
        stanceOriginalIndex = availableStances[0].originalIndex;
        g_currentStance = 1;  // Define a stance atual como a primeira da lista
                              // Como a stance mudou implicitamente, precisamos aplicar seus efeitos aqui também
        std::vector<AppliedEffect> firstStanceEffects;
        auto cat_it_implicit = animManager->GetCategories().find(categoryName);
        if (cat_it_implicit != animManager->GetCategories().end()) {
            const WeaponCategory& category_implicit = cat_it_implicit->second;
            if (stanceOriginalIndex > 0 && stanceOriginalIndex <= category_implicit.instances.size()) {
                firstStanceEffects = category_implicit.instances[stanceOriginalIndex - 1].appliedEffects;
            }
        }
        SKSE::log::info("[Moveset Sink] Stance implícita definida para {}. Aplicando {} efeitos.", g_currentStance,
                        firstStanceEffects.size());
        ApplyAndTrackEffects(player, firstStanceEffects, g_lastAppliedStanceEffects);
        ApplyAndTrackEffects(player, {}, g_lastAppliedMovesetEffects);  // Limpa moveset antigo
    }

    // Pega os movesets disponíveis PARA A STANCE ATUAL (agora garantida > 0 se houver stances)
    const auto availableMovesets = animManager->GetAvailableMovesets(player, categoryName, stanceOriginalIndex);
    const int maxMovesets = availableMovesets.size();

    bool movesetChanged = false;  // Flag
    // Guarda o índice original do moveset ANTES de qualquer mudança
    int originalMovesetIndexBeforeChange = 0;
    if (g_currentMoveset > 0 && g_currentMoveset <= maxMovesets) {
        originalMovesetIndexBeforeChange = availableMovesets[g_currentMoveset - 1].originalIndex;
    }

    switch (event.type) {
        case SkyPromptAPI::kAccepted:
            if (event.prompt.eventID == 2 || event.prompt.eventID == 3) {  // Next ou Back
                if (maxMovesets > 0) {                                     // Só muda se houver movesets
                    int oldMovesetValue = g_currentMoveset;
                    if (event.prompt.eventID == 2) {  // Back
                        g_currentMoveset = (g_currentMoveset - 1);
                        if (g_currentMoveset < 1) g_currentMoveset = maxMovesets;
                    } else {  // Next
                        g_currentMoveset = (g_currentMoveset % maxMovesets) + 1;
                    }
                    if (g_currentMoveset != oldMovesetValue) {
                        movesetChanged = true;  // Define a flag AQUI
                    }
                }
            }
            break;  // Sai do case kAccepted

        case SkyPromptAPI::kTimeout:
            if (maxMovesets > 0) {  // Só reenvia se houver movesets
                SkyPromptAPI::SendPrompt(MovesetChangesSink::GetSingleton(), MenuShowing);
            } else {  // Se não há movesets, fecha o menu
                GlobalControl::MovesetChangesOpen = false;
                SkyPromptAPI::RemovePrompt(MovesetChangesSink::GetSingleton(), MenuShowing);
                // Reenvia os principais
                if (SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID)) {
                }
                if (SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID)) {
                }
            }
            return;  // Retorna cedo

        case SkyPromptAPI::kUp:
            if (event.prompt.eventID == 1) {  // Se soltar o botão que abriu este menu
                GlobalControl::MovesetChangesOpen = false;
                SkyPromptAPI::RemovePrompt(MovesetChangesSink::GetSingleton(), MenuShowing);
                // Reenvia os prompts principais
                if (SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID)) {
                }
                if (SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID)) {
                }
            }
            return;  // Retorna cedo
    }  // Fim do switch(event.type)

    // --- LÓGICA DE APLICAR EFEITOS E ATUALIZAR JOGO/UI (CONTROLADA PELA FLAG) ---
    if (movesetChanged) {
        int currentListIndex = (g_currentMoveset > 0) ? g_currentMoveset - 1 : -1;
        // Se g_currentMoveset for 0 (reset ou stance sem movesets), originalMovesetIndexToApply será 0.
        int originalMovesetIndexToApply =
            (currentListIndex != -1) ? availableMovesets[currentListIndex].originalIndex : 0;

        SKSE::log::info("[Moveset Change/Reset] Mudando para Moveset (Original Index): {}",
                        originalMovesetIndexToApply);

        // 1. Aplica o novo moveset no jogo (ou 0 se resetado/vazio)
        RE::PlayerCharacter::GetSingleton()->SetGraphVariableInt("testarone", originalMovesetIndexToApply);

        // 2. Coleta TODOS os efeitos aplicáveis (Stance + Moveset + SubMoveset "Pai")
        std::vector<AppliedEffect> combinedEffects;
        auto cat_it = animManager->GetCategories().find(categoryName);
        if (cat_it != animManager->GetCategories().end()) {
            const WeaponCategory& category = cat_it->second;  // Usa const reference
            // Garante que temos uma stance válida para buscar efeitos
            if (stanceOriginalIndex > 0 && stanceOriginalIndex <= category.instances.size()) {
                const auto& stanceInstance = category.instances[stanceOriginalIndex - 1];
                // Efeitos da Stance (sempre incluídos)
                combinedEffects.insert(combinedEffects.end(), stanceInstance.appliedEffects.begin(),
                                       stanceInstance.appliedEffects.end());

                // Encontra o ModInstance e SubAnimationInstance correspondentes ao NOVO originalMovesetIndexToApply
                if (originalMovesetIndexToApply > 0) {  // Só busca se houver um moveset para aplicar
                    int parentCounter = 0;
                    const ModInstance* currentModInst = nullptr;
                    const SubAnimationInstance* currentSubInst = nullptr;
                    auto& mutableStanceInstance = const_cast<CategoryInstance&>(stanceInstance);

                    for (const auto& modInst : mutableStanceInstance.modInstances) {
                        if (!modInst.isSelected) continue;
                        for (const auto& subInst : modInst.subAnimationInstances) {
                            bool isParent =
                                !(subInst.pFront || subInst.pBack || subInst.pLeft || subInst.pRight ||
                                  subInst.pFrontRight || subInst.pFrontLeft || subInst.pBackRight ||
                                  subInst.pBackLeft || subInst.pRandom || subInst.pDodge);  // Mesma lógica isParent
                            if (!subInst.isSelected || !isParent) continue;
                            parentCounter++;
                            if (parentCounter == originalMovesetIndexToApply) {  // Achou o moveset alvo
                                currentModInst = &modInst;
                                currentSubInst = &subInst;
                                goto found_instances_moveset_apply;  // Label única
                            }
                        }
                    }
                found_instances_moveset_apply:;  // Fim da busca
                    if (currentModInst) {
                        combinedEffects.insert(combinedEffects.end(), currentModInst->appliedEffects.begin(),
                                               currentModInst->appliedEffects.end());
                    }
                    if (currentSubInst) {
                        combinedEffects.insert(combinedEffects.end(), currentSubInst->appliedEffects.begin(),
                                               currentSubInst->appliedEffects.end());
                    }
                }  // Fim if (originalMovesetIndexToApply > 0)
            }  // Fim if (stanceOriginalIndex > 0 ...)
        }
        // Remove duplicatas da lista combinada (importante!)
        std::sort(combinedEffects.begin(), combinedEffects.end());
        combinedEffects.erase(std::unique(combinedEffects.begin(), combinedEffects.end()), combinedEffects.end());

        // 3. Aplica/Remove efeitos do Moveset (combinados com os da stance)
        // ApplyAndTrackEffects compara 'combinedEffects' com 'g_lastAppliedMovesetEffects'
        SKSE::log::info("[Moveset Change/Reset] Aplicando {} efeitos combinados para Moveset (Lista Index {})",
                        combinedEffects.size(), g_currentMoveset);
        ApplyAndTrackEffects(player, combinedEffects, g_lastAppliedMovesetEffects);

        // 4. Atualiza outras lógicas e a UI *depois* de aplicar efeitos
        UpdatePowerAttackGlobals();
        UpdateSkyPromptTexts();

        // 5. Reenvia os prompts do menu ATUAL (MovesetChangesSink) ou os principais
        if (GlobalControl::MovesetChangesOpen && event.type != SkyPromptAPI::kDeclined) {
            SkyPromptAPI::SendPrompt(MovesetChangesSink::GetSingleton(), MenuShowing);
        } else {  // Se foi um kDeclined ou o menu foi fechado
            SkyPromptAPI::SendPrompt(MovesetSink::GetSingleton(), g_clientID);
            SkyPromptAPI::SendPrompt(StancesSink::GetSingleton(), g_clientID);
        }

    } else if (event.type == SkyPromptAPI::kAccepted && maxMovesets <= 1) {
        // Caso especial: Apertou next/back mas só tinha 0 ou 1 moveset. Reseta o timer.
        SkyPromptAPI::SendPrompt(MovesetChangesSink::GetSingleton(), MenuShowing);
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
            case AppliedEffect::EffectType::Spell: {
                RE::SpellItem* spellToRemove = form->As<RE::SpellItem>();
                RE::EffectSetting* mgefToRemove = form->As<RE::EffectSetting>();

                // Tenta Dispel
                // GetActiveEffectList retorna BSSimpleList<ActiveEffect*>*
                if (auto activeEffectList = actor->AsMagicTarget()->GetActiveEffectList()) {
                    SKSE::log::debug("Iterando lista de efeitos ativos para Dispel (Forward)...");

                    // Usa um iterador C++ padrão para BSSimpleList
                    auto it = activeEffectList->begin();
                    while (it != activeEffectList->end()) {
                        RE::ActiveEffect* activeEffect = *it;
                        // Avança o iterador ANTES de potencialmente chamar Dispel
                        auto nextIt = std::next(it);

                        if (activeEffect) {  // Verifica se o ponteiro é válido
                            bool shouldDispel = false;
                            RE::MagicItem* sourceMagicItem = activeEffect->spell;

                            // Verifica se o efeito ativo veio do SPELL que estamos removendo
                            if (spellToRemove && sourceMagicItem == spellToRemove) {
                                shouldDispel = true;
                                SKSE::log::info("Dispelando efeito de {}: {}", spellToRemove->GetName(),
                                                activeEffect->GetBaseObject() ? activeEffect->GetBaseObject()->GetName()
                                                                              : "Nome Inválido");
                            }
                            // Verifica se o efeito ativo é o MGEF direto que estamos removendo
                            else if (mgefToRemove && activeEffect->GetBaseObject() == mgefToRemove) {
                                shouldDispel = true;
                                SKSE::log::info("Dispelando efeito direto de MGEF: {}", mgefToRemove->GetName());
                            }

                            if (shouldDispel) {
                                activeEffect->Dispel(true);  // true = force dispel immediately
                                // Dispel pode ou não remover o item da lista imediatamente.
                                // Avançar o iterador *antes* garante que não tenhamos problemas
                                // mesmo se o item for removido.
                            }
                        }
                        // Continua a iteração com o próximo iterador salvo
                        it = nextIt;
                    }  // Fim do while
                } else {
                    SKSE::log::warn("Não foi possível obter ActiveEffectList para Dispel.");
                }

                // Após tentar Dispel, remove o Spell da lista do ator (se aplicável)
                if (spellToRemove && actor->HasSpell(spellToRemove)) {
                    SKSE::log::info("Removendo Spell/Ability da lista do ator: {} ({:08X})", spellToRemove->GetName(),
                                    spellToRemove->GetFormID());
                    actor->RemoveSpell(spellToRemove);
                }
            }  // Fim do escopo
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
            case AppliedEffect::EffectType::Spell:
                if (auto spell = form->As<RE::SpellItem>()) {
                    // Adiciona Habilidades/Poderes Menores passivamente
                    if (spell->GetSpellType() == RE::MagicSystem::SpellType::kAbility ||
                        spell->GetSpellType() == RE::MagicSystem::SpellType::kLesserPower) {
                        if (!actor->HasSpell(spell)) {
                            SKSE::log::info("Adicionando Habilidade/Poder Menor: {} ({:08X})", spell->GetName(),
                                            spell->GetFormID());
                            actor->AddSpell(spell);
                        } else {
                            SKSE::log::debug("Habilidade/Poder Menor {} ({:08X}) já presente.", spell->GetName(),
                                             spell->GetFormID());
                        }
                    } else {  // Casta outros tipos de Spells IMEDIATAMENTE
                        SKSE::log::info("Tentando castar Spell imediatamente: {} ({:08X})", spell->GetName(),
                                        spell->GetFormID());

                        if (auto caster = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)) {
                            // Verifica se o ator pode castar (ainda é uma boa prática)
                            RE::MagicSystem::CannotCastReason reason =
                                RE::MagicSystem::CannotCastReason::kOK;  // Inicializa com OK
                            // Nota: CheckCast pode não ser perfeito para CastSpellImmediate,
                            // mas é uma verificação básica de magicka/silence.
                            if (Settings::MGKRequeriment) {
                                if (actor->CheckCast(spell, false, &reason)) {
                                    auto magicItem = form->As<RE::MagicItem>();

                                    // Verificação de segurança: magicItem não deve ser nulo se spell também não era.
                                    if (!magicItem) {
                                        SKSE::log::error(
                                            "Falha ao converter Form para MagicItem, embora seja um SpellItem!");
                                        return;
                                    }

                                    float magickaCost = magicItem->CalculateMagickaCost(actor);

                                    logger::info("[ApplyHitEffects] Custo de Magicka calculado para {}: {}",
                                                 spell->GetName(), magickaCost);

                                    if (magickaCost > 0.0f) {
                                        actor->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kMagicka,
                                                                                     magickaCost);
                                    }

                                    caster->CastSpellImmediate(spell, false, actor, 1.0f, false, -1.0f, actor);
                                    SKSE::log::info("[ApplyHitEffects] CastSpellImmediate chamado para {}",
                                                    spell->GetName());

                                } else {
                                    SKSE::log::warn(
                                        "Não foi possível castar {} via CastSpellImmediate. Razão CheckCast: {}",
                                        spell->GetName(), static_cast<int>(reason));
                                }
                            } else {
                                caster->CastSpellImmediate(spell, false, actor, 1.0f, false, -1.0f, actor);
                            }

                        } else {
                            SKSE::log::error("Não foi possível obter MagicCaster para CastSpellImmediate de {}",
                                             spell->GetName());
                        }
                        
                    }
                } else {
                    SKSE::log::warn("FormID {:08X} (Plugin: {}) não é um SpellItem válido para adição/cast.",
                                    effect.formID, effect.pluginName);
                }
                break;
        }
    }

    // 3. Atualiza a lista de rastreamento para a próxima mudança
    lastAppliedEffects = newEffectsConst;  // Armazena a lista NÃO ordenada original
}



void GlobalControl::UpdateEffectsForDirectionalChange(int oldState, int newState) {
    SKSE::log::info("Mudança de estado direcional detectada: {} -> {}", oldState, newState);
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;

    // 1. Obter Stance e Moveset Pai atuais
    std::string categoryName = GetCurrentWeaponCategoryName();
    auto animManager = AnimationManager::GetSingleton();
    const auto availableStances = animManager->GetAvailableStances(player, categoryName);
    int originalStanceIndex = 0;
    if (g_currentStance > 0 && g_currentStance <= availableStances.size()) {
        originalStanceIndex = availableStances[g_currentStance - 1].originalIndex;
    } else {
        return;
    }  // Precisa de uma stance ativa

    const auto availableMovesets = animManager->GetAvailableMovesets(player, categoryName, originalStanceIndex);
    if (g_currentMoveset <= 0 || g_currentMoveset > availableMovesets.size()) {
        return;  // Precisa de um moveset pai ativo
    }
    int originalParentMovesetIndex = availableMovesets[g_currentMoveset - 1].originalIndex;

    // 2. Encontrar Stance, ModInstance Pai, SubAnimationInstance Pai
    auto cat_it = animManager->GetCategories().find(categoryName);
    if (cat_it == animManager->GetCategories().end()) return;
    const WeaponCategory& category = cat_it->second;
    if (originalStanceIndex <= 0 || originalStanceIndex > category.instances.size()) return;
    const CategoryInstance& stanceInstance = category.instances[originalStanceIndex - 1];
    const ModInstance* parentModInst = nullptr;
    const SubAnimationInstance* parentSubInst = nullptr;
    int parentCounter = 0;
    auto& mutableStanceInstance = const_cast<CategoryInstance&>(stanceInstance);  // Para iterar
    for (const auto& modInst : mutableStanceInstance.modInstances) {
        if (!modInst.isSelected) continue;
        for (const auto& subInst : modInst.subAnimationInstances) {
            bool isParent =
                !(subInst.pFront || subInst.pBack || subInst.pLeft || subInst.pRight || subInst.pFrontRight ||
                  subInst.pFrontLeft || subInst.pBackRight || subInst.pBackLeft || subInst.pRandom || subInst.pDodge);
            if (!subInst.isSelected || !isParent) continue;
            parentCounter++;
            if (parentCounter == originalParentMovesetIndex) {
                parentModInst = &modInst;
                parentSubInst = &subInst;
                goto found_parent_instances_directional;
            }
        }
    }
found_parent_instances_directional:;
    if (!parentModInst || !parentSubInst) {
        SKSE::log::error("Não foi possível encontrar ModInstance/SubInstance pai para índice {}",
                         originalParentMovesetIndex);
        return;
    }

    // 3. Determinar o Sub-moveset Efetivo (Pai ou Filho Direcional)
    const SubAnimationInstance* effectiveSubInst = parentSubInst;  // Começa com o pai como padrão
    if (newState > 0) {                                            // Se há uma direção
        // Procura o filho correspondente DENTRO do parentModInst
        for (const auto& childSubInst : parentModInst->subAnimationInstances) {
            if (!childSubInst.isSelected) continue;
            // Verifica se é o filho direcional correto
            bool isDirectionalMatch =
                (newState == 1 && childSubInst.pFront) || (newState == 2 && childSubInst.pFrontRight) ||
                (newState == 3 && childSubInst.pRight) || (newState == 4 && childSubInst.pBackRight) ||
                (newState == 5 && childSubInst.pBack) || (newState == 6 && childSubInst.pBackLeft) ||
                (newState == 7 && childSubInst.pLeft) ||
                (newState == 8 && childSubInst.pFrontLeft);  // Adapte os números se necessário

            if (isDirectionalMatch) {
                // Verifica os perks do filho
                if (NCheckActorHasPerks(player, childSubInst.perkList)) {
                    effectiveSubInst = &childSubInst;  // Encontrou filho válido, ele se torna o efetivo
                    SKSE::log::info("Sub-moveset direcional encontrado e válido: {}", newState);
                    break;  // Para de procurar filhos
                } else {
                    SKSE::log::info("Sub-moveset direcional {} encontrado, mas jogador não tem perks.", newState);
                    // Continua sendo o pai
                    break;  // Para de procurar filhos para esta direção
                }
            }
        }
    }

    // 4. Coletar Efeitos Combinados
    std::vector<AppliedEffect> combinedEffects;
    combinedEffects.insert(combinedEffects.end(), stanceInstance.appliedEffects.begin(),
                           stanceInstance.appliedEffects.end());
    combinedEffects.insert(combinedEffects.end(), parentModInst->appliedEffects.begin(),
                           parentModInst->appliedEffects.end());
    combinedEffects.insert(combinedEffects.end(), effectiveSubInst->appliedEffects.begin(),
                           effectiveSubInst->appliedEffects.end());

    // 5. Remover Duplicatas
    std::sort(combinedEffects.begin(), combinedEffects.end());
    combinedEffects.erase(std::unique(combinedEffects.begin(), combinedEffects.end()), combinedEffects.end());

    // 6. Aplicar
    SKSE::log::info("Aplicando {} efeitos combinados para estado direcional {}", combinedEffects.size(), newState);
    ApplyAndTrackEffects(player, combinedEffects, g_lastAppliedMovesetEffects);

    // 7. (Opcional) Atualizar UI? UpdateSkyPromptTexts pode precisar ser chamado
    //    se o nome do moveset mudar com base na direção (GetCurrentMovesetName já faz isso)
    //    e se você quiser que os prompts Next/Back reflitam o moveset pai.
    // UpdateSkyPromptTexts(); // Descomente se necessário
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
        SkyPromptAPI::RemovePrompt(EquipMenu::GetSingleton(), GlobalControl::Dynamicgrip);
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
    if (g_comboState.isTimerRunning) {
        auto now = std::chrono::steady_clock::now();
        auto time_left_ms = std::chrono::duration_cast<std::chrono::milliseconds>(g_comboState.comboTimeoutTimestamp - now).count();
        //SKSE::log::info("[UpdateHandler] Checando timer... g_comboState.isTimerRunning: {}. Tempo restante: {} ms", g_comboState.isTimerRunning,time_left_ms);
    } 
    if (g_comboState.isTimerRunning && std::chrono::steady_clock::now() >= g_comboState.comboTimeoutTimestamp) {
        g_comboState.isTimerRunning = false;

        if (Settings::CycleMoveset) {
            SKSE::GetTaskInterface()->AddTask([]() { TriggerSmartRandomNumber("Fim de Combo (C++)"); });
        }
    } else if (g_hitComboState.isTimerRunning &&
               std::chrono::steady_clock::now() >= g_hitComboState.comboTimeoutTimestamp) {
        g_hitComboState.isTimerRunning = false;

        // Se o timer expirar, o combo de acertos é zerado.
        if (GlobalControl::g_currentHitCount > 0) {
            SKSE::log::info("Hit combo timer expired. Resetting hit count from {} to 0.",
                            GlobalControl::g_currentHitCount);
            GlobalControl::g_currentHitCount = 0;
            player->SetGraphVariableInt("CycleMovesetHitCount", GlobalControl::g_currentHitCount);
            AnimationManager::GetSingleton()->OnHit(player, 0);
        }
      }

    if (a_event && a_event->holder && a_event->holder->IsPlayerRef()) {
        const std::string_view eventName = a_event->tag;
        
        if(eventName == "weaponSwing" || eventName == "weaponLeftSwing" ||
            eventName == "h2hAttack" || eventName == "PowerAttack_Start_end") {

            if (!g_hitComboState.isTimerRunning && GlobalControl::g_currentHitCount > 0) {
                SKSE::log::info("New swing starting after combo expired. Resetting hit count from {} to 0.",
                                GlobalControl::g_currentHitCount);
                GlobalControl::g_currentHitCount = 0;
                player->SetGraphVariableInt("CycleMovesetHitCount", GlobalControl::g_currentHitCount);
                AnimationManager::GetSingleton()->OnHit(player, 0);
            }

            g_comboState.isTimerRunning = true;
            auto timeout_ms = std::chrono::milliseconds(static_cast<int>(Settings::CycleTimer * 1000));
            g_comboState.comboTimeoutTimestamp = std::chrono::steady_clock::now() + timeout_ms;
            

        } else if (eventName == "weaponDraw" || eventName == "weaponSheathe") {
            g_comboState.isTimerRunning = false;  // Cancela qualquer combo pendente
            if (Settings::CycleMoveset) {
                TriggerSmartRandomNumber(std::string(eventName));
            }
        }

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
            SKSE::GetTaskInterface()->AddTask([actor]() { NPCrandomNumber(actor, "Fim de Combo"); });
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

    //SKSE::log::info("{} (Ator {:08X}): Escolheu o moveset #{} da prioridade {}", eventSource, formID,chosenMoveset.index, chosenMoveset.priority);
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


std::span<const SkyPromptAPI::Prompt> GlobalControl::EquipMenu::GetPrompts() const { return prompts; }

void GlobalControl::EquipMenu::ProcessEvent(SkyPromptAPI::PromptEvent event) const {
    if (event.type != SkyPromptAPI::PromptEventType::kAccepted) {
        return;
    }

    auto player = RE::PlayerCharacter::GetSingleton();
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
            //logger::info("Equipando {} na mão esquerda.", itemToEquip->GetName());
            EquipItemWithGripChange(player, itemToEquip, Hooks::g_leftHandSlot);
            break;

        case 1:  // Equipar na mão DIREITA
            //logger::info("Equipando {} na mão direita.", itemToEquip->GetName());
            EquipItemWithGripChange(player, itemToEquip, Hooks::g_rightHandSlot);
            break;
        case 2:  // Equipar nas duas mãos
            //logger::info("Equipando {} nas duas maos.", itemToEquip->GetName());
            EquipItemWithGripChange(player, itemToEquip, Hooks::g_twoHandSlot);
            break;
    }
}

void GlobalControl::EquipMenu::Show(RE::TESBoundObject* a_weapon) const {
    RE::TESObjectWEAP* weapon = nullptr;

    if (const auto a_ref = RE::Inventory3DManager::GetSingleton()->tempRef) {
        const auto refid = a_ref->GetFormID();
        Left_Hand.refid = refid;
        Right_Hand.refid = refid;
    } else {
        // Se, por algum motivo, não houver um modelo 3D, voltamos para 0.
        Left_Hand.refid = 0;
        Right_Hand.refid = 0;
    }
    weapon = a_weapon->As<RE::TESObjectWEAP>();
    if (isTwoHanded(weapon)) {
        SkyPromptAPI::SendPrompt(this, GlobalControl::Dynamicgrip); 
    }
    
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

        bool canUse2HHandle = false;
        const TwoHandHandleConfig* configToCheck = nullptr;

        if (a_actor->IsPlayerRef()) {
            configToCheck = &handle::player2HConfig;
            SKSE::log::info("Equip2H: Verificando requisitos 2H Handle para o Jogador.");
        } else {
            configToCheck = &handle::npc2HConfig;
            SKSE::log::info("Equip2H: Verificando requisitos 2H Handle para NPC '{}'.", a_actor->GetName());
        }

        if (configToCheck) {
            // Checa Nível
            bool levelMet = a_actor->GetLevel() >= configToCheck->minimumLevel;
            // Checa Perks (usando a função de Utils.cpp)
            bool perksMet = NCheckActorHasPerks(a_actor, configToCheck->requiredPerks);

            SKSE::log::info("  - Nível Mínimo: {} (Ator: {}) -> {}", configToCheck->minimumLevel, a_actor->GetLevel(),
                            levelMet ? "OK" : "FALHOU");
            SKSE::log::info("  - Perks Necessários: {} -> {}", configToCheck->requiredPerks.size(),
                            perksMet ? "OK" : "FALHOU");

            if (levelMet && perksMet) {
                canUse2HHandle = true;
                SKSE::log::info("  --> Requisitos 2H Handle CUMPRIDOS.");
            } else {
                SKSE::log::warn("  --> Requisitos 2H Handle NÃO CUMPRIDOS. Equipamento normal será forçado.");
            }
        } else {
            SKSE::log::warn("Equip2H: Não foi possível determinar a configuração 2H Handle a ser verificada.");
        }
        // Força o jogo a pensar que é um item de mão direita
        if (canUse2HHandle) {
            weapon->SetEquipSlot(Hooks::g_rightHandSlot);
            func(a, a_actor, a_form, extraData, count, equipSlot, queueEquip, true, playSounds, true);

            // 3. RESTAURAR (DEPOIS de chamar func)
            if (weapon && originalSlot) {
                // Restaura o slot original para o estado normal (2H)
                weapon->SetEquipSlot(originalSlot);
            }
            if (a_actor) {
                logger::info("--- Verificando Ocupação dos Slots Pós-Equip ---");

                // Função helper para checar e logar um slot
                auto logSlotStatus = [a_actor](RE::BGSEquipSlot* slot, const char* slotName) {
                    if (!slot) {
                        logger::warn("Tentando logar um slot nulo: {}", slotName);
                        return;
                    }

                    RE::TESForm* equippedItem = a_actor->GetEquippedObjectInSlot(slot);
                    if (equippedItem) {
                        // Se o item existir, loga o nome dele
                        logger::info("Slot [{}]: {}", slotName, equippedItem->GetName());
                    } else {
                        // Se estiver vazio, loga "Vazio"
                        logger::info("Slot [{}]: Vazio", slotName);
                    }
                };

                // Logar os três slots que você pediu
                logSlotStatus(Hooks::g_rightHandSlot, "g_rightHandSlot");
                logSlotStatus(Hooks::g_leftHandSlot, "g_leftHandSlot");
                logSlotStatus(Hooks::g_twoHandSlot, "g_twoHandSlot");

                logger::info("-------------------------------------------------");
            }
            return;
        }
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

RE::TESIdleForm* GetIdleByFormID(RE::FormID a_formID, const std::string& a_pluginName) {
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    RE::TESForm* lookupForm = dataHandler ? dataHandler->LookupForm(a_formID, a_pluginName) : nullptr;
    if (!lookupForm) {
        SKSE::log::warn("Não foi possível encontrar o FormID 0x{:X} no plugin {}", a_formID, a_pluginName);
        return nullptr;
    }
    // Verificamos se o tipo do formulário é IdleForm e fazemos o cast.
    if (lookupForm->GetFormType() == RE::FormType::Idle) {
        return static_cast<RE::TESIdleForm*>(lookupForm);
    }
    SKSE::log::warn("O FormID 0x{:X} não é um TESIdleForm.", a_formID);
    return nullptr;
}



void PlayIdleAnimationTarget(RE::Actor* a_actor, RE::TESIdleForm* a_idle, RE::Actor* a_target) {
    if (a_actor && a_idle) {
        if (auto* processManager = a_actor->GetActorRuntimeData().currentProcess) {
            processManager->PlayIdle(a_actor, a_idle, a_target);

            SKSE::log::info("Tocando animação idle FormID 0x{:X}", a_idle->GetFormID());
        } else {
            SKSE::log::error("Não foi possível obter o AIProcess (currentProcess) do ator.");
        }
    }
}

const std::string skyrim = "Skyrim.esm";

RE::BSEventNotifyControl GlobalControl::HitEventHandler::ProcessEvent(
    const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>* a_source) {
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!a_event || !a_event->cause || !a_event->target || a_event->source == 0) {
        return RE::BSEventNotifyControl::kContinue;
    }

    // 2. Checar se o causador é o jogador
    if (!a_event->cause->IsPlayerRef()) {
        return RE::BSEventNotifyControl::kContinue;
    }
    auto* Teste = GetIdleByFormID(0xF4699, skyrim);
    // 3. Checar se o alvo é um NPC válido
    auto* targetNPC = a_event->target.get()->As<RE::Actor>();
    if (!targetNPC || targetNPC->IsPlayerRef() || targetNPC->IsDead()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    // 4. Checar se o NPC é hostil
    if (targetNPC->IsHostileToActor(RE::PlayerCharacter::GetSingleton())) {
        // 5. Checar se a fonte do dano é uma arma (e não um feitiço, soco, etc. a menos que queira)
        auto* weaponForm = RE::TESForm::LookupByID(a_event->source);
        if (weaponForm && weaponForm->IsWeapon()) {

            GlobalControl::g_currentHitCount++;
            player->SetGraphVariableInt("CycleMovesetHitCount", GlobalControl::g_currentHitCount);
            SKSE::log::info("Player hit hostile target. New hit count: {}", GlobalControl::g_currentHitCount);
            AnimationManager::GetSingleton()->OnHit(player, GlobalControl::g_currentHitCount);
            // 2. Esta foi uma rebatida bem-sucedida, reinicie o cronômetro do combo para estender a janela
            PlayIdleAnimationTarget(player, Teste, targetNPC);
            //player->NotifyAnimationGraph("attackPowerStartInPlace");
            g_hitComboState.isTimerRunning = true;
            auto timeout_ms = std::chrono::milliseconds(static_cast<int>(Settings::HitTimer * 1000));
            g_hitComboState.comboTimeoutTimestamp = std::chrono::steady_clock::now() + timeout_ms;
        }
    }

    return RE::BSEventNotifyControl::kContinue;
}

void AnimationManager::OnHit(RE::Actor* actor, int hitCount) {
    if (!actor || !actor->IsPlayerRef()) {
        return;
    }

    SKSE::log::info("[OnHit] Verificando contagem de acertos {} para o ator {}", hitCount, actor->GetName());

    // 1. Obter Stance, Moveset, e Sub-Moveset atuais
    std::string categoryName = GetCurrentWeaponCategoryName();
    auto cat_it = _categories.find(categoryName);
    if (cat_it == _categories.end()) {
        // Sem categoria, aplica lista vazia (limpa efeitos)
        ApplyHitEffects(actor, {});
        return;
    }

    const auto availableStances = GetAvailableStances(actor, categoryName);
    int originalStanceIndex = 0;
    if (GlobalControl::g_currentStance > 0 && GlobalControl::g_currentStance <= availableStances.size()) {
        originalStanceIndex = availableStances[GlobalControl::g_currentStance - 1].originalIndex;
    } else {
        // Sem stance válida, limpa efeitos
        ApplyHitEffects(actor, {});
        return;
    }

    const auto availableMovesets = GetAvailableMovesets(actor, categoryName, originalStanceIndex);
    if (GlobalControl::g_currentMoveset <= 0 || GlobalControl::g_currentMoveset > availableMovesets.size()) {
        // Sem moveset válido, limpa efeitos
        ApplyHitEffects(actor, {});
        return;
    }
    int originalParentMovesetIndex = availableMovesets[GlobalControl::g_currentMoveset - 1].originalIndex;

    // 2. Encontrar Stance, ModInstance (Pai), SubAnimationInstance (Pai)
    const WeaponCategory& category = cat_it->second;
    if (originalStanceIndex <= 0 || originalStanceIndex > category.instances.size()) {
        ApplyHitEffects(actor, {});
        return;
    }
    const CategoryInstance& stanceInstance = category.instances[originalStanceIndex - 1];

    const ModInstance* parentModInst = nullptr;
    const SubAnimationInstance* parentSubInst = nullptr;
    int parentCounter = 0;
    auto& mutableStanceInstance = const_cast<CategoryInstance&>(stanceInstance);
    for (const auto& modInst : mutableStanceInstance.modInstances) {
        if (!modInst.isSelected) continue;
        for (const auto& subInst : modInst.subAnimationInstances) {
            bool isParent =
                !(subInst.pFront || subInst.pBack || subInst.pLeft || subInst.pRight || subInst.pFrontRight ||
                  subInst.pFrontLeft || subInst.pBackRight || subInst.pBackLeft || subInst.pRandom || subInst.pDodge);
            if (!subInst.isSelected || !isParent) continue;
            parentCounter++;
            if (parentCounter == originalParentMovesetIndex) {
                parentModInst = &modInst;
                parentSubInst = &subInst;
                goto found_parent_instances_onhit;
            }
        }
    }
found_parent_instances_onhit:;

    if (!parentModInst || !parentSubInst) {
        SKSE::log::error("[OnHit] Não foi possível encontrar Mod/Sub-instâncias pai para o índice {}",
                         originalParentMovesetIndex);
        ApplyHitEffects(actor, {});
        return;
    }



    // 3. Determinar o Sub-moveset Efetivo (Pai ou Filho Direcional)
    const SubAnimationInstance* effectiveSubInst = parentSubInst;  // Começa com o pai como padrão

    // Pega o estado direcional ATUAL
    int directionalState = GlobalControl::InputListener::GetDirectionalState();

    if (directionalState > 0) {  // Se há uma direção
        // Procura o filho correspondente DENTRO do parentModInst
        for (const auto& childSubInst : parentModInst->subAnimationInstances) {
            if (!childSubInst.isSelected) continue;
            bool isDirectionalMatch =
                (directionalState == 1 && childSubInst.pFront) || (directionalState == 2 && childSubInst.pFrontRight) ||
                (directionalState == 3 && childSubInst.pRight) || (directionalState == 4 && childSubInst.pBackRight) ||
                (directionalState == 5 && childSubInst.pBack) || (directionalState == 6 && childSubInst.pBackLeft) ||
                (directionalState == 7 && childSubInst.pLeft) || (directionalState == 8 && childSubInst.pFrontLeft);

            if (isDirectionalMatch) {
                if (NCheckActorHasPerks(actor, childSubInst.perkList)) {
                    effectiveSubInst = &childSubInst;  // Encontrou filho válido
                    SKSE::log::info("[OnHit] Usando regras de Hit Count do filho direcional (Estado {}).",
                                    directionalState);
                    break;
                } else {
                    SKSE::log::info(
                        "[OnHit] Filho direcional {} encontrado, mas jogador não tem perks. Usando regras do pai.",
                        directionalState);
                    // effectiveSubInst continua sendo o pai
                    break;  // Para de procurar filhos para esta direção
                }
            }
        }
    } else {
        SKSE::log::info("[OnHit] Sem direção. Usando regras de Hit Count do pai.");
    }

    // 4. Coletar TODAS as HitCountRules da hierarquia (Usando effectiveSubInst)
    std::vector<HitCountRule> allRules;
    allRules.insert(allRules.end(), stanceInstance.hitRules.begin(), stanceInstance.hitRules.end());
    allRules.insert(allRules.end(), parentModInst->hitRules.begin(), parentModInst->hitRules.end());
    allRules.insert(allRules.end(), effectiveSubInst->hitRules.begin(),
                    effectiveSubInst->hitRules.end());  // <-- USA O EFETIVO


    // 5. Ordena por hitCount (necessário para a lógica de "melhor camada")
    std::vector<HitCountRule> comboRules;
    std::vector<HitCountRule> periodicRules;
    for (const auto& rule : allRules) {
        if (rule.isPeriodic) {
            periodicRules.push_back(rule);
        } else {
            comboRules.push_back(rule);
        }
    }

    std::vector<AppliedEffect> finalEffectsToApply;

    // 6. Processar Regras de "Combo" (Lógica antiga)
    std::sort(comboRules.begin(), comboRules.end());  // Ordena por hitCount
    int highestValidHitCount = -1;
    std::vector<AppliedEffect> comboEffectsLayer;

    for (const auto& rule : comboRules) {
        if (hitCount >= rule.hitCount) {
            if (NCheckActorHasPerks(actor, rule.perks)) {
                if (rule.hitCount > highestValidHitCount) {
                    highestValidHitCount = rule.hitCount;
                    comboEffectsLayer.clear();  // Descarta camada anterior
                    comboEffectsLayer.insert(comboEffectsLayer.end(), rule.effects.begin(), rule.effects.end());
                } else if (rule.hitCount == highestValidHitCount) {
                    comboEffectsLayer.insert(comboEffectsLayer.end(), rule.effects.begin(), rule.effects.end());
                }
            }
        } else {
            break;  // Lista está ordenada
        }
    }

    if (highestValidHitCount != -1) {
        SKSE::log::info("[OnHit] Encontrada camada de Combo válida: {} hits.", highestValidHitCount);
        finalEffectsToApply.insert(finalEffectsToApply.end(), comboEffectsLayer.begin(), comboEffectsLayer.end());
    }

    // 7. Processar Regras de "Hit Effects" (Lógica Nova)
    if (hitCount > 0) {  // Só processa se houver pelo menos 1 hit
        for (const auto& rule : periodicRules) {
            // Regra é válida se hitCount > 0 E o hit atual é um múltiplo
            if (rule.hitCount > 0 && (hitCount % rule.hitCount == 0)) {
                // Checa os perks
                if (NCheckActorHasPerks(actor, rule.perks)) {
                    SKSE::log::info("[OnHit] Regra Periódica ativada: ({} / {}).", hitCount, rule.hitCount);
                    // Adiciona os efeitos. Não limpamos, acumulamos.
                    finalEffectsToApply.insert(finalEffectsToApply.end(), rule.effects.begin(), rule.effects.end());
                } else {
                    SKSE::log::info("[OnHit] Regra Periódica pulada (falha no perk): ({} / {}).", hitCount,
                                    rule.hitCount);
                }
            }
        }
    }

    // 8. Aplicar os efeitos mesclados de ambos os tipos
    if (!finalEffectsToApply.empty()) {
        // Remove duplicatas da lista *final*
        std::sort(finalEffectsToApply.begin(), finalEffectsToApply.end());
        finalEffectsToApply.erase(std::unique(finalEffectsToApply.begin(), finalEffectsToApply.end()),
                                  finalEffectsToApply.end());

        SKSE::log::info("[OnHit] Aplicando {} efeitos mesclados (Combo + Periódico) para {} acertos.",
                        finalEffectsToApply.size(), hitCount);
        ApplyHitEffects(actor, finalEffectsToApply);
    } else {
        // Nenhuma regra (nem combo, nem periódica) foi ativada.
        // Limpa quaisquer efeitos de acerto anteriores.
        SKSE::log::info(
            "[OnHit] Nenhuma regra (Combo ou Periódica) válida encontrada para {} acertos. Limpando efeitos.",
            hitCount);
        ApplyHitEffects(actor, {});
    }
    // --- FIM DA NOVA LÓGICA ---
}


void AnimationManager::ApplyHitEffects(RE::Actor* actor, const std::vector<AppliedEffect>& newEffectsHit) {
    if (!actor) return;
    if (!actor->IsPlayerRef()) return;  // Só se aplica ao jogador por enquanto

    // Usamos cópias para poder ordenar
    std::vector<AppliedEffect> newEffects = newEffectsHit;
    // Usa a nova variável de membro para rastreamento
    std::vector<AppliedEffect> oldEffects = _lastAppliedHitEffects;

    std::sort(newEffects.begin(), newEffects.end());
    std::sort(oldEffects.begin(), oldEffects.end());

    // 1. Encontra efeitos para REMOVER
    std::vector<AppliedEffect> toRemove;
    std::set_difference(oldEffects.begin(), oldEffects.end(), newEffects.begin(), newEffects.end(),
                        std::back_inserter(toRemove));

    for (const auto& effect : toRemove) {
        RE::TESForm* form = RE::TESForm::LookupByID(effect.formID);
        if (!form) {
            SKSE::log::warn("[ApplyHitEffects] FormID {:08X} não encontrado para remoção.", effect.formID);
            continue;
        }

        switch (effect.type) {
            case AppliedEffect::EffectType::Perk:
                if (auto perk = form->As<RE::BGSPerk>()) {
                    if (actor->HasPerk(perk)) {
                        SKSE::log::info("[ApplyHitEffects] Removendo Perk: {}", perk->GetName());
                        actor->RemovePerk(perk);
                    }
                }
                break;
            case AppliedEffect::EffectType::Spell: {
                RE::SpellItem* spellToRemove = form->As<RE::SpellItem>();
                if (auto activeEffectList = actor->AsMagicTarget()->GetActiveEffectList()) {
                    auto it = activeEffectList->begin();
                    while (it != activeEffectList->end()) {
                        RE::ActiveEffect* activeEffect = *it;
                        auto nextIt = std::next(it);
                        if (activeEffect) {
                            RE::MagicItem* sourceMagicItem = activeEffect->spell;
                            if (spellToRemove && sourceMagicItem == spellToRemove) {
                                SKSE::log::info("[ApplyHitEffects] Dispelando efeito de {}: {}",
                                                spellToRemove->GetName(),
                                                activeEffect->GetBaseObject() ? activeEffect->GetBaseObject()->GetName()
                                                                              : "Nome Inválido");
                                activeEffect->Dispel(true);
                            }
                        }
                        it = nextIt;
                    }
                }
                if (spellToRemove && actor->HasSpell(spellToRemove)) {
                    SKSE::log::info("[ApplyHitEffects] Removendo Spell/Ability: {}", spellToRemove->GetName());
                    actor->RemoveSpell(spellToRemove);
                }
            } break;
            case AppliedEffect::EffectType::MagicEffect:
                break;
        }
    }

    // 2. Encontra efeitos para ADICIONAR
    std::vector<AppliedEffect> toAdd;
    std::set_difference(newEffects.begin(), newEffects.end(), oldEffects.begin(), oldEffects.end(),
                        std::back_inserter(toAdd));

    for (const auto& effect : toAdd) {
        RE::TESForm* form = RE::TESForm::LookupByID(effect.formID);
        if (!form) {
            SKSE::log::warn("[ApplyHitEffects] FormID {:08X} não encontrado para adição.", effect.formID);
            continue;
        }

        switch (effect.type) {
            case AppliedEffect::EffectType::Perk:
                if (auto perk = form->As<RE::BGSPerk>()) {
                    if (!actor->HasPerk(perk)) {
                        SKSE::log::info("[ApplyHitEffects] Adicionando Perk: {}", perk->GetName());
                        actor->AddPerk(perk, 1);
                    }
                }
                break;
            case AppliedEffect::EffectType::Spell:
                if (auto spell = form->As<RE::SpellItem>()) {
                    if (spell->GetSpellType() == RE::MagicSystem::SpellType::kAbility ||
                        spell->GetSpellType() == RE::MagicSystem::SpellType::kLesserPower) {
                        if (!actor->HasSpell(spell)) {
                            SKSE::log::info("[ApplyHitEffects] Adicionando Ability/Power: {}", spell->GetName());
                            actor->AddSpell(spell);
                        }
                    } else {
                        // --- INÍCIO DA MODIFICAÇÃO: Lógica copiada de ApplyAndTrackEffects ---
                        SKSE::log::info("[ApplyHitEffects] Tentando castar Spell: {} ({:08X})", spell->GetName(),
                                        spell->GetFormID());

                        if (auto caster = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)) {
                            // Inicializa a razão com OK
                            RE::MagicSystem::CannotCastReason reason =
                                RE::MagicSystem::CannotCastReason::kOK;  
                            if (Settings::MGKRequeriment) {
                                if (actor->CheckCast(spell, false, &reason)) {
                                    auto magicItem = form->As<RE::MagicItem>();

                                    // Verificação de segurança: magicItem não deve ser nulo se spell também não era.
                                    if (!magicItem) {
                                        SKSE::log::error(
                                            "Falha ao converter Form para MagicItem, embora seja um SpellItem!");
                                        return;  
                                    }

                                    float magickaCost = magicItem->CalculateMagickaCost(actor);

                                    logger::info(
                                        "[ApplyHitEffects] Custo de Magicka calculado para {}: {}",
                                                 spell->GetName(), magickaCost);

                                    if (magickaCost > 0.0f) {
                                        actor->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kMagicka,
                                                                                     magickaCost);
                                    }

                                    caster->CastSpellImmediate(spell, false, actor, 1.0f, false, -1.0f, actor);
                                    SKSE::log::info("[ApplyHitEffects] CastSpellImmediate chamado para {}",
                                                    spell->GetName());

                                } else {
                                    // Se não puder, avisa no log
                                    SKSE::log::warn("[ApplyHitEffects] Não foi possível castar {}. Razão CheckCast: {}",
                                                    spell->GetName(), static_cast<int>(reason));
                                }

                            } else {
                                caster->CastSpellImmediate(spell, false, actor, 1.0f, false, -1.0f, actor);
                            }

                           

                        } else {
                            SKSE::log::error("[ApplyHitEffects] Não foi possível obter MagicCaster para CastSpellImmediate de {}",
                                             spell->GetName());
                        }
                        // --- FIM DA MODIFICAÇÃO ---
                    }
                }
                break;
            case AppliedEffect::EffectType::MagicEffect:
                SKSE::log::warn(
                    "[ApplyHitEffects] Não é possível adicionar diretamente um MagicEffect ({}). Adicione o Spell pai.",
                    form->GetName());
                break;
        }
    }

    // 3. Atualiza a lista de rastreamento
    _lastAppliedHitEffects = newEffectsHit;  // Armazena a lista original, não ordenada
}