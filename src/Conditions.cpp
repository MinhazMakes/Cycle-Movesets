#include "Conditions.h"  // Certifique-se que o include para Conditions.h está correto

namespace Conditions {
    // Construtor
    IsEquipSlotOccupied::IsEquipSlotOccupied() {
        // Usamos kText
        slotNameComponent = static_cast<ITextConditionComponent*>(
            AddBaseComponent(ConditionComponentType::kText, "Slot Name",
                             "Slot a verificar (Ex: RightHand, LeftHand, TwoHand, DualWield)"));

        slotNameComponent->SetAllowSpaces(false);
        slotNameComponent->SetTextValue("RightHand");  // Define "RightHand" como valor padrão
    }

    RE::BSString IsEquipSlotOccupied::GetArgument() const { return slotNameComponent->GetTextValue(); }

    // --- FUNÇÃO EvaluateImpl CORRIGIDA ---
    bool IsEquipSlotOccupied::EvaluateImpl(RE::TESObjectREFR* a_refr,
                                           [[maybe_unused]] RE::hkbClipGenerator* a_clipGenerator,
                                           [[maybe_unused]] void* a_subMod) const {
        if (!a_refr) {
            return false;
        }

        auto actor = a_refr->As<RE::Actor>();
        if (!actor) {
            return false;
        }

        // Pega o nome do slot digitado pelo usuário
        std::string slotName = slotNameComponent->GetTextValue().c_str();

        // Pega os objetos nos slots relevantes
        RE::TESForm* rightHandObject = actor->GetEquippedObjectInSlot(Hooks::g_rightHandSlot);  // 0x13f42
        RE::TESForm* leftHandObject = actor->GetEquippedObjectInSlot(Hooks::g_leftHandSlot);    // 0x13f43
        RE::TESForm* twoHandObject = actor->GetEquippedObjectInSlot(Hooks::g_twoHandSlot);      // 0x13f45

        // --- Lógica Principal (Corrigida conforme sua instrução) ---

        if (_stricmp(slotName.c_str(), "RightHand") == 0) {
            // Retorna true se o slot da mão direita estiver ocupado E for uma arma
            return (rightHandObject && rightHandObject->IsWeapon());
        } else if (_stricmp(slotName.c_str(), "LeftHand") == 0) {
            // Retorna true se o slot da mão esquerda estiver ocupado E for uma arma
            return (leftHandObject && leftHandObject->IsWeapon());
        } else if (_stricmp(slotName.c_str(), "TwoHand") == 0) {
            // Retorna true se o slot 'TwoHand' (0x13f45) estiver ocupado E for uma arma
            // Nota: Este slot é raramente usado para armas, mas a lógica está aqui conforme solicitado.
            // A verificação de "arma de duas mãos" geralmente envolve checar se g_rightHandSlot
            // tem uma arma com a flag TwoHanded, mas seguimos sua lógica de slots.
            return (twoHandObject && twoHandObject->IsWeapon());
        } else if (_stricmp(slotName.c_str(), "DualWield") == 0) {
            // Retorna true se AMBOS os slots estiverem ocupados E AMBOS forem armas
            return (rightHandObject && rightHandObject->IsWeapon() && leftHandObject && leftHandObject->IsWeapon());
        }

        // Se o texto não for reconhecido, falha
        return false;
    }
}  // namespace Conditions