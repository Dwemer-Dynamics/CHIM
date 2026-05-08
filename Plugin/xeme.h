#define vocab_PHONEME_AA 1   // odd      AA D
#define vocab_PHONEME_AE 2   // at       AE T
#define vocab_PHONEME_AH 3   // hut      HH AH T
#define vocab_PHONEME_AO 4   // ought    AO T
#define vocab_PHONEME_AW 5   // cow      K AW
#define vocab_PHONEME_AY 6   // hide     HH AY D
#define vocab_PHONEME_B 7    // be       B IY
#define vocab_PHONEME_CH 8   // cheese   CH IY Z
#define vocab_PHONEME_D 9    // dee      D IY
#define vocab_PHONEME_DH 10  // thee     DH IY
#define vocab_PHONEME_EH 11  // Ed       EH D
#define vocab_PHONEME_ER 12  // hurt     HH ER T
#define vocab_PHONEME_EY 13  // ate      EY T
#define vocab_PHONEME_F 14   // fee      F IY
#define vocab_PHONEME_G 15   // green    G R IY N
#define vocab_PHONEME_HH 16  // he       HH IY
#define vocab_PHONEME_IH 17  // it       IH T
#define vocab_PHONEME_IY 18  // eat      IY T
#define vocab_PHONEME_JH 19  // gee      JH IY
#define vocab_PHONEME_K 20   // key      K IY
#define vocab_PHONEME_L 21   // lee      L IY
#define vocab_PHONEME_M 22   // me       M IY
#define vocab_PHONEME_N 23   // knee     N IY
#define vocab_PHONEME_NG 24  // ping     P IH NG
#define vocab_PHONEME_OW 25  // oat      OW T
#define vocab_PHONEME_OY 26  // toy      T OY
#define vocab_PHONEME_P 27   // pee      P IY
#define vocab_PHONEME_R 28   // read     R IY D
#define vocab_PHONEME_S 29   // sea      S IY
#define vocab_PHONEME_SH 30  // she      SH IY
#define vocab_PHONEME_T 31   // tea      T IY
#define vocab_PHONEME_TH 32  // theta    TH EY T AH
#define vocab_PHONEME_UH 33  // hood     HH UH D
#define vocab_PHONEME_UW 34  // two      T UW
#define vocab_PHONEME_V 35   // vee      V IY
#define vocab_PHONEME_W 36   // we       W IY
#define vocab_PHONEME_Y 37   // yield    Y IY L D
#define vocab_PHONEME_Z 38   // zee      Z IY
#define vocab_PHONEME_ZH 39  // seizure  S IY ZH ER
#define vocab_PHONEME_0 40  // seizure  S IY ZH ER

// Visemes used by FaceGen Modeller 3.1
#define vocab_VISEME_AAH 0      // aah
#define vocab_VISEME_BIG_AAH 1  // big aah
#define vocab_VISEME_B_M_P 2    // B,M,P
#define vocab_VISEME_CH_J_SH 3  // ch,J,sh
#define vocab_VISEME_D_S_T 4    // D,S,T
#define vocab_VISEME_EE 5       // ee
#define vocab_VISEME_EH 6       // eh
#define vocab_VISEME_F_V 7      // F,V
#define vocab_VISEME_I 8        // i
#define vocab_VISEME_K 9       // K
#define vocab_VISEME_N 10       // N
#define vocab_VISEME_OH 11      // oh
#define vocab_VISEME_OOH_Q 11   // ooh,Q
#define vocab_VISEME_R 13       // R
#define vocab_VISEME_TH 14      // th
#define vocab_VISEME_W 15       // W
#define vocab_VISEME_0 7       // reset



std::unordered_map<std::string, int> phonemeLabelToIdentifier = {
    {"AA", vocab_PHONEME_AA}, {"AE", vocab_PHONEME_AE}, {"AH", vocab_PHONEME_AH}, {"AO", vocab_PHONEME_AO},
    {"AW", vocab_PHONEME_AW}, {"AY", vocab_PHONEME_AY}, {"B", vocab_PHONEME_B},   {"CH", vocab_PHONEME_CH},
    {"D", vocab_PHONEME_D},   {"DH", vocab_PHONEME_DH}, {"EH", vocab_PHONEME_EH}, {"ER", vocab_PHONEME_ER},
    {"EY", vocab_PHONEME_EY}, {"F", vocab_PHONEME_F},   {"G", vocab_PHONEME_G},   {"HH", vocab_PHONEME_HH},
    {"IH", vocab_PHONEME_IH}, {"IY", vocab_PHONEME_IY}, {"JH", vocab_PHONEME_JH}, {"K", vocab_PHONEME_K},
    {"L", vocab_PHONEME_L},   {"M", vocab_PHONEME_M},   {"N", vocab_PHONEME_N},   {"NG", vocab_PHONEME_NG},
    {"OW", vocab_PHONEME_OW}, {"OY", vocab_PHONEME_OY}, {"P", vocab_PHONEME_P},   {"R", vocab_PHONEME_R},
    {"S", vocab_PHONEME_S},   {"SH", vocab_PHONEME_SH}, {"T", vocab_PHONEME_T},   {"TH", vocab_PHONEME_TH},
    {"UH", vocab_PHONEME_UH}, {"UW", vocab_PHONEME_UW}, {"V", vocab_PHONEME_V},   {"W", vocab_PHONEME_W},
    {"Y", vocab_PHONEME_Y},   {"Z", vocab_PHONEME_Z},   {"ZH", vocab_PHONEME_ZH}, {" ", vocab_PHONEME_0},
    {",", vocab_PHONEME_0},   {".", vocab_PHONEME_0},   {"U", vocab_PHONEME_UH},  {"A", vocab_PHONEME_AA}, 
    {"O", vocab_PHONEME_OY},  {"E", vocab_PHONEME_EY},  {"I", vocab_PHONEME_IY}};


