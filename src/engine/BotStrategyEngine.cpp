/*
 * Thin AC-style strategy engine for SkyFire playerbots.
 */

#include "BotStrategyEngine.h"

#include "SharedDefines.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace
{
    void SplitCsv(std::string const& text, std::vector<std::string>& out)
    {
        std::string cur;
        for (char ch : text)
        {
            if (ch == ',')
            {
                if (!cur.empty())
                    out.push_back(cur);
                cur.clear();
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(ch)))
            {
                if (!cur.empty() && cur.back() != ' ')
                    cur.push_back(' ');
                continue;
            }
            cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        while (!cur.empty() && cur.back() == ' ')
            cur.pop_back();
        if (!cur.empty())
            out.push_back(cur);
    }
}

void BotStrategyEngine::Clear()
{
    _combat.clear();
    _nonCombat.clear();
}

void BotStrategyEngine::ResetToRoleDefaults(bool isTank, bool isHealer, uint8 cls)
{
    Clear();
    _isTank = isTank;
    _isHealer = isHealer;
    if (cls)
        _cls = cls;

    // Non-combat defaults (AC: nc + follow + food/loot-ish)
    Add("food", BotState::NonCombat);
    Add("follow", BotState::NonCombat);
    Add("loot", BotState::NonCombat);

    // Shared combat utilities (thin AC aoe/boost/cc/avoid aoe).
    Add("boost", BotState::Combat);
    Add("avoid aoe", BotState::Combat);

    // Combat defaults by role (AC AiFactory-style)
    if (isTank)
    {
        Add("tank", BotState::Combat);
        Add("tank assist", BotState::Combat);
        Add("aoe", BotState::Combat);
    }
    else if (isHealer)
    {
        Add("heal", BotState::Combat);
        Add("heal", BotState::NonCombat); // OOC top-ups after pulls
        Add("save mana", BotState::Combat);
        // Healers: aoe off unless explicitly enabled.
    }
    else
    {
        Add("dps", BotState::Combat);
        Add("dps assist", BotState::Combat);
        Add("threat", BotState::Combat);
        Add("aoe", BotState::Combat);
        // AC-style: give the tank a few seconds before DPS open.
        Add("wait for attack", BotState::Combat);
        // Hybrid DPS emergency-heal below threshold (co +/-offheal).
        if (_cls == CLASS_PALADIN || _cls == CLASS_PRIEST || _cls == CLASS_SHAMAN
            || _cls == CLASS_DRUID || _cls == CLASS_MONK)
            Add("offheal", BotState::Combat);
    }

    // CC default ON for classes that rely on it (AC AiFactory).
    if (_cls == CLASS_MAGE || _cls == CLASS_HUNTER || _cls == CLASS_WARLOCK)
        Add("cc", BotState::Combat);
}

BotStrategyEngine::StrategySet& BotStrategyEngine::SetFor(BotState state)
{
    return state == BotState::Combat ? _combat : _nonCombat;
}

BotStrategyEngine::StrategySet const& BotStrategyEngine::SetFor(BotState state) const
{
    return state == BotState::Combat ? _combat : _nonCombat;
}

