// Swipe typing and word suggestions: BetterFrame Keyboard's swipe.py and its completions, ported.
// The swipe is compared with the path each word would trace over the keyboard as it is now, and
// more common words are preferred. Word lists: assets/keyboard/words/<language>.txt, most common first.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rv {
class Assets;
}

namespace rv::kb {

struct Pt {
    float x = 0, y = 0;
};

class Decoder {
   public:
    explicit Decoder(const Assets& assets) : m_assets(assets) {}
    bool Available(const std::string& lang2);
    const std::vector<std::string>& Words(const std::string& lang2);
    // keys: letter -> key centre (canvas px); keyw: key width. Up to n words, best first.
    std::vector<std::string> Decode(const std::string& lang2, const std::vector<Pt>& path,
                                    const std::map<std::string, Pt>& keys, float keyw, size_t n = 4);
    // The most common words that start with what's typed (as typed: lower case).
    std::vector<std::string> Completions(const std::string& lang2, const std::string& lowerTyped, size_t n);
    // Builds the index for this keyboard ahead of the first swipe.
    void Warm(const std::string& lang2, const std::map<std::string, Pt>& keys);
    std::mutex& Lock() { return m_lock; }

   private:
    struct Entry {
        int rank;
        std::string word;
        std::vector<std::string> seq;
        bool shaped = false, resampled = false;
        std::vector<Pt> ideal, resampledPts;
        float length = 0;
    };
    using Index = std::map<std::pair<std::string, std::string>, std::vector<Entry>>;
    Index& IndexFor(const std::string& lang2, const std::map<std::string, Pt>& keys);

    const Assets& m_assets;
    std::mutex m_lock;
    std::map<std::string, std::vector<std::string>> m_lists;
    std::map<std::string, bool> m_available;
    std::string m_indexSig;
    Index m_index;
    std::string m_compLang;
    std::vector<std::pair<std::string, int>> m_compIndex;  // (lower case, rank), sorted
};

}  // namespace rv::kb
