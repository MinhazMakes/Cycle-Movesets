#pragma once
#include <array>
#include <filesystem>
#include <string>
#include <vector>
#include "MCP.h"
// Enum para os tipos de regra
enum class RuleType { UniqueNPC, Faction, Keyword, Race, GeneralNPC, Player };

// Structs para guardar os dados carregados do jogo (para os pop-ups de seleção)
struct FactionInfo {
    RE::FormID formID;
    std::string editorID;
    std::string pluginName;
};

struct KeywordInfo {
    RE::FormID formID;
    std::string editorID;
    std::string pluginName;
};

struct RaceInfo {
    RE::FormID formID;
    std::string editorID;
    std::string fullName;
    std::string pluginName;
};


RuleType RuleTypeFromString(const std::string& s);
std::string RuleTypeToString(RuleType type);

struct PerkInfo {
    RE::FormID formID;
    std::string editorID;
    std::string name;
    std::string pluginName;
};

struct AvailableItem {
    std::string name;
    int originalIndex;  // O índice real (1-based) que o OAR espera
};

struct AppliedEffect {
    enum class EffectType { Perk, MagicEffect, Spell };

    EffectType type;
    std::string pluginName;
    RE::FormID formID;
    std::string origin = "";  // "Stance", "Moveset", "SubMoveset" (para rastreamento na UI)

    // Necessário para usar std::set ou std::unique mais tarde
    bool operator<(const AppliedEffect& other) const {
        if (formID != other.formID) return formID < other.formID;
        return type < other.type;  // Diferencia Perk 123 de MGEF 123
    }
    bool operator==(const AppliedEffect& other) const { return formID == other.formID && type == other.type; }
};

struct DPATags {
    bool hasA = false;  // Para BFCO_PowerAttackA.hkx
    bool hasB = false;  // Para BFCO_PowerAttackB.hkx
    bool hasL = false;  // Para BFCO_PowerAttackL.hkx
    bool hasR = false;  // Para BFCO_PowerAttackR.hkx

    // Função auxiliar para verificar se algum DPA está disponível
    bool any() const { return hasA || hasB || hasL || hasR; }
};
struct MovesetTags {
    DPATags dpaTags;
    bool hasCPA = false;
};
// --- Definições da Biblioteca ---
struct SubAnimationDef {
    std::string name;
    std::filesystem::path path;
    int attackCount = 0;       // Contagem de arquivos BFCO_Attack
    int powerAttackCount = 0;  // Contagem de arquivos BFCO_PowerAttack
    bool hasIdle = false;      // Presença de arquivos "idle"
    bool hasAnimations = false;
    DPATags dpaTags;
    bool hasCPA = false; 
    std::filesystem::path triggerFile;
};
struct AnimationModDef {
    std::string name;
    std::string author;
    std::vector<SubAnimationDef> subAnimations;
};

struct PerkDef {
    std::string pluginName;
    RE::FormID formID = 0;
    std::string origin;  // "Stance", "Moveset", ou "SubMoveset"

    // Opcional: Adicionar um comparador para facilitar buscas
    bool operator==(const RE::FormID& otherFormID) const { return formID == otherFormID; }
};

struct HitCountRule {
    int hitCount = 0;
    std::vector<PerkDef> perks;
    std::vector<AppliedEffect> effects;

    // Operador para ordenação (útil para a UI)
    bool operator<(const HitCountRule& other) const { return hitCount < other.hitCount; }
};

// --- Estruturas de Configuração do Usuário ---
struct SubAnimationInstance {
    // --- ALTERADO: Usamos nomes para salvar/carregar. Os índices serão preenchidos em tempo de execução. ---
    std::string sourceModName;  // Nome do mod de origem (e.g., "BFCO")
    std::string sourceSubName;  // Nome da sub-animação de origem (e.g., "700036")
    size_t sourceModIndex;
    size_t sourceSubAnimIndex;
    std::array<char, 128> editedName{};
    bool isSelected = true;
    bool pFront = false;
    bool pBack = false;
    bool pLeft = false;
    bool pRight = false;
    bool pFrontRight = false;
    bool pFrontLeft = false;
    bool pBackRight = false;
    bool pBackLeft = false;
    bool pRandom = false;
    bool pDodge = false;
    DPATags dpaTags;
    bool hasCPA = false;
    std::vector<PerkDef> perkList;
    std::vector<AppliedEffect> appliedEffects;
    std::vector<HitCountRule> hitRules;
};

struct ModInstance {
    size_t sourceModIndex;
    bool isSelected = true;
    std::vector<SubAnimationInstance> subAnimationInstances;
    int level = 0;  // Condição de Nível Mínimo
    int hp = 100;   // Condição de HP Máximo (em porcentagem)
    int st = 100;
    int mn = 100;
    int order = 0;
    std::vector<PerkDef> perkList;
    std::vector<AppliedEffect> appliedEffects;
    std::vector<HitCountRule> hitRules;
};

struct CategoryInstance {
    std::vector<ModInstance> modInstances;
    std::vector<PerkDef> perkList;
    std::vector<AppliedEffect> appliedEffects;
    std::vector<HitCountRule> hitRules;
};

struct WeaponCategory {
    std::string name;
    double equippedTypeValue;
    double leftHandEquippedTypeValue = -1.0;
    int activeInstanceIndex = 0;
    bool isDualWield = false;
    bool isShieldCategory = false;
    std::vector<std::string> keywords;
    std::vector<std::string> leftHandKeywords;
    std::vector<CategoryInstance> instances;
    // --- NOVO ---
    // Armazena os nomes customizados das stances
    std::vector<std::string> stanceNames;
    // Buffer para edição no ImGui (evita problemas com std::string)
    std::vector<std::array<char, 64>> stanceNameBuffers;

    bool isCustom = false;
    std::string baseCategoryName;
    bool ownerIsPlayer;
    int uiTabVersion = 0;
};

struct UserMoveset {
    std::string name;
    std::vector<SubAnimationInstance> subAnimations;
};

struct MovesetRule {
    RuleType type;
    std::string displayName;  // Nome amigável para a UI (ex: "Ulfric Stormcloak", "BanditFaction")
    std::string identifier;   // O identificador único (FormID em string ou EditorID)
    std::string pluginName;   // Relevante para FormIDs
    RE::FormID formID;
    // Cada regra tem seu próprio conjunto de categorias de armas.
    // Reutiliza a mesma estrutura que você já tem para o jogador e NPCs.
    std::map<std::string, WeaponCategory> categories;
};
struct NpcRuleMatch {
    const MovesetRule* rule = nullptr;  // O ponteiro que precisamos!
    int movesetCount = 0;
    int priority = 0;
};
void WheelerKeys();
inline int WheelerKeyboard = 0;
inline int WheelerGamepad = 0;

struct TwoHandHandleConfig {
    std::vector<PerkDef> requiredPerks;
    int minimumLevel = 0;
    std::vector<PerkDef> requiredPerksDual2H;
};
namespace handle{
    inline TwoHandHandleConfig player2HConfig;
    inline TwoHandHandleConfig npc2HConfig;
}
