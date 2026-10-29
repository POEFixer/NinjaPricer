#pragma once
// RuneshapeLabel.h — the game's "Runeshape Combinations" panel in every game language.
//
// The panel's title is the game's ClientStrings text AreaRewardRunicInscriptions ("Runeshape
// Combinations", "Рунотворческие комбинации", ...), and each recipe row's label is
// RemnantRecipieAutoDescription filled in: "{0}x {1}" in English ("3x Orb of Augmentation"), but
// "{1} ({0})" in Russian, "{1} x{0} " in Spanish, "{0} {1}" in Portuguese. NinjaPricer asks the host
// for both texts in the current game language (ctx()->Lang.GameString, desktop 371+) and keeps the
// English ones as the last candidates, so an older host behaves as before.
//
// ParseByTemplate is the host's rule (POEFixer/core/runeshape/RuneshapePanelMatch.h, Task 6.3),
// copied because a plugin never includes host internals: keep the two in step. STL-only:
// tools/ninja_pricer_test runs it.
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace NinjaRuneshape {

inline constexpr std::string_view kTitleId = "AreaRewardRunicInscriptions";
inline constexpr std::string_view kLabelId = "RemnantRecipieAutoDescription";
inline constexpr std::string_view kEnglishTitle = "Runeshape Combinations";
inline constexpr std::string_view kEnglishTemplate = "{0}x {1}";
inline constexpr size_t kMaxNameBytes = 96;

inline std::string_view TrimAsciiSpace(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

// The element's text (UTF-8) is one of the title candidates, exactly after trimming ASCII spaces.
inline bool TitleMatches(std::string_view utf8, const std::vector<std::string>& titles) {
    const std::string_view text = TrimAsciiSpace(utf8);
    if (text.empty()) return false;
    for (const std::string& title : titles)
        if (!TrimAsciiSpace(title).empty() && TrimAsciiSpace(title) == text) return true;
    return false;
}

namespace Detail {
// 1-3 decimal digits (so at most 999), not 0.
inline bool ParseCount(std::string_view digits, int& count) {
    if (digits.empty() || digits.size() > 3) return false;
    int value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    if (value < 1) return false;
    count = value;
    return true;
}
}  // namespace Detail

// A recipe row's label (UTF-8) read through a label template: `{0}` is the count (1-999 in
// decimal), `{1}` the name (non-empty, at most 96 bytes), every other part of the template is
// matched exactly. The template holds {0} and {1} once each, in either order, and no other brace;
// the label and the template are trimmed of ASCII spaces at both ends first.
inline bool ParseByTemplate(std::string_view utf8Label, std::string_view tmpl, int& count, std::string& name) {
    const std::string_view label = TrimAsciiSpace(utf8Label);
    tmpl = TrimAsciiSpace(tmpl);
    constexpr std::string_view kCount = "{0}", kName = "{1}";
    const size_t p0 = tmpl.find(kCount), p1 = tmpl.find(kName);
    if (p0 == std::string_view::npos || p1 == std::string_view::npos) return false;
    const bool countFirst = p0 < p1;
    const size_t first = countFirst ? p0 : p1, second = countFirst ? p1 : p0;
    if (second < first + 3) return false;
    const std::string_view lit0 = tmpl.substr(0, first);
    const std::string_view lit1 = tmpl.substr(first + 3, second - first - 3);
    const std::string_view lit2 = tmpl.substr(second + 3);
    for (const std::string_view lit : { lit0, lit1, lit2 })
        if (lit.find_first_of("{}") != std::string_view::npos) return false;   // another placeholder

    if (label.size() < lit0.size() + lit1.size() + lit2.size() + 2) return false;
    if (label.substr(0, lit0.size()) != lit0 || label.substr(label.size() - lit2.size()) != lit2) return false;
    const std::string_view middle = label.substr(lit0.size(), label.size() - lit0.size() - lit2.size());
    // Every place the middle literal occurs splits the middle into the two fields; the first split
    // whose count parses and whose name fits wins (a name may itself hold the literal).
    for (size_t at = middle.find(lit1); at != std::string_view::npos; at = middle.find(lit1, at + 1)) {
        const std::string_view a = middle.substr(0, at);
        const std::string_view b = middle.substr(at + lit1.size());
        const std::string_view countText = countFirst ? a : b;
        const std::string_view nameText = countFirst ? b : a;
        int parsed = 0;
        if (!Detail::ParseCount(countText, parsed)) continue;
        if (nameText.empty() || nameText.size() > kMaxNameBytes) continue;
        count = parsed;
        name.assign(nameText);
        return true;
    }
    return false;
}

// The candidates a panel is searched and read with: `gameText` (the host's text in the game
// language, "" when unknown) first, then the English one, each once.
inline std::vector<std::string> Candidates(const std::string& gameText, std::string_view english) {
    std::vector<std::string> out;
    if (!TrimAsciiSpace(gameText).empty()) out.push_back(gameText);
    if (gameText != english) out.emplace_back(english);
    return out;
}

// A reward label -> {qty, name}. The English form "Nx ItemName" keeps its earlier reading (the name
// exactly as shown after "Nx ", which the price matcher's custom keys rely on); any other label is
// read through the templates (the game language's first); a label nothing reads is {1, label}.
// False for labels never priced: the panel title, the unused tab placeholders and skill rewards.
inline bool ParseReward(const std::string& label, const std::vector<std::string>& titles,
                        const std::vector<std::string>& templates, int& outQty, std::string& outName) {
    if (TrimAsciiSpace(label).empty()) return false;
    if (TitleMatches(label, titles)) return false;
    if (label.rfind("[dnt-", 0) == 0) return false;    // "[dnt-unused] tab one/two"
    if (label.rfind("Skill:", 0) == 0) return false;   // skill rewards: not tradeable

    int qty = 0;
    size_t i = 0;
    while (i < label.size() && label[i] >= '0' && label[i] <= '9' && qty < 100000) {
        qty = qty * 10 + (label[i] - '0');
        i++;
    }
    if (i > 0 && i + 1 < label.size() && label[i] == 'x' && label[i + 1] == ' ') {
        outQty = (qty < 1) ? 1 : qty;
        outName = label.substr(i + 2);   // strip the "Nx " quantity prefix
        return !outName.empty();
    }
    for (const std::string& tmpl : templates)
        if (ParseByTemplate(label, tmpl, outQty, outName)) return true;
    outQty = 1;
    outName = label;
    return true;
}

// One reward of a resolved Runeshape device (ctx()->Runeshape.Rewards): the host's English name,
// the quantity, the recipe size, the combination weight and the unit price the host found for
// that name.
struct RewardCandidate {
    std::string name;
    int count = 0;
    int recipeSize = 0;
    int weight = 0;
    bool priced = false;
    float unitChaos = 0.0f;
};

// What a panel row is known to be: its combination weight when one weight fits it, and the one
// English reward name it stands for ("" when not known).
struct RowMatch {
    bool hasWeight = false;
    int weight = 0;
    std::string english;
};

inline std::string LowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return out;
}

// The device rewards a panel row is matched against, indexed by their lower-cased English name.
class RewardIndex {
public:
    void Add(RewardCandidate candidate) {
        m_byName[LowerAscii(candidate.name)].push_back(m_all.size());
        m_all.push_back(std::move(candidate));
    }
    bool Empty() const { return m_all.empty(); }

    // A panel row: the name it shows (the game language's), its quantity, its rune-symbol count
    // (0 = unknown) and the unit price the host found for that name (the host prices a localized
    // base-type name through the same entry as its English name, Task 6.5). An English row is
    // matched by its name. A row in another game language shows a localized name the host's
    // English rewards never hold: it is matched by its quantity and recipe size, among the
    // rewards whose price is the row's own (unpriced ones: an unpriced row). Either way a weight
    // is given only when every matching reward has that one weight, and the English name only
    // when they all have that one name.
    RowMatch Match(std::string_view rowName, int qty, int symbols, bool rowPriced, float rowUnitChaos) const {
        std::vector<const RewardCandidate*> hits;
        const auto named = m_byName.find(LowerAscii(rowName));
        if (named != m_byName.end()) {
            for (const size_t i : named->second) {
                const RewardCandidate& c = m_all[i];
                if (c.count != qty) continue;
                if (symbols > 0 && c.recipeSize > 0 && c.recipeSize != symbols) continue;
                hits.push_back(&c);
            }
        } else if (symbols > 0) {
            for (const RewardCandidate& c : m_all) {
                if (c.count != qty || c.recipeSize != symbols) continue;
                if (c.priced != rowPriced) continue;
                if (rowPriced && !SamePrice(c.unitChaos, rowUnitChaos)) continue;
                hits.push_back(&c);
            }
        }
        RowMatch m;
        if (hits.empty()) {
            if (named != m_byName.end()) m.english = m_all[named->second.front()].name;
            return m;
        }
        m.hasWeight = true;
        m.weight = hits.front()->weight;
        m.english = hits.front()->name;
        for (const RewardCandidate* c : hits) {
            if (c->weight != m.weight) m.hasWeight = false;
            if (LowerAscii(c->name) != LowerAscii(m.english)) m.english.clear();
        }
        if (!m.hasWeight) m.weight = 0;
        return m;
    }

private:
    // One price entry gives both prices: equal up to float rounding.
    static bool SamePrice(float a, float b) {
        return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= 1e-4f * (std::max)(1.0f, std::fabs(b));
    }

    std::vector<RewardCandidate> m_all;
    std::unordered_map<std::string, std::vector<size_t>> m_byName;
};

}  // namespace NinjaRuneshape
