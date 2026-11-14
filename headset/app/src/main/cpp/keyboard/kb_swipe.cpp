#include "kb_swipe.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "kb_layout.h"
#include "platform/assets.h"

namespace rv::kb {

namespace {

constexpr int N = 32;  // points each path is resampled to
const std::map<std::string, std::string> kLigatures = {{"œ", "oe"}, {"æ", "ae"}, {"ß", "ss"}, {"ĳ", "ij"}};

float Dist(const Pt& a, const Pt& b) { return std::hypot(a.x - b.x, a.y - b.y); }

float PathLength(const std::vector<Pt>& p) {
    float s = 0;
    for (size_t i = 1; i < p.size(); i++) s += Dist(p[i - 1], p[i]);
    return s;
}

std::vector<Pt> Resample(const std::vector<Pt>& pts, int n = N) {
    if (pts.size() == 1) return std::vector<Pt>(n, pts[0]);
    std::vector<float> d = {0};
    for (size_t i = 1; i < pts.size(); i++) d.push_back(d.back() + Dist(pts[i - 1], pts[i]));
    const float total = d.back();
    if (total <= 1e-6f) return std::vector<Pt>(n, pts[0]);
    std::vector<Pt> out;
    size_t j = 0;
    for (int i = 0; i < n; i++) {
        const float t = total * i / (n - 1);
        while (j + 2 < d.size() && d[j + 1] < t) j++;
        float seg = d[j + 1] - d[j];
        if (seg == 0) seg = 1e-6f;
        const float a = (t - d[j]) / seg;
        out.push_back({pts[j].x + (pts[j + 1].x - pts[j].x) * a, pts[j].y + (pts[j + 1].y - pts[j].y) * a});
    }
    return out;
}

// Average distance between two paths after letting them stretch against each other (time warping).
float Dtw(const std::vector<Pt>& a, const std::vector<Pt>& b, int band = 5) {
    const int n = int(a.size());
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> prev(n + 1, inf), cur(n + 1);
    prev[0] = 0;
    for (int i = 1; i <= n; i++) {
        std::fill(cur.begin(), cur.end(), inf);
        for (int j = std::max(1, i - band); j <= std::min(n, i + band); j++) {
            const float c = Dist(a[i - 1], b[j - 1]);
            cur[j] = c + std::min({prev[j], prev[j - 1], cur[j - 1]});
        }
        std::swap(prev, cur);
    }
    return prev[n] / n;
}

// How closely the swipe passes over each letter of a word, in order: (average, worst) distance.
std::pair<float, float> Passes(const std::vector<Pt>& path, const std::vector<Pt>& letters) {
    const size_t n = path.size();
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> prev, cur(n);
    std::vector<std::pair<float, float>> best(n), nb(n);
    std::vector<bool> has(n);
    for (size_t li = 0; li < letters.size(); li++) {
        std::vector<float> d(n);
        for (size_t j = 0; j < n; j++) d[j] = Dist(path[j], letters[li]);
        if (li == 0) {
            cur = d;
            for (size_t j = 0; j < n; j++) best[j] = {d[j], d[j]};
            std::fill(has.begin(), has.end(), true);
        } else {
            std::fill(cur.begin(), cur.end(), inf);
            std::vector<bool> nhas(n, false);
            float run = inf;
            std::pair<float, float> runB{};
            bool runHas = false;
            for (size_t j = 0; j < n; j++) {
                if (prev[j] < run) {
                    run = prev[j];
                    runB = best[j];
                    runHas = has[j];
                }
                if (runHas) {
                    cur[j] = run + d[j];
                    nb[j] = {cur[j], std::max(runB.second, d[j])};
                    nhas[j] = true;
                }
            }
            best = nb;
            has = nhas;
        }
        prev = cur;
    }
    size_t j = 0;
    for (size_t i = 1; i < n; i++)
        if (prev[i] < prev[j]) j = i;
    return {prev[j] / letters.size(), best[j].second};
}

}  // namespace

bool Decoder::Available(const std::string& lang2) {
    auto it = m_available.find(lang2);
    if (it != m_available.end()) return it->second;
    std::vector<uint8_t> probe;
    const bool ok = !lang2.empty() && m_assets.Read(("keyboard/words/" + lang2 + ".txt").c_str(), probe);
    if (ok) {
        std::vector<std::string> words;
        size_t start = 0;
        const std::string text(probe.begin(), probe.end());
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string w = text.substr(start, end - start);
            while (!w.empty() && (w.back() == '\r' || w.back() == ' ')) w.pop_back();
            if (!w.empty()) words.push_back(w);
            start = end + 1;
        }
        m_lists[lang2] = std::move(words);
    }
    m_available[lang2] = ok;
    return ok;
}

