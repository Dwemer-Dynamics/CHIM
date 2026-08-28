#include <Windows.h>
#include <mutex>
#include "Globals.h"

namespace logger = SKSE::log;

#ifndef SPG_MISC_H
#define SPG_MISC_H




std::string getCurrentTimeMillis();
std::uint32_t ConvertToTimestamp(const std::tm& time);

std::string getTimestampNanos();
void replaceAll(std::string& str, const std::string& from, const std::string& to);

void setNewActionModeFromConfig();

void ProcedureSendActiveQuests();

long long GetGameTimeStamp();
void ResetGameTimeStamp();

std::string GetPlayerLocation();
std::string GetPlayerName();
std::string BuildActorReferenceSource(RE::Actor* actor);
std::string GetCurrentWeatherDescription();
std::string BuildCurrentWorldContextDetails();

void ReplaceTagsInQuestText(RE::BSString* a_text, const RE::TESQuest* a_quest, std::uint32_t a_questInstanceId);
std::string GetCurrentDescriptionWithReplacedTags(
    const RE::TESQuest* a_quest,
    std::uint32_t a_questInstanceId);


void InterruptNPC(RE::Actor* actor, AIAgent* agent);

float GetPitchFromQuaternion(const RE::NiQuaternion& q);
float GetPitchFromQuaternionDebug(const RE::NiQuaternion& q);

// Returns true if the scene pointer is not null (i.e., player is in a scene)
inline bool CheckScene(RE::BGSScene* scene)
{
    if (!scene) return false;
    if (scene->GetFormID() == 0x40200da) return false;  // special case for some reason
    
    return scene != nullptr;
}
    


struct RE::TESTrapHitEvent  // not finished
{
    RE::NiPointer<RE::TESObjectREFR> trap;
    RE::NiPointer<RE::TESObjectREFR> target;
    uint32_t flag;  // flag ? 0 - traphit, 1 - traphitend, 2 - traphitstart
    float f1;
    float f2;
    float f3;
    float f4;
    float f5;
    float f6;
    float f7;  // 120 damage
    float f8;
    float f9;
    float f10;
    float f11;
    float f12;
    float f13;
    float f14;
};

struct RE::TESQuestInitEvent {
    uint32_t formID;
};

struct RE::TESTopicInfoEvent {
    Actor* speaker;      // 00 - NiTPointer<Actor>
    void* unk04;         // 04 - BSTSmartPointer<REFREventCallbacks::IEventCallback>
    FormID topicInfoID;  // 08
    bool flag;           // 0C

    inline bool IsStarting() { return !flag; }
    inline bool IsStopping() { return flag; }
};

struct RE::TESBookReadEvent {
    RE::NiPointer<RE::TESObjectREFR> book;
};


struct RE::TESPackageEvent {
    enum class EventType : uint32_t  // not sure
    {
        kStart = 0,
        kChange = 1,
        kEnd = 2
    };
    RE::NiPointer<RE::TESObjectREFR> actor;
    RE::FormID package;
    EventType type;
};

struct RE::TESSceneActionEvent {
    void* reference;
    RE::FormID sceneId;
    uint32_t actionIndex;
    RE::FormID questId;
    uint32_t actorAliasId;
};

struct RE::TESScenePhaseEvent {
    RE::FormID sceneFormID;  // 00
    uint32_t unk04;
    uint32_t unk08;
    TESObjectREFR* unk0C;
};
struct RE::TESSceneEvent {
    RE::TESForm* reference;  // what ref?
    RE::FormID sceneId;
};

struct TESLockChangedEventEx {
    RE::TESObjectREFR* lock;
    RE::TESObjectREFR* unlocker;
};

namespace RE
{
	namespace BSGraphics
	{
		enum class TextureFileFormat
		{
			kBMP = 0,
			kJPG = 1,
			kTGA = 2,
			kPNG = 3,
			kDDS = 4,
		};
	}
}

namespace RE {
    struct ScreenshotHandler : public MenuEventHandler {
    public:
        inline static constexpr auto RTTI = RTTI_ScreenshotHandler;
        inline static constexpr auto VTABLE = VTABLE_ScreenshotHandler;

        ~ScreenshotHandler() override;  // 00

        // override (MenuEventHandler)
        bool CanProcess(InputEvent* a_event) override;      // 01
        bool ProcessButton(ButtonEvent* a_event) override;  // 05

        // members
        bool screenshotQueued;       // 10
        bool multiScreenshotQueued;  // 11
        std::uint16_t pad12;         // 12
        std::uint32_t pad14;         // 14
    };
    static_assert(sizeof(ScreenshotHandler) == 0x18);
}




// Forward declarations for voice CSV functions
std::string FindVoiceInCSV(const std::string& voiceType);

struct AudioFileBufferEntry {
    RE::Actor* actor;
    std::string data;
    int speechLengt;
};

class AudioFilesBufferManager {
public:
    static std::list<AudioFileBufferEntry> audioFilesBuffer;
    