std::unordered_map<int, int> phonemeToViseme = {
    {vocab_PHONEME_AA, vocab_VISEME_BIG_AAH}, {vocab_PHONEME_AE, vocab_VISEME_AAH},
    {vocab_PHONEME_AH, vocab_VISEME_AAH},     {vocab_PHONEME_AO, vocab_VISEME_BIG_AAH},
    {vocab_PHONEME_AW, vocab_VISEME_BIG_AAH}, {vocab_PHONEME_AY, vocab_VISEME_AAH},
    {vocab_PHONEME_B, vocab_VISEME_B_M_P},    {vocab_PHONEME_CH, vocab_VISEME_CH_J_SH},
    {vocab_PHONEME_D, vocab_VISEME_D_S_T},    {vocab_PHONEME_DH, vocab_VISEME_TH},
    {vocab_PHONEME_EH, vocab_VISEME_EH},      {vocab_PHONEME_ER, vocab_VISEME_R},
    {vocab_PHONEME_EY, vocab_VISEME_EH},      {vocab_PHONEME_F, vocab_VISEME_F_V},
    {vocab_PHONEME_G, vocab_VISEME_CH_J_SH},  {vocab_PHONEME_HH, vocab_VISEME_EH},
    {vocab_PHONEME_IH, vocab_VISEME_I},       {vocab_PHONEME_IY, vocab_VISEME_EE},
    {vocab_PHONEME_JH, vocab_VISEME_CH_J_SH}, {vocab_PHONEME_K, vocab_VISEME_CH_J_SH},
    {vocab_PHONEME_L, vocab_VISEME_TH},       {vocab_PHONEME_M, vocab_VISEME_B_M_P},
    {vocab_PHONEME_N, vocab_VISEME_N},        {vocab_PHONEME_NG, vocab_VISEME_D_S_T},
    {vocab_PHONEME_OW, vocab_VISEME_OH},      {vocab_PHONEME_OY, vocab_VISEME_OOH_Q},
    {vocab_PHONEME_P, vocab_VISEME_B_M_P},    {vocab_PHONEME_R, vocab_VISEME_R},
    {vocab_PHONEME_S, vocab_VISEME_D_S_T},    {vocab_PHONEME_SH, vocab_VISEME_CH_J_SH},
    {vocab_PHONEME_T, vocab_VISEME_D_S_T},    {vocab_PHONEME_TH, vocab_VISEME_TH},
    {vocab_PHONEME_UH, vocab_VISEME_OH},      {vocab_PHONEME_UW, vocab_VISEME_OOH_Q},
    {vocab_PHONEME_V, vocab_VISEME_F_V},      {vocab_PHONEME_W, vocab_VISEME_W},
    {vocab_PHONEME_Y, vocab_VISEME_EE},       {vocab_PHONEME_Z, vocab_VISEME_W},
    {vocab_PHONEME_ZH, vocab_VISEME_CH_J_SH}, {vocab_PHONEME_0, vocab_VISEME_0}
};

std::string getVISEMEName(int viseme) {
    switch (viseme) {
        case 0:
            return "aah";
        case 1:
            return "big aah";
        case 2:
            return "B,M,P";
        case 3:
            return "ch,J,sh";
        case 4:
            return "D,S,T";
        case 5:
            return "ee";
        case 6:
            return "eh";
        case 7:
            return "F,V";
        case 8:
            return "i";
        case 9:
            return "K";
        case 10:
            return "N";
        case 11:
            return "oh";
        case 12:
            return "ooh,Q";
        case 13:
            return "R";
        case 14:
            return "th";
        case 15:
            return "W";
        case -1:
            return "[close]";
        default:
            return "Unknown VISEME";
    }
}