const std::vector<std::string>& Decoder::Words(const std::string& lang2) {
    static const std::vector<std::string> none;
    if (!Available(lang2)) return none;
    return m_lists[lang2];
}

Decoder::Index& Decoder::IndexFor(const std::string& lang2, const std::map<std::string, Pt>& keys) {
    std::string sig = lang2;
    for (const auto& [k, p] : keys) sig += "|" + k + ":" + std::to_string(int(p.x)) + "," + std::to_string(int(p.y));
    if (sig == m_indexSig) return m_index;
    m_index.clear();
    m_indexSig = sig;
    const Data& data = Data::Get();
    const auto& words = Words(lang2);
    for (int rank = 0; rank < int(words.size()); rank++) {
        std::vector<std::string> seq;
        bool ok = true;
        for (const std::string& c : Chars(words[rank])) {
            std::string k = keys.count(c) ? c : data.Base(c);
            std::vector<std::string> parts;
            if (keys.count(k)) {
                parts = {k};
            } else if (auto lig = kLigatures.find(c); lig != kLigatures.end()) {
                bool all = true;
                for (char x : lig->second) all = all && keys.count(std::string(1, x));
                if (all)
                    for (char x : lig->second) parts.push_back(std::string(1, x));
            }
            if (parts.empty()) {
                ok = false;
                break;
            }
            for (const std::string& p : parts)
                if (seq.empty() || seq.back() != p) seq.push_back(p);
        }
        if (!ok || seq.size() < 2) continue;
        Entry e;
        e.rank = rank;
        e.word = words[rank];
        e.seq = seq;
        m_index[{seq.front(), seq.back()}].push_back(std::move(e));
    }
    return m_index;
}

void Decoder::Warm(const std::string& lang2, const std::map<std::string, Pt>& keys) {
    std::lock_guard<std::mutex> l(m_lock);
    IndexFor(lang2, keys);
}