    // Add an entry
    static void addAudioFile(RE::Actor* actor, const std::string& data, int speechLength) {
    
        //{ _data = 0x000001d1a70d2978 "Data\\Sound\\Voice\\Nessa.esp\\NQ_NessaVT\\NQ_A_NQ_A_Hello_0000FB13_1.wav" }
        bool zealotAdvisor = true;
        if (data.starts_with("Sound\\Voice\\Skyrim.esm")) {
            
            zealotAdvisor = false;
        
        } else if (data.starts_with("Sound\\Voice\\Dragonborn.esm")) {
            zealotAdvisor = false;

        } else if (data.starts_with("Sound\\Voice\\Dawnguard.esm")) {
            zealotAdvisor = false;

        } else if (data.starts_with("Sound\\Voice\\Update.esm")) {
            zealotAdvisor = false;

        } else if (data.starts_with("Sound\\Voice\\Hearthfires.esm")) {
            zealotAdvisor = false;

        } 

        if (zealotAdvisor && false) {
            logger::info("No vanilla NPC ... skipping");
            return;
        }

        if (!audioFilesBuffer.empty()) {
            const auto& lastEntry = audioFilesBuffer.back();
            if (lastEntry.speechLengt >= speechLength && lastEntry.actor==actor) {
                // Don't insert if the last element's speechLength is greater than or equal to the new entry's
                // speechLength
                return;
            }
        }

        audioFilesBuffer.emplace_back(AudioFileBufferEntry{actor, data, speechLength});

    }




