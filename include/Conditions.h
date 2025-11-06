#pragma once

#include "OAR/OpenAnimationReplacer-ConditionTypes.h"  
#include "OAR/OpenAnimationReplacerAPI-Conditions.h"  
#include "Hooks.h"                                   
#include "PCH.h"
#include "ClibUtil/singleton.hpp"
#include "ClibUtil/editorID.hpp"

namespace Conditions {
    class IsEquipSlotOccupied : public CustomCondition {
    public:
        constexpr static inline std::string_view CONDITION_NAME = "IsEquipSlotOccupied";

        IsEquipSlotOccupied();

        // --- Funções obrigatórias da interface ICondition ---
        RE::BSString GetName() const override { return CONDITION_NAME.data(); }
        RE::BSString GetDescription() const override {
            return "Verifica se o slot de equipamento especificado esta ocupado.";
        }
        constexpr REL::Version GetRequiredVersion() const override { return {1, 0, 0}; }
        RE::BSString GetArgument() const override;

    protected:
        bool EvaluateImpl(RE::TESObjectREFR* a_refr, RE::hkbClipGenerator* a_clipGenerator,
                          void* a_subMod) const override;

        // --- Componente da Condição ---
        // Voltamos a usar ITextConditionComponent
        ITextConditionComponent* slotNameComponent;
    };

    class IsRefFormID : public CustomCondition {
    public:
        constexpr static inline std::string_view CONDITION_NAME = "IsRefFormID";

        IsRefFormID();

        RE::BSString GetName() const override { return CONDITION_NAME.data(); }
        RE::BSString GetDescription() const override {
            return "Verifica se o RefFormID do alvo corresponde ao valor especificado.";
        }
        constexpr REL::Version GetRequiredVersion() const override { return {1, 0, 0}; }
        RE::BSString GetArgument() const override;

    protected:
        bool EvaluateImpl(RE::TESObjectREFR* a_refr, RE::hkbClipGenerator* a_clipGenerator,
                          void* a_subMod) const override;

        ITextConditionComponent* RefFormIDComponent;
    };
}