std::string BotStrategyEngine::NormalizeName(std::string name)
{
    // Collapse internal whitespace and known aliases.
    std::string out;
    bool space = false;
    for (char ch : name)
    {
        if (std::isspace(static_cast<unsigned char>(ch)))
        {
            space = true;
            continue;
        }
        if (space && !out.empty())
            out.push_back(' ');
        space = false;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    if (out == "tankassist") out = "tank assist";
    if (out == "autowalk" || out == "auto walk") out = "walk";
    if (out == "healdps" || out == "heal dps") out = "healer dps";
    if (out == "savemana") out = "save mana";
    if (out == "dpsassist") out = "dps assist";
    if (out == "waitforattack" || out == "wait attack") out = "wait for attack";
    if (out == "avoidaoe" || out == "avoid") out = "avoid aoe";
    if (out == "off heal" || out == "off-heal") out = "offheal";
    return out;
}

bool BotStrategyEngine::IsKnown(std::string const& name, BotState state) const
{
    if (name == "passive" || name == "grind" || name == "walk")
        return true;

    if (state == BotState::NonCombat)
        return name == "food" || name == "follow" || name == "stay" || name == "loot"
            || name == "quests" || name == "heal";

    // Combat names (role gate applied separately).
    return name == "tank" || name == "tank assist" || name == "dps" || name == "dps assist"
        || name == "heal" || name == "healer dps" || name == "save mana" || name == "threat"
        || name == "wait for attack" || name == "offheal"
        || name == "aoe" || name == "boost" || name == "cc" || name == "avoid aoe";
}

bool BotStrategyEngine::IsRoleAllowed(std::string const& name, BotState state, bool isTank, bool isHealer) const
{
    if (name == "passive" || name == "grind" || name == "walk")
        return true;
    if (state == BotState::NonCombat)
    {
        if (name == "heal")
            return isHealer; // dedicated healers only for nc +heal
        return IsKnown(name, state);
    }

    // Shared combat toggles — any role may enable/disable.
    if (name == "aoe" || name == "boost" || name == "cc" || name == "avoid aoe")
        return true;

    if (isTank)
        return name == "tank" || name == "tank assist" || name == "dps";
    if (isHealer)
        return name == "heal" || name == "healer dps" || name == "save mana" || name == "wait for attack";
    return name == "dps" || name == "dps assist" || name == "threat" || name == "wait for attack"
        || name == "offheal";
}

void BotStrategyEngine::ApplyMutualExclusions(std::string const& name, BotState state, bool enabled)
{
    if (!enabled)
        return;

    StrategySet& set = SetFor(state);

    if (name == "follow")
    {
        set.erase("stay");
        // Follow implies not passive grinding stay — mirror AC follow pack lightly.
    }
    else if (name == "stay")
    {
        set.erase("follow");
        set.erase("grind");
    }
    else if (name == "grind")
    {
        set.erase("passive");
        set.erase("stay");
        if (state == BotState::NonCombat)
            set.insert("follow");
    }
    else if (name == "passive")
    {
        set.erase("grind");
    }
    else if (name == "tank")
    {
        set.erase("dps");
    }
    else if (name == "dps" && _isTank)
    {
        set.erase("tank");
    }
    else if (name == "heal")
    {
        set.erase("healer dps");
    }
    else if (name == "healer dps")
    {
        set.erase("heal");
    }
}

void BotStrategyEngine::Add(std::string const& rawName, BotState state)
{
    std::string const name = NormalizeName(rawName);
    if (name.empty() || !IsKnown(name, state))
        return;
    if (!IsRoleAllowed(name, state, _isTank, _isHealer))
        return;
    SetFor(state).insert(name);
    ApplyMutualExclusions(name, state, true);
    // One switch: nc +walk and co +walk enable the same self-bot movement.
    if (name == "walk")
        SetFor(state == BotState::Combat ? BotState::NonCombat : BotState::Combat).insert(name);
}

void BotStrategyEngine::Remove(std::string const& rawName, BotState state)
{
    std::string const name = NormalizeName(rawName);
    if (name.empty())
        return;
    SetFor(state).erase(name);
    if (name == "walk")
        SetFor(state == BotState::Combat ? BotState::NonCombat : BotState::Combat).erase(name);
}

void BotStrategyEngine::Toggle(std::string const& rawName, BotState state)
{
    std::string const name = NormalizeName(rawName);
    if (name.empty() || !IsKnown(name, state))
        return;
    if (!IsRoleAllowed(name, state, _isTank, _isHealer))
        return;
    if (Has(name, state))
        Remove(name, state);
    else
        Add(name, state);
}

void BotStrategyEngine::Set(std::string const& name, BotState state, bool enabled)
{
    if (enabled)
        Add(name, state);
    else
        Remove(name, state);
}

bool BotStrategyEngine::Has(std::string const& rawName, BotState state) const
{
    return SetFor(state).count(NormalizeName(rawName)) > 0;
}

bool BotStrategyEngine::HasAny(BotState state) const
{
    return !SetFor(state).empty();
}

std::vector<std::string> BotStrategyEngine::List(BotState state) const
{
    return std::vector<std::string>(SetFor(state).begin(), SetFor(state).end());
}

std::string BotStrategyEngine::Format(BotState state) const
{
    // List known names for this state/role with +/- like AC "?".
    std::vector<std::string> names;
    if (state == BotState::NonCombat)
        names = { "food", "follow", "stay", "loot", "quests", "heal", "passive", "grind", "walk" };
    else if (_isTank)
        names = { "passive", "grind", "tank", "tank assist", "dps",
                  "aoe", "boost", "cc", "avoid aoe", "walk" };
    else if (_isHealer)
        names = { "passive", "grind", "heal", "healer dps", "save mana", "wait for attack",
                  "aoe", "boost", "cc", "avoid aoe", "walk" };
    else
        names = { "passive", "grind", "dps", "dps assist", "threat", "wait for attack", "offheal",
                  "aoe", "boost", "cc", "avoid aoe", "walk" };

    std::string out;
    for (std::string const& n : names)
    {
        if (!out.empty())
            out += ", ";
        out += Has(n, state) ? "+" : "-";
        out += n;
    }
    return out;
}

std::string BotStrategyEngine::ChangeStrategy(std::string const& text, BotState state)
{
    if (text.empty() || text == "?")
        return Format(state);

    std::vector<std::string> parts;
    SplitCsv(text, parts);

    std::string report;
    auto append = [&](std::string const& piece)
    {
        if (!report.empty())
            report += ", ";
        report += piece;
    };

    for (std::string part : parts)
    {
        while (!part.empty() && part.front() == ' ')
            part.erase(part.begin());
        while (!part.empty() && part.back() == ' ')
            part.pop_back();
        if (part.empty())
            continue;

        if (part == "?")
        {
            append(Format(state));
            continue;
        }

        char op = part[0];
        std::string name;
        if (op == '+' || op == '-' || op == '~')
            name = NormalizeName(part.substr(1));
        else
        {
            // Bare name => treat as +
            op = '+';
            name = NormalizeName(part);
        }

        if (name.empty())
            continue;

        if (!IsKnown(name, state))
        {
            append("!" + name + "(unknown)");
            continue;
        }
        if (!IsRoleAllowed(name, state, _isTank, _isHealer))
        {
            append("!" + name + "(wrong role)");
            continue;
        }

        if (op == '~')
            Toggle(name, state);
        else if (op == '-')
            Remove(name, state);
        else
            Add(name, state);

        append(std::string(Has(name, state) ? "+" : "-") + name);
    }

    if (report.empty())
        report = Format(state);
    return report;
}

void BotStrategyEngine::ApplyFollowPack()
{
    // AC FollowChatShortcutAction
    ChangeStrategy("+follow,-passive,-grind,-quests", BotState::NonCombat);
    ChangeStrategy("-stay,-passive,-grind", BotState::Combat);
    // Ensure follow is on NC even if stay was set.
    Add("follow", BotState::NonCombat);
    Remove("stay", BotState::NonCombat);
    // Healers should top up OOC after pulls (nc +heal).
    if (_isHealer)
        Add("heal", BotState::NonCombat);
}

void BotStrategyEngine::ApplyStayPack()
{
    // AC StayChatShortcutAction
    ChangeStrategy("+stay,-passive", BotState::NonCombat);
    ChangeStrategy("+stay,-follow,-passive", BotState::Combat);
    Add("stay", BotState::NonCombat);
    Remove("follow", BotState::NonCombat);
}

void BotStrategyEngine::ApplyFleePack()
{
    // AC FleeChatShortcutAction — follow + passive (don't fight)
    ChangeStrategy("+follow,-stay,+passive", BotState::NonCombat);
    ChangeStrategy("+follow,-stay,+passive", BotState::Combat);
    Add("follow", BotState::NonCombat);
    Remove("stay", BotState::NonCombat);
    Add("passive", BotState::NonCombat);
    Add("passive", BotState::Combat);
}

void BotStrategyEngine::ApplyGrindPack()
{
    // AC GrindChatShortcutAction (NC)
    ChangeStrategy("+grind,-passive,-stay", BotState::NonCombat);
    Add("grind", BotState::Combat);
    Remove("passive", BotState::Combat);
}

void BotStrategyEngine::ApplyPassivePack()
{
    Add("passive", BotState::NonCombat);
    Add("passive", BotState::Combat);
    Remove("grind", BotState::NonCombat);
    Remove("grind", BotState::Combat);
}

void BotStrategyEngine::ApplyAggressivePack()
{
    Remove("passive", BotState::NonCombat);
    Remove("passive", BotState::Combat);
}

void BotStrategyEngine::ApplyResetPack()
{
    ResetToRoleDefaults(_isTank, _isHealer, _cls);
}