    static std::string findAudioFile(RE::Actor* actor) {

        std::unordered_map<std::string, std::string> skyrimVoicesBanned = {
            {"femalecommoner", "skyrim.esm\\crdogvoice\\da03_da03barbasmoreinfo0_0001cdaa_3.fuz"},
            {"maleuniquedbspectrallachance", "skyrim.esm\\crdogvoice\\da03_da03barbasmoreinfo0_0001cdaa_3.fuz"},
            {"maleuniqueemperor", "skyrim.esm\\crdogvoice\\da03_da03barbasmoreinfo0_0001cdaa_3.fuz"},
            {"maleuniquehermaeusmora", "skyrim.esm\\crdogvoice\\da03_da03barbasmoreinfo0_0001cdaa_3.fuz"},
            {"maleuniquesheogorath", "skyrim.esm\\crdogvoice\\da03_da03barbasmoreinfo0_0001cdaa_3.fuz"},
        };

        std::unordered_map<std::string, std::string> skyrimVoices = {
            {"crdogvoice", "skyrim.esm\\crdogvoice\\da03_da03barbasmoreinfo0_0001cdaa_3.fuz"},
            {"crdragonpriestvoice", "skyrim.esm\\crdragonpriestvoice\\mg07__00080107_1.fuz"},
            {"crdragonvoice", "dawnguard.esm\\crdragonvoice\\dlc1vqdrag_dlc1vqdragontli_0001328f_1.fuz"},
            {"crdraugrvoice", "skyrim.esm\\crdraugrvoice\\dialoguedraugr__0001695e_1.fuz"},
            {"crdremoravoice", "skyrim.esm\\crdremoravoice\\dunmidden0_middenvelehkwea_00075c75_2.fuz"},
            {"crhagravenvoice", "skyrim.esm\\crhagravenvoice\\dunblindcl_dunblindcliffre_00077c23_1.fuz"},
            {"cruniquealduin", "skyrim.esm\\cruniquealduin\\mq206__0007a0da_1.fuz"},
            {"cruniqueodahviing", "skyrim.esm\\cruniqueodahviing\\mq301_mq301odahviingb2_00048f05_2.fuz"},
            {"cruniquepaarthurnax", "skyrim.esm\\cruniquepaarthurnax\\mq301_mq301paarthurnaxcall_000d2cf4_1.fuz"},
            {"dlc1femaleuniquefura", "dawnguard.esm\\dlc1femaleuniquefura\\dlc1rv01_dlc1rv01completet_000150e2_1.fuz"},
            {"dlc1femaleuniquevalerica",
             "dawnguard.esm\\dlc1femaleuniquevalerica\\dlc1vq05_dlc1vq05valericai_0000f84f_4.fuz"},
            {"dlc1femalevampire", "dawnguard.esm\\dlc1femalevampire\\dlc1dialoguevampire__0001983f_1.fuz"},
            {"dlc1ld_femalenorduniquekatria",
             "dawnguard.esm\\dlc1ld_femalenorduniquekatria\\dlc1ld_ark_dlc1ld_d2_katri_000163fc_1.fuz"},
            {"dlc1maleuniquedexion", "dawnguard.esm\\dlc1maleuniquedexion\\dlc1dialog_dlc1dialoguehun_0000fd6f_2.fuz"},
            {"dlc1maleuniqueflorentius",
             "dawnguard.esm\\dlc1maleuniqueflorentius\\dlc1dialog_dlc1dialoguehun_0000e7a9_1.fuz"},
            {"dlc1maleuniquegaran", "dawnguard.esm\\dlc1maleuniquegaran\\dlc1vampir_dlc1vampirebase_00018b65_1.fuz"},
            {"dlc1maleuniquegelebor",
             "dawnguard.esm\\dlc1maleuniquegelebor\\dlc1vq07_dlc1vq07gelebortl_00015068_2.fuz"},
            {"dlc1maleuniquegunmar", "dawnguard.esm\\dlc1maleuniquegunmar\\dlc1radian_dlc1radianttrol_0001056f_3.fuz"},
            {"dlc1maleuniqueharkon", "dawnguard.esm\\dlc1maleuniqueharkon\\dlc1vq03va_dlc1vq03vampire_000069a8_2.fuz"},
            {"dlc1maleuniqueisran", "dawnguard.esm\\dlc1maleuniqueisran\\dlc1vq03hunter__000098c7_2.fuz"},
            {"dlc1maleuniquejiub", "dawnguard.esm\\dlc1maleuniquejiub\\dlc1vqsain_dlc1vqsaintdrem_00014156_1.fuz"},
            {"dlc1maleuniquesnowelfghost",
             "dawnguard.esm\\dlc1maleuniquesnowelfghost\\dlc1vq07_dlc1vq07prelateca_00014f41_1.fuz"},
            {"dlc1maleuniquevyrthur", "dawnguard.esm\\dlc1maleuniquevyrthur\\dlc1vq07__0000d67b_2.fuz"},
            {"dlc1malevampire", "dawnguard.esm\\dlc1malevampire\\dlc1vq03va_dlc1vq03vampire_000098c4_2.fuz"},
            {"dlc1seranavoice", "dawnguard.esm\\dlc1seranavoice\\dlc1vq05_dlc1vq05relations_00014f82_1.fuz"},
            {"dlc2crgiantvoicekarstaag",
             "dragonborn.esm\\dlc2crgiantvoicekarstaag\\dlc2dunkar_dlc2dunkarstaag_00028203_1.fuz"},
            {"dlc2femaledarkelfcommoner",
             "dragonborn.esm\\dlc2femaledarkelfcommoner\\dlc2rrfavo_dlc2rrfavor06in_00024fa3_3.fuz"},
            {"dlc2femaleuniquefrea", "dragonborn.esm\\dlc2femaleuniquefrea\\dlc2dialog_dlc2dialogueska_00039211_4.fuz"},
            {"dlc2maledarkelfcommoner",
             "dragonborn.esm\\dlc2maledarkelfcommoner\\dlc2dialog_dlc2drrbeggarss_00034f84_3.fuz"},
            {"dlc2maledarkelfcynical",
             "dragonborn.esm\\dlc2maledarkelfcynical\\dlc2dunkol_dlc2dunkolbjorn_000275ac_3.fuz"},
            {"dlc2maleuniqueadril", "dragonborn.esm\\dlc2maleuniqueadril\\dlc2rrarri_dlc2rrarrivalsc_00039254_1.fuz"},
            {"dlc2maleuniquelleril", "dragonborn.esm\\dlc2maleuniquelleril\\dlc2dialog_dlc2drrmorvaynt_00023fd5_3.fuz"},
            {"dlc2maleuniquemiraak", "dragonborn.esm\\dlc2maleuniquemiraak\\dlc2mq02__0003a140_1.fuz"},
            {"dlc2maleuniquemodyn", "dragonborn.esm\\dlc2maleuniquemodyn\\dlc2dialog_dlc2dgcrimewant_0002c095_1.fuz"},
            {"dlc2maleuniqueneloth", "dragonborn.esm\\dlc2maleuniqueneloth\\dlc2mq04_dlc2mq04nelothnch_00019cb5_1.fuz"},
            {"dlc2maleuniquestorn", "dragonborn.esm\\dlc2maleuniquestorn\\dlc2mq05_dlc2mq05mq05storn_0001dfb3_1.fuz"},
            {"dlc2rieklingvoice", "dragonborn.esm\\dlc2rieklingvoice\\dlc2mh02_dlc2mh02chiefredg_0001fe0f_1.fuz"},
            {"femaleargonian", "skyrim.esm\\femaleargonian\\ms04_ms04avanchnzelinnkeep_00056551_1.fuz"},
            {"femalechild", "skyrim.esm\\femalechild\\dbeviction_dbnazireviction_0006f9a0_1.fuz"},
            {"femalecommander", "skyrim.esm\\femalecommander\\cw_cwcampaignfieldcomissio_000221e8_1.fuz"},
            //{"femalecommoner", "skyrim.esm\\femalecommoner\\dialogueso_dialoguesolitud_000c0699_1.fuz"},
            {"femalecondescending", "skyrim.esm\\femalecondescending\\tg02b_tg02btoniliabranchto_000d33bf_1.fuz"},
            {"femalecoward", "skyrim.esm\\femalecoward\\dunsouthfr_dunboulderfallq_0003a6a7_3.fuz"},
            {"femaledarkelf", "skyrim.esm\\femaledarkelf\\da02_da02whosboethiah_0004d8be_3.fuz"},
            {"femaleelfhaughty", "skyrim.esm\\femaleelfhaughty\\dbeviction_dbnazireviction_0006f99f_1.fuz"},
            {"femaleeventoned", "skyrim.esm\\femaleeventoned\\ms01_ms01margretinfoinvisi_000d6686_2.fuz"},
            {"femalekhajiit", "skyrim.esm\\femalekhajiit\\caravanscene7__00072d27_1.fuz"},
            {"femalenord", "skyrim.esm\\femalenord\\dialogueguardsgeneral__000dd08b_1.fuz"},
            {"femaleoldgrumpy", "skyrim.esm\\femaleoldgrumpy\\dialogueriften__0008bacf_1.fuz"},
            {"femaleoldkindly", "skyrim.esm\\femaleoldkindly\\darksidecontractdialogue__0009bd39_1.fuz"},
            {"femaleorc", "skyrim.esm\\femaleorc\\dialoguetu_tutorialcombat_000dd63c_1.fuz"},
            {"femaleshrill", "skyrim.esm\\femaleshrill\\dialoguewi_dialoguewinterh_0006de34_2.fuz"},
            {"femalesultry", "skyrim.esm\\femalesultry\\dialogueso_dialoguesolitud_000c069b_2.fuz"},
            {"femaleuniqueastrid", "skyrim.esm\\femaleuniqueastrid\\darkbrotherhood__000fdbe6_1.fuz"},
            {"femaleuniqueazura", "skyrim.esm\\femaleuniqueazura\\da01_da01azurafinaltopic02_0009377e_1.fuz"},
            {"femaleuniqueboethiah", "skyrim.esm\\femaleuniqueboethiah\\da02_da02boethahslayfollow_0005fc7f_3.fuz"},
            {"femaleuniquedelphine", "skyrim.esm\\femaleuniquedelphine\\mq201_mq201delphineintrori_000410d4_1.fuz"},
            {"femaleuniqueelenwen", "skyrim.esm\\femaleuniqueelenwen\\mq302__000d94ba_1.fuz"},
            {"femaleuniqueghost", "skyrim.esm\\femaleuniqueghost\\t02_t02rukimocksplayertopi_00038281_1.fuz"},
            {"femaleuniquekarliah", "skyrim.esm\\femaleuniquekarliah\\tgdialogue_tgdialoguekarli_000d61dd_2.fuz"},
            {"femaleuniquemaven", "skyrim.esm\\femaleuniquemaven\\ms03_ms03mavenoffertopic_0001da36_1.fuz"},
            {"femaleuniquemephala", "skyrim.esm\\femaleuniquemephala\\da08__0010ebc3_1.fuz"},
            {"femaleuniquemeridia", "skyrim.esm\\femaleuniquemeridia\\da09_da09meridiadawnbreake_0004e4ca_1.fuz"},
            {"femaleuniquemirabelleervine", "skyrim.esm\\femaleuniquemirabelleervine\\mg06_mg06stage10mirabellee_0002757b_3.fuz"},
            {"femaleuniquenamira", "skyrim.esm\\femaleuniquenamira\\da11_da11voiceofnamiratopi_00089425_1.fuz"},
            {"femaleuniquenightmother", "skyrim.esm\\femaleuniquenightmother\\dbrecurring__000a030d_1.fuz"},
            {"femaleuniquenocturnal", "skyrim.esm\\femaleuniquenocturnal\\tg09__0001a2c8_5.fuz"},
            {"femaleuniquevaermina", "skyrim.esm\\femaleuniquevaermina\\da16miniscenes__000e155d_1.fuz"},
            {"femaleuniquevex", "skyrim.esm\\femaleuniquevex\\tgdialoguehqscene09__0003daab_1.fuz"},
            {"femaleyoungeager", "skyrim.esm\\femaleyoungeager\\db03__000c027a_3.fuz"},
            {"maleargonian", "skyrim.esm\\maleargonian\\dbeviction_dbnazireviction_0006f9a3_1.fuz"},
            {"malebandit", "skyrim.esm\\malebandit\\dunbrokenoarqst__0001d253_1.fuz"},
            {"malebrute", "skyrim.esm\\malebrute\\db07_db07playerphrasetopic_00028e27_2.fuz"},
            {"malechild", "skyrim.esm\\malechild\\dialogueri_dialogueriftenh_0008bbce_1.fuz"},
            {"malecommander", "skyrim.esm\\malecommander\\cw_cwcampaignfieldcomissio_000221e8_1.fuz"},
            {"malecommoner", "skyrim.esm\\malecommoner\\ms03_ms03louismotivationto_00074a4c_1.fuz"},
            {"malecommoneraccented", "skyrim.esm\\malecommoneraccented\\tgtq04_tgtq04torstenintrob_0007d672_3.fuz"},
            {"malecondescending", "skyrim.esm\\malecondescending\\dialogueda_dialoguedawnsta_00090e03_1.fuz"},
            {"malecoward", "skyrim.esm\\malecoward\\da15_da15queststart_0002bd02_2.fuz"},
            {"maledarkelf", "skyrim.esm\\maledarkelf\\dialoguefo_hirelingidles_000e1692_1.fuz"},
            {"maledrunk", "skyrim.esm\\maledrunk\\mq201party__000c0829_1.fuz"},
            {"maleelfhaughty", "dawnguard.esm\\maleelfhaughty\\dlc1rv08_dlc1rv08start3_0000ce01_2.fuz"},
            {"maleeventoned", "skyrim.esm\\maleeventoned\\dbrecurrin_dbrecurringcont_00087b80_1.fuz"},
            {"maleeventonedaccented", "skyrim.esm\\maleeventonedaccented\\db05_db05goodtimebranchtop_000a7041_1.fuz"},
            {"maleforsworn", "skyrim.esm\\maleforsworn\\duneldergl_duneldergleamt0_0001fb79_1.fuz"},
            {"maleguard", "skyrim.esm\\maleguard\\dialogueguardsgeneral__000dd08b_1.fuz"},
            {"malekhajiit", "skyrim.esm\\malekhajiit\\wemaiqthel_wemaiqtheliarhe_000b9df0_1.fuz"},
            {"malenord", "skyrim.esm\\malenord\\darkbrothe_nightgatehadrin_000da678_3.fuz"},
            {"malenordcommander", "skyrim.esm\\malenordcommander\\dialogueguardsgeneral__000dd0f5_1.fuz"},
            {"maleoldgrumpy", "skyrim.esm\\maleoldgrumpy\\darkbrotherhood__0006a3dc_1.fuz"},
            {"maleoldkindly", "skyrim.esm\\maleoldkindly\\dialoguema__000253b1_1.fuz"},
            {"maleorc", "dawnguard.esm\\maleorc\\dlc1vqfvbo_dlc1vqfvbooksbr_0001a3df_2.fuz"},
            {"maleslycynical", "skyrim.esm\\maleslycynical\\ms05_ms05viarmotask2_000534e7_2.fuz"},
            {"malesoldier", "skyrim.esm\\malesoldier\\db01misc_db01miscguardgree_000556f5_1.fuz"},
            {"maleuniqueamaundmotierre","skyrim.esm\\maleuniqueamaundmotierre\\db04_amaundcontractinfobra_0003bce8_1.fuz"},
            {"maleuniqueancano", "skyrim.esm\\maleuniqueancano\\mg03_mg03ancanoforcegreetr_00107ab4_1.fuz"},
            {"maleuniquearngeir", "skyrim.esm\\maleuniquearngeir\\mq00_mqarngeirdragonbornto_00030317_6.fuz"},
            {"maleuniqueaventusaretino", "skyrim.esm\\maleuniqueaventusaretino\\db01_db01aventusgrelodalre_00051400_4.fuz"},
            {"maleuniquebrynjolf", "skyrim.esm\\maleuniquebrynjolf\\tg01_tg01brynjolfduringque_000206a8_1.fuz"},
            {"maleuniquecicero", "skyrim.esm\\maleuniquecicero\\darkbrothe_dbcicerostatewa_0009be4a_1.fuz"},
            {"maleuniqueclavicusvile", "skyrim.esm\\maleuniqueclavicusvile\\da03_da03vilegreet1c_000bd765_1.fuz"},
            {"maleuniquedbblackdoor", "skyrim.esm\\maleuniquedbblackdoor\\darkbrothe_dbblackdoordawn_00019512_1.fuz"},
            {"maleuniquedbguardian", "skyrim.esm\\maleuniquedbguardian\\darkbrotherhood__00074748_1.fuz"},
           // {"maleuniquedbspectrallachance","skyrim.esm\\maleuniquedbspectrallachance\\darkbrotherhood__000e0c89_1.fuz"},
            {"maleuniquedelvinmallory", "skyrim.esm\\maleuniquedelvinmallory\\darkbrothe_dbsancmalloryre_00098b68_1.fuz"},
            //{"maleuniqueemperor", "skyrim.esm\\maleuniqueemperor\\db11_db11emperorplayerresp_0004fd52_3.fuz"},
            {"maleuniqueesbern", "skyrim.esm\\maleuniqueesbern\\mq00__000b0bc0_1.fuz"},
            {"maleuniquegallus", "skyrim.esm\\maleuniquegallus\\tg09_tg09galluspilgrimspat_0001a29e_2.fuz"},
            {"maleuniquegalmar", "skyrim.esm\\maleuniquegalmar\\cw02b__000a2028_1.fuz"},
            {"maleuniqueghost", "skyrim.esm\\maleuniqueghost\\dunvalthum_dunvalthumeqsth_0009a7d0_2.fuz"},
            {"maleuniqueghostsvaknir", "skyrim.esm\\maleuniqueghostsvaknir\\ms05_dunde__000e3653_1.fuz"},
            {"maleuniquehadvar", "skyrim.esm\\maleuniquehadvar\\cwsharedin_cwsharedinfosta_000e724d_1.fuz"},
            //{"maleuniquehermaeusmora","dragonborn.esm\\maleuniquehermaeusmora\\dlc2bookdungeoncontroller__00032164_1.fuz"},
            {"maleuniquehircine", "skyrim.esm\\maleuniquehircine\\da05_da05questingbeastimon_00015c64_1.fuz"},
            {"maleuniquekodlakwhitemane", "skyrim.esm\\maleuniquekodlakwhitemane\\c06_c06kodlakseeyalaterbra_000582d7_4.fuz"},
            {"maleuniquemalacath", "skyrim.esm\\maleuniquemalacath\\da06__000223d5_1.fuz"},
            {"maleuniquemehrunesdagon", "skyrim.esm\\maleuniquemehrunesdagon\\da07_da07dagonvoicebrancht_00097ef1_2.fuz"},
            {"maleuniquemercerfrey", "skyrim.esm\\maleuniquemercerfrey\\tg05_tg05mercerkarliahpurs_000b8387_2.fuz"},
            {"maleuniquemgaugur", "skyrim.esm\\maleuniquemgaugur\\mg04_mg04stage40augurthalm_000240b0_3.fuz"},
            {"maleuniquemolagbal", "skyrim.esm\\maleuniquemolagbal\\da10_da10molagbalstatuetop_000ddcee_2.fuz"},
            {"maleuniquenazir", "skyrim.esm\\maleuniquenazir\\db11_db11nazirbeginplayerr_000769d3_4.fuz"},
            {"maleuniqueperyite", "skyrim.esm\\maleuniqueperyite\\da13_da13peryitetoplevelto_000a8057_1.fuz"},
            {"maleuniqueseptimus", "skyrim.esm\\maleuniqueseptimus\\da04_da04septimuscubewhatt_000e4a36_3.fuz"},
            //{"maleuniquesheogorath", "skyrim.esm\\maleuniquesheogorath\\da15_da15sheomeet1a_0002bcfc_6.fuz"},
            {"maleuniquetullius", "skyrim.esm\\maleuniquetullius\\cw_cwsharedinfo_000e6c7e_1.fuz"},
            {"maleuniqueulfric", "skyrim.esm\\maleuniqueulfric\\cw_cwsharedinfo_000ce099_4.fuz"},
            {"malewarlock", "skyrim.esm\\malewarlock\\dunhobsfallqst__00082fb8_1.fuz"},
            {"maleyoungeager", "skyrim.esm\\maleyoungeager\\wejs02_wejs02gourmet_000b5d3e_2.fuz"},
            {"specialfemaleuniquegormlaith", "skyrim.esm\\specialfemaleuniquegormlaith\\mq304__00096ff8_1.fuz"},
            {"specialmaleuniquefelldir", "skyrim.esm\\specialmaleuniquefelldir\\mq206__000cd9eb_2.fuz"},
            {"specialmaleuniquehakon", "skyrim.esm\\specialmaleuniquehakon\\mq305_mq305heroblockingtop_000af66c_1.fuz"},
            {"specialmaleuniquetsun", "skyrim.esm\\specialmaleuniquetsun\\mq304_mq304tsunintroa2_0004fa47_1.fuz"},
            {"femaleneivavoice", "Neiva_Deep_Water_Follower_1_Form.esp\\FemaleNeivaVoice\\NeivaDeepW_NeivaDeepWaterF_00000A6E_1.wav"},

            {"femalecommoner", "AIAgent.esp\\femalecommoner\\femalecommoner.wav"},
            {"maleuniquedbspectrallachance", "AIAgent.esp\\maleuniquedbspectrallachance\\maleuniquedbspectrallachance.wav"},
            {"maleuniqueemperor", "AIAgent.esp\\maleuniqueemperor\\maleuniqueemperor.wav"},
            {"maleuniquehermaeusmora", "AIAgent.esp\\maleuniquehermaeusmora\\maleuniquehermaeusmora.wav"},
            {"maleuniquesheogorath", "AIAgent.esp\\maleuniquesheogorath\\maleuniquesheogorath.wav"}


        };

        if (!actor) return "";

        int maxSpeechLength = 0;
        std::string audioData;

        auto tesnpc = actor->GetActorBase();
        
        if (tesnpc) {
            auto voice = tesnpc->voiceType;
            if (voice) {
                std::string currentName = tesnpc->GetFullName();
                std::string voicename(tesnpc->voiceType->GetFormEditorID());
                std::transform(voicename.begin(), voicename.end(), voicename.begin(),
                               [](unsigned char c) { return std::tolower(c); });

                if (voicename == "crcowvoice") 
                    voicename = "maledrunk" ;
                else if (voicename == "crchickenvoice") 
                    voicename = "malecoward";
                else if (voicename == "crhorsevoice") 
                    voicename = "malecommoner";
                else if (voicename == "crgoatvoice") 
                    voicename = "maleslycynical";
                else if (voicename == "crdogvoice") 
                    voicename = "maleyoungeager";
                else if (voicename == "crdogdeathhound") 
                    voicename = "malewarlock";
                else if (voicename == "crmudcrabvoice")
                    voicename = "maleoldkindly";
                else if (voicename == "crdoghusky")
                    voicename = "maleyoungeager";
                
                if (currentName == "Arvak" || currentName == "arvak") 
                    voicename = "malecommoneraccented";
                
                

                if (skyrimVoicesBanned.find(voicename) != skyrimVoicesBanned.end()) {
                    ;
                    //logger:info("[AIFF] XTTS Voice generation disabled due to VA rights, will use custom samples");
                }

                logger::info("[VOICE] Voice name for: {} -> {} ", tesnpc->GetFullName(),voicename);
                
                auto hardcodedIt = skyrimVoices.find(voicename);
                if (hardcodedIt != skyrimVoices.end()) {
                    std::string filename = hardcodedIt->second;
                    
                    if (!filename.empty()) {
                        filename[0] = std::toupper(filename[0]);  // Convert the first character to lowercase
                    }
                    audioData.assign("Sound\\Voice\\" + filename);
                    logger::info("[VOICE] Skyrim hardcoded file found for {}: {} ", voicename, audioData);
                    return audioData;
                } else {
                    // Check CSV voice files after hardcoded list fails
                    std::string csvVoiceFile = FindVoiceInCSV(voicename);
                    if (!csvVoiceFile.empty()) {
                        audioData.assign("Sound\\Voice\\" + csvVoiceFile);
                        logger::info("[VOICE] CSV voice file found for {}: {} ", voicename, audioData);
                        return audioData;
                    }
                } 
            } else {
                // We could check actor name, and if actor name it's in a list.
                logger::info("Actor has no voice type: {}", tesnpc->GetFullName());
                std::string currentName = tesnpc->GetFullName();
                std::string forcedVoiceType;
                if (currentName == "Horse" || currentName == "horse") {
                    forcedVoiceType = "malecommoner";
                } else if (currentName == "shadowmere" || currentName == "Shadowmere") {
                    forcedVoiceType = "malecommander";
                } else if (currentName == "Frost" || currentName == "frost") {
                    forcedVoiceType = "malecondescending";
                } else if (currentName == "Arvak" || currentName == "arvak") {
                    forcedVoiceType = "malecommoneraccented";
                } else if (currentName == "chicken" || currentName == "Chicken") {
                    forcedVoiceType = "malecoward";
                } else if (currentName == "cow" || currentName == "Cow") {
                    forcedVoiceType = "maledrunk";
                } else if (currentName == "dog" || currentName == "Dog") {
                    forcedVoiceType = "maleyoungeager";
                } else if (currentName == "stray dog" || currentName == "Stray Dog" || currentName == "Stray dog") {
                    forcedVoiceType = "maleyoungeager";
                } else if (currentName == "death hound" || currentName == "Death Hound" || currentName == "Death hound") {
                    forcedVoiceType = "malewarlock";
                } else if (currentName == "husky" || currentName == "Husky") {
                    forcedVoiceType = "maleyoungeager";
                } else if (currentName == "goat" || currentName == "Goat") {
                    forcedVoiceType = "maleslycynical";
                } else if (currentName == "mudcrab" || currentName == "Mudcrab") {
                    forcedVoiceType = "maleoldkindly";
                } else if (currentName == "Vaca" || currentName == "vaca") {
                    forcedVoiceType = "maledrunk";
                } else if (currentName == "Caballo" || currentName == "caballo") {
                    forcedVoiceType = "malecommoner";
                } else if (currentName == "Bear" || currentName == "bear") {
                    forcedVoiceType = "maleorc";
                } else if (currentName == "Cave Bear" || currentName == "cave bear") {
                    forcedVoiceType = "maleorc";
                } else if (currentName == "Snow Bear" || currentName == "snow bear") {
                    forcedVoiceType = "maleorc";
                } else if (currentName == "Horker" || currentName == "horker") {
                    forcedVoiceType = "maleoldgrumpy";
                } else if (currentName == "Wolf" || currentName == "wolf") {
                    forcedVoiceType = "maleeventonedaccented";
                } else if (currentName == "Ice Wolf" || currentName == "ice wolf") {
                    forcedVoiceType = "maleeventonedaccented";
                } else if (currentName == "Pit Wolf" || currentName == "pit wolf") {
                    forcedVoiceType = "maleeventonedaccented";
                } else if (currentName == "Sabre Cat" || currentName == "sabre cat") {
                    forcedVoiceType = "malecommander";
                } else if (currentName == "Snow Sabre Cat" || currentName == "snow sabre cat") {
                    forcedVoiceType = "malecommander";
                } else if (currentName == "Vale Sabre Cat" || currentName == "vale sabre cat") {
                    forcedVoiceType = "malecommander";
                } else if (currentName == "Mammoth" || currentName == "mammoth") {
                    forcedVoiceType = "maleoldkindly";
                } else if (currentName == "Skeever" || currentName == "skeever") {
                    forcedVoiceType = "malecoward";
                } else if (currentName == "Venomfang Skeever" || currentName == "venomfang skeever") {
                    forcedVoiceType = "malecoward";
                } else if (currentName == "Slaughterfish" || currentName == "slaughterfish") {
                    forcedVoiceType = "maledarkelf";
                } else if (currentName == "Elk" || currentName == "elk") {
                    forcedVoiceType = "maleelfhaughty";
                } else if (currentName == "Deer" || currentName == "deer") {
                    forcedVoiceType = "malecondescending";
                } else if (currentName == "Vale Deer" || currentName == "vale deer") {
                    forcedVoiceType = "malecondescending";
                } else if (currentName == "Fox" || currentName == "fox") {
                    forcedVoiceType = "malekhajiit";
                } else if (currentName == "Snow Fox" || currentName == "snow fox") {
                    forcedVoiceType = "malekhajiit";
                } else if (currentName == "Rabbit" || currentName == "rabbit") {
                    forcedVoiceType = "malebandit";
                }
                

                if (forcedVoiceType != "") {
                    logger::info("[VOICE] Using forced voicetype {} ", forcedVoiceType);
                    if (skyrimVoices.find(forcedVoiceType) != skyrimVoices.end()) {
                        std::string filename = skyrimVoices[forcedVoiceType];

                        if (!filename.empty()) {
                            filename[0] = std::toupper(filename[0]);  // Convert the first character to lowercase
                        }
                        audioData.assign("Sound\\Voice\\" + filename);
                        logger::info("[VOICE] Skyrim hardcoded file found for forced voice {}: {} ", forcedVoiceType, audioData);
                        return audioData;
                    } else {
                        // Check CSV voice files for forced voice type
                        std::string csvVoiceFile = FindVoiceInCSV(forcedVoiceType);
                        if (!csvVoiceFile.empty()) {
                            audioData.assign("Sound\\Voice\\" + csvVoiceFile);
                            logger::info("[VOICE] CSV voice file found for forced voice {}: {} ", forcedVoiceType, audioData);
                            return audioData;
                        }
                    }
                }


            }
        }

        // Fallback to audio files buffer
        for (auto it = audioFilesBuffer.begin(); it != audioFilesBuffer.end(); ++it) {
            if (it->actor) {
                if (it->actor != actor) {
                    continue;
                }

                if (it->speechLengt > maxSpeechLength) {
                    maxSpeechLength = it->speechLengt;
                    audioData = it->data;
                }
            }
        }
        
        if (!audioData.empty()) {
            logger::info("[VOICE] Audio buffer voice selected: {} (length: {})", audioData, maxSpeechLength);
        } else {
            logger::warn("[VOICE] No voice file found for actor: {}", tesnpc ? tesnpc->GetFullName() : "Unknown Actor");
        }

        return audioData;
    }
    