std::vector<std::string> Decoder::Decode(const std::string& lang2, const std::vector<Pt>& path,
                                         const std::map<std::string, Pt>& keys, float keyw, size_t n) {
    if (path.size() < 2 || keys.empty()) return {};
    const float wFreq = 0.03f, wLen = 0.08f, wEnds = 0.12f, wPass = 0.3f, wWorst = 0.3f, passMax = 1.6f;
    Index& idx = IndexFor(lang2, keys);
    const Pt s = path.front(), e = path.back();
    auto near = [&](const Pt& p, float r) {
        std::vector<std::string> out;
        for (const auto& [k, c] : keys)
            if (Dist(c, p) <= r * keyw) out.push_back(k);
        return out;
    };
    const auto starts = near(s, 1.25f), ends = near(e, 1.25f);
    const std::vector<Pt> user = Resample(path);
    const float ulen = PathLength(path) / keyw;
    struct Scored {
        float score;
        int rank;
        Entry* entry;
    };
    std::vector<Scored> scored;
    for (const std::string& a : starts)
        for (const std::string& b : ends) {
            auto it = idx.find({a, b});
            if (it == idx.end()) continue;
            for (Entry& en : it->second) {
                if (!en.shaped) {  // a word's ideal path never changes for this keyboard: worked out once
                    for (const std::string& k : en.seq) en.ideal.push_back(keys.at(k));
                    en.length = PathLength(en.ideal);
                    en.shaped = true;
                }
                const float ilen = en.length / keyw;
                if (ilen > ulen * 2.2f + 1.5f || ulen > ilen * 2.2f + 1.5f) continue;
                if (!en.resampled) {
                    en.resampledPts = Resample(en.ideal);
                    en.resampled = true;
                }
                float dist = 0;
                for (int i = 0; i < N; i++) dist += Dist(user[i], en.resampledPts[i]);
                dist /= N * keyw;
                const float endsOff = (Dist(s, en.ideal.front()) + Dist(e, en.ideal.back())) / keyw;
                const float score = dist + wFreq * std::log(float(en.rank) + 20) + wLen * std::fabs(ilen - ulen) / (ilen + 1) +
                                    wEnds * endsOff;
                scored.push_back({score, en.rank, &en});
            }
        }
    std::sort(scored.begin(), scored.end(), [](const Scored& x, const Scored& y) {
        return x.score != y.score ? x.score < y.score : x.rank < y.rank;
    });
    // second pass on the best few: time warping, and the swipe really went over every letter, in order
    std::vector<Scored> best, far;
    const std::vector<Pt> fine = Resample(path, 64);
    for (size_t i = 0; i < scored.size() && i < 60; i++) {
        Entry& en = *scored[i].entry;
        const float ilen = en.length / keyw;
        const float endsOff = (Dist(s, en.ideal.front()) + Dist(e, en.ideal.back())) / keyw;
        const float dtw = Dtw(user, en.resampledPts) / keyw;
        auto [avg, worst] = Passes(fine, en.ideal);
        avg /= keyw;
        worst /= keyw;
        const float sc = dtw + wPass * avg + wFreq * std::log(float(en.rank) + 20) + wLen * std::fabs(ilen - ulen) / (ilen + 1) +
                         wEnds * endsOff;
        (worst <= passMax ? best : far).push_back({sc + wWorst * std::max(0.0f, worst - 0.7f), en.rank, &en});
    }
    auto byScore = [](const Scored& x, const Scored& y) { return x.score != y.score ? x.score < y.score : x.rank < y.rank; };
    std::sort(best.begin(), best.end(), byScore);
    std::sort(far.begin(), far.end(), byScore);
    best.insert(best.end(), far.begin(), far.end());
    std::vector<std::string> out;
    for (const Scored& x : best) {
        if (std::find(out.begin(), out.end(), x.entry->word) == out.end()) out.push_back(x.entry->word);
        if (out.size() >= n) break;
    }
    return out;
}

std::vector<std::string> Decoder::Completions(const std::string& lang2, const std::string& p, size_t n) {
    if (m_compLang != lang2) {
        m_compIndex.clear();
        const auto& words = Words(lang2);
        const Data& data = Data::Get();
        for (int r = 0; r < int(words.size()); r++) m_compIndex.push_back({data.LowerWord(words[r]), r});
        std::sort(m_compIndex.begin(), m_compIndex.end());
        m_compLang = lang2;
    }
    auto lo = std::lower_bound(m_compIndex.begin(), m_compIndex.end(), std::make_pair(p, -1));
    auto hi = std::lower_bound(m_compIndex.begin(), m_compIndex.end(), std::make_pair(p + "\xF4\x8F\xBF\xBF", -1));
    std::vector<int> ranks;
    for (auto it = lo; it != hi; ++it)
        if (it->first.size() > p.size()) ranks.push_back(it->second);
    std::sort(ranks.begin(), ranks.end());
    const auto& words = Words(lang2);
    std::vector<std::string> out;
    for (size_t i = 0; i < ranks.size() && i < n + 1; i++) out.push_back(words[ranks[i]]);
    return out;
}

}  // namespace rv::kb
