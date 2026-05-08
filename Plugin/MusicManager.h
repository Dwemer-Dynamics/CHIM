#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <map>
#include "json.hpp"
#include "Globals.h" // Neeeded for AIAgent definition

typedef unsigned long DWORD;

struct Alignment {
    double start_time;
    double end_time;
    double duration;
    std::string transcribed;
    std::string matched_lyrics;
    float relative_power;
    std::string sentiment;
    std::string animation1;
    std::string animation2;
};

struct Predominance {
    double start_time;
    double end_time;
    std::string predominant_stem;
    std::string secondary_stem;
    std::map<std::string, float> bpms;
    std::map<std::string, float> powers;
};

struct SongData {
    std::string audio_file;
    std::vector<char> audioBuffer;
    DWORD audioSize;
    double duration;
    std::vector<Alignment> alignments;
    std::vector<Predominance> predominance;
};

struct LyricInfo {
    std::string text;
    double start_time;
    double end_time;
    double duration;
    std::string animation1;
    std::string animation2;
};

class MusicManager {
private:
    static MusicManager* instance;
    SongData currentSong;
    std::thread playThread;
    bool isPlaying;
    bool stopped;
    bool paused;
    std::mutex mtx;
    MusicManager();
    ~MusicManager();

public:
    static MusicManager& getInstance();
    bool downloadSong(const std::string& songName);
    void playSong(AIAgent *singer);
    void stopSong();
    void pauseSong();
    void resumeSong();
    LyricInfo getCurrentLyrics(double time);
    std::string getPredominantStem(double time);
    std::string getSecondaryStem(double time);
    float getBPM(double time);
    std::map<std::string, float> getBPMs(double time);
    float getBPMsFor(double time, std::string stem);
    float getRelativePower(double time);
    std::map<std::string, float> getPowers(double time);
    std::string getSentiment(double time);
    std::string jusTrim(const std::string& input);
};