    static void clear() {
        audioFilesBuffer.clear();
    
    }

};


std::string GetCombatStateString(const RE::TESCombatEvent* event);

// CSV Import Data Detection Functions
void DetectAndUploadImportDataFiles();
std::vector<std::string> FindImportDataFiles(const std::string& directoryPath);
std::string ParseImportDataCSV(const std::string& filePath);
bool IsValidImportType(const std::string& fileType);
std::vector<std::string> FindCustomActionImportFiles(const std::string& directoryPath);
void ProcessCustomActionImportFiles(const std::vector<std::string>& customActionFiles);

// Oghma Import Detection Functions  
std::vector<std::string> FindOghmaImportFiles(const std::string& directoryPath);
void ProcessOghmaImportFiles(const std::vector<std::string>& oghmaFiles);

// Dynamic Oghma Import Detection Functions
std::vector<std::string> FindDynamicOghmaImportFiles(const std::string& directoryPath);
void ProcessDynamicOghmaImportFiles(const std::vector<std::string>& dynamicOghmaFiles);

// Item Import Detection Functions
std::vector<std::string> FindItemImportFiles(const std::string& directoryPath);
void ProcessItemImportFiles(const std::vector<std::string>& itemFiles);

// Traditional Quest Import Detection Functions
std::vector<std::string> FindTraditionalQuestImportFiles(const std::string& directoryPath);
void ProcessTraditionalQuestImportFiles(const std::vector<std::string>& traditionalQuestFiles);

// Voice CSV Detection Functions
std::vector<std::string> FindVoiceCSVFiles(const std::string& directoryPath);
void LoadVoiceCSVData();
std::string FindVoiceInCSV(const std::string& voiceType);
std::unordered_map<std::string, std::string> ParseVoiceCSV(const std::string& filePath);
void ReloadVoiceCSVData();

// Low process actors;
std::vector<std::pair<std::string, RE::FormID>> GetLowProcessActorNamesFromRef(RE::Actor* target);

#endif // SPG_MISC_H

// VoiceRecordControl.h

#ifndef VOICE_RECORD_CONTROL_H
#define VOICE_RECORD_CONTROL_H


class VoiceRecordControl {

private:
    // Private constructor to prevent external instantiation
    VoiceRecordControl() : boolValue(false) {}

    // Private copy constructor and assignment operator to prevent cloning
    VoiceRecordControl(const VoiceRecordControl&) = delete;
    VoiceRecordControl& operator=(const VoiceRecordControl&) = delete;

    // Private data member to hold the boolean variable
    bool boolValue;

    // Private mutex for thread safety
    std::mutex mutex_;

public:
    // Public static method to access the singleton instance
    static VoiceRecordControl& getInstance() {
        static VoiceRecordControl instance;
        return instance;
    }

    // Getter method for the boolean variable
    bool getRecording()  {
        std::lock_guard<std::mutex> lock(mutex_);
        return boolValue;
    }

    // Setter method for the boolean variable
    void setRecording(bool value) {
        std::lock_guard<std::mutex> lock(mutex_);
        boolValue = value;
    }
};

#endif  // VOICE_RECORD_CONTROL_